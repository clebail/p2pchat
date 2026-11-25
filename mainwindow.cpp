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
#include "peerconnection.h"
#include "protocol.h"
#include "safetynumber.h"
#include "torservice.h"
#ifdef P2P_TERMINAL
#  include "terminalsession.h"
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
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
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
#include <QTcpServer>
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

constexpr int MaxSeenIds = 5000;
constexpr int MaxPending = 16;             // connexions entrantes pas encore authentifiées
constexpr int PresenceGraceMs = 60 * 1000; // délai avant d'annoncer une déconnexion
constexpr qint64 TypingSendMs = 3000;    // « écrit… » : renvoyé au plus toutes les 3 s pendant la frappe
constexpr qint64 TypingShownMs = 6000;   // affiché tant que le dernier signal a moins de 6 s
constexpr qint64 FastRetryMs = 2 * 60 * 1000; // après un (re)démarrage : essais toutes les 10 s
constexpr qint64 WakeGapMs = 45 * 1000;       // tic de 10 s arrivé si tard : sortie de veille
// Au-delà du délai d'inactivité du correspondant (100 s), il nous a déjà fermé la connexion
constexpr qint64 SleepKillsLinksMs = 110 * 1000;

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);
    m_historyPath = dataDir + "/history.jsonl";

    m_group.ensureIdentity();

    // Le secret du groupe, la clé d'identité, les clés Tor et l'historique : lisibles par nous seuls
    constexpr auto OwnerRW = QFile::ReadOwner | QFile::WriteOwner;
    m_settings.sync();
    QFile::setPermissions(m_settings.fileName(), OwnerRW);
    QFile::setPermissions(dataDir, OwnerRW | QFile::ExeOwner);
    if (QFile::exists(m_historyPath))
        QFile::setPermissions(m_historyPath, OwnerRW);

    m_group.migrateLegacy();

    m_server = new QTcpServer(this);
    m_server->setProxy(QNetworkProxy::NoProxy);
    connect(m_server, &QTcpServer::newConnection, this, &MainWindow::onNewConnection);
    if (!m_server->listen(QHostAddress::LocalHost, 0))
        QMessageBox::critical(this, tr("P2P Chat"), tr("Écoute locale impossible : %1").arg(m_server->errorString()));

    // Tentative périodique vers les membres hors ligne (chacun selon son délai, cf. m_backoff)
    m_retryTimer.setInterval(10000);
    connect(&m_retryTimer, &QTimer::timeout, this, &MainWindow::connectToMembers);
    m_syncTimer.setInterval(5 * 60 * 1000);
    connect(&m_syncTimer, &QTimer::timeout, this, [this] {
        if (!m_active.isEmpty())
            broadcast(inventoryMessage());
    });
    m_syncTimer.start();

    m_presenceTimer.setSingleShot(true);
    m_presenceTimer.setInterval(1000);
    m_typingTimer.setInterval(1000);
    connect(&m_typingTimer, &QTimer::timeout, this, [this] {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (auto it = m_typing.begin(); it != m_typing.end();)
            it = it.value() <= now ? m_typing.erase(it) : std::next(it);
        if (m_typing.isEmpty())
            m_typingTimer.stop();
        updateStatus();
    });
    connect(&m_presenceTimer, &QTimer::timeout, this, [this] { broadcast(presenceMessage()); });

    // Les minuteurs sont suspendus pendant la veille : un tic très en retard signale le réveil
    m_wakeTimer.setInterval(10000);
    connect(&m_wakeTimer, &QTimer::timeout, this, [this] {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const qint64 gap = now - m_lastTick;
        m_lastTick = now;
        if (gap > WakeGapMs)
            onWake(gap);
    });
    m_lastTick = QDateTime::currentMSecsSinceEpoch();
    m_wakeTimer.start();

#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    // Changement de réseau (Wi-Fi, 4G, câble) : inutile d'attendre la fin des délais d'attente
    if (QNetworkInformation::loadBackendByFeatures(QNetworkInformation::Feature::Reachability)) {
        connect(QNetworkInformation::instance(), &QNetworkInformation::reachabilityChanged, this,
                [this](QNetworkInformation::Reachability r) {
                    qInfo().noquote() << "[réseau] joignabilité :" << int(r);
                    if (r == QNetworkInformation::Reachability::Online)
                        QTimer::singleShot(3000, this, &MainWindow::retrySoon); // laisse Tor s'en apercevoir
                });
    }
#endif

    buildUi();
    setupTray();
    m_notifier = new Notifier(this);
    m_notifier->setTray(m_tray);
    connect(m_notifier, &Notifier::activated, this, &MainWindow::showAndRaise);
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

    loadHistory();
    startTor();
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
QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }
QScrollBar::handle:vertical { background: #c4c7c5; border-radius: 4px; min-height: 30px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
)";

// Icône « joindre une image » (cadre, montagne et soleil)
QIcon imageIcon()
{
    QPixmap pm(48, 48);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor c("#444746");
    p.setPen(QPen(c, 3.5));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(7, 9, 34, 30), 5, 5);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawEllipse(QPointF(31, 18), 3.5, 3.5);
    QPainterPath m;
    m.moveTo(11, 35);
    m.lineTo(20, 23);
    m.lineTo(26, 30);
    m.lineTo(30, 26);
    m.lineTo(37, 35);
    m.closeSubpath();
    p.drawPath(m);
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
    resize(480, 720);
    setStyleSheet(StyleSheet);

    auto *central = new QWidget(this);
    central->setObjectName("central");
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Barre du haut : titre, membres en ligne, menu ⋮
    auto *topBar = new QWidget(central);
    topBar->setObjectName("topBar");
    topBar->setAttribute(Qt::WA_StyledBackground);
    auto *top = new QHBoxLayout(topBar);
    top->setContentsMargins(20, 10, 8, 10);
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
    menu->addAction(tr("Inviter quelqu'un…"), this, &MainWindow::invite);
    menu->addAction(tr("Rejoindre avec un code…"), this, &MainWindow::join);
    menu->addAction(tr("Se reconnecter maintenant"), this, &MainWindow::reconnectNow);
#ifdef P2P_TERMINAL
    menu->addSeparator();
    m_termShareAct = menu->addAction(tr("Partager ma console…"), this, &MainWindow::shareTerminal);
    m_termCtlAct = menu->addAction(QString(), this, [this] {
        if (m_term && m_term->role() == TerminalSession::Host)
            m_term->setControl(!m_term->control());
    });
    m_termStopAct = menu->addAction(QString(), this, &MainWindow::stopTerminal);
    connect(menu, &QMenu::aboutToShow, this, [this] {
        const bool host = m_term && m_term->role() == TerminalSession::Host;
        m_termShareAct->setEnabled(!m_term && m_termOffer.sid.isEmpty() && !m_active.isEmpty());
        m_termCtlAct->setVisible(host);
        if (host)
            m_termCtlAct->setText(m_term->control() ? tr("Retirer le contrôle à %1").arg(m_term->peerNick())
                                                    : tr("Donner le contrôle à %1").arg(m_term->peerNick()));
        m_termStopAct->setVisible(m_term || !m_termOffer.sid.isEmpty());
        m_termStopAct->setText(!m_term ? tr("Annuler la proposition de console")
                               : host  ? tr("Arrêter le partage de console")
                                       : tr("Quitter la console de %1").arg(m_term->peerNick()));
    });
