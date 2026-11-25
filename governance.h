#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QStringList>

#include <optional>

// Gouvernance du groupe : qui administre, qui est exclu, et quel secret est en vigueur.
//
// Une chaîne d'enregistrements signés, synchronisée comme les messages (protocole version 11) :
//   {"t":"group", "seq", "prev" (empreinte du précédent), "adm" (administrateurs), "exc" (exclus),
//    "sh" (empreinte SHA-256 du secret en vigueur), "a" (auteur), "ts", "s" (signature)}
// Le premier (seq 0) fait de son auteur l'unique administrateur ; son empreinte est l'identifiant du
// groupe, transmis avec les codes d'invitation. Chaque suivant est signé par un administrateur du
// précédent. Le secret lui-même ne figure jamais dans un enregistrement : il ne voyage que par les
// connexions chiffrées, vers les membres non exclus, qui le vérifient grâce à "sh".
namespace Governance {

constexpr int Version = 11;                                // version du protocole qui comprend tout cela
constexpr qint64 SecretGraceMs = 30LL * 24 * 3600 * 1000;  // ancien secret encore accepté (membres absents)

struct Record
{
    int seq = -1;
    QString prev;            // empreinte du précédent (vide pour le premier)
    QStringList admins;      // adresses onion
    QStringList excluded;    // adresses onion, toutes les exclusions en vigueur
    QByteArray secretHash;   // SHA-256 du secret en vigueur
    QString author;
    qint64 ts = 0;
    QJsonObject json;        // tel que signé et échangé

    QString hash() const;    // empreinte hexadécimale (identifiant du groupe pour le premier)
    bool isAdmin(const QString &onion) const { return admins.contains(onion); }
    bool isExcluded(const QString &onion) const { return excluded.contains(onion); }
};

QByteArray secretHash(const QByteArray &secret);
QByteArray signingBytes(const QJsonObject &record);

// Premier enregistrement (création du groupe, ou groupe d'une version précédente) ; previous == nullptr
// Suivant : changement d'administrateurs ou d'exclusions, nouveau secret
QJsonObject create(const QByteArray &identitySeed, const QString &authorOnion, const Record *previous,
                   const QStringList &admins, const QStringList &excluded, const QByteArray &secret,
                   qint64 timestampMs);

// Lecture sans vérification de signature (forme seulement)
std::optional<Record> parse(const QJsonObject &record);

enum class Check { Ok, Malformed, BadSignature, NotAdmin, NotNext, WrongGroup };
// Peut-on ajouter cet enregistrement après head (aucun : premier) ? authorKey : clé de son auteur.
// expectedGroup : identifiant du groupe connu par le code d'invitation (vide : le premier vu fait foi).
Check checkNext(const std::optional<Record> &head, const QJsonObject &candidate, const QByteArray &authorKey,
                const QString &expectedGroup = {});

} // namespace Governance
