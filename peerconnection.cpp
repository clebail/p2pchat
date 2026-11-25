#include "peerconnection.h"

#include "protocol.h"

#include <QDebug>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QTcpSocket>
#include <QtEndian>

static const QByteArray Magic("P2PCHAT1");
static constexpr quint32 MaxFrame = 256 * 1024;

static const QByteArray HelloContext("p2pchat-hello-v4");
static const QByteArray TagContext("p2pchat-secret-tag-v1");
static constexpr int TagBytes = 8;
static constexpr int MaxSecrets = 4; // le secret actuel et quelques précédents

PeerConnection::PeerConnection(QTcpSocket *socket, bool outgoing, const QByteArray &psk, const QString &nickname,
                               const QString &ownOnion, const QByteArray &identitySeed, QObject *parent)
    : QObject(parent), m_socket(socket), m_outgoing(outgoing), m_psks{psk}, m_nick(nickname), m_ownOnion(ownOnion),
      m_identitySeed(identitySeed), m_advertisedVersion(Protocol::Version)
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
    QByteArray frame = Magic + m_kx.publicKey();
    if (m_psks.size() > 1) // sinon : trame des versions précédentes
        for (const QByteArray &psk : std::as_const(m_psks))
            frame += secretTag(psk, m_kx.publicKey());
    sendFrame(frame);
}

void PeerConnection::setSecrets(const QList<QByteArray> &secrets)
{
    if (!secrets.isEmpty())
        m_psks = secrets.mid(0, MaxSecrets);
}

QByteArray PeerConnection::secretTag(const QByteArray &secret, const QByteArray &ephemeralPublic)
{
    // Lié à la clé éphémère : d'une connexion à l'autre, rien ne permet de relier les étiquettes
    return QCryptographicHash::hash(TagContext + secret + ephemeralPublic, QCryptographicHash::Sha256).left(TagBytes);
}

void PeerConnection::sendHello()
{
    // Preuve d'identité liée à cette session (les clés éphémères ne servent qu'une fois)
    const QByteArray proof = Ed25519::sign(
        m_identitySeed, Crypto::pack({HelloContext, m_ourEphemeral, m_peerEphemeral, m_ownOnion.toUtf8()}));
    sendEncrypted({{"t", "hello"},
                   {"n", m_nick},
                   {"o", m_ownOnion},
                   {"v", m_advertisedVersion},
                   {"k", Protocol::toB64(Ed25519::publicKey(m_identitySeed))},
                   {"s", Protocol::toB64(proof)}});
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
        const int tagsBytes = int(frame.size()) - Magic.size() - 32;
        if (!frame.startsWith(Magic) || tagsBytes < 0 || tagsBytes % TagBytes != 0 || tagsBytes > MaxSecrets * TagBytes) {
            fail(tr("ce n'est pas un P2P Chat à l'autre bout"));
            return;
        }
        const QByteArray ourPub = m_kx.publicKey(), peerPub = frame.mid(Magic.size(), 32);
        m_dh = m_kx.sharedSecret(peerPub);
        m_ourEphemeral = ourPub;
        m_peerEphemeral = peerPub;
        m_state = WaitHello;
        if (m_dh.isEmpty()) {
            fail(tr("échange de clés impossible"));
            return;
        }
        // Secret commun : le plus récent des nôtres qui figure parmi les siens
        if (tagsBytes > 0) {
            QList<QByteArray> theirs;
            for (int i = 0; i < tagsBytes; i += TagBytes)
                theirs << frame.mid(Magic.size() + 32 + i, TagBytes);
            for (int k = 0; k < m_psks.size() && m_pskIndex < 0; ++k)
                if (theirs.contains(secretTag(m_psks.at(k), peerPub)))
                    m_pskIndex = k;
            if (m_pskIndex < 0) {
                fail(tr("code secret différent (invitation périmée ?)"));
                return;
            }
        } else if (m_psks.size() == 1) {
            m_pskIndex = 0; // comme les versions précédentes
        } else {
            // Un seul secret de son côté, sans étiquette : on le reconnaîtra à son hello, et on répondra ensuite
            m_waitPeerHello = true;
            qInfo().noquote() << "[peer] clé reçue, attente de son hello pour choisir le secret";
            return;
        }
        if (!m_channel.init(m_dh, m_psks.at(m_pskIndex), ourPub, peerPub)) {
            fail(tr("échange de clés impossible"));
            return;
        }
        qInfo().noquote() << "[peer] clé reçue, envoi du hello";
        sendHello();
        return;
    }

    QByteArray plain;
    if (m_waitPeerHello) {
        for (int k = 0; k < m_psks.size() && m_pskIndex < 0; ++k) {
            SecureChannel candidate;
            if (candidate.init(m_dh, m_psks.at(k), m_ourEphemeral, m_peerEphemeral) && candidate.decrypt(frame, plain)) {
                m_channel = candidate;
                m_pskIndex = k;
            }
        }
        if (m_pskIndex < 0) {
            fail(tr("code secret différent (invitation périmée ?)"));
            return;
        }
        m_waitPeerHello = false;
        sendHello(); // avec le secret qu'il utilise
    } else if (!m_channel.decrypt(frame, plain)) {
        fail(m_state == WaitHello ? tr("code secret différent (invitation périmée ?)") : tr("message corrompu"));
        return;
    }
    m_idleTimer.start();
    // Trame binaire (après la poignée de main seulement)
    if (m_state == Ready && !plain.isEmpty() && plain.at(0) != '{') {
        ++m_binaryReceived;
        emit binaryReceived(quint8(plain.at(0)), plain.mid(1));
        return;
    }
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

void PeerConnection::sendBinary(quint8 type, const QByteArray &payload)
{
    if (m_state != Ready || type == quint8('{'))
        return;
    sendFrame(m_channel.encrypt(QByteArray(1, char(type)) + payload));
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
