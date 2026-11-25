#pragma once
#include "connectionpolicy.h"
#include "groupstore.h"
#include "history.h"
#include "protocol.h"
#include "reactions.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QTimer>

#include <functional>

struct Invitation;
class PeerConnection;
class QTcpServer;
class QTcpSocket;

// Un membre du groupe, sans interface : maillage complet (chaque membre est relié directement à chacun
// des autres, via son adresse onion), reconnexion, présence relayée, synchronisation de l'historique,
// messages, réactions, « écrit… », historique sur disque.
//
// Le transport est fourni de l'extérieur (Dialer) : le proxy SOCKS de Tor dans l'application, une
// connexion locale directe dans les tests (tests/tst_groupnode).
class GroupNode : public QObject
{
    Q_OBJECT
public:
    enum class Trust { Verified, Unsigned, Invalid };
    enum class JoinResult { Joined, OwnCode, KeyMismatch };

    // Crée le socket vers un membre et lance la connexion (Tor : adresse onion résolue par le proxy)
    using Dialer = std::function<QTcpSocket *(const QString &onion)>;

    // Délais : ceux de l'application par défaut ; raccourcis par les tests
    struct Timing
    {
        int retryIntervalMs = 10 * 1000;    // tic des rappels (chaque membre selon son espacement)
        int firstDialDelayMs = 5 * 1000;     // après le démarrage de Tor
        int redialDelayMs = 5 * 1000;        // après une coupure
        int presenceGraceMs = 60 * 1000;     // délai avant d'annoncer une déconnexion
        int presenceBatchMs = 1000;          // regroupe les annonces de présence
        int syncIntervalMs = 5 * 60 * 1000;  // inventaire de contrôle
        qint64 fastRetryMs = 2 * 60 * 1000;  // après un (re)démarrage : essais à intervalle fixe
        qint64 typingSendMs = 3000;          // « écrit… » : renvoyé au plus toutes les 3 s
        qint64 typingShownMs = 6000;         // affiché tant que le dernier signal a moins de 6 s
        int wakeTickMs = 10 * 1000;          // détection de sortie de veille
        qint64 wakeGapMs = 45 * 1000;        // tic arrivé si tard : sortie de veille
        qint64 sleepKillsLinksMs = 110 * 1000; // au-delà, les correspondants nous ont déjà fermé la porte
    };

    GroupNode(GroupStore &group, const QString &historyPath, const Timing &timing, QObject *parent = nullptr);
    GroupNode(GroupStore &group, const QString &historyPath, QObject *parent = nullptr)
        : GroupNode(group, historyPath, Timing{}, parent)
    {
    }
    ~GroupNode() override;

    // Écoute locale (127.0.0.1, port libre), jointe par le service onion
    bool listen(QString *error = nullptr);
    quint16 port() const;
    void setDialer(Dialer dialer) { m_dialer = std::move(dialer); }

    // Réseau prêt (adresse onion publiée) : on rappelle tout le monde, en mode rapide
    void setOnline(const QString &ownOnion);
    // Réseau perdu (Tor arrêté) : tout est fermé
    void setOffline();
    QString ownOnion() const { return m_ownOnion; }
    bool canSend() const;

    // ---- Historique
    void loadHistory(); // émet messageAdded pour les History::DisplayedRows derniers messages
    void clearHistory();
    bool isMine(const QString &id) const; // message ou réaction de nous
    QList<Reactions::Summary> reactionSummary(const QString &id) const;
    Trust verify(const QJsonObject &msg, bool mine) const;
    QString verifiedReplyTo(const QJsonObject &msg, bool mine) const; // vide : pas de citation authentique
    QByteArray verifiedImage(const QJsonObject &msg, bool mine) const;

    // ---- Groupe
    void createGroup(); // nouveau secret si l'on n'en a pas
    // Code d'invitation reçu. Un autre groupe en cours est quitté (l'interface a demandé confirmation).
    JoinResult join(const Invitation &inv);
    void leaveGroup();
    void announceNickname(); // notre pseudo a changé

    // ---- Envoi
    // Message signé, envoyé aux membres connectés (les autres le recevront par synchronisation) ;
    // vide si impossible (pas de groupe, pas de réseau, rien à envoyer)
    QJsonObject sendMessage(const QString &text, const Protocol::Message::Extras &extras = {});
    void react(const QString &id, const QString &emoji); // ajoute ou retire la nôtre
    void typing();                                       // au plus toutes les Timing::typingSendMs
    void sendTo(const QString &onion, const QJsonObject &obj); // console partagée

    // ---- Connexions
    void connectToMembers();
    void reconnectNow(); // abandonne les tentatives en cours et relance tout de suite
    void retrySoon();    // le réseau est revenu : on rappelle tout le monde, en mode rapide
    void closeAll();

