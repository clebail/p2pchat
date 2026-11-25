#pragma once
#include <QByteArray>
#include <QImage>
#include <QString>

// Images jointes aux messages.
//
// Une image voyage dans le message lui-même (une trame fait au plus 256 Ko) : elle est réduite et
// recompressée en JPEG d'au plus MaxBytes avant l'envoi. À la réception, seuls JPEG et PNG de taille
// raisonnable sont décodés (pas d'« image bombe »).
namespace Attachment {

constexpr int MaxBytes = 150 * 1024;      // JPEG envoyé (≈ 200 Ko en base64 : tient dans une trame)
constexpr int MaxDimension = 1600;        // plus grand côté de l'image envoyée
constexpr int MaxDecodedSide = 8192;      // image choisie ou déposée : au-delà, refusée
constexpr qint64 MaxFileBytes = 30 << 20; // fichier choisi ou déposé
constexpr int ThumbnailSide = 480;        // image gardée pour l'affichage dans la liste

// Fichier image choisi ou déposé (orientation EXIF appliquée) ; image nulle et message si impossible
QImage load(const QString &path, QString *error = nullptr);
// Réduction + JPEG d'au plus maxBytes (transparence sur fond blanc) ; vide si impossible
QByteArray encode(const QImage &image, int maxBytes = MaxBytes);
// Image reçue : JPEG ou PNG seulement, dimensions bornées ; image nulle sinon
QImage decode(const QByteArray &bytes);
// Version réduite pour l'affichage
QImage thumbnail(const QImage &image, int maxSide = ThumbnailSide);

bool isImageFile(const QString &path); // d'après l'extension, pour le glisser-déposer

} // namespace Attachment
