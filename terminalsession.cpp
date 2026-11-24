#include "terminalsession.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QUuid>
#include <QtEndian>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <memory>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#ifdef Q_OS_MACOS
#  include <util.h> // forkpty
#else
#  include <pty.h>
#  include <sys/syscall.h>
#endif

extern char **environ;

namespace {

constexpr int ChunkBytes = 32 * 1024;         // affichage envoyé par message (base64 : ~43 Ko < trame max)
constexpr qint64 BacklogHigh = 256 * 1024;    // au-delà, on cesse de lire le shell…
constexpr qint64 BacklogLow = 64 * 1024;      // …jusqu'à ce que Tor ait écoulé le retard
constexpr int MaxLocalPending = 1024 * 1024;  // affichage gardé en attendant la fenêtre locale

#ifdef Q_OS_MACOS

QString shellQuote(const QString &s)
{
    QString q = s;
    q.replace('\'', QLatin1String("'\\''"));
    return '\'' + q + '\'';
}

// Fenêtre de terminal qui exécute `P2PChat --terminal <socket> <titre>`.
// Terminal.app n'accepte pas de commande en argument : on lui fait ouvrir un petit script
// .command (pas d'AppleScript, qui demanderait l'autorisation de piloter Terminal).
bool launchTerminal(const QStringList &clientArgs, QString *error)
{
    QString command = shellQuote(QCoreApplication::applicationFilePath());
    for (const QString &a : clientArgs)
        command += ' ' + shellQuote(a);
    const QString path = QDir::tempPath() + QStringLiteral("/p2pchat-%1.command")
                                                .arg(QString::fromLatin1(QUuid::createUuid().toRfc4122().toHex().left(16)));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QObject::tr("script de lancement impossible : %1").arg(f.errorString());
        return false;
    }
    f.write("#!/bin/sh\nrm -f \"$0\"\nclear\nexec " + command.toUtf8() + "\n");
    f.close();
    f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

    // P2PCHAT_TERMINAL : nom d'application (« iTerm », « Ghostty »…) ; sinon iTerm2 s'il est installé
    QStringList apps;
    const QString forced = qEnvironmentVariable("P2PCHAT_TERMINAL");
    if (!forced.isEmpty())
        apps << forced;
    if (QFileInfo::exists("/Applications/iTerm.app"))
        apps << "iTerm";
    apps << "Terminal";
    for (const QString &app : std::as_const(apps))
        if (QProcess::execute("/usr/bin/open", {"-a", app, path}) == 0)
            return true;
    QFile::remove(path);
    if (error)
        *error = QObject::tr("impossible d'ouvrir le Terminal");
    return false;
}

#else

// Fenêtre de terminal qui exécute `p2pchat --terminal <socket> <titre>`
bool launchTerminal(const QStringList &clientArgs, QString *error)
{
    const QString self = QCoreApplication::applicationFilePath();
    struct Candidate
    {
        QString exe;
        QStringList before; // arguments avant la commande
    };
    QList<Candidate> candidates;
    // P2PCHAT_TERMINAL : terminal imposé, appelé avec « -e commande… »
    const QString forced = qEnvironmentVariable("P2PCHAT_TERMINAL");
    if (!forced.isEmpty())
        candidates.append({forced, {"-e"}});
    candidates += {{"konsole", {"-e"}},
                   {"gnome-terminal", {"--"}},
                   {"xfce4-terminal", {"-x"}},
                   {"kitty", {}},
                   {"alacritty", {"-e"}},
                   {"foot", {}},
                   {"wezterm", {"start", "--"}},
                   {"x-terminal-emulator", {"-e"}},
                   {"xterm", {"-e"}}};
    for (const Candidate &c : std::as_const(candidates)) {
        const QString path = QStandardPaths::findExecutable(c.exe);
        if (path.isEmpty())
            continue;
        if (QProcess::startDetached(path, c.before + QStringList{self} + clientArgs))
            return true;
    }
    if (error)
        *error = QObject::tr("aucun terminal trouvé (konsole, gnome-terminal, xterm…) ; "
                             "la variable P2PCHAT_TERMINAL permet d'en indiquer un");
    return false;
}

