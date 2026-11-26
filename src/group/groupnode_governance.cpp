#include "groupnode.h"
#include "groupnode_p.h"

#include "avatar.h"
#include "crypto.h"
#include "invitation.h"
#include "peerconnection.h"

#include <QDebug>

#include <algorithm>

// GroupNode : gouvernance (administrateurs, exclusions, secret), nom et avatar de la conversation, invitations directes, avatars des membres (le reste de GroupNode : groupnode.cpp)

using GroupNodePrivate::now;
using Protocol::isValidOnion;

// ---------------------------------------------------------------- Gouvernance

std::optional<Governance::Record> GroupNode::head() const
{
    return m_chain.isEmpty() ? std::nullopt : std::optional<Governance::Record>(m_chain.last());
}

QString GroupNode::groupId() const
{
    return m_chain.isEmpty() ? QString() : m_chain.first().hash();
}

bool GroupNode::isAdmin(const QString &onion) const
{
    const auto h = head();
    return h && h->isAdmin(onion.isEmpty() ? m_ownOnion : onion);
}

QStringList GroupNode::admins() const
{
    const auto h = head();
    return h ? h->admins : QStringList();
}

QStringList GroupNode::excludedMembers() const
{
    const auto h = head();
    return h ? h->excluded : QStringList();
}

bool GroupNode::canGovern() const
{
    return m_group.inGroup() && !m_ownOnion.isEmpty() && (m_chain.isEmpty() || isAdmin());
}

QByteArray GroupNode::keyOf(const QString &onion) const
{
    return !m_ownOnion.isEmpty() && onion == m_ownOnion ? m_group.identityKey() : m_group.pinnedKey(onion);
}

void GroupNode::ensureGenesis()
{
    // Seulement pour un groupe que nous avons créé : un groupe d'une version précédente n'a pas de fondateur
    if (!m_chain.isEmpty() || !m_group.isFounder() || !m_group.inGroup() || m_ownOnion.isEmpty())
        return;
    const QJsonObject g = Governance::create(m_group.identitySeed(), m_ownOnion, nullptr, {}, {}, m_group.secret(), now());
    m_chain << *Governance::parse(g);
    m_group.appendGovernanceRecord(g);
}

bool GroupNode::exclude(const QString &onion, QString *error)
{
    if (onion == m_ownOnion || !Protocol::isValidOnion(onion)) {
        if (error)
            *error = tr("membre inconnu");
        return false;
    }
    QStringList excluded = excludedMembers();
    if (!excluded.contains(onion))
        excluded << onion;
    return newRecord(excluded, {}, true, error);
}

bool GroupNode::renewSecret(QString *error)
{
    return newRecord(excludedMembers(), {}, true, error);
}

bool GroupNode::setAdmin(const QString &onion, bool admin, QString *error)
{
    auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    if (!Protocol::isValidOnion(onion) || (onion != m_ownOnion && !m_group.isMember(onion)))
        return fail(tr("membre inconnu"));
    if (m_chain.isEmpty() && !admin)
        return fail(tr("ce groupe n'a pas encore d'administrateur"));
    QStringList admins = m_chain.isEmpty() ? QStringList{m_ownOnion} : head()->admins;
    if (admin == admins.contains(onion))
        return fail(admin ? tr("déjà administrateur") : tr("pas administrateur"));
    if (admin)
        admins << onion;
    else
        admins.removeAll(onion);
    if (admins.isEmpty())
        return fail(tr("il faut au moins un administrateur"));
    return newRecord(excludedMembers(), admins, false, error);
}

