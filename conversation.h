#pragma once
#include "chatview.h"
#include "groupnode.h"
#include "groupstore.h"
#include "invitation.h"

#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointer>

#include <memory>
#include <optional>

class QSettings;

// Une conversation affichable : son état (GroupStore), son groupe (GroupNode) et sa vue (ChatView).
// Elle traite elle-même les signaux de son groupe (messages, lignes système, réactions, fichiers) et
// calcule ce que la fenêtre montre d'elle (nom, aperçu, ligne d'état, notifications). La fenêtre
// principale (MainWindow) en affiche une à la fois et reçoit ses signaux.
class Conversation : public QObject
{
    Q_OBJECT
public:
    Conversation(QSettings &settings, const QString &id, const QString &historyPath,
                 const GroupNode::Timing &timing = {}, QObject *parent = nullptr);
    ~Conversation() override;

    QString id() const { return m_id; }
    GroupStore &store() { return *m_store; }
    const GroupStore &store() const { return *m_store; }
    GroupNode &node() { return *m_node; }
    const GroupNode &node() const { return *m_node; }
    ChatView *view() const { return m_view; } // à placer dans la fenêtre (cf. MainWindow)

    void loadHistory(); // les derniers messages enregistrés, dans la vue
    void leave();       // quitte le groupe ; la conversation garde son nom (historique conservé)

    // ---- Ce que la fenêtre en montre
    bool isBlank() const;         // ni groupe ni historique : page d'accueil
    QString displayName() const;  // nom partagé, sinon pseudos des membres (après un départ : dernier nom connu)
    QString preview() const { return m_preview; } // dernier message, pour la colonne
    QString subtitle() const;     // ligne d'état (HTML) : membres en ligne, « X écrit… »
    QString subtitleToolTip() const; // état détaillé de chaque membre (HTML)
    QString typingText() const;   // vide si personne n'écrit
    QString memberNick(const QString &onion) const; // « Un membre » si inconnu
    // Avatar d'un membre (cf. avatar.h), image nulle s'il n'en a pas. Les avatars sont communs à toutes les
    // conversations : forgetAvatar est appelé sur chacune quand l'une en reçoit un nouveau.
    QImage avatarOf(const QString &onion) const;
    void forgetAvatar(const QString &onion);
    QImage conversationAvatar() const; // choisi par un administrateur ; image nulle : aucun
    QStringList mentionNames() const; // pseudos reconnus dans les « @… » (membres, nous, « tous »)

    // ---- Non-lus (la fenêtre décide quand en ajouter : conversation cachée ou fenêtre en arrière-plan)
    int unread() const { return m_unread; }
    void addUnread(int count, qint64 firstTs); // séparateur « Nouveaux messages » à partir de firstTs
    void markRead();                            // retour sur la conversation
    QString draft; // texte en cours d'écriture quand on change de conversation

    void appendSystem(const QString &text);
    void setAuthorVerified(const QString &onion, bool verified) { m_view->setAuthorVerified(onion, verified); }
    // Notifications de cette conversation (cf. notifications.h, réglage « ne pas déranger » à part)
    bool notificationsAllowed(bool forMe, qint64 pausedUntil, qint64 nowMs) const;
    static QString notificationBody(const QJsonObject &msg); // texte, ou « 📎 nom », « 📷 légende »

signals:
    void changed(); // nom, aperçu, non-lus, membres, connexions : colonne et en-tête à redessiner
    // Messages reçus : de quoi notifier (count > 1 : rattrapage groupé)
    void messagesArrived(int count, const QString &title, const QString &text, qint64 firstTs, bool forMe);
    void excludedFromGroup(const QString &byNick);
    void avatarChanged(const QString &onion); // reçu par cette conversation : les autres oublient l'ancien
    void directInvitation(const QString &fromNick, const Invitation &invitation, const QString &name);
    // Actions de la vue qui demandent la fenêtre (citation, dialogues)
    void replyRequested(const QString &id, const QString &author, const QString &text);
    void editRequested(const QString &id, const QString &text);
    void deleteRequested(const QString &id);
    void linkClicked(const QString &url);
    void openFileRequested(const QString &fileId);
    void saveFileRequested(const QString &fileId);

private:
    void connectNode();
    void connectView();
    void appendMessage(const QJsonObject &msg, bool mine, GroupNode::Trust trust);
    void onMessagesReceived(const QList<QJsonObject> &messages, bool catchUp);
    bool mentionsMe(const QJsonObject &msg) const;
    bool repliesToMe(const QJsonObject &msg) const;
    QString notificationTitle(const QJsonObject &msg) const;
    QList<ChatEntry::Reaction> reactionSummary(const QString &id) const;
    void updateFileEntry(const QString &fileId);
    void onMessageEdited(const QString &id); // modifié ou supprimé : la vue (et l'aperçu) suivent
    void rememberName(); // nom affiché tant qu'on est membre, gardé quand on ne l'est plus
    void keepName();

    QString m_id;
    std::unique_ptr<GroupStore> m_store;
    std::unique_ptr<GroupNode> m_node;
    QPointer<ChatView> m_view; // sans parent : rangée ensuite dans la fenêtre, supprimée avec nous
    int m_unread = 0;
    QString m_preview, m_previewId; // dernier message affiché, et son identifiant
    QString m_knownName;
    mutable QHash<QString, QImage> m_avatars; // décodés (image nulle : pas d'avatar)
    mutable std::optional<QImage> m_conversationAvatar; // décodé à la première demande
};
