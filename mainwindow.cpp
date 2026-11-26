#include "mainwindow.h"

#include "appstyle.h"
#include "attachment.h"
#include "chatview.h"
#include "composer.h"
#include "crypto.h"
#include "invitation.h"
#include "mentioncompleter.h"
#include "mentions.h"
#include "notifications.h"
#include "notifier.h"
#include "governance.h"
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
                                c->node().retrySoon();
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

MainWindow::~MainWindow()
{
    // Les conversations (et leurs groupes) avant la fenêtre : plus aucun de leurs signaux vers nous
    for (const auto &c : m_convs)
        c->disconnect(this);
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
    c->node().setDialer([this](const QString &onion) -> QTcpSocket * {
        if (!m_tor)
            return nullptr;
        auto *s = new QTcpSocket;
        s->setProxy(QNetworkProxy(QNetworkProxy::Socks5Proxy, "127.0.0.1", m_tor->socksPort()));
        s->connectToHost(onion, TorService::OnionPort); // résolu par Tor, pas localement
        return s;
    });
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
    connect(c, &Conversation::excludedFromGroup, this, [this, c](const QString &by) {
        auto *box = new QMessageBox(QMessageBox::Information, tr("Exclu du groupe"),
                                    tr("%1, administrateur de « %2 », vous en a exclu.\n\n"
                                       "Vous ne recevez plus les messages. L'historique déjà reçu est conservé.")
                                        .arg(by, c->displayName()),
                                    QMessageBox::Ok, this);
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->open(); // sans bloquer la boucle d'événements
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
    if (!m_onion.isEmpty())
        c->node().setOnline(m_onion);
    updateConversationItem(c);
    return c;
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
    if (width() < 640 && changed && m_sidebar->isVisible())
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
    item->setIcon(ChatView::roundAvatar(c->conversationAvatar(), c->displayName(), ConvAvatarSide, devicePixelRatioF()));
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

// ---------------------------------------------------------------- Interface

void MainWindow::buildUi()
{
    setWindowTitle(tr("P2P Chat"));
    resize(820, 720);
    setStyleSheet(AppStyle::styleSheet());

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
    m_titleAvatar = new QLabel(topBar);
    m_titleAvatar->setFixedSize(TitleAvatarSide, TitleAvatarSide);
    top->addWidget(m_titleAvatar, 0, Qt::AlignTop);
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
    menuButton->setIcon(AppStyle::moreIcon());
    menuButton->setIconSize(QSize(24, 24));
    menuButton->setFixedSize(40, 40);
    menuButton->setPopupMode(QToolButton::InstantPopup);
    auto *menu = new QMenu(menuButton);
    QAction *inviteAct = menu->addAction(tr("Inviter quelqu'un…"), this, &MainWindow::invite);
    menu->addAction(tr("Nouvelle conversation…"), this, &MainWindow::startConversation);
    QAction *withAct = menu->addAction(tr("Nouvelle conversation avec…"), this, &MainWindow::chooseConversationMembers);
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
    menu->addAction(tr("Changer d'avatar…"), this, &MainWindow::changeAvatar);
    QAction *renameAct = menu->addAction(tr("Renommer la conversation…"), this, &MainWindow::renameConversation);
    QAction *convAvatarAct =
        menu->addAction(tr("Avatar de la conversation…"), this, &MainWindow::changeConversationAvatar);
    menu->addAction(tr("Vider la conversation"), this, &MainWindow::clearConversation);
    menu->addSeparator();
    QAction *leaveAct = menu->addAction(tr("Quitter la conversation…"), this, &MainWindow::resetAll);
    QAction *deleteAct = menu->addAction(tr("Supprimer la conversation…"), this, &MainWindow::deleteConversation);
    connect(menu, &QMenu::aboutToShow, this, [this, inviteAct, leaveAct, deleteAct, withAct, renameAct, convAvatarAct] {
        withAct->setEnabled(m_group->inGroup() && !m_group->members().isEmpty());
        // Réservé aux administrateurs (le nom vaut pour tous)
        renameAct->setEnabled(m_node->isAdmin() && !m_node->groupId().isEmpty());
        convAvatarAct->setEnabled(renameAct->isEnabled());
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
    m_attachButton->setIcon(AppStyle::attachIcon());
    m_attachButton->setIconSize(QSize(24, 24));
    m_attachButton->setFixedSize(48, 48);
    m_attachButton->setCursor(Qt::PointingHandCursor);
    m_attachButton->setToolTip(tr("Joindre une image ou un fichier (ou le glisser dans la fenêtre, ou le coller)"));
    connect(m_attachButton, &QToolButton::clicked, this, &MainWindow::chooseAttachment);
    m_sendButton = new QToolButton(composerBar);
    m_sendButton->setObjectName("sendButton");
    m_sendButton->setIcon(AppStyle::sendIcon());
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
    m_convList->setIconSize(QSize(ConvAvatarSide, ConvAvatarSide));
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
    if (!m_shown)
        return;
    const QList<Member> list = m_group->members();
    m_pages->setCurrentWidget(m_shown->isBlank() ? m_home : static_cast<QWidget *>(m_chat));

    // Titre : le nom de la conversation, ou les pseudos des membres
    m_title->setText(m_shown->isBlank() ? tr("P2P Chat") : m_shown->displayName());
    m_titleAvatar->setVisible(!m_shown->isBlank());
    m_titleAvatar->setPixmap(ChatView::roundAvatar(m_shown->conversationAvatar(), m_shown->displayName(),
                                                   TitleAvatarSide, devicePixelRatioF()));
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
}

void MainWindow::resetAll()
{
    if (QMessageBox::question(this, tr("Quitter la conversation"),
                              tr("Quitter « %1 » ?\n"
                                 "Les autres membres en sont avertis. Il faudra une nouvelle invitation pour "
                                 "revenir. L'historique est conservé sur ce poste (⋮ → Supprimer la "
                                 "conversation pour l'effacer).").arg(m_shown->displayName()))
        != QMessageBox::Yes)
        return;
    m_shown->leave();
    appendSystem(tr("Vous avez quitté la conversation."));
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

