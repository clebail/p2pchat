#include "governance.h"

#include "crypto.h"
#include "protocol.h"

#include <QCryptographicHash>
#include <QJsonArray>

namespace {

const QByteArray GovernanceContext("p2pchat-group-v1");
constexpr int MaxAdmins = 32, MaxExcluded = 256;

QJsonArray toArray(const QStringList &list)
{
    QJsonArray a;
    for (const QString &s : list)
        a.append(s);
    return a;
}

std::optional<QStringList> onions(const QJsonValue &v, int max)
{
    if (!v.isArray())
        return std::nullopt;
    QStringList out;
    for (const QJsonValue &item : v.toArray()) {
        const QString onion = item.toString();
        if (!Protocol::isValidOnion(onion) || out.contains(onion))
            return std::nullopt;
        out << onion;
    }
    if (out.size() > max)
        return std::nullopt;
    return out;
}

} // namespace

QString Governance::Record::hash() const
{
    return QString::fromLatin1(QCryptographicHash::hash(signingBytes(json), QCryptographicHash::Sha256).toHex());
}

QByteArray Governance::secretHash(const QByteArray &secret)
{
    return QCryptographicHash::hash(GovernanceContext + secret, QCryptographicHash::Sha256);
}

QByteArray Governance::signingBytes(const QJsonObject &r)
{
    auto joined = [](const QJsonValue &v) {
        QByteArray out;
        for (const QJsonValue &item : v.toArray())
            out += item.toString().toUtf8() + ',';
        return out;
    };
    return Crypto::pack({GovernanceContext, QByteArray::number(r.value("seq").toInt(-1)),
                         r.value("prev").toString().toLatin1(), joined(r.value("adm")), joined(r.value("exc")),
                         r.value("sh").toString().toLatin1(), r.value("a").toString().toUtf8(),
                         QByteArray::number(qint64(r.value("ts").toDouble()))});
}

QJsonObject Governance::create(const QByteArray &identitySeed, const QString &authorOnion, const Record *previous,
                               const QStringList &admins, const QStringList &excluded, const QByteArray &secret,
                               qint64 timestampMs)
{
    QJsonObject r{{"t", "group"},
                  {"seq", previous ? previous->seq + 1 : 0},
                  {"prev", previous ? previous->hash() : QString()},
                  {"adm", toArray(previous ? admins : QStringList{authorOnion})},
                  {"exc", toArray(previous ? excluded : QStringList{})},
                  {"sh", Protocol::toB64(secretHash(secret))},
                  {"a", authorOnion},
                  {"ts", double(timestampMs)}};
    r.insert("s", Protocol::toB64(Ed25519::sign(identitySeed, signingBytes(r))));
    return r;
}

std::optional<Governance::Record> Governance::parse(const QJsonObject &o)
{
    Record r;
    r.seq = o.value("seq").toInt(-1);
    r.prev = o.value("prev").toString();
    r.author = o.value("a").toString();
    r.ts = qint64(o.value("ts").toDouble());
    r.secretHash = Protocol::fromB64(o.value("sh"));
    const auto admins = onions(o.value("adm"), MaxAdmins);
    const auto excluded = onions(o.value("exc"), MaxExcluded);
    if (r.seq < 0 || !admins || admins->isEmpty() || !excluded || r.secretHash.size() != 32
        || !Protocol::isValidOnion(r.author) || (r.seq == 0) != r.prev.isEmpty()
        || (r.seq > 0 && !Protocol::isHexDigest(r.prev)))
        return std::nullopt;
    for (const QString &a : *admins)
        if (excluded->contains(a))
            return std::nullopt; // un administrateur exclu n'a pas de sens
    r.admins = *admins;
    r.excluded = *excluded;
    r.json = o;
    return r;
}

Governance::Check Governance::checkNext(const std::optional<Record> &head, const QJsonObject &candidate,
                                        const QByteArray &authorKey, const QString &expectedGroup)
{
    const auto r = parse(candidate);
    if (!r)
        return Check::Malformed;
    if (!Ed25519::verify(authorKey, signingBytes(candidate), Protocol::fromB64(candidate.value("s"))))
        return Check::BadSignature;
    if (!head) {
        // Premier enregistrement : son auteur en est l'unique administrateur
        if (r->seq != 0 || r->admins != QStringList{r->author} || !r->excluded.isEmpty())
            return Check::NotNext;
        if (!expectedGroup.isEmpty() && r->hash() != expectedGroup)
            return Check::WrongGroup;
        return Check::Ok;
    }
    if (r->seq != head->seq + 1 || r->prev != head->hash())
        return Check::NotNext;
    if (!head->isAdmin(r->author))
        return Check::NotAdmin;
    return Check::Ok;
}

namespace {
QByteArray nameSigningBytes(const QJsonObject &r, const QString &groupId)
{
    return Crypto::pack({"p2pchat-name-v1", groupId.toLatin1(), r.value("n").toString().toUtf8(),
                         r.value("a").toString().toUtf8(), QByteArray::number(qint64(r.value("ts").toDouble()))});
}
} // namespace

QJsonObject Governance::createName(const QByteArray &identitySeed, const QString &authorOnion, const QString &groupId,
                                   const QString &name, qint64 timestampMs)
{
    QJsonObject r{{"t", "conv-name"},
                  {"n", name.trimmed().left(MaxNameLength)},
                  {"a", authorOnion},
                  {"ts", double(timestampMs)}};
    r.insert("s", Protocol::toB64(Ed25519::sign(identitySeed, nameSigningBytes(r, groupId))));
    return r;
}

bool Governance::verifyName(const QJsonObject &r, const QByteArray &authorKey, const QString &groupId)
{
    const QString name = r.value("n").toString();
    return !groupId.isEmpty() && name.size() <= MaxNameLength && name == name.trimmed()
           && Protocol::isValidOnion(r.value("a").toString()) && r.value("ts").toDouble() > 0
           && Ed25519::verify(authorKey, nameSigningBytes(r, groupId), Protocol::fromB64(r.value("s")));
}
