#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <optional>

#include "filestore.h"

// Éléments communs du protocole : formats, limites et messages signés.
namespace Protocol {

constexpr int MaxNickLength = 40;

// Version annoncée dans le hello. 4 : identité Ed25519 ; 5 : citations (réponses) ; 6 : images ;
// 7 : réactions (cf. reactions.h) ; 8 : GIF animés ; 9 : fichiers (cf. filestore.h).
// Les versions antérieures à MinVersion sont refusées.
constexpr int Version = 9;
constexpr int FilesVersion = 9;
constexpr int MinVersion = 4;

// Ce qu'un pair de cette version ne voit pas (pour l'avertir dans l'état des membres) ; vide s'il est à jour
QStringList missingFeatures(int peerVersion);

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
// Image jointe (version 6) : en plus,
//   "img" : JPEG ou PNG en base64 (cf. attachment.h)
//   "si"  : signature de (identifiant, auteur, empreinte SHA-256 de l'image)
//   "x" est alors la légende, ou ImagePlaceholder sans légende (texte vu par les versions précédentes)
// "s" ne couvre ni la citation ni l'image : les versions précédentes vérifient, affichent (sans
// citation ni image), relaient et synchronisent ces messages tels quels. Un relais peut retirer une
// citation ou une image (le message reste authentique), mais pas la modifier ni en ajouter une.
namespace Message {
constexpr int MaxIdLength = 64;
// Ce que l'auteur signe dans "s" : identifiant, adresse, pseudo, texte et heure
QByteArray signingBytes(const QJsonObject &m);
// Ce que l'auteur signe dans "sr" : la même chose, plus le message cité
QByteArray replySigningBytes(const QJsonObject &m);
// Ce que l'auteur signe dans "si" : identifiant, auteur et empreinte de l'image
QByteArray imageSigningBytes(const QJsonObject &m, const QByteArray &image);
// Ce que l'auteur signe dans "sf" : identifiant, auteur et fiche du fichier joint, champs tels que reçus
QByteArray fileSigningBytes(const QJsonObject &m);
extern const QString ImagePlaceholder;
// Texte vu par les versions sans fichiers (et notre légende quand il n'y en a pas)
QString filePlaceholder(const QJsonObject &fileInfo);

struct Extras
{
    QString replyTo;  // message cité
    QByteArray image; // JPEG ou PNG, déjà réduit (Attachment::encode)
    QJsonObject file; // fiche d'un fichier joint (Files::FileInfo::toJson)
};
QJsonObject create(const QByteArray &identitySeed, const QString &authorOnion, const QString &nick,
                   const QString &text, qint64 timestampMs, const Extras &extras = {});
bool verify(const QJsonObject &m, const QByteArray &authorKey);
QString id(const QJsonObject &m);
qint64 timestamp(const QJsonObject &m);
// Message cité, seulement si sa signature "sr" est valide ; vide sinon (pas une réponse, ou citation douteuse)
QString verifiedReplyTo(const QJsonObject &m, const QByteArray &authorKey);
// Image jointe, seulement si sa signature "si" est valide ; vide sinon
QByteArray verifiedImage(const QJsonObject &m, const QByteArray &authorKey);
// Fichier joint ("f"), seulement si sa signature "sf" est valide et sa fiche correcte ; nullopt sinon
std::optional<Files::FileInfo> verifiedFile(const QJsonObject &m, const QByteArray &authorKey);
// Texte à afficher : la légende (vide pour une image ou un fichier sans légende)
QString caption(const QJsonObject &m, bool hasAttachment);
} // namespace Message

} // namespace Protocol
