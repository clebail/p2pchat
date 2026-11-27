#include "mainwindow.h"

#include "appstyle.h"
#include "attachment.h"
#include "chatview.h"
#include "composer.h"
#include "crypto.h"
#include "devicelink.h"
#include "invitation.h"
#include "language.h"
#include "mentioncompleter.h"
#include "mentions.h"
#include "notifications.h"
#include "notifier.h"
#include "governance.h"
#include "groupnode.h"
#include "protocol.h"
#include "safetynumber.h"
#include "spellchecker.h"
#include "switchboard.h"
#include "torservice.h"
#ifdef P2P_TERMINAL
#  include "terminalsession.h"
#  include "terminalsharing.h"
#endif

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QComboBox>
#include <QSystemTrayIcon>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QActionGroup>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QStackedWidget>
#include <QToolButton>
#include <QMessageBox>
#include <QNetworkProxy>
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
#  include <QNetworkInformation>
#endif
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QShortcut>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <algorithm>

using Protocol::isValidOnion;

namespace {

QString torInstallHelp()
{
#if defined(Q_OS_MACOS)
    return QObject::tr("Installez tor avec Homebrew (<code>brew install tor</code>), ou installez Tor Browser "
                       "dans Applications, ou placez l'exécutable <code>tor</code> dans P2PChat.app/Contents/MacOS.");
#elif defined(Q_OS_WIN)
    return QObject::tr("Téléchargez le « Tor Expert Bundle » sur torproject.org et copiez son dossier "
                       "<code>tor</code> à côté de p2pchat.exe (ou installez Tor Browser sur le Bureau).");
#else
    return QObject::tr("Installez le paquet tor (<code>sudo apt install tor</code>). "
                       "Inutile d'activer le service : P2P Chat lance sa propre instance.");
#endif
}

} // namespace

QString MainWindow::dataDir()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir;
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    GroupStore(m_settings).ensureIdentity();

    // Le secret du groupe, la clé d'identité, les clés Tor et l'historique : lisibles par nous seuls
    constexpr auto OwnerRW = QFile::ReadOwner | QFile::WriteOwner;
    m_settings.sync();
    QFile::setPermissions(m_settings.fileName(), OwnerRW);
    QFile::setPermissions(dataDir(), OwnerRW | QFile::ExeOwner);

    // Écoute commune à toutes les conversations, jointe par le service onion
    m_board = new Switchboard(this);
    QString listenError;
    if (!m_board->listen(&listenError))
        QMessageBox::critical(this, tr("P2P Chat"), tr("Écoute locale impossible : %1").arg(listenError));

#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    // Changement de réseau (Wi-Fi, 4G, câble) : inutile d'attendre la fin des délais d'attente
    if (QNetworkInformation::loadBackendByFeatures(QNetworkInformation::Feature::Reachability)) {
        connect(QNetworkInformation::instance(), &QNetworkInformation::reachabilityChanged, this,
                [this](QNetworkInformation::Reachability r) {
                    qInfo().noquote() << "[réseau] joignabilité :" << int(r);
                    if (r == QNetworkInformation::Reachability::Online)
                        QTimer::singleShot(3000, this, [this] { // laisse Tor s'en apercevoir
                            for (const auto &c : m_convs)
                                c->node().retrySoon();
                        });
                });
    }
#endif

    buildUi();
    applySpelling();
    setupTray();
    m_notifier = new Notifier(this);
    m_notifier->setTray(m_tray);
    connect(m_notifier, &Notifier::activated, this, [this] {
        if (Conversation *c = conversationById(m_lastNotified))
            showConversation(c);
        showAndRaise();
    });
#ifdef Q_OS_MACOS
    // macOS : la croix masque la fenêtre, un clic sur l'icône du Dock la rouvre
    m_hideOnClose = true;
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive && !isVisible())
            showAndRaise();
    });
#endif
    if (m_hideOnClose)
        QApplication::setQuitOnLastWindowClosed(false);

    // Conversations : l'ancienne disposition (un seul groupe) devient la première
    m_conversations.migrate();
    for (const QString &id : m_conversations.ids())
        openConversation(id);
    showConversation(m_convs.empty() ? newConversation() : m_convs.front().get());
    setupDeviceSync();
    startTor();
}

MainWindow::~MainWindow()
{
    // Les conversations (et leurs groupes) avant la fenêtre : plus aucun de leurs signaux vers nous
    for (const auto &c : m_convs) {
        c->disconnect(this);
        c->node().disconnect(this);
    }
    m_sync.reset();
    if (m_selfNode)
        m_selfNode->disconnect(this);
#ifdef P2P_TERMINAL
    qDeleteAll(m_sharings);
#endif
    m_sharings.clear();
    m_convs.clear();
}