bool GroupNode::newRecord(const QStringList &excluded, QStringList admins, bool renew, QString *error)
{
    if (!canGovern()) {
        if (error)
            *error = m_ownOnion.isEmpty() ? tr("réseau pas encore prêt") : tr("seul un administrateur du groupe le peut");
        return false;
    }
    // Groupe d'une version précédente : nous en devenons l'administrateur
    if (m_chain.isEmpty()) {
        const QJsonObject g =
            Governance::create(m_group.identitySeed(), m_ownOnion, nullptr, {}, {}, m_group.secret(), now());
        m_chain << *Governance::parse(g);
        m_group.appendGovernanceRecord(g);
    }
    const Governance::Record previous = m_chain.last();
    const QByteArray secret = renew ? Crypto::randomBytes(16) : m_group.secret();
    if (admins.isEmpty())
        admins = previous.admins;
    for (const QString &e : excluded)
        admins.removeAll(e);
    const QJsonObject r =
        Governance::create(m_group.identitySeed(), m_ownOnion, &previous, admins, excluded, secret, now());
    m_chain << *Governance::parse(r);
    m_group.appendGovernanceRecord(r);
    if (renew) {
        // L'ancien secret reste accepté un moment : les absents reviendront avec, et recevront le nouveau
        m_group.retireSecret(m_group.secret(), now() + Governance::SecretGraceMs);
        m_group.setSecret(secret);
    }
    applyRecord(previous, m_chain.last());
    for (PeerConnection *p : m_active.values())
        if (p->peerVersion() >= Governance::Version)
            sendGovernance(p);
    if (renew)
        emit secretRenewed(m_group.nickname());
    emit stateChanged();
    return true;
}

void GroupNode::sendGovernance(PeerConnection *c)
{
    if (m_chain.isEmpty())
        return;
    QJsonArray records;
    for (const Governance::Record &r : std::as_const(m_chain))
        records.append(r.json);
    c->send({{"t", "gov"}, {"records", records}});
    // Le secret en vigueur, seulement à un membre non exclu, et s'il a changé depuis la création
    const QString peer = c->peerOnion();
    if (m_chain.last().seq > 0 && !m_chain.last().isExcluded(peer)
        && Governance::secretHash(m_group.secret()) == m_chain.last().secretHash)
        c->send({{"t", "gov-secret"}, {"seq", m_chain.last().seq}, {"k", Protocol::toB64(m_group.secret())}});
}

bool GroupNode::integrateRecords(const QJsonArray &records)
{
    bool advanced = false;
    for (const QJsonValue &v : records) {
        const QJsonObject r = v.toObject();
        const auto parsed = Governance::parse(r);
        if (!parsed)
            return advanced;
        if (parsed->seq < m_chain.size()) {
            if (m_chain.at(parsed->seq).hash() != parsed->hash()) {
                qWarning().noquote() << "[gouvernance] chaîne divergente reçue : ignorée";
                return advanced;
            }
            continue; // déjà connu
        }
        const auto h = head();
        if (Governance::checkNext(h, r, keyOf(parsed->author), h ? QString() : m_group.expectedGroupId())
            != Governance::Check::Ok) {
            qWarning().noquote() << "[gouvernance] enregistrement refusé, seq" << parsed->seq;
            return advanced;
        }
        m_chain << *parsed;
        m_group.appendGovernanceRecord(r);
        advanced = true;
        if (h)
            applyRecord(*h, *parsed);
        if (!m_group.inGroup())
            return advanced; // nous venons d'être exclus
    }
    return advanced;
}

