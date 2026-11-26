#include "groupnode.h"
#include "groupnode_p.h"

#include "crypto.h"
#include "invitation.h"
#include "peerconnection.h"
#include "switchboard.h"

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QTcpSocket>

#include <algorithm>

using GroupNodePrivate::now;
using Protocol::isValidOnion;

namespace {

constexpr int MaxSeenIds = 5000;
constexpr int MaxPending = 16; // connexions entrantes pas encore authentifiées

} // namespace

GroupNode::GroupNode(GroupStore &group, const QString &historyPath, const Timing &timing, QObject *parent)
    : QObject(parent), m_group(group), m_historyPath(historyPath), m_timing(timing)
{
    // Tentative périodique vers les membres hors ligne (chacun selon son délai, cf. m_backoff)
    m_retryTimer.setInterval(m_timing.retryIntervalMs);
    connect(&m_retryTimer, &QTimer::timeout, this, &GroupNode::connectToMembers);
    m_syncTimer.setInterval(m_timing.syncIntervalMs);
    connect(&m_syncTimer, &QTimer::timeout, this, [this] {
        if (!m_active.isEmpty())
            broadcast(inventoryMessage());
    });
    m_syncTimer.start();

    m_presenceTimer.setSingleShot(true);
    m_presenceTimer.setInterval(m_timing.presenceBatchMs);
    connect(&m_presenceTimer, &QTimer::timeout, this, [this] { broadcast(presenceMessage()); });

    m_typingTimer.setInterval(1000);
    connect(&m_typingTimer, &QTimer::timeout, this, [this] {
        const qint64 t = now();
        for (auto it = m_typing.begin(); it != m_typing.end();)
            it = it.value() <= t ? m_typing.erase(it) : std::next(it);
        if (m_typing.isEmpty())
            m_typingTimer.stop();
        emit typingChanged();
    });

    // Les minuteurs sont suspendus pendant la veille : un tic très en retard signale le réveil
    m_wakeTimer.setInterval(m_timing.wakeTickMs);
    connect(&m_wakeTimer, &QTimer::timeout, this, [this] {
        const qint64 t = now();
        const qint64 gap = t - m_lastTick;
        m_lastTick = t;
        if (gap > m_timing.wakeGapMs)
            onWake(gap);
    });
    m_lastTick = now();
    m_wakeTimer.start();

    for (const QJsonObject &r : m_group.governanceRecords()) // déjà vérifiés lors de leur arrivée
        if (const auto parsed = Governance::parse(r))
            m_chain << *parsed;

    setFilesDir(QFileInfo(historyPath).absolutePath() + "/fichiers");
    m_fileTimer.setInterval(m_timing.fileTickMs);
    connect(&m_fileTimer, &QTimer::timeout, this, &GroupNode::filesTick);
    m_fileTimer.start();
}

GroupNode::~GroupNode()
{
    // Les connexions sont nos enfants : pas de signal closed vers un objet à moitié détruit
    const QList<PeerConnection *> all = m_active.values() + m_attempts.values() + m_pending.values();
    for (PeerConnection *c : all)
        c->disconnect(this);
}

bool GroupNode::listen(QString *error)
{
    if (!m_ownBoard) {
        m_ownBoard = new Switchboard(this);
        m_ownBoard->add(this);
    }
    return m_ownBoard->listen(error);
}

quint16 GroupNode::port() const
{
    return m_ownBoard ? m_ownBoard->port() : 0;
}

bool GroupNode::acceptsIncoming() const
{
    return m_group.inGroup() && !m_ownOnion.isEmpty();
}

QList<QByteArray> GroupNode::acceptedSecrets() const
{
    return m_group.acceptedSecrets(now());
}

void GroupNode::setOnline(const QString &ownOnion)
{
    m_ownOnion = ownOnion;
    ensureGenesis(); // groupe créé avant que le réseau soit prêt
    // Juste après le démarrage, Tor peut encore compléter ses informations d'annuaire
    m_backoff.clear();
    m_fastRetryUntil = now() + m_timing.fastRetryMs;
    QTimer::singleShot(m_timing.firstDialDelayMs, this, &GroupNode::connectToMembers);
    m_retryTimer.start();
    emit stateChanged();
}

void GroupNode::setOffline()
{
    m_ownOnion.clear();
    m_retryTimer.stop();
    closeAll();
    emit stateChanged();
}

bool GroupNode::canSend() const
{
    return m_group.inGroup() && !m_group.members().isEmpty() && !m_ownOnion.isEmpty();
}

// ---------------------------------------------------------------- Membres et signatures

bool GroupNode::mergeMember(const QString &onion, const QString &nick)
{
    return m_group.mergeMember(onion, nick, m_ownOnion);
}