Conversation *MainWindow::openConversation(const QString &id)
{
    const QString historyPath = m_conversations.historyPath(id);
    QFile::setPermissions(m_conversations.dir(id), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    if (QFile::exists(historyPath))
        QFile::setPermissions(historyPath, QFile::ReadOwner | QFile::WriteOwner);
    m_convs.push_back(std::make_unique<Conversation>(m_settings, id, historyPath));
    Conversation *c = m_convs.back().get();

    // Transport : Tor ; écoute commune à toutes les conversations
    c->node().setDialer([this](const QString &onion) { return dialTor(onion); });
    m_board->add(&c->node());
    m_pages->addWidget(c->view());
    auto *item = new QListWidgetItem(m_convList);
    item->setData(Qt::UserRole, id);
    m_items.insert(c, item);

    connect(c, &Conversation::changed, this, [this, c] { onConversationChanged(c); });
    connect(c, &Conversation::messagesArrived, this,
            [this, c](int count, const QString &title, const QString &text, qint64 firstTs, bool forMe) {
                onMessagesArrived(c, count, title, text, firstTs, forMe);
            });
    connect(c, &Conversation::avatarChanged, this, [this](const QString &onion) {
        for (const auto &other : m_convs) // commun à toutes les conversations
            other->forgetAvatar(onion);
    });
    // Différé : le groupe de nos postes est quitté, hors du traitement du message reçu
    connect(&c->node(), &GroupNode::unlinked, this, &MainWindow::onUnlinked, Qt::QueuedConnection);
    connect(c, &Conversation::excludedFromGroup, this, [this, c](const QString &by) {
        showNotice(tr("Exclu du groupe"),
                   tr("%1, administrateur de « %2 », vous en a exclu.\n\n"
                      "Vous ne recevez plus les messages. L'historique déjà reçu est conservé.")
                       .arg(by, c->displayName()));
    });
    connect(c, &Conversation::directInvitation, this,
            [this, c](const QString &nick, const Invitation &inv, const QString &name) {
                askDirectInvitation(c, nick, inv, name);
            });
    connect(c, &Conversation::replyRequested, this, &MainWindow::startReply);
    connect(c, &Conversation::editRequested, this, &MainWindow::startEdit);
    connect(c, &Conversation::deleteRequested, this, &MainWindow::confirmDelete);
    connect(c, &Conversation::linkClicked, this, &MainWindow::openLink);
    connect(c, &Conversation::openFileRequested, this, &MainWindow::openFile);
    connect(c, &Conversation::saveFileRequested, this, &MainWindow::saveFileAs);
    connect(c->view(), &ChatView::searchResultChanged, this, [this](int current, int total) {
        m_searchCount->setText(m_searchEdit->text().trimmed().isEmpty() ? QString()
                               : total == 0                             ? tr("Aucun résultat")
                                                                        : tr("%1 / %2").arg(current).arg(total));
    });
    setupTerminalSharing(c);
    c->loadHistory();
    // Conversation rejointe ici : nos autres postes la rejoignent aussi (cf. devicesync.h)
    connect(&c->node(), &GroupNode::stateChanged, this, [this] {
        if (m_syncCheck)
            m_syncCheck->start();
    });
    if (!m_onion.isEmpty())
        c->node().setOnline(m_onion);
    updateConversationItem(c);
    return c;
}

void MainWindow::showNotice(const QString &title, const QString &text)
{
    auto *box = new QMessageBox(QMessageBox::Information, title, text, QMessageBox::Ok, this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->open(); // sans bloquer la boucle d'événements
}

QTcpSocket *MainWindow::dialTor(const QString &onion)
{
    if (!m_tor)
        return nullptr;
    auto *s = new QTcpSocket;
    s->setProxy(QNetworkProxy(QNetworkProxy::Socks5Proxy, "127.0.0.1", m_tor->socksPort()));
    s->connectToHost(onion, TorService::OnionPort); // résolu par Tor, pas localement
    return s;
}

Conversation *MainWindow::newConversation()
{
    return openConversation(m_conversations.create());
}

void MainWindow::closeConversation(Conversation *c)
{
    const auto it = std::find_if(m_convs.begin(), m_convs.end(), [c](const auto &p) { return p.get() == c; });
    if (it == m_convs.end())
        return;
    if (m_shown == c) {
        m_shown = nullptr;
        m_group = nullptr;
        m_node = nullptr;
        m_chat = nullptr;
        m_sharing = nullptr;
    }
    const QString id = c->id();
    m_board->remove(&c->node());
#ifdef P2P_TERMINAL
    delete m_sharings.take(c); // avant le groupe, qu'elle utilise
#endif
    delete m_items.take(c);
    m_convs.erase(it); // la conversation, son groupe et sa vue
    m_conversations.remove(id);
}

Conversation *MainWindow::conversationById(const QString &id) const
{
    for (const auto &c : m_convs)
        if (c->id() == id)
            return c.get();
    return nullptr;
}

void MainWindow::showConversation(Conversation *c)
{
    if (!c)
        return;
    const bool changed = m_shown != c;
    if (m_shown && changed) {
        cancelReply(); // d'abord : une modification en cours n'est pas un brouillon
        m_shown->draft = m_input->text();
        cancelImage();
        closeSearch();
    }
    m_shown = c;
    m_group = &c->store();
    m_node = &c->node();
    m_chat = c->view();
    m_sharing = m_sharings.value(c);
    if (changed) {
        m_input->setText(c->draft);
        m_convList->setCurrentItem(m_items.value(c));
    }
    if (isActiveWindow())
        c->markRead();
    updateUnread();
    updateStatus();
    if (changed && m_sidebar->isVisible() && isNarrow())
        setSidebarVisible(false); // étroit : la conversation prend toute la place
}

void MainWindow::onConversationChanged(Conversation *c)
{
    updateConversationItem(c);
    updateUnread();
    if (c == m_shown)
        updateStatus();
}

void MainWindow::updateConversationItem(Conversation *c)
{
    QListWidgetItem *item = m_items.value(c);
    if (!item)
        return;
    QString text = c->displayName();
    if (c->unread())
        text += QStringLiteral("  (%1)").arg(c->unread());
    QString second = c->preview();
    if (!c->store().inGroup() && !c->isBlank())
        second = tr("Vous avez quitté cette conversation");
    if (!second.isEmpty()) {
        second.replace('\n', ' ');
        text += '\n' + (second.size() > 42 ? second.left(41) + QStringLiteral("…") : second);
    }
    item->setText(text);
    QFont f = m_convList->font();
    f.setBold(c->unread() > 0);
    item->setFont(f);
    item->setToolTip(c->displayName());
    item->setIcon(c->avatarPixmap(ConvAvatarSide, devicePixelRatioF()));
}

void MainWindow::startConversation()
{
    // Une conversation vierge (page d'accueil) sert d'abord
    showConversation(m_shown->isBlank() ? m_shown : newConversation());
    invite();
}

void MainWindow::renameConversation()
{
    // Réservé aux administrateurs : le nom vaut pour tous les membres
    if (!m_node->isAdmin() || m_node->groupId().isEmpty())
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Renommer la conversation"),
                                               tr("Nom de la conversation, pour tous les membres (vide : leurs pseudos) :"),
                                               QLineEdit::Normal, m_node->sharedName(), &ok);
    if (!ok)
        return;
    QString error;
    if (!m_node->setSharedName(name, &error))
        QMessageBox::warning(this, tr("Renommer la conversation"), tr("Impossible : %1.").arg(error));
    onConversationChanged(m_shown);
}

void MainWindow::deleteConversation()
{
    if (m_group->inGroup()) {
        resetAll();
        if (m_group->inGroup())
            return;
    }
    if (!m_shown->isBlank()
        && QMessageBox::question(this, tr("Supprimer la conversation"),
                                 tr("Supprimer « %1 » de ce poste ?\n"
                                    "Son historique et ses fichiers seront effacés.").arg(m_shown->displayName()))
               != QMessageBox::Yes)
        return;
    Conversation *gone = m_shown;
    Conversation *next = nullptr;
    for (const auto &c : m_convs)
        if (c.get() != gone) {
            next = c.get();
            break;
        }
    closeConversation(gone);
    showConversation(next ? next : newConversation());
}

// ---------------------------------------------------------------- Système (zone de notification, fermeture)

void MainWindow::setupTray()
{
#ifndef Q_OS_MACOS
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return; // ex. GNOME sans l'extension AppIndicator : la croix quitte normalement
    m_tray = new QSystemTrayIcon(QApplication::windowIcon(), this);
    m_tray->setToolTip(tr("P2P Chat"));
    auto *menu = new QMenu(this);
    menu->addAction(tr("Ouvrir P2P Chat"), this, &MainWindow::showAndRaise);
    menu->addSeparator();
    menu->addAction(tr("Quitter"), qApp, &QCoreApplication::quit);
    m_tray->setContextMenu(menu);
    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason != QSystemTrayIcon::Trigger && reason != QSystemTrayIcon::DoubleClick)
            return;
        if (isVisible() && isActiveWindow())
            hide();
        else
            showAndRaise();
    });
    m_tray->show();
    m_hideOnClose = true;