void GroupNode::applyRecord(const Governance::Record &previous, const Governance::Record &record)
{
    const QString by = record.author == m_ownOnion ? m_group.nickname() : m_group.nickOf(record.author);
    for (const QString &onion : record.excluded) {
        if (previous.isExcluded(onion))
            continue;
        if (onion == m_ownOnion) { // c'est nous : plus aucune connexion, groupe quitté (historique gardé)
            closeAll();
            m_group.leaveGroup();
            m_chain.clear();
            emit excludedFromGroup(by);
            emit stateChanged();
            return;
        }
        const QString nick = m_group.nickOf(onion);
        QList<Member> members = m_group.members();
        members.erase(std::remove_if(members.begin(), members.end(), [&](const Member &m) { return m.onion == onion; }),
                      members.end());
        m_group.setMembers(members);
        if (PeerConnection *c = m_active.value(onion)) {
            if (c->peerVersion() >= Governance::Version)
                sendGovernance(c); // la chaîne lui apprend son exclusion
            c->close();
        }
        if (PeerConnection *c = m_attempts.value(onion))
            c->close();
        emit memberExcluded(onion, nick, by);
    }
    // Administrateurs nommés ou retirés (un exclu perd son rôle sans que ce soit annoncé à part)
    auto nickOf = [this](const QString &onion) { return onion == m_ownOnion ? m_group.nickname() : m_group.nickOf(onion); };
    for (const QString &onion : record.admins)
        if (!previous.isAdmin(onion))
            emit adminChanged(onion, nickOf(onion), true, by);
    for (const QString &onion : previous.admins)
        if (!record.isAdmin(onion) && !record.isExcluded(onion))
            emit adminChanged(onion, nickOf(onion), false, by);
}

void GroupNode::onGovernance(PeerConnection *c, const QJsonObject &o)
{
    if (!integrateRecords(o.value("records").toArray()))
        return;
    if (!m_group.inGroup())
        return; // exclus
    // Nouveaux administrateurs : leurs modérations reçues avant cet enregistrement comptent désormais
    for (const QString &target : m_edits.targets())
        emit messageEdited(target);
    // Transmis aux autres membres connectés (ceux qui ne sont reliés qu'à nous l'apprennent ainsi)
    for (PeerConnection *p : m_active.values())
        if (p != c && p->peerVersion() >= Governance::Version)
            sendGovernance(p);
    emit stateChanged();
}

void GroupNode::onSecret(PeerConnection *c, const QJsonObject &o)
{
    const auto h = head();
    const QByteArray secret = Protocol::fromB64(o.value("k"));
    if (!h || o.value("seq").toInt(-1) != h->seq || secret.size() < 16
        || Governance::secretHash(secret) != h->secretHash || secret == m_group.secret())
        return; // pas le secret en vigueur, ou déjà le nôtre
    m_group.retireSecret(m_group.secret(), now() + Governance::SecretGraceMs);
    m_group.setSecret(secret);
    emit secretRenewed(h->author == m_ownOnion ? m_group.nickname() : m_group.nickOf(h->author));
    // À notre tour de le transmettre aux membres connectés qui ne l'auraient pas
    for (PeerConnection *p : m_active.values())
        if (p != c && p->peerVersion() >= Governance::Version)
            sendGovernance(p);
}

void GroupNode::onLeave(PeerConnection *c, const QJsonObject &o)
{
    const QString onion = o.value("a").toString();
    const qint64 ts = qint64(o.value("ts").toDouble());
    // Signé par le membre qui part : personne ne peut annoncer le départ d'un autre
    if (onion != m_active.key(c)
        || !Ed25519::verify(m_group.pinnedKey(onion),
                            Crypto::pack({"p2pchat-leave-v1", onion.toUtf8(), QByteArray::number(ts)}),
                            Protocol::fromB64(o.value("s"))))
        return;
    const QString nick = m_group.nickOf(onion);
    m_group.setDeparted(onion, true);
    QList<Member> members = m_group.members();
    members.erase(std::remove_if(members.begin(), members.end(), [&](const Member &m) { return m.onion == onion; }),
                  members.end());
    m_group.setMembers(members);
    emit memberLeft(onion, nick);
    emit stateChanged();
}

// ---------------------------------------------------------------- Conversations : nom partagé, invitations

QString GroupNode::sharedName() const
{
    return m_group.nameRecord().value("n").toString();
}

