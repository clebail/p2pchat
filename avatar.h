#pragma once
#include <QByteArray>
#include <QImage>
#include <QJsonObject>
#include <QString>

// Avatar d'un membre (version 14) : petite image carrée choisie par son auteur, commune à toutes ses
// conversations. Elle circule dans un enregistrement signé par son auteur, que les autres relaient ; le plus
// récent l'emporte. {"t":"avatar", "a" (auteur), "ts", "img" (JPEG en base64 ; vide : avatar retiré), "s"}
namespace Avatar {

constexpr int Version = 14;
constexpr int Side = 96;               // pixels : de quoi afficher 32 points sur un écran haute densité
constexpr int MaxBytes = 16 * 1024;    // JPEG reçu au-delà : refusé

// Image choisie par l'utilisateur : recadrée au centre, carrée, réduite, en JPEG ; vide si inutilisable
QByteArray fromImage(const QImage &image);
// Image reçue : JPEG de taille raisonnable, sinon image nulle
QImage decode(const QByteArray &jpeg);

QJsonObject create(const QByteArray &identitySeed, const QString &authorOnion, const QByteArray &jpeg,
                   qint64 timestampMs);
// Forme, image et signature (authorKey : clé connue de son auteur)
bool verify(const QJsonObject &record, const QByteArray &authorKey);

QString author(const QJsonObject &record);
qint64 timestamp(const QJsonObject &record); // 0 : aucun enregistrement
QByteArray image(const QJsonObject &record); // vide : pas d'avatar (jamais choisi, ou retiré)

} // namespace Avatar
