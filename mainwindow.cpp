#include "mainwindow.h"

#include "crypto.h"
#include "invitation.h"
#include "peerconnection.h"
#include "torservice.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkProxy>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextBrowser>
#include <QToolBar>
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

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);
    m_historyPath = dataDir + "/history.jsonl";

    m_server = new QTcpServer(this);
    m_server->setProxy(QNetworkProxy::NoProxy);
    connect(m_server, &QTcpServer::newConnection, this, &MainWindow::onNewConnection);
    if (!m_server->listen(QHostAddress::LocalHost, 0))
        QMessageBox::critical(this, tr("P2P Chat"), tr("Écoute locale impossible : %1").arg(m_server->errorString()));

    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, &MainWindow::connectToPeer);

    buildUi();
    loadHistory();
    startTor();
}

// ---------------------------------------------------------------- Interface

void MainWindow::buildUi()
{
    setWindowTitle(tr("P2P Chat"));
    resize(560, 640);

    QToolBar *tb = addToolBar(tr("Actions"));
    tb->setMovable(false);
    tb->setToolButtonStyle(Qt::ToolButtonTextOnly);
    tb->addAction(tr("Inviter"), this, &MainWindow::invite)
        ->setToolTip(tr("Créer le code à envoyer à votre correspondant"));
    tb->addAction(tr("Rejoindre"), this, &MainWindow::join)->setToolTip(tr("Coller le code reçu"));
    tb->addSeparator();
    tb->addAction(tr("Pseudo…"), this, &MainWindow::changeNickname);
    tb->addAction(tr("Réinitialiser"), this, &MainWindow::resetAll)
        ->setToolTip(tr("Oublier le correspondant et le code secret"));

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);

    m_status = new QLabel(central);
    m_status->setTextFormat(Qt::RichText);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    m_history = new QTextBrowser(central);
    m_history->setOpenExternalLinks(true);
    QFont f = m_history->font();
    f.setPointSizeF(f.pointSizeF() * 1.15);
    m_history->setFont(f);
    layout->addWidget(m_history, 1);

    auto *row = new QHBoxLayout;
    m_input = new QLineEdit(central);
    m_input->setMaxLength(4000);
    m_input->setPlaceholderText(tr("Écrire un message…"));
    m_input->setFont(f);
    m_sendButton = new QPushButton(tr("Envoyer"), central);
    row->addWidget(m_input, 1);
    row->addWidget(m_sendButton);
    layout->addLayout(row);
    setCentralWidget(central);

    connect(m_input, &QLineEdit::returnPressed, this, &MainWindow::sendCurrent);
    connect(m_sendButton, &QPushButton::clicked, this, &MainWindow::sendCurrent);
}

void MainWindow::updateStatus()
{
    const QString orange = "<span style='color:#e37400'>●</span> ";
    QString text;
    if (m_active) {
        text = tr("<span style='color:#188038'>●</span> Connecté avec <b>%1</b>")
                   .arg(m_active->peerNickname().toHtmlEscaped());
    } else if (!m_torError.isEmpty()) {
        text = tr("<span style='color:#c5221f'>●</span> Tor : %1").arg(m_torError.toHtmlEscaped());
    } else if (m_myOnion.isEmpty()) {
        text = orange + tr("Démarrage de Tor… %1").arg(m_torStatus.toHtmlEscaped());
    } else if (secret().isEmpty()) {
        text = tr("<span style='color:gray'>●</span> Cliquez sur <b>Inviter</b> pour créer un code à envoyer, "
                  "ou sur <b>Rejoindre</b> si on vous en a envoyé un.");
    } else if (peerOnion().isEmpty()) {
        text = orange + tr("Invitation envoyée : en attente de la première connexion de votre correspondant…");
    } else if (m_attempt) {
        text = orange + tr("Connexion à <b>%1</b>…").arg(peerNick().toHtmlEscaped());
    } else {
        text = orange + tr("<b>%1</b> n'est pas en ligne. La connexion se fera toute seule dès que "
                           "son programme sera ouvert.").arg(peerNick().toHtmlEscaped());
        if (!m_lastError.isEmpty())
            text += tr(" <span style='color:gray'>(%1)</span>").arg(m_lastError.toHtmlEscaped());
    }
    m_status->setText(text);
    m_input->setEnabled(m_active);
    m_sendButton->setEnabled(m_active);
    if (m_active)
        m_input->setFocus();
}