#endif
}

void MainWindow::showAndRaise()
{
    show();
    // Wayland interdit à une application de s'activer elle-même
    // (QWindow::requestActivate() n'y est pas supporté et produit un
    // avertissement) : on se contente d'afficher/dé-minimiser la fenêtre.
    if (QGuiApplication::platformName().startsWith(QLatin1String("wayland"))) {
        setWindowState(windowState() & ~Qt::WindowMinimized);
        return;
    }
    setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
    raise();
    activateWindow();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // Clic sur la croix (événement « spontané ») : on masque seulement.
    // Quitter (menu, Cmd+Q, zone de notification) envoie un événement non spontané.
    if (m_hideOnClose && event->spontaneous()) {
        event->ignore();
        hide();
        if (m_tray && !m_settings.value("trayHintShown").toBool()) {
            m_settings.setValue("trayHintShown", true);
            m_notifier->notify(tr("P2P Chat"), tr("P2P Chat reste ouvert dans la zone de notification "
                                                  "pour recevoir les messages."));
        }
        return;
    }
    event->accept();
}

void MainWindow::updateUnread()
{
    m_unread = 0;
    for (const auto &c : m_convs)
        m_unread += c->unread();
    setWindowTitle(m_unread ? tr("(%1) P2P Chat").arg(m_unread) : tr("P2P Chat"));
    m_notifier->setBadge(m_unread);
    if (!m_tray)
        return;
    if (m_unread == 0) {
        m_tray->setIcon(QApplication::windowIcon());
        m_tray->setToolTip(tr("P2P Chat"));
        return;
    }
    // Icône avec une pastille rouge
    QPixmap pm = QApplication::windowIcon().pixmap(64, 64);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(Qt::white, 4));
    p.setBrush(QColor("#d93025"));
    p.drawEllipse(QPointF(48, 16), 14, 14);
    p.end();
    m_tray->setIcon(QIcon(pm));
    m_tray->setToolTip(tr("P2P Chat — %n message(s) non lu(s)", nullptr, m_unread));
}