bool GroupNode::setSharedName(const QString &name, QString *error)
{
    if (m_chain.isEmpty() || !isAdmin()) {
        if (error)
            *error = tr("seul un administrateur du groupe le peut");
        return false;
    }
    // Strictement plus récent que le nom en vigueur (horloges décalées : on avance d'une milliseconde)
    const qint64 ts = std::max(now(), qint64(m_group.nameRecord().value("ts").toDouble()) + 1);
    const QJsonObject r = Governance::createName(m_group.identitySeed(), m_ownOnion, groupId(), name, ts);
    m_group.setNameRecord(r);
    for (PeerConnection *p : m_active.values())
        sendName(p);
    emit sharedNameChanged(sharedName(), m_group.nickname());
    emit stateChanged();
    return true;
}

void GroupNode::sendName(PeerConnection *c)
{
    const QJsonObject r = m_group.nameRecord();
    if (!r.isEmpty() && c->peerVersion() >= Governance::NameVersion)
        c->send(r);
}

void GroupNode::onName(PeerConnection *c, const QJsonObject &o)
{
    const auto h = head();
    const QString author = o.value("a").toString();
    if (!h || !h->isAdmin(author) || o.value("ts").toDouble() <= m_group.nameRecord().value("ts").toDouble()
        || !Governance::verifyName(o, keyOf(author), groupId()))
        return; // pas d'un administrateur, pas plus récent, ou falsifié
    QJsonObject r = o;
    r.insert("t", "conv-name");
    m_group.setNameRecord(r);
    for (PeerConnection *p : m_active.values()) // relais, pour ceux qui ne sont reliés qu'à nous
        if (p != c)
            sendName(p);
    emit sharedNameChanged(sharedName(), author == m_ownOnion ? m_group.nickname() : m_group.nickOf(author));
    emit stateChanged();
}

// ---------------------------------------------------------------- Avatars

QByteArray GroupNode::conversationAvatar() const
{
    return Avatar::image(m_group.conversationAvatarRecord());
}

bool GroupNode::setConversationAvatar(const QByteArray &jpeg, QString *error)
{
    if (m_chain.isEmpty() || !isAdmin()) {
        if (error)
            *error = tr("seul un administrateur du groupe le peut");
        return false;
    }
    // Strictement plus récent que celui en vigueur (horloges décalées : on avance d'une milliseconde)
    const qint64 ts = std::max(now(), Avatar::timestamp(m_group.conversationAvatarRecord()) + 1);
    m_group.setConversationAvatarRecord(
        Avatar::createForConversation(m_group.identitySeed(), m_ownOnion, groupId(), jpeg, ts));
    for (PeerConnection *p : m_active.values())
        sendConversationAvatar(p);
    emit conversationAvatarChanged(jpeg.isEmpty(), m_group.nickname());
    return true;
}

void GroupNode::sendConversationAvatar(PeerConnection *c)
{
    const QJsonObject r = m_group.conversationAvatarRecord();
    if (!r.isEmpty() && c->peerVersion() >= Avatar::ConversationVersion)
        c->send(r);
}

void GroupNode::onConversationAvatar(PeerConnection *c, const QJsonObject &o)
{
    const auto h = head();
    const QString author = Avatar::author(o);
    if (!h || !h->isAdmin(author) || Avatar::timestamp(o) <= Avatar::timestamp(m_group.conversationAvatarRecord())
        || !Avatar::verifyForConversation(o, keyOf(author), groupId()))
        return; // pas d'un administrateur, pas plus récent, ou falsifié
    QJsonObject r = o;
    r.insert("t", "conv-avatar");
    m_group.setConversationAvatarRecord(r);
    for (PeerConnection *p : m_active.values()) // relais, pour ceux qui ne sont reliés qu'à nous
        if (p != c)
            sendConversationAvatar(p);
    emit conversationAvatarChanged(Avatar::image(r).isEmpty(),
                                   author == m_ownOnion ? m_group.nickname() : m_group.nickOf(author));
}

