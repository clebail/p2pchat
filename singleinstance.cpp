#include "singleinstance.h"

#include <QCryptographicHash>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>

namespace {
const QByteArray ShowRequest("show\n");
}

SingleInstance::SingleInstance(const QString &dataDir, QObject *parent)
    : QObject(parent), m_dataDir(QDir(dataDir).absolutePath())
{
    // Nom court et propre au dossier de données (donc à l'utilisateur) : un chemin de socket Unix est limité
    m_serverName = QStringLiteral("p2pchat-")
                   + QString::fromLatin1(
                       QCryptographicHash::hash(m_dataDir.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
}

SingleInstance::~SingleInstance()
{
    if (m_server)
        m_server->close();
    if (m_lock)
        m_lock->unlock();
}

bool SingleInstance::acquire()
{
    QDir().mkpath(m_dataDir);
    m_lock = std::make_unique<QLockFile>(m_dataDir + "/instance.lock");
    m_lock->setStaleLockTime(0); // jamais périmé par l'âge : seulement si son processus n'existe plus
    if (!m_lock->tryLock(0)) {
        m_lock.reset();
        return false;
    }
    // Socket laissé par une instance plantée : il ne gêne plus, le verrou est à nous
    QLocalServer::removeServer(m_serverName);
    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    connect(m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *s = m_server->nextPendingConnection()) {
            connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
            connect(s, &QLocalSocket::readyRead, this, [this, s] {
                if (s->canReadLine() && s->readLine(64) == ShowRequest)
                    emit activationRequested();
                s->disconnectFromServer();
            });
        }
    });
    m_server->listen(m_serverName); // à défaut, l'instance unique reste garantie par le verrou
    return true;
}

bool SingleInstance::activateRunning(int timeoutMs)
{
    QLocalSocket socket;
    socket.connectToServer(m_serverName);
    if (!socket.waitForConnected(timeoutMs))
        return false;
    socket.write(ShowRequest);
    return socket.waitForBytesWritten(timeoutMs);
}