void MainWindow::setSidebarVisible(bool visible)
{
    m_sidebar->setVisible(visible);
}

// En deçà, la colonne des conversations se replie. 640 px d'ordinaire ; davantage si la colonne et le contenu ne
// tiennent pas dedans (polices plus larges) : sans quoi la fenêtre, bornée à leur largeur minimale, ne pourrait
// jamais devenir assez étroite pour la replier.
bool MainWindow::isNarrow() const
{
    const int content = m_pages->parentWidget()->minimumSizeHint().width();
    return width() < qMax(640, m_sidebar->width() + content + 1);
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    // Étroit : la colonne se replie (le bouton ☰ la rouvre par-dessus) ; large : comme l'a voulu l'utilisateur
    if (m_sidebar)
        setSidebarVisible(isNarrow() ? false : m_sidebarWanted);
}

void MainWindow::updateStatus()
{
    if (!m_shown)
        return;
    const QList<Member> list = m_group->members();
    m_pages->setCurrentWidget(m_shown->isBlank() ? m_home : static_cast<QWidget *>(m_chat));

    // Titre : le nom de la conversation, ou les pseudos des membres
    m_title->setText(m_shown->isBlank() ? tr("P2P Chat") : m_shown->displayName());
    m_titleAvatar->setVisible(!m_shown->isBlank());
    m_titleAvatar->setPixmap(m_shown->avatarPixmap(TitleAvatarSide, devicePixelRatioF()));
    QStringList others;
    for (const Member &m : list)
        others << m.nick;
    m_mentions->setNames(others << Mentions::Everyone);

    // Réseau d'abord (commun à toutes les conversations), puis l'état de celle-ci
    const QString dot = QStringLiteral("<span style='color:%1'>●</span> ");
    if (!m_torError.isEmpty())
        m_subtitle->setText(dot.arg("#d93025") + tr("Tor : %1").arg(m_torError.toHtmlEscaped()));
    else if (m_onion.isEmpty())
        m_subtitle->setText(dot.arg("#e37400") + tr("Connexion au réseau… %1").arg(m_torStatus.toHtmlEscaped()));
    else
        m_subtitle->setText(m_shown->subtitle());
    m_subtitle->setToolTip(m_shown->subtitleToolTip());

    const bool online = m_node->hasConnections();
    const bool canWrite = m_node->canSend();
    m_input->setEnabled(canWrite);
    m_sendButton->setEnabled(canWrite);
    m_attachButton->setEnabled(canWrite);
    m_input->setPlaceholderText(online ? tr("Message") : tr("Message (transmis à la prochaine connexion)"));
    if (canWrite && !m_input->hasFocus())
        m_input->setFocus();
}

