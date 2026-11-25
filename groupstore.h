#pragma once
#include <QByteArray>
#include <QList>
#include <QString>

class QSettings;

struct Member
{
    QString onion, nick;
};

// État persistant du groupe et de l'identité de ce poste, rangé dans les réglages :
//   nickname, identity/seed, group/{secret, members, shareHistoryUntil},
//   keyring (onion -> clé Ed25519), firstSeen (onion -> ms), verified (onion -> clé vérifiée),
//   history/clearedBefore
class GroupStore
{
public:
    static constexpr int MaxMembers = 32; // maillage complet : au-delà, ce n'est plus raisonnable

    explicit GroupStore(QSettings &settings);

    // Reprise de la configuration de la version à deux participants (clés contact/*)
    void migrateLegacy();

    QString nickname() const;
    void setNickname(const QString &nick);

    // Secret du groupe (sert de mot de passe) ; vide : pas de groupe
    QByteArray secret() const;
    void setSecret(const QByteArray &secret);
    bool inGroup() const { return !secret().isEmpty(); }
    void leaveGroup(); // oublie le secret et les membres (pas le trousseau ni l'historique)

    // Identité Ed25519 de ce poste (la graine de 32 octets est la clé privée)
    void ensureIdentity();
    QByteArray identitySeed() const;
    QByteArray identityKey() const;

    QList<Member> members() const;
    void setMembers(const QList<Member> &list);
    // Ajoute un membre ou met à jour son pseudo ; true seulement si c'est un nouveau membre
    bool mergeMember(const QString &onion, const QString &nick, const QString &ownOnion);
    bool isMember(const QString &onion) const;
    QString nickOf(const QString &onion) const; // vide si inconnu

    // Trousseau : clé mémorisée à la première rencontre, conservée même après avoir quitté le
    // groupe (pour vérifier l'historique)
    QByteArray pinnedKey(const QString &onion) const;
    bool pinKey(const QString &onion, const QByteArray &key, qint64 nowMs); // false si une autre clé est connue
    qint64 firstSeen(const QString &onion) const; // 0 : avant cette fonctionnalité, ou historique partagé

    // Identité vérifiée de vive voix (code de sécurité comparé) : valable pour la clé mémorisée
    bool isVerified(const QString &onion) const;
    void setVerified(const QString &onion, bool verified);

    // Partage de l'historique avec les personnes qui rejoignent avant cette date
    qint64 shareHistoryUntil() const;
    void setShareHistoryUntil(qint64 ms); // 0 : pas de partage

    // Historique vidé localement : rien d'antérieur n'est affiché ni re-téléchargé
    qint64 clearedBefore() const;
    void setClearedBefore(qint64 ms);

private:
    QSettings &m_settings;
};
