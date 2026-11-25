#include "protocol.h"

#include "crypto.h"
#include "filestore.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonValue>
#include <QRegularExpression>

namespace {
constexpr auto B64Options = QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals;
const QByteArray MessageContext("p2pchat-msg-v1");
const QByteArray ReplyContext("p2pchat-reply-v1");
const QByteArray ImageContext("p2pchat-image-v1");
const QByteArray FileContext("p2pchat-file-v1");
const QString FilePlaceholderSuffix = QStringLiteral(" (mettez à jour P2P Chat pour le télécharger)");
constexpr int MaxImageB64 = 300 * 1024; // garde-fou avant décodage (l'image envoyée fait ≤ 150 Ko)
} // namespace

QStringList Protocol::missingFeatures(int peerVersion)
{
    static const QList<QPair<int, const char *>> features{{5, QT_TRANSLATE_NOOP("Protocol", "les citations")},
                                                          {6, QT_TRANSLATE_NOOP("Protocol", "les images")},
                                                          {7, QT_TRANSLATE_NOOP("Protocol", "les réactions")},
                                                          {8, QT_TRANSLATE_NOOP("Protocol", "les GIF animés")},
                                                          {9, QT_TRANSLATE_NOOP("Protocol", "les fichiers")},
                                                          {11, QT_TRANSLATE_NOOP("Protocol", "les exclusions et le renouvellement du secret")}};
    QStringList missing;
    for (const auto &f : features)
        if (peerVersion < f.first)
            missing << QCoreApplication::translate("Protocol", f.second);
    return missing;
}

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

QByteArray Protocol::Message::fileSigningBytes(const QJsonObject &m)
{
    // Champs bruts, tels qu'envoyés : la signature ne dépend pas de la façon dont le JSON est réécrit
    const QJsonObject f = m.value("f").toObject();
    QByteArray chunks;
    for (const QJsonValue &v : f.value("c").toArray())
        chunks += v.toString().toLatin1() + ',';
    return Crypto::pack({FileContext, m.value("id").toString().toUtf8(), m.value("a").toString().toUtf8(),
                         f.value("n").toString().toUtf8(), QByteArray::number(qint64(f.value("z").toDouble())),
                         f.value("m").toString().toUtf8(), f.value("h").toString().toLatin1(), chunks});
}

QString Protocol::Message::filePlaceholder(const QJsonObject &fileInfo)
{
    return QStringLiteral("📎 %1, %2")
               .arg(Files::sanitizeName(fileInfo.value("n").toString()),
                    Files::formatSize(qint64(fileInfo.value("z").toDouble())))
           + FilePlaceholderSuffix;
}

QJsonObject Protocol::Message::create(const QByteArray &identitySeed, const QString &authorOnion,
                                      const QString &nick, const QString &text, qint64 timestampMs,
                                      const Extras &extras)
{
    const QString shown = !text.isEmpty()           ? text
                          : !extras.image.isEmpty() ? ImagePlaceholder
                          : !extras.file.isEmpty()  ? filePlaceholder(extras.file)
                                                    : text;
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
    if (!extras.file.isEmpty()) {
        m.insert("f", extras.file);
        m.insert("sf", toB64(Ed25519::sign(identitySeed, fileSigningBytes(m))));
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

std::optional<Files::FileInfo> Protocol::Message::verifiedFile(const QJsonObject &m, const QByteArray &authorKey)
{
    if (!m.value("f").isObject() || !m.contains("sf"))
        return std::nullopt;
    if (!Ed25519::verify(authorKey, fileSigningBytes(m), fromB64(m.value("sf"))))
        return std::nullopt;
    return Files::FileInfo::fromJson(m.value("f").toObject());
}

QString Protocol::Message::caption(const QJsonObject &m, bool hasAttachment)
{
    const QString x = m.value("x").toString();
    if (hasAttachment
        && (x == ImagePlaceholder || (m.value("f").isObject() && x == filePlaceholder(m.value("f").toObject()))))
        return {};
    return x;
}
