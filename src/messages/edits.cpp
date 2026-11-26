#include "edits.h"

#include "crypto.h"
#include "protocol.h"

#include <tuple>

namespace {
const QByteArray EditContext("p2pchat-edit-v1");

// Ordre indépendant de l'arrivée : une suppression l'emporte toujours (un message supprimé ne revient pas),
// puis la plus récente, puis l'identifiant
auto rank(const Edits::Latest &l)
{
    return std::make_tuple(l.deleted, l.ts, l.id);
}
} // namespace

QByteArray Edits::signingBytes(const QJsonObject &r)
{
    return Crypto::pack({EditContext, r.value("id").toString().toUtf8(), r.value("a").toString().toUtf8(),
                         r.value("m").toString().toUtf8(), r.value("ed").toString().toUtf8(),
                         r.value("del").toBool() ? QByteArray("1") : QByteArray("0"),
                         QByteArray::number(Protocol::Message::timestamp(r))});
}

QJsonObject Edits::create(const QByteArray &identitySeed, const QString &authorOnion, const QString &target,
                          const QString &text, bool deleted, qint64 timestampMs)
{
    QJsonObject r{{"t", "edit"},
                  {"id", QString::fromLatin1(Crypto::randomBytes(12).toHex())},
                  {"a", authorOnion},
                  {"m", target},
                  {"ed", deleted ? QString() : text.left(MaxTextLength)},
                  {"del", deleted},
                  {"ts", double(timestampMs)}};
    r.insert("s", Protocol::toB64(Ed25519::sign(identitySeed, signingBytes(r))));
    return r;
}

bool Edits::isEdit(const QJsonObject &obj)
{
    return obj.contains("ed");
}

QString Edits::target(const QJsonObject &r)
{
    return r.value("m").toString().left(Protocol::Message::MaxIdLength);
}

bool Edits::verify(const QJsonObject &r, const QByteArray &authorKey)
{
    const QString text = r.value("ed").toString();
    const bool deleted = r.value("del").toBool();
    return !target(r).isEmpty() && r.value("del").isBool() && text.size() <= MaxTextLength
           && (deleted ? text.isEmpty() : !text.trimmed().isEmpty())
           && Ed25519::verify(authorKey, signingBytes(r), Protocol::fromB64(r.value("s")));
}

bool Edits::State::apply(const QJsonObject &r)
{
    Latest e;
    e.text = r.value("ed").toString();
    e.deleted = r.value("del").toBool();
    e.ts = Protocol::Message::timestamp(r);
    e.id = r.value("id").toString();
    e.author = r.value("a").toString();
    auto &byAuthor = m_state[target(r)];
    const QString author = r.value("a").toString();
    const auto it = byAuthor.constFind(author);
    if (it != byAuthor.cend() && rank(*it) >= rank(e))
        return false;
    byAuthor.insert(author, e);
    return true;
}

std::optional<Edits::Latest> Edits::State::latest(const QString &target, const QStringList &authors) const
{
    const auto t = m_state.constFind(target);
    if (t == m_state.cend())
        return std::nullopt;
    std::optional<Latest> best;
    for (const QString &author : authors) {
        const auto it = t->constFind(author);
        if (it != t->cend() && (!best || rank(*it) > rank(*best)))
            best = *it;
    }
    return best;
}
