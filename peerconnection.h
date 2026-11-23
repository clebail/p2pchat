#pragma once
#include "crypto.h"

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QTimer>

class QTcpSocket;

// Une connexion chiffrée de bout en bout avec le correspondant (transportée par Tor).
//
// Protocole (trames = longueur 4 octets big-endian + contenu) :
//   1. chaque côté envoie en clair : "P2PCHAT1" + clé publique X25519 éphémère
//   2. clés de session = HKDF(DH, sel = secret de l'invitation)
//   3. tout le reste est chiffré (ChaCha20-Poly1305) : JSON hello / msg / ping
// Un correspondant sans le bon secret échoue au premier message chiffré.
class PeerConnection : public QObject
{
    Q_OBJECT
public:
    // Le socket peut être déjà connecté (entrant) ou en cours de connexion (sortant).
    PeerConnection(QTcpSocket *socket, bool outgoing, const QByteArray &psk, const QString &nickname,
                   const QString &ownOnion, QObject *parent = nullptr);

    bool isReady() const { return m_state == Ready; }
    bool isOutgoing() const { return m_outgoing; }
    QString peerNickname() const { return m_peerNick; }
    QString peerOnion() const { return m_peerOnion; }
    void sendChat(const QString &text);
    void close();

signals:
    void ready();
    void messageReceived(const QString &text, const QDateTime &timestamp);
    void closed(const QString &reason); // émis une seule fois

private:
    enum State { Connecting, WaitPeerKey, WaitHello, Ready, Closed };

    void onConnected();
    void onReadyRead();
    void handleFrame(const QByteArray &frame);
    void sendFrame(const QByteArray &payload);
    void sendEncrypted(const QJsonObject &obj);
    void fail(const QString &reason);

    QTcpSocket *m_socket;
    bool m_outgoing;
    QByteArray m_psk;
    QString m_nick, m_ownOnion, m_peerNick, m_peerOnion;
    State m_state = Connecting;
    KeyExchange m_kx;
    SecureChannel m_channel;
    QByteArray m_buffer;
    QTimer m_handshakeTimer, m_pingTimer, m_idleTimer;
};
