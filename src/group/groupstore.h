#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

class QSettings;

struct Member
{
    QString onion, nick;
};

// État persistant d'une conversation et de l'identité de ce poste, rangé dans les réglages.
// Communs à toutes les conversations : nickname, identity/seed, keyring (onion -> clé Ed25519),
// verified (onion -> clé vérifiée).
// Propres à la conversation, sous « conversations/<id>/ » (cf. conversations.h) :
//   group/{secret, members, shareHistoryUntil, records…}, firstSeen (onion -> ms), history/clearedBefore
// Sans identifiant de conversation : identité seule, ou ancienne disposition (migration).
class GroupStore
{
public:
    static constexpr int MaxMembers = 32; // maillage complet : au-delà, ce n'est plus raisonnable

    explicit GroupStore(QSettings &settings, const QString &conversation = {});
    QString conversation() const { return m_conversation; }

    // Reprise de la configuration de la version à deux participants (clés contact/*)
    void migrateLegacy();

    QString nickname() const;
    void setNickname(const QString &nick);

    // Secret du groupe (sert de mot de passe) ; vide : pas de groupe
    QByteArray secret() const;
    void setSecret(const QByteArray &secret);
    bool inGroup() const { return !secret().isEmpty(); }
    void leaveGroup(); // oublie le secret, les membres et la gouvernance (pas le trousseau ni l'historique)

    // Secrets précédents, encore acceptés jusqu'à leur date (membres absents lors d'un renouvellement)
    void retireSecret(const QByteArray &secret, qint64 untilMs);
    // Secret actuel puis précédents encore valables, du plus récent au plus ancien
    QList<QByteArray> acceptedSecrets(qint64 nowMs) const;

    // Gouvernance (cf. governance.h) : enregistrements vérifiés, dans l'ordre
    QList<QJsonObject> governanceRecords() const;
    void appendGovernanceRecord(const QJsonObject &record);
    QString expectedGroupId() const; // identifiant du groupe annoncé par le code d'invitation
    void setExpectedGroupId(const QString &id);
    bool isFounder() const; // nous avons créé ce groupe (avec cette version) : premier administrateur
    void setFounder(bool founder);
    // Membres partis d'eux-mêmes : plus réintroduits par les listes de membres des autres
    bool hasDeparted(const QString &onion) const;
    void setDeparted(const QString &onion, bool departed);

    // Identité Ed25519 de ce poste (la graine de 32 octets est la clé privée), et celle de la personne, commune à
    // tous ses postes liés (cf. person.h). ensureIdentity crée celles qui manquent.
    void ensureIdentity();
    QByteArray identitySeed() const;
    QByteArray identityKey() const;
    QByteArray personSeed() const;
    QByteArray personKey() const;
    qint64 personSince() const; // date de notre certificat de poste
    void setPersonSeed(const QByteArray &seed, qint64 sinceMs); // poste lié : la personne d'un autre poste

    // Certificats de poste reçus et vérifiés (cf. Person), communs à toutes les conversations. Le premier vu pour
    // une adresse fixe sa personne : un certificat d'une autre personne pour ce poste est refusé.
    QJsonObject certificate(const QString &onion) const;
    void setCertificate(const QString &onion, const QJsonObject &certificate);
    QByteArray personOf(const QString &onion) const; // vide : poste sans certificat (version précédente)
    bool isMyDevice(const QString &onion) const;    // un autre de nos postes

    QList<Member> members() const;
    void setMembers(const QList<Member> &list);
    // Ajoute un membre ou met à jour son pseudo ; true seulement si c'est un nouveau membre
    bool mergeMember(const QString &onion, const QString &nick, const QString &ownOnion);
    bool isMember(const QString &onion) const;
    QString nickOf(const QString &onion) const; // vide si inconnu

    // Trousseau : clé mémorisée à la première rencontre, conservée même après avoir quitté le
    // groupe (pour vérifier l'historique)
    QByteArray pinnedKey(const QString &onion) const;
    bool pinKey(const QString &onion, const QByteArray &pub, qint64 nowMs); // false si une autre clé est connue
    qint64 firstSeen(const QString &onion) const; // 0 : avant cette fonctionnalité, ou historique partagé

    // Avatars (cf. avatar.h), communs à toutes les conversations. Le nôtre : image JPEG (vide : aucun) et date du
    // choix (0 : jamais choisi) ; ceux des autres : enregistrements reçus et vérifiés (vide : inconnu)
    QByteArray ownAvatar() const;
    qint64 ownAvatarTime() const;
    void setOwnAvatar(const QByteArray &jpeg, qint64 timestampMs);
    QJsonObject avatarRecord(const QString &onion) const;
    void setAvatarRecord(const QString &onion, const QJsonObject &record);

    // Identité vérifiée de vive voix (code de sécurité comparé) : valable pour la clé mémorisée. Pour un poste
    // certifié, c'est la personne qui est vérifiée (tous ses postes à la fois) ; sinon, le poste et sa clé.
    bool isVerified(const QString &onion) const;
    void setVerified(const QString &onion, bool verified);

    // Partage de l'historique avec les personnes qui rejoignent avant cette date
    qint64 shareHistoryUntil() const;
    void setShareHistoryUntil(qint64 ms); // 0 : pas de partage

    // Nom partagé de la conversation (cf. Governance::createName), tel que reçu et vérifié
    QJsonObject nameRecord() const;
    void setNameRecord(const QJsonObject &record);
    // Avatar partagé de la conversation (cf. Avatar::createForConversation), tel que reçu et vérifié
    QJsonObject conversationAvatarRecord() const;
    void setConversationAvatarRecord(const QJsonObject &record);
    // Nom de la conversation choisi sur ce poste (vide : les pseudos des membres)
    QString name() const;
    void setName(const QString &name);
    // Notifications de cette conversation (cf. notifications.h) ; vide : réglage général
    QString notifyMode() const;
    void setNotifyMode(const QString &mode);

    // Historique vidé localement : rien d'antérieur n'est affiché ni re-téléchargé
    qint64 clearedBefore() const;
    void setClearedBefore(qint64 ms);

private:
    QString key(const QString &name) const { return m_prefix + name; } // clé propre à la conversation

    QSettings &m_settings;
    QString m_conversation, m_prefix;
};
