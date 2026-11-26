#pragma once
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QStringList>

// Réactions (👍 ❤️ …) aux messages.
//
// Chaque réaction est un petit enregistrement signé par son auteur, synchronisé et relayé comme les
// messages (protocole version 7) :
//   {"t":"react", "id", "a" (onion de l'auteur), "m" (message visé), "re" (émoji),
//    "on" (ajoutée ou retirée), "ts" (ms), "s" (signature)}
// Pour un auteur, un message et un émoji, c'est l'enregistrement le plus récent qui compte.
namespace Reactions {

constexpr int Version = 7; // version du protocole qui comprend les réactions

// Émojis proposés ; les autres sont ignorés à la réception (pas de texte arbitraire)
extern const QStringList Allowed;

QJsonObject create(const QByteArray &identitySeed, const QString &authorOnion, const QString &target,
                   const QString &emoji, bool on, qint64 timestampMs);
bool isReaction(const QJsonObject &obj);
QByteArray signingBytes(const QJsonObject &r);
// Signature valide, émoji autorisé, message visé renseigné
bool verify(const QJsonObject &r, const QByteArray &authorKey);
QString target(const QJsonObject &r);

struct Summary
{
    QString emoji;
    QStringList authors; // onions, dans l'ordre d'arrivée ; vide pour la nôtre
    bool mine = false;   // nous en faisons partie
};

class State
{
public:
    // Prend en compte un enregistrement (déjà vérifié) ; true si l'affichage du message visé change
    bool apply(const QJsonObject &r, bool mine);
    // Réactions actives d'un message, dans l'ordre de Allowed
    QList<Summary> summary(const QString &target) const;
    bool mineOn(const QString &target, const QString &emoji) const;
    void clear() { m_state.clear(); }

private:
    struct Entry
    {
        qint64 ts = 0;
        QString id; // départage deux enregistrements de même heure
        bool on = false;
        bool mine = false;
        QString author, emoji;
    };
    QHash<QString, QHash<QString, Entry>> m_state; // message -> (auteur, émoji) -> dernier enregistrement
};

} // namespace Reactions
