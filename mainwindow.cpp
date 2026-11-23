#include "mainwindow.h"

#include "chatview.h"
#include "crypto.h"
#include "invitation.h"
#include "notifier.h"
#include "peerconnection.h"
#include "torservice.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
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
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

namespace {

bool isValidOnion(const QString &s)
{
    static const QRegularExpression re(QStringLiteral("^[a-z2-7]{56}\\.onion$"));
    return re.match(s).hasMatch();
}

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

const QByteArray MessageContext("p2pchat-msg-v1");

QString toB64(const QByteArray &b)
{
    return QString::fromLatin1(b.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QByteArray fromB64(const QJsonValue &v)
{
    return QByteArray::fromBase64(v.toString().toLatin1(), QByteArray::Base64UrlEncoding);
}

// Ce que l'auteur signe : identifiant, adresse, pseudo, texte et heure
QByteArray messageSigningBytes(const QJsonObject &m)
{
    return Crypto::pack({MessageContext, m.value("id").toString().toUtf8(), m.value("a").toString().toUtf8(),
                         m.value("n").toString().toUtf8(), m.value("x").toString().toUtf8(),
                         QByteArray::number(qint64(m.value("ts").toDouble()))});
}

constexpr int MaxSeenIds = 5000;
constexpr int MaxMembers = 32; // maillage complet : au-delà, ce n'est plus raisonnable
constexpr int MaxPending = 16; // connexions entrantes pas encore authentifiées
constexpr qint64 SyncWindowMs = 30LL * 24 * 3600 * 1000; // historique synchronisé : 30 jours
constexpr qint64 NewMemberMarginMs = 3600LL * 1000;      // marge d'horloge pour un nouveau membre
constexpr int MaxInventory = 4000;                       // identifiants annoncés au plus
constexpr int MaxSyncBatchBytes = 150 * 1024;            // taille d'un lot (trame max : 256 Ko)

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);
    m_historyPath = dataDir + "/history.jsonl";

    if (identitySeed().size() != 32)
        m_settings.setValue("identity/seed", Ed25519::generateSeed());

    // Le secret du groupe, la clé d'identité, les clés Tor et l'historique : lisibles par nous seuls
    constexpr auto OwnerRW = QFile::ReadOwner | QFile::WriteOwner;
    m_settings.sync();
    QFile::setPermissions(m_settings.fileName(), OwnerRW);
    QFile::setPermissions(dataDir, OwnerRW | QFile::ExeOwner);
    if (QFile::exists(m_historyPath))
        QFile::setPermissions(m_historyPath, OwnerRW);

    // Reprise de la configuration de la version à deux participants
    if (m_settings.contains("contact/secret")) {
        m_settings.setValue("group/secret", m_settings.value("contact/secret"));
        const QString onion = m_settings.value("contact/onion").toString();
        if (isValidOnion(onion))
            saveMembers({{onion, m_settings.value("contact/nick").toString()}});
        m_settings.remove("contact");
    }

    m_server = new QTcpServer(this);
    m_server->setProxy(QNetworkProxy::NoProxy);
    connect(m_server, &QTcpServer::newConnection, this, &MainWindow::onNewConnection);
    if (!m_server->listen(QHostAddress::LocalHost, 0))
        QMessageBox::critical(this, tr("P2P Chat"), tr("Écoute locale impossible : %1").arg(m_server->errorString()));

    // Tentative périodique vers les membres hors ligne
    m_retryTimer.setInterval(30000);
    connect(&m_retryTimer, &QTimer::timeout, this, &MainWindow::connectToMembers);
    m_syncTimer.setInterval(5 * 60 * 1000);
    connect(&m_syncTimer, &QTimer::timeout, this, [this] {
        if (!m_active.isEmpty())
            broadcast(inventoryMessage());
    });
    m_syncTimer.start();

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
    const QList<Member> list = members();
    const bool inGroup = !secret().isEmpty();
    m_pages->setCurrentIndex(inGroup || m_chat->chatModel()->rowCount() > 0 ? 1 : 0);

    // Titre : les pseudos des membres
    QStringList names;
    for (const Member &m : list)
        names << (m.nick.isEmpty() ? tr("?") : m.nick);
    m_title->setText(names.isEmpty() ? tr("P2P Chat") : names.join(", "));

    const QString dot = QStringLiteral("<span style='color:%1'>●</span> ");
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
        QStringList online;
        for (const Member &m : list)
            if (m_active.contains(m.onion))
                online << m.nick.toHtmlEscaped();
        if (online.isEmpty())
            sub = dot.arg("#9aa0a6") + tr("Personne n'est en ligne");
        else if (online.size() == list.size())
            sub = dot.arg("#188038") + (list.size() == 1 ? tr("En ligne") : tr("Tout le monde est en ligne"));
        else
            sub = dot.arg("#188038") + tr("En ligne : %1").arg(online.join(", "));
    }
    m_subtitle->setText(sub);