void MainWindow::appendMessage(const QString &who, const QString &text, const QDateTime &ts, bool mine, bool save)
{
    QString body = text.toHtmlEscaped();
    body.replace('\n', "<br>");
    m_history->append(QStringLiteral("<span style='color:gray'>%1</span> <b style='color:%2'>%3</b> : %4")
                          .arg(ts.toString("dd/MM HH:mm"), mine ? "#1a73e8" : "#c5221f", who.toHtmlEscaped(), body));
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
    m_history->append(QStringLiteral("<i style='color:gray'>— %1</i>").arg(text.toHtmlEscaped()));
}

void MainWindow::loadHistory()
{
    QFile file(m_historyPath);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QList<QByteArray> lines = file.readAll().split('\n');
    for (qsizetype i = qMax<qsizetype>(0, lines.size() - 300); i < lines.size(); ++i) {
        const QJsonObject o = QJsonDocument::fromJson(lines.at(i)).object();
        if (!o.isEmpty())
            appendMessage(o["from"].toString(), o["text"].toString(),
                          QDateTime::fromString(o["ts"].toString(), Qt::ISODate), o["mine"].toBool(), false);
    }
}

void MainWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::ActivationChange && isActiveWindow()) {
        m_unread = 0;
        setWindowTitle(tr("P2P Chat"));
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
        connectToPeer();
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
        m_settings.setValue("contact/secret", Crypto::randomBytes(16));
    if (m_myOnion.isEmpty()) {
        m_inviteWanted = true;
        appendSystem(tr("Le code s'affichera dès que Tor aura démarré…"));
        return;
    }
    showInvitation();
}

void MainWindow::showInvitation()
{
    const QString code = Invitation{secret(), m_myOnion, nickname()}.toCode();

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Code d'invitation"));
    auto *lay = new QVBoxLayout(&dlg);
    auto *intro = new QLabel(tr("Envoyez ce code <b>une seule fois</b> à votre correspondant (mail, SMS…). "
                                "Il installe P2P Chat, clique sur <b>Rejoindre</b> et colle le code.<br>"
                                "Ensuite vous n'aurez plus rien à faire : il suffit que vos deux programmes soient "
                                "ouverts.<br><i>Ce code sert aussi de mot de passe : ne le publiez pas.</i>"), &dlg);
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
    if (!peerOnion().isEmpty() && peerOnion() != inv->onion
        && QMessageBox::question(this, tr("Rejoindre"),
                                 tr("Remplacer votre correspondant actuel (%1) par %2 ?").arg(peerNick(), inv->nickname))
               != QMessageBox::Yes)
        return;

    closeAll();
    m_settings.setValue("contact/secret", inv->secret);
    m_settings.setValue("contact/onion", inv->onion);
    m_settings.setValue("contact/nick", inv->nickname);
    m_lastError.clear();
    appendSystem(tr("Invitation de %1 enregistrée.").arg(inv->nickname));
    connectToPeer();
    updateStatus();
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
        watch(new PeerConnection(s, false, secret(), nickname(), m_myOnion, this));
    }
}

void MainWindow::connectToPeer()
{
    if (m_active || m_attempt || m_myOnion.isEmpty() || secret().isEmpty() || peerOnion().isEmpty())
        return;
    auto *s = new QTcpSocket;
    s->setProxy(QNetworkProxy(QNetworkProxy::Socks5Proxy, "127.0.0.1", m_tor->socksPort()));
    m_attempt = new PeerConnection(s, true, secret(), nickname(), m_myOnion, this);
    watch(m_attempt);
    s->connectToHost(peerOnion(), TorService::OnionPort); // résolu par Tor, pas localement
    updateStatus();
}