#endif

QByteArray frame(char type, const QByteArray &payload)
{
    QByteArray f(5, Qt::Uninitialized);
    f[0] = type;
    qToBigEndian<quint32>(quint32(payload.size()), f.data() + 1);
    return f + payload;
}

QByteArray b64(const QByteArray &b)
{
    return b.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

} // namespace

TerminalSession::TerminalSession(Role role, const QString &sid, const QString &peerOnion, const QString &peerNick,
                                 bool control, QObject *parent)
    : QObject(parent), m_role(role), m_sid(sid), m_peerOnion(peerOnion), m_peerNick(peerNick), m_control(control)
{
    m_connectTimeout.setSingleShot(true);
    m_connectTimeout.setInterval(20000);
    connect(&m_connectTimeout, &QTimer::timeout, this,
            [this] { end(tr("la fenêtre de terminal ne s'est pas ouverte")); });

    m_flushTimer.setSingleShot(true);
    m_flushTimer.setInterval(20); // regroupe les petites écritures du shell
    connect(&m_flushTimer, &QTimer::timeout, this, &TerminalSession::flushRemote);

    m_backpressureTimer.setInterval(100);
    connect(&m_backpressureTimer, &QTimer::timeout, this, [this] {
        if (!m_backlog || m_backlog() < BacklogLow) {
            m_backpressureTimer.stop();
            if (m_readNotifier)
                m_readNotifier->setEnabled(true);
        }
    });
}

TerminalSession::~TerminalSession()
{
    blockSignals(true); // pas de signaux pendant la destruction
    end(tr("session fermée"), false);
}

bool TerminalSession::start(QString *error)
{
#ifdef Q_OS_MACOS
    QString dir = QDir::tempPath(); // $TMPDIR : propre à l'utilisateur
#else
    QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (dir.isEmpty())
        dir = QDir::tempPath();
#endif
    // Nom aléatoire propre à cette session (pas le sid, connu de l'autre membre). Court :
    // le chemin d'un socket Unix est limité (104 octets sous macOS, 108 sous Linux).
    const QString name = QStringLiteral("p2pchat-%1.sock")
                             .arg(QString::fromLatin1(QUuid::createUuid().toRfc4122().toHex().left(16)));
    QString path = dir + '/' + name;
    if (QFile::encodeName(path).size() >= 100)
        path = QStringLiteral("/tmp/") + name;
    QLocalServer::removeServer(path);
    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption); // lisible par cet utilisateur seulement
    if (!m_server->listen(path)) {
        if (error)
            *error = tr("socket local impossible : %1").arg(m_server->errorString());
        return false;
    }
    connect(m_server, &QLocalServer::newConnection, this, &TerminalSession::onLocalConnection);

    if (m_role == Host && !spawnShell(error))
        return false;

    const QString title = m_role == Host ? tr("Console partagée avec %1").arg(m_peerNick)
                                         : tr("Console de %1").arg(m_peerNick);
    if (!launchTerminal({"--terminal", m_server->fullServerName(), title}, error))
        return false;
    m_connectTimeout.start();
    return true;
}

void TerminalSession::onLocalConnection()
{
    while (QLocalSocket *s = m_server->nextPendingConnection()) {
        if (m_local || m_ended) { // une seule fenêtre par session
            s->abort();
            s->deleteLater();
            continue;
        }
        m_local = s;
        m_connectTimeout.stop();
        connect(s, &QLocalSocket::readyRead, this, &TerminalSession::onLocalReadyRead);
        connect(s, &QLocalSocket::disconnected, this, [this] {
            end(tr("vous avez fermé la console"));
        });
        updateTitle();
        if (m_role == Host)
            sendLocal('M', (m_control ? tr("Console partagée avec %1, qui peut taper des commandes.")
                                      : tr("Console partagée avec %1, en lecture seule."))
                               .arg(m_peerNick)
                               .toUtf8()
                           + tr(" Tapez « exit » pour arrêter.").toUtf8());
        else
            sendLocal('M', (m_control ? tr("Console de %1 : vous pouvez taper des commandes.")
                                      : tr("Console de %1 : lecture seule."))
                               .arg(m_peerNick)
                               .toUtf8());
        if (!m_localPending.isEmpty()) {
            sendLocal('D', m_localPending);
            m_localPending.clear();
        }
    }
}

