#include "attachment.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QPainter>

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("Attachment", text);
}

bool sizeAcceptable(const QSize &s)
{
    return s.isValid() && s.width() <= Attachment::MaxDecodedSide && s.height() <= Attachment::MaxDecodedSide;
}

QByteArray toJpeg(const QImage &image, int quality)
{
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, "jpeg");
    writer.setQuality(quality);
    writer.setOptimizedWrite(true);
    return writer.write(image) ? out : QByteArray();
}

// Structure d'un GIF, lue sans rien décoder. Qt5 ne s'y prête pas : QImageReader::size() y renvoie une
// taille fausse, alors que read() alloue l'écran logique entier annoncé dans l'en-tête (un GIF de quelques
// octets annonçant 65535 × 65535 coûterait 17 Go).
struct GifInfo
{
    bool valid = false;
    int width = 0, height = 0; // écran logique, agrandi aux images qui en dépassent
    int frames = 0;
};

GifInfo gifInfo(const QByteArray &d)
{
    GifInfo info;
    const int n = int(d.size());
    auto u8 = [&](int i) { return quint8(d.at(i)); };
    auto le16 = [&](int i) { return int(u8(i)) | (int(u8(i + 1)) << 8); };
    // Suite de sous-blocs (longueur, données…) terminée par un bloc vide ; -1 si tronquée
    auto skipSubBlocks = [&](int i) {
        while (i < n && u8(i) != 0)
            i += 1 + u8(i);
        return i < n ? i + 1 : -1;
    };
    if (n < 13 || (!d.startsWith("GIF87a") && !d.startsWith("GIF89a")))
        return info;
    info.width = le16(6);
    info.height = le16(8);
    int i = 13;
    if (u8(10) & 0x80) // palette globale
        i += 3 * (1 << ((u8(10) & 7) + 1));
    while (i >= 0 && i < n) {
        const quint8 block = u8(i);
        if (block == 0x3b) { // fin
            info.valid = info.frames > 0;
            return info;
        }
        if (block == 0x21 && i + 1 < n) { // extension
            i = skipSubBlocks(i + 2);
        } else if (block == 0x2c && i + 10 <= n) { // image
            info.width = qMax(info.width, le16(i + 1) + le16(i + 5));
            info.height = qMax(info.height, le16(i + 3) + le16(i + 7));
            const quint8 packed = u8(i + 9);
            i += 10;
            if (packed & 0x80) // palette locale
                i += 3 * (1 << ((packed & 7) + 1));
            i = i < n ? skipSubBlocks(i + 1) : -1; // taille de code LZW, puis données
            ++info.frames;
        } else {
            return info; // bloc inconnu ou fichier tronqué
        }
    }
    return info;
}

bool gifAcceptable(const GifInfo &g)
{
    return g.valid && g.width > 0 && g.height > 0 && g.width <= 2 * Attachment::MaxDimension
           && g.height <= 2 * Attachment::MaxDimension && g.frames <= Attachment::MaxGifFrames;
}

} // namespace

QImage Attachment::load(const QString &path, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QImage();
    };
    const QFileInfo info(path);
    if (!info.isFile())
        return fail(tr("fichier introuvable"));
    if (info.size() > MaxFileBytes)
        return fail(tr("fichier trop volumineux (30 Mo au plus)"));
    QImageReader reader(path);
    reader.setAutoTransform(true); // photos de téléphone : orientation EXIF
    if (!reader.canRead())
        return fail(tr("ce n'est pas une image reconnue"));
    if (!sizeAcceptable(reader.size()))
        return fail(tr("image trop grande"));
    if (reader.format() == "gif") { // QImageReader::size() n'est pas fiable pour les GIF (cf. gifInfo)
        QFile f(path);
        const GifInfo g = f.open(QIODevice::ReadOnly) ? gifInfo(f.readAll()) : GifInfo();
        if (!g.valid || !sizeAcceptable(QSize(g.width, g.height)))
            return fail(tr("image GIF trop grande ou abîmée"));
    }
    const QImage image = reader.read();
    if (image.isNull())
        return fail(tr("image illisible : %1").arg(reader.errorString()));
    return image;
}