bool GroupNode::pinKey(const QString &onion, const QByteArray &key)
{
    return m_group.pinKey(onion, key, now());
}

QByteArray GroupNode::authorKey(const QJsonObject &record, bool mine) const
{
    // « mine » plutôt que notre adresse : au chargement de l'historique, Tor n'a pas encore démarré
    return mine ? m_group.identityKey() : m_group.pinnedKey(record.value("a").toString());
}

GroupNode::Trust GroupNode::verify(const QJsonObject &msg, bool mine) const
{
    if (!msg.contains("s"))
        return Trust::Unsigned;
    return Protocol::Message::verify(msg, authorKey(msg, mine)) ? Trust::Verified : Trust::Invalid;
}

QString GroupNode::verifiedReplyTo(const QJsonObject &msg, bool mine) const
{
    return Protocol::Message::verifiedReplyTo(msg, authorKey(msg, mine));
}

QByteArray GroupNode::verifiedImage(const QJsonObject &msg, bool mine) const
{
    return Protocol::Message::verifiedImage(msg, authorKey(msg, mine));
}

QJsonObject GroupNode::membersMessage() const
{
    using Protocol::toB64;
    QJsonArray arr;
    arr.append(QJsonObject{{"o", m_ownOnion}, {"n", m_group.nickname()}, {"k", toB64(m_group.identityKey())}});
    for (const Member &m : m_group.members())
        arr.append(QJsonObject{{"o", m.onion}, {"n", m.nick}, {"k", toB64(m_group.pinnedKey(m.onion))}});
    return {{"t", "members"}, {"m", arr}};
}

void GroupNode::createGroup()
{
    if (!m_group.inGroup()) {
        m_group.setSecret(Crypto::randomBytes(16));
        m_group.setFounder(true);
        m_chain.clear();
    }
    ensureGenesis();
    emit stateChanged();
}

GroupNode::JoinResult GroupNode::join(const Invitation &inv)
{
    if (inv.onion == m_ownOnion)
        return JoinResult::OwnCode;
    const bool otherGroup = m_group.inGroup() && !m_group.acceptedSecrets(now()).contains(inv.secret);
    if (const QByteArray known = m_group.pinnedKey(inv.onion); !known.isEmpty() && known != inv.identityKey)
        return JoinResult::KeyMismatch; // vérifié avant de quitter quoi que ce soit
    if (otherGroup) { // autre groupe : quitté (membres, secrets, gouvernance)
        closeAll();
        m_group.leaveGroup();
        m_chain.clear();
    }
    if (!pinKey(inv.onion, inv.identityKey))
        return JoinResult::KeyMismatch;
    if (!m_group.inGroup() || otherGroup) {
        m_group.setSecret(inv.secret);
        m_group.setExpectedGroupId(inv.groupId);
    }
    m_group.setDeparted(inv.onion, false);
    mergeMember(inv.onion, inv.nickname);
    connectToMembers();
    emit stateChanged();
    return JoinResult::Joined;
}

void GroupNode::leaveGroup()
{
    // Départ annoncé (signé : personne ne peut le faire à notre place) aux membres connectés
    if (!m_ownOnion.isEmpty()) {
        const qint64 ts = now();
        const QByteArray proof = Ed25519::sign(
            m_group.identitySeed(), Crypto::pack({"p2pchat-leave-v1", m_ownOnion.toUtf8(), QByteArray::number(ts)}));
        const QJsonObject leave{{"t", "gov-leave"}, {"a", m_ownOnion}, {"ts", double(ts)}, {"s", Protocol::toB64(proof)}};
        for (PeerConnection *p : std::as_const(m_active))
            if (p->peerVersion() >= Governance::Version)
                p->send(leave);
    }
    closeAll();
    m_group.leaveGroup();
    m_chain.clear();
    m_lastError.clear();
    emit stateChanged();
}

void GroupNode::announceNickname()
{
    broadcast(membersMessage()); // les autres mettent à jour notre pseudo
}

// ---------------------------------------------------------------- Historique

void GroupNode::loadHistory()
{
    QFile file(m_historyPath);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QList<History::Row> rows = History::parse(file.readAll(), m_group.clearedBefore());
    const qsizetype firstShown = qMax<qsizetype>(0, rows.size() - History::DisplayedRows);
    for (qsizetype i = 0; i < rows.size(); ++i) {
        const History::Row &row = rows[i];
        if (Reactions::isReaction(row.msg)) {
            if (verifyReaction(row.msg, row.mine))
                acceptReaction(row.msg, row.mine, false);
            continue;
        }
        if (Edits::isEdit(row.msg)) {
            if (Edits::verify(row.msg, authorKey(row.msg, row.mine)))
                acceptEdit(row.msg, row.mine, false);
            continue;
        }
        const Trust trust = row.legacy ? Trust::Unsigned : verify(row.msg, row.mine);
        if (i >= firstShown) {
            addMessage(row.msg, row.mine, trust, false);
        } else if (trust == Trust::Verified && !Protocol::Message::id(row.msg).isEmpty()) {
            // Les plus anciens (non affichés) restent disponibles pour la synchronisation
            m_store.insert(Protocol::Message::id(row.msg), History::stored(row.msg, row.mine));
            noteFileAnnounce(row.msg, row.mine);
        }
    }
}