void TerminalSession::onLocalReadyRead()
{
    m_localIn += m_local->readAll();
    while (m_localIn.size() >= 5) {
        const quint32 len = qFromBigEndian<quint32>(m_localIn.constData() + 1);
        if (len > 1024 * 1024) {
            end(tr("trame locale invalide"));
            return;
        }
        if (quint32(m_localIn.size()) < 5 + len)
            break;
        const char type = m_localIn.at(0);
        const QByteArray payload = m_localIn.mid(5, int(len));
        m_localIn.remove(0, int(5 + len));

        if (type == 'D') {
            if (m_role == Host) {
                writePty(payload);
            } else if (m_control) {
                emit sendToPeer({{"t", "term-data"}, {"sid", m_sid}, {"d", QString::fromLatin1(b64(payload))}});
            } else {
                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                if (now - m_lastReadOnlyNotice > 5000) {
                    m_lastReadOnlyNotice = now;
                    sendLocal('M', tr("Lecture seule : %1 ne vous a pas donné la main.").arg(m_peerNick).toUtf8());
                }
            }
        } else if (type == 'R' && payload.size() == 4) {
            const int cols = qFromBigEndian<quint16>(payload.constData());
            const int rows = qFromBigEndian<quint16>(payload.constData() + 2);
            if (m_role == Host) {
                m_localCols = cols;
                m_localRows = rows;
                applySize();
            } else {
                emit sendToPeer({{"t", "term-resize"}, {"sid", m_sid}, {"c", cols}, {"r", rows}});
            }
        }
    }
}

void TerminalSession::sendLocal(char type, const QByteArray &payload)
{
    if (m_local && m_local->state() == QLocalSocket::ConnectedState) {
        m_local->write(frame(type, payload));
    } else if (type == 'D' && m_localPending.size() < MaxLocalPending) {
        m_localPending += payload;
    }
}

void TerminalSession::updateTitle()
{
    QString title;
    if (m_role == Host)
        title = (m_control ? tr("Console partagée avec %1 — %1 peut taper")
                           : tr("Console partagée avec %1 — lecture seule pour %1"))
                    .arg(m_peerNick);
    else
        title = tr("Console de %1 — %2").arg(m_peerNick, m_control ? tr("contrôle") : tr("lecture seule"));
    sendLocal('T', title.toUtf8());
}

void TerminalSession::setControl(bool control)
{
    if (m_ended || control == m_control)
        return;
    m_control = control;
    updateTitle();
    if (m_role == Host) {
        emit sendToPeer({{"t", "term-ctl"}, {"sid", m_sid}, {"ctl", control}});
        sendLocal('M', (control ? tr("%1 peut maintenant taper des commandes.")
                                : tr("%1 ne peut plus taper de commandes (lecture seule)."))
                           .arg(m_peerNick)
                           .toUtf8());
    } else {
        sendLocal('M', (control ? tr("%1 vous a donné la main.") : tr("%1 vous a retiré la main (lecture seule)."))
                           .arg(m_peerNick)
                           .toUtf8());
    }
}

void TerminalSession::handlePeerMessage(const QJsonObject &o)
{
    if (m_ended)
        return;
    const QString type = o.value("t").toString();
    if (type == "term-data") {
        const QByteArray data = QByteArray::fromBase64(o.value("d").toString().toLatin1(), QByteArray::Base64UrlEncoding);
        if (m_role == Guest)
            sendLocal('D', data);
        else if (m_control) // l'hôte vérifie lui-même : un invité en lecture seule ne tape rien
            writePty(data);
    } else if (type == "term-resize" && m_role == Host) {
        m_remoteCols = qBound(0, o.value("c").toInt(), 1000);
        m_remoteRows = qBound(0, o.value("r").toInt(), 1000);
        applySize();
    } else if (type == "term-ctl" && m_role == Guest) {
        setControl(o.value("ctl").toBool());
    } else if (type == "term-end") {
        end(m_role == Host ? tr("%1 a quitté la console").arg(m_peerNick)
                           : tr("%1 a arrêté le partage").arg(m_peerNick),
            false);
    }
}

