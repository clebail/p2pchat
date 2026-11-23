#include "torservice.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTcpServer>

namespace {

quint16 freeLocalPort()
{
    QTcpServer s;
    s.listen(QHostAddress::LocalHost, 0);
    return s.serverPort();
}

// Chemin pour le torrc : barres obliques (acceptées partout) et guillemets (espaces)
QString torPath(const QString &p)
{
    QString s = QDir::fromNativeSeparators(QDir(p).absolutePath());
    s.replace('\\', "\\\\").replace('"', "\\\"");
    return '"' + s + '"';
}

void makePrivateDir(const QString &path)
{
    QDir().mkpath(path);
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

} // namespace

TorService::TorService(const QString &dataDir, quint16 localPort, QObject *parent)
    : QObject(parent), m_dataDir(dataDir), m_localPort(localPort)
{
    m_process.setProcessChannelMode(QProcess::MergedChannels);
    connect(&m_process, &QProcess::readyRead, this, &TorService::onOutput);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            fail(tr("impossible de lancer tor (%1)").arg(m_process.program()));
    });
    connect(&m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this] {
        fail(m_lastProblem.isEmpty() ? tr("tor s'est arrêté") : tr("tor s'est arrêté : %1").arg(m_lastProblem));
    });
}

TorService::~TorService()
{
    if (m_process.state() != QProcess::NotRunning) {
        m_process.disconnect(this);
        m_process.terminate();
        if (!m_process.waitForFinished(3000))
            m_process.kill();
        m_process.waitForFinished(1000);
    }
}

QString TorService::findExecutable()
{
    // 1. Chemin forcé dans la configuration
    const QString forced = QSettings().value("torPath").toString();
    if (!forced.isEmpty() && QFileInfo(forced).isExecutable())
        return forced;

#ifdef Q_OS_WIN
    const QString exe = "tor.exe";
#else
    const QString exe = "tor";
#endif
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString home = QDir::homePath();
    const QStringList candidates = {
        // 2. Livré avec le programme (dossier de l'exe, ou P2PChat.app/Contents/MacOS)
        appDir + "/" + exe,
        appDir + "/tor/" + exe,
        appDir + "/../Resources/tor/" + exe,
        // 3. Installations courantes
        "/opt/homebrew/bin/tor",                                        // macOS Apple Silicon (brew)
        "/usr/local/bin/tor",                                           // macOS Intel (brew)
        "/opt/local/bin/tor",                                           // MacPorts
        "/Applications/Tor Browser.app/Contents/MacOS/Tor/tor",         // Tor Browser macOS
        home + "/Desktop/Tor Browser/Browser/TorBrowser/Tor/tor.exe",   // Tor Browser Windows
        home + "/Bureau/Tor Browser/Browser/TorBrowser/Tor/tor.exe",
        "/usr/bin/tor",
        "/usr/sbin/tor",
    };
    for (const QString &c : candidates)
        if (QFileInfo(c).isExecutable() && !QFileInfo(c).isDir())
            return QFileInfo(c).canonicalFilePath();
    // 4. PATH
    return QStandardPaths::findExecutable(exe);
}

void TorService::start()
{
    const QString program = findExecutable();
    if (program.isEmpty()) {
        fail(tr("tor est introuvable sur cet ordinateur"));
        return;
    }
    makePrivateDir(m_dataDir);
    m_socksPort = freeLocalPort();

    const QString torrcPath = m_dataDir + "/torrc";
    QFile torrc(torrcPath);
    if (!torrc.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(tr("impossible d'écrire %1").arg(torrcPath));
        return;
    }
    const QString conf = QStringLiteral(
        "DataDirectory %1\n"
        "SocksPort 127.0.0.1:%2\n"
        "HiddenServiceDir %3\n"
        "HiddenServicePort %4 127.0.0.1:%5\n"
        // Uniquement des connexions sortantes vers des ports classiques
        "ReachableAddresses accept *:443, accept *:80, reject *:*\n"
        // tor s'arrête tout seul si le programme plante
        "__OwningControllerProcess %6\n"
        "AvoidDiskWrites 1\n"
        "Log notice stdout\n")
        .arg(torPath(m_dataDir))
        .arg(m_socksPort)
        .arg(torPath(m_dataDir + "/hs"))
        .arg(OnionPort)
        .arg(m_localPort)
        .arg(QCoreApplication::applicationPid());
    torrc.write(conf.toUtf8());
    torrc.close();

    // --defaults-torrc vide : on ignore la configuration système éventuelle
    const QString defaults = m_dataDir + "/torrc-defaults";
    QFile(defaults).open(QIODevice::WriteOnly | QIODevice::Truncate);

    m_ready = m_failed = false;
    m_lastProblem.clear();
    emit progress(0, tr("lancement"));
    m_process.start(program, {"-f", torrcPath, "--defaults-torrc", defaults});
}

void TorService::onOutput()
{
    static const QRegularExpression boot(QStringLiteral("Bootstrapped (\\d+)%[^:]*: (.*)"));
    m_buffer += m_process.readAll();
    int nl;
    while ((nl = m_buffer.indexOf('\n')) >= 0) {
        const QString line = QString::fromUtf8(m_buffer.left(nl)).trimmed();
        m_buffer.remove(0, nl + 1);
        if (line.contains("[err]") || line.contains("[warn]"))
            m_lastProblem = line.section("] ", 1);
        const auto m = boot.match(line);
        if (!m.hasMatch() || m_ready)
            continue;
        const int pct = m.captured(1).toInt();
        emit progress(pct, m.captured(2));
        if (pct == 100) {
            QFile f(m_dataDir + "/hs/hostname");
            if (!f.open(QIODevice::ReadOnly)) {
                fail(tr("adresse onion introuvable"));
                return;
            }
            m_onion = QString::fromLatin1(f.readAll()).trimmed();
            m_ready = true;
            emit ready(m_onion);
        }
    }
}

void TorService::fail(const QString &error)
{
    if (m_failed)
        return;
    m_failed = true;
    m_ready = false;
    emit failed(error);
}