    // ---- État (pour l'affichage)
    bool hasConnections() const { return !m_active.isEmpty(); }
    bool isConnected(const QString &onion) const { return m_active.contains(onion); }
    bool isAttempting(const QString &onion) const { return m_attempts.contains(onion); }
    int peerVersion(const QString &onion) const; // 0 : pas connecté
    qint64 pendingBytes(const QString &onion) const;
    QString lastError(const QString &onion) const { return m_lastError.value(onion); }
    qint64 nextTry(const QString &onion) const { return m_backoff.value(onion).nextTry; }
    QHash<QString, QString> relayedOnline() const; // onion -> onion du membre qui le voit en ligne
    QStringList typingMembers() const;             // onions des membres en train d'écrire

signals:
    // À afficher : message chargé, reçu (direct ou synchronisation) ou envoyé
    void messageAdded(const QJsonObject &msg, bool mine, GroupNode::Trust trust);
    // Messages des autres, authentiques, pour les notifications (catchUp : rattrapés par synchronisation)
    void messagesReceived(const QList<QJsonObject> &messages, bool catchUp);
    void reactionsChanged(const QString &id);
    void memberAdded(const QString &onion, const QString &nick, bool direct); // direct : il s'est connecté à nous
    void presenceChanged(const QString &onion, const QString &nick, bool online, const QString &reason);
    void identityRejected(const QString &onion, const QString &nick); // clé différente de celle connue
    void invalidMessage(const QString &author);                       // signature invalide
    void stateChanged();                                              // connexions, membres, erreurs
    void typingChanged();
    // Console partagée
    void peerReady(const QString &onion);
    void peerLost(const QString &onion);
    void allClosed();
    void terminalMessage(const QString &onion, const QJsonObject &obj);

private:
    PeerConnection *newConnection(QTcpSocket *socket, bool outgoing);
    void onNewConnection();
    void dial(const QString &onion);
    void onClosed(PeerConnection *c, const QString &reason);
    void announceDropLater(const QString &onion, const QString &reason);
    void setActive(PeerConnection *c);
    void onReceived(PeerConnection *c, const QJsonObject &obj);
    void onMembers(PeerConnection *c, const QJsonObject &obj);
    void onPresence(PeerConnection *c, const QJsonObject &obj);
    void onChatMessage(PeerConnection *c, const QJsonObject &obj);
    void onPeerTyping(PeerConnection *c);
    void onWake(qint64 gapMs);
    void broadcast(const QJsonObject &obj, PeerConnection *except = nullptr);
    void checkQuietDrops();

    bool mergeMember(const QString &onion, const QString &nick);
    bool pinKey(const QString &onion, const QByteArray &key);
    QByteArray authorKey(const QJsonObject &record, bool mine) const;
    QJsonObject membersMessage() const;
    QJsonObject presenceMessage() const;

    // Historique et synchronisation (cf. history.h)
    void addMessage(const QJsonObject &msg, bool mine, Trust trust, bool save);
    void appendToHistoryFile(const QJsonObject &record, bool mine);
    qint64 syncCutoff() const;
    QJsonObject inventoryMessage() const;
    void sendBatches(PeerConnection *c, const QList<QJsonObject> &messages);
    void receiveSync(PeerConnection *from, const QJsonArray &messages);

    // Réactions (cf. reactions.h)
    bool acceptReaction(const QJsonObject &r, bool mine, bool save); // false : déjà connue
    bool verifyReaction(const QJsonObject &r, bool mine) const;
    void broadcastReaction(const QJsonObject &r, PeerConnection *except = nullptr);

    GroupStore &m_group;
    QString m_historyPath;
    Timing m_timing;
    Dialer m_dialer;
    QTcpServer *m_server;
    QString m_ownOnion;

    QHash<QString, PeerConnection *> m_active;   // onion -> connexion établie
    QHash<QString, PeerConnection *> m_attempts; // onion -> connexion sortante en cours
    QSet<PeerConnection *> m_pending;            // entrantes avant le hello
    QHash<QString, QString> m_lastError;         // onion -> dernière erreur
    // Déconnexions pas encore annoncées : on laisse au membre un délai de grâce pour revenir
    struct Dropped
    {
        QString reason;
        quint64 token = 0;
    };
    QHash<QString, Dropped> m_dropped;
    quint64 m_dropToken = 0;
    QHash<QString, ConnectionPolicy::Backoff> m_backoff; // espacement des appels sans réponse
    qint64 m_fastRetryUntil = 0;
    QHash<QString, QSet<QString>> m_peerPresence; // membre connecté -> membres qu'il voit en ligne
    QHash<QString, QString> m_quietDrops;         // coupure directe non annoncée (joignable via un autre)
    QSet<QString> m_seenIds;                      // anti-doublon des messages relayés
    QSet<QString> m_identityWarned;               // avertissements d'identité déjà émis
    History::Store m_store;                       // historique signé : id -> message ou réaction
    Reactions::State m_reactions;
    QHash<QString, qint64> m_typing;              // onion -> fin de l'indication « écrit… »
    qint64 m_lastTypingSent = 0;
    qint64 m_lastTick = 0;

    QTimer m_retryTimer, m_syncTimer, m_presenceTimer, m_typingTimer, m_wakeTimer;
};