#endif
    menu->addSeparator();
    QMenu *notifications = menu->addMenu(tr("Notifications"));
    connect(notifications, &QMenu::aboutToShow, this, [this, notifications] { buildNotificationMenu(notifications); });
    buildNotificationMenu(notifications); // le sous-menu doit avoir un contenu pour s'afficher
    menu->addAction(tr("Vérifier l'identité d'un membre…"), this, &MainWindow::verifyMember);
    menu->addAction(tr("Changer de pseudo…"), this, &MainWindow::changeNickname);
    menu->addAction(tr("Vider la conversation"), this, &MainWindow::clearConversation);
    menu->addSeparator();
    menu->addAction(tr("Quitter le groupe…"), this, &MainWindow::resetAll);
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

    // Page d'accueil (pas encore de groupe)
    auto *home = new QWidget(m_pages);
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
    m_pages->addWidget(home);

    m_chat = new ChatView(m_pages);
    m_pages->addWidget(m_chat);
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
    connect(m_input, &Composer::imageInserted, this, &MainWindow::attachFromMime); // collage, dépôt
    m_attachButton = new QToolButton(composerBar);
    m_attachButton->setObjectName("attachButton");
    m_attachButton->setIcon(imageIcon());
    m_attachButton->setIconSize(QSize(24, 24));
    m_attachButton->setFixedSize(48, 48);
    m_attachButton->setCursor(Qt::PointingHandCursor);
    m_attachButton->setToolTip(tr("Joindre une image (ou la glisser dans la fenêtre, ou la coller)"));
    connect(m_attachButton, &QToolButton::clicked, this, &MainWindow::chooseImage);
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
    setCentralWidget(central);

    connect(m_input, &Composer::returnPressed, this, &MainWindow::sendCurrent);
    // Échap annule la réponse en cours (la liste des mentions, si elle est ouverte, se ferme d'abord)
    auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), m_input, nullptr, nullptr, Qt::WidgetShortcut);
    connect(escape, &QShortcut::activated, this, &MainWindow::cancelReply);
    connect(m_chat, &ChatView::replyRequested, this, &MainWindow::startReply);
    connect(m_chat, &ChatView::linkClicked, this, &MainWindow::openLink);
    connect(m_chat, &ChatView::reactionToggled, this, &MainWindow::react);
    connect(m_chat, &ChatView::searchResultChanged, this, [this](int current, int total) {
        m_searchCount->setText(m_searchEdit->text().trimmed().isEmpty() ? QString()
                               : total == 0                             ? tr("Aucun résultat")
                                                                        : tr("%1 / %2").arg(current).arg(total));
    });
    connect(m_input, &Composer::textEdited, this, &MainWindow::onTyping);
    connect(m_sendButton, &QToolButton::clicked, this, &MainWindow::sendCurrent);
}

