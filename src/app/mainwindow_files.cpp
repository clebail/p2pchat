// Images et fichiers joints : choisis, collés ou déposés sur la fenêtre (cf. attachment.h), puis ouverts ou
// enregistrés à la réception (cf. filestore.h)
#include "composer.h"
#include "filestore.h"
#include "groupnode.h"
#include "mainwindow.h"

#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QStandardPaths>
#include <QUrl>

// ---------------------------------------------------------------- Images

void MainWindow::chooseAttachment()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    const QString path = QFileDialog::getOpenFileName(this, tr("Joindre une image ou un fichier"), dir,
                                                      tr("Tous les fichiers (*)") + ";;" + Attachment::fileDialogFilter());
    if (!path.isEmpty())
        attachPath(path);
}

void MainWindow::attachPath(const QString &path)
{
    // Une image reste une image (réduite, affichée dans la bulle) ; tout le reste devient un fichier joint
    if (Attachment::isImageFile(path)) {
        const Attachment::Prepared prepared = Attachment::prepareFile(path);
        if (!prepared.data.isEmpty()) {
            attachPrepared(prepared);
            return;
        }
    }
    const QFileInfo info(path);
    QString problem;
    if (!info.isFile())
        problem = tr("fichier introuvable");
    else if (info.size() == 0)
        problem = tr("fichier vide");
    else if (info.size() > Files::MaxFileBytes)
        problem = tr("fichier trop volumineux (%1, %2 au plus)")
                      .arg(Files::formatSize(info.size()), Files::formatSize(Files::MaxFileBytes));
    if (!problem.isEmpty()) {
        QMessageBox::warning(this, tr("Joindre un fichier"), tr("Impossible de joindre ce fichier : %1.").arg(problem));
        return;
    }
    m_pendingImage.clear();
    m_pendingFile = path;
    m_imagePreview->setPixmap(QFileIconProvider().icon(info).pixmap(40, 40));
    m_imageInfo->setText(tr("<b>%1</b> · %2<br><span style='color:#5f6368'>%3</span>")
                             .arg(Files::sanitizeName(info.fileName()).toHtmlEscaped(), Files::formatSize(info.size()),
                                  info.size() > Files::AutoDownloadBytes
                                      ? tr("Les autres le téléchargeront s'ils le souhaitent")
                                      : tr("Ajoutez une légende si vous voulez, puis Entrée")));
    m_imageBar->setVisible(true);
    m_input->setFocus();
}

bool MainWindow::attachFromMime(const QMimeData *mime)
{
    if (!mime || !m_input->isEnabled())
        return false;
    // Fichier (gestionnaire de fichiers) : on relit le fichier pour garder la qualité, l'orientation et
    // l'animation d'un GIF
    for (const QUrl &url : mime->urls()) {
        if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile()) {
            attachPath(url.toLocalFile());
            return true;
        }
    }
    // Image copiée depuis un navigateur ou une capture d'écran (toujours fixe)
    if (mime->hasImage()) {
        attachPrepared(Attachment::prepareImage(qvariant_cast<QImage>(mime->imageData())));
        return true;
    }
    return false;
}

void MainWindow::attachPrepared(const Attachment::Prepared &prepared)
{
    if (prepared.data.isEmpty()) {
        QMessageBox::warning(this, tr("Joindre une image"),
                             tr("Impossible de joindre cette image%1.")
                                 .arg(prepared.error.isEmpty() ? QString() : " : " + prepared.error));
        return;
    }
    m_pendingImage = prepared.data;
    m_pendingFile.clear();
    m_imagePreview->setPixmap(QPixmap::fromImage(
        prepared.preview.scaled(m_imagePreview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    const QString kind = prepared.animated ? tr("GIF animé") : tr("Image");
    QString hint = tr("Ajoutez une légende si vous voulez, puis Entrée");
    if (prepared.frozen) // trop lourd pour partir tel quel
        hint = tr("<span style='color:#b06000'>GIF trop lourd (%1 Ko, %2 Ko au plus) : il sera envoyé figé</span>")
                   .arg((prepared.sourceBytes + 1023) / 1024)
                   .arg(Attachment::MaxBytes / 1024);
    else
        hint = QStringLiteral("<span style='color:#5f6368'>%1</span>").arg(hint);
    m_imageInfo->setText(tr("<b>%1</b> · %2 × %3 · %4 Ko<br>%5")
                             .arg(kind)
                             .arg(prepared.preview.width())
                             .arg(prepared.preview.height())
                             .arg((prepared.data.size() + 1023) / 1024)
                             .arg(hint));
    m_imageBar->setVisible(true);
    m_input->setFocus();
}

void MainWindow::cancelImage()
{
    m_pendingImage.clear();
    m_pendingFile.clear();
    m_imagePreview->clear();
    m_imageBar->setVisible(false);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    const QMimeData *mime = event->mimeData();
    bool usable = mime->hasImage();
    for (const QUrl &url : mime->urls())
        usable = usable || (url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile());
    if (usable && m_input->isEnabled())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    if (attachFromMime(event->mimeData()))
        event->acceptProposedAction();
}

// ---------------------------------------------------------------- Fichiers joints

void MainWindow::openFile(const QString &fileId)
{
    const auto info = m_node->files().info(fileId);
    const QString stored = m_node->files().path(fileId);
    if (!info || stored.isEmpty())
        return;
    if (Files::isRisky(info->name)
        && QMessageBox::warning(this, tr("Ouvrir le fichier"),
                                tr("<p><b>%1</b> est un programme ou un script.</p>"
                                   "<p>L'ouvrir peut exécuter du code sur cet ordinateur, avec vos droits. "
                                   "Ne le faites que si vous savez d'où il vient.</p>")
                                    .arg(info->name.toHtmlEscaped()),
                                QMessageBox::Open | QMessageBox::Cancel, QMessageBox::Cancel)
               != QMessageBox::Open)
        return;
    // Le magasin range les fichiers sous leur empreinte : on l'ouvre sous son vrai nom, depuis une copie dans
    // le dossier d'exécution de l'utilisateur (Linux : /run/user/<uid>, réservé et vidé à la déconnexion)
    QString base = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    const QString root = base + "/p2pchat-ouverts";
    const QString dir = root + "/" + fileId.left(16);
    QDir().mkpath(dir);
    constexpr auto OwnerOnly = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
    QFile::setPermissions(root, OwnerOnly);
    QFile::setPermissions(dir, OwnerOnly);
    const QString named = dir + "/" + info->name;
    if (QFileInfo(named).size() != info->size) {
        QFile::remove(named);
        if (!QFile::copy(stored, named)) {
            QMessageBox::warning(this, tr("Ouvrir le fichier"), tr("Impossible de préparer %1.").arg(info->name));
            return;
        }
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(named));
}

void MainWindow::saveFileAs(const QString &fileId)
{
    const auto info = m_node->files().info(fileId);
    const QString stored = m_node->files().path(fileId);
    if (!info || stored.isEmpty())
        return;
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    const QString path = QFileDialog::getSaveFileName(this, tr("Enregistrer le fichier"), dir + "/" + info->name);
    if (path.isEmpty())
        return;
    if (QFile::exists(path))
        QFile::remove(path); // le dialogue a déjà demandé confirmation
    if (!QFile::copy(stored, path))
        QMessageBox::warning(this, tr("Enregistrer le fichier"), tr("Impossible d'enregistrer %1.").arg(path));
}
