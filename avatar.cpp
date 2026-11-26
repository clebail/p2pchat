#include "avatar.h"

#include "crypto.h"
#include "protocol.h"

#include <QBuffer>
#include <QImageReader>
#include <QImageWriter>
#include <QPainter>

namespace {

// Membre : {contexte, auteur, date, image} ; conversation : identifiant du groupe en plus
QByteArray signingBytes(const QJsonObject &r, const QString &groupId = {})
{
    QList<QByteArray> parts{groupId.isEmpty() ? "p2pchat-avatar-v1" : "p2pchat-conv-avatar-v1"};
    if (!groupId.isEmpty())
        parts << groupId.toLatin1();
    parts << r.value("a").toString().toUtf8() << QByteArray::number(qint64(r.value("ts").toDouble()))
          << Protocol::fromB64(r.value("img"));
    return Crypto::pack(parts);
}

bool wellFormed(const QJsonObject &r, const QByteArray &authorKey)
{
    const QByteArray img = Avatar::image(r);
    return !authorKey.isEmpty() && Protocol::isValidOnion(Avatar::author(r)) && Avatar::timestamp(r) > 0
           && (img.isEmpty() || !Avatar::decode(img).isNull());
}

QByteArray toJpeg(const QImage &image, int quality)
{
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, "jpeg");
    writer.setQuality(quality);
    return writer.write(image) ? out : QByteArray();
}

} // namespace

QByteArray Avatar::fromImage(const QImage &source)
{
    if (source.isNull())
        return {};
    // Carré central, sur fond blanc (transparence d'un PNG), réduit
    const int side = qMin(source.width(), source.height());
    const QRect crop((source.width() - side) / 2, (source.height() - side) / 2, side, side);
    QImage square(Side, Side, QImage::Format_RGB32);
    square.fill(Qt::white);
    {
        QPainter p(&square);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(QRect(0, 0, Side, Side), source, crop);
    }
    for (int quality = 85; quality >= 40; quality -= 15) {
        const QByteArray jpeg = toJpeg(square, quality);
        if (!jpeg.isEmpty() && jpeg.size() <= MaxBytes)
            return jpeg;
    }
    return {};
}

QImage Avatar::decode(const QByteArray &jpeg)
{
    if (jpeg.isEmpty() || jpeg.size() > MaxBytes)
        return {};
    QBuffer buffer;
    buffer.setData(jpeg);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "jpeg");
    // Taille lue dans l'en-tête avant de décoder : nous n'envoyons que Side × Side
    const QSize size = reader.size();
    if (reader.format() != "jpeg" || !size.isValid() || size.width() > 2 * Side || size.height() > 2 * Side)
        return {};
    return reader.read();
}

QJsonObject Avatar::create(const QByteArray &identitySeed, const QString &authorOnion, const QByteArray &jpeg,
                           qint64 timestampMs)
{
    QJsonObject r{{"t", "avatar"}, {"a", authorOnion}, {"ts", double(timestampMs)}, {"img", Protocol::toB64(jpeg)}};
    r.insert("s", Protocol::toB64(Ed25519::sign(identitySeed, signingBytes(r))));
    return r;
}

bool Avatar::verify(const QJsonObject &r, const QByteArray &authorKey)
{
    return wellFormed(r, authorKey) && Ed25519::verify(authorKey, signingBytes(r), Protocol::fromB64(r.value("s")));
}

QJsonObject Avatar::createForConversation(const QByteArray &identitySeed, const QString &authorOnion,
                                          const QString &groupId, const QByteArray &jpeg, qint64 timestampMs)
{
    QJsonObject r{{"t", "conv-avatar"}, {"a", authorOnion}, {"ts", double(timestampMs)}, {"img", Protocol::toB64(jpeg)}};
    r.insert("s", Protocol::toB64(Ed25519::sign(identitySeed, signingBytes(r, groupId))));
    return r;
}

bool Avatar::verifyForConversation(const QJsonObject &r, const QByteArray &authorKey, const QString &groupId)
{
    return !groupId.isEmpty() && wellFormed(r, authorKey)
           && Ed25519::verify(authorKey, signingBytes(r, groupId), Protocol::fromB64(r.value("s")));
}

QString Avatar::author(const QJsonObject &r)
{
    return r.value("a").toString();
}

qint64 Avatar::timestamp(const QJsonObject &r)
{
    return qint64(r.value("ts").toDouble());
}

QByteArray Avatar::image(const QJsonObject &r)
{
    return Protocol::fromB64(r.value("img"));
}