void MainWindow::startReply(const QString &id, const QString &author, const QString &text)
{
    if (!m_editId.isEmpty())
        cancelReply(); // on ne répond pas en modifiant
    const bool mine = m_node->isMine(id);
    m_replyTo = id;
    m_replyLabel->setText(tr("Réponse à <b>%1</b><br><span style='color:#5f6368'>%2</span>")
                              .arg((mine ? tr("vous-même") : author).toHtmlEscaped(),
                                   ChatView::excerpt(text, 90).toHtmlEscaped()));
    m_replyBar->setVisible(true);
    m_input->setFocus();
}

void MainWindow::startEdit(const QString &id, const QString &text)
{
    cancelReply();
    cancelImage();
    m_editId = id;
    const QJsonObject msg = m_node->storedMessage(id);
    const QString whose = msg.value("mine").toBool()
                              ? tr("Modification de votre message")
                              : tr("Modification du message de %1 (modération)").arg(memberNick(msg.value("a").toString()));
    m_replyLabel->setText(tr("<b>%1</b><br><span style='color:#5f6368'>Entrée : enregistrer · Échap : annuler</span>")
                              .arg(whose.toHtmlEscaped()));
    m_replyBar->setVisible(true);
    m_input->setText(text);
    m_input->setFocus();
}

void MainWindow::confirmDelete(const QString &id)
{
    const QJsonObject msg = m_node->storedMessage(id);
    const QString question = msg.value("mine").toBool()
                                 ? tr("Supprimer ce message pour tout le monde ?")
                                 : tr("Supprimer le message de %1 pour tout le monde (modération) ? Votre nom "
                                      "apparaîtra à sa place.").arg(memberNick(msg.value("a").toString()));
    if (QMessageBox::question(this, tr("Supprimer le message"),
                              question + tr("\n\n"
                                 "Il est remplacé par « Message supprimé » chez les membres à jour. Ceux qui "
                                 "utilisent une version précédente de P2P Chat le voient encore, et rien ne "
                                 "garantit qu'il disparaisse de leur disque."))
        != QMessageBox::Yes)
        return;
    if (!m_node->deleteMessage(id))
        QMessageBox::warning(this, tr("Supprimer le message"), tr("Impossible de supprimer ce message pour l'instant."));
}

void MainWindow::cancelReply()
{
    if (!m_editId.isEmpty()) {
        m_editId.clear();
        m_input->clear(); // le texte était celui du message modifié
    }
    m_replyTo.clear();
    m_replyBar->setVisible(false);
}

void MainWindow::clearConversation()
{
    if (QMessageBox::question(this, tr("Vider la conversation"),
                              tr("Effacer tous les messages de cette fenêtre ?\n"
                                 "Ils sont supprimés uniquement sur cet ordinateur, et ne seront pas "
                                 "récupérés auprès des autres membres."))
        != QMessageBox::Yes)
        return;
    m_chat->clearAll();
    cancelReply();
    cancelImage();
    m_node->clearHistory();
    updateStatus();
}

void MainWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::ActivationChange && isActiveWindow() && m_shown) {
        m_shown->markRead(); // signal changed : colonne et compteur à jour
    }
    QMainWindow::changeEvent(event);
}

// ---------------------------------------------------------------- Tor

void MainWindow::startTor()
{
    delete m_tor;
    m_tor = nullptr;
    m_onion.clear();
    for (const auto &c : m_convs)
        c->node().setOffline();
    if (m_selfNode)
        m_selfNode->setOffline();
    m_torError.clear();
    m_torStatus.clear();

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/tor";
    m_tor = new TorService(dir, m_board->port(), this);
    connect(m_tor, &TorService::progress, this, [this](int pct, const QString &summary) {
        m_torStatus = QStringLiteral("%1 % (%2)").arg(pct).arg(summary);
        updateStatus();
    });
    connect(m_tor, &TorService::ready, this, [this](const QString &onion) {
        m_onion = onion;
        for (const auto &c : m_convs)
            c->node().setOnline(onion);
        if (m_selfNode)
            m_selfNode->setOnline(onion);
        if (Conversation *c = conversationById(m_inviteFor)) {
            m_inviteFor.clear();
            showConversation(c);
            showInvitation();
        }
    });
    connect(m_tor, &TorService::failed, this, &MainWindow::onTorFailed);
    m_tor->start();
    updateStatus();
}