void GroupNode::clearHistory()
{
    QFile::remove(m_historyPath);
    m_store.clear();
    m_reactions.clear();
    m_edits.clear();
    m_files->clear(); // fichiers reçus et envoyés : effacés avec la conversation
    m_fileInfos.clear();
    m_downloads.clear();
    m_group.setClearedBefore(now());
}

bool GroupNode::isMine(const QString &id) const
{
    return m_store.value(id).value("mine").toBool();
}

QList<Reactions::Summary> GroupNode::reactionSummary(const QString &id) const
{
    return id.isEmpty() ? QList<Reactions::Summary>() : m_reactions.summary(id);
}

void GroupNode::addMessage(const QJsonObject &msg, bool mine, Trust trust, bool save)
{
    const QString id = Protocol::Message::id(msg);
    if (!id.isEmpty() && trust == Trust::Verified) { // seuls les messages authentiques sont resynchronisés
        m_store.insert(id, History::stored(msg, mine));
        m_seenIds.insert(id);
        noteFileAnnounce(msg, mine);
    }
    emit messageAdded(msg, mine, trust);
    if (save)
        appendToHistoryFile(msg, mine);
}

void GroupNode::appendToHistoryFile(const QJsonObject &record, bool mine)
{
    // Enregistré tel qu'il a été signé : l'historique reste vérifiable
    QFile file(m_historyPath);
    const bool created = !file.exists();
    if (file.open(QIODevice::Append)) {
        if (created)
            file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        file.write(History::toLine(record, mine));
    }
}

// ---------------------------------------------------------------- Envoi

QJsonObject GroupNode::sendMessage(const QString &text, const Protocol::Message::Extras &extras)
{
    if ((text.isEmpty() && extras.image.isEmpty() && extras.file.isEmpty()) || !canSend())
        return {};
    const QJsonObject msg = Protocol::Message::create(m_group.identitySeed(), m_ownOnion, m_group.nickname(), text,
                                                      now(), extras);
    m_seenIds.insert(Protocol::Message::id(msg));
    m_lastTypingSent = 0; // prochaine frappe : signalée tout de suite
    broadcast(msg);       // sans personne en ligne, il partira à la prochaine synchronisation
    addMessage(msg, true, Trust::Verified, true);
    return msg;
}

void GroupNode::typing()
{
    const qint64 t = now();
    if (t - m_lastTypingSent < m_timing.typingSendMs)
        return;
    m_lastTypingSent = t;
    // Seulement aux membres reliés directement : ni signé, ni relayé, ni conservé
    broadcast({{"t", "typing"}});
}

void GroupNode::sendTo(const QString &onion, const QJsonObject &obj)
{
    if (PeerConnection *c = m_active.value(onion))
        c->send(obj);
}

int GroupNode::peerVersion(const QString &onion) const
{
    const PeerConnection *c = m_active.value(onion);
    return c ? c->peerVersion() : 0;
}

qint64 GroupNode::pendingBytes(const QString &onion) const
{
    const PeerConnection *c = m_active.value(onion);
    return c ? c->pendingBytes() : 0;
}

QStringList GroupNode::typingMembers() const
{
    return m_typing.keys();
}

// ---------------------------------------------------------------- Connexions

PeerConnection *GroupNode::newConnection(QTcpSocket *socket, bool outgoing)
{
    auto *c = new PeerConnection(socket, outgoing, m_group.secret(), m_group.nickname(), m_ownOnion,
                                 m_group.identitySeed(), this);
    c->setSecrets(m_group.acceptedSecrets(now())); // actuel, puis précédents (membres absents)
    wire(c);
    return c;
}

void GroupNode::wire(PeerConnection *c)
{
    c->setAdvertisedVersion(m_advertisedVersion);
    connect(c, &PeerConnection::ready, this, [this, c] { setActive(c); });
    connect(c, &PeerConnection::received, this, [this, c](const QJsonObject &o) { onReceived(c, o); });
    connect(c, &PeerConnection::binaryReceived, this,
            [this, c](quint8 type, const QByteArray &payload) { onBinary(c, type, payload); });
    connect(c, &PeerConnection::closed, this, [this, c](const QString &reason) { onClosed(c, reason); });
}

