#include "groupstore.h"

#include "crypto.h"
#include "person.h"
#include "protocol.h"

#include <QDateTime>
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

void GroupStore::setNickname(const QString &nick, qint64 timestampMs)
{
    m_settings.setValue("nickname", nick.left(Protocol::MaxNickLength));
    m_settings.setValue("nicknameTime", timestampMs > 0 ? timestampMs : QDateTime::currentMSecsSinceEpoch());
}

qint64 GroupStore::nicknameTime() const
{
    return m_settings.value("nicknameTime").toLongLong();
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
    if (personSeed().size() != 32)
        setPersonSeed(Ed25519::generateSeed(), QDateTime::currentMSecsSinceEpoch());
}

QByteArray GroupStore::personSeed() const
{
    return m_settings.value("identity/person").toByteArray();
}

QByteArray GroupStore::personKey() const
{
    const QByteArray seed = personSeed();
    return seed.size() == 32 ? Ed25519::publicKey(seed) : QByteArray();
}

qint64 GroupStore::personSince() const
{
    return m_settings.value("identity/personSince").toLongLong();
}

void GroupStore::setPersonSeed(const QByteArray &seed, qint64 sinceMs)
{
    m_settings.setValue("identity/person", seed);
    m_settings.setValue("identity/personSince", sinceMs);
}

bool GroupStore::personAnnounced() const
{
    return m_settings.value("identity/personAnnounced").toBool();
}

void GroupStore::setPersonAnnounced()
{
    if (!personAnnounced())
        m_settings.setValue("identity/personAnnounced", true);
}

QJsonObject GroupStore::certificate(const QString &onion) const
{
    return isValidOnion(onion) ? readObject(m_settings, "devices/" + onion) : QJsonObject();
}

void GroupStore::setCertificate(const QString &onion, const QJsonObject &certificate)
{
    if (isValidOnion(onion))
        writeObject(m_settings, "devices/" + onion, certificate);
}

QByteArray GroupStore::personOf(const QString &onion) const
{
    return Person::personKey(certificate(onion));
}

bool GroupStore::isMyDevice(const QString &onion) const
{
    const QByteArray person = personOf(onion);
    return !person.isEmpty() && person == personKey();
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

QByteArray GroupStore::ownAvatar() const
{
    return Protocol::fromB64(m_settings.value("avatar/image").toString());
}

qint64 GroupStore::ownAvatarTime() const
{
    return m_settings.value("avatar/ts").toLongLong();
}

void GroupStore::setOwnAvatar(const QByteArray &jpeg, qint64 timestampMs)
{
    if (jpeg.isEmpty())
        m_settings.remove("avatar/image");
    else
        m_settings.setValue("avatar/image", Protocol::toB64(jpeg));
    m_settings.setValue("avatar/ts", timestampMs);
}

QJsonObject GroupStore::avatarRecord(const QString &onion) const
{
    return isValidOnion(onion) ? readObject(m_settings, "avatars/" + onion) : QJsonObject();
}

void GroupStore::setAvatarRecord(const QString &onion, const QJsonObject &record)
{
    if (isValidOnion(onion))
        writeObject(m_settings, "avatars/" + onion, record);
}

bool GroupStore::isVerified(const QString &onion) const
{
    if (const QByteArray person = personOf(onion); !person.isEmpty())
        return readObject(m_settings, "verifiedPersons").contains(Protocol::toB64(person));
    const QByteArray key = pinnedKey(onion);
    return !key.isEmpty() && Protocol::fromB64(readObject(m_settings, "verified").value(onion)) == key;
}

void GroupStore::setVerified(const QString &onion, bool verified)
{
    if (const QByteArray person = personOf(onion); !person.isEmpty()) {
        QJsonObject p = readObject(m_settings, "verifiedPersons");
        if (verified)
            p.insert(Protocol::toB64(person), true);
        else
            p.remove(Protocol::toB64(person));
        writeObject(m_settings, "verifiedPersons", p);
        return;
    }
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

void GroupStore::shareHistoryWith(const QString &onion)
{
    if (!isValidOnion(onion))
        return;
    QJsonObject seen = readObject(m_settings, key("firstSeen"));
    seen.insert(onion, 0.0);
    writeObject(m_settings, key("firstSeen"), seen);
}

QStringList GroupStore::promotedDevices() const
{
    return m_settings.value(key("group/promotedDevices")).toStringList();
}

void GroupStore::setPromotedDevices(const QStringList &onions)
{
    m_settings.setValue(key("group/promotedDevices"), onions);
}

QJsonObject GroupStore::syncedConversations() const
{
    return readObject(m_settings, "self/conversations");
}

void GroupStore::setSyncedConversations(const QJsonObject &records)
{
    writeObject(m_settings, "self/conversations", records);
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

QJsonObject GroupStore::conversationAvatarRecord() const
{
    return readObject(m_settings, key("group/avatarRecord"));
}

void GroupStore::setConversationAvatarRecord(const QJsonObject &record)
{
    writeObject(m_settings, key("group/avatarRecord"), record);
}