void MainWindow::onTorFailed(const QString &error)
{
    m_torError = error;
    m_onion.clear();
    for (const auto &c : m_convs)
        c->node().setOffline();
    if (m_selfNode)
        m_selfNode->setOffline();
    updateStatus();

    QMessageBox box(QMessageBox::Warning, tr("P2P Chat"),
                    tr("<b>Tor n'a pas pu démarrer</b> : %1<br><br>%2").arg(error.toHtmlEscaped(), torInstallHelp()),
                    QMessageBox::NoButton, this);
    QPushButton *retry = box.addButton(tr("Réessayer"), QMessageBox::AcceptRole);
    QPushButton *choose = box.addButton(tr("Choisir l'exécutable tor…"), QMessageBox::ActionRole);
    box.addButton(QMessageBox::Close);
    box.exec();
    if (box.clickedButton() == choose) {
        const QString path = QFileDialog::getOpenFileName(this, tr("Exécutable tor"));
        if (path.isEmpty())
            return;
        m_settings.setValue("torPath", path);
        startTor();
    } else if (box.clickedButton() == retry) {
        startTor();
    }
}

// ---------------------------------------------------------------- Mise en relation

void MainWindow::invite()
{
    m_node->createGroup();
    if (m_node->ownOnion().isEmpty()) {
        m_inviteFor = m_shown->id();
        appendSystem(tr("Le code s'affichera dès que Tor aura démarré…"));
        updateStatus();
        return;
    }
    showInvitation();
    updateStatus();
}

void MainWindow::join()
{
    bool ok = false;
    const QString text = QInputDialog::getMultiLineText(this, tr("Rejoindre"),
                                                        tr("Collez le code d'invitation reçu :"), {}, &ok);
    if (!ok || text.trimmed().isEmpty())
        return;
    const auto inv = Invitation::fromCode(text);
    if (!inv && DeviceLink::Code::looksLikeCode(text)) {
        QMessageBox::information(this, tr("Rejoindre"),
                                 tr("C'est un code de liaison de poste : utilisez-le avec ⋮ → Lier ce poste à mon "
                                    "compte…"));
        return;
    }
    if (!inv) {
        QMessageBox::warning(this, tr("Rejoindre"), tr("Ce code n'est pas valide (incomplet ?)."));
        return;
    }
    joinWith(*inv);
}

void MainWindow::joinWith(const Invitation &inv)
{
    if (inv.onion == m_onion) {
        QMessageBox::warning(this, tr("Rejoindre"), tr("C'est votre propre code !"));
        return;
    }
    // Conversation déjà connue (même groupe), sinon la vierge affichée, sinon une nouvelle
    Conversation *target = nullptr;
    for (const auto &c : m_convs) {
        const bool sameGroup = !inv.groupId.isEmpty() && c->node().groupId() == inv.groupId;
        if (c->store().inGroup() && (sameGroup || c->node().acceptedSecrets().contains(inv.secret)))
            target = c.get();
    }
    const bool created = !target && !m_shown->isBlank();
    if (!target)
        target = m_shown->isBlank() ? m_shown : newConversation();
    showConversation(target);
    const GroupNode::JoinResult result = m_node->join(inv);
    if (result == GroupNode::JoinResult::Joined) {
        appendSystem(tr("Invitation de %1 enregistrée.").arg(inv.nickname));
        updateStatus();
        return;
    }
    if (created) { // rien à garder
        closeConversation(target);
        showConversation(m_convs.front().get());
    }
    if (result == GroupNode::JoinResult::OwnCode)
        QMessageBox::warning(this, tr("Rejoindre"), tr("C'est votre propre code !"));
    else
        QMessageBox::warning(this, tr("Rejoindre"),
                             tr("<p>Ce code présente une clé d'identité différente de celle déjà connue pour %1.<br>"
                                "Par sécurité, il est refusé.</p>").arg(inv.nickname.toHtmlEscaped()));
}

void MainWindow::startConversationWith(const QStringList &onions, const QString &name)
{
    // Conversation privée déjà ouverte avec ce membre : on y retourne
    if (onions.size() == 1 && name.trimmed().isEmpty()) {
        for (const auto &c : m_convs) {
            const QList<Member> members = c->store().members();
            if (c->store().inGroup() && members.size() == 1 && members.first().onion == onions.first()) {
                showConversation(c.get());
                return;
            }
        }
    }
    if (m_onion.isEmpty()) {
        QMessageBox::information(this, tr("Nouvelle conversation"), tr("Le réseau n'est pas encore prêt."));
        return;
    }
    Conversation *source = m_shown;
    Conversation *c = newConversation();
    c->node().createGroup();
    if (!name.trimmed().isEmpty())
        c->node().setSharedName(name);
    const Invitation inv{c->store().secret(), m_onion, c->store().nickname(), c->store().identityKey(), c->node().groupId()};
    QStringList sent, failed;
    for (const QString &onion : onions)
        (source->node().inviteDirectly(onion, inv, name) ? sent : failed) << source->memberNick(onion);
    showConversation(c);
    if (!sent.isEmpty())
        appendSystem(tr("Invitation envoyée à %1 : la conversation commence dès qu'elle est acceptée.")
                         .arg(sent.join(", ")));
    if (!failed.isEmpty())
        appendSystem(tr("⚠ Invitation impossible pour %1 (plus connecté). ⋮ → Inviter quelqu'un… donne un code "
                        "à envoyer autrement.").arg(failed.join(", ")));
}