void TerminalSession::end(const QString &reason, bool notifyPeer)
{
    if (m_ended)
        return;
    m_ended = true;
    m_connectTimeout.stop();
    m_backpressureTimer.stop();
    if (m_role == Host)
        flushRemote(); // dernier affichage du shell
    m_flushTimer.stop();
    if (notifyPeer)
        emit sendToPeer({{"t", "term-end"}, {"sid", m_sid}});

    if (m_local) {
        m_local->disconnect(this);
        if (m_local->state() == QLocalSocket::ConnectedState) {
            m_local->write(frame('E', tr("Fin du partage : %1.").arg(reason).toUtf8()));
            m_local->flush();
        }
        m_local->disconnectFromServer();
    }
    if (m_server)
        m_server->close();

    delete m_readNotifier;
    m_readNotifier = nullptr;
    delete m_writeNotifier;
    m_writeNotifier = nullptr;
    if (m_master >= 0) {
        ::close(m_master);
        m_master = -1;
    }
    if (m_pid > 0) {
        ::kill(m_pid, SIGHUP);
        reapChild();
    }
    emit finished(reason);
}

// ---------------------------------------------------------------- Hôte : shell dans un pseudo-terminal

bool TerminalSession::spawnShell(QString *error)
{
    // Shell de l'utilisateur ($SHELL, absent si l'application est lancée depuis le Finder)
    QByteArray shell = qgetenv("SHELL");
    if (shell.isEmpty())
        if (const passwd *pw = ::getpwuid(::getuid()); pw && pw->pw_shell)
            shell = pw->pw_shell;
    if (shell.isEmpty() || !QFileInfo(QFile::decodeName(shell)).isExecutable())
        shell = QFileInfo::exists("/bin/bash") ? "/bin/bash" : "/bin/sh";
    QByteArray argv0 = QFileInfo(QFile::decodeName(shell)).fileName().toLocal8Bit();
#ifdef Q_OS_MACOS
    // Comme Terminal.app : shell de connexion, sinon ~/.zprofile (PATH de Homebrew…) n'est pas lu
    argv0.prepend('-');
#endif
    const QByteArray home = QFile::encodeName(QDir::homePath());

    // Environnement préparé avant fork() : l'enfant d'un programme multithread ne doit plus allouer
    QList<QByteArray> env;
    for (char **e = environ; *e; ++e) {
        const QByteArray kv(*e);
        if (!kv.startsWith("TERM=") && !kv.startsWith("P2PCHAT_SHARED_WITH="))
            env << kv;
    }
    env << "TERM=xterm-256color" << "P2PCHAT_SHARED_WITH=" + m_peerNick.toUtf8();
    if (qEnvironmentVariableIsEmpty("LANG")) // application graphique macOS : souvent absent
        env << "LANG=" + QLocale::system().name().toLatin1() + ".UTF-8";
    std::vector<char *> envp;
    for (QByteArray &kv : env)
        envp.push_back(kv.data());
    envp.push_back(nullptr);
    char *argv[] = {argv0.data(), nullptr};

    winsize ws{};
    ws.ws_col = 80;
    ws.ws_row = 24;
    int master = -1;
    const pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) {
        if (error)
            *error = tr("pseudo-terminal impossible : %1").arg(QString::fromLocal8Bit(strerror(errno)));
        return false;
    }
    if (pid == 0) {
        // Enfant : ne garder que le terminal (pas les sockets de P2P Chat ni Tor)
#ifdef SYS_close_range
        if (::syscall(SYS_close_range, 3U, ~0U, 0U) != 0)
#endif
            for (int fd = 3; fd < 4096; ++fd)
                ::close(fd);
        if (::chdir(home.constData()) != 0) {
        }
        ::execve(shell.constData(), argv, envp.data());
        ::_exit(127);
    }

    m_pid = pid;
    m_master = master;
    ::fcntl(master, F_SETFL, ::fcntl(master, F_GETFL) | O_NONBLOCK);
    ::fcntl(master, F_SETFD, FD_CLOEXEC);
    m_readNotifier = new QSocketNotifier(master, QSocketNotifier::Read, this);
    connect(m_readNotifier, &QSocketNotifier::activated, this, &TerminalSession::readPty);
    m_writeNotifier = new QSocketNotifier(master, QSocketNotifier::Write, this);
    m_writeNotifier->setEnabled(false);
    connect(m_writeNotifier, &QSocketNotifier::activated, this, &TerminalSession::flushPtyWrites);
    return true;
}

