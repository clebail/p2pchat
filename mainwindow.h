#pragma once
#include "attachment.h"
#include "chatview.h"
#include "conversation.h"
#include "conversations.h"
#include "groupnode.h"
#include "groupstore.h"
#include "invitation.h"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMainWindow>
#include <QPointer>
#include <QSet>
#include <QSettings>
#include <QTimer>

#include <functional>
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
    ~MainWindow() override;
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
    // Conversations (cf. conversation.h) : une affichée à la fois
    Conversation *openConversation(const QString &id); // existante (réglages, historique) ou nouvelle
    Conversation *newConversation();
    void closeConversation(Conversation *c); // la supprime : réglages, historique, fichiers
    Conversation *conversationById(const QString &id) const;
    void showConversation(Conversation *c);
    void updateConversationItem(Conversation *c);
    void onConversationChanged(Conversation *c);
    void startConversation(); // ⋮ → Nouvelle conversation
    void renameConversation();
    void deleteConversation();
    void appendSystem(const QString &text) { m_shown->appendSystem(text); }

    // Interface
    void buildUi();
    QWidget *buildSidebar(QWidget *parent);
    void setSidebarVisible(bool visible);
    void updateStatus(); // en-tête et zone de saisie, pour la conversation affichée
    // Réponse avec citation : bandeau au-dessus de la zone de saisie
    void startReply(const QString &id, const QString &author, const QString &text);
    void cancelReply(); // annule aussi une modification en cours
    // Modification d'un de nos messages : bandeau au-dessus de la zone de saisie, Entrée enregistre
    void startEdit(const QString &id, const QString &text);
    void confirmDelete(const QString &id);
    // Image jointe au message en cours d'écriture (aperçu au-dessus de la zone de saisie)
    void chooseAttachment();
    void attachPath(const QString &path); // image, ou fichier joint
    bool attachFromMime(const QMimeData *mime); // false : rien d'utilisable
    void attachPrepared(const Attachment::Prepared &prepared);
    void cancelImage();

    // Fichiers joints (cf. filestore.h)
    void openFile(const QString &fileId);   // avertissement avant un programme ou un script
    void saveFileAs(const QString &fileId);

    // « Alice écrit… » : signal envoyé aux membres connectés, au plus toutes les 3 s pendant la frappe
    void onTyping();

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
    void joinWith(const Invitation &invitation); // conversation déjà connue, vierge affichée, ou nouvelle
    // Invitation directe (version 12) : nouvelle conversation avec des membres de celle affichée
    void chooseConversationMembers();
    void startConversationWith(const QStringList &onions, const QString &name);
    void askDirectInvitation(Conversation *from, const QString &nick, const Invitation &invitation,
                             const QString &name);

    QString memberNick(const QString &onion) const { return m_shown->memberNick(onion); }
    // Messages reçus dans c : non-lus si elle est cachée ou la fenêtre en arrière-plan, puis notification
    // (cf. Conversation::messagesArrived)
    void onMessagesArrived(Conversation *c, int count, const QString &title, const QString &text, qint64 firstTs,
                           bool forMe);
    void buildNotificationMenu(QMenu *menu);

    void reconnectNow(); // Tor en panne : relancé ; sinon, le groupe rappelle tout le monde
    void sendCurrent();

    // Console partagée (Linux, macOS) : négociation dans TerminalSharing, ici les questions posées
    void setupTerminalSharing(Conversation *c);
    void shareTerminal();
    void askTerminalOffer(Conversation *c, const QString &nick, bool control);

    void changeNickname();
    void changeAvatar(); // le nôtre, commun à toutes les conversations (cf. avatar.h)
    void changeConversationAvatar(); // administrateurs : le même pour tous les membres
    // Choix d'un avatar : aperçu (current, sinon initiale de name), Choisir une image…, Retirer
    void avatarDialog(const QString &title, const QString &note, const std::function<QByteArray()> &current,
                      const std::function<QString()> &name,
                      const std::function<bool(const QByteArray &, QString *)> &apply);
    // Code de sécurité (cf. safetynumber.h) : choix du membre, puis comparaison de vive voix
    void verifyMember();
    // Membres (⋮ → Membres…) : rôles, présence, exclusion et renouvellement du secret (administrateurs)
    void showMembers();
    void confirmExclude(const QString &onion);
    void confirmAdmin(const QString &onion); // nommer, ou retirer le rôle (à un autre ou à nous)
    void confirmRenewSecret();
    void showSafetyNumber(const QString &onion);
    void resetAll();

    static QString dataDir();

    QSettings m_settings;
    Conversations m_conversations{m_settings, dataDir()};
    std::vector<std::unique_ptr<Conversation>> m_convs; // dans l'ordre de la colonne
    QHash<Conversation *, QListWidgetItem *> m_items;   // ligne de chacune dans la colonne
    QHash<Conversation *, TerminalSharing *> m_sharings; // console partagée de chacune
    // Conversation affichée, et raccourcis vers son état, son groupe, sa vue et sa console
    Conversation *m_shown = nullptr;
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
    QString m_editId;  // ou identifiant du message en cours de modification
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
    QLabel *m_titleAvatar = nullptr; // avatar de la conversation affichée
    static constexpr int ConvAvatarSide = 36, TitleAvatarSide = 40;

    QSystemTrayIcon *m_tray = nullptr; // Linux / Windows
    Notifier *m_notifier = nullptr;
    bool m_hideOnClose = false;        // la croix masque la fenêtre au lieu de quitter
};
