#include "invitation.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

static const QString Prefix = QStringLiteral("P2PCHAT-");
static constexpr auto B64 = QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals;

QString Invitation::toCode() const
{
    const QJsonObject o{{"v", 3},
                        {"k", QString::fromLatin1(secret.toBase64(B64))},
                        {"o", onion},
                        {"n", nickname},
                        {"i", QString::fromLatin1(identityKey.toBase64(B64))}};
    return Prefix + QString::fromLatin1(QJsonDocument(o).toJson(QJsonDocument::Compact).toBase64(B64));
}

std::optional<Invitation> Invitation::fromCode(const QString &code)
{
    // Tolère les retours à la ligne et espaces ajoutés par les logiciels de mail
    QString c = code;
    c.remove(QRegularExpression(QStringLiteral("\\s")));
    const int start = c.indexOf(Prefix);
    if (start < 0)
        return std::nullopt;
    c = c.mid(start + Prefix.size());
    c.remove(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-].*$")));

    const auto decoded = QByteArray::fromBase64Encoding(c.toLatin1(), B64 | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded)
        return std::nullopt;
    const QJsonObject o = QJsonDocument::fromJson(*decoded).object();
    if (o.value("v").toInt() != 3)
        return std::nullopt;

    Invitation inv;
    inv.secret = QByteArray::fromBase64(o.value("k").toString().toLatin1(), B64);
    inv.onion = o.value("o").toString();
    inv.nickname = o.value("n").toString().left(40);
    inv.identityKey = QByteArray::fromBase64(o.value("i").toString().toLatin1(), B64);
    static const QRegularExpression onionRe(QStringLiteral("^[a-z2-7]{56}\\.onion$"));
    if (inv.secret.size() < 16 || inv.identityKey.size() != 32 || !onionRe.match(inv.onion).hasMatch())
        return std::nullopt;
    return inv;
}