void MainWindow::askDirectInvitation(Conversation *from, const QString &nick, const Invitation &invitation,
                                     const QString &name)
{
    const QString text =
        (name.isEmpty() ? tr("<b>%1</b> vous propose une conversation privée.").arg(nick.toHtmlEscaped())
                        : tr("<b>%1</b> vous invite à la conversation « %2 ».")
                              .arg(nick.toHtmlEscaped(), name.toHtmlEscaped()))
        + tr("<br><span style='color:#5f6368'>Invitation reçue dans « %1 ».</span>")
              .arg(from->displayName().toHtmlEscaped());
    auto *box = new QMessageBox(QMessageBox::Question, tr("Invitation"), text, QMessageBox::NoButton, this);
    QPushButton *accept = box->addButton(tr("Rejoindre"), QMessageBox::AcceptRole);
    box->addButton(tr("Refuser"), QMessageBox::RejectRole);
    box->setAttribute(Qt::WA_DeleteOnClose);
    connect(box, &QMessageBox::finished, this, [this, box, accept, invitation] {
        if (box->clickedButton() == accept)
            joinWith(invitation);
    });
    box->open();
    if (!isVisible() || !isActiveWindow()) {
        m_lastNotified = from->id();
        m_notifier->notify(nick, name.isEmpty() ? tr("vous propose une conversation privée")
                                                : tr("vous invite à « %1 »").arg(name));
        if (isVisible())
            QApplication::alert(this);
    }
}

// ---------------------------------------------------------------- Membres et connexions

void MainWindow::reconnectNow()
{
    if (!m_torError.isEmpty()) { // Tor en panne : on le relance, il reconnectera une fois prêt
        startTor();
        return;
    }
    for (const auto &c : m_convs)
        c->node().reconnectNow();
}

// ---------------------------------------------------------------- Notifications

void MainWindow::onMessagesArrived(Conversation *c, int count, const QString &title, const QString &text,
                                   qint64 firstTs, bool forMe)
{
    if (c == m_shown && isVisible() && isActiveWindow())
        return; // sous les yeux
    c->addUnread(count, firstTs); // compteur et pastille : toujours à jour
    m_lastNotified = c->id();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!c->notificationsAllowed(forMe, m_settings.value("notify/pausedUntil").toLongLong(), now))
        return;
    // Plusieurs conversations : on précise laquelle
    m_notifier->notify(m_convs.size() > 1 ? tr("%1 · %2").arg(title, c->displayName()) : title, text);
    if (isVisible())
        QApplication::alert(this);
}

void MainWindow::buildNotificationMenu(QMenu *menu)
{
    using namespace Notifications;
    // Reconstruit à chaque ouverture : la pause affichée dépend de l'heure
    menu->clear();
    qDeleteAll(menu->findChildren<QActionGroup *>()); // groupes de l'ouverture précédente
    // Propre à la conversation affichée (aucune encore pendant la construction de la fenêtre)
    const Mode mode = modeFromString(m_group ? m_group->notifyMode() : m_settings.value("notify/mode").toString());
    auto *group = new QActionGroup(menu);
    const QList<QPair<Mode, QString>> modes{{All, tr("Tous les messages")},
                                           {ForMe, tr("Mentions et réponses seulement")},
                                           {Silent, tr("Aucune")}};
    for (const auto &m : modes) {
        QAction *a = menu->addAction(m.second, this, [this, value = m.first] {
            if (m_group)
                m_group->setNotifyMode(modeToString(value));
        });
        a->setCheckable(true);
        a->setChecked(mode == m.first);
        group->addAction(a);
    }
    menu->addSeparator();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 paused = m_settings.value("notify/pausedUntil").toLongLong();
    if (paused > now) {
        const QDateTime until = QDateTime::fromMSecsSinceEpoch(paused);
        menu->addAction(tr("Reprendre (en pause jusqu'à %1)")
                            .arg(until.date() == QDate::currentDate() ? until.toString("HH:mm")
                                                                      : tr("demain %1").arg(until.toString("HH:mm"))),
                        this, [this] { m_settings.remove("notify/pausedUntil"); });
    } else {
        menu->addAction(tr("Ne pas déranger pendant 1 h"), this, [this] {
            m_settings.setValue("notify/pausedUntil", QDateTime::currentDateTime().addSecs(3600).toMSecsSinceEpoch());
        });
        menu->addAction(tr("Ne pas déranger jusqu'à demain 8 h"), this, [this] {
            m_settings.setValue("notify/pausedUntil", tomorrowMorning(QDateTime::currentDateTime()).toMSecsSinceEpoch());
        });
    }
}