QJsonObject GroupNode::avatarRecordOf(const QString &onion) const
{
    if (onion.isEmpty())
        return {};
    if (onion == m_ownOnion)
        return m_group.ownAvatarTime() > 0 ? Avatar::create(m_group.identitySeed(), m_ownOnion, m_group.ownAvatar(),
                                                            m_group.ownAvatarTime())
                                           : QJsonObject();
    return m_group.avatarRecord(onion);
}

void GroupNode::sendAvatarInventory(PeerConnection *c)
{
    // Date des avatars connus (le nôtre et ceux des membres) : il nous renvoie ceux qu'il a de plus récents
    if (c->peerVersion() < Avatar::Version)
        return;
    QJsonObject have;
    if (m_group.ownAvatarTime() > 0)
        have.insert(m_ownOnion, double(m_group.ownAvatarTime()));
    for (const Member &m : m_group.members())
        if (const qint64 ts = Avatar::timestamp(m_group.avatarRecord(m.onion)); ts > 0)
            have.insert(m.onion, double(ts));
    c->send({{"t", "avatar-have"}, {"h", have}});
}

void GroupNode::onAvatarInventory(PeerConnection *c, const QJsonObject &o)
{
    // Seulement les avatars des membres de cette conversation : rien sur nos autres contacts
    const QJsonObject theirs = o.value("h").toObject();
    QStringList onions{m_ownOnion};
    for (const Member &m : m_group.members())
        if (m.onion != c->peerOnion())
            onions << m.onion;
    for (const QString &onion : std::as_const(onions)) {
        const QJsonObject r = avatarRecordOf(onion);
        if (!r.isEmpty() && Avatar::timestamp(r) > qint64(theirs.value(onion).toDouble()))
            c->send(r);
    }
}

void GroupNode::onAvatar(PeerConnection *c, const QJsonObject &o)
{
    const QString author = Avatar::author(o);
    if (author == m_ownOnion || !m_group.isMember(author)
        || Avatar::timestamp(o) <= Avatar::timestamp(m_group.avatarRecord(author)) || !Avatar::verify(o, keyOf(author)))
        return; // le nôtre, pas d'un membre, pas plus récent, ou falsifié
    QJsonObject r = o;
    r.insert("t", "avatar");
    m_group.setAvatarRecord(author, r);
    for (PeerConnection *p : m_active.values()) // relais, pour ceux qui ne sont reliés qu'à nous
        if (p != c && p->peerVersion() >= Avatar::Version)
            p->send(r);
    emit avatarChanged(author);
}

void GroupNode::publishAvatar()
{
    const QJsonObject r = avatarRecordOf(m_ownOnion);
    if (r.isEmpty())
        return;
    for (PeerConnection *p : m_active.values())
        if (p->peerVersion() >= Avatar::Version)
            p->send(r);
    emit avatarChanged(m_ownOnion);
}

bool GroupNode::canInviteDirectly(const QString &onion) const
{
    const PeerConnection *c = m_active.value(onion);
    return c && c->peerVersion() >= Governance::NameVersion;
}

bool GroupNode::inviteDirectly(const QString &onion, const Invitation &invitation, const QString &name)
{
    if (!canInviteDirectly(onion) || invitation.onion != m_ownOnion)
        return false;
    m_active.value(onion)->send({{"t", "conv-invite"},
                                 {"c", invitation.toCode()},
                                 {"n", name.trimmed().left(Governance::MaxNameLength)}});
    return true;
}

void GroupNode::onDirectInvitation(PeerConnection *c, const QJsonObject &o)
{
    const auto inv = Invitation::fromCode(o.value("c").toString());
    const QString from = m_active.key(c);
    // Seulement pour sa propre conversation : son adresse et sa clé d'identité (celle de cette connexion)
    if (!inv || from.isEmpty() || inv->onion != from || inv->identityKey != c->peerIdentityKey()
        || m_group.acceptedSecrets(now()).contains(inv->secret))
        return;
    emit directInvitation(from, c->peerNickname(), *inv,
                          o.value("n").toString().trimmed().left(Governance::MaxNameLength));
}
