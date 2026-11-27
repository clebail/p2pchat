#pragma once
#include "invitation.h"

#include <QJsonObject>
#include <QObject>
#include <QStringList>

#include <optional>

class GroupNode;
class GroupStore;

// Synchronisation entre nos postes liés (version 16 ; cf. person.h, devicelink.h).
//
// Nos postes forment entre eux un groupe caché (« self ») : un GroupNode ordinaire, qui se charge des connexions,
// sans message affiché. Chaque fois qu'un de nos postes s'y connecte, et à chaque changement, on lui envoie notre
// état (le plus récent l'emporte, élément par élément) :
//   {"t":"self-state", "n"/"nt" (pseudo et date), "a"/"at" (avatar et date), "w" (dictionnaire personnel : réuni),
//    "c":[{"g" (identifiant du groupe), "in" (membre ou parti), "ts", "n" (nom donné ici), "c" (code pour nous
//    rejoindre, préparé pour ce poste : historique partagé)}]}
// Une conversation rejointe sur un poste l'est sur tous ; quittée sur un poste, elle l'est sur tous. Seules les
// conversations avec gouvernance (identifiant de groupe) sont synchronisées.
namespace DeviceSync {

constexpr int MaxConversations = 256;

// Ce que la synchronisation demande à l'application (la fenêtre, ou un test)
class Hooks
{
public:
    virtual ~Hooks() = default;
    virtual QStringList joinedGroups() const = 0;                 // conversations dont nous sommes membre
    virtual QString localName(const QString &groupId) const = 0;  // nom donné sur ce poste (vide : aucun)
    // Code pour qu'un autre de nos postes nous y rejoigne (cf. GroupNode::invitationForMyDevice)
    virtual std::optional<Invitation> invitationFor(const QString &groupId, const QString &onion,
                                                    const QByteArray &key) = 0;
    virtual bool joinConversation(const Invitation &invitation, const QString &name) = 0;
    virtual void leaveConversation(const QString &groupId) = 0;
    virtual QStringList words() const = 0;
    // Reçus d'un autre de nos postes (déjà enregistrés pour le pseudo et l'avatar) : à annoncer, à afficher
    virtual void profileChanged(bool nickname, bool avatar, const QStringList &newWords) = 0;
};

class Engine : public QObject
{
    Q_OBJECT
public:
    // identity : réglages communs (pseudo, avatar, registre des conversations) ; self : le groupe de nos postes
    Engine(GroupStore &identity, GroupNode &self, Hooks &hooks, QObject *parent = nullptr);

    void noteLeft(const QString &groupId); // quittée ici : sur tous nos postes
    void publish();                         // quelque chose a changé ici : nos postes connectés sont prévenus
    void checkJoined();                     // conversation rejointe ici depuis : nos postes prévenus

    QJsonObject stateFor(const QString &onion); // ce qu'on envoie à ce poste
    void apply(const QString &from, const QJsonObject &state);

signals:
    void conversationJoined(const QString &groupId); // rejointe à la demande d'un autre de nos postes
    void conversationLeft(const QString &groupId);   // quittée sur un autre de nos postes

private:
    bool refresh(); // conversations rejointes ici depuis la dernière fois : notées (true : il y en a)
    void sendTo(const QString &onion);

    GroupStore &m_identity;
    GroupNode &m_self;
    Hooks &m_hooks;
};

} // namespace DeviceSync