    // Infobulle : état détaillé de chaque membre
    QStringList tips;
    for (const Member &m : list) {
        QString state = m_active.contains(m.onion)     ? tr("en ligne")
                        : m_attempts.contains(m.onion) ? tr("connexion…")
                                                       : tr("hors ligne");
        if (!m_active.contains(m.onion) && m_lastError.contains(m.onion))
            state += " — " + m_lastError.value(m.onion);
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
    m_chat->addMessage(msg.value("n").toString().left(40), msg.value("x").toString(), ts, mine, trust);
    const QString id = msg.value("id").toString();
    if (!id.isEmpty() && trust == ChatEntry::Verified) { // seuls les messages authentiques sont resynchronisés
        QJsonObject stored = msg;
        stored.remove("t");
        stored["mine"] = mine;
        m_store.insert(id, stored);
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
        QJsonObject line = msg;
        line.remove("t");
        line["mine"] = mine;
        file.write(QJsonDocument(line).toJson(QJsonDocument::Compact) + '\n');
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
    struct Row
    {
        QJsonObject msg;
        bool mine;
        ChatEntry::Trust trust;
        qint64 ts;
    };
    QList<Row> rows;
    QSet<QString> ids;
    const qint64 cleared = m_settings.value("history/clearedBefore").toLongLong();
    for (const QByteArray &line : file.readAll().split('\n')) {
        const QJsonObject o = QJsonDocument::fromJson(line).object();
        if (o.isEmpty())
            continue;
        if (o.contains("from")) { // ancien format, antérieur aux signatures
            const qint64 ts = QDateTime::fromString(o["ts"].toString(), Qt::ISODate).toMSecsSinceEpoch();
            const QJsonObject legacy{{"n", o["from"]}, {"x", o["text"]}, {"ts", double(ts)}};
            rows.append({legacy, o["mine"].toBool(), ChatEntry::Unsigned, ts});
            continue;
        }
        const QString id = o["id"].toString();
        if (!id.isEmpty() && ids.contains(id))
            continue;
        ids.insert(id);
        const qint64 ts = qint64(o["ts"].toDouble());
        if (ts < cleared)
            continue;
        const bool mine = o["mine"].toBool();
        rows.append({o, mine, verifyMessage(o, mine), ts});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.ts < b.ts; });
    for (qsizetype i = qMax<qsizetype>(0, rows.size() - 500); i < rows.size(); ++i)
        appendMessage(rows[i].msg, rows[i].mine, rows[i].trust, false);
    // Les plus anciens (non affichés) restent disponibles pour la synchronisation
    for (qsizetype i = 0; i < rows.size() - 500; ++i) {
        const QString id = rows[i].msg.value("id").toString();
        if (!id.isEmpty() && rows[i].trust == ChatEntry::Verified) {
            QJsonObject stored = rows[i].msg;
            stored["mine"] = rows[i].mine;
            m_store.insert(id, stored);
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
    m_settings.setValue("history/clearedBefore", QDateTime::currentMSecsSinceEpoch());
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
    if (secret().isEmpty())
        m_settings.setValue("group/secret", Crypto::randomBytes(16));
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
    const QString code = Invitation{secret(), m_myOnion, nickname(), myIdentityKey()}.toCode();

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
    share->setChecked(m_settings.value("group/shareHistoryUntil").toLongLong() > QDateTime::currentMSecsSinceEpoch());
    connect(share, &QCheckBox::toggled, this, [this](bool on) {
        if (on)
            m_settings.setValue("group/shareHistoryUntil",
                                QDateTime::currentDateTime().addDays(7).toMSecsSinceEpoch());
        else
            m_settings.remove("group/shareHistoryUntil");
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
                                   .arg(code, nickname()));
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
    if (!secret().isEmpty() && secret() != inv->secret) {
        if (QMessageBox::question(this, tr("Rejoindre"),
                                  tr("<p>Vous faites déjà partie d'une autre conversation. "
                                     "La quitter pour rejoindre celle de %1 ?</p>").arg(inv->nickname.toHtmlEscaped()))
            != QMessageBox::Yes)
            return;
        closeAll();
        saveMembers({});
    }
    if (!pinKey(inv->onion, inv->identityKey)) {
        QMessageBox::warning(this, tr("Rejoindre"),
                             tr("<p>Ce code présente une clé d'identité différente de celle déjà connue pour %1.<br>"
                                "Par sécurité, il est refusé.</p>").arg(inv->nickname.toHtmlEscaped()));
        return;
    }
    m_settings.setValue("group/secret", inv->secret);
    mergeMember(inv->onion, inv->nickname);
    appendSystem(tr("Invitation de %1 enregistrée.").arg(inv->nickname));
    connectToMembers();
    updateStatus();
}

// ---------------------------------------------------------------- Membres

QList<MainWindow::Member> MainWindow::members() const
{
    QList<Member> list;
    const QJsonArray arr = QJsonDocument::fromJson(m_settings.value("group/members").toByteArray()).array();
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        if (isValidOnion(o["o"].toString()))
            list.append({o["o"].toString(), o["n"].toString()});
    }
    return list;
}

void MainWindow::saveMembers(const QList<Member> &list)
{
    QJsonArray arr;
    for (const Member &m : list)
        arr.append(QJsonObject{{"o", m.onion}, {"n", m.nick}});
    m_settings.setValue("group/members", QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

bool MainWindow::mergeMember(const QString &onion, const QString &nick)
{
    if (!isValidOnion(onion) || onion == m_myOnion)
        return false;
    QList<Member> list = members();
    for (Member &m : list) {
        if (m.onion == onion) {
            if (!nick.isEmpty() && nick != m.nick) {
                m.nick = nick.left(40);
                saveMembers(list);
            }
            return false;
        }
    }
    if (list.size() >= MaxMembers)
        return false; // protège contre une liste de membres gonflée artificiellement
    list.append({onion, nick.left(40)});
    saveMembers(list);
    return true;
}

QString MainWindow::memberNick(const QString &onion) const
{
    for (const Member &m : members())
        if (m.onion == onion && !m.nick.isEmpty())
            return m.nick;
    return tr("Un membre");
}

QJsonObject MainWindow::membersMessage() const
{
    QJsonArray arr;
    arr.append(QJsonObject{{"o", m_myOnion}, {"n", nickname()}, {"k", toB64(myIdentityKey())}});
    for (const Member &m : members())
        arr.append(QJsonObject{{"o", m.onion}, {"n", m.nick}, {"k", toB64(pinnedKey(m.onion))}});
    return {{"t", "members"}, {"m", arr}};
}

// ---------------------------------------------------------------- Identité et signatures


QByteArray MainWindow::identitySeed() const
{
    return m_settings.value("identity/seed").toByteArray();
}

QByteArray MainWindow::myIdentityKey() const
{
    return Ed25519::publicKey(identitySeed());
}

QByteArray MainWindow::pinnedKey(const QString &onion) const
{
    const QJsonObject ring = QJsonDocument::fromJson(m_settings.value("keyring").toByteArray()).object();
    return fromB64(ring.value(onion));
}

bool MainWindow::pinKey(const QString &onion, const QByteArray &key)
{
    if (key.size() != 32 || !isValidOnion(onion))
        return false;
    const QByteArray known = pinnedKey(onion);
    if (!known.isEmpty())
        return known == key;
    QJsonObject ring = QJsonDocument::fromJson(m_settings.value("keyring").toByteArray()).object();
    ring.insert(onion, toB64(key));
    m_settings.setValue("keyring", QJsonDocument(ring).toJson(QJsonDocument::Compact));
    QJsonObject seen = QJsonDocument::fromJson(m_settings.value("firstSeen").toByteArray()).object();
    // Invitation avec partage d'historique en cours : le nouveau venu reçoit les 30 derniers jours
    const bool shareHistory =
        m_settings.value("group/shareHistoryUntil").toLongLong() > QDateTime::currentMSecsSinceEpoch();
    seen.insert(onion, shareHistory ? 0.0 : double(QDateTime::currentMSecsSinceEpoch()));
    m_settings.setValue("firstSeen", QJsonDocument(seen).toJson(QJsonDocument::Compact));
    return true;
}

QJsonObject MainWindow::signedMessage(const QString &text) const
{
    QJsonObject m{{"t", "msg"},
                  {"id", QString::fromLatin1(Crypto::randomBytes(12).toHex())},
                  {"a", m_myOnion},
                  {"n", nickname()},
                  {"x", text},
                  {"ts", double(QDateTime::currentMSecsSinceEpoch())}};
    m.insert("s", toB64(Ed25519::sign(identitySeed(), messageSigningBytes(m))));
    return m;
}

ChatEntry::Trust MainWindow::verifyMessage(const QJsonObject &msg, bool mine) const
{
    if (!msg.contains("s"))
        return ChatEntry::Unsigned;
    const QByteArray key = mine ? myIdentityKey() : pinnedKey(msg.value("a").toString());
    return Ed25519::verify(key, messageSigningBytes(msg), fromB64(msg.value("s"))) ? ChatEntry::Verified
                                                                                   : ChatEntry::Invalid;
}

// ---------------------------------------------------------------- Connexions

void MainWindow::onNewConnection()
{
    while (QTcpSocket *s = m_server->nextPendingConnection()) {
        if (secret().isEmpty() || m_myOnion.isEmpty() || m_pending.size() >= MaxPending) {
            s->abort();
            s->deleteLater();
            continue;
        }
        auto *c = new PeerConnection(s, false, secret(), nickname(), m_myOnion, identitySeed(), this);
        m_pending.insert(c);
        watch(c);
    }
}

void MainWindow::connectToMembers()
{
    if (m_myOnion.isEmpty() || secret().isEmpty())
        return;
    for (const Member &m : members())
        if (!m_active.contains(m.onion) && !m_attempts.contains(m.onion))
            dial(m.onion);
    updateStatus();
}

void MainWindow::dial(const QString &onion)
{
    auto *s = new QTcpSocket;
    s->setProxy(QNetworkProxy(QNetworkProxy::Socks5Proxy, "127.0.0.1", m_tor->socksPort()));
    auto *c = new PeerConnection(s, true, secret(), nickname(), m_myOnion, identitySeed(), this);
    m_attempts.insert(onion, c);
    watch(c);
    s->connectToHost(onion, TorService::OnionPort); // résolu par Tor, pas localement
}

void MainWindow::watch(PeerConnection *c)
{
    connect(c, &PeerConnection::ready, this, [this, c] { setActive(c); });
    connect(c, &PeerConnection::received, this, [this, c](const QJsonObject &o) { onReceived(c, o); });
    connect(c, &PeerConnection::closed, this, [this, c](const QString &reason) {
        c->deleteLater();
        m_pending.remove(c);
        const QString attemptKey = m_attempts.key(c);
        if (!attemptKey.isEmpty()) {
            m_attempts.remove(attemptKey);
            m_lastError.insert(attemptKey, reason);
            QTimer::singleShot(10000, this, &MainWindow::connectToMembers);
        }
        const QString activeKey = m_active.key(c);
        if (!activeKey.isEmpty()) {
            m_active.remove(activeKey);
            m_lastError.insert(activeKey, reason);
            appendSystem(tr("%1 s'est déconnecté(e) : %2").arg(memberNick(activeKey), reason));
            QTimer::singleShot(5000, this, &MainWindow::connectToMembers);
        }
        updateStatus();
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

    // Deux membres peuvent s'appeler en même temps. Règle identique des deux côtés :
    // on garde la connexion ouverte par l'adresse onion la plus petite.
    const bool wasOnline = m_active.contains(onion);
    if (PeerConnection *old = m_active.value(onion); old && old != c) {
        auto initiator = [this](PeerConnection *p) { return p->isOutgoing() ? m_myOnion : p->peerOnion(); };
        const QString keep = initiator(old), other = initiator(c);
        if (keep != other && keep < other) {
            c->close();
            return;
        }
        m_active.insert(onion, c); // avant close() : le signal closed de l'ancienne ne compte pas
        old->close();
    }
    m_active.insert(onion, c);
    m_lastError.remove(onion);

    const bool isNew = mergeMember(onion, c->peerNickname());
    if (isNew)
        appendSystem(tr("%1 a rejoint la conversation.").arg(c->peerNickname()));
    else if (!wasOnline)
        appendSystem(tr("%1 est en ligne.").arg(c->peerNickname()));

    // Échange des listes de membres : chacun découvre ceux qu'il ne connaît pas encore
    c->send(membersMessage());
    if (isNew)
        broadcast(membersMessage(), c);
    c->send(inventoryMessage()); // après la liste des membres : les clés des auteurs sont connues
    updateStatus();
}

// ---------------------------------------------------------------- Synchronisation

qint64 MainWindow::firstSeen(const QString &onion) const
{
    const QJsonObject seen = QJsonDocument::fromJson(m_settings.value("firstSeen").toByteArray()).object();
    return qint64(seen.value(onion).toDouble()); // 0 : membre connu avant cette fonctionnalité
}

qint64 MainWindow::syncCutoff() const
{
    return qMax(QDateTime::currentMSecsSinceEpoch() - SyncWindowMs,
                m_settings.value("history/clearedBefore").toLongLong());
}

QJsonObject MainWindow::inventoryMessage() const
{
    const qint64 cutoff = syncCutoff();
    QList<QPair<qint64, QString>> recent;
    for (auto it = m_store.cbegin(); it != m_store.cend(); ++it) {
        const qint64 ts = qint64(it.value().value("ts").toDouble());
        if (ts >= cutoff)
            recent.append({ts, it.key()});
    }
    std::sort(recent.begin(), recent.end(), std::greater<>());
    QJsonArray ids;
    for (int i = 0; i < qMin(int(recent.size()), MaxInventory); ++i)
        ids.append(recent[i].second);
    return {{"t", "inv"}, {"ids", ids}};
}

void MainWindow::sendMissing(PeerConnection *c, const QJsonArray &theirIds)
{
    QSet<QString> theirs;
    for (const QJsonValue &v : theirIds)
        theirs.insert(v.toString());
    QList<QJsonObject> missing;
    for (auto it = m_store.cbegin(); it != m_store.cend(); ++it)
        if (!theirs.contains(it.key()))
            missing.append(it.value());
    sendBatches(c, missing);
}

void MainWindow::sendBatches(PeerConnection *c, QList<QJsonObject> messages)
{
    // Un nouveau membre ne reçoit pas ce qui s'est dit avant son arrivée
    const qint64 cutoff = qMax(syncCutoff(), firstSeen(c->peerOnion()) - NewMemberMarginMs);
    messages.removeIf([cutoff](const QJsonObject &m) { return qint64(m.value("ts").toDouble()) < cutoff; });
    std::sort(messages.begin(), messages.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return a.value("ts").toDouble() < b.value("ts").toDouble();
    });

    QJsonArray batch;
    qsizetype bytes = 0;
    auto flush = [&] {
        if (!batch.isEmpty())
            c->send({{"t", "sync"}, {"m", batch}});
        batch = QJsonArray();
        bytes = 0;
    };
    for (QJsonObject m : std::as_const(messages)) {
        m.remove("mine");
        const qsizetype size = QJsonDocument(m).toJson(QJsonDocument::Compact).size();
        if (bytes + size > MaxSyncBatchBytes)
            flush();
        batch.append(m);
        bytes += size;
    }
    flush();
}

void MainWindow::receiveSync(PeerConnection *from, const QJsonArray &messages)
{
    QList<QJsonObject> accepted;
    const qint64 cutoff = syncCutoff();
    int received = 0;
    QString lastAuthor, lastText;
    for (const QJsonValue &v : messages) {
        QJsonObject m = v.toObject();
        const QString id = m.value("id").toString();
        if (id.isEmpty() || m_store.contains(id) || qint64(m.value("ts").toDouble()) < cutoff)
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
    if (received == 0)
        return;
    if (!isVisible() || !isActiveWindow()) {
        m_unread += received;
        updateUnread();
        if (received == 1)
            m_notifier->notify(lastAuthor, lastText);
        else
            m_notifier->notify(tr("P2P Chat"), tr("%n message(s) reçu(s) pendant votre absence", nullptr, received));
    }
}

void MainWindow::onReceived(PeerConnection *c, const QJsonObject &o)
{
    const QString type = o.value("t").toString();
    if (type == "members") {
        bool changed = false;
        for (const QJsonValue &v : o.value("m").toArray()) {
            const QJsonObject m = v.toObject();
            const QString onion = m["o"].toString();
            if (onion == m_myOnion)
                continue;
            // Un membre présenté sans clé, ou avec une clé différente de celle connue, est ignoré
            if (!pinKey(onion, fromB64(m["k"]))) {
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
    } else if (type == "inv") {
        sendMissing(c, o.value("ids").toArray());
    } else if (type == "sync") {
        receiveSync(c, o.value("m").toArray());
    } else if (type == "msg") {
        const QString id = o.value("id").toString();
        if (id.isEmpty() || m_seenIds.contains(id) || m_store.contains(id))
            return;
        if (m_seenIds.size() > MaxSeenIds)
            m_seenIds.clear();
        m_seenIds.insert(id);

        const QString author = o.value("n").toString().left(40);
        if (verifyMessage(o, false) != ChatEntry::Verified) {
            appendSystem(tr("⚠ Message attribué à « %1 » rejeté : signature invalide.").arg(author));
            return;
        }
        appendMessage(o, false, ChatEntry::Verified);

        // Relais : au cas où un membre ne serait pas encore relié directement à l'auteur
        broadcast(o, c);

        if (!isVisible() || !isActiveWindow()) {
            ++m_unread;
            updateUnread();
            m_notifier->notify(author, o.value("x").toString());
            if (isVisible())
                QApplication::alert(this);
        }
    }
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
    for (PeerConnection *c : all)
        c->close();
}

void MainWindow::sendCurrent()
{
    const QString text = m_input->text().trimmed();
    if (text.isEmpty() || secret().isEmpty() || members().isEmpty() || m_myOnion.isEmpty())
        return; // sans personne en ligne, le message partira à la prochaine synchronisation
    const QJsonObject msg = signedMessage(text);
    m_seenIds.insert(msg.value("id").toString());
    broadcast(msg);
    appendMessage(msg, true, ChatEntry::Verified);
    m_input->clear();
}

// ---------------------------------------------------------------- Réglages

QString MainWindow::nickname() const { return m_settings.value("nickname").toString(); }
QByteArray MainWindow::secret() const { return m_settings.value("group/secret").toByteArray(); }

void MainWindow::changeNickname()
{
    bool ok = false;
    const QString n = QInputDialog::getText(this, tr("Pseudo"), tr("Votre pseudo :"), QLineEdit::Normal,
                                            nickname(), &ok).trimmed();
    if (!ok || n.isEmpty())
        return;
    m_settings.setValue("nickname", n.left(40));
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
    m_settings.remove("group");
    m_lastError.clear();
    appendSystem(tr("Vous avez quitté le groupe."));
    updateStatus();
}
