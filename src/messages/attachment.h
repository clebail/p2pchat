#pragma once
#include <QByteArray>
#include <QImage>
#include <QString>

// Images jointes aux messages.
//
// Une image voyage dans le message lui-même (une trame fait au plus 256 Ko) : elle est réduite et
// recompressée en JPEG d'au plus MaxBytes avant l'envoi. Exception : un GIF animé qui tient déjà dans
// MaxBytes part tel quel (Qt sait lire les GIF animés, pas les écrire) ; plus lourd, il part figé.
// À la réception, seuls JPEG, PNG et GIF de taille raisonnable sont décodés (pas d'« image bombe »).
namespace Attachment {

constexpr int MaxBytes = 150 * 1024;      // JPEG envoyé (≈ 200 Ko en base64 : tient dans une trame)
constexpr int MaxDimension = 1600;        // plus grand côté de l'image envoyée
constexpr int MaxDecodedSide = 8192;      // image choisie ou déposée : au-delà, refusée
constexpr qint64 MaxFileBytes = 30 << 20; // fichier choisi ou déposé
constexpr int ThumbnailSide = 480;        // image gardée pour l'affichage dans la liste
constexpr int MaxGifFrames = 500;         // GIF animé : au-delà, refusé (décodage trop coûteux)

// Pièce jointe prête à partir
struct Prepared
{
    QByteArray data;     // JPEG, ou GIF animé tel quel ; vide si impossible
    QImage preview;      // première image
    bool animated = false;
    bool frozen = false; // GIF animé trop lourd : envoyé figé (première image)
    qint64 sourceBytes = 0;
    QString error;
};
// Fichier choisi ou déposé
Prepared prepareFile(const QString &path);
// Image collée (presse-papiers) : toujours fixe
Prepared prepareImage(const QImage &image);
// GIF de plusieurs images
bool isAnimated(const QByteArray &bytes);

// Fichier image choisi ou déposé (orientation EXIF appliquée) ; image nulle et message si impossible
QImage load(const QString &path, QString *error = nullptr);
// Réduction + JPEG d'au plus maxBytes (transparence sur fond blanc) ; vide si impossible
QByteArray encode(const QImage &image, int maxBytes = MaxBytes);
// Image reçue : JPEG, PNG ou GIF seulement, dimensions bornées ; image nulle sinon (GIF : première image)
QImage decode(const QByteArray &bytes);
// Version réduite pour l'affichage
QImage thumbnail(const QImage &image, int maxSide = ThumbnailSide);

bool isImageFile(const QString &path); // d'après l'extension : formats que Qt sait lire ici
QString fileDialogFilter();            // « Images (*.jpg *.png …) » pour le sélecteur de fichier

} // namespace Attachment
