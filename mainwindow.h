#pragma once
#include "chatview.h"
#include "connectionpolicy.h"
#include "groupstore.h"
#include "history.h"
#include "reactions.h"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMainWindow>
#include <QPointer>
#include <QSet>
#include <QSettings>
#include <QTimer>

class PeerConnection;
class QAction;
class QLabel;
class QMenu;
class QMessageBox;
class QLineEdit;
class QPushButton;
class QTcpServer;
class QTcpSocket;
class QStackedWidget;
class QSystemTrayIcon;
class Composer;
class MentionCompleter;
class QDragEnterEvent;
class QDropEvent;
class QMimeData;
class Notifier;
class QToolButton;
class TerminalSession;
class TorService;

// Conversation de groupe en maillage complet : chaque membre est connecté
// directement à chacun des autres, via son adresse onion.
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    void changeEvent(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    // Glisser-déposer d'une image n'importe où sur la fenêtre (la zone de saisie gère le sien et le collage)
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    // Interface
    void buildUi();
    void updateStatus();
    // msg : message signé tel qu'échangé sur le réseau (id, a, n, x, ts, s)
    void appendMessage(const QJsonObject &msg, bool mine, ChatEntry::Trust trust, bool save = true);
    void appendSystem(const QString &text);
    // Pseudos reconnus dans les mentions « @… » (membres, nous-même et « tous »)
    QStringList mentionNames() const;
    bool mentionsMe(const QJsonObject &msg) const;
    bool repliesToMe(const QJsonObject &msg) const; // réponse à l'un de nos messages
    QString replyTo(const QJsonObject &msg, bool mine) const; // message cité, si la citation est bien signée
    QString notificationTitle(const QJsonObject &msg) const;
    // Réponse avec citation : bandeau au-dessus de la zone de saisie
    void startReply(const QString &id, const QString &author, const QString &text);
    void cancelReply();
    // Image jointe au message en cours d'écriture (aperçu au-dessus de la zone de saisie)
    void chooseImage();
    bool attachFromMime(const QMimeData *mime); // false : rien d'utilisable
    void attachImage(const QImage &image, const QString &error = {});
    void cancelImage();
    QString notificationBody(const QJsonObject &msg) const;

    // « Alice écrit… » : signal envoyé aux membres connectés, au plus toutes les 3 s pendant la frappe
    void onTyping();
    void onPeerTyping(PeerConnection *c);
    QString typingText() const; // vide si personne n'écrit

    // Réactions (cf. reactions.h)
    void react(const QString &id, const QString &emoji); // ajoute ou retire la nôtre
    bool acceptReaction(const QJsonObject &r, bool mine, bool save); // false : déjà connue
    bool verifyReaction(const QJsonObject &r, bool mine) const;
    QList<ChatEntry::Reaction> reactionSummary(const QString &id) const;
    void broadcastReaction(const QJsonObject &r, PeerConnection *except = nullptr);

    // Recherche dans la conversation (Ctrl+F)
    void openSearch();
    void closeSearch();

    // Lien cliqué : ouvert dans le navigateur, hors Tor (confirmation la première fois)
    void openLink(const QString &url);
    void loadHistory();
    void appendToHistoryFile(const QJsonObject &record, bool mine);
    void clearConversation();
    void setupTray();
    void showAndRaise();
    void updateUnread();

    // Tor
    void startTor();
    void onTorFailed(const QString &error);

    // Mise en relation
    void invite();
    void showInvitation();
    void join();

    // Membres du groupe (cf. GroupStore)
    bool mergeMember(const QString &onion, const QString &nick); // true si nouveau membre
    bool pinKey(const QString &onion, const QByteArray &key);    // false si une autre clé est déjà connue
    QString memberNick(const QString &onion) const;
    QJsonObject membersMessage() const;
    ChatEntry::Trust verifyMessage(const QJsonObject &msg, bool mine) const;

    // Synchronisation de l'historique (cf. history.h)
    QJsonObject inventoryMessage() const;
    void sendBatches(PeerConnection *c, const QList<QJsonObject> &messages); // filtrés pour ce membre
    void receiveSync(PeerConnection *from, const QJsonArray &messages);
    qint64 syncCutoff() const;
    // Message reçu alors que la fenêtre n'est pas au premier plan
    // firstTs : heure du plus ancien de ces messages (séparateur « Nouveaux messages ») ;
    // forMe : nous sommes mentionnés ou on nous répond (cf. réglage des notifications)
    void notifyUnread(int count, const QString &title, const QString &text, qint64 firstTs, bool forMe);
    bool notificationsAllowed(bool forMe) const;
    void buildNotificationMenu(QMenu *menu);

    // Connexions
    void onNewConnection();
    void connectToMembers();
    void reconnectNow(); // abandonne les tentatives en cours et relance tout de suite
    void dial(const QString &onion);
    PeerConnection *newConnection(QTcpSocket *socket, bool outgoing);
    void onClosed(PeerConnection *c, const QString &reason);
    void announceDropLater(const QString &onion, const QString &reason);
    void setActive(PeerConnection *c);
    void onReceived(PeerConnection *c, const QJsonObject &obj);
    void onMembers(PeerConnection *c, const QJsonObject &obj);
    void onPresence(PeerConnection *c, const QJsonObject &obj);
    void onChatMessage(PeerConnection *c, const QJsonObject &obj);
    void broadcast(const QJsonObject &obj, PeerConnection *except = nullptr);
    void closeAll();
    void onWake(qint64 gapMs);          // l'ordinateur sort de veille (ou le programme était figé)
    void retrySoon();                   // le réseau est revenu : on rappelle tout le monde

    // Présence relayée : chaque membre annonce à qui il est connecté directement
    QJsonObject presenceMessage() const;
    QHash<QString, QString> relayedOnline() const; // onion -> onion du membre qui le voit en ligne
    void checkQuietDrops();
    void sendCurrent();

    // Console partagée (Linux, macOS) : un membre ouvre sa console à un autre, cf. terminalsession.h
    void shareTerminal();
    void stopTerminal();
    void onTerminalMessage(PeerConnection *c, const QJsonObject &obj);
    bool startTerminal(bool host, const QString &sid, const QString &onion, bool control);
    void terminalPeerLost(const QString &onion);
    void sendTo(const QString &onion, const QJsonObject &obj);

    void changeNickname();
    void resetAll();

    QSettings m_settings;
    GroupStore m_group{m_settings};
    QTcpServer *m_server = nullptr; // 127.0.0.1 uniquement, joint via le service onion
    TorService *m_tor = nullptr;
    QString m_myOnion, m_torStatus, m_torError;
    bool m_inviteWanted = false;

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
    QHash<QString, Dropped> m_dropped; // onion -> déconnexion en attente d'annonce
    quint64 m_dropToken = 0;
    // Espacement progressif des appels vers les membres injoignables
    QHash<QString, ConnectionPolicy::Backoff> m_backoff;
    qint64 m_fastRetryUntil = 0; // jusque-là, essais à intervalle fixe (Tor vient de (re)démarrer)
    // Présence relayée
    QHash<QString, QSet<QString>> m_peerPresence; // membre connecté -> membres qu'il voit en ligne
    QHash<QString, QString> m_quietDrops;         // coupure directe non annoncée (joignable via un autre)
    QTimer m_presenceTimer;                       // regroupe les annonces de présence
    // Détection de sortie de veille : écart anormal entre deux tics
    QTimer m_wakeTimer;
    qint64 m_lastTick = 0;
    QSet<QString> m_seenIds;                     // anti-doublon des messages relayés
    QSet<QString> m_identityWarned;              // avertissements d'identité déjà affichés
    History::Store m_store;                      // historique signé : id -> message ou réaction
    Reactions::State m_reactions;
    QTimer m_retryTimer;
    QTimer m_syncTimer; // inventaire de contrôle périodique
    int m_unread = 0;

    struct TermOffer
    {
        QString sid, onion;
        bool control = false;
    };
    TerminalSession *m_term = nullptr;  // une seule console partagée à la fois
    TermOffer m_termOffer;              // proposition envoyée, en attente de réponse
    TermOffer m_termIncoming;           // proposition reçue, en attente de notre réponse
    QPointer<QMessageBox> m_termPrompt;
    QAction *m_termShareAct = nullptr, *m_termCtlAct = nullptr, *m_termStopAct = nullptr;

    ChatView *m_chat = nullptr;
    QStackedWidget *m_pages = nullptr; // accueil (pas de groupe) / conversation
    Composer *m_input = nullptr;
    MentionCompleter *m_mentions = nullptr;
    QWidget *m_replyBar = nullptr;
    QLabel *m_replyLabel = nullptr;
    QString m_replyTo; // identifiant du message cité par le message en cours d'écriture
    QToolButton *m_attachButton = nullptr;
    QWidget *m_imageBar = nullptr;
    QLabel *m_imagePreview = nullptr, *m_imageInfo = nullptr;
    QByteArray m_pendingImage; // JPEG prêt à partir avec le prochain message
    QHash<QString, qint64> m_typing; // onion -> fin de l'indication « écrit… » (ms)
    qint64 m_lastTypingSent = 0;
    QTimer m_typingTimer;            // efface les indications expirées
    QWidget *m_searchBar = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QLabel *m_searchCount = nullptr;
    QToolButton *m_sendButton = nullptr;
    QLabel *m_title = nullptr, *m_subtitle = nullptr;
    QString m_historyPath;

    QSystemTrayIcon *m_tray = nullptr; // Linux / Windows
    Notifier *m_notifier = nullptr;
    bool m_hideOnClose = false;        // la croix masque la fenêtre au lieu de quitter
};
