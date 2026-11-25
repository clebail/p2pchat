#pragma once
#include "chatview.h"
#include "connectionpolicy.h"
#include "groupstore.h"
#include "history.h"

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
class QMessageBox;
class QLineEdit;
class QPushButton;
class QTcpServer;
class QTcpSocket;
class QStackedWidget;
class QSystemTrayIcon;
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

private:
    // Interface
    void buildUi();
    void updateStatus();
    // msg : message signé tel qu'échangé sur le réseau (id, a, n, x, ts, s)
    void appendMessage(const QJsonObject &msg, bool mine, ChatEntry::Trust trust, bool save = true);
    void appendSystem(const QString &text);
    void loadHistory();
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
    void notifyUnread(int count, const QString &title, const QString &text);

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
    History::Store m_store;                      // historique signé : id -> message
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
    QLineEdit *m_input = nullptr;
    QToolButton *m_sendButton = nullptr;
    QLabel *m_title = nullptr, *m_subtitle = nullptr;
    QString m_historyPath;

    QSystemTrayIcon *m_tray = nullptr; // Linux / Windows
    Notifier *m_notifier = nullptr;
    bool m_hideOnClose = false;        // la croix masque la fenêtre au lieu de quitter
};
