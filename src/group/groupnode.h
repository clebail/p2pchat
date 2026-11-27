#pragma once
#include "connectionpolicy.h"
#include "edits.h"
#include "filestore.h"
#include "governance.h"
#include "groupstore.h"
#include "history.h"
#include "invitation.h"
#include "protocol.h"
#include "reactions.h"

#include <QBitArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>

class PeerConnection;
class QTcpSocket;
class Switchboard;

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
        int fileTickMs = 200;                  // fichiers : délais dépassés, envois en attente
        qint64 fileRequestTimeoutMs = 60 * 1000; // morceau demandé sans réponse : autre source
        qint64 fileBacklogBytes = 64 * 1024;   // morceau envoyé seulement si la connexion a écoulé son retard
    };

    GroupNode(GroupStore &group, const QString &historyPath, const Timing &timing, QObject *parent = nullptr);
    GroupNode(GroupStore &group, const QString &historyPath, QObject *parent = nullptr)
        : GroupNode(group, historyPath, Timing{}, parent)
    {
    }
    ~GroupNode() override;

    // Écoute locale (127.0.0.1, port libre), jointe par le service onion : propre à ce groupe. Avec plusieurs
    // conversations, un Switchboard commun écoute à la place et confie à chacune ses connexions entrantes.
    bool listen(QString *error = nullptr);
    quint16 port() const;
    bool acceptsIncoming() const;           // groupe en cours, réseau prêt
    QList<QByteArray> acceptedSecrets() const; // actuel, puis précédents (membres absents)
    PeerConnection *createIncoming(QTcpSocket *socket, QObject *parent); // poignée de main à commencer
    void adoptIncoming(PeerConnection *c);  // secret reconnu : la suite nous revient
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
    bool isFromMe(const QString &id) const; // écrit sur ce poste ou un autre des nôtres (postes liés)
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
    void publishAvatar();    // notre avatar a changé (cf. GroupStore::setOwnAvatar) : envoyé aux membres connectés

    // ---- Envoi
    // Message signé, envoyé aux membres connectés (les autres le recevront par synchronisation) ;
    // vide si impossible (pas de groupe, pas de réseau, rien à envoyer)
    QJsonObject sendMessage(const QString &text, const Protocol::Message::Extras &extras = {});
    void react(const QString &id, const QString &emoji); // ajoute ou retire la nôtre
    // Modification et suppression de nos messages (cf. edits.h) ; false si ce n'est pas possible
    // Notre message, ou celui d'un autre si nous sommes administrateur (modération) ; authentique, pas supprimé
    bool canEdit(const QString &id) const;
    bool editMessage(const QString &id, const QString &text);
    bool deleteMessage(const QString &id);
    // Dernière version d'un message par son auteur (aucune : jamais modifié)
    std::optional<Edits::Latest> latestEdit(const QJsonObject &msg) const;
    // Qui peut modifier ce message : son auteur, et quiconque a été administrateur du groupe
    QStringList editors(const QJsonObject &msg) const;
    QJsonObject storedMessage(const QString &id) const { return m_store.value(id); } // vide : inconnu
    void typing();                                       // au plus toutes les Timing::typingSendMs
    void sendTo(const QString &onion, const QJsonObject &obj); // console partagée

    // ---- Gouvernance (cf. governance.h) : administrateurs, exclusions, renouvellement du secret
    QString groupId() const;                     // vide : pas encore de gouvernance
    bool isAdmin(const QString &onion = {}) const; // nous, par défaut
    QStringList admins() const;
    QStringList excludedMembers() const;
    // Groupe d'une version précédente (sans gouvernance) : le premier qui exclut ou renouvelle devient
    // administrateur. Ensuite, seuls les administrateurs le peuvent.
    bool canGovern() const;
    bool exclude(const QString &onion, QString *error = nullptr);
    bool renewSecret(QString *error = nullptr);
    // Nomme un membre administrateur, ou lui retire ce rôle (nous compris) ; il en reste toujours un
    bool setAdmin(const QString &onion, bool admin, QString *error = nullptr);

    // ---- Conversations (version 12)
    // Nom partagé, choisi par un administrateur (vide : aucun)
    QString sharedName() const;
    // Avatar de la conversation (JPEG, vide : aucun), choisi par un administrateur et le même pour tous
    QByteArray conversationAvatar() const;
    bool setConversationAvatar(const QByteArray &jpeg, QString *error = nullptr); // vide : retiré
    bool setSharedName(const QString &name, QString *error = nullptr);
    // Invitation à une autre conversation, par la connexion chiffrée avec ce membre ; false s'il n'est pas
    // connecté directement ou utilise une version précédente
    bool inviteDirectly(const QString &onion, const Invitation &invitation, const QString &name);
    bool canInviteDirectly(const QString &onion) const;
    // Liaison d'un autre de nos postes (cf. devicelink.h) : sa clé retenue, l'historique lui sera partagé ; le code
    // qu'il utilisera pour nous rejoindre (aucun : pas de groupe, pas de réseau, ou ce n'est pas un de nos postes)
    std::optional<Invitation> invitationForMyDevice(const QString &onion, const QByteArray &key);

    // ---- Fichiers (cf. filestore.h) : annonce signée, puis morceaux demandés à qui les a
    void setFilesDir(const QString &dir); // par défaut : « fichiers » à côté de l'historique
    Files::FileStore &files() { return *m_files; }
    // Copie le fichier dans le magasin et l'annonce au groupe ; message vide (et error) si impossible
    QJsonObject sendFile(const QString &path, const QString &caption = {}, const QString &replyTo = {},
                         QString *error = nullptr);
    std::optional<Files::FileInfo> verifiedFile(const QJsonObject &msg, bool mine) const;
    void download(const QString &fileId); // automatique pour les petits fichiers (Files::AutoDownloadBytes)
    void cancelDownload(const QString &fileId);
    struct FileState
    {
        enum Kind { Unknown, NotDownloaded, Downloading, Waiting, Complete } kind = Unknown;
        int received = 0, total = 0; // morceaux
        QStringList sources;         // membres qui nous l'envoient en ce moment
    };
    FileState fileState(const QString &fileId) const;

    // ---- Connexions
    void connectToMembers();
    void reconnectNow(); // abandonne les tentatives en cours et relance tout de suite
    void retrySoon();    // le réseau est revenu : on rappelle tout le monde, en mode rapide
    void closeAll();

    // ---- État (pour l'affichage)
    bool hasConnections() const { return !m_active.isEmpty(); }
    bool isConnected(const QString &onion) const { return m_active.contains(onion); }
    QStringList connectedPeers() const { return m_active.keys(); }
    bool isAttempting(const QString &onion) const { return m_attempts.contains(onion); }
    bool isMyDevice(const QString &onion) const { return m_group.isMyDevice(onion); } // un autre de nos postes
    int peerVersion(const QString &onion) const; // 0 : pas connecté
    qint64 pendingBytes(const QString &onion) const;
    QString lastError(const QString &onion) const { return m_lastError.value(onion); }
    qint64 nextTry(const QString &onion) const { return m_backoff.value(onion).nextTry; }
    QHash<QString, QString> relayedOnline() const; // onion -> onion du membre qui le voit en ligne
    QStringList typingMembers() const;             // onions des membres en train d'écrire
    int binaryChunksReceived() const { return m_binaryChunks; } // morceaux reçus en trame binaire (tests)
    void setAdvertisedVersion(int version) { m_advertisedVersion = version; } // tests : version précédente

