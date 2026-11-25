#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QString>

// Éléments communs du protocole : formats, limites et messages signés.
namespace Protocol {

constexpr int MaxNickLength = 40;

// Version annoncée dans le hello. 4 : identité Ed25519 ; 5 : citations (réponses).
// Les versions antérieures à MinVersion sont refusées.
constexpr int Version = 5;
constexpr int MinVersion = 4;

// Adresse de service onion v3 : 56 caractères base32 + ".onion"
bool isValidOnion(const QString &s);

// Base64 « URL » sans « = » final, utilisée partout dans les échanges
QString toB64(const QByteArray &b);
QByteArray fromB64(const QString &s);
QByteArray fromB64(const QJsonValue &v);

// Message de discussion signé par son auteur, tel qu'échangé et enregistré :
//   {"t":"msg", "id", "a" (onion de l'auteur), "n" (pseudo), "x" (texte), "ts" (ms), "s" (signature)}
// Réponse avec citation (version 5) : en plus,
//   "r"  : identifiant du message cité
//   "sr" : signature de l'ensemble, citation comprise
// "s" ne couvre pas la citation : les versions précédentes vérifient, affichent (sans citation),
// relaient et synchronisent les réponses telles quelles. Un relais peut retirer la citation (le message
// reste authentique), mais pas la modifier ni en ajouter une.
namespace Message {
constexpr int MaxIdLength = 64;
// Ce que l'auteur signe dans "s" : identifiant, adresse, pseudo, texte et heure
QByteArray signingBytes(const QJsonObject &m);
// Ce que l'auteur signe dans "sr" : la même chose, plus le message cité
QByteArray replySigningBytes(const QJsonObject &m);
QJsonObject create(const QByteArray &identitySeed, const QString &authorOnion, const QString &nick,
                   const QString &text, qint64 timestampMs, const QString &replyTo = {});
bool verify(const QJsonObject &m, const QByteArray &authorKey);
QString id(const QJsonObject &m);
qint64 timestamp(const QJsonObject &m);
// Message cité, seulement si sa signature "sr" est valide ; vide sinon (pas une réponse, ou citation douteuse)
QString verifiedReplyTo(const QJsonObject &m, const QByteArray &authorKey);
} // namespace Message

} // namespace Protocol