PeerConnection *GroupNode::createIncoming(QTcpSocket *socket, QObject *parent)
{
    return new PeerConnection(socket, false, m_group.secret(), m_group.nickname(), m_ownOnion,
                              m_group.identitySeed(), parent);
}

void GroupNode::adoptIncoming(PeerConnection *c)
{
    if (!acceptsIncoming() || m_pending.size() >= MaxPending) {
        c->close();
        c->deleteLater();
        return;
    }
    c->setParent(this);
    c->setNickname(m_group.nickname());
    wire(c);
    m_pending.insert(c);
}

void GroupNode::connectToMembers()
{
    if (m_ownOnion.isEmpty() || !m_group.inGroup() || !m_dialer)
        return;
    const qint64 t = now();
    for (const Member &m : m_group.members())
        if (!m_active.contains(m.onion) && !m_attempts.contains(m.onion) && m_backoff.value(m.onion).due(t))
            dial(m.onion);
    emit stateChanged();
}

void GroupNode::reconnectNow()
{
    // Les tentatives en cours peuvent rester bloquées jusqu'au délai de poignée de main (45 s)
    const QList<PeerConnection *> stuck = m_attempts.values();
    for (PeerConnection *c : stuck)
        c->close(); // retire la tentative de m_attempts (signal closed synchrone)
    m_backoff.clear();
    m_retryTimer.start(); // repart pour un cycle complet
    connectToMembers();
}

void GroupNode::retrySoon()
{
    if (m_ownOnion.isEmpty())
        return; // réseau pas prêt : setOnline rappellera tout le monde
    m_fastRetryUntil = now() + m_timing.fastRetryMs;
    reconnectNow();
}

void GroupNode::onWake(qint64 gapMs)
{
    qInfo().noquote() << "[réseau] reprise après une pause de" << gapMs / 1000 << "s";
    if (m_ownOnion.isEmpty())
        return;
    // Pendant la pause, nos pings ne sont pas partis : les correspondants nous ont fermé la porte,
    // mais on ne s'en apercevrait qu'au bout du délai d'inactivité. On repart tout de suite.
    if (gapMs > m_timing.sleepKillsLinksMs) {
        const QList<PeerConnection *> dead = m_active.values();
        for (PeerConnection *c : dead)
            c->close();
    }
    retrySoon();
}

void GroupNode::dial(const QString &onion)
{
    QTcpSocket *s = m_dialer(onion);
    if (!s)
        return;
    m_attempts.insert(onion, newConnection(s, true));
}

void GroupNode::onClosed(PeerConnection *c, const QString &reason)
{
    c->deleteLater();
    m_pending.remove(c);
    const QString attemptKey = m_attempts.key(c);
    if (!attemptKey.isEmpty()) {
        m_attempts.remove(attemptKey);
        m_lastError.insert(attemptKey, reason);
        const qint64 t = now();
        m_backoff[attemptKey].failed(t, t < m_fastRetryUntil);
    }
    const QString activeKey = m_active.key(c);
    if (!activeKey.isEmpty()) {
        m_active.remove(activeKey);
        m_lastError.insert(activeKey, reason);
        m_backoff.remove(activeKey); // il était là il y a un instant : on le rappelle tout de suite
        emit peerLost(activeKey);
        m_peerPresence.remove(activeKey); // ceux qu'il voyait ne sont plus joignables par lui
        m_peerFiles.remove(activeKey);
        m_serveQueue.remove(activeKey);
        dropRequests(activeKey); // les autres sources prennent le relais
        m_chunkMs.remove(activeKey);
        QTimer::singleShot(0, this, &GroupNode::pumpDownloads);
        if (m_typing.remove(activeKey) > 0)
            emit typingChanged();
        m_presenceTimer.start();
        checkQuietDrops();
        announceDropLater(activeKey, reason);
        QTimer::singleShot(m_timing.redialDelayMs, this, &GroupNode::connectToMembers);
    }
    emit stateChanged();
}

void GroupNode::announceDropLater(const QString &onion, const QString &reason)
{
    // La déconnexion n'est annoncée que si le membre ne revient pas dans le délai de grâce
    const quint64 token = ++m_dropToken;
    m_dropped.insert(onion, {reason, token});
    QTimer::singleShot(m_timing.presenceGraceMs, this, [this, onion, token] {
        const auto it = m_dropped.constFind(onion);
        if (it == m_dropped.cend() || it->token != token)
            return;
        const QString why = it->reason;
        m_dropped.erase(it);
        if (m_active.contains(onion))
            return;
        // Toujours joignable par un autre membre : annoncé seulement quand il ne l'est plus
        if (relayedOnline().contains(onion))
            m_quietDrops.insert(onion, why);
        else
            emit presenceChanged(onion, m_group.nickOf(onion), false, why);
    });
}

