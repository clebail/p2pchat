#pragma once
#include "chatview.h"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMainWindow>
#include <QSet>
#include <QSettings>
#include <QTimer>

class PeerConnection;
class QLabel;
class QLineEdit;
class QPushButton;
class QTcpServer;
class QStackedWidget;
class QSystemTrayIcon;
class Notifier;
class QToolButton;
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
    struct Member
    {
        QString onion, nick;
    };

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

    // Membres du groupe
    QList<Member> members() const;
    void saveMembers(const QList<Member> &list);
    bool mergeMember(const QString &onion, const QString &nick); // true si nouveau membre
    QJsonObject membersMessage() const;

    // Identité : clé Ed25519 de ce poste, et trousseau des clés des membres (mémorisées à la
    // première rencontre, conservées même après avoir quitté le groupe pour vérifier l'historique)
    QByteArray identitySeed() const;
    QByteArray myIdentityKey() const;
    QByteArray pinnedKey(const QString &onion) const;
    bool pinKey(const QString &onion, const QByteArray &key); // false si une autre clé est déjà connue
    QJsonObject signedMessage(const QString &text) const;
    ChatEntry::Trust verifyMessage(const QJsonObject &msg, bool mine) const;

    // Synchronisation de l'historique : à chaque connexion, chacun envoie l'inventaire de ses
    // messages récents et reçoit en retour ceux qui lui manquent (signés par leur auteur).
    QJsonObject inventoryMessage() const;
    void sendMissing(PeerConnection *c, const QJsonArray &theirIds);
    void sendBatches(PeerConnection *c, QList<QJsonObject> messages); // filtrés pour ce membre
    void receiveSync(PeerConnection *from, const QJsonArray &messages);
    qint64 syncCutoff() const;                    // rien d'antérieur n'est échangé
    qint64 firstSeen(const QString &onion) const; // date de première rencontre d'un membre
    QString memberNick(const QString &onion) const;

    // Connexions
    void onNewConnection();
    void connectToMembers();
    void dial(const QString &onion);
    void watch(PeerConnection *c);
    void setActive(PeerConnection *c);
    void onReceived(PeerConnection *c, const QJsonObject &obj);
    void broadcast(const QJsonObject &obj, PeerConnection *except = nullptr);
    void closeAll();
    void sendCurrent();

    void changeNickname();
    void resetAll();
    QString nickname() const;
    QByteArray secret() const;

    QSettings m_settings;
    QTcpServer *m_server = nullptr; // 127.0.0.1 uniquement, joint via le service onion
    TorService *m_tor = nullptr;
    QString m_myOnion, m_torStatus, m_torError;
    bool m_inviteWanted = false;

    QHash<QString, PeerConnection *> m_active;   // onion -> connexion établie
    QHash<QString, PeerConnection *> m_attempts; // onion -> connexion sortante en cours
    QSet<PeerConnection *> m_pending;            // entrantes avant le hello
    QHash<QString, QString> m_lastError;         // onion -> dernière erreur
    QSet<QString> m_seenIds;                     // anti-doublon des messages relayés
    QSet<QString> m_identityWarned;              // avertissements d'identité déjà affichés
    QHash<QString, QJsonObject> m_store;         // historique signé : id -> message
    QTimer m_retryTimer;
    QTimer m_syncTimer; // inventaire de contrôle périodique
    int m_unread = 0;

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