signals:
    // À afficher : message chargé, reçu (direct ou synchronisation) ou envoyé
    void messageAdded(const QJsonObject &msg, bool mine, GroupNode::Trust trust);
    // Messages des autres, authentiques, pour les notifications (catchUp : rattrapés par synchronisation)
    void messagesReceived(const QList<QJsonObject> &messages, bool catchUp);
    void reactionsChanged(const QString &id);
    void messageEdited(const QString &id); // modifié ou supprimé (cf. latestEdit)
    void memberAdded(const QString &onion, const QString &nick, bool direct); // direct : il s'est connecté à nous
    void presenceChanged(const QString &onion, const QString &nick, bool online, const QString &reason);
    void identityRejected(const QString &onion, const QString &nick); // clé différente de celle connue
    void invalidMessage(const QString &author);                       // signature invalide
    void stateChanged();                                              // connexions, membres, erreurs
    void typingChanged();
    void fileChanged(const QString &fileId); // téléchargement commencé, progression, fin, en attente
    // Gouvernance
    void memberExcluded(const QString &onion, const QString &nick, const QString &byNick);
    void excludedFromGroup(const QString &byNick); // nous : connexions fermées, groupe quitté
    void secretRenewed(const QString &byNick);
    void memberLeft(const QString &onion, const QString &nick);
    void adminChanged(const QString &onion, const QString &nick, bool admin, const QString &byNick);
    void sharedNameChanged(const QString &name, const QString &byNick);
    void avatarChanged(const QString &onion); // avatar d'un membre reçu (cf. GroupStore::avatarRecord)
    void devicesChanged(); // certificat de poste reçu : un membre est relié à sa personne (cf. person.h)
    void conversationAvatarChanged(bool removed, const QString &byNick);
    // Un membre nous invite à une autre conversation (son code, vérifié : c'est bien le sien)
    void directInvitation(const QString &fromOnion, const QString &fromNick, const Invitation &invitation,
                          const QString &name);
    // Console partagée
    void peerReady(const QString &onion);
    void peerLost(const QString &onion);
    void allClosed();
    void terminalMessage(const QString &onion, const QJsonObject &obj);
    // Synchronisation entre nos postes (type « self-… », cf. devicesync.h) : seulement d'un de nos postes
    void selfMessage(const QString &onion, const QJsonObject &obj);

