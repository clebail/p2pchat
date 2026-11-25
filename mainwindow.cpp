#include "mainwindow.h"

#include "chatview.h"
#include "crypto.h"
#include "invitation.h"
#include "notifier.h"
#include "peerconnection.h"
#include "protocol.h"
#include "torservice.h"
#ifdef P2P_TERMINAL
#  include "terminalsession.h"
#endif

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QSystemTrayIcon>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
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
QLineEdit#composer { background: #f0f4f9; color: #1f1f1f; border: none; border-radius: 24px;
                     padding: 0 20px; font-size: 15px; min-height: 48px; }
QLineEdit#composer:disabled { color: #9aa0a6; }
QToolButton#sendButton { background: #0b57d0; border: none; border-radius: 24px; }
QToolButton#sendButton:hover { background: #0842a0; }
QToolButton#sendButton:disabled { background: #c4c7c5; }
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

    // Zone de saisie
    auto *composerBar = new QWidget(central);
    composerBar->setObjectName("composerBar");
    composerBar->setAttribute(Qt::WA_StyledBackground);
    auto *row = new QHBoxLayout(composerBar);
    row->setContentsMargins(12, 8, 12, 12);
    row->setSpacing(8);
    m_input = new QLineEdit(composerBar);
    m_input->setObjectName("composer");
    m_input->setMaxLength(4000);
    m_input->setPlaceholderText(tr("Message"));
    m_sendButton = new QToolButton(composerBar);
    m_sendButton->setObjectName("sendButton");
    m_sendButton->setIcon(sendIcon());
    m_sendButton->setIconSize(QSize(22, 22));
    m_sendButton->setFixedSize(48, 48);
    m_sendButton->setCursor(Qt::PointingHandCursor);
    m_sendButton->setToolTip(tr("Envoyer"));
    row->addWidget(m_input, 1);
    row->addWidget(m_sendButton);
    layout->addWidget(composerBar);
    setCentralWidget(central);

    connect(m_input, &QLineEdit::returnPressed, this, &MainWindow::sendCurrent);
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
        if (!m_active.contains(m.onion) && m_lastError.contains(m.onion))
            state += " — " + m_lastError.value(m.onion);
        if (!m_active.contains(m.onion) && !m_attempts.contains(m.onion)
            && m_backoff.value(m.onion).nextTry > QDateTime::currentMSecsSinceEpoch())
            state += tr(" (nouvel essai à %1)")
                         .arg(QDateTime::fromMSecsSinceEpoch(m_backoff.value(m.onion).nextTry).toString("HH:mm:ss"));
        tips << QStringLiteral("%1 : %2").arg(m.nick, state).toHtmlEscaped();
    }
    m_subtitle->setToolTip(tips.isEmpty() ? QString() : "<p>" + tips.join("<br>") + "</p>");

    const bool online = !m_active.isEmpty();
    const bool canWrite = inGroup && !list.isEmpty() && !m_myOnion.isEmpty();
    m_input->setEnabled(canWrite);
    m_sendButton->setEnabled(canWrite);
    m_input->setPlaceholderText(online ? tr("Message") : tr("Message (transmis à la prochaine connexion)"));
    if (canWrite && !m_input->hasFocus())
        m_input->setFocus();
}

void MainWindow::appendMessage(const QJsonObject &msg, bool mine, ChatEntry::Trust trust, bool save)
{
    const qint64 ms = qint64(msg.value("ts").toDouble());
    const QDateTime ts = ms > 0 ? QDateTime::fromMSecsSinceEpoch(ms) : QDateTime::currentDateTime();
    m_chat->addMessage(msg.value("n").toString().left(Protocol::MaxNickLength), msg.value("x").toString(), ts, mine,
                       trust);
    const QString id = Protocol::Message::id(msg);
    if (!id.isEmpty() && trust == ChatEntry::Verified) { // seuls les messages authentiques sont resynchronisés
        m_store.insert(id, History::stored(msg, mine));
        m_seenIds.insert(id);
    }
    if (!save)
        return;
    // On enregistre le message tel qu'il a été signé : l'historique reste vérifiable
    QFile file(m_historyPath);
    const bool created = !file.exists();
    if (file.open(QIODevice::Append)) {
        if (created)
            file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        file.write(History::toLine(msg, mine));
    }
}

void MainWindow::appendSystem(const QString &text)
{
    m_chat->addSystem(text);
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
    QFile::remove(m_historyPath);
    m_store.clear();
    m_group.setClearedBefore(QDateTime::currentMSecsSinceEpoch());
    updateStatus();
}

void MainWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::ActivationChange && isActiveWindow() && m_unread) {
        m_unread = 0;
        updateUnread();
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
    for (const QJsonArray &batch : History::batches(messages, cutoff))
        c->send({{"t", "sync"}, {"m", batch}});
}

void MainWindow::receiveSync(PeerConnection *from, const QJsonArray &messages)
{
    QList<QJsonObject> accepted;
    const qint64 cutoff = syncCutoff();
    int received = 0;
    QString lastAuthor, lastText;
    for (const QJsonValue &v : messages) {
        const QJsonObject m = v.toObject();
        const QString id = Protocol::Message::id(m);
        if (id.isEmpty() || m_store.contains(id) || Protocol::Message::timestamp(m) < cutoff)
            continue;
        const bool mine = !m_myOnion.isEmpty() && m.value("a").toString() == m_myOnion;
        // Chaque message est vérifié avec la clé de son auteur : un membre ne peut rien inventer
        if (verifyMessage(m, mine) != ChatEntry::Verified) {
            qWarning().noquote() << "[synchro] message rejeté, signature invalide :" << id;
            continue;
        }
        appendMessage(m, mine, ChatEntry::Verified);
        accepted.append(m);
        if (!mine) {
            ++received;
            lastAuthor = m.value("n").toString();
            lastText = m.value("x").toString();
        }
    }
    // Relais aux autres membres connectés, qui ont pu manquer ces messages eux aussi
    if (!accepted.isEmpty())
        for (PeerConnection *p : std::as_const(m_active))
            if (p != from)
                sendBatches(p, accepted);
    if (received == 1)
        notifyUnread(1, lastAuthor, lastText);
    else if (received > 1)
        notifyUnread(received, tr("P2P Chat"),
                     tr("%n message(s) reçu(s) pendant votre absence", nullptr, received));
}

void MainWindow::notifyUnread(int count, const QString &title, const QString &text)
{
    if (isVisible() && isActiveWindow())
        return;
    m_unread += count;
    updateUnread();
    m_notifier->notify(title, text);
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
    if (!m_active.contains(authorOnion) && m_backoff.remove(authorOnion) > 0)
        connectToMembers();

    // Relais : au cas où un membre ne serait pas encore relié directement à l'auteur
    broadcast(o, c);

    notifyUnread(1, author, o.value("x").toString());
    if (isVisible() && !isActiveWindow())
        QApplication::alert(this);
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
    const QString text = m_input->text().trimmed();
    if (text.isEmpty() || !m_group.inGroup() || m_group.members().isEmpty() || m_myOnion.isEmpty())
        return; // sans personne en ligne, le message partira à la prochaine synchronisation
    const QJsonObject msg = Protocol::Message::create(m_group.identitySeed(), m_myOnion, m_group.nickname(), text,
                                                      QDateTime::currentMSecsSinceEpoch());
    m_seenIds.insert(Protocol::Message::id(msg));
    broadcast(msg);
    appendMessage(msg, true, ChatEntry::Verified);
    m_input->clear();
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
