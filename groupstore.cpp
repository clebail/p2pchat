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

GroupStore::GroupStore(QSettings &settings, const QString &conversation)
    : m_settings(settings), m_conversation(conversation),
      m_prefix(conversation.isEmpty() ? QString() : "conversations/" + conversation + "/")
{
}

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
    return m_settings.value(key("group/secret")).toByteArray();
}

void GroupStore::setSecret(const QByteArray &secret)
{
    m_settings.setValue(key("group/secret"), secret);
}

void GroupStore::leaveGroup()
{
    m_settings.remove(key("group"));
}

void GroupStore::retireSecret(const QByteArray &secret, qint64 untilMs)
{
    QJsonArray previous = QJsonDocument::fromJson(m_settings.value(key("group/previousSecrets")).toByteArray()).array();
    previous.prepend(QJsonObject{{"k", Protocol::toB64(secret)}, {"until", double(untilMs)}});
    while (previous.size() > 3) // le secret actuel et trois précédents au plus
        previous.removeLast();
    m_settings.setValue(key("group/previousSecrets"), QJsonDocument(previous).toJson(QJsonDocument::Compact));
}

QList<QByteArray> GroupStore::acceptedSecrets(qint64 nowMs) const
{
    QList<QByteArray> out;
    if (!secret().isEmpty())
        out << secret();
    const QJsonArray previous = QJsonDocument::fromJson(m_settings.value(key("group/previousSecrets")).toByteArray()).array();
    for (const QJsonValue &v : previous) {
        const QJsonObject o = v.toObject();
        const QByteArray k = Protocol::fromB64(o.value("k"));
        if (!k.isEmpty() && qint64(o.value("until").toDouble()) > nowMs && !out.contains(k))
            out << k;
    }
    return out;
}

QList<QJsonObject> GroupStore::governanceRecords() const
{
    QList<QJsonObject> out;
    for (const QJsonValue &v : QJsonDocument::fromJson(m_settings.value(key("group/records")).toByteArray()).array())
        out << v.toObject();
    return out;
}

void GroupStore::appendGovernanceRecord(const QJsonObject &record)
{
    QJsonArray records = QJsonDocument::fromJson(m_settings.value(key("group/records")).toByteArray()).array();
    records.append(record);
    m_settings.setValue(key("group/records"), QJsonDocument(records).toJson(QJsonDocument::Compact));
}

bool GroupStore::isFounder() const
{
    return m_settings.value(key("group/founder")).toBool();
}

void GroupStore::setFounder(bool founder)
{
    m_settings.setValue(key("group/founder"), founder);
}

bool GroupStore::hasDeparted(const QString &onion) const
{
    return readObject(m_settings, key("group/departed")).contains(onion);
}

void GroupStore::setDeparted(const QString &onion, bool departed)
{
    QJsonObject d = readObject(m_settings, key("group/departed"));
    if (departed)
        d.insert(onion, true);
    else
        d.remove(onion);
    writeObject(m_settings, key("group/departed"), d);
}

QString GroupStore::expectedGroupId() const
{
    return m_settings.value(key("group/expectedId")).toString();
}

void GroupStore::setExpectedGroupId(const QString &id)
{
    if (id.isEmpty())
        m_settings.remove(key("group/expectedId"));
    else
        m_settings.setValue(key("group/expectedId"), id);
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
    const QJsonArray arr = QJsonDocument::fromJson(m_settings.value(key("group/members")).toByteArray()).array();
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
    m_settings.setValue(key("group/members"), QJsonDocument(arr).toJson(QJsonDocument::Compact));
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

bool GroupStore::pinKey(const QString &onion, const QByteArray &pub, qint64 nowMs)
{
    if (pub.size() != 32 || !isValidOnion(onion))
        return false;
    const QByteArray known = pinnedKey(onion);
    if (!known.isEmpty() && known != pub)
        return false;
    if (known.isEmpty()) {
        QJsonObject ring = readObject(m_settings, "keyring");
        ring.insert(onion, Protocol::toB64(pub));
        writeObject(m_settings, "keyring", ring);
    }
    // Première rencontre dans cette conversation (la clé peut être connue par une autre). Invitation avec
    // partage d'historique en cours : le nouveau venu reçoit les 30 derniers jours
    QJsonObject seen = readObject(m_settings, key("firstSeen"));
    if (!seen.contains(onion)) {
        seen.insert(onion, shareHistoryUntil() > nowMs ? 0.0 : double(nowMs));
        writeObject(m_settings, key("firstSeen"), seen);
    }
    return true;
}

bool GroupStore::isVerified(const QString &onion) const
{
    const QByteArray key = pinnedKey(onion);
    return !key.isEmpty() && Protocol::fromB64(readObject(m_settings, "verified").value(onion)) == key;
}

void GroupStore::setVerified(const QString &onion, bool verified)
{
    QJsonObject v = readObject(m_settings, "verified");
    if (verified && !pinnedKey(onion).isEmpty())
        v.insert(onion, Protocol::toB64(pinnedKey(onion))); // la clé vérifiée, pas seulement le membre
    else
        v.remove(onion);
    writeObject(m_settings, "verified", v);
}

qint64 GroupStore::firstSeen(const QString &onion) const
{
    return qint64(readObject(m_settings, key("firstSeen")).value(onion).toDouble());
}

qint64 GroupStore::shareHistoryUntil() const
{
    return m_settings.value(key("group/shareHistoryUntil")).toLongLong();
}

void GroupStore::setShareHistoryUntil(qint64 ms)
{
    if (ms > 0)
        m_settings.setValue(key("group/shareHistoryUntil"), ms);
    else
        m_settings.remove(key("group/shareHistoryUntil"));
}

qint64 GroupStore::clearedBefore() const
{
    return m_settings.value(key("history/clearedBefore")).toLongLong();
}

void GroupStore::setClearedBefore(qint64 ms)
{
    m_settings.setValue(key("history/clearedBefore"), ms);
}

QString GroupStore::name() const
{
    return m_settings.value(key("name")).toString();
}

void GroupStore::setName(const QString &name)
{
    if (name.trimmed().isEmpty())
        m_settings.remove(key("name"));
    else
        m_settings.setValue(key("name"), name.trimmed().left(60));
}

QString GroupStore::notifyMode() const
{
    return m_settings.value(key("notify/mode"), m_settings.value("notify/mode")).toString();
}

void GroupStore::setNotifyMode(const QString &mode)
{
    m_settings.setValue(key("notify/mode"), mode);
}

QJsonObject GroupStore::nameRecord() const
{
    return readObject(m_settings, key("group/nameRecord"));
}

void GroupStore::setNameRecord(const QJsonObject &record)
{
    writeObject(m_settings, key("group/nameRecord"), record);
}