private:
    PeerConnection *newConnection(QTcpSocket *socket, bool outgoing);
    void wire(PeerConnection *c); // signaux de la connexion vers nous
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

    // Gouvernance
    std::optional<Governance::Record> head() const;
    void ensureGenesis();
    // Nouvel enregistrement de gouvernance (nous devons être administrateur) ; admins vide : ceux en place,
    // moins les exclus. renew : nouveau secret (obligatoire dès qu'on exclut quelqu'un)
    bool newRecord(const QStringList &excluded, QStringList admins, bool renew, QString *error);
    bool integrateRecords(const QJsonArray &records);            // true si la chaîne a avancé
    void applyRecord(const Governance::Record &previousHead, const Governance::Record &record);
    void sendGovernance(PeerConnection *c);                       // chaîne, puis secret si membre non exclu
    void onName(PeerConnection *c, const QJsonObject &o);
    void onDirectInvitation(PeerConnection *c, const QJsonObject &o);
    void sendName(PeerConnection *c);
    void sendConversationAvatar(PeerConnection *c);
    void onConversationAvatar(PeerConnection *c, const QJsonObject &o);
    QJsonObject avatarRecordOf(const QString &onion) const; // le nôtre (signé à la volée) ou celui reçu
    QJsonObject ownCertificate() const;
    void sendCertificates(PeerConnection *c); // le nôtre et ceux des membres
    void onCertificates(PeerConnection *c, const QJsonObject &o);
    void promoteMyDevices(); // administrateur : nos autres postes le deviennent aussi (une fois chacun)
    void sendAvatarInventory(PeerConnection *c);
    void onAvatarInventory(PeerConnection *c, const QJsonObject &o);
    void onAvatar(PeerConnection *c, const QJsonObject &o);
    void onGovernance(PeerConnection *c, const QJsonObject &o);
    void onSecret(PeerConnection *c, const QJsonObject &o);
    void onLeave(PeerConnection *c, const QJsonObject &o);
    QByteArray keyOf(const QString &onion) const;

    // Fichiers
    void noteFileAnnounce(const QJsonObject &msg, bool mine);
    void advertiseFiles(const QStringList &ids, PeerConnection *only = nullptr); // file-have
    void onFileHave(PeerConnection *c, const QJsonObject &o);
    void onFileWant(PeerConnection *c, const QJsonObject &o);
    void onFileChunk(PeerConnection *c, const QJsonObject &o);
    void onBinary(PeerConnection *c, quint8 type, const QByteArray &payload);
    void handleChunk(PeerConnection *c, const QString &id, int index, const QByteArray &data);
    void onFileNone(PeerConnection *c, const QJsonObject &o);
    void pumpDownloads();
    void serveQueued();
    void filesTick();
    bool peerHasChunk(const QString &peer, const QString &fileId, int chunk) const;
    QList<PeerConnection *> fileCapablePeers() const;

    // Réactions (cf. reactions.h)
    bool acceptReaction(const QJsonObject &r, bool mine, bool save); // false : déjà connue
    bool verifyReaction(const QJsonObject &r, bool mine) const;
    // Modifications (cf. edits.h)
    bool acceptEdit(const QJsonObject &r, bool mine, bool save); // false : déjà connue
    void broadcastEdit(const QJsonObject &r, PeerConnection *except = nullptr);
    void broadcastReaction(const QJsonObject &r, PeerConnection *except = nullptr);

    GroupStore &m_group;
    QString m_historyPath;
    Timing m_timing;
    Dialer m_dialer;
    Switchboard *m_ownBoard = nullptr; // cf. listen()
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
    QList<Governance::Record> m_chain;            // gouvernance vérifiée
    QSet<QString> m_seenIds;                      // anti-doublon des messages relayés
    QSet<QString> m_identityWarned;               // avertissements d'identité déjà émis
    History::Store m_store;                       // historique signé : id -> message ou réaction
    Reactions::State m_reactions;
    Edits::State m_edits;
    QHash<QString, qint64> m_typing;              // onion -> fin de l'indication « écrit… »
    qint64 m_lastTypingSent = 0;
    qint64 m_lastTick = 0;

    // Fichiers
    std::unique_ptr<Files::FileStore> m_files;
    QHash<QString, Files::FileInfo> m_fileInfos; // fiches annoncées par des messages authentiques
    // Téléchargement depuis plusieurs membres : jusqu'à MaxSources à la fois, PerSource morceaux en vol
    // chacun (la latence de Tor est masquée), les morceaux les plus rares d'abord, et pour finir les derniers
    // demandés aussi aux sources libres (une source lente ne bloque pas la fin)
    static constexpr int MaxSources = 4, PerSource = 2;
    struct Request
    {
        QString source;
        int chunk = -1;
        qint64 at = 0;
    };
    struct Download
    {
        QList<Request> inFlight;
        QSet<QString> bad;    // membres qui ont fourni un morceau faux : plus sollicités
        int sinceAnnounce = 0; // morceaux reçus depuis la dernière annonce de notre progression
        qint64 announcedAt = 0;
    };
    QHash<QString, Download> m_downloads;
    QHash<QString, double> m_chunkMs; // membre -> durée moyenne d'un morceau (sources lentes moins sollicitées)
    void dropRequests(const QString &peer, const QString &fileId = {}); // vide : tous les fichiers
    QHash<QString, QHash<QString, QBitArray>> m_peerFiles; // membre -> fichier -> morceaux (vide : complet)
    QHash<QString, QList<QPair<QString, int>>> m_serveQueue; // membre -> morceaux qu'il nous a demandés
    QTimer m_fileTimer;
    qint64 m_lastCleanup = 0;
    int m_binaryChunks = 0;
    int m_advertisedVersion = Protocol::Version;

    QTimer m_retryTimer, m_syncTimer, m_presenceTimer, m_typingTimer, m_wakeTimer;
};
