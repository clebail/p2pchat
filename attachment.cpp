#include "attachment.h"

#include <QBuffer>
#include <QCoreApplication>
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
    const QSize size = reader.size();
    if ((format != "jpeg" && format != "png") || !size.isValid() || size.width() > 2 * MaxDimension
        || size.height() > 2 * MaxDimension)
        return {};
    return reader.read();
}

QImage Attachment::thumbnail(const QImage &image, int maxSide)
{
    if (image.isNull() || qMax(image.width(), image.height()) <= maxSide)
        return image;
    return image.scaled(maxSide, maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

bool Attachment::isImageFile(const QString &path)
{
    static const QStringList extensions{"jpg", "jpeg", "png", "gif", "bmp", "webp", "heic", "tif", "tiff"};
    return extensions.contains(QFileInfo(path).suffix().toLower());
}
