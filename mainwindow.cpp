#include "mainwindow.h"

#include "attachment.h"
#include "chatview.h"
#include "composer.h"
#include "crypto.h"
#include "invitation.h"
#include "mentioncompleter.h"
#include "mentions.h"
#include "notifications.h"
#include "notifier.h"
#include "groupnode.h"
#include "protocol.h"
#include "safetynumber.h"
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
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QComboBox>
#include <QSystemTrayIcon>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileIconProvider>
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
                                c->node->retrySoon();
                        });
                });
    }
#endif

    buildUi();
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
    startTor();
}

MainWindow::Conversation *MainWindow::openConversation(const QString &id)
{
    auto owned = std::make_unique<Conversation>();
    Conversation *c = owned.get();
    m_convs.push_back(std::move(owned));
    c->id = id;
    c->store = std::make_unique<GroupStore>(m_settings, id);
    const QString historyPath = m_conversations.historyPath(id);
    QFile::setPermissions(m_conversations.dir(id), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    if (QFile::exists(historyPath))
        QFile::setPermissions(historyPath, QFile::ReadOwner | QFile::WriteOwner);

    // Le groupe : connexions, synchronisation, historique (cf. groupnode.h). Transport : Tor.
    c->node = new GroupNode(*c->store, historyPath, this);
    c->node->setDialer([this](const QString &onion) -> QTcpSocket * {
        if (!m_tor)
            return nullptr;
        auto *s = new QTcpSocket;
        s->setProxy(QNetworkProxy(QNetworkProxy::Socks5Proxy, "127.0.0.1", m_tor->socksPort()));
        s->connectToHost(onion, TorService::OnionPort); // résolu par Tor, pas localement
        return s;
    });
    m_board->add(c->node);

    c->chat = new ChatView(m_pages);
    m_pages->addWidget(c->chat);
    c->item = new QListWidgetItem(m_convList);
    c->item->setData(Qt::UserRole, id);

    connectNode(c);
    connectChat(c);
    inContext(c, [this] { setupTerminalSharing(m_ctx); });
    inContext(c, [this] {
        m_node->loadHistory();
        m_chat->scrollToBottom();
    });
    if (!m_onion.isEmpty())
        c->node->setOnline(m_onion);
    updateConversationItem(c);
    return c;
}

MainWindow::Conversation *MainWindow::newConversation()
{
    return openConversation(m_conversations.create());
}

void MainWindow::closeConversation(Conversation *c)
{
    const auto it = std::find_if(m_convs.begin(), m_convs.end(), [c](const auto &p) { return p.get() == c; });
    if (it == m_convs.end())
        return;
    if (m_shown == c)
        m_shown = nullptr;
    if (m_ctx == c)
        m_ctx = nullptr;
    const QString id = c->id;
    m_board->remove(c->node);
    delete c->sharing; // avant le groupe, qu'il utilise
    delete c->node;
    delete c->chat;
    delete c->item;
    m_convs.erase(it); // et son GroupStore
    m_conversations.remove(id);
}

MainWindow::Conversation *MainWindow::conversationById(const QString &id) const
{
    for (const auto &c : m_convs)
        if (c->id == id)
            return c.get();
    return nullptr;
}

void MainWindow::setContext(Conversation *c)
{
    m_ctx = c;
    m_group = c ? c->store.get() : nullptr;
    m_node = c ? c->node : nullptr;
    m_chat = c ? c->chat : nullptr;
    m_sharing = c ? c->sharing : nullptr;
}

void MainWindow::showConversation(Conversation *c)
{
    if (!c)
        return;
    if (m_shown && m_shown != c) {
        m_shown->draft = m_input->text();
        cancelReply();
        cancelImage();
        closeSearch();
    }
    const bool changed = m_shown != c;
    m_shown = c;
    setContext(c);
    if (changed) {
        m_input->setText(c->draft);
        m_convList->setCurrentItem(c->item);
    }
    if (c->unread && isActiveWindow()) {
        c->unread = 0;
        m_chat->revealUnreadMarker();
        updateUnread();
    }
    updateStatus();
    if (width() < 640 && changed && m_sidebar->isVisible())
        setSidebarVisible(false); // étroit : la conversation prend toute la place
}

bool MainWindow::isBlank(const Conversation *c) const
{
    return !c->store->inGroup() && c->chat->chatModel()->rowCount() == 0;
}

QString MainWindow::displayName(const Conversation *c) const
{
    if (!c->store->name().isEmpty())
        return c->store->name();
    QStringList names;
    for (const Member &m : c->store->members())
        names << (m.nick.isEmpty() ? tr("?") : m.nick);
    if (!names.isEmpty())
        return names.join(", ");
    return c->store->inGroup() ? tr("Nouvelle conversation") : isBlank(c) ? tr("Conversation vierge") : tr("Ancienne conversation");
}

void MainWindow::updateConversationItem(Conversation *c)
{
    if (!c->item)
        return;
    QString text = displayName(c);
    if (c->unread)
        text += QStringLiteral("  (%1)").arg(c->unread);
    QString second = c->preview;
    if (!c->store->inGroup() && !isBlank(c))
        second = tr("Vous avez quitté cette conversation");
    if (!second.isEmpty()) {
        second.replace('\n', ' ');
        text += '\n' + (second.size() > 42 ? second.left(41) + QStringLiteral("…") : second);
    }
    c->item->setText(text);
    QFont f = m_convList->font();
    f.setBold(c->unread > 0);
    c->item->setFont(f);
    c->item->setToolTip(displayName(c));
}

void MainWindow::startConversation()
{
    // Une conversation vierge (page d'accueil) sert d'abord
    Conversation *c = isBlank(m_shown) ? m_shown : newConversation();
    showConversation(c);
    invite();
}

void MainWindow::renameConversation()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Renommer la conversation"),
                                               tr("Nom affiché sur ce poste (vide : les pseudos des membres) :"),
                                               QLineEdit::Normal, m_group->name(), &ok);
    if (!ok)
        return;
    m_group->setName(name);
    updateConversationItem(m_shown);
    updateStatus();
}

