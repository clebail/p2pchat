#include "groupstore.h"

#include "crypto.h"
#include "protocol.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

#include <algorithm>

using Protocol::isValidOnion;

namespace {

QJsonObject readObject(const QSettings &s, const QString &key)
{
    return QJsonDocument::fromJson(s.value(key).toByteArray()).object();
}

void writeObject(QSettings &s, const QString &key, const QJsonObject &o)
{
    s.setValue(key, QJsonDocument(o).toJson(QJsonDocument::Compact));
}

} // namespace

GroupStore::GroupStore(QSettings &settings) : m_settings(settings) {}

void GroupStore::migrateLegacy()
{
    if (!m_settings.contains("contact/secret"))
        return;
    m_settings.setValue("group/secret", m_settings.value("contact/secret"));
    const QString onion = m_settings.value("contact/onion").toString();
    if (isValidOnion(onion))
        setMembers({{onion, m_settings.value("contact/nick").toString()}});
    m_settings.remove("contact");
}

QString GroupStore::nickname() const
{
    return m_settings.value("nickname").toString();
}

void GroupStore::setNickname(const QString &nick)
{
    m_settings.setValue("nickname", nick.left(Protocol::MaxNickLength));
}

QByteArray GroupStore::secret() const
{
    return m_settings.value("group/secret").toByteArray();
}

void GroupStore::setSecret(const QByteArray &secret)
{
    m_settings.setValue("group/secret", secret);
}

void GroupStore::leaveGroup()
{
    m_settings.remove("group");
}

void GroupStore::ensureIdentity()
{
    if (identitySeed().size() != 32)
        m_settings.setValue("identity/seed", Ed25519::generateSeed());
}

QByteArray GroupStore::identitySeed() const
{
    return m_settings.value("identity/seed").toByteArray();
}

QByteArray GroupStore::identityKey() const
{
    return Ed25519::publicKey(identitySeed());
}

QList<Member> GroupStore::members() const
{
    QList<Member> list;
    const QJsonArray arr = QJsonDocument::fromJson(m_settings.value("group/members").toByteArray()).array();
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        if (isValidOnion(o["o"].toString()))
            list.append({o["o"].toString(), o["n"].toString()});
    }
    return list;
}

void GroupStore::setMembers(const QList<Member> &list)
{
    QJsonArray arr;
    for (const Member &m : list)
        arr.append(QJsonObject{{"o", m.onion}, {"n", m.nick}});
    m_settings.setValue("group/members", QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

bool GroupStore::mergeMember(const QString &onion, const QString &nick, const QString &ownOnion)
{
    if (!isValidOnion(onion) || onion == ownOnion)
        return false;
    const QString name = nick.left(Protocol::MaxNickLength);
    QList<Member> list = members();
    for (Member &m : list) {
        if (m.onion == onion) {
            if (!name.isEmpty() && name != m.nick) {
                m.nick = name;
                setMembers(list);
            }
            return false;
        }
    }
    if (list.size() >= MaxMembers)
        return false; // protège contre une liste de membres gonflée artificiellement
    list.append({onion, name});
    setMembers(list);
    return true;
}

bool GroupStore::isMember(const QString &onion) const
{
    const QList<Member> list = members();
    return std::any_of(list.cbegin(), list.cend(), [&](const Member &m) { return m.onion == onion; });
}

QString GroupStore::nickOf(const QString &onion) const
{
    for (const Member &m : members())
        if (m.onion == onion)
            return m.nick;
    return {};
}

QByteArray GroupStore::pinnedKey(const QString &onion) const
{
    return Protocol::fromB64(readObject(m_settings, "keyring").value(onion));
}

bool GroupStore::pinKey(const QString &onion, const QByteArray &key, qint64 nowMs)
{
    if (key.size() != 32 || !isValidOnion(onion))
        return false;
    const QByteArray known = pinnedKey(onion);
    if (!known.isEmpty())
        return known == key;
    QJsonObject ring = readObject(m_settings, "keyring");
    ring.insert(onion, Protocol::toB64(key));
    writeObject(m_settings, "keyring", ring);
    // Invitation avec partage d'historique en cours : le nouveau venu reçoit les 30 derniers jours
    QJsonObject seen = readObject(m_settings, "firstSeen");
    seen.insert(onion, shareHistoryUntil() > nowMs ? 0.0 : double(nowMs));
    writeObject(m_settings, "firstSeen", seen);
    return true;
}

qint64 GroupStore::firstSeen(const QString &onion) const
{
    return qint64(readObject(m_settings, "firstSeen").value(onion).toDouble());
}

qint64 GroupStore::shareHistoryUntil() const
{
    return m_settings.value("group/shareHistoryUntil").toLongLong();
}

void GroupStore::setShareHistoryUntil(qint64 ms)
{
    if (ms > 0)
        m_settings.setValue("group/shareHistoryUntil", ms);
    else
        m_settings.remove("group/shareHistoryUntil");
}

qint64 GroupStore::clearedBefore() const
{
    return m_settings.value("history/clearedBefore").toLongLong();
}

void GroupStore::setClearedBefore(qint64 ms)
{
    m_settings.setValue("history/clearedBefore", ms);
}