void MainWindow::updateStatus()
{
    const QList<Member> list = m_group.members();
    const bool inGroup = m_group.inGroup();
    m_pages->setCurrentIndex(inGroup || m_chat->chatModel()->rowCount() > 0 ? 1 : 0);

    // Titre : les pseudos des membres
    QStringList names;
    for (const Member &m : list)
        names << (m.nick.isEmpty() ? tr("?") : m.nick);
    m_title->setText(names.isEmpty() ? tr("P2P Chat") : names.join(", "));
    QStringList others;
    for (const Member &m : list)
        others << m.nick;
    m_mentions->setNames(others << Mentions::Everyone);

    const QString dot = QStringLiteral("<span style='color:%1'>●</span> ");
    const QHash<QString, QString> relayed = relayedOnline();
    QString sub;
    if (!m_torError.isEmpty()) {
        sub = dot.arg("#d93025") + tr("Tor : %1").arg(m_torError.toHtmlEscaped());
    } else if (m_myOnion.isEmpty()) {
        sub = dot.arg("#e37400") + tr("Connexion au réseau… %1").arg(m_torStatus.toHtmlEscaped());
    } else if (!inGroup) {
        sub = tr("Aucune conversation");
    } else if (list.isEmpty()) {
        sub = dot.arg("#e37400") + tr("En attente de la première personne invitée…");
    } else if (const QString typing = typingText(); !typing.isEmpty()) {
        sub = dot.arg("#188038") + QStringLiteral("<i>%1</i>").arg(typing.toHtmlEscaped());
    } else {
        // Les membres vus en ligne par un autre sont joignables : nos messages passent par lui
        QStringList online;
        bool allDirect = true;
        for (const Member &m : list) {
            if (m_active.contains(m.onion)) {
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
        QString state = m_active.contains(m.onion)     ? tr("en ligne")
                        : relayed.contains(m.onion)    ? tr("en ligne via %1").arg(memberNick(relayed.value(m.onion)))
                        : m_attempts.contains(m.onion) ? tr("connexion…")
                                                       : tr("hors ligne");
        if (relayed.contains(m.onion) && m_attempts.contains(m.onion))
            state += tr(", connexion directe…");
        if (const PeerConnection *c = m_active.value(m.onion); c && c->peerVersion() < Protocol::Version)
            state += tr(" — ancienne version de P2P Chat (ne voit pas %1)")
                         .arg(Protocol::missingFeatures(c->peerVersion()).join(", "));
        if (!m_active.contains(m.onion) && m_lastError.contains(m.onion))
            state += " — " + m_lastError.value(m.onion);
        if (!m_active.contains(m.onion) && !m_attempts.contains(m.onion)
            && m_backoff.value(m.onion).nextTry > QDateTime::currentMSecsSinceEpoch())
            state += tr(" (nouvel essai à %1)")
                         .arg(QDateTime::fromMSecsSinceEpoch(m_backoff.value(m.onion).nextTry).toString("HH:mm:ss"));
        const QString name = m_group.isVerified(m.onion) ? tr("%1 ✔").arg(m.nick) : m.nick;
        tips << QStringLiteral("%1 : %2").arg(name, state).toHtmlEscaped();
    }
    m_subtitle->setToolTip(tips.isEmpty() ? QString() : "<p>" + tips.join("<br>") + "</p>");

    const bool online = !m_active.isEmpty();
    const bool canWrite = inGroup && !list.isEmpty() && !m_myOnion.isEmpty();
    m_input->setEnabled(canWrite);
    m_sendButton->setEnabled(canWrite);
    m_attachButton->setEnabled(canWrite);
    m_input->setPlaceholderText(online ? tr("Message") : tr("Message (transmis à la prochaine connexion)"));
    if (canWrite && !m_input->hasFocus())
        m_input->setFocus();
}

void MainWindow::appendMessage(const QJsonObject &msg, bool mine, ChatEntry::Trust trust, bool save)
{
    const qint64 ms = qint64(msg.value("ts").toDouble());
    const QDateTime ts = ms > 0 ? QDateTime::fromMSecsSinceEpoch(ms) : QDateTime::currentDateTime();
    // Image : seulement si elle est bien signée par l'auteur et décodable sans risque
    QByteArray imageData;
    QImage image;
    if (trust == ChatEntry::Verified && msg.contains("img")) {
        const QByteArray key = mine ? m_group.identityKey() : m_group.pinnedKey(msg.value("a").toString());
        imageData = Protocol::Message::verifiedImage(msg, key);
        image = Attachment::decode(imageData);
    }
    const QString text = Protocol::Message::caption(msg, !image.isNull());
    QList<QPair<int, int>> spans;
    const QList<Mentions::Span> found = Mentions::find(text, mentionNames());
    for (const Mentions::Span &m : found)
        spans.append({m.start, m.length});
    const QString id = Protocol::Message::id(msg);
    ChatEntry e;
    e.author = msg.value("n").toString().left(Protocol::MaxNickLength);
    if (!mine && trust == ChatEntry::Verified) { // le ✔ ne vaut que pour un message bien signé
        e.authorOnion = msg.value("a").toString();
        e.authorVerified = m_group.isVerified(e.authorOnion);
    }
    e.text = text;
    e.ts = ts;
    e.mine = mine;
    e.trust = trust;
    e.mentions = spans;
    e.mentionsMe = !mine && (Mentions::mentions(found, m_group.nickname()) || repliesToMe(msg));
    if (trust == ChatEntry::Verified) { // identifiants et citations : seulement pour les messages authentiques
        e.id = id;
        e.replyTo = replyTo(msg, mine);
        if (!image.isNull()) {
            e.image = Attachment::thumbnail(image);
            e.imageData = imageData;
            e.animated = Attachment::isAnimated(imageData);
        }
    }
    e.reactions = reactionSummary(e.id); // réactions arrivées avant le message (synchronisation)
    m_chat->addMessage(e);
    if (!id.isEmpty() && trust == ChatEntry::Verified) { // seuls les messages authentiques sont resynchronisés
        m_store.insert(id, History::stored(msg, mine));
        m_seenIds.insert(id);
    }
    if (save)
        appendToHistoryFile(msg, mine);
}

void MainWindow::appendToHistoryFile(const QJsonObject &record, bool mine)
{
    // Enregistré tel qu'il a été signé : l'historique reste vérifiable
    QFile file(m_historyPath);
    const bool created = !file.exists();
    if (file.open(QIODevice::Append)) {
        if (created)
            file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        file.write(History::toLine(record, mine));
    }
}

void MainWindow::appendSystem(const QString &text)
{
    m_chat->addSystem(text);
}

QStringList MainWindow::mentionNames() const
{
    QStringList names{m_group.nickname(), Mentions::Everyone};
    for (const Member &m : m_group.members())
        names << m.nick;
    return names;
}

bool MainWindow::mentionsMe(const QJsonObject &msg) const
{
    return Mentions::mentions(Mentions::find(msg.value("x").toString(), mentionNames()), m_group.nickname());
}

QString MainWindow::replyTo(const QJsonObject &msg, bool mine) const
{
    // Comme verifyMessage : au chargement de l'historique, notre adresse onion n'est pas encore connue
    const QByteArray key = mine ? m_group.identityKey() : m_group.pinnedKey(msg.value("a").toString());
    return Protocol::Message::verifiedReplyTo(msg, key);
}

bool MainWindow::repliesToMe(const QJsonObject &msg) const
{
    const QString quoted = replyTo(msg, false); // appelé pour les messages des autres
    return !quoted.isEmpty() && m_store.value(quoted).value("mine").toBool();
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
    const bool mine = m_store.value(id).value("mine").toBool();
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

void MainWindow::loadHistory()
{
    QFile file(m_historyPath);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QList<History::Row> rows = History::parse(file.readAll(), m_group.clearedBefore());
    const qsizetype firstShown = qMax<qsizetype>(0, rows.size() - History::DisplayedRows);
    for (qsizetype i = 0; i < rows.size(); ++i) {
        const History::Row &row = rows[i];
        if (Reactions::isReaction(row.msg)) {
            if (verifyReaction(row.msg, row.mine))
                acceptReaction(row.msg, row.mine, false);
            continue;
        }
        const ChatEntry::Trust trust = row.legacy ? ChatEntry::Unsigned : verifyMessage(row.msg, row.mine);
        if (i >= firstShown) {
            appendMessage(row.msg, row.mine, trust, false);
        } else if (trust == ChatEntry::Verified && !Protocol::Message::id(row.msg).isEmpty()) {
            // Les plus anciens (non affichés) restent disponibles pour la synchronisation
            m_store.insert(Protocol::Message::id(row.msg), History::stored(row.msg, row.mine));
        }
    }
    m_chat->scrollToBottom();
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
    QFile::remove(m_historyPath);
    m_store.clear();
    m_reactions.clear();
    m_group.setClearedBefore(QDateTime::currentMSecsSinceEpoch());
    updateStatus();
}

void MainWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::ActivationChange && isActiveWindow()) {
        m_chat->revealUnreadMarker();
        if (m_unread) {
            m_unread = 0;
            updateUnread();
        }
    }
    QMainWindow::changeEvent(event);
}

// ---------------------------------------------------------------- Tor

void MainWindow::startTor()
{
    delete m_tor;
    m_myOnion.clear();
    m_torError.clear();
    m_torStatus.clear();

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/tor";
    m_tor = new TorService(dir, m_server->serverPort(), this);
    connect(m_tor, &TorService::progress, this, [this](int pct, const QString &summary) {
        m_torStatus = QStringLiteral("%1 % (%2)").arg(pct).arg(summary);
        updateStatus();
    });
    connect(m_tor, &TorService::ready, this, [this](const QString &onion) {
        m_myOnion = onion;
        if (m_inviteWanted) {
            m_inviteWanted = false;
            showInvitation();
        }
        // Juste après le démarrage, Tor peut encore compléter ses informations d'annuaire
        m_backoff.clear();
        m_fastRetryUntil = QDateTime::currentMSecsSinceEpoch() + FastRetryMs;
        QTimer::singleShot(5000, this, &MainWindow::connectToMembers);
        m_retryTimer.start();
        updateStatus();
    });
    connect(m_tor, &TorService::failed, this, &MainWindow::onTorFailed);
    m_tor->start();
    updateStatus();
}

void MainWindow::onTorFailed(const QString &error)
{
    m_myOnion.clear();
    m_torError = error;
    m_retryTimer.stop();
    closeAll();
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
    if (!m_group.inGroup())
        m_group.setSecret(Crypto::randomBytes(16));
    if (m_myOnion.isEmpty()) {
        m_inviteWanted = true;
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
        Invitation{m_group.secret(), m_myOnion, m_group.nickname(), m_group.identityKey()}.toCode();

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
    share->setChecked(m_group.shareHistoryUntil() > QDateTime::currentMSecsSinceEpoch());
    connect(share, &QCheckBox::toggled, this, [this](bool on) {
        m_group.setShareHistoryUntil(on ? QDateTime::currentDateTime().addDays(7).toMSecsSinceEpoch() : 0);
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
                                   .arg(code, m_group.nickname()));
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
    if (inv->onion == m_myOnion) {
        QMessageBox::warning(this, tr("Rejoindre"), tr("C'est votre propre code !"));
        return;
    }
    if (m_group.inGroup() && m_group.secret() != inv->secret) {
        if (QMessageBox::question(this, tr("Rejoindre"),
                                  tr("<p>Vous faites déjà partie d'une autre conversation. "
                                     "La quitter pour rejoindre celle de %1 ?</p>").arg(inv->nickname.toHtmlEscaped()))
            != QMessageBox::Yes)
            return;
        closeAll();
        m_group.setMembers({});
    }
    if (!pinKey(inv->onion, inv->identityKey)) {
        QMessageBox::warning(this, tr("Rejoindre"),
                             tr("<p>Ce code présente une clé d'identité différente de celle déjà connue pour %1.<br>"
                                "Par sécurité, il est refusé.</p>").arg(inv->nickname.toHtmlEscaped()));
        return;
    }
    m_group.setSecret(inv->secret);
    mergeMember(inv->onion, inv->nickname);
    appendSystem(tr("Invitation de %1 enregistrée.").arg(inv->nickname));
    connectToMembers();
    updateStatus();
}

// ---------------------------------------------------------------- Membres

bool MainWindow::mergeMember(const QString &onion, const QString &nick)
{
    return m_group.mergeMember(onion, nick, m_myOnion);
}

bool MainWindow::pinKey(const QString &onion, const QByteArray &key)
{
    return m_group.pinKey(onion, key, QDateTime::currentMSecsSinceEpoch());
}

QString MainWindow::memberNick(const QString &onion) const
{
    const QString nick = m_group.nickOf(onion);
    return nick.isEmpty() ? tr("Un membre") : nick;
}

QJsonObject MainWindow::membersMessage() const
{
    using Protocol::toB64;
    QJsonArray arr;
    arr.append(QJsonObject{{"o", m_myOnion}, {"n", m_group.nickname()}, {"k", toB64(m_group.identityKey())}});
    for (const Member &m : m_group.members())
        arr.append(QJsonObject{{"o", m.onion}, {"n", m.nick}, {"k", toB64(m_group.pinnedKey(m.onion))}});
    return {{"t", "members"}, {"m", arr}};
}

ChatEntry::Trust MainWindow::verifyMessage(const QJsonObject &msg, bool mine) const
{
    if (!msg.contains("s"))
        return ChatEntry::Unsigned;
    const QByteArray key = mine ? m_group.identityKey() : m_group.pinnedKey(msg.value("a").toString());
    return Protocol::Message::verify(msg, key) ? ChatEntry::Verified : ChatEntry::Invalid;
}

// ---------------------------------------------------------------- Connexions

PeerConnection *MainWindow::newConnection(QTcpSocket *socket, bool outgoing)
{
    auto *c = new PeerConnection(socket, outgoing, m_group.secret(), m_group.nickname(), m_myOnion,
                                 m_group.identitySeed(), this);
    connect(c, &PeerConnection::ready, this, [this, c] { setActive(c); });
    connect(c, &PeerConnection::received, this, [this, c](const QJsonObject &o) { onReceived(c, o); });
    connect(c, &PeerConnection::closed, this, [this, c](const QString &reason) { onClosed(c, reason); });
    return c;
}

void MainWindow::onNewConnection()
{
    while (QTcpSocket *s = m_server->nextPendingConnection()) {
        if (!m_group.inGroup() || m_myOnion.isEmpty() || m_pending.size() >= MaxPending) {
            s->abort();
            s->deleteLater();
            continue;
        }
        m_pending.insert(newConnection(s, false));
    }
}

void MainWindow::connectToMembers()
{
    if (m_myOnion.isEmpty() || !m_group.inGroup())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const Member &m : m_group.members())
        if (!m_active.contains(m.onion) && !m_attempts.contains(m.onion) && m_backoff.value(m.onion).due(now))
            dial(m.onion);
    updateStatus();
}

void MainWindow::reconnectNow()
{
    if (!m_torError.isEmpty()) { // Tor en panne : on le relance, il reconnectera une fois prêt
        startTor();
        return;
    }
    // Les tentatives en cours peuvent rester bloquées jusqu'au délai de poignée de main (45 s)
    const QList<PeerConnection *> stuck = m_attempts.values();
    for (PeerConnection *c : stuck)
        c->close(); // retire la tentative de m_attempts (signal closed synchrone)
    m_backoff.clear();
    m_retryTimer.start(); // repart pour un cycle complet
    connectToMembers();
}

void MainWindow::dial(const QString &onion)
{
    auto *s = new QTcpSocket;
    s->setProxy(QNetworkProxy(QNetworkProxy::Socks5Proxy, "127.0.0.1", m_tor->socksPort()));
    m_attempts.insert(onion, newConnection(s, true));
    s->connectToHost(onion, TorService::OnionPort); // résolu par Tor, pas localement
}

void MainWindow::onClosed(PeerConnection *c, const QString &reason)
{
    c->deleteLater();
    m_pending.remove(c);
    const QString attemptKey = m_attempts.key(c);
    if (!attemptKey.isEmpty()) {
        m_attempts.remove(attemptKey);
        m_lastError.insert(attemptKey, reason);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        m_backoff[attemptKey].failed(now, now < m_fastRetryUntil);
    }
    const QString activeKey = m_active.key(c);
    if (!activeKey.isEmpty()) {
        m_active.remove(activeKey);
        m_lastError.insert(activeKey, reason);
        m_backoff.remove(activeKey); // il était là il y a un instant : on le rappelle tout de suite
        terminalPeerLost(activeKey);
        m_peerPresence.remove(activeKey); // ceux qu'il voyait ne sont plus joignables par lui
        m_typing.remove(activeKey);
        m_presenceTimer.start();
        checkQuietDrops();
        announceDropLater(activeKey, reason);
        QTimer::singleShot(5000, this, &MainWindow::connectToMembers);
    }
    updateStatus();
}

void MainWindow::announceDropLater(const QString &onion, const QString &reason)
{
    // La déconnexion n'est annoncée que si le membre ne revient pas dans le délai de grâce
    const quint64 token = ++m_dropToken;
    m_dropped.insert(onion, {reason, token});
    QTimer::singleShot(PresenceGraceMs, this, [this, onion, token] {
        const auto it = m_dropped.constFind(onion);
        if (it == m_dropped.cend() || it->token != token)
            return;
        const QString why = it->reason;
        m_dropped.erase(it);
        if (m_active.contains(onion))
            return;
        // Toujours joignable par un autre membre : annoncé seulement quand il ne l'est plus
        if (relayedOnline().contains(onion))
            m_quietDrops.insert(onion, why);
        else
            m_chat->addPresence(memberNick(onion), false, why);
    });
}

void MainWindow::setActive(PeerConnection *c)
{
    m_pending.remove(c);
    const QString dialed = m_attempts.key(c);
    if (!dialed.isEmpty())
        m_attempts.remove(dialed);

    const QString onion = c->peerOnion();
    if (!isValidOnion(onion) || onion == m_myOnion) {
        c->close();
        return;
    }
    // En sortie, Tor garantit qu'on a joint l'adresse appelée : le pair doit annoncer celle-là
    if (!dialed.isEmpty() && dialed != onion) {
        c->close();
        return;
    }
    // La signature du hello prouve la possession de la clé ; elle doit être celle connue pour ce membre
    if (!pinKey(onion, c->peerIdentityKey())) {
        if (!m_identityWarned.contains(onion)) {
            m_identityWarned.insert(onion);
            appendSystem(tr("⚠ Connexion refusée : « %1 » ne présente pas la clé d'identité connue pour ce membre.")
                             .arg(c->peerNickname()));
        }
        c->close();
        return;
    }

    // Revenu pendant le délai de grâce : la coupure passe inaperçue
    const bool graceReturn = m_dropped.remove(onion) > 0;
    const bool quietReturn = m_quietDrops.remove(onion) > 0;
    const bool wasOnline = m_active.contains(onion) || graceReturn || quietReturn;
    if (PeerConnection *old = m_active.value(onion); old && old != c) {
        // Appels croisés : les deux côtés gardent la même connexion
        auto initiator = [this](PeerConnection *p) { return p->isOutgoing() ? m_myOnion : p->peerOnion(); };
        if (ConnectionPolicy::keepExisting(initiator(old), initiator(c))) {
            c->close();
            return;
        }
        m_active.insert(onion, c); // avant close() : le signal closed de l'ancienne ne compte pas
        old->close();
    }
    m_active.insert(onion, c);
    m_lastError.remove(onion);
    m_backoff.remove(onion);

    const bool isNew = mergeMember(onion, c->peerNickname());
    if (isNew)
        appendSystem(tr("%1 a rejoint la conversation.").arg(c->peerNickname()));
    else if (!wasOnline)
        m_chat->addPresence(c->peerNickname(), true);

    // Échange des listes de membres : chacun découvre ceux qu'il ne connaît pas encore
    c->send(membersMessage());
    if (isNew)
        broadcast(membersMessage(), c);
    c->send(inventoryMessage()); // après la liste des membres : les clés des auteurs sont connues
#ifdef P2P_TERMINAL
    if (m_term && m_term->peerOnion() == onion)
        m_term->setLinkUp(); // reprise de la console partagée
#endif
    c->send(presenceMessage());
    if (!wasOnline) {
        m_presenceTimer.start(); // les autres apprennent qu'on le voit
        // Une connexion vient d'aboutir : Tor et le réseau fonctionnent, inutile de faire attendre les autres
        if (!m_backoff.isEmpty()) {
            m_backoff.clear();
            connectToMembers();
            return; // connectToMembers met l'état à jour
        }
    }
    updateStatus();
}

// ---------------------------------------------------------------- Synchronisation

qint64 MainWindow::syncCutoff() const
{
    return History::syncCutoff(QDateTime::currentMSecsSinceEpoch(), m_group.clearedBefore());
}

QJsonObject MainWindow::inventoryMessage() const
{
    return {{"t", "inv"}, {"ids", History::inventory(m_store, syncCutoff())}};
}

void MainWindow::sendBatches(PeerConnection *c, const QList<QJsonObject> &messages)
{
    const qint64 cutoff = History::cutoffFor(syncCutoff(), m_group.firstSeen(c->peerOnion()));
    QList<QJsonObject> toSend = messages;
    if (c->peerVersion() < Reactions::Version) // il ne saurait pas quoi en faire
        toSend.erase(std::remove_if(toSend.begin(), toSend.end(), &Reactions::isReaction), toSend.end());
    for (const QJsonArray &batch : History::batches(toSend, cutoff))
        c->send({{"t", "sync"}, {"m", batch}});
}

void MainWindow::receiveSync(PeerConnection *from, const QJsonArray &messages)
{
    QList<QJsonObject> accepted;
    const qint64 cutoff = syncCutoff();
    int received = 0, mentioned = 0;
    qint64 firstTs = 0;
    QString lastTitle, lastText;
    for (const QJsonValue &v : messages) {
        const QJsonObject m = v.toObject();
        const QString id = Protocol::Message::id(m);
        if (id.isEmpty() || m_store.contains(id) || Protocol::Message::timestamp(m) < cutoff)
            continue;
        const bool mine = !m_myOnion.isEmpty() && m.value("a").toString() == m_myOnion;
        if (Reactions::isReaction(m)) {
            if (verifyReaction(m, mine) && acceptReaction(m, mine, true))
                accepted.append(m);
            continue;
        }
        // Chaque message est vérifié avec la clé de son auteur : un membre ne peut rien inventer
        if (verifyMessage(m, mine) != ChatEntry::Verified) {
            qWarning().noquote() << "[synchro] message rejeté, signature invalide :" << id;
            continue;
        }
        appendMessage(m, mine, ChatEntry::Verified);
        accepted.append(m);
        if (!mine) {
            ++received;
            mentioned += (mentionsMe(m) || repliesToMe(m)) ? 1 : 0;
            const qint64 ts = Protocol::Message::timestamp(m);
            firstTs = firstTs ? qMin(firstTs, ts) : ts;
            lastTitle = notificationTitle(m);
            lastText = notificationBody(m);
        }
    }
    // Relais aux autres membres connectés, qui ont pu manquer ces messages eux aussi
    if (!accepted.isEmpty())
        for (PeerConnection *p : std::as_const(m_active))
            if (p != from)
                sendBatches(p, accepted);
    if (received == 1)
        notifyUnread(1, lastTitle, lastText, firstTs, mentioned > 0);
    else if (received > 1)
        notifyUnread(received, tr("P2P Chat"),
                     tr("%n message(s) reçu(s) pendant votre absence", nullptr, received)
                         + (mentioned ? tr(", dont %n pour vous", nullptr, mentioned) : QString()),
                     firstTs, mentioned > 0);
}

void MainWindow::notifyUnread(int count, const QString &title, const QString &text, qint64 firstTs, bool forMe)
{
    if (isVisible() && isActiveWindow())
        return;
    m_chat->markUnread(QDateTime::fromMSecsSinceEpoch(firstTs));
    m_unread += count; // compteur et pastille : toujours à jour
    updateUnread();
    if (!notificationsAllowed(forMe))
        return;
    m_notifier->notify(title, text);
    if (isVisible())
        QApplication::alert(this);
}

bool MainWindow::notificationsAllowed(bool forMe) const
{
    return Notifications::shouldNotify(Notifications::modeFromString(m_settings.value("notify/mode").toString()),
                                       m_settings.value("notify/pausedUntil").toLongLong(),
                                       QDateTime::currentMSecsSinceEpoch(), forMe);
}

void MainWindow::buildNotificationMenu(QMenu *menu)
{
    using namespace Notifications;
    // Reconstruit à chaque ouverture : la pause affichée dépend de l'heure
    menu->clear();
    qDeleteAll(menu->findChildren<QActionGroup *>()); // groupes de l'ouverture précédente
    const Mode mode = modeFromString(m_settings.value("notify/mode").toString());
    auto *group = new QActionGroup(menu);
    const QList<QPair<Mode, QString>> modes{{All, tr("Tous les messages")},
                                           {ForMe, tr("Mentions et réponses seulement")},
                                           {Silent, tr("Aucune")}};
    for (const auto &m : modes) {
        QAction *a = menu->addAction(m.second, this, [this, value = m.first] {
            m_settings.setValue("notify/mode", modeToString(value));
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

// ---------------------------------------------------------------- Messages reçus

void MainWindow::onReceived(PeerConnection *c, const QJsonObject &o)
{
    const QString type = o.value("t").toString();
    if (type.startsWith(QLatin1String("term-")))
        onTerminalMessage(c, o);
    else if (type == "members")
        onMembers(c, o);
    else if (type == "presence")
        onPresence(c, o);
    else if (type == "inv")
        sendBatches(c, History::missing(m_store, o.value("ids").toArray()));
    else if (type == "sync")
        receiveSync(c, o.value("m").toArray());
    else if (type == "react") {
        const QString id = Protocol::Message::id(o);
        if (id.isEmpty() || m_store.contains(id) || !verifyReaction(o, false))
            return;
        acceptReaction(o, false, true);
        broadcastReaction(o, c); // relais, comme les messages
    } else if (type == "typing")
        onPeerTyping(c);
    else if (type == "msg")
        onChatMessage(c, o);
}

void MainWindow::onMembers(PeerConnection *c, const QJsonObject &o)
{
    bool changed = false;
    for (const QJsonValue &v : o.value("m").toArray()) {
        const QJsonObject m = v.toObject();
        const QString onion = m["o"].toString();
        if (onion == m_myOnion)
            continue;
        // Un membre présenté sans clé, ou avec une clé différente de celle connue, est ignoré
        if (!pinKey(onion, Protocol::fromB64(m["k"]))) {
            qWarning().noquote() << "[identité] clé refusée pour" << onion.left(8);
            continue;
        }
        if (mergeMember(onion, m["n"].toString())) {
            changed = true;
            appendSystem(tr("%1 fait partie de la conversation.").arg(m["n"].toString()));
        }
    }
    if (changed) {
        broadcast(membersMessage(), c);
        connectToMembers();
    }
    updateStatus(); // un pseudo a pu changer
}

void MainWindow::onPresence(PeerConnection *c, const QJsonObject &o)
{
    const QString from = m_active.key(c);
    if (from.isEmpty())
        return; // connexion remplacée entre-temps
    QSet<QString> seen;
    for (const QJsonValue &v : o.value("on").toArray()) {
        const QString onion = v.toString();
        if (onion != from && m_group.isMember(onion))
            seen.insert(onion);
    }
    const QSet<QString> before = m_peerPresence.value(from);
    m_peerPresence.insert(from, seen);
    // Un membre vient d'apparaître chez un autre : il est en ligne, on l'appelle sans attendre
    bool redial = false;
    for (const QString &onion : std::as_const(seen))
        if (!before.contains(onion) && !m_active.contains(onion) && !m_attempts.contains(onion)) {
            m_backoff.remove(onion);
            redial = true;
        }
    checkQuietDrops();
    if (redial)
        connectToMembers();
    else
        updateStatus();
}

void MainWindow::onChatMessage(PeerConnection *c, const QJsonObject &o)
{
    const QString id = Protocol::Message::id(o);
    if (id.isEmpty() || m_seenIds.contains(id) || m_store.contains(id))
        return;
    if (m_seenIds.size() > MaxSeenIds)
        m_seenIds.clear();
    m_seenIds.insert(id);

    const QString author = o.value("n").toString().left(Protocol::MaxNickLength);
    if (verifyMessage(o, false) != ChatEntry::Verified) {
        appendSystem(tr("⚠ Message attribué à « %1 » rejeté : signature invalide.").arg(author));
        return;
    }
    appendMessage(o, false, ChatEntry::Verified);

    // L'auteur vient d'écrire (message relayé) : il est en ligne, inutile d'attendre pour le rappeler
    const QString authorOnion = o.value("a").toString();
    if (m_typing.remove(authorOnion) > 0) // il a fini d'écrire
        updateStatus();
    if (!m_active.contains(authorOnion) && m_backoff.remove(authorOnion) > 0)
        connectToMembers();

    // Relais : au cas où un membre ne serait pas encore relié directement à l'auteur
    broadcast(o, c);

    notifyUnread(1, notificationTitle(o), notificationBody(o), Protocol::Message::timestamp(o),
                 mentionsMe(o) || repliesToMe(o));
}

void MainWindow::broadcast(const QJsonObject &obj, PeerConnection *except)
{
    for (PeerConnection *p : std::as_const(m_active))
        if (p != except)
            p->send(obj);
}

void MainWindow::closeAll()
{
    QList<PeerConnection *> all = m_active.values() + m_attempts.values() + m_pending.values();
    m_active.clear();
    m_attempts.clear();
    m_pending.clear();
    m_dropped.clear();
    m_backoff.clear();
    m_peerPresence.clear();
    m_quietDrops.clear();
    for (const PeerConnection *c : std::as_const(all))
        if (c->isReady())
            terminalPeerLost(c->peerOnion());
#ifdef P2P_TERMINAL
    if (m_term) // plus aucune connexion (Tor arrêté, groupe quitté) : inutile d'attendre
        m_term->end(tr("connexions fermées"), false);
#endif
    for (PeerConnection *c : all)
        c->close();
}

void MainWindow::onWake(qint64 gapMs)
{
    qInfo().noquote() << "[réseau] reprise après une pause de" << gapMs / 1000 << "s";
    if (m_myOnion.isEmpty())
        return;
    // Pendant la pause, nos pings ne sont pas partis : les correspondants nous ont fermé la porte,
    // mais on ne s'en apercevrait qu'au bout du délai d'inactivité. On repart tout de suite.
    if (gapMs > SleepKillsLinksMs) {
        const QList<PeerConnection *> dead = m_active.values();
        for (PeerConnection *c : dead)
            c->close();
    }
    retrySoon();
}

void MainWindow::retrySoon()
{
    if (m_myOnion.isEmpty())
        return; // Tor pas prêt : il rappellera tout le monde lui-même
    m_fastRetryUntil = QDateTime::currentMSecsSinceEpoch() + FastRetryMs;
    reconnectNow();
}

QJsonObject MainWindow::presenceMessage() const
{
    QJsonArray on;
    for (auto it = m_active.cbegin(); it != m_active.cend(); ++it)
        on.append(it.key());
    return {{"t", "presence"}, {"on", on}};
}

QHash<QString, QString> MainWindow::relayedOnline() const
{
    const QList<QString> active = m_active.keys();
    return ConnectionPolicy::relayedOnline(m_peerPresence, QSet<QString>(active.cbegin(), active.cend()), m_myOnion);
}

void MainWindow::checkQuietDrops()
{
    if (m_quietDrops.isEmpty())
        return;
    const QHash<QString, QString> relayed = relayedOnline();
    for (auto it = m_quietDrops.begin(); it != m_quietDrops.end();) {
        if (!m_active.contains(it.key()) && !relayed.contains(it.key())) {
            m_chat->addPresence(memberNick(it.key()), false, it.value());
            it = m_quietDrops.erase(it);
        } else {
            ++it;
        }
    }
}

void MainWindow::sendCurrent()
{
    if (m_mentions->isPopupVisible())
        return; // Entrée choisit un pseudo dans la liste
    const QString text = m_input->text().trimmed();
    if ((text.isEmpty() && m_pendingImage.isEmpty()) || !m_group.inGroup() || m_group.members().isEmpty()
        || m_myOnion.isEmpty())
        return; // sans personne en ligne, le message partira à la prochaine synchronisation
    const QJsonObject msg = Protocol::Message::create(m_group.identitySeed(), m_myOnion, m_group.nickname(), text,
                                                      QDateTime::currentMSecsSinceEpoch(), {m_replyTo, m_pendingImage});
    m_seenIds.insert(Protocol::Message::id(msg));
    m_chat->clearUnreadMarker(); // on a répondu : les messages précédents sont lus
    m_lastTypingSent = 0;        // prochaine frappe : signalée tout de suite
    broadcast(msg);
    appendMessage(msg, true, ChatEntry::Verified);
    m_input->clear();
    cancelReply();
    cancelImage();
}

// ---------------------------------------------------------------- Images

void MainWindow::chooseImage()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString path =
        QFileDialog::getOpenFileName(this, tr("Joindre une image"), dir, Attachment::fileDialogFilter());
    if (!path.isEmpty())
        attachPrepared(Attachment::prepareFile(path));
}

bool MainWindow::attachFromMime(const QMimeData *mime)
{
    if (!mime || !m_input->isEnabled())
        return false;
    // Fichier (gestionnaire de fichiers) : on relit le fichier pour garder la qualité, l'orientation et
    // l'animation d'un GIF
    for (const QUrl &url : mime->urls()) {
        if (url.isLocalFile() && Attachment::isImageFile(url.toLocalFile())) {
            attachPrepared(Attachment::prepareFile(url.toLocalFile()));
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
    m_imagePreview->clear();
    m_imageBar->setVisible(false);
}

QString MainWindow::notificationBody(const QJsonObject &msg) const
{
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
        usable = usable || (url.isLocalFile() && Attachment::isImageFile(url.toLocalFile()));
    if (usable && m_input->isEnabled())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    if (attachFromMime(event->mimeData()))
        event->acceptProposedAction();
}


// ---------------------------------------------------------------- Réglages

void MainWindow::changeNickname()
{
    bool ok = false;
    const QString n = QInputDialog::getText(this, tr("Pseudo"), tr("Votre pseudo :"), QLineEdit::Normal,
                                            m_group.nickname(), &ok).trimmed();
    if (!ok || n.isEmpty())
        return;
    m_group.setNickname(n);
    broadcast(membersMessage()); // les autres mettent à jour notre pseudo
}

void MainWindow::resetAll()
{
    if (QMessageBox::question(this, tr("Quitter le groupe"),
                              tr("Oublier tous les membres et le code secret ?\n"
                                 "Il faudra une nouvelle invitation. L'historique est conservé.\n"
                                 "(Les autres membres continueront d'essayer de vous joindre "
                                 "mais seront refusés.)"))
        != QMessageBox::Yes)
        return;
    closeAll();
    m_group.leaveGroup();
    m_lastError.clear();
    appendSystem(tr("Vous avez quitté le groupe."));
    updateStatus();
}

// ---------------------------------------------------------------- Code de sécurité

void MainWindow::verifyMember()
{
    // Membres dont on connaît la clé (rencontrés au moins une fois, ou présentés par un autre)
    QList<Member> known;
    for (const Member &m : m_group.members())
        if (!m_group.pinnedKey(m.onion).isEmpty())
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
        names << (m_group.isVerified(m.onion) ? tr("%1 ✔").arg(m.nick) : m.nick);
    bool ok = false;
    const QString choice = QInputDialog::getItem(this, tr("Vérifier l'identité"), tr("Membre :"), names, 0, false, &ok);
    if (ok)
        showSafetyNumber(known.at(int(names.indexOf(choice))).onion);
}

void MainWindow::showSafetyNumber(const QString &onion)
{
    if (m_myOnion.isEmpty()) {
        QMessageBox::information(this, tr("Vérifier l'identité"),
                                 tr("Le code s'affichera une fois connecté au réseau Tor."));
        return;
    }
    const QString number =
        SafetyNumber::compute(m_myOnion, m_group.identityKey(), onion, m_group.pinnedKey(onion));
    if (number.isEmpty())
        return;
    const QString nick = memberNick(onion);
    const bool verified = m_group.isVerified(onion);

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
        m_group.setVerified(onion, !verified);
        m_chat->setAuthorVerified(onion, !verified);
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

void MainWindow::react(const QString &id, const QString &emoji)
{
    if (id.isEmpty() || m_myOnion.isEmpty() || !m_group.inGroup() || !Reactions::Allowed.contains(emoji))
        return;
    const bool on = !m_reactions.mineOn(id, emoji); // la nôtre est déjà là : on la retire
    const QJsonObject r = Reactions::create(m_group.identitySeed(), m_myOnion, id, emoji, on,
                                            QDateTime::currentMSecsSinceEpoch());
    acceptReaction(r, true, true);
    broadcastReaction(r);
}

bool MainWindow::verifyReaction(const QJsonObject &r, bool mine) const
{
    const QByteArray key = mine ? m_group.identityKey() : m_group.pinnedKey(r.value("a").toString());
    return Reactions::verify(r, key);
}

bool MainWindow::acceptReaction(const QJsonObject &r, bool mine, bool save)
{
    const QString id = Protocol::Message::id(r);
    if (id.isEmpty() || m_store.contains(id))
        return false;
    m_store.insert(id, History::stored(r, mine)); // synchronisée comme les messages
    m_seenIds.insert(id);
    if (m_reactions.apply(r, mine))
        m_chat->setReactions(Reactions::target(r), reactionSummary(Reactions::target(r)));
    if (save)
        appendToHistoryFile(r, mine);
    return true;
}

QList<ChatEntry::Reaction> MainWindow::reactionSummary(const QString &id) const
{
    QList<ChatEntry::Reaction> out;
    if (id.isEmpty())
        return out;
    for (const Reactions::Summary &s : m_reactions.summary(id)) {
        QStringList names;
        for (const QString &onion : s.authors)
            names << (onion.isEmpty() || onion == m_myOnion ? tr("Vous") : memberNick(onion));
        out.append({s.emoji, int(s.authors.size()), s.mine, names.join(", ")});
    }
    return out;
}

void MainWindow::broadcastReaction(const QJsonObject &r, PeerConnection *except)
{
    for (PeerConnection *p : std::as_const(m_active))
        if (p != except && p->peerVersion() >= Reactions::Version)
            p->send(r);
}

// ---------------------------------------------------------------- Écriture en cours, recherche, liens

void MainWindow::onTyping()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_input->text().trimmed().isEmpty() || now - m_lastTypingSent < TypingSendMs)
        return;
    m_lastTypingSent = now;
    // Seulement aux membres reliés directement : ni signé, ni relayé, ni conservé
    broadcast({{"t", "typing"}});
}

void MainWindow::onPeerTyping(PeerConnection *c)
{
    const QString from = m_active.key(c);
    if (from.isEmpty())
        return;
    m_typing.insert(from, QDateTime::currentMSecsSinceEpoch() + TypingShownMs);
    if (!m_typingTimer.isActive())
        m_typingTimer.start();
    updateStatus();
}

QString MainWindow::typingText() const
{
    QStringList names;
    for (auto it = m_typing.cbegin(); it != m_typing.cend(); ++it)
        names << memberNick(it.key());
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

void MainWindow::sendTo(const QString &onion, const QJsonObject &obj)
{
    if (PeerConnection *c = m_active.value(onion))
        c->send(obj);
}

#ifdef P2P_TERMINAL

void MainWindow::shareTerminal()
{
    if (m_term || !m_termOffer.sid.isEmpty()) {
        QMessageBox::information(this, tr("Partager ma console"), tr("Un partage de console est déjà en cours."));
        return;
    }
    QList<Member> online;
    for (const Member &m : m_group.members())
        if (m_active.contains(m.onion))
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
    if (dlg.exec() != QDialog::Accepted || m_term || !m_termOffer.sid.isEmpty())
        return;

    const QString onion = who->currentData().toString();
    if (!m_active.contains(onion)) {
        QMessageBox::information(this, tr("Partager ma console"), tr("%1 n'est plus en ligne.").arg(memberNick(onion)));
        return;
    }
    const QString sid = QString::fromLatin1(Crypto::randomBytes(12).toHex());
    m_termOffer = {sid, onion, control->isChecked()};
    sendTo(onion, {{"t", "term-offer"}, {"sid", sid}, {"ctl", m_termOffer.control}});
    appendSystem(tr("Proposition de partage de console envoyée à %1…").arg(memberNick(onion)));

    QTimer::singleShot(120000, this, [this, sid] {
        if (m_termOffer.sid != sid)
            return;
        sendTo(m_termOffer.onion, {{"t", "term-end"}, {"sid", sid}});
        appendSystem(tr("%1 n'a pas répondu à la proposition de partage de console.").arg(memberNick(m_termOffer.onion)));
        m_termOffer = {};
    });
}

void MainWindow::stopTerminal()
{
    if (m_term) {
        m_term->end(m_term->role() == TerminalSession::Host ? tr("vous avez arrêté le partage")
                                                            : tr("vous avez quitté la console"));
    } else if (!m_termOffer.sid.isEmpty()) {
        sendTo(m_termOffer.onion, {{"t", "term-end"}, {"sid", m_termOffer.sid}});
        appendSystem(tr("Proposition de partage de console annulée."));
        m_termOffer = {};
    }
}

bool MainWindow::startTerminal(bool host, const QString &sid, const QString &onion, bool control)
{
    auto *t = new TerminalSession(host ? TerminalSession::Host : TerminalSession::Guest, sid, onion,
                                  memberNick(onion), control, this);
    t->setBacklogProbe([this, onion] {
        const PeerConnection *c = m_active.value(onion);
        return c ? c->pendingBytes() : qint64(0);
    });
    connect(t, &TerminalSession::sendToPeer, this, [this, onion](const QJsonObject &o) { sendTo(onion, o); });
    connect(t, &TerminalSession::finished, this, [this, t](const QString &reason) {
        if (m_term == t)
            m_term = nullptr;
        t->deleteLater();
        appendSystem(tr("Console partagée fermée : %1.").arg(reason));
    });
    m_term = t;
    QString error;
    if (!t->start(&error)) {
        t->end(error); // prévient l'autre membre
        return false;
    }
    return true;
}

void MainWindow::onTerminalMessage(PeerConnection *c, const QJsonObject &o)
{
    const QString type = o.value("t").toString();
    const QString sid = o.value("sid").toString();
    const QString from = c->peerOnion();
    if (sid.isEmpty())
        return;

    if (type == "term-offer") {
        if (m_term || m_termPrompt || !m_termOffer.sid.isEmpty()) {
            c->send({{"t", "term-refuse"}, {"sid", sid}, {"busy", true}});
            return;
        }
        const bool control = o.value("ctl").toBool();
        const QString nick = memberNick(from);
        m_termIncoming = {sid, from, control};
        auto *box = new QMessageBox(
            QMessageBox::Question, tr("Console partagée"),
            tr("<b>%1</b> vous propose de voir sa console.<br><br>%2")
                .arg(nick.toHtmlEscaped(),
                     control ? tr("Vous pourrez y taper des commandes, exécutées sur son poste.")
                             : tr("En lecture seule : vous verrez ce qui s'affiche, sans pouvoir taper.")),
            QMessageBox::NoButton, this);
        QPushButton *accept = box->addButton(tr("Ouvrir la console"), QMessageBox::AcceptRole);
        box->addButton(tr("Refuser"), QMessageBox::RejectRole);
        box->setAttribute(Qt::WA_DeleteOnClose);
        m_termPrompt = box;
        connect(box, &QMessageBox::finished, this, [this, box, accept] {
            const TermOffer offer = m_termIncoming;
            m_termIncoming = {};
            if (offer.sid.isEmpty())
                return; // annulée entre-temps par l'autre membre
            if (box->clickedButton() != accept) {
                sendTo(offer.onion, {{"t", "term-refuse"}, {"sid", offer.sid}});
                appendSystem(tr("Vous avez refusé la console de %1.").arg(memberNick(offer.onion)));
                return;
            }
            if (!m_active.contains(offer.onion) || m_term) {
                appendSystem(tr("%1 n'est plus en ligne : console non ouverte.").arg(memberNick(offer.onion)));
                return;
            }
            if (startTerminal(false, offer.sid, offer.onion, offer.control))
                sendTo(offer.onion, {{"t", "term-accept"}, {"sid", offer.sid}});
        });
        box->open();
        if (!isVisible() || !isActiveWindow()) {
            m_notifier->notify(nick, tr("vous propose de partager sa console"));
            if (isVisible())
                QApplication::alert(this);
        }
        return;
    }

    // Réponses à notre proposition
    if (!m_termOffer.sid.isEmpty() && sid == m_termOffer.sid && from == m_termOffer.onion) {
        const TermOffer offer = m_termOffer;
        if (type == "term-accept") {
            m_termOffer = {};
            if (startTerminal(true, offer.sid, offer.onion, offer.control))
                appendSystem(tr("Console partagée avec %1 (%2).")
                                 .arg(memberNick(offer.onion),
                                      offer.control ? tr("peut taper des commandes") : tr("lecture seule")));
        } else if (type == "term-refuse" || type == "term-end") {
            m_termOffer = {};
            appendSystem(o.value("busy").toBool()
                             ? tr("%1 a déjà une console partagée en cours.").arg(memberNick(offer.onion))
                             : tr("%1 a refusé le partage de console.").arg(memberNick(offer.onion)));
        }
        return;
    }

    // Proposition reçue, annulée avant notre réponse
    if (type == "term-end" && sid == m_termIncoming.sid && from == m_termIncoming.onion) {
        m_termIncoming = {};
        if (m_termPrompt)
            m_termPrompt->close();
        appendSystem(tr("%1 a annulé sa proposition de console.").arg(memberNick(from)));
        return;
    }

    // Session en cours : seul l'autre membre de la session peut y écrire
    if (m_term && sid == m_term->sid() && from == m_term->peerOnion()) {
        m_term->handlePeerMessage(o);
        return;
    }
    // Reprise d'une session qui n'existe plus chez nous (terminée pendant la coupure)
    if (type == "term-resume")
        c->send({{"t", "term-end"}, {"sid", sid}});
}

void MainWindow::terminalPeerLost(const QString &onion)
{
    if (m_term && m_term->peerOnion() == onion)
        m_term->setLinkDown(); // la session attend le retour du membre (2 min au plus)
    if (m_termOffer.onion == onion && !m_termOffer.sid.isEmpty()) {
        m_termOffer = {};
        appendSystem(tr("Proposition de console annulée : %1 s'est déconnecté(e).").arg(memberNick(onion)));
    }
    if (m_termIncoming.onion == onion && !m_termIncoming.sid.isEmpty()) {
        m_termIncoming = {};
        if (m_termPrompt)
            m_termPrompt->close();
    }
}

#else // pas de console partagée sur cette plateforme

void MainWindow::shareTerminal() {}
void MainWindow::stopTerminal() {}
bool MainWindow::startTerminal(bool, const QString &, const QString &, bool) { return false; }
void MainWindow::terminalPeerLost(const QString &) {}

void MainWindow::onTerminalMessage(PeerConnection *c, const QJsonObject &o)
{
    if (o.value("t").toString() == "term-offer")
        c->send({{"t", "term-refuse"}, {"sid", o.value("sid").toString()}});
}

#endif