void TerminalSession::readPty()
{
    char buf[16384];
    for (;;) {
        const ssize_t n = ::read(m_master, buf, sizeof(buf));
        if (n > 0) {
            const QByteArray data(buf, int(n));
            sendLocal('D', data);
            m_toRemote += data;
            if (m_toRemote.size() >= ChunkBytes)
                flushRemote();
            else if (!m_flushTimer.isActive())
                m_flushTimer.start();
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            break;
        // EIO : le shell s'est terminé (plus personne côté esclave)
        end(tr("le shell s'est terminé"));
        return;
    }
    // Tor ne suit pas : on cesse de lire, le shell sera bloqué en écriture (comme en SSH)
    if (m_backlog && m_backlog() > BacklogHigh && m_readNotifier) {
        m_readNotifier->setEnabled(false);
        m_backpressureTimer.start();
    }
}

void TerminalSession::flushRemote()
{
    while (!m_toRemote.isEmpty()) {
        const QByteArray chunk = m_toRemote.left(ChunkBytes);
        m_toRemote.remove(0, chunk.size());
        emit sendToPeer({{"t", "term-data"}, {"sid", m_sid}, {"d", QString::fromLatin1(b64(chunk))}});
    }
}

void TerminalSession::writePty(const QByteArray &data)
{
    if (m_master < 0 || data.isEmpty())
        return;
    if (m_toPty.size() > 1024 * 1024) // shell bloqué : on n'accumule pas sans fin
        return;
    m_toPty += data;
    flushPtyWrites();
}

void TerminalSession::flushPtyWrites()
{
    while (!m_toPty.isEmpty() && m_master >= 0) {
        const ssize_t n = ::write(m_master, m_toPty.constData(), size_t(m_toPty.size()));
        if (n > 0) {
            m_toPty.remove(0, int(n));
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        break; // EAGAIN : on attend que le pseudo-terminal puisse recevoir
    }
    if (m_writeNotifier)
        m_writeNotifier->setEnabled(!m_toPty.isEmpty());
}

void TerminalSession::applySize()
{
    if (m_master < 0)
        return;
    // Un seul shell, donc une seule taille : la plus petite des deux fenêtres
    int cols = m_localCols, rows = m_localRows;
    if (m_remoteCols > 0 && (cols == 0 || m_remoteCols < cols))
        cols = m_remoteCols;
    if (m_remoteRows > 0 && (rows == 0 || m_remoteRows < rows))
        rows = m_remoteRows;
    if (cols <= 0 || rows <= 0)
        return;
    winsize ws{};
    ws.ws_col = ushort(cols);
    ws.ws_row = ushort(rows);
    ::ioctl(m_master, TIOCSWINSZ, &ws); // le noyau prévient le shell (SIGWINCH)
}

void TerminalSession::reapChild()
{
    // Récupère le shell terminé ; s'il ignore SIGHUP, SIGKILL au bout de 3 s
    const int pid = m_pid;
    m_pid = -1;
    auto tries = std::make_shared<int>(0);
    auto *timer = new QTimer(QCoreApplication::instance());
    timer->setInterval(200);
    QObject::connect(timer, &QTimer::timeout, timer, [pid, tries, timer] {
        int status = 0;
        const pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid || (r < 0 && errno == ECHILD)) {
            timer->deleteLater();
            return;
        }
        if (++*tries == 15)
            ::kill(pid, SIGKILL);
    });
    timer->start();
}