void MainWindow::deleteConversation()
{
    if (m_group->inGroup()) {
        resetAll();
        if (m_group->inGroup())
            return;
    }
    if (!isBlank(m_shown)
        && QMessageBox::question(this, tr("Supprimer la conversation"),
                                 tr("Supprimer « %1 » de ce poste ?\n"
                                    "Son historique et ses fichiers seront effacés.").arg(displayName(m_shown)))
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
    updateUnread();
}

void MainWindow::connectNode(Conversation *c)
{
    GroupNode *n = c->node;
    // Chaque signal est traité dans le contexte de sa conversation (affichée ou non)
    auto in = [this, c](auto f) { return [this, c, f](auto &&...args) { inContext(c, [&] { f(args...); }); }; };
    connect(n, &GroupNode::messageAdded, this, in([this](const QJsonObject &m, bool mine, GroupNode::Trust t) {
        appendMessage(m, mine, t);
    }));
    connect(n, &GroupNode::messagesReceived, this, in([this](const QList<QJsonObject> &m, bool catchUp) {
        onMessagesReceived(m, catchUp);
    }));
    connect(n, &GroupNode::reactionsChanged, this,
            in([this](const QString &id) { m_chat->setReactions(id, reactionSummary(id)); }));
    connect(n, &GroupNode::memberAdded, this, in([this](const QString &, const QString &nick, bool direct) {
        appendSystem(direct ? tr("%1 a rejoint la conversation.").arg(nick)
                            : tr("%1 fait partie de la conversation.").arg(nick));
    }));
    connect(n, &GroupNode::presenceChanged, this,
            in([this](const QString &onion, const QString &nick, bool online, const QString &reason) {
                m_chat->addPresence(nick.isEmpty() ? memberNick(onion) : nick, online, reason);
            }));
    connect(n, &GroupNode::identityRejected, this, in([this](const QString &, const QString &nick) {
        appendSystem(tr("⚠ Connexion refusée : « %1 » ne présente pas la clé d'identité connue pour ce membre.")
                         .arg(nick));
    }));
    connect(n, &GroupNode::invalidMessage, this, in([this](const QString &author) {
        appendSystem(tr("⚠ Message attribué à « %1 » rejeté : signature invalide.").arg(author));
    }));
    connect(n, &GroupNode::memberExcluded, this,
            in([this](const QString &, const QString &nick, const QString &by) {
                appendSystem(tr("%1 ne fait plus partie du groupe (exclusion par %2).").arg(nick, by));
            }));
    connect(n, &GroupNode::secretRenewed, this, in([this](const QString &by) {
        appendSystem(tr("Nouveau secret du groupe (renouvelé par %1) : les anciens codes d'invitation ne "
                        "fonctionnent plus.").arg(by));
    }));
    connect(n, &GroupNode::memberLeft, this, in([this](const QString &, const QString &nick) {
        appendSystem(tr("%1 a quitté le groupe.").arg(nick));
    }));
    connect(n, &GroupNode::excludedFromGroup, this, in([this](const QString &by) {
        appendSystem(tr("⚠ %1 vous a exclu du groupe. L'historique est conservé ; il faudra une nouvelle "
                        "invitation pour revenir.").arg(by));
        updateStatus();
        auto *box = new QMessageBox(QMessageBox::Information, tr("Exclu du groupe"),
                                    tr("%1, administrateur de « %2 », vous en a exclu.\n\n"
                                       "Vous ne recevez plus les messages. L'historique déjà reçu est conservé.")
                                        .arg(by, displayName(m_ctx)),
                                    QMessageBox::Ok, this);
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->open(); // sans bloquer la boucle d'événements
    }));
    connect(n, &GroupNode::stateChanged, this, in([this] { updateStatus(); }));
    connect(n, &GroupNode::typingChanged, this, in([this] { updateStatus(); }));
    connect(n, &GroupNode::fileChanged, this, in([this](const QString &id) { updateFileEntry(id); }));
}

void MainWindow::connectChat(Conversation *c)
{
    ChatView *v = c->chat;
    auto in = [this, c](auto f) { return [this, c, f](auto &&...args) { inContext(c, [&] { f(args...); }); }; };
    connect(v, &ChatView::fileRequested, this,
            in([this](const QString &id, ChatView::FileAction a) { onFileRequested(id, a); }));
    connect(v, &ChatView::replyRequested, this, in([this](const QString &id, const QString &author, const QString &text) {
        startReply(id, author, text);
    }));
    connect(v, &ChatView::linkClicked, this, &MainWindow::openLink);
    connect(v, &ChatView::reactionToggled, this,
            in([this](const QString &id, const QString &emoji) { m_node->react(id, emoji); }));
    connect(v, &ChatView::searchResultChanged, this, [this](int current, int total) {
        m_searchCount->setText(m_searchEdit->text().trimmed().isEmpty() ? QString()
                               : total == 0                             ? tr("Aucun résultat")
                                                                        : tr("%1 / %2").arg(current).arg(total));
    });
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
        m_unread += c->unread;
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

// ---------------------------------------------------------------- Interface

namespace {

const char *StyleSheet = R"(
QMainWindow, QWidget#central, QWidget#home { background: #ffffff; }
QWidget#topBar { background: #ffffff; border-bottom: 1px solid #e1e3e1; }
QLabel#title { color: #1f1f1f; font-size: 17px; font-weight: 600; }
QLabel#subtitle { color: #5f6368; font-size: 12px; }
QToolButton#menuButton { border: none; border-radius: 20px; background: transparent; }
QToolButton#menuButton:hover { background: #e9eef6; }
QToolButton#menuButton::menu-indicator { image: none; }
QWidget#composerBar { background: #ffffff; }
QWidget#replyBar { background: #f0f4f9; border-left: 3px solid #0b57d0; border-radius: 6px; }
QLabel#replyLabel { color: #444746; font-size: 13px; }
QToolButton#replyClose { border: none; border-radius: 12px; color: #444746; font-size: 15px; }
QToolButton#replyClose:hover { background: #dde3ea; }
QPlainTextEdit#composer { background: #f0f4f9; color: #1f1f1f; border: none; border-radius: 24px;
                          padding: 10px 14px; font-size: 15px; }
QPlainTextEdit#composer:disabled { color: #9aa0a6; }
QToolButton#sendButton { background: #0b57d0; border: none; border-radius: 24px; }
QToolButton#sendButton:hover { background: #0842a0; }
QToolButton#sendButton:disabled { background: #c4c7c5; }
QToolButton#attachButton { border: none; border-radius: 24px; background: transparent; }
QToolButton#attachButton:hover { background: #e9eef6; }
QLabel#imagePreview { border-radius: 6px; }
QWidget#searchBar { background: #ffffff; border-bottom: 1px solid #e1e3e1; }
QLineEdit#searchEdit { background: #f0f4f9; border: none; border-radius: 16px; padding: 0 12px; min-height: 32px; }
QLabel#searchCount { color: #5f6368; padding: 0 6px; }
QToolButton#searchButton { border: none; border-radius: 16px; color: #444746; }
QToolButton#searchButton:hover { background: #e9eef6; }
QMenu { background: #f0f4f9; border: 1px solid #e1e3e1; border-radius: 8px; padding: 6px 0; }
QMenu::item { padding: 9px 28px 9px 20px; color: #1f1f1f; }
QMenu::item:selected { background: #dde3ea; }
QMenu::separator { height: 1px; background: #e1e3e1; margin: 4px 0; }
QPushButton#primary { background: #0b57d0; color: white; border: none; border-radius: 20px;
                      padding: 10px 28px; font-weight: 600; }
QPushButton#primary:hover { background: #0842a0; }
QPushButton#tonal { background: #d3e3fd; color: #041e49; border: none; border-radius: 20px;
                    padding: 10px 28px; font-weight: 600; }
QPushButton#tonal:hover { background: #c2d7fb; }
QLabel#homeTitle { color: #1f1f1f; font-size: 22px; }
QLabel#homeText { color: #5f6368; font-size: 14px; }
QWidget#sidebar { background: #f0f4f9; border-right: 1px solid #e1e3e1; }
QLabel#sidebarTitle { color: #1f1f1f; font-size: 15px; font-weight: 600; }
QListWidget#convList { background: transparent; border: none; outline: none; }
QListWidget#convList::item { color: #1f1f1f; padding: 8px 10px; border-radius: 12px; margin: 1px 6px; }
QListWidget#convList::item:selected { background: #d3e3fd; color: #041e49; }
QListWidget#convList::item:hover:!selected { background: #e1e5ea; }
QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }
QScrollBar::handle:vertical { background: #c4c7c5; border-radius: 4px; min-height: 30px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
)";

// Icône « joindre » : trombone (Material « attach_file »), dessiné à 45°
QIcon attachIcon()
{
    QPixmap pm(48, 48);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(24, 24);
    p.rotate(45);
    p.setPen(QPen(QColor("#444746"), 3.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    // Trois branches emboîtées : extérieure, intérieure, centrale
    QPainterPath clip;
    clip.moveTo(6, 12);
    clip.lineTo(6, -12);
    clip.arcTo(QRectF(-6, -18, 12, 12), 0, 180);
    clip.lineTo(-6, 14);
    clip.arcTo(QRectF(-9, 8, 18, 14), 180, 180);
    clip.lineTo(9, -6);
    p.drawPath(clip);
    p.drawLine(QPointF(0, -10), QPointF(0, 10));
    return QIcon(pm);
}

// Icône « envoyer » (avion en papier Material) dessinée en blanc
QIcon sendIcon()
{
    QPixmap pm(48, 48);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(2, 2);
    p.translate(1, 0);
    QPainterPath path;
    path.moveTo(2, 21);
    path.lineTo(23, 12);
    path.lineTo(2, 3);
    path.lineTo(2, 10);
    path.lineTo(17, 12);
    path.lineTo(2, 14);
    path.closeSubpath();
    p.fillPath(path, Qt::white);
    return QIcon(pm);
}

// Icône « ⋮ »
QIcon moreIcon()
{
    QPixmap pm(48, 48);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#444746"));
    for (int i = 0; i < 3; ++i)
        p.drawEllipse(QPointF(24, 12 + i * 12), 4, 4);
    return QIcon(pm);
}

} // namespace

void MainWindow::buildUi()
{
    setWindowTitle(tr("P2P Chat"));
    resize(820, 720);
    setStyleSheet(StyleSheet);

    // Colonne des conversations à gauche, conversation affichée à droite
    auto *window = new QWidget(this);
    window->setObjectName("central");
    auto *columns = new QHBoxLayout(window);
    columns->setContentsMargins(0, 0, 0, 0);
    columns->setSpacing(0);
    m_sidebar = buildSidebar(window);
    columns->addWidget(m_sidebar);
    auto *central = new QWidget(window);
    central->setObjectName("central");
    columns->addWidget(central, 1);
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Barre du haut : titre, membres en ligne, menu ⋮
    auto *topBar = new QWidget(central);
    topBar->setObjectName("topBar");
    topBar->setAttribute(Qt::WA_StyledBackground);
    auto *top = new QHBoxLayout(topBar);
    top->setContentsMargins(8, 10, 8, 10);
    m_sidebarButton = new QToolButton(topBar);
    m_sidebarButton->setObjectName("menuButton");
    m_sidebarButton->setText(QStringLiteral("☰"));
    m_sidebarButton->setFixedSize(40, 40);
    m_sidebarButton->setCursor(Qt::PointingHandCursor);
    m_sidebarButton->setToolTip(tr("Afficher ou masquer les conversations"));
    connect(m_sidebarButton, &QToolButton::clicked, this, [this] {
        m_sidebarWanted = !m_sidebar->isVisible();
        setSidebarVisible(m_sidebarWanted);
    });
    top->addWidget(m_sidebarButton, 0, Qt::AlignTop);
    top->addSpacing(4);
    auto *titles = new QVBoxLayout;
    titles->setSpacing(1);
    m_title = new QLabel(tr("P2P Chat"), topBar);
    m_title->setObjectName("title");
    m_title->setTextFormat(Qt::PlainText); // contient des pseudos choisis par les autres
    m_subtitle = new QLabel(topBar);
    m_subtitle->setObjectName("subtitle");
    m_subtitle->setTextFormat(Qt::RichText);
    m_subtitle->setWordWrap(true);
    titles->addWidget(m_title);
    titles->addWidget(m_subtitle);
    top->addLayout(titles, 1);

    auto *menuButton = new QToolButton(topBar);
    menuButton->setObjectName("menuButton");
    menuButton->setIcon(moreIcon());
    menuButton->setIconSize(QSize(24, 24));
    menuButton->setFixedSize(40, 40);
    menuButton->setPopupMode(QToolButton::InstantPopup);
    auto *menu = new QMenu(menuButton);
    QAction *inviteAct = menu->addAction(tr("Inviter quelqu'un…"), this, &MainWindow::invite);
    menu->addAction(tr("Nouvelle conversation…"), this, &MainWindow::startConversation);
    menu->addAction(tr("Rejoindre avec un code…"), this, &MainWindow::join);
    menu->addAction(tr("Se reconnecter maintenant"), this, &MainWindow::reconnectNow);
#ifdef P2P_TERMINAL
    menu->addSeparator();
    m_termShareAct = menu->addAction(tr("Partager ma console…"), this, &MainWindow::shareTerminal);
    m_termCtlAct = menu->addAction(QString(), this, [this] { m_sharing->toggleControl(); });
    m_termStopAct = menu->addAction(QString(), this, [this] { m_sharing->stop(); });
    connect(menu, &QMenu::aboutToShow, this, [this] {
        const TerminalSession *term = m_sharing->session();
        const bool host = term && term->role() == TerminalSession::Host;
        m_termShareAct->setEnabled(m_sharing->canShare() && m_node->hasConnections());
        m_termCtlAct->setVisible(host);
        if (host)
            m_termCtlAct->setText(term->control() ? tr("Retirer le contrôle à %1").arg(term->peerNick())
                                                  : tr("Donner le contrôle à %1").arg(term->peerNick()));
        m_termStopAct->setVisible(term || m_sharing->hasOutgoingOffer());
        m_termStopAct->setText(!term  ? tr("Annuler la proposition de console")
                               : host ? tr("Arrêter le partage de console")
                                      : tr("Quitter la console de %1").arg(term->peerNick()));
    });
#endif
    menu->addSeparator();
    QMenu *notifications = menu->addMenu(tr("Notifications"));
    connect(notifications, &QMenu::aboutToShow, this, [this, notifications] { buildNotificationMenu(notifications); });
    buildNotificationMenu(notifications); // le sous-menu doit avoir un contenu pour s'afficher
    menu->addAction(tr("Membres…"), this, &MainWindow::showMembers);
    menu->addAction(tr("Vérifier l'identité d'un membre…"), this, &MainWindow::verifyMember);
    menu->addAction(tr("Changer de pseudo…"), this, &MainWindow::changeNickname);
    menu->addAction(tr("Renommer la conversation…"), this, &MainWindow::renameConversation);
    menu->addAction(tr("Vider la conversation"), this, &MainWindow::clearConversation);
    menu->addSeparator();
    QAction *leaveAct = menu->addAction(tr("Quitter la conversation…"), this, &MainWindow::resetAll);
    QAction *deleteAct = menu->addAction(tr("Supprimer la conversation…"), this, &MainWindow::deleteConversation);
    connect(menu, &QMenu::aboutToShow, this, [this, inviteAct, leaveAct, deleteAct] {
        inviteAct->setText(m_group->inGroup() ? tr("Inviter quelqu'un…") : tr("Créer la conversation et inviter…"));
        leaveAct->setVisible(m_group->inGroup());
        deleteAct->setVisible(!m_group->inGroup());
    });
#ifndef Q_OS_MACOS // sous macOS : menu de l'application / Cmd+Q
    menu->addAction(tr("Quitter P2P Chat"), qApp, &QCoreApplication::quit);
#endif
    menuButton->setMenu(menu);
    top->addWidget(menuButton, 0, Qt::AlignTop);
    layout->addWidget(topBar);

    // Barre de recherche (Ctrl+F)
    m_searchBar = new QWidget(central);
    m_searchBar->setObjectName("searchBar");
    m_searchBar->setAttribute(Qt::WA_StyledBackground);
    auto *searchRow = new QHBoxLayout(m_searchBar);
    searchRow->setContentsMargins(12, 6, 8, 6);
    searchRow->setSpacing(4);
    m_searchEdit = new QLineEdit(m_searchBar);
    m_searchEdit->setObjectName("searchEdit");
    m_searchEdit->setPlaceholderText(tr("Rechercher dans la conversation"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchCount = new QLabel(m_searchBar);
    m_searchCount->setObjectName("searchCount");
    auto searchButton = [this](const QString &text, const QString &tip) {
        auto *b = new QToolButton(m_searchBar);
        b->setObjectName("searchButton");
        b->setText(text);
        b->setToolTip(tip);
        b->setFixedSize(32, 32);
        b->setCursor(Qt::PointingHandCursor);
        return b;
    };
    QToolButton *older = searchButton(QStringLiteral("▲"), tr("Résultat précédent, plus ancien (Entrée)"));
    QToolButton *newer = searchButton(QStringLiteral("▼"), tr("Résultat suivant, plus récent (Maj+Entrée)"));
    QToolButton *closeSearch = searchButton(QStringLiteral("✕"), tr("Fermer la recherche (Échap)"));
    searchRow->addWidget(m_searchEdit, 1);
    searchRow->addWidget(m_searchCount);
    searchRow->addWidget(older);
    searchRow->addWidget(newer);
    searchRow->addWidget(closeSearch);
    m_searchBar->setVisible(false);
    layout->addWidget(m_searchBar);
    connect(m_searchEdit, &QLineEdit::textChanged, this, [this](const QString &t) { m_chat->setSearchTerm(t); });
    connect(m_searchEdit, &QLineEdit::returnPressed, this, [this] {
        m_chat->searchNext(!(QGuiApplication::keyboardModifiers() & Qt::ShiftModifier));
    });
    connect(older, &QToolButton::clicked, this, [this] { m_chat->searchNext(true); });
    connect(newer, &QToolButton::clicked, this, [this] { m_chat->searchNext(false); });
    connect(closeSearch, &QToolButton::clicked, this, &MainWindow::closeSearch);
    auto *searchEscape = new QShortcut(QKeySequence(Qt::Key_Escape), m_searchEdit, nullptr, nullptr,
                                       Qt::WidgetShortcut);
    connect(searchEscape, &QShortcut::activated, this, &MainWindow::closeSearch);
    auto *find = new QShortcut(QKeySequence::Find, this);
    connect(find, &QShortcut::activated, this, &MainWindow::openSearch);

    m_pages = new QStackedWidget(central);

    // Page d'accueil (conversation vierge)
    auto *home = new QWidget(m_pages);
    m_home = home;
    home->setObjectName("home");
    auto *hl = new QVBoxLayout(home);
    hl->setContentsMargins(32, 32, 32, 32);
    hl->addStretch(2);
    auto *logo = new QLabel(home);
    logo->setPixmap(QIcon(":/icons/p2pchat.png").pixmap(112, 112));
    logo->setAlignment(Qt::AlignCenter);
    hl->addWidget(logo);
    hl->addSpacing(16);
    auto *homeTitle = new QLabel(tr("Bienvenue !"), home);
    homeTitle->setObjectName("homeTitle");
    homeTitle->setAlignment(Qt::AlignCenter);
    hl->addWidget(homeTitle);
    auto *homeText = new QLabel(tr("Créez une conversation et envoyez le code à vos proches,\n"
                                   "ou collez le code que l'on vous a envoyé."), home);
    homeText->setObjectName("homeText");
    homeText->setAlignment(Qt::AlignCenter);
    homeText->setWordWrap(true);
    hl->addWidget(homeText);
    hl->addSpacing(24);
    auto *inviteBtn = new QPushButton(tr("Créer une conversation"), home);
    inviteBtn->setObjectName("primary");
    inviteBtn->setCursor(Qt::PointingHandCursor);
    auto *joinBtn = new QPushButton(tr("J'ai reçu un code"), home);
    joinBtn->setObjectName("tonal");
    joinBtn->setCursor(Qt::PointingHandCursor);
    connect(inviteBtn, &QPushButton::clicked, this, &MainWindow::invite);
    connect(joinBtn, &QPushButton::clicked, this, &MainWindow::join);
    for (QPushButton *b : {inviteBtn, joinBtn}) {
        auto *row = new QHBoxLayout;
        row->addStretch();
        row->addWidget(b);
        row->addStretch();
        hl->addLayout(row);
        hl->addSpacing(8);
    }
    hl->addStretch(3);
    m_pages->addWidget(home); // puis une vue par conversation (openConversation)
    layout->addWidget(m_pages, 1);

    // Bandeau « Réponse à … » (réponse avec citation en cours d'écriture)
    m_replyBar = new QWidget(central);
    auto *replyRowLayout = new QHBoxLayout(m_replyBar);
    replyRowLayout->setContentsMargins(12, 6, 12, 0);
    auto *replyBox = new QWidget(m_replyBar);
    replyBox->setObjectName("replyBar");
    replyBox->setAttribute(Qt::WA_StyledBackground);
    auto *replyLayout = new QHBoxLayout(replyBox);
    replyLayout->setContentsMargins(12, 6, 6, 6);
    m_replyLabel = new QLabel(replyBox);
    m_replyLabel->setObjectName("replyLabel");
    m_replyLabel->setTextFormat(Qt::RichText);
    m_replyLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); // extrait long : pas d'élargissement
    auto *replyClose = new QToolButton(replyBox);
    replyClose->setObjectName("replyClose");
    replyClose->setText(QStringLiteral("✕"));
    replyClose->setFixedSize(24, 24);
    replyClose->setCursor(Qt::PointingHandCursor);
    replyClose->setToolTip(tr("Annuler la réponse (Échap)"));
    connect(replyClose, &QToolButton::clicked, this, &MainWindow::cancelReply);
    replyLayout->addWidget(m_replyLabel, 1);
    replyLayout->addWidget(replyClose, 0, Qt::AlignTop);
    replyRowLayout->addWidget(replyBox);
    m_replyBar->setVisible(false);
    layout->addWidget(m_replyBar);

    // Aperçu de l'image jointe au message en cours d'écriture
    m_imageBar = new QWidget(central);
    auto *imageRowLayout = new QHBoxLayout(m_imageBar);
    imageRowLayout->setContentsMargins(12, 6, 12, 0);
    auto *imageBox = new QWidget(m_imageBar);
    imageBox->setObjectName("replyBar"); // même aspect que le bandeau de réponse
    imageBox->setAttribute(Qt::WA_StyledBackground);
    auto *imageLayout = new QHBoxLayout(imageBox);
    imageLayout->setContentsMargins(8, 6, 6, 6);
    m_imagePreview = new QLabel(imageBox);
    m_imagePreview->setObjectName("imagePreview");
    m_imagePreview->setFixedSize(56, 56);
    m_imagePreview->setAlignment(Qt::AlignCenter);
    m_imageInfo = new QLabel(imageBox);
    m_imageInfo->setObjectName("replyLabel");
    auto *imageClose = new QToolButton(imageBox);
    imageClose->setObjectName("replyClose");
    imageClose->setText(QStringLiteral("✕"));
    imageClose->setFixedSize(24, 24);
    imageClose->setCursor(Qt::PointingHandCursor);
    imageClose->setToolTip(tr("Retirer l'image"));
    connect(imageClose, &QToolButton::clicked, this, &MainWindow::cancelImage);
    imageLayout->addWidget(m_imagePreview);
    imageLayout->addWidget(m_imageInfo, 1);
    imageLayout->addWidget(imageClose, 0, Qt::AlignTop);
    imageRowLayout->addWidget(imageBox);
    m_imageBar->setVisible(false);
    layout->addWidget(m_imageBar);

    // Zone de saisie
    auto *composerBar = new QWidget(central);
    composerBar->setObjectName("composerBar");
    composerBar->setAttribute(Qt::WA_StyledBackground);
    auto *row = new QHBoxLayout(composerBar);
    row->setContentsMargins(12, 8, 12, 12);
    row->setSpacing(8);
    m_input = new Composer(composerBar);
    m_input->setObjectName("composer");
    m_input->setMaxLength(4000);
    m_input->setMinimumHeight(48);
    m_input->setPlaceholderText(tr("Message"));
    m_input->setToolTip(tr("Entrée : envoyer · Maj+Entrée : aller à la ligne · `code`, ```bloc```"));
    m_mentions = new MentionCompleter(m_input);
    connect(m_input, &Composer::attachmentInserted, this, &MainWindow::attachFromMime); // collage, dépôt
    m_attachButton = new QToolButton(composerBar);
    m_attachButton->setObjectName("attachButton");
    m_attachButton->setIcon(attachIcon());
    m_attachButton->setIconSize(QSize(24, 24));
    m_attachButton->setFixedSize(48, 48);
    m_attachButton->setCursor(Qt::PointingHandCursor);
    m_attachButton->setToolTip(tr("Joindre une image ou un fichier (ou le glisser dans la fenêtre, ou le coller)"));
    connect(m_attachButton, &QToolButton::clicked, this, &MainWindow::chooseAttachment);
    m_sendButton = new QToolButton(composerBar);
    m_sendButton->setObjectName("sendButton");
    m_sendButton->setIcon(sendIcon());
    m_sendButton->setIconSize(QSize(22, 22));
    m_sendButton->setFixedSize(48, 48);
    m_sendButton->setCursor(Qt::PointingHandCursor);
    m_sendButton->setToolTip(tr("Envoyer"));
    row->addWidget(m_attachButton, 0, Qt::AlignBottom); // la zone de saisie grandit vers le haut
    row->addWidget(m_input, 1);
    row->addWidget(m_sendButton, 0, Qt::AlignBottom);
    setAcceptDrops(true);
    layout->addWidget(composerBar);
    setCentralWidget(window);

    connect(m_input, &Composer::returnPressed, this, &MainWindow::sendCurrent);
    // Échap annule la réponse en cours (la liste des mentions, si elle est ouverte, se ferme d'abord)
    auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), m_input, nullptr, nullptr, Qt::WidgetShortcut);
    connect(escape, &QShortcut::activated, this, &MainWindow::cancelReply);
    connect(m_input, &Composer::textEdited, this, &MainWindow::onTyping);
    connect(m_sendButton, &QToolButton::clicked, this, &MainWindow::sendCurrent);
}

QWidget *MainWindow::buildSidebar(QWidget *parent)
{
    auto *side = new QWidget(parent);
    side->setObjectName("sidebar");
    side->setAttribute(Qt::WA_StyledBackground);
    side->setFixedWidth(260);
    auto *lay = new QVBoxLayout(side);
    lay->setContentsMargins(0, 14, 0, 8);
    lay->setSpacing(6);
    auto *head = new QHBoxLayout;
    head->setContentsMargins(18, 0, 10, 0);
    auto *title = new QLabel(tr("Conversations"), side);
    title->setObjectName("sidebarTitle");
    head->addWidget(title, 1);
    auto *add = new QToolButton(side);
    add->setObjectName("menuButton");
    add->setText(QStringLiteral("＋"));
    add->setFixedSize(36, 36);
    add->setCursor(Qt::PointingHandCursor);
    add->setToolTip(tr("Nouvelle conversation, ou rejoindre avec un code"));
    add->setPopupMode(QToolButton::InstantPopup);
    auto *addMenu = new QMenu(add);
    addMenu->addAction(tr("Nouvelle conversation…"), this, &MainWindow::startConversation);
    addMenu->addAction(tr("Rejoindre avec un code…"), this, &MainWindow::join);
    add->setMenu(addMenu);
    head->addWidget(add);
    lay->addLayout(head);
    m_convList = new QListWidget(side);
    m_convList->setObjectName("convList");
    m_convList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_convList->setTextElideMode(Qt::ElideRight);
    lay->addWidget(m_convList, 1);
    connect(m_convList, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        showConversation(conversationById(item->data(Qt::UserRole).toString()));
    });
    return side;
}

void MainWindow::setSidebarVisible(bool visible)
{
    m_sidebar->setVisible(visible);
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    // Étroit : la colonne se replie (le bouton ☰ la rouvre par-dessus) ; large : comme l'a voulu l'utilisateur
    if (m_sidebar)
        setSidebarVisible(width() >= 640 ? m_sidebarWanted : false);
}

void MainWindow::updateStatus()
{
    if (!m_ctx)
        return;
    updateConversationItem(m_ctx);
    if (!isShown())
        return; // conversation en arrière-plan : sa ligne dans la colonne suffit
    const QList<Member> list = m_group->members();
    const bool inGroup = m_group->inGroup();
    m_pages->setCurrentWidget(isBlank(m_ctx) ? m_home : static_cast<QWidget *>(m_chat));

    // Titre : le nom de la conversation, ou les pseudos des membres
    m_title->setText(isBlank(m_ctx) ? tr("P2P Chat") : displayName(m_ctx));
    QStringList others;
    for (const Member &m : list)
        others << m.nick;
    m_mentions->setNames(others << Mentions::Everyone);

    const QString dot = QStringLiteral("<span style='color:%1'>●</span> ");
    const QHash<QString, QString> relayed = m_node->relayedOnline();
    QString sub;
    if (!m_torError.isEmpty()) {
        sub = dot.arg("#d93025") + tr("Tor : %1").arg(m_torError.toHtmlEscaped());
    } else if (m_node->ownOnion().isEmpty()) {
        sub = dot.arg("#e37400") + tr("Connexion au réseau… %1").arg(m_torStatus.toHtmlEscaped());
    } else if (!inGroup) {
        sub = isBlank(m_ctx) ? tr("Nouvelle conversation") : tr("Vous avez quitté cette conversation");
    } else if (list.isEmpty()) {
        sub = dot.arg("#e37400") + tr("En attente de la première personne invitée…");
    } else if (const QString typing = typingText(); !typing.isEmpty()) {
        sub = dot.arg("#188038") + QStringLiteral("<i>%1</i>").arg(typing.toHtmlEscaped());
    } else {
        // Les membres vus en ligne par un autre sont joignables : nos messages passent par lui
        QStringList online;
        bool allDirect = true;
        for (const Member &m : list) {
            if (m_node->isConnected(m.onion)) {
                online << m.nick.toHtmlEscaped();
            } else if (relayed.contains(m.onion)) {
                online << tr("%1 (via %2)").arg(m.nick, memberNick(relayed.value(m.onion))).toHtmlEscaped();
                allDirect = false;
            }
        }
        if (online.isEmpty())
            sub = dot.arg("#9aa0a6") + tr("Personne n'est en ligne");
        else if (online.size() == list.size() && allDirect)
            sub = dot.arg("#188038") + (list.size() == 1 ? tr("En ligne") : tr("Tout le monde est en ligne"));
        else
            sub = dot.arg("#188038") + tr("En ligne : %1").arg(online.join(", "));
    }
    m_subtitle->setText(sub);

    // Infobulle : état détaillé de chaque membre
    QStringList tips;
    for (const Member &m : list) {
        const bool connected = m_node->isConnected(m.onion), attempting = m_node->isAttempting(m.onion);
        QString state = connected                   ? tr("en ligne")
                        : relayed.contains(m.onion) ? tr("en ligne via %1").arg(memberNick(relayed.value(m.onion)))
                        : attempting                ? tr("connexion…")
                                                    : tr("hors ligne");
        if (relayed.contains(m.onion) && attempting)
            state += tr(", connexion directe…");
        if (const QStringList missing = Protocol::missingFeatures(m_node->peerVersion(m.onion));
            connected && !missing.isEmpty())
            state += tr(" — ancienne version de P2P Chat (ne voit pas %1)").arg(missing.join(", "));
        if (!connected && !m_node->lastError(m.onion).isEmpty())
            state += " — " + m_node->lastError(m.onion);
        if (!connected && !attempting && m_node->nextTry(m.onion) > QDateTime::currentMSecsSinceEpoch())
            state += tr(" (nouvel essai à %1)")
                         .arg(QDateTime::fromMSecsSinceEpoch(m_node->nextTry(m.onion)).toString("HH:mm:ss"));
        const QString name = m_group->isVerified(m.onion) ? tr("%1 ✔").arg(m.nick) : m.nick;
        tips << QStringLiteral("%1 : %2").arg(name, state).toHtmlEscaped();
    }
    m_subtitle->setToolTip(tips.isEmpty() ? QString() : "<p>" + tips.join("<br>") + "</p>");

    const bool online = m_node->hasConnections();
    const bool canWrite = m_node->canSend();
    m_input->setEnabled(canWrite);
    m_sendButton->setEnabled(canWrite);
    m_attachButton->setEnabled(canWrite);
    m_input->setPlaceholderText(online ? tr("Message") : tr("Message (transmis à la prochaine connexion)"));
    if (canWrite && !m_input->hasFocus())
        m_input->setFocus();
}

void MainWindow::appendMessage(const QJsonObject &msg, bool mine, GroupNode::Trust nodeTrust)
{
    const ChatEntry::Trust trust = nodeTrust == GroupNode::Trust::Verified   ? ChatEntry::Verified
                                   : nodeTrust == GroupNode::Trust::Unsigned ? ChatEntry::Unsigned
                                                                             : ChatEntry::Invalid;
    const qint64 ms = qint64(msg.value("ts").toDouble());
    const QDateTime ts = ms > 0 ? QDateTime::fromMSecsSinceEpoch(ms) : QDateTime::currentDateTime();
    // Image : seulement si elle est bien signée par l'auteur et décodable sans risque
    QByteArray imageData;
    QImage image;
    if (trust == ChatEntry::Verified && msg.contains("img")) {
        imageData = m_node->verifiedImage(msg, mine);
        image = Attachment::decode(imageData);
    }
    // Fichier joint : seulement si sa fiche est bien signée par l'auteur
    std::optional<Files::FileInfo> file;
    if (trust == ChatEntry::Verified && msg.contains("f"))
        file = m_node->verifiedFile(msg, mine);
    const QString text = Protocol::Message::caption(msg, !image.isNull() || file.has_value());
    QList<QPair<int, int>> spans;
    const QList<Mentions::Span> found = Mentions::find(text, mentionNames());
    for (const Mentions::Span &m : found)
        spans.append({m.start, m.length});
    const QString id = Protocol::Message::id(msg);
    ChatEntry e;
    e.author = msg.value("n").toString().left(Protocol::MaxNickLength);
    if (!mine && trust == ChatEntry::Verified) { // le ✔ ne vaut que pour un message bien signé
        e.authorOnion = msg.value("a").toString();
        e.authorVerified = m_group->isVerified(e.authorOnion);
    }
    e.text = text;
    e.ts = ts;
    e.mine = mine;
    e.trust = trust;
    e.mentions = spans;
    e.mentionsMe = !mine && (Mentions::mentions(found, m_group->nickname()) || repliesToMe(msg));
    if (trust == ChatEntry::Verified) { // identifiants et citations : seulement pour les messages authentiques
        e.id = id;
        e.replyTo = m_node->verifiedReplyTo(msg, mine);
        if (file) {
            e.file.id = file->id();
            e.file.name = file->name;
            e.file.mime = file->mime;
            e.file.size = file->size;
            e.file.risky = Files::isRisky(file->name);
            e.file.state = fileEntryState(m_node->fileState(e.file.id).kind);
            e.file.received = m_node->fileState(e.file.id).received;
            e.file.total = file->chunkCount();
        }
        if (!image.isNull()) {
            e.image = Attachment::thumbnail(image);
            e.imageData = imageData;
            e.animated = Attachment::isAnimated(imageData);
        }
    }
    e.reactions = reactionSummary(e.id); // réactions arrivées avant le message (synchronisation)
    m_chat->addMessage(e);
    m_ctx->preview = mine ? tr("Vous : %1").arg(notificationBody(msg)) : tr("%1 : %2").arg(e.author, notificationBody(msg));
    updateConversationItem(m_ctx);
}

void MainWindow::onMessagesReceived(const QList<QJsonObject> &messages, bool catchUp)
{
    int mentioned = 0;
    qint64 firstTs = 0;
    for (const QJsonObject &m : messages) {
        mentioned += (mentionsMe(m) || repliesToMe(m)) ? 1 : 0;
        const qint64 ts = Protocol::Message::timestamp(m);
        firstTs = firstTs ? qMin(firstTs, ts) : ts;
    }
    const int count = int(messages.size());
    if (count == 1 || !catchUp)
        notifyUnread(count, notificationTitle(messages.last()), notificationBody(messages.last()), firstTs,
                     mentioned > 0);
    else
        notifyUnread(count, tr("P2P Chat"),
                     tr("%n message(s) reçu(s) pendant votre absence", nullptr, count)
                         + (mentioned ? tr(", dont %n pour vous", nullptr, mentioned) : QString()),
                     firstTs, mentioned > 0);
}

void MainWindow::appendSystem(const QString &text)
{
    m_chat->addSystem(text);
}

QStringList MainWindow::mentionNames() const
{
    QStringList names{m_group->nickname(), Mentions::Everyone};
    for (const Member &m : m_group->members())
        names << m.nick;
    return names;
}

bool MainWindow::mentionsMe(const QJsonObject &msg) const
{
    return Mentions::mentions(Mentions::find(msg.value("x").toString(), mentionNames()), m_group->nickname());
}

bool MainWindow::repliesToMe(const QJsonObject &msg) const
{
    const QString quoted = m_node->verifiedReplyTo(msg, false); // appelé pour les messages des autres
    return !quoted.isEmpty() && m_node->isMine(quoted);
}

QString MainWindow::notificationTitle(const QJsonObject &msg) const
{
    const QString author = msg.value("n").toString().left(Protocol::MaxNickLength);
    if (repliesToMe(msg))
        return tr("%1 vous a répondu").arg(author);
    if (mentionsMe(msg))
        return tr("%1 vous a mentionné(e)").arg(author);
    return author;
}

void MainWindow::startReply(const QString &id, const QString &author, const QString &text)
{
    const bool mine = m_node->isMine(id);
    m_replyTo = id;
    m_replyLabel->setText(tr("Réponse à <b>%1</b><br><span style='color:#5f6368'>%2</span>")
                              .arg((mine ? tr("vous-même") : author).toHtmlEscaped(),
                                   ChatView::excerpt(text, 90).toHtmlEscaped()));
    m_replyBar->setVisible(true);
    m_input->setFocus();
}

void MainWindow::cancelReply()
{
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
        m_shown->chat->revealUnreadMarker();
        if (m_shown->unread) {
            m_shown->unread = 0;
            updateConversationItem(m_shown);
            updateUnread();
        }
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
        c->node->setOffline();
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
            c->node->setOnline(onion);
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
        c->node->setOffline();
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
        m_inviteFor = m_ctx->id;
        appendSystem(tr("Le code s'affichera dès que Tor aura démarré…"));
        updateStatus();
        return;
    }
    showInvitation();
    updateStatus();
}

void MainWindow::showInvitation()
{
    const QString code =
        Invitation{m_group->secret(), m_node->ownOnion(), m_group->nickname(), m_group->identityKey(), m_node->groupId()}.toCode();

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Code d'invitation"));
    auto *lay = new QVBoxLayout(&dlg);
    auto *intro = new QLabel(tr("Envoyez ce code <b>une seule fois</b> à chaque personne à inviter (mail, SMS…). "
                                "Elle installe P2P Chat, clique sur <b>Rejoindre</b> et colle le code : "
                                "elle sera automatiquement reliée à tous les membres.<br>"
                                "Ensuite il n'y a plus rien à faire, il suffit que les programmes soient ouverts.<br>"
                                "<i>Ce code sert aussi de mot de passe du groupe : ne le publiez pas.</i>"), &dlg);
    intro->setWordWrap(true);
    lay->addWidget(intro);

    auto *edit = new QPlainTextEdit(code, &dlg);
    edit->setReadOnly(true);
    edit->setMinimumSize(480, 100);
    lay->addWidget(edit);

    // Partage de l'historique récent avec les personnes qui rejoignent dans les 7 jours
    auto *share = new QCheckBox(tr("Partager l'historique des 30 derniers jours avec la personne invitée"), &dlg);
    share->setToolTip(tr("Utile pour ajouter un de vos propres appareils.\n"
                         "Valable pour les personnes qui rejoignent dans les 7 jours."));
    share->setChecked(m_group->shareHistoryUntil() > QDateTime::currentMSecsSinceEpoch());
    connect(share, &QCheckBox::toggled, this, [this](bool on) {
        m_group->setShareHistoryUntil(on ? QDateTime::currentDateTime().addDays(7).toMSecsSinceEpoch() : 0);
    });
    lay->addWidget(share);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    QPushButton *copy = buttons->addButton(tr("Copier le code"), QDialogButtonBox::ActionRole);
    QPushButton *mail = buttons->addButton(tr("Envoyer par mail…"), QDialogButtonBox::ActionRole);
    connect(copy, &QPushButton::clicked, &dlg, [code, copy] {
        QGuiApplication::clipboard()->setText(code);
        copy->setText(QObject::tr("Copié ✔"));
    });
    connect(mail, &QPushButton::clicked, &dlg, [this, code] {
        QUrlQuery q;
        q.addQueryItem("subject", tr("Invitation P2P Chat"));
        q.addQueryItem("body", tr("Salut !\n\nOuvre P2P Chat, clique sur « Rejoindre » et colle ce code :\n\n%1\n\n%2")
                                   .arg(code, m_group->nickname()));
        QUrl url("mailto:");
        url.setQuery(q);
        QDesktopServices::openUrl(url);
    });
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(buttons);
    dlg.exec();
}

void MainWindow::join()
{
    bool ok = false;
    const QString text = QInputDialog::getMultiLineText(this, tr("Rejoindre"),
                                                        tr("Collez le code d'invitation reçu :"), {}, &ok);
    if (!ok || text.trimmed().isEmpty())
        return;
    const auto inv = Invitation::fromCode(text);
    if (!inv) {
        QMessageBox::warning(this, tr("Rejoindre"), tr("Ce code n'est pas valide (incomplet ?)."));
        return;
    }
    if (inv->onion == m_onion) {
        QMessageBox::warning(this, tr("Rejoindre"), tr("C'est votre propre code !"));
        return;
    }
    // Conversation déjà connue (même groupe), sinon la vierge affichée, sinon une nouvelle
    Conversation *target = nullptr;
    for (const auto &c : m_convs) {
        const bool sameGroup = !inv->groupId.isEmpty() && c->node->groupId() == inv->groupId;
        if (c->store->inGroup() && (sameGroup || c->node->acceptedSecrets().contains(inv->secret)))
            target = c.get();
    }
    const bool created = !target && !isBlank(m_shown);
    if (!target)
        target = isBlank(m_shown) ? m_shown : newConversation();
    showConversation(target);
    const GroupNode::JoinResult result = m_node->join(*inv);
    if (result == GroupNode::JoinResult::Joined) {
        appendSystem(tr("Invitation de %1 enregistrée.").arg(inv->nickname));
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
                                "Par sécurité, il est refusé.</p>").arg(inv->nickname.toHtmlEscaped()));
}

