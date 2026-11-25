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
//      ainsi que la version du protocole ("v", cf. Protocol::Version)
//   1 bis. plusieurs secrets (le groupe en a changé, cf. governance.h) : la trame de clé est suivie d'une
//      étiquette de 8 octets par secret, et les deux côtés retiennent le plus récent qu'ils ont en commun.
//      Avec un seul secret, la trame est celle des versions précédentes.
//   5. une trame chiffrée qui ne commence pas par « { » est binaire (version 10) : son premier octet en
//      donne le type (cf. sendBinary), le reste est brut (pas de base64)
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
    int peerVersion() const { return m_peerVersion; }        // version du protocole annoncée dans le hello
    void send(const QJsonObject &obj);
    // Secrets acceptés, du plus récent au plus ancien (à régler avant la connexion)
    void setSecrets(const QList<QByteArray> &secrets);
    int secretIndex() const { return m_pskIndex; } // secret retenu : 0 = le plus récent, -1 = pas encore
    static QByteArray secretTag(const QByteArray &secret, const QByteArray &ephemeralPublic);
    // Trame binaire : type (premier octet, jamais « { ») puis contenu brut. Seulement si peerVersion() >= 10.
    void sendBinary(quint8 type, const QByteArray &payload);
    int binaryFramesReceived() const { return m_binaryReceived; }
    // Version annoncée dans notre hello (les tests simulent ainsi une version précédente)
    void setAdvertisedVersion(int version) { m_advertisedVersion = version; }
    void close();
    qint64 pendingBytes() const; // en attente d'envoi (Tor plus lent que l'émetteur)

signals:
    void ready();
    void received(const QJsonObject &obj);
    void binaryReceived(quint8 type, const QByteArray &payload);
    void closed(const QString &reason); // émis une seule fois

private:
    enum State { Connecting, WaitPeerKey, WaitHello, Ready, Closed };

    void onConnected();
    void onReadyRead();
    void handleFrame(const QByteArray &frame);
    void sendFrame(const QByteArray &payload);
    void sendEncrypted(const QJsonObject &obj);
    void sendHello();
    void fail(const QString &reason);

    QTcpSocket *m_socket;
    bool m_outgoing;
    QList<QByteArray> m_psks;
    int m_pskIndex = -1;
    QByteArray m_dh;
    bool m_waitPeerHello = false; // secret à deviner d'après le hello du correspondant (sans étiquettes)
    QString m_nick, m_ownOnion, m_peerNick, m_peerOnion;
    QByteArray m_identitySeed, m_peerKey, m_ourEphemeral, m_peerEphemeral;
    State m_state = Connecting;
    int m_peerVersion = 0;
    int m_advertisedVersion;
    int m_binaryReceived = 0;
    KeyExchange m_kx;
    SecureChannel m_channel;
    QByteArray m_buffer;
    QTimer m_handshakeTimer, m_pingTimer, m_idleTimer;
};
