#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QString>

// Éléments communs du protocole : formats, limites et messages signés.
namespace Protocol {

constexpr int MaxNickLength = 40;

// Adresse de service onion v3 : 56 caractères base32 + ".onion"
bool isValidOnion(const QString &s);

// Base64 « URL » sans « = » final, utilisée partout dans les échanges
QString toB64(const QByteArray &b);
QByteArray fromB64(const QString &s);
QByteArray fromB64(const QJsonValue &v);

// Message de discussion signé par son auteur, tel qu'échangé et enregistré :
//   {"t":"msg", "id", "a" (onion de l'auteur), "n" (pseudo), "x" (texte), "ts" (ms), "s" (signature)}
//   + "r" : identifiant du message auquel il répond (facultatif)
namespace Message {
constexpr int MaxIdLength = 64;
// Ce que l'auteur signe : identifiant, adresse, pseudo, texte et heure, puis le message cité s'il y en a
// un (les messages sans citation gardent la signature des versions précédentes)
QByteArray signingBytes(const QJsonObject &m);
QJsonObject create(const QByteArray &identitySeed, const QString &authorOnion, const QString &nick,
                   const QString &text, qint64 timestampMs, const QString &replyTo = {});
bool verify(const QJsonObject &m, const QByteArray &authorKey);
QString id(const QJsonObject &m);
qint64 timestamp(const QJsonObject &m);
QString replyTo(const QJsonObject &m); // vide : pas une réponse
} // namespace Message

} // namespace Protocol
