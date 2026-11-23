#pragma once
#include <QDateTime>
#include <QHash>
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
class QToolButton;
class ChatView;
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

private:
    struct Member
    {
        QString onion, nick;
    };

    // Interface
    void buildUi();
    void updateStatus();
    void appendMessage(const QString &who, const QString &text, const QDateTime &ts, bool mine,
                       bool save = true);
    void appendSystem(const QString &text);
    void loadHistory();
    void clearConversation();

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
    QTimer m_retryTimer;
    int m_unread = 0;

    ChatView *m_chat = nullptr;
    QStackedWidget *m_pages = nullptr; // accueil (pas de groupe) / conversation
    QLineEdit *m_input = nullptr;
    QToolButton *m_sendButton = nullptr;
    QLabel *m_title = nullptr, *m_subtitle = nullptr;
    QString m_historyPath;
};
