#include "devicesync.h"

#include "avatar.h"
#include "groupnode.h"
#include "groupnode_p.h"
#include "groupstore.h"
#include "protocol.h"

#include <QJsonArray>
#include <QRegularExpression>

namespace DeviceSync {

namespace {

bool isGroupId(const QString &g)
{
    static const QRegularExpression re(QStringLiteral("^[0-9a-f]{64}$"));
    return re.match(g).hasMatch();
}

} // namespace

Engine::Engine(GroupStore &identity, GroupNode &self, Hooks &hooks, QObject *parent)
    : QObject(parent), m_identity(identity), m_self(self), m_hooks(hooks)
{
    connect(&m_self, &GroupNode::peerReady, this, [this](const QString &onion) {
        if (m_identity.isMyDevice(onion))
            sendTo(onion);
    });
    // Certificat arrivé après la connexion : c'est seulement maintenant un de nos postes
    connect(&m_self, &GroupNode::devicesChanged, this, &Engine::publish);
    connect(&m_self, &GroupNode::selfMessage, this, [this](const QString &onion, const QJsonObject &o) {
        if (o.value("t").toString() == "self-state")
            apply(onion, o);
    });
}

bool Engine::refresh()
{
    QJsonObject records = m_identity.syncedConversations();
    bool changed = false;
    for (const QString &g : m_hooks.joinedGroups()) {
        const QJsonObject r = records.value(g).toObject();
        if (isGroupId(g) && !r.value("in").toBool()) { // rejointe ici (ou de nouveau après l'avoir quittée)
            records.insert(g, QJsonObject{{"in", true}, {"ts", double(GroupNodePrivate::now())}});
            changed = true;
        }
    }
    if (changed)
        m_identity.setSyncedConversations(records);
    return changed;
}

void Engine::checkJoined()
{
    if (refresh())
        publish();
}

void Engine::noteLeft(const QString &groupId)
{
    if (!isGroupId(groupId))
        return;
    QJsonObject records = m_identity.syncedConversations();
    records.insert(groupId, QJsonObject{{"in", false}, {"ts", double(GroupNodePrivate::now())}});
    m_identity.setSyncedConversations(records);
    publish();
}

void Engine::publish()
{
    for (const QString &onion : m_self.connectedPeers())
        if (m_identity.isMyDevice(onion))
            sendTo(onion);
}

void Engine::sendTo(const QString &onion)
{
    m_self.sendTo(onion, stateFor(onion));
}

QJsonObject Engine::stateFor(const QString &onion)
{
    refresh();
    const QByteArray key = m_identity.pinnedKey(onion);
    const QJsonObject records = m_identity.syncedConversations();
    const QStringList joined = m_hooks.joinedGroups();
    QJsonArray convs;
    for (auto it = records.constBegin(); it != records.constEnd() && convs.size() < MaxConversations; ++it) {
        const QJsonObject r = it.value().toObject();
        QJsonObject c{{"g", it.key()}, {"in", r.value("in").toBool()}, {"ts", r.value("ts")}};
        if (r.value("in").toBool() && joined.contains(it.key())) {
            c.insert("n", m_hooks.localName(it.key()));
            if (const auto inv = m_hooks.invitationFor(it.key(), onion, key))
                c.insert("c", inv->toCode());
        }
        convs.append(c);
    }
    QJsonArray words;
    for (const QString &w : m_hooks.words())
        words.append(w);
    const QByteArray avatar = m_identity.ownAvatar();
    return {{"t", "self-state"},
            {"n", m_identity.nickname()},
            {"nt", double(m_identity.nicknameTime())},
            {"a", Protocol::toB64(avatar)},
            {"at", double(m_identity.ownAvatarTime())},
            {"w", words},
            {"c", convs}};
}

void Engine::apply(const QString &from, const QJsonObject &state)
{
    if (!m_identity.isMyDevice(from))
        return;
    refresh();
    bool changed = false; // à faire suivre à nos autres postes

    // Profil : le plus récent l'emporte ; le dictionnaire est réuni
    bool nickChanged = false, avatarChanged = false;
    const QString nick = state.value("n").toString().trimmed().left(Protocol::MaxNickLength);
    const qint64 nickTime = qint64(state.value("nt").toDouble());
    if (!nick.isEmpty() && nickTime > m_identity.nicknameTime()) {
        nickChanged = nick != m_identity.nickname();
        m_identity.setNickname(nick, nickTime);
        changed = true;
    }
    const QByteArray avatar = Protocol::fromB64(state.value("a"));
    const qint64 avatarTime = qint64(state.value("at").toDouble());
    const bool validAvatar = avatar.isEmpty() || (avatar.size() <= Avatar::MaxBytes && !Avatar::decode(avatar).isNull());
    if (validAvatar && avatarTime > m_identity.ownAvatarTime()) {
        avatarChanged = avatar != m_identity.ownAvatar();
        m_identity.setOwnAvatar(avatar, avatarTime);
        changed = true;
    }
    const QStringList known = m_hooks.words();
    QStringList newWords;
    for (const QJsonValue &v : state.value("w").toArray()) {
        const QString w = v.toString();
        if (!w.isEmpty() && w.size() <= 100 && !known.contains(w) && !newWords.contains(w) && newWords.size() < 5000)
            newWords << w;
    }
    if (nickChanged || avatarChanged || !newWords.isEmpty()) {
        m_hooks.profileChanged(nickChanged, avatarChanged, newWords);
        changed = changed || !newWords.isEmpty();
    }

    // Conversations : rejointes ou quittées, la décision la plus récente l'emporte
    QJsonObject records = m_identity.syncedConversations();
    const QJsonArray convs = state.value("c").toArray();
    for (int i = 0; i < convs.size() && i < MaxConversations; ++i) {
        const QJsonObject c = convs.at(i).toObject();
        const QString g = c.value("g").toString();
        const qint64 ts = qint64(c.value("ts").toDouble());
        if (!isGroupId(g) || ts <= qint64(records.value(g).toObject().value("ts").toDouble()))
            continue; // pas plus récent que ce qu'on sait
        const bool in = c.value("in").toBool();
        const bool member = m_hooks.joinedGroups().contains(g);
        if (in && !member) {
            // Le poste qui nous l'annonce ne peut nous y faire entrer que s'il en est membre : sinon, on attend
            const auto inv = Invitation::fromCode(c.value("c").toString());
            if (!inv || inv->onion != from || inv->groupId != g
                || !m_hooks.joinConversation(*inv, c.value("n").toString().trimmed().left(100)))
                continue;
            emit conversationJoined(g);
        } else if (!in && member) {
            m_hooks.leaveConversation(g);
            emit conversationLeft(g);
        }
        records.insert(g, QJsonObject{{"in", in}, {"ts", double(ts)}});
        changed = true;
    }
    if (changed) {
        m_identity.setSyncedConversations(records);
        // Nos autres postes connectés (une chaîne de postes converge ainsi) ; pas celui qui nous l'a appris
        for (const QString &onion : m_self.connectedPeers())
            if (onion != from && m_identity.isMyDevice(onion))
                sendTo(onion);
    }
}

} // namespace DeviceSync