QByteArray Attachment::encode(const QImage &source, int maxBytes)
{
    if (source.isNull())
        return {};
    // JPEG n'a pas de transparence : on la pose sur du blanc
    QImage image(source.size(), QImage::Format_RGB32);
    image.fill(Qt::white);
    {
        QPainter p(&image);
        p.drawImage(0, 0, source);
    }
    if (qMax(image.width(), image.height()) > MaxDimension)
        image = image.scaled(MaxDimension, MaxDimension, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    // Qualité dégressive, puis réduction si cela ne suffit pas
    for (;;) {
        for (int quality : {85, 75, 65, 55, 45}) {
            const QByteArray jpeg = toJpeg(image, quality);
            if (jpeg.isEmpty())
                return {};
            if (jpeg.size() <= maxBytes)
                return jpeg;
        }
        if (qMax(image.width(), image.height()) <= 64)
            return {};
        image = image.scaled(image.width() * 3 / 4, image.height() * 3 / 4, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
    }
}

QImage Attachment::decode(const QByteArray &bytes)
{
    if (bytes.isEmpty() || bytes.size() > MaxBytes * 2)
        return {};
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    // Nous n'envoyons pas plus de MaxDimension : un PNG de quelques Ko peut sinon décompresser en Go
    const QByteArray format = reader.format();
    if (format == "gif") {
        if (!gifAcceptable(gifInfo(bytes))) // structure lue par nous : cf. gifInfo
            return {};
    } else {
        const QSize size = reader.size();
        if ((format != "jpeg" && format != "png") || !size.isValid() || size.width() > 2 * MaxDimension
            || size.height() > 2 * MaxDimension)
            return {};
    }
    return reader.read();
}

bool Attachment::isAnimated(const QByteArray &bytes)
{
    const GifInfo g = gifInfo(bytes);
    return g.valid && g.frames > 1;
}

Attachment::Prepared Attachment::prepareImage(const QImage &image)
{
    Prepared p;
    p.data = encode(image);
    if (p.data.isEmpty()) {
        p.error = tr("image illisible");
        return p;
    }
    p.preview = QImage::fromData(p.data);
    return p;
}

Attachment::Prepared Attachment::prepareFile(const QString &path)
{
    Prepared p;
    QFile file(path);
    const QFileInfo info(path);
    p.sourceBytes = info.size();
    // GIF animé assez léger : envoyé tel quel, pour garder l'animation
    if (info.isFile() && info.size() <= MaxBytes && file.open(QIODevice::ReadOnly)) {
        const QByteArray bytes = file.readAll();
        if (isAnimated(bytes)) {
            const QImage first = decode(bytes); // mêmes contrôles qu'à la réception
            if (!first.isNull() && qMax(first.width(), first.height()) <= MaxDimension) {
                p.data = bytes;
                p.preview = first;
                p.animated = true;
                return p;
            }
        }
    } else if (info.isFile() && info.size() <= MaxFileBytes && file.open(QIODevice::ReadOnly)) {
        p.frozen = isAnimated(file.readAll()); // trop lourd pour partir tel quel
        file.close();
    }
    const QImage image = load(path, &p.error);
    if (image.isNull())
        return p;
    const Prepared fixed = prepareImage(image);
    p.data = fixed.data;
    p.preview = fixed.preview;
    p.error = fixed.error;
    return p;
}

QImage Attachment::thumbnail(const QImage &image, int maxSide)
{
    if (image.isNull() || qMax(image.width(), image.height()) <= maxSide)
        return image;
    return image.scaled(maxSide, maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

namespace {

// Formats d'images que ce Qt sait lire (selon les modules installés : WebP, HEIC… ou pas)
QStringList readableExtensions()
{
    static const QStringList extensions = [] {
        QStringList out;
        for (const QByteArray &f : QImageReader::supportedImageFormats()) {
            const QString ext = QString::fromLatin1(f).toLower();
            if (ext != "svg" && ext != "svgz" && ext != "ico" && ext != "cur" && ext != "pbm" && ext != "pgm"
                && ext != "ppm" && ext != "xbm" && ext != "xpm")
                out << ext; // formats d'icônes et de dessin vectoriel : pas des photos
        }
        if (out.contains("jpeg") && !out.contains("jpg"))
            out << "jpg";
        return out;
    }();
    return extensions;
}

} // namespace

bool Attachment::isImageFile(const QString &path)
{
    return readableExtensions().contains(QFileInfo(path).suffix().toLower());
}

QString Attachment::fileDialogFilter()
{
    QStringList patterns;
    for (const QString &ext : readableExtensions())
        patterns << "*." + ext;
    return tr("Images (%1)").arg(patterns.join(' '));
}
