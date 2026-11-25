#pragma once
#include "attachment.h"
#include "chatview.h"
#include "conversations.h"
#include "groupnode.h"
#include "groupstore.h"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMainWindow>
#include <QPointer>
#include <QSet>
#include <QSettings>
#include <QTimer>

#include <memory>
#include <vector>

class QAction;
class QListWidget;
class QListWidgetItem;
class QLabel;
class QMenu;
class QMessageBox;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QSystemTrayIcon;
class Composer;
class MentionCompleter;
class QDragEnterEvent;
class QDropEvent;
class QResizeEvent;
class QMimeData;
class Notifier;
class QToolButton;
class TerminalSession;
class TerminalSharing;
class TorService;

class Switchboard;

// Fenêtre principale : la liste des conversations, celle affichée, Tor, les notifications et la console
// partagée. Chaque conversation (connexions, synchronisation, historique) est un GroupNode.

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    // Affiche la fenêtre (depuis la zone de notification, une notification ou une autre instance lancée)
    void showAndRaise();

protected:
    void changeEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    // Glisser-déposer d'une image n'importe où sur la fenêtre (la zone de saisie gère le sien et le collage)
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    // Une conversation : son état, son groupe, sa vue
    struct Conversation
    {
        QString id;
        std::unique_ptr<GroupStore> store;
        GroupNode *node = nullptr;
        ChatView *chat = nullptr;
        TerminalSharing *sharing = nullptr;
        QListWidgetItem *item = nullptr; // dans la colonne de gauche
        int unread = 0;
        QString preview; // dernier message, pour la colonne
        QString draft;   // texte en cours d'écriture quand on change de conversation
    };
    Conversation *openConversation(const QString &id); // existante (réglages, historique) ou nouvelle
    Conversation *newConversation();
    void closeConversation(Conversation *c); // la supprime : réglages, historique, fichiers
    Conversation *conversationById(const QString &id) const;
    void showConversation(Conversation *c);
    // m_group, m_node, m_chat, m_sharing : ceux de la conversation en contexte, en principe celle affichée ;
    // le temps de traiter un signal d'une autre, c'est elle (cf. inContext)
    void setContext(Conversation *c);
    template<typename F> void inContext(Conversation *c, F &&f)
    {
        Conversation *previous = m_ctx;
        setContext(c);
        f();
        setContext(previous);
    }
    bool isShown() const { return m_ctx == m_shown; }
    bool isBlank(const Conversation *c) const; // ni groupe ni historique : page d'accueil
    QString displayName(const Conversation *c) const;
    void updateConversationItem(Conversation *c);
    void startConversation(); // ⋮ → Nouvelle conversation
    void renameConversation();
    void deleteConversation();

    // Interface
    void buildUi();
    QWidget *buildSidebar(QWidget *parent);
    void setSidebarVisible(bool visible);
    void updateStatus();
    void connectNode(Conversation *c); // signaux du groupe -> interface
    void connectChat(Conversation *c);
    // msg : message signé tel qu'échangé sur le réseau (id, a, n, x, ts, s)
    void appendMessage(const QJsonObject &msg, bool mine, GroupNode::Trust trust);
    void onMessagesReceived(const QList<QJsonObject> &messages, bool catchUp); // notifications
    void appendSystem(const QString &text);
    // Pseudos reconnus dans les mentions « @… » (membres, nous-même et « tous »)
    QStringList mentionNames() const;
    bool mentionsMe(const QJsonObject &msg) const;
    bool repliesToMe(const QJsonObject &msg) const; // réponse à l'un de nos messages
    QString notificationTitle(const QJsonObject &msg) const;
    // Réponse avec citation : bandeau au-dessus de la zone de saisie
    void startReply(const QString &id, const QString &author, const QString &text);
    void cancelReply();
    // Image jointe au message en cours d'écriture (aperçu au-dessus de la zone de saisie)
    void chooseAttachment();
    void attachPath(const QString &path); // image, ou fichier joint
    bool attachFromMime(const QMimeData *mime); // false : rien d'utilisable
    void attachPrepared(const Attachment::Prepared &prepared);
    void cancelImage();
    QString notificationBody(const QJsonObject &msg) const;

    // Fichiers joints (cf. filestore.h)
    static ChatEntry::File::State fileEntryState(int groupNodeKind);
    void updateFileEntry(const QString &fileId);
    void onFileRequested(const QString &fileId, ChatView::FileAction action);
    void openFile(const QString &fileId);   // avertissement avant un programme ou un script
    void saveFileAs(const QString &fileId);

    // « Alice écrit… » : signal envoyé aux membres connectés, au plus toutes les 3 s pendant la frappe
    void onTyping();
    QString typingText() const; // vide si personne n'écrit

    // Réactions d'un message, pour l'affichage (noms des membres)
    QList<ChatEntry::Reaction> reactionSummary(const QString &id) const;

    // Recherche dans la conversation (Ctrl+F)
    void openSearch();
    void closeSearch();

    // Lien cliqué : ouvert dans le navigateur, hors Tor (confirmation la première fois)
    void openLink(const QString &url);
    void clearConversation();
    void setupTray();
    void updateUnread();

    // Tor
    void startTor();
    void onTorFailed(const QString &error);

    // Mise en relation
    void invite();
    void showInvitation();
    void join();

    QString memberNick(const QString &onion) const; // « Un membre » si inconnu
    // Message reçu alors que la fenêtre n'est pas au premier plan
    // firstTs : heure du plus ancien de ces messages (séparateur « Nouveaux messages ») ;
    // forMe : nous sommes mentionnés ou on nous répond (cf. réglage des notifications)
    void notifyUnread(int count, const QString &title, const QString &text, qint64 firstTs, bool forMe);
    bool notificationsAllowed(bool forMe) const;
    void buildNotificationMenu(QMenu *menu);

    void reconnectNow(); // Tor en panne : relancé ; sinon, le groupe rappelle tout le monde
    void sendCurrent();

    // Console partagée (Linux, macOS) : négociation dans TerminalSharing, ici les questions posées
    void setupTerminalSharing(Conversation *c);
    void shareTerminal();
    void askTerminalOffer(const QString &onion, const QString &nick, bool control);

    void changeNickname();
    // Code de sécurité (cf. safetynumber.h) : choix du membre, puis comparaison de vive voix
    void verifyMember();
    // Membres (⋮ → Membres…) : rôles, présence, exclusion et renouvellement du secret (administrateurs)
    void showMembers();
    void confirmExclude(const QString &onion);
    void confirmRenewSecret();
    void showSafetyNumber(const QString &onion);
    void resetAll();

    static QString dataDir();

    QSettings m_settings;
    Conversations m_conversations{m_settings, dataDir()};
    std::vector<std::unique_ptr<Conversation>> m_convs; // dans l'ordre de la colonne
    Conversation *m_shown = nullptr, *m_ctx = nullptr;
    GroupStore *m_group = nullptr;
    GroupNode *m_node = nullptr;
    Switchboard *m_board = nullptr;
    TorService *m_tor = nullptr;
    QString m_onion; // notre adresse, une fois Tor prêt
    QString m_torStatus, m_torError;
    QString m_inviteFor;      // conversation dont le code s'affichera quand Tor sera prêt
    QString m_lastNotified;   // conversation de la dernière notification (ouverte par un clic dessus)

    int m_unread = 0; // toutes conversations confondues

    TerminalSharing *m_sharing = nullptr;
    QPointer<QMessageBox> m_termPrompt; // « Ouvrir la console de … ? »
    QAction *m_termShareAct = nullptr, *m_termCtlAct = nullptr, *m_termStopAct = nullptr;

    ChatView *m_chat = nullptr;
    QStackedWidget *m_pages = nullptr; // accueil (conversation vierge), puis une vue par conversation
    QWidget *m_home = nullptr;
    QWidget *m_sidebar = nullptr;
    QListWidget *m_convList = nullptr;
    QToolButton *m_sidebarButton = nullptr;
    bool m_sidebarWanted = true; // choix de l'utilisateur (la colonne se replie d'elle-même si c'est étroit)
    Composer *m_input = nullptr;
    MentionCompleter *m_mentions = nullptr;
    QWidget *m_replyBar = nullptr;
    QLabel *m_replyLabel = nullptr;
    QString m_replyTo; // identifiant du message cité par le message en cours d'écriture
    QToolButton *m_attachButton = nullptr;
    QWidget *m_imageBar = nullptr;
    QLabel *m_imagePreview = nullptr, *m_imageInfo = nullptr;
    QByteArray m_pendingImage; // JPEG prêt à partir avec le prochain message
    QString m_pendingFile;     // ou fichier à joindre (chemin)
    QWidget *m_searchBar = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QLabel *m_searchCount = nullptr;
    QToolButton *m_sendButton = nullptr;
    QLabel *m_title = nullptr, *m_subtitle = nullptr;

    QSystemTrayIcon *m_tray = nullptr; // Linux / Windows
    Notifier *m_notifier = nullptr;
    bool m_hideOnClose = false;        // la croix masque la fenêtre au lieu de quitter
};
