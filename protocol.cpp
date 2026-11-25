#include "protocol.h"

#include "crypto.h"

#include <QJsonValue>
#include <QRegularExpression>

namespace {
constexpr auto B64Options = QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals;
const QByteArray MessageContext("p2pchat-msg-v1");
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
    QList<QByteArray> parts{MessageContext,
                            m.value("id").toString().toUtf8(),
                            m.value("a").toString().toUtf8(),
                            m.value("n").toString().toUtf8(),
                            m.value("x").toString().toUtf8(),
                            QByteArray::number(timestamp(m))};
    if (m.contains("r"))
        parts << m.value("r").toString().toUtf8();
    return Crypto::pack(parts);
}

QJsonObject Protocol::Message::create(const QByteArray &identitySeed, const QString &authorOnion,
                                      const QString &nick, const QString &text, qint64 timestampMs,
                                      const QString &replyTo)
{
    QJsonObject m{{"t", "msg"},
                  {"id", QString::fromLatin1(Crypto::randomBytes(12).toHex())},
                  {"a", authorOnion},
                  {"n", nick},
                  {"x", text},
                  {"ts", double(timestampMs)}};
    if (!replyTo.isEmpty())
        m.insert("r", replyTo.left(MaxIdLength));
    m.insert("s", toB64(Ed25519::sign(identitySeed, signingBytes(m))));
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

QString Protocol::Message::replyTo(const QJsonObject &m)
{
    return m.value("r").toString().left(MaxIdLength);
}