void GroupNode::setActive(PeerConnection *c)
{
    m_pending.remove(c);
    const QString dialed = m_attempts.key(c);
    if (!dialed.isEmpty())
        m_attempts.remove(dialed);

    const QString onion = c->peerOnion();
    if (!isValidOnion(onion) || onion == m_ownOnion) {
        c->close();
        return;
    }
    // En sortie, Tor garantit qu'on a joint l'adresse appelée : le pair doit annoncer celle-là
    if (!dialed.isEmpty() && dialed != onion) {
        c->close();
        return;
    }
    // Exclu : on lui montre la chaîne qui l'établit (il saura pourquoi), puis on ferme
    if (const auto h = head(); h && h->isExcluded(onion)) {
        if (c->peerVersion() >= Governance::Version)
            sendGovernance(c);
        c->close(); // après envoi (la fermeture vide le tampon)
        return;
    }
    // Ancien secret : réservé aux membres déjà connus (un vieux code d'invitation ne fait plus entrer)
    if (c->secretIndex() > 0 && m_group.pinnedKey(onion).isEmpty()) {
        c->close();
        return;
    }
    // La signature du hello prouve la possession de la clé ; elle doit être celle connue pour ce membre
    if (!pinKey(onion, c->peerIdentityKey())) {
        if (!m_identityWarned.contains(onion)) {
            m_identityWarned.insert(onion);
            emit identityRejected(onion, c->peerNickname());
        }
        c->close();
        return;
    }

    // Revenu pendant le délai de grâce : la coupure passe inaperçue
    const bool graceReturn = m_dropped.remove(onion) > 0;
    const bool quietReturn = m_quietDrops.remove(onion) > 0;
    const bool wasOnline = m_active.contains(onion) || graceReturn || quietReturn;
    if (PeerConnection *old = m_active.value(onion); old && old != c) {
        // Appels croisés : les deux côtés gardent la même connexion
        auto initiator = [this](PeerConnection *p) { return p->isOutgoing() ? m_ownOnion : p->peerOnion(); };
        if (ConnectionPolicy::keepExisting(initiator(old), initiator(c))) {
            c->close();
            return;
        }
        m_active.insert(onion, c); // avant close() : le signal closed de l'ancienne ne compte pas
        old->close();
    }
    m_active.insert(onion, c);
    m_lastError.remove(onion);
    m_backoff.remove(onion);

    const bool isNew = mergeMember(onion, c->peerNickname());
    if (isNew)
        emit memberAdded(onion, c->peerNickname(), true);
    else if (!wasOnline)
        emit presenceChanged(onion, c->peerNickname(), true, {});

    m_group.setDeparted(onion, false); // revenu
    // Échange des listes de membres : chacun découvre ceux qu'il ne connaît pas encore (et leurs clés)
    c->send(membersMessage());
    // Puis la gouvernance, dont il peut alors vérifier les auteurs ; un membre revenu avec l'ancien
    // secret reçoit le nouveau
    if (c->peerVersion() >= Governance::Version)
        sendGovernance(c);
    sendName(c);
    sendConversationAvatar(c);
    sendAvatarInventory(c);
    if (isNew)
        broadcast(membersMessage(), c);
    c->send(inventoryMessage()); // après la liste des membres : les clés des auteurs sont connues
    if (c->peerVersion() >= Protocol::FilesVersion) {
        QStringList ids = m_files->complete();
        ids += m_downloads.keys(); // en cours : il peut déjà profiter des morceaux reçus
        advertiseFiles(ids, c);
    }
    emit peerReady(onion);       // reprise de la console partagée
    c->send(presenceMessage());
    if (!wasOnline) {
        m_presenceTimer.start(); // les autres apprennent qu'on le voit
        // Une connexion vient d'aboutir : le réseau fonctionne, inutile de faire attendre les autres
        if (!m_backoff.isEmpty()) {
            m_backoff.clear();
            connectToMembers();
            return; // connectToMembers signale le changement d'état
        }
    }
    emit stateChanged();
}

void GroupNode::broadcast(const QJsonObject &obj, PeerConnection *except)
{
    for (PeerConnection *p : std::as_const(m_active))
        if (p != except)
            p->send(obj);
}

