#pragma once
#include "crypto.h"

#include <QJsonObject>
#include <QObject>
#include <QTimer>

class QTcpSocket;

// Une connexion chiffrée de bout en bout avec un membre du groupe (transportée par Tor).
//
// Protocole (trames = longueur 4 octets big-endian + contenu) :
//   1. chaque côté envoie en clair : "P2PCHAT1" + clé publique X25519 éphémère
//   2. clés de session = HKDF(DH, sel = secret du groupe)
//   3. tout le reste est chiffré (ChaCha20-Poly1305) : objets JSON
//      {"t":"hello"} puis {"t":"members"}, {"t":"msg"}, {"t":"ping"}
//   4. le hello contient la clé d'identité Ed25519 du poste et sa signature de
//      (clé éphémère émise, clé éphémère reçue, adresse onion annoncée)
// Un pair sans le bon secret échoue au premier message chiffré ; un membre qui
// annonce l'adresse d'un autre ne peut pas produire la signature attendue.
class PeerConnection : public QObject
{
    Q_OBJECT
public:
    // Le socket peut être déjà connecté (entrant) ou en cours de connexion (sortant).
    // identitySeed : clé privée Ed25519 du poste, qui signe la poignée de main.
    PeerConnection(QTcpSocket *socket, bool outgoing, const QByteArray &psk, const QString &nickname,
                   const QString &ownOnion, const QByteArray &identitySeed, QObject *parent = nullptr);

    bool isReady() const { return m_state == Ready; }
    bool isOutgoing() const { return m_outgoing; }
    QString peerNickname() const { return m_peerNick; }
    QString peerOnion() const { return m_peerOnion; }
    QByteArray peerIdentityKey() const { return m_peerKey; } // vérifiée par la signature du hello
    void send(const QJsonObject &obj);
    void close();
    qint64 pendingBytes() const; // en attente d'envoi (Tor plus lent que l'émetteur)

signals:
    void ready();
    void received(const QJsonObject &obj);
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
    QByteArray m_identitySeed, m_peerKey, m_ourEphemeral, m_peerEphemeral;
    State m_state = Connecting;
    KeyExchange m_kx;
    SecureChannel m_channel;
    QByteArray m_buffer;
    QTimer m_handshakeTimer, m_pingTimer, m_idleTimer;
};
