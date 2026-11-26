#include "reactions.h"

#include "crypto.h"
#include "protocol.h"

#include <algorithm>

namespace {
const QByteArray ReactionContext("p2pchat-react-v1");
}

const QStringList Reactions::Allowed{QStringLiteral("👍"), QStringLiteral("❤️"), QStringLiteral("😂"),
                                     QStringLiteral("😮"), QStringLiteral("😢"), QStringLiteral("🎉")};

QByteArray Reactions::signingBytes(const QJsonObject &r)
{
    return Crypto::pack({ReactionContext, r.value("id").toString().toUtf8(), r.value("a").toString().toUtf8(),
                         r.value("m").toString().toUtf8(), r.value("re").toString().toUtf8(),
                         r.value("on").toBool() ? QByteArray("1") : QByteArray("0"),
                         QByteArray::number(Protocol::Message::timestamp(r))});
}

QJsonObject Reactions::create(const QByteArray &identitySeed, const QString &authorOnion, const QString &target,
                              const QString &emoji, bool on, qint64 timestampMs)
{
    QJsonObject r{{"t", "react"},
                  {"id", QString::fromLatin1(Crypto::randomBytes(12).toHex())},
                  {"a", authorOnion},
                  {"m", target},
                  {"re", emoji},
                  {"on", on},
                  {"ts", double(timestampMs)}};
    r.insert("s", Protocol::toB64(Ed25519::sign(identitySeed, signingBytes(r))));
    return r;
}

bool Reactions::isReaction(const QJsonObject &obj)
{
    return obj.contains("re");
}

QString Reactions::target(const QJsonObject &r)
{
    return r.value("m").toString().left(Protocol::Message::MaxIdLength);
}

bool Reactions::verify(const QJsonObject &r, const QByteArray &authorKey)
{
    return Allowed.contains(r.value("re").toString()) && !target(r).isEmpty() && r.value("on").isBool()
           && Ed25519::verify(authorKey, signingBytes(r), Protocol::fromB64(r.value("s")));
}

bool Reactions::State::apply(const QJsonObject &r, bool mine)
{
    Entry e;
    e.ts = Protocol::Message::timestamp(r);
    e.id = r.value("id").toString();
    e.on = r.value("on").toBool();
    e.mine = mine;
    e.author = r.value("a").toString();
    e.emoji = r.value("re").toString();
    auto &forMessage = m_state[target(r)];
    const QString key = e.author + QLatin1Char('\n') + e.emoji;
    const auto it = forMessage.constFind(key);
    if (it != forMessage.cend() && (it->ts > e.ts || (it->ts == e.ts && it->id >= e.id)))
        return false; // un enregistrement plus récent est déjà connu
    const bool changed = it == forMessage.cend() ? e.on : it->on != e.on;
    forMessage.insert(key, e);
    return changed;
}

QList<Reactions::Summary> Reactions::State::summary(const QString &target) const
{
    QList<Summary> out;
    const auto forMessage = m_state.value(target);
    for (const QString &emoji : Allowed) {
        QList<Entry> on;
        for (const Entry &e : forMessage)
            if (e.on && e.emoji == emoji)
                on.append(e);
        if (on.isEmpty())
            continue;
        std::sort(on.begin(), on.end(), [](const Entry &a, const Entry &b) { return a.ts < b.ts; });
        Summary s;
        s.emoji = emoji;
        for (const Entry &e : std::as_const(on)) {
            s.authors << (e.mine ? QString() : e.author); // notre onion n'est pas toujours connu (Tor)
            s.mine = s.mine || e.mine;
        }
        out.append(s);
    }
    return out;
}

bool Reactions::State::mineOn(const QString &target, const QString &emoji) const
{
    const auto forMessage = m_state.value(target);
    return std::any_of(forMessage.cbegin(), forMessage.cend(),
                       [&](const Entry &e) { return e.mine && e.on && e.emoji == emoji; });
}