void MainWindow::watch(PeerConnection *c)
{
    connect(c, &PeerConnection::ready, this, [this, c] { setActive(c); });
    connect(c, &PeerConnection::messageReceived, this, [this, c](const QString &text, const QDateTime &ts) {
        if (c != m_active)
            return;
        appendMessage(c->peerNickname(), text, ts, false);
        if (!isActiveWindow()) {
            ++m_unread;
            setWindowTitle(tr("(%1) P2P Chat").arg(m_unread));
            QApplication::alert(this);
        }
    });
    connect(c, &PeerConnection::closed, this, [this, c](const QString &reason) {
        c->deleteLater();
        if (c == m_attempt) {
            m_attempt = nullptr;
            if (!m_active) {
                m_lastError = reason;
                m_retryTimer.start(30000);
            }
        } else if (c == m_active) {
            m_active = nullptr;
            m_lastError = reason;
            appendSystem(tr("Déconnecté : %1").arg(reason));
            m_retryTimer.start(5000);
        }
        updateStatus();
    });
}

void MainWindow::setActive(PeerConnection *c)
{
    if (c == m_attempt)
        m_attempt = nullptr;

    // Les deux côtés peuvent s'appeler en même temps. Règle identique des deux côtés :
    // on garde la connexion ouverte par l'adresse onion la plus petite.
    if (m_active && m_active != c) {
        auto initiator = [this](PeerConnection *p) { return p->isOutgoing() ? m_myOnion : p->peerOnion(); };
        const QString keep = initiator(m_active), other = initiator(c);
        if (keep != other && keep < other) {
            c->close();
            return;
        }
        PeerConnection *old = m_active;
        m_active = c; // avant close() pour que le signal closed ne compte pas comme une déconnexion
        old->close();
    }
    m_active = c;
    m_retryTimer.stop();
    m_lastError.clear();

    // Premier contact côté invitant : on apprend l'adresse du correspondant
    if (isValidOnion(c->peerOnion()) && c->peerOnion() != m_myOnion) {
        if (peerOnion().isEmpty())
            appendSystem(tr("%1 a accepté l'invitation.").arg(c->peerNickname()));
        m_settings.setValue("contact/onion", c->peerOnion());
    }
    m_settings.setValue("contact/nick", c->peerNickname());
    if (m_attempt) {
        PeerConnection *a = m_attempt;
        m_attempt = nullptr;
        a->close();
    }
    appendSystem(tr("Connecté avec %1.").arg(c->peerNickname()));
    updateStatus();
}

void MainWindow::closeAll()
{
    m_retryTimer.stop();
    PeerConnection *a = m_active, *b = m_attempt;
    m_active = m_attempt = nullptr;
    if (a)
        a->close();
    if (b)
        b->close();
    m_retryTimer.stop();
}

void MainWindow::sendCurrent()
{
    const QString text = m_input->text().trimmed();
    if (text.isEmpty() || !m_active)
        return;
    m_active->sendChat(text);
    appendMessage(nickname(), text, QDateTime::currentDateTime(), true);
    m_input->clear();
}

// ---------------------------------------------------------------- Réglages

QString MainWindow::nickname() const { return m_settings.value("nickname").toString(); }
QByteArray MainWindow::secret() const { return m_settings.value("contact/secret").toByteArray(); }
QString MainWindow::peerOnion() const { return m_settings.value("contact/onion").toString(); }
QString MainWindow::peerNick() const
{
    const QString n = m_settings.value("contact/nick").toString();
    return n.isEmpty() ? tr("Votre correspondant") : n;
}

void MainWindow::changeNickname()
{
    bool ok = false;
    const QString n = QInputDialog::getText(this, tr("Pseudo"), tr("Votre pseudo :"), QLineEdit::Normal,
                                            nickname(), &ok).trimmed();
    if (ok && !n.isEmpty())
        m_settings.setValue("nickname", n.left(40));
}

void MainWindow::resetAll()
{
    if (QMessageBox::question(this, tr("Réinitialiser"),
                              tr("Oublier le correspondant et le code secret ?\n"
                                 "Il faudra échanger un nouveau code. L'historique est conservé."))
        != QMessageBox::Yes)
        return;
    closeAll();
    m_settings.remove("contact");
    m_lastError.clear();
    appendSystem(tr("Réinitialisé."));
    updateStatus();
}
