#pragma once
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <optional>

// Modification et suppression d'un message par son auteur (protocole version 13).
//
// Comme les réactions : un petit enregistrement signé, rangé dans l'historique, synchronisé et relayé :
//   {"t":"edit", "id", "a" (onion de l'auteur), "m" (message visé), "ed" (nouveau texte), "del" (supprimé),
//    "ts" (ms), "s" (signature)}
// Il ne vaut que s'il vient de l'auteur du message visé ou d'un administrateur du groupe (modération ;
// vérifié à l'affichage, le message pouvant arriver après) ; une suppression l'emporte, puis le plus récent. Une suppression masque le message chez les membres à jour, sans
// garantir qu'il disparaisse de leur disque.
namespace Edits {

constexpr int Version = 13;
constexpr int MaxTextLength = 4000;

QJsonObject create(const QByteArray &identitySeed, const QString &authorOnion, const QString &target,
                   const QString &text, bool deleted, qint64 timestampMs);
bool isEdit(const QJsonObject &obj);
QByteArray signingBytes(const QJsonObject &r);
bool verify(const QJsonObject &r, const QByteArray &authorKey); // signature, message visé, longueur
QString target(const QJsonObject &r);

struct Latest
{
    QString text;
    bool deleted = false;
    qint64 ts = 0;
    QString id;     // départage deux enregistrements de même heure
    QString author; // l'auteur du message, ou un administrateur
};

class State
{
public:
    // Prend en compte un enregistrement (déjà vérifié) ; true s'il remplace la version connue
    bool apply(const QJsonObject &r);
    // Version qui l'emporte parmi celles de ces auteurs (aucune : jamais modifié)
    std::optional<Latest> latest(const QString &target, const QStringList &authors) const;
    QStringList targets() const { return m_state.keys(); } // messages modifiés
    void clear() { m_state.clear(); }

private:
    QHash<QString, QHash<QString, Latest>> m_state; // message -> auteur -> dernière version
};

} // namespace Edits