void GroupNode::closeAll()
{
    const QList<PeerConnection *> all = m_active.values() + m_attempts.values() + m_pending.values();
    m_active.clear();
    m_attempts.clear();
    m_pending.clear();
    m_dropped.clear();
    m_backoff.clear();
    m_peerPresence.clear();
    m_quietDrops.clear();
    m_typing.clear();
    m_peerFiles.clear();
    m_serveQueue.clear();
    for (Download &d : m_downloads)
        d.inFlight.clear();
    m_chunkMs.clear();
    for (const PeerConnection *c : all)
        if (c->isReady())
            emit peerLost(c->peerOnion());
    emit allClosed(); // plus aucune connexion (réseau arrêté, groupe quitté)
    for (PeerConnection *c : all)
        c->close();
}

// ---------------------------------------------------------------- Présence relayée

QJsonObject GroupNode::presenceMessage() const
{
    QJsonArray on;
    for (auto it = m_active.cbegin(); it != m_active.cend(); ++it)
        on.append(it.key());
    return {{"t", "presence"}, {"on", on}};
}

QHash<QString, QString> GroupNode::relayedOnline() const
{
    const QList<QString> active = m_active.keys();
    return ConnectionPolicy::relayedOnline(m_peerPresence, QSet<QString>(active.cbegin(), active.cend()),
                                           m_ownOnion);
}

void GroupNode::checkQuietDrops()
{
    if (m_quietDrops.isEmpty())
        return;
    const QHash<QString, QString> relayed = relayedOnline();
    for (auto it = m_quietDrops.begin(); it != m_quietDrops.end();) {
        if (!m_active.contains(it.key()) && !relayed.contains(it.key())) {
            emit presenceChanged(it.key(), m_group.nickOf(it.key()), false, it.value());
            it = m_quietDrops.erase(it);
        } else {
            ++it;
        }
    }
}

// ---------------------------------------------------------------- Synchronisation

qint64 GroupNode::syncCutoff() const
{
    return History::syncCutoff(now(), m_group.clearedBefore());
}

QJsonObject GroupNode::inventoryMessage() const
{
    return {{"t", "inv"}, {"ids", History::inventory(m_store, syncCutoff())}};
}

void GroupNode::sendBatches(PeerConnection *c, const QList<QJsonObject> &messages)
{
    const qint64 cutoff = History::cutoffFor(syncCutoff(), m_group.firstSeen(c->peerOnion()));
    QList<QJsonObject> toSend = messages;
    if (c->peerVersion() < Reactions::Version) // il ne saurait pas quoi en faire
        toSend.erase(std::remove_if(toSend.begin(), toSend.end(), &Reactions::isReaction), toSend.end());
    if (c->peerVersion() < Edits::Version)
        toSend.erase(std::remove_if(toSend.begin(), toSend.end(), &Edits::isEdit), toSend.end());
    for (const QJsonArray &batch : History::batches(toSend, cutoff))
        c->send({{"t", "sync"}, {"m", batch}});
}

void GroupNode::receiveSync(PeerConnection *from, const QJsonArray &messages)
{
    QList<QJsonObject> accepted, received;
    const qint64 cutoff = syncCutoff();
    for (const QJsonValue &v : messages) {
        const QJsonObject m = v.toObject();
        const QString id = Protocol::Message::id(m);
        if (id.isEmpty() || m_store.contains(id) || Protocol::Message::timestamp(m) < cutoff)
            continue;
        const bool mine = !m_ownOnion.isEmpty() && m.value("a").toString() == m_ownOnion;
        if (Reactions::isReaction(m)) {
            if (verifyReaction(m, mine) && acceptReaction(m, mine, true))
                accepted.append(m);
            continue;
        }
        if (Edits::isEdit(m)) {
            if (Edits::verify(m, authorKey(m, mine)) && acceptEdit(m, mine, true))
                accepted.append(m);
            continue;
        }
        // Chaque message est vérifié avec la clé de son auteur : un membre ne peut rien inventer
        if (verify(m, mine) != Trust::Verified) {
            qWarning().noquote() << "[synchro] message rejeté, signature invalide :" << id;
            continue;
        }
        addMessage(m, mine, Trust::Verified, true);
        accepted.append(m);
        if (!mine)
            received.append(m);
    }
    // Relais aux autres membres connectés, qui ont pu manquer ces messages eux aussi
    if (!accepted.isEmpty())
        for (PeerConnection *p : std::as_const(m_active))
            if (p != from)
                sendBatches(p, accepted);
    if (!received.isEmpty())
        emit messagesReceived(received, true);
}

// ---------------------------------------------------------------- Messages reçus