// ---------------------------------------------------------------- Membres et connexions

QString MainWindow::memberNick(const QString &onion) const
{
    const QString nick = m_group->nickOf(onion);
    return nick.isEmpty() ? tr("Un membre") : nick;
}

void MainWindow::reconnectNow()
{
    if (!m_torError.isEmpty()) { // Tor en panne : on le relance, il reconnectera une fois prêt
        startTor();
        return;
    }
    for (const auto &c : m_convs)
        c->node->reconnectNow();
}

// ---------------------------------------------------------------- Notifications

void MainWindow::notifyUnread(int count, const QString &title, const QString &text, qint64 firstTs, bool forMe)
{
    if (isVisible() && isActiveWindow() && isShown())
        return;
    m_chat->markUnread(QDateTime::fromMSecsSinceEpoch(firstTs));
    m_ctx->unread += count; // compteur et pastille : toujours à jour
    updateConversationItem(m_ctx);
    updateUnread();
    m_lastNotified = m_ctx->id;
    if (!notificationsAllowed(forMe))
        return;
    // Plusieurs conversations : on précise laquelle
    m_notifier->notify(m_convs.size() > 1 ? tr("%1 · %2").arg(title, displayName(m_ctx)) : title, text);
    if (isVisible())
        QApplication::alert(this);
}