// ---------------------------------------------------------------- Envoi

void MainWindow::sendCurrent()
{
    if (m_mentions->isPopupVisible())
        return; // Entrée choisit un pseudo dans la liste
    const QString text = m_input->text().trimmed();
    if (!m_editId.isEmpty()) { // modification d'un message
        if (text.isEmpty() || !m_node->editMessage(m_editId, text))
            return;
        m_editId.clear();
        m_input->clear();
        cancelReply();
        return;
    }
    // Sans personne en ligne, le message partira à la prochaine synchronisation
    if (!m_pendingFile.isEmpty()) {
        QString error;
        if (m_node->sendFile(m_pendingFile, text, m_replyTo, &error).isEmpty()) {
            QMessageBox::warning(this, tr("Envoyer un fichier"), tr("Impossible d'envoyer ce fichier : %1.").arg(error));
            return;
        }
    } else if (m_node->sendMessage(text, {m_replyTo, m_pendingImage, {}}).isEmpty()) {
        return;
    }
    m_chat->clearUnreadMarker(); // on a répondu : les messages précédents sont lus
    m_input->clear();
    cancelReply();
    cancelImage();
}

// ---------------------------------------------------------------- Réglages

void MainWindow::changeNickname()
{
    bool ok = false;
    const QString n = QInputDialog::getText(this, tr("Pseudo"), tr("Votre pseudo :"), QLineEdit::Normal,
                                            m_group->nickname(), &ok).trimmed();
    if (!ok || n.isEmpty())
        return;
    m_group->setNickname(n); // commun à toutes les conversations
    for (const auto &c : m_convs)
        c->node().announceNickname();
    m_sync->publish(); // et nos autres postes
}

void MainWindow::resetAll()
{
    const bool everywhere = hasOtherDevices() && !m_node->groupId().isEmpty();
    if (QMessageBox::question(this, tr("Quitter la conversation"),
                              tr("Quitter « %1 » ?\n"
                                 "Les autres membres en sont avertis. Il faudra une nouvelle invitation pour "
                                 "revenir. L'historique est conservé sur ce poste (⋮ → Supprimer la "
                                 "conversation pour l'effacer).").arg(m_shown->displayName())
                                  + (everywhere ? tr("\n\nVous la quitterez aussi sur vos autres postes.") : QString()))
        != QMessageBox::Yes)
        return;
    const QString groupId = m_node->groupId();
    m_shown->leave();
    m_sync->noteLeft(groupId); // sur tous nos postes
    appendSystem(everywhere ? tr("Vous avez quitté la conversation, sur tous vos postes.")
                            : tr("Vous avez quitté la conversation."));
    updateStatus();
}

// ---------------------------------------------------------------- Écriture en cours, recherche, liens

void MainWindow::onTyping()
{
    if (!m_input->text().trimmed().isEmpty())
        m_node->typing();
}

void MainWindow::openSearch()
{
    m_searchBar->setVisible(true);
    m_searchEdit->setFocus();
    m_searchEdit->selectAll();
    if (!m_searchEdit->text().isEmpty())
        m_chat->setSearchTerm(m_searchEdit->text());
}

void MainWindow::closeSearch()
{
    m_searchBar->setVisible(false);
    m_chat->setSearchTerm(QString()); // retire le surlignage
    m_input->setFocus();
}

void MainWindow::openLink(const QString &url)
{
    const QUrl u(url);
    if (!u.isValid() || (u.scheme() != "http" && u.scheme() != "https"))
        return;
    if (!m_settings.value("links/noConfirm").toBool()) {
        QMessageBox box(QMessageBox::Question, tr("Ouvrir le lien"),
                        tr("<p>Ouvrir ce lien dans votre navigateur ?</p><p><b>%1</b></p>"
                           "<p>Il s'ouvre <b>en dehors de Tor</b> : le site verra votre adresse IP, comme pour "
                           "n'importe quel lien.</p>")
                            .arg(url.toHtmlEscaped()),
                        QMessageBox::Open | QMessageBox::Cancel, this);
        auto *dontAsk = new QCheckBox(tr("Ne plus demander"), &box);
        box.setCheckBox(dontAsk);
        if (box.exec() != QMessageBox::Open)
            return;
        if (dontAsk->isChecked())
            m_settings.setValue("links/noConfirm", true);
    }
    QDesktopServices::openUrl(u);
}

