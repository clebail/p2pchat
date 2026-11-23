#include "mainwindow.h"

#include "chatview.h"
#include "crypto.h"
#include "invitation.h"
#include "notifier.h"
#include "peerconnection.h"
#include "torservice.h"

#include <QApplication>
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

constexpr int MaxSeenIds = 5000;

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);
    m_historyPath = dataDir + "/history.jsonl";

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
        tips << QStringLiteral("%1 : %2").arg(m.nick, state);
    }
    m_subtitle->setToolTip(tips.join('\n'));

    const bool online = !m_active.isEmpty();
    m_input->setEnabled(online);
    m_sendButton->setEnabled(online);
    m_input->setPlaceholderText(online ? tr("Message") : tr("Personne n'est en ligne"));
    if (online && !m_input->hasFocus())
        m_input->setFocus();
}

void MainWindow::appendMessage(const QString &who, const QString &text, const QDateTime &ts, bool mine, bool save)
{
    m_chat->addMessage(who, text, ts, mine);
    if (!save)
        return;
    QFile file(m_historyPath);
    if (file.open(QIODevice::Append)) {
        const QJsonObject o{{"ts", ts.toString(Qt::ISODate)}, {"from", who}, {"text", text}, {"mine", mine}};
        file.write(QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n');
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
    const QList<QByteArray> lines = file.readAll().split('\n');
    for (qsizetype i = qMax<qsizetype>(0, lines.size() - 500); i < lines.size(); ++i) {
        const QJsonObject o = QJsonDocument::fromJson(lines.at(i)).object();
        if (!o.isEmpty())
            appendMessage(o["from"].toString(), o["text"].toString(),
                          QDateTime::fromString(o["ts"].toString(), Qt::ISODate), o["mine"].toBool(), false);
    }
    m_chat->scrollToBottom();
}

void MainWindow::clearConversation()
{
    if (QMessageBox::question(this, tr("Vider la conversation"),
                              tr("Effacer tous les messages de cette fenêtre ?\n"
                                 "Ils sont supprimés uniquement sur cet ordinateur."))
        != QMessageBox::Yes)
        return;
    m_chat->clearAll();
    QFile::remove(m_historyPath);
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
    const QString code = Invitation{secret(), m_myOnion, nickname()}.toCode();

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
                                  tr("Vous faites déjà partie d'une autre conversation. "
                                     "La quitter pour rejoindre celle de %1 ?").arg(inv->nickname))
            != QMessageBox::Yes)
            return;
        closeAll();
        saveMembers({});
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
    arr.append(QJsonObject{{"o", m_myOnion}, {"n", nickname()}});
    for (const Member &m : members())
        arr.append(QJsonObject{{"o", m.onion}, {"n", m.nick}});
    return {{"t", "members"}, {"m", arr}};
}

// ---------------------------------------------------------------- Connexions

void MainWindow::onNewConnection()
{
    while (QTcpSocket *s = m_server->nextPendingConnection()) {
        if (secret().isEmpty() || m_myOnion.isEmpty()) {
            s->abort();
            s->deleteLater();
            continue;
        }
        auto *c = new PeerConnection(s, false, secret(), nickname(), m_myOnion, this);
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
    auto *c = new PeerConnection(s, true, secret(), nickname(), m_myOnion, this);
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
    updateStatus();
}

void MainWindow::onReceived(PeerConnection *c, const QJsonObject &o)
{
    const QString type = o.value("t").toString();
    if (type == "members") {
        bool changed = false;
        for (const QJsonValue &v : o.value("m").toArray()) {
            const QJsonObject m = v.toObject();
            if (mergeMember(m["o"].toString(), m["n"].toString())) {
                changed = true;
                appendSystem(tr("%1 fait partie de la conversation.").arg(m["n"].toString()));
            }
        }
        if (changed) {
            broadcast(membersMessage(), c);
            connectToMembers();
        }
        updateStatus(); // un pseudo a pu changer
    } else if (type == "msg") {
        const QString id = o.value("id").toString();
        if (id.isEmpty() || m_seenIds.contains(id))
            return;
        if (m_seenIds.size() > MaxSeenIds)
            m_seenIds.clear();
        m_seenIds.insert(id);

        const qint64 ms = qint64(o.value("ts").toDouble());
        const QDateTime ts = ms > 0 ? QDateTime::fromMSecsSinceEpoch(ms) : QDateTime::currentDateTime();
        const QString author = o.value("n").toString().left(40);
        appendMessage(author.isEmpty() ? c->peerNickname() : author, o.value("x").toString(), ts, false);

        // Relais : au cas où un membre ne serait pas encore relié directement à l'auteur
        broadcast(o, c);

        if (!isVisible() || !isActiveWindow()) {
            ++m_unread;
            updateUnread();
            m_notifier->notify(author.isEmpty() ? c->peerNickname() : author, o.value("x").toString());
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
    if (text.isEmpty() || m_active.isEmpty())
        return;
    const QString id = QString::fromLatin1(Crypto::randomBytes(12).toHex());
    m_seenIds.insert(id);
    broadcast({{"t", "msg"},
               {"id", id},
               {"a", m_myOnion},
               {"n", nickname()},
               {"x", text},
               {"ts", double(QDateTime::currentMSecsSinceEpoch())}});
    appendMessage(nickname(), text, QDateTime::currentDateTime(), true);
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