bool MainWindow::notificationsAllowed(bool forMe) const
{
    return Notifications::shouldNotify(Notifications::modeFromString(m_group->notifyMode()),
                                       m_settings.value("notify/pausedUntil").toLongLong(),
                                       QDateTime::currentMSecsSinceEpoch(), forMe);
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

QString MainWindow::notificationBody(const QJsonObject &msg) const
{
    if (msg.contains("f")) {
        const QString caption = Protocol::Message::caption(msg, true);
        const QString name = Files::sanitizeName(msg.value("f").toObject().value("n").toString());
        return caption.isEmpty() ? tr("📎 %1").arg(name) : tr("📎 %1 — %2").arg(name, caption);
    }
    if (!msg.contains("img"))
        return msg.value("x").toString();
    const QString caption = Protocol::Message::caption(msg, true);
    return caption.isEmpty() ? tr("📷 Image") : tr("📷 %1").arg(caption);
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

ChatEntry::File::State MainWindow::fileEntryState(int kind)
{
    switch (GroupNode::FileState::Kind(kind)) {
    case GroupNode::FileState::NotDownloaded:
        return ChatEntry::File::NotDownloaded;
    case GroupNode::FileState::Downloading:
        return ChatEntry::File::Downloading;
    case GroupNode::FileState::Waiting:
        return ChatEntry::File::Waiting;
    case GroupNode::FileState::Complete:
        return ChatEntry::File::Complete;
    case GroupNode::FileState::Unknown:
        break;
    }
    return ChatEntry::File::NotDownloaded;
}

void MainWindow::updateFileEntry(const QString &fileId)
{
    const GroupNode::FileState s = m_node->fileState(fileId);
    m_chat->setFileState(fileId, fileEntryState(s.kind), s.received, s.total);
}

void MainWindow::onFileRequested(const QString &fileId, ChatView::FileAction action)
{
    switch (action) {
    case ChatView::FileAction::Download:
        m_node->download(fileId);
        break;
    case ChatView::FileAction::Cancel:
        m_node->cancelDownload(fileId);
        break;
    case ChatView::FileAction::Open:
        openFile(fileId);
        break;
    case ChatView::FileAction::SaveAs:
        saveFileAs(fileId);
        break;
    }
}

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
        c->node->announceNickname();
}

void MainWindow::resetAll()
{
    if (QMessageBox::question(this, tr("Quitter la conversation"),
                              tr("Quitter « %1 » ?\n"
                                 "Les autres membres en sont avertis. Il faudra une nouvelle invitation pour "
                                 "revenir. L'historique est conservé sur ce poste (⋮ → Supprimer la "
                                 "conversation pour l'effacer).").arg(displayName(m_ctx)))
        != QMessageBox::Yes)
        return;
    m_node->leaveGroup();
    appendSystem(tr("Vous avez quitté la conversation."));
    updateStatus();
}

// ---------------------------------------------------------------- Membres et gouvernance

void MainWindow::showMembers()
{
    if (!m_group->inGroup()) {
        QMessageBox::information(this, tr("Membres"), tr("Vous ne faites partie d'aucun groupe."));
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Membres du groupe"));
    dlg.resize(420, 420);
    auto *lay = new QVBoxLayout(&dlg);
    auto *list = new QListWidget(&dlg);
    lay->addWidget(list);
    auto *info = new QLabel(&dlg);
    info->setWordWrap(true);
    lay->addWidget(info);
    auto *buttons = new QHBoxLayout;
    auto *verifyBtn = new QPushButton(tr("Vérifier l'identité…"), &dlg);
    auto *excludeBtn = new QPushButton(tr("Exclure…"), &dlg);
    auto *renewBtn = new QPushButton(tr("Renouveler le secret…"), &dlg);
    auto *closeBtn = new QPushButton(tr("Fermer"), &dlg);
    buttons->addWidget(verifyBtn);
    buttons->addWidget(excludeBtn);
    buttons->addWidget(renewBtn);
    buttons->addStretch(1);
    buttons->addWidget(closeBtn);
    lay->addLayout(buttons);
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

    auto fill = [this, list, info, renewBtn] {
        const QString selected = list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString();
        list->clear();
        auto roles = [this](const QString &onion) {
            QStringList r;
            if (m_node->isAdmin(onion))
                r << tr("administrateur");
            if (!onion.isEmpty() && m_group->isVerified(onion))
                r << tr("identité vérifiée ✔");
            return r;
        };
        QStringList mine = roles(m_node->ownOnion());
        auto *me = new QListWidgetItem(mine.isEmpty() ? tr("%1 (vous)").arg(m_group->nickname())
                                                      : tr("%1 (vous) — %2").arg(m_group->nickname(), mine.join(", ")));
        QFont bold = me->font();
        bold.setBold(true);
        me->setFont(bold);
        list->addItem(me);
        for (const Member &m : m_group->members()) {
            QStringList r = roles(m.onion);
            r.prepend(m_node->isConnected(m.onion) ? tr("en ligne") : tr("hors ligne"));
            auto *item = new QListWidgetItem(tr("%1 — %2").arg(m.nick, r.join(", ")));
            item->setData(Qt::UserRole, m.onion);
            list->addItem(item);
            if (m.onion == selected)
                list->setCurrentItem(item);
        }
        QString text;
        if (m_node->groupId().isEmpty())
            text = tr("Ce groupe n'a pas encore d'administrateur : le premier membre qui exclut quelqu'un ou "
                      "renouvelle le secret le deviendra.");
        else if (m_node->isAdmin())
            text = tr("Vous administrez ce groupe : vous pouvez exclure un membre et renouveler le secret.");
        else {
            QStringList names;
            for (const QString &a : m_node->admins())
                names << memberNick(a);
            text = tr("Administration : %1.").arg(names.join(", "));
        }
        if (const int n = int(m_node->excludedMembers().size()))
            text += " " + tr("%n membre(s) exclu(s).", nullptr, n);
        info->setText(text);
        renewBtn->setEnabled(m_node->canGovern());
    };
    auto updateButtons = [this, list, verifyBtn, excludeBtn] {
        const QString onion = list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString();
        verifyBtn->setEnabled(!onion.isEmpty() && !m_group->pinnedKey(onion).isEmpty());
        excludeBtn->setEnabled(!onion.isEmpty() && m_node->canGovern());
    };
    auto current = [list] {
        return list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString();
    };
    connect(list, &QListWidget::currentItemChanged, &dlg, updateButtons);
    connect(verifyBtn, &QPushButton::clicked, &dlg, [this, current] { showSafetyNumber(current()); });
    connect(excludeBtn, &QPushButton::clicked, &dlg, [this, current] { confirmExclude(current()); });
    connect(renewBtn, &QPushButton::clicked, &dlg, [this] { confirmRenewSecret(); });
    connect(m_node, &GroupNode::stateChanged, &dlg, [fill, updateButtons] { fill(); updateButtons(); });
    fill();
    updateButtons();
    dlg.exec();
}

void MainWindow::confirmExclude(const QString &onion)
{
    const QString nick = memberNick(onion);
    if (QMessageBox::question(this, tr("Exclure %1").arg(nick),
                              tr("Exclure %1 du groupe ?\n\n"
                                 "Un nouveau secret est créé et transmis aux seuls autres membres : %1 ne recevra "
                                 "plus les messages et ne pourra plus revenir, même avec un ancien code "
                                 "d'invitation. Il garde l'historique déjà reçu.\n\n"
                                 "Les membres absents recevront le nouveau secret à leur retour (dans les 30 jours).")
                                  .arg(nick))
        != QMessageBox::Yes)
        return;
    QString error;
    if (!m_node->exclude(onion, &error))
        QMessageBox::warning(this, tr("Exclure %1").arg(nick), tr("Impossible d'exclure %1 : %2.").arg(nick, error));
}

void MainWindow::confirmRenewSecret()
{
    if (QMessageBox::question(this, tr("Renouveler le secret"),
                              tr("Créer un nouveau secret pour le groupe ?\n\n"
                                 "Les codes d'invitation déjà donnés ne fonctionneront plus pour les personnes "
                                 "qui ne sont pas encore venues. Les membres actuels reçoivent le nouveau secret "
                                 "automatiquement (les absents, à leur retour dans les 30 jours)."))
        != QMessageBox::Yes)
        return;
    QString error;
    if (!m_node->renewSecret(&error))
        QMessageBox::warning(this, tr("Renouveler le secret"), tr("Impossible : %1.").arg(error));
}

// ---------------------------------------------------------------- Code de sécurité

void MainWindow::verifyMember()
{
    // Membres dont on connaît la clé (rencontrés au moins une fois, ou présentés par un autre)
    QList<Member> known;
    for (const Member &m : m_group->members())
        if (!m_group->pinnedKey(m.onion).isEmpty())
            known << m;
    if (known.isEmpty()) {
        QMessageBox::information(this, tr("Vérifier l'identité"),
                                 tr("Aucun membre à vérifier pour le moment : il faut s'être connecté au moins une fois."));
        return;
    }
    if (known.size() == 1) {
        showSafetyNumber(known.first().onion);
        return;
    }
    QStringList names;
    for (const Member &m : std::as_const(known))
        names << (m_group->isVerified(m.onion) ? tr("%1 ✔").arg(m.nick) : m.nick);
    bool ok = false;
    const QString choice = QInputDialog::getItem(this, tr("Vérifier l'identité"), tr("Membre :"), names, 0, false, &ok);
    if (ok)
        showSafetyNumber(known.at(int(names.indexOf(choice))).onion);
}

void MainWindow::showSafetyNumber(const QString &onion)
{
    if (m_node->ownOnion().isEmpty()) {
        QMessageBox::information(this, tr("Vérifier l'identité"),
                                 tr("Le code s'affichera une fois connecté au réseau Tor."));
        return;
    }
    const QString number =
        SafetyNumber::compute(m_node->ownOnion(), m_group->identityKey(), onion, m_group->pinnedKey(onion));
    if (number.isEmpty())
        return;
    const QString nick = memberNick(onion);
    const bool verified = m_group->isVerified(onion);

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Code de sécurité avec %1").arg(nick));
    auto *lay = new QVBoxLayout(&dlg);
    auto *intro = new QLabel(tr("<p>Comparez ce code avec celui qu'affiche <b>%1</b> sur son poste "
                                "(⋮ → <i>Vérifier l'identité d'un membre…</i>), de vive voix : au téléphone ou "
                                "en personne, pas par ce chat.</p>"
                                "<p>S'ils sont identiques, personne ne s'est interposé entre vous lors de "
                                "l'invitation : vos messages lui parviennent bien, à elle seule.</p>")
                                 .arg(nick.toHtmlEscaped()),
                             &dlg);
    intro->setWordWrap(true);
    lay->addWidget(intro);
    // Deux lignes de trois groupes, plus faciles à lire à voix haute
    auto *code = new QLabel(number.section(' ', 0, 2) + QLatin1Char('\n') + number.section(' ', 3, 5), &dlg);
    code->setAlignment(Qt::AlignCenter);
    code->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSizeF(mono.pointSizeF() * 1.8);
    mono.setLetterSpacing(QFont::AbsoluteSpacing, 2);
    code->setFont(mono);
    code->setStyleSheet("QLabel { background: #f0f4f9; border-radius: 12px; padding: 16px; color: #1f1f1f; }");
    lay->addWidget(code);
    auto *state = new QLabel(verified ? tr("<span style='color:#188038'>✔ Vous avez vérifié l'identité de %1.</span>")
                                            .arg(nick.toHtmlEscaped())
                                      : tr("<span style='color:#5f6368'>Identité pas encore vérifiée.</span>"),
                             &dlg);
    lay->addWidget(state);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    QPushButton *toggle = buttons->addButton(verified ? tr("Retirer la vérification") : tr("Les codes sont identiques"),
                                             QDialogButtonBox::ActionRole);
    connect(toggle, &QPushButton::clicked, &dlg, [this, onion, verified, &dlg] {
        m_group->setVerified(onion, !verified);
        for (const auto &c : m_convs) // le trousseau est commun à toutes les conversations
            c->chat->setAuthorVerified(onion, !verified);
        if (!verified)
            appendSystem(tr("Vous avez vérifié l'identité de %1 ✔").arg(memberNick(onion)));
        updateStatus();
        dlg.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(buttons);
    dlg.exec();
}

// ---------------------------------------------------------------- Réactions

QList<ChatEntry::Reaction> MainWindow::reactionSummary(const QString &id) const
{
    QList<ChatEntry::Reaction> out;
    if (id.isEmpty())
        return out;
    for (const Reactions::Summary &s : m_node->reactionSummary(id)) {
        QStringList names;
        for (const QString &onion : s.authors)
            names << (onion.isEmpty() || onion == m_node->ownOnion() ? tr("Vous") : memberNick(onion));
        out.append({s.emoji, int(s.authors.size()), s.mine, names.join(", ")});
    }
    return out;
}

// ---------------------------------------------------------------- Écriture en cours, recherche, liens

void MainWindow::onTyping()
{
    if (!m_input->text().trimmed().isEmpty())
        m_node->typing();
}

QString MainWindow::typingText() const
{
    QStringList names;
    for (const QString &onion : m_node->typingMembers())
        names << memberNick(onion);
    names.sort(Qt::CaseInsensitive);
    if (names.isEmpty())
        return {};
    if (names.size() == 1)
        return tr("%1 écrit…").arg(names.first());
    if (names.size() == 2)
        return tr("%1 et %2 écrivent…").arg(names.at(0), names.at(1));
    return tr("Plusieurs personnes écrivent…");
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

// ---------------------------------------------------------------- Console partagée

#ifdef P2P_TERMINAL

void MainWindow::setupTerminalSharing(Conversation *c)
{
    GroupStore *store = c->store.get();
    c->sharing = new TerminalSharing(
        *c->node,
        [store](const QString &onion) {
            const QString nick = store->nickOf(onion);
            return nick.isEmpty() ? tr("Un membre") : nick;
        },
        this);
    m_sharing = c->sharing;
    connect(c->sharing, &TerminalSharing::systemMessage, this,
            [this, c](const QString &text) { inContext(c, [&] { appendSystem(text); }); });
    connect(c->sharing, &TerminalSharing::offerReceived, this,
            [this, c](const QString &onion, const QString &nick, bool control) {
                inContext(c, [&] { askTerminalOffer(onion, nick, control); });
            });
    connect(c->sharing, &TerminalSharing::offerWithdrawn, this, [this] {
        if (m_termPrompt)
            m_termPrompt->close();
    });
}

void MainWindow::shareTerminal()
{
    // Une seule console partagée à la fois, toutes conversations confondues
    for (const auto &c : m_convs)
        if (!c->sharing->canShare()) {
            QMessageBox::information(this, tr("Partager ma console"), tr("Un partage de console est déjà en cours."));
            return;
        }
    QList<Member> online;
    for (const Member &m : m_group->members())
        if (m_node->isConnected(m.onion))
            online << m;
    if (online.isEmpty()) {
        QMessageBox::information(this, tr("Partager ma console"), tr("Personne n'est en ligne pour le moment."));
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Partager ma console"));
    auto *lay = new QVBoxLayout(&dlg);
    lay->addWidget(new QLabel(tr("Ouvrir une console sur ce poste, partagée avec :"), &dlg));
    auto *who = new QComboBox(&dlg);
    for (const Member &m : std::as_const(online))
        who->addItem(m.nick, m.onion);
    lay->addWidget(who);
    auto *control = new QCheckBox(tr("Autoriser cette personne à taper des commandes"), &dlg);
    lay->addWidget(control);
    auto *warn = new QLabel(tr("Elle verra tout ce qui s'affiche dans la console. Avec le contrôle, elle pourra "
                               "exécuter des commandes avec vos droits sur ce poste. Vous pourrez retirer le "
                               "contrôle ou arrêter à tout moment (menu ⋮, ou « exit » dans la console)."),
                            &dlg);
    warn->setWordWrap(true);
    warn->setStyleSheet("color: #5f6368;");
    lay->addWidget(warn);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Proposer"));
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(buttons);
    if (dlg.exec() != QDialog::Accepted || !m_sharing->canShare())
        return;

    const QString onion = who->currentData().toString();
    if (!m_sharing->propose(onion, control->isChecked()))
        QMessageBox::information(this, tr("Partager ma console"), tr("%1 n'est plus en ligne.").arg(memberNick(onion)));
}

void MainWindow::askTerminalOffer(const QString &, const QString &nick, bool control)
{
    auto *box = new QMessageBox(
        QMessageBox::Question, tr("Console partagée"),
        tr("<b>%1</b> vous propose de voir sa console.<br><br>%2")
            .arg(nick.toHtmlEscaped(), control ? tr("Vous pourrez y taper des commandes, exécutées sur son poste.")
                                               : tr("En lecture seule : vous verrez ce qui s'affiche, sans pouvoir taper.")),
        QMessageBox::NoButton, this);
    QPushButton *accept = box->addButton(tr("Ouvrir la console"), QMessageBox::AcceptRole);
    box->addButton(tr("Refuser"), QMessageBox::RejectRole);
    box->setAttribute(Qt::WA_DeleteOnClose);
    m_termPrompt = box;
    // Fermée aussi quand la proposition est retirée : answer() n'a alors plus d'effet
    TerminalSharing *sharing = m_sharing; // celui de la conversation qui propose
    connect(box, &QMessageBox::finished, sharing, [sharing, box, accept] { sharing->answer(box->clickedButton() == accept); });
    box->open();
    if (!isVisible() || !isActiveWindow()) {
        m_lastNotified = m_ctx->id;
        m_notifier->notify(nick, tr("vous propose de partager sa console"));
        if (isVisible())
            QApplication::alert(this);
    }
}

#else // pas de console partagée sur cette plateforme

void MainWindow::setupTerminalSharing(Conversation *c)
{
    // Toute proposition est refusée
    GroupNode *node = c->node;
    connect(node, &GroupNode::terminalMessage, this, [node](const QString &from, const QJsonObject &o) {
        if (o.value("t").toString() == "term-offer")
            node->sendTo(from, {{"t", "term-refuse"}, {"sid", o.value("sid").toString()}});
    });
}

#endif
