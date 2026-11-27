// Postes liés (cf. devicelink.h) : « Lier un autre poste… » sur le poste déjà utilisé, « Lier ce poste à mon
// compte… » sur le nouveau
#include "composer.h"
#include "conversation.h"
#include "devicelink.h"
#include "mainwindow.h"
#include "spellchecker.h"
#include "switchboard.h"

#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

void MainWindow::linkAnotherDevice()
{
    if (m_onion.isEmpty()) {
        QMessageBox::information(this, tr("Lier un autre poste"), tr("Le réseau n'est pas encore prêt."));
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Lier un autre poste"));
    dlg.setMinimumWidth(480);
    auto *lay = new QVBoxLayout(&dlg);
    auto *intro = new QLabel(
        tr("<p>Sur le nouveau poste, ouvrez P2P Chat, puis ⋮ → <b>Lier ce poste à mon compte…</b> et saisissez ce "
           "code. Il recevra votre identité, votre pseudo, votre avatar et votre dictionnaire, et rejoindra vos "
           "conversations avec leurs 30 derniers jours d'historique.</p>"
           "<p><b>Ce code donne accès à votre identité : ne le transmettez qu'à vous-même.</b> "
           "Il ne sert qu'une fois, pendant 10 minutes.</p>"),
        &dlg);
    intro->setWordWrap(true);
    lay->addWidget(intro);
    auto *codeEdit = new QPlainTextEdit(&dlg);
    codeEdit->setReadOnly(true);
    codeEdit->setMaximumHeight(90);
    lay->addWidget(codeEdit);
    auto *status = new QLabel(&dlg);
    status->setWordWrap(true);
    lay->addWidget(status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    QPushButton *copy = buttons->addButton(tr("Copier le code"), QDialogButtonBox::ActionRole);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(buttons);

    // Conversations à rejoindre : celles dont nous sommes membres, préparées pour ce poste
    int joined = 0;
    auto provider = [this, &joined](const QString &onion, const QByteArray &key) {
        QList<DeviceLink::Conversation> list;
        for (const auto &c : m_convs)
            if (const auto inv = c->node().invitationForMyDevice(onion, key))
                list.append({*inv, c->store().name()});
        joined = int(list.size());
        return list;
    };
    DeviceLink::Host host(*m_group, m_onion, m_settings.value("spelling/words").toStringList(), provider);
    m_board->setLinkHost(&host);
    codeEdit->setPlainText(host.code().toCode());
    connect(copy, &QPushButton::clicked, &dlg, [codeEdit] { QApplication::clipboard()->setText(codeEdit->toPlainText()); });

    QTimer tick;
    auto showWaiting = [&host, status] {
        status->setText(tr("En attente du nouveau poste… (encore %n minute(s))", nullptr,
                           int((host.remainingMs() + 59999) / 60000)));
    };
    connect(&tick, &QTimer::timeout, &dlg, showWaiting);
    tick.start(5000);
    showWaiting();
    auto finished = [&](const QString &text) {
        tick.stop();
        codeEdit->clear();
        codeEdit->setEnabled(false);
        copy->setEnabled(false);
        status->setText(text);
    };
    connect(&host, &DeviceLink::Host::linked, &dlg, [&] {
        finished(tr("✓ Poste lié. Il rejoint %n conversation(s).", nullptr, joined));
        updateStatus();
    });
    connect(&host, &DeviceLink::Host::refused, &dlg, [&](const QString &reason) {
        if (host.isActive()) // le code reste valable (autre tentative)
            status->setText(tr("⚠ Liaison refusée : %1.").arg(reason));
        else
            finished(tr("⚠ Liaison refusée : %1. Fermez et recommencez pour obtenir un nouveau code.").arg(reason));
    });
    connect(&host, &DeviceLink::Host::expired, &dlg,
            [&] { finished(tr("Code expiré. Fermez et recommencez pour en obtenir un nouveau.")); });
    dlg.exec();
    m_board->setLinkHost(nullptr);
}

void MainWindow::linkThisDevice()
{
    if (m_group->personAnnounced()) {
        QMessageBox::information(
            this, tr("Lier ce poste"),
            tr("<p>Ce poste s'est déjà présenté sous sa propre identité aux membres de vos conversations : il ne "
               "peut plus être rattaché à un autre compte.</p>"
               "<p>Pour utiliser un même compte sur plusieurs postes, liez-les depuis un poste qui n'a encore "
               "rejoint aucune conversation.</p>"));
        return;
    }
    if (m_onion.isEmpty()) {
        QMessageBox::information(this, tr("Lier ce poste"), tr("Le réseau n'est pas encore prêt."));
        return;
    }
    bool ok = false;
    const QString text = QInputDialog::getMultiLineText(
        this, tr("Lier ce poste"),
        tr("Sur votre autre poste : ⋮ → Lier un autre poste…, puis collez ici le code affiché.\n"
           "Ce poste prendra votre identité, votre pseudo, votre avatar et votre dictionnaire."),
        {}, &ok);
    if (!ok || text.trimmed().isEmpty())
        return;
    const auto code = DeviceLink::Code::fromCode(text);
    if (!code) {
        QMessageBox::warning(this, tr("Lier ce poste"), tr("Ce code n'est pas valide (incomplet ?)."));
        return;
    }
    if (code->onion == m_onion) {
        QMessageBox::warning(this, tr("Lier ce poste"), tr("C'est le code de ce poste-ci : saisissez-le sur l'autre."));
        return;
    }
    QTcpSocket *socket = dialTor(code->onion);
    if (!socket)
        return;

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Lier ce poste"));
    auto *lay = new QVBoxLayout(&dlg);
    auto *status = new QLabel(tr("Connexion à votre autre poste par Tor… (jusqu'à une minute)"), &dlg);
    status->setWordWrap(true);
    lay->addWidget(status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(buttons);

    DeviceLink::Guest guest(*m_group, m_onion, *code, socket);
    QString error;
    DeviceLink::Profile profile;
    QList<DeviceLink::Conversation> conversations;
    bool linked = false;
    connect(&guest, &DeviceLink::Guest::linked, &dlg,
            [&](const DeviceLink::Profile &p, const QList<DeviceLink::Conversation> &list) {
                linked = true;
                profile = p;
                conversations = list;
                dlg.accept();
            });
    connect(&guest, &DeviceLink::Guest::failed, &dlg, [&](const QString &reason) {
        error = reason;
        dlg.reject();
    });
    dlg.exec();
    if (!linked) {
        if (!error.isEmpty())
            QMessageBox::warning(this, tr("Lier ce poste"), tr("La liaison a échoué : %1.").arg(error));
        return;
    }
    applyLink(profile, conversations);
}

void MainWindow::applyLink(const DeviceLink::Profile &profile, const QList<DeviceLink::Conversation> &conversations)
{
    // Dictionnaire personnel : réuni avec celui de ce poste
    QStringList words = m_settings.value("spelling/words").toStringList();
    for (const QString &w : profile.words)
        if (!words.contains(w)) {
            words << w;
            if (m_spell)
                m_spell->addWord(w);
        }
    m_settings.setValue("spelling/words", words);
    m_input->recheckSpelling();
    // Pseudo et avatar (déjà enregistrés) : annoncés dans les conversations de ce poste
    for (const auto &c : m_convs) {
        c->node().announceNickname();
        c->node().publishAvatar();
    }
    int count = 0;
    for (const DeviceLink::Conversation &conv : conversations)
        count += joinLinked(conv.invitation, conv.name) ? 1 : 0;
    appendSystem(tr("Ce poste est lié à votre compte. %n conversation(s) rejointe(s) : l'historique des 30 "
                    "derniers jours arrive dès que votre autre poste est joignable.", nullptr, count));
    updateStatus();
}

bool MainWindow::joinLinked(const Invitation &inv, const QString &name)
{
    for (const auto &c : m_convs) {
        const bool sameGroup = !inv.groupId.isEmpty() && c->node().groupId() == inv.groupId;
        if (c->store().inGroup() && (sameGroup || c->node().acceptedSecrets().contains(inv.secret)))
            return false; // déjà membre
    }
    const bool created = !m_shown->isBlank();
    Conversation *target = created ? newConversation() : m_shown;
    if (target->node().join(inv) != GroupNode::JoinResult::Joined) {
        if (created)
            closeConversation(target);
        return false;
    }
    if (!name.isEmpty())
        target->store().setName(name); // le nom qu'on lui donnait sur l'autre poste
    updateConversationItem(target);
    return true;
}
