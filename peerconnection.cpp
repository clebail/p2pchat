#include "peerconnection.h"

#include "protocol.h"

#include <QDebug>
#include <QJsonDocument>
#include <QTcpSocket>
#include <QtEndian>

static const QByteArray Magic("P2PCHAT1");
static constexpr quint32 MaxFrame = 256 * 1024;

static const QByteArray HelloContext("p2pchat-hello-v4");

PeerConnection::PeerConnection(QTcpSocket *socket, bool outgoing, const QByteArray &psk, const QString &nickname,
                               const QString &ownOnion, const QByteArray &identitySeed, QObject *parent)
    : QObject(parent), m_socket(socket), m_outgoing(outgoing), m_psk(psk), m_nick(nickname), m_ownOnion(ownOnion),
      m_identitySeed(identitySeed)
{
    socket->setParent(this);
    connect(socket, &QTcpSocket::connected, this, &PeerConnection::onConnected);
    connect(socket, &QTcpSocket::readyRead, this, &PeerConnection::onReadyRead);
    connect(socket, &QTcpSocket::disconnected, this, [this] { fail(tr("le correspondant s'est déconnecté")); });
    connect(socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError err) {
        if (err == QAbstractSocket::ProxyConnectionRefusedError || err == QAbstractSocket::ProxyNotFoundError)
            fail(tr("Tor ne répond pas"));
        else if (m_state == Connecting)
            fail(tr("correspondant injoignable (programme fermé de son côté ?)"));
        else
            fail(tr("le correspondant s'est déconnecté"));
    });

    // Via Tor, le premier circuit vers un service onion prend souvent 5 à 20 s, mais une requête
    // peut aussi rester bloquée dans Tor : mieux vaut abandonner et réessayer qu'attendre.
    m_handshakeTimer.setSingleShot(true);
    connect(&m_handshakeTimer, &QTimer::timeout, this, [this] { fail(tr("délai de connexion dépassé")); });
    m_handshakeTimer.start(45000);

    m_pingTimer.setInterval(30000);
    connect(&m_pingTimer, &QTimer::timeout, this, [this] { sendEncrypted({{"t", "ping"}}); });

    m_idleTimer.setSingleShot(true);
    m_idleTimer.setInterval(100000);
    connect(&m_idleTimer, &QTimer::timeout, this, [this] { fail(tr("plus de nouvelles du correspondant")); });

    if (socket->state() == QAbstractSocket::ConnectedState)
        QMetaObject::invokeMethod(this, &PeerConnection::onConnected, Qt::QueuedConnection);
}

void PeerConnection::onConnected()
{
    if (m_state != Connecting)
        return;
    qInfo().noquote() << "[peer]" << (m_outgoing ? "sortante" : "entrante") << "connectée, envoi de la clé";
    m_state = WaitPeerKey;
    m_handshakeTimer.start(30000);
    sendFrame(Magic + m_kx.publicKey());
}

void PeerConnection::onReadyRead()
{
    m_buffer += m_socket->readAll();
    while (m_state != Closed && m_buffer.size() >= 4) {
        const quint32 len = qFromBigEndian<quint32>(m_buffer.constData());
        if (len > MaxFrame) {
            fail(tr("trame trop grande"));
            return;
        }
        if (quint32(m_buffer.size()) < 4 + len)
            break;
        const QByteArray frame = m_buffer.mid(4, int(len));
        m_buffer.remove(0, int(4 + len));
        handleFrame(frame);
    }
}

void PeerConnection::handleFrame(const QByteArray &frame)
{
    if (m_state == WaitPeerKey) {
        if (frame.size() != Magic.size() + 32 || !frame.startsWith(Magic)) {
            fail(tr("ce n'est pas un P2P Chat à l'autre bout"));
            return;
        }
        const QByteArray ourPub = m_kx.publicKey(), peerPub = frame.mid(Magic.size());
        if (!m_channel.init(m_kx.sharedSecret(peerPub), m_psk, ourPub, peerPub)) {
            fail(tr("échange de clés impossible"));
            return;
        }
        qInfo().noquote() << "[peer] clé reçue, envoi du hello";
        m_ourEphemeral = ourPub;
        m_peerEphemeral = peerPub;
        m_state = WaitHello;
        // Preuve d'identité liée à cette session (les clés éphémères ne servent qu'une fois)
        const QByteArray proof = Ed25519::sign(
            m_identitySeed, Crypto::pack({HelloContext, ourPub, peerPub, m_ownOnion.toUtf8()}));
        sendEncrypted({{"t", "hello"},
                       {"n", m_nick},
                       {"o", m_ownOnion},
                       {"v", Protocol::Version},
                       {"k", Protocol::toB64(Ed25519::publicKey(m_identitySeed))},
                       {"s", Protocol::toB64(proof)}});
        return;
    }

    QByteArray plain;
    if (!m_channel.decrypt(frame, plain)) {
        fail(m_state == WaitHello ? tr("code secret différent (invitation périmée ?)") : tr("message corrompu"));
        return;
    }
    m_idleTimer.start();
    const QJsonObject o = QJsonDocument::fromJson(plain).object();
    const QString type = o.value("t").toString();

    if (m_state == WaitHello) {
        if (type != "hello") {
            fail(tr("protocole inattendu"));
            return;
        }
        m_peerNick = o.value("n").toString().left(Protocol::MaxNickLength);
        if (m_peerNick.isEmpty())
            m_peerNick = tr("Correspondant");
        m_peerOnion = o.value("o").toString().left(70);
        const QByteArray key = Protocol::fromB64(o.value("k")), proof = Protocol::fromB64(o.value("s"));
        m_peerVersion = o.value("v").toInt();
        if (m_peerVersion < Protocol::MinVersion) {
            fail(tr("le correspondant utilise une ancienne version de P2P Chat"));
            return;
        }
        if (!Ed25519::verify(key, Crypto::pack({HelloContext, m_peerEphemeral, m_ourEphemeral, m_peerOnion.toUtf8()}),
                             proof)) {
            fail(tr("signature d'identité invalide"));
            return;
        }
        m_peerKey = key;
        qInfo().noquote() << "[peer] prêt avec" << m_peerNick << m_peerOnion.left(8);
        m_state = Ready;
        m_handshakeTimer.stop();
        m_pingTimer.start();
        emit ready();
    } else if (type != "ping") {
        emit received(o);
    }
}

void PeerConnection::sendFrame(const QByteArray &payload)
{
    QByteArray header(4, Qt::Uninitialized);
    qToBigEndian<quint32>(quint32(payload.size()), header.data());
    m_socket->write(header + payload);
}

void PeerConnection::sendEncrypted(const QJsonObject &obj)
{
    if (m_state != WaitHello && m_state != Ready)
        return;
    sendFrame(m_channel.encrypt(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
}

void PeerConnection::send(const QJsonObject &obj)
{
    if (m_state == Ready)
        sendEncrypted(obj);
}

qint64 PeerConnection::pendingBytes() const
{
    return m_socket->bytesToWrite();
}

void PeerConnection::close()
{
    fail(tr("connexion fermée"));
}

void PeerConnection::fail(const QString &reason)
{
    if (m_state == Closed)
        return;
    qInfo().noquote() << "[peer] fermeture :" << reason << "(état" << m_state << ")";
    m_state = Closed;
    m_handshakeTimer.stop();
    m_pingTimer.stop();
    m_idleTimer.stop();
    m_socket->disconnect(this);
    m_socket->flush();
    m_socket->disconnectFromHost();
    emit closed(reason);
}