void GroupNode::onReceived(PeerConnection *c, const QJsonObject &o)
{
    const QString type = o.value("t").toString();
    if (type.startsWith(QLatin1String("term-"))) {
        emit terminalMessage(c->peerOnion(), o);
    } else if (type == "members") {
        onMembers(c, o);
    } else if (type == "presence") {
        onPresence(c, o);
    } else if (type == "inv") {
        sendBatches(c, History::missing(m_store, o.value("ids").toArray()));
    } else if (type == "sync") {
        receiveSync(c, o.value("m").toArray());
    } else if (type == "react") {
        const QString id = Protocol::Message::id(o);
        if (id.isEmpty() || m_store.contains(id) || !verifyReaction(o, false))
            return;
        acceptReaction(o, false, true);
        broadcastReaction(o, c); // relais, comme les messages
    } else if (type == "edit") {
        const QString id = Protocol::Message::id(o);
        if (id.isEmpty() || m_store.contains(id) || !Edits::verify(o, authorKey(o, false)))
            return;
        acceptEdit(o, false, true);
        broadcastEdit(o, c); // relais, comme les messages
    } else if (type == "gov") {
        onGovernance(c, o);
    } else if (type == "gov-secret") {
        onSecret(c, o);
    } else if (type == "gov-leave") {
        onLeave(c, o);
    } else if (type == "conv-name") {
        onName(c, o);
    } else if (type == "conv-avatar") {
        onConversationAvatar(c, o);
    } else if (type == "avatar-have") {
        onAvatarInventory(c, o);
    } else if (type == "avatar") {
        onAvatar(c, o);
    } else if (type == "conv-invite") {
        onDirectInvitation(c, o);
    } else if (type == "typing") {
        onPeerTyping(c);
    } else if (type == "file-have") {
        onFileHave(c, o);
    } else if (type == "file-want") {
        onFileWant(c, o);
    } else if (type == "file-chunk") {
        onFileChunk(c, o);
    } else if (type == "file-none") {
        onFileNone(c, o);
    } else if (type == "msg") {
        onChatMessage(c, o);
    }
}

void GroupNode::onMembers(PeerConnection *c, const QJsonObject &o)
{
    bool changed = false;
    for (const QJsonValue &v : o.value("m").toArray()) {
        const QJsonObject m = v.toObject();
        const QString onion = m["o"].toString();
        if (onion == m_ownOnion)
            continue;
        // Un membre présenté sans clé, ou avec une clé différente de celle connue, est ignoré
        if (!pinKey(onion, Protocol::fromB64(m["k"]))) {
            qWarning().noquote() << "[identité] clé refusée pour" << onion.left(8);
            continue;
        }
        if (m_group.hasDeparted(onion) || (head() && head()->isExcluded(onion)))
            continue; // parti ou exclu : pas réintroduit par la liste d'un autre
        if (mergeMember(onion, m["n"].toString())) {
            changed = true;
            emit memberAdded(onion, m["n"].toString(), false);
        }
    }
    if (changed) {
        broadcast(membersMessage(), c);
        connectToMembers();
    }
    emit stateChanged(); // un pseudo a pu changer
}

void GroupNode::onPresence(PeerConnection *c, const QJsonObject &o)
{
    const QString from = m_active.key(c);
    if (from.isEmpty())
        return; // connexion remplacée entre-temps
    QSet<QString> seen;
    for (const QJsonValue &v : o.value("on").toArray()) {
        const QString onion = v.toString();
        if (onion != from && m_group.isMember(onion))
            seen.insert(onion);
    }
    const QSet<QString> before = m_peerPresence.value(from);
    m_peerPresence.insert(from, seen);
    // Un membre vient d'apparaître chez un autre : il est en ligne, on l'appelle sans attendre
    bool redial = false;
    for (const QString &onion : std::as_const(seen))
        if (!before.contains(onion) && !m_active.contains(onion) && !m_attempts.contains(onion)) {
            m_backoff.remove(onion);
            redial = true;
        }
    checkQuietDrops();
    if (redial)
        connectToMembers();
    else
        emit stateChanged();
}

void GroupNode::onChatMessage(PeerConnection *c, const QJsonObject &o)
{
    const QString id = Protocol::Message::id(o);
    if (id.isEmpty() || m_seenIds.contains(id) || m_store.contains(id))
        return;
    if (m_seenIds.size() > MaxSeenIds)
        m_seenIds.clear();
    m_seenIds.insert(id);

    if (verify(o, false) != Trust::Verified) {
        emit invalidMessage(o.value("n").toString().left(Protocol::MaxNickLength));
        return;
    }
    addMessage(o, false, Trust::Verified, true);

    const QString authorOnion = o.value("a").toString();
    if (m_typing.remove(authorOnion) > 0) // il a fini d'écrire
        emit typingChanged();
    // L'auteur vient d'écrire (message relayé) : il est en ligne, inutile d'attendre pour le rappeler
    if (!m_active.contains(authorOnion) && m_backoff.remove(authorOnion) > 0)
        connectToMembers();

    // Relais : au cas où un membre ne serait pas encore relié directement à l'auteur
    broadcast(o, c);
    emit messagesReceived({o}, false);
}

