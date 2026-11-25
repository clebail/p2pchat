#include "protocol.h"

#include "crypto.h"

#include <QCryptographicHash>
#include <QJsonValue>
#include <QRegularExpression>

namespace {
constexpr auto B64Options = QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals;
const QByteArray MessageContext("p2pchat-msg-v1");
const QByteArray ReplyContext("p2pchat-reply-v1");
const QByteArray ImageContext("p2pchat-image-v1");
constexpr int MaxImageB64 = 300 * 1024; // garde-fou avant décodage (l'image envoyée fait ≤ 150 Ko)
} // namespace

bool Protocol::isValidOnion(const QString &s)
{
    static const QRegularExpression re(QStringLiteral("^[a-z2-7]{56}\\.onion$"));
    return re.match(s).hasMatch();
}

QString Protocol::toB64(const QByteArray &b)
{
    return QString::fromLatin1(b.toBase64(B64Options));
}

QByteArray Protocol::fromB64(const QString &s)
{
    return QByteArray::fromBase64(s.toLatin1(), QByteArray::Base64UrlEncoding);
}

QByteArray Protocol::fromB64(const QJsonValue &v)
{
    return fromB64(v.toString());
}

QByteArray Protocol::Message::signingBytes(const QJsonObject &m)
{
    return Crypto::pack({MessageContext, m.value("id").toString().toUtf8(), m.value("a").toString().toUtf8(),
                         m.value("n").toString().toUtf8(), m.value("x").toString().toUtf8(),
                         QByteArray::number(timestamp(m))});
}

QByteArray Protocol::Message::replySigningBytes(const QJsonObject &m)
{
    return Crypto::pack({ReplyContext, m.value("id").toString().toUtf8(), m.value("a").toString().toUtf8(),
                         m.value("n").toString().toUtf8(), m.value("x").toString().toUtf8(),
                         QByteArray::number(timestamp(m)), m.value("r").toString().toUtf8()});
}

const QString Protocol::Message::ImagePlaceholder =
    QStringLiteral("📷 Image (mettez à jour P2P Chat pour la voir)");

QByteArray Protocol::Message::imageSigningBytes(const QJsonObject &m, const QByteArray &image)
{
    return Crypto::pack({ImageContext, m.value("id").toString().toUtf8(), m.value("a").toString().toUtf8(),
                         QCryptographicHash::hash(image, QCryptographicHash::Sha256)});
}

QJsonObject Protocol::Message::create(const QByteArray &identitySeed, const QString &authorOnion,
                                      const QString &nick, const QString &text, qint64 timestampMs,
                                      const Extras &extras)
{
    const QString shown = text.isEmpty() && !extras.image.isEmpty() ? ImagePlaceholder : text;
    QJsonObject m{{"t", "msg"},
                  {"id", QString::fromLatin1(Crypto::randomBytes(12).toHex())},
                  {"a", authorOnion},
                  {"n", nick},
                  {"x", shown},
                  {"ts", double(timestampMs)}};
    m.insert("s", toB64(Ed25519::sign(identitySeed, signingBytes(m))));
    if (!extras.replyTo.isEmpty()) {
        m.insert("r", extras.replyTo.left(MaxIdLength));
        m.insert("sr", toB64(Ed25519::sign(identitySeed, replySigningBytes(m))));
    }
    if (!extras.image.isEmpty()) {
        m.insert("img", toB64(extras.image));
        m.insert("si", toB64(Ed25519::sign(identitySeed, imageSigningBytes(m, extras.image))));
    }
    return m;
}

bool Protocol::Message::verify(const QJsonObject &m, const QByteArray &authorKey)
{
    return m.contains("s") && Ed25519::verify(authorKey, signingBytes(m), fromB64(m.value("s")));
}

QString Protocol::Message::id(const QJsonObject &m)
{
    return m.value("id").toString();
}

qint64 Protocol::Message::timestamp(const QJsonObject &m)
{
    return qint64(m.value("ts").toDouble());
}

QString Protocol::Message::verifiedReplyTo(const QJsonObject &m, const QByteArray &authorKey)
{
    const QString r = m.value("r").toString();
    if (r.isEmpty() || r.size() > MaxIdLength || !m.contains("sr"))
        return {};
    return Ed25519::verify(authorKey, replySigningBytes(m), fromB64(m.value("sr"))) ? r : QString();
}

QByteArray Protocol::Message::verifiedImage(const QJsonObject &m, const QByteArray &authorKey)
{
    const QString encoded = m.value("img").toString();
    if (encoded.isEmpty() || encoded.size() > MaxImageB64 || !m.contains("si"))
        return {};
    const QByteArray image = fromB64(encoded);
    return Ed25519::verify(authorKey, imageSigningBytes(m, image), fromB64(m.value("si"))) ? image : QByteArray();
}

QString Protocol::Message::caption(const QJsonObject &m, bool hasImage)
{
    const QString x = m.value("x").toString();
    return hasImage && x == ImagePlaceholder ? QString() : x;
}