void GroupNode::onPeerTyping(PeerConnection *c)
{
    const QString from = m_active.key(c);
    if (from.isEmpty())
        return;
    m_typing.insert(from, now() + m_timing.typingShownMs);
    if (!m_typingTimer.isActive())
        m_typingTimer.start();
    emit typingChanged();
}

// ---------------------------------------------------------------- Réactions

void GroupNode::react(const QString &id, const QString &emoji)
{
    if (id.isEmpty() || m_ownOnion.isEmpty() || !m_group.inGroup() || !Reactions::Allowed.contains(emoji))
        return;
    const bool on = !m_reactions.mineOn(id, emoji); // la nôtre est déjà là : on la retire
    const QJsonObject r = Reactions::create(m_group.identitySeed(), m_ownOnion, id, emoji, on, now());
    acceptReaction(r, true, true);
    broadcastReaction(r);
}

bool GroupNode::verifyReaction(const QJsonObject &r, bool mine) const
{
    return Reactions::verify(r, authorKey(r, mine));
}

bool GroupNode::acceptReaction(const QJsonObject &r, bool mine, bool save)
{
    const QString id = Protocol::Message::id(r);
    if (id.isEmpty() || m_store.contains(id))
        return false;
    m_store.insert(id, History::stored(r, mine)); // synchronisée comme les messages
    m_seenIds.insert(id);
    if (m_reactions.apply(r, mine))
        emit reactionsChanged(Reactions::target(r));
    if (save)
        appendToHistoryFile(r, mine);
    return true;
}

void GroupNode::broadcastReaction(const QJsonObject &r, PeerConnection *except)
{
    for (PeerConnection *p : std::as_const(m_active))
        if (p != except && p->peerVersion() >= Reactions::Version)
            p->send(r);
}

// ---------------------------------------------------------------- Modifications et suppressions

bool GroupNode::canEdit(const QString &id) const
{
    const QJsonObject stored = m_store.value(id);
    if (id.isEmpty() || stored.isEmpty() || Reactions::isReaction(stored) || Edits::isEdit(stored)
        || (!stored.value("mine").toBool() && !isAdmin()))
        return false;
    const auto latest = latestEdit(stored);
    return !latest || !latest->deleted;
}

bool GroupNode::editMessage(const QString &id, const QString &text)
{
    const QString trimmed = text.trimmed();
    if (m_ownOnion.isEmpty() || !m_group.inGroup() || !canEdit(id) || trimmed.isEmpty()
        || trimmed.size() > Edits::MaxTextLength)
        return false;
    const QJsonObject r = Edits::create(m_group.identitySeed(), m_ownOnion, id, trimmed, false, now());
    acceptEdit(r, true, true);
    broadcastEdit(r);
    return true;
}

bool GroupNode::deleteMessage(const QString &id)
{
    if (m_ownOnion.isEmpty() || !m_group.inGroup() || !canEdit(id))
        return false;
    const QJsonObject r = Edits::create(m_group.identitySeed(), m_ownOnion, id, {}, true, now());
    acceptEdit(r, true, true);
    broadcastEdit(r);
    return true;
}

std::optional<Edits::Latest> GroupNode::latestEdit(const QJsonObject &msg) const
{
    // Seules comptent les modifications de l'auteur du message et des administrateurs (signatures vérifiées)
    return m_edits.latest(Protocol::Message::id(msg), editors(msg));
}

QStringList GroupNode::editors(const QJsonObject &msg) const
{
    // Administrateur à un moment de la chaîne : une modération reste valable s'il est retiré ensuite, et
    // tous les membres en jugent de la même façon
    QStringList out{msg.value("a").toString()};
    for (const Governance::Record &r : m_chain)
        for (const QString &a : r.admins)
            if (!out.contains(a))
                out << a;
    return out;
}

bool GroupNode::acceptEdit(const QJsonObject &r, bool mine, bool save)
{
    const QString id = Protocol::Message::id(r);
    if (id.isEmpty() || m_store.contains(id))
        return false;
    m_store.insert(id, History::stored(r, mine)); // synchronisée comme les messages
    m_seenIds.insert(id);
    if (m_edits.apply(r))
        emit messageEdited(Edits::target(r));
    if (save)
        appendToHistoryFile(r, mine);
    return true;
}

void GroupNode::broadcastEdit(const QJsonObject &r, PeerConnection *except)
{
    for (PeerConnection *p : std::as_const(m_active))
        if (p != except && p->peerVersion() >= Edits::Version)
            p->send(r);
}
