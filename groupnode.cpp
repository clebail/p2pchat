#include "groupnode.h"

#include "avatar.h"
#include "crypto.h"
#include "invitation.h"
#include "peerconnection.h"
#include "switchboard.h"

#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QtEndian>

#include <algorithm>

using Protocol::isValidOnion;

namespace {

constexpr int MaxSeenIds = 5000;
constexpr int MaxPending = 16; // connexions entrantes pas encore authentifiées

qint64 now()
{
    return QDateTime::currentMSecsSinceEpoch();
}

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

// ---------------------------------------------------------------- Fichiers

void GroupNode::setFilesDir(const QString &dir)
{
    m_files = std::make_unique<Files::FileStore>(dir);
    m_files->cleanup(now());
    m_lastCleanup = now();
}

std::optional<Files::FileInfo> GroupNode::verifiedFile(const QJsonObject &msg, bool mine) const
{
    return Protocol::Message::verifiedFile(msg, authorKey(msg, mine));
}

QList<PeerConnection *> GroupNode::fileCapablePeers() const
{
    QList<PeerConnection *> peers;
    for (PeerConnection *p : m_active)
        if (p->peerVersion() >= Protocol::FilesVersion)
            peers << p;
    return peers;
}

QJsonObject GroupNode::sendFile(const QString &path, const QString &caption, const QString &replyTo, QString *error)
{
    if (!canSend()) {
        if (error)
            *error = tr("pas de conversation, ou réseau pas encore prêt");
        return {};
    }
    const auto info = m_files->import(path, error);
    if (!info)
        return {};
    m_fileInfos.insert(info->id(), *info);
    advertiseFiles({info->id()}); // avant l'annonce : ceux qui la reçoivent savent à qui le demander
    return sendMessage(caption, {replyTo, {}, info->toJson()});
}

void GroupNode::noteFileAnnounce(const QJsonObject &msg, bool mine)
{
    if (!msg.contains("f"))
        return;
    const auto info = verifiedFile(msg, mine);
    if (!info)
        return;
    const QString id = info->id();
    m_fileInfos.insert(id, *info);
    if (mine || m_files->has(id) || m_downloads.contains(id))
        return;
    // Petit fichier récent, ou téléchargement interrompu : on le (re)prend sans attendre de clic
    const bool recent = Protocol::Message::timestamp(msg) >= now() - Files::RetentionMs;
    const bool interrupted = m_files->received(id).count(true) > 0;
    if (interrupted || (recent && info->size <= Files::AutoDownloadBytes))
        download(id);
}

void GroupNode::download(const QString &fileId)
{
    const auto it = m_fileInfos.constFind(fileId);
    if (it == m_fileInfos.cend() || m_files->has(fileId) || m_downloads.contains(fileId))
        return;
    QString error;
    if (!m_files->begin(*it, &error)) {
        qWarning().noquote() << "[fichiers] téléchargement impossible :" << error;
        return;
    }
    m_downloads.insert(fileId, {});
    emit fileChanged(fileId);
    pumpDownloads();
}

void GroupNode::cancelDownload(const QString &fileId)
{
    if (!m_downloads.remove(fileId))
        return;
    if (!m_files->has(fileId))
        m_files->remove(fileId);
    emit fileChanged(fileId);
}

GroupNode::FileState GroupNode::fileState(const QString &fileId) const
{
    FileState state;
    const auto info = m_fileInfos.constFind(fileId);
    if (info == m_fileInfos.cend() && !m_files->has(fileId))
        return state;
    state.total = info != m_fileInfos.cend() ? info->chunkCount() : 0;
    if (m_files->has(fileId)) {
        state.kind = FileState::Complete;
        state.received = state.total;
        return state;
    }
    state.received = m_files->received(fileId).count(true);
    const auto d = m_downloads.constFind(fileId);
    if (d == m_downloads.cend()) {
        state.kind = FileState::NotDownloaded;
    } else if (!d->inFlight.isEmpty()) {
        state.kind = FileState::Downloading;
        for (const Request &r : d->inFlight)
            if (!state.sources.contains(r.source))
                state.sources << r.source;
    } else {
        state.kind = FileState::Waiting; // aucun membre connecté ne l'a pour l'instant
    }
    return state;
}

bool GroupNode::peerHasChunk(const QString &peer, const QString &fileId, int chunk) const
{
    const auto files = m_peerFiles.constFind(peer);
    if (files == m_peerFiles.cend())
        return false;
    const auto have = files->constFind(fileId);
    if (have == files->cend())
        return false;
    return have->isEmpty() || (chunk < have->size() && have->testBit(chunk)); // vide : fichier complet
}

void GroupNode::advertiseFiles(const QStringList &ids, PeerConnection *only)
{
    QJsonArray files;
    for (const QString &id : ids) {
        if (m_files->has(id)) {
            files.append(QJsonObject{{"f", id}, {"have", "all"}});
        } else {
            const QBitArray have = m_files->received(id);
            if (have.count(true) == 0)
                continue;
            QString bits(have.size(), QLatin1Char('0'));
            for (int i = 0; i < have.size(); ++i)
                if (have.testBit(i))
                    bits[i] = QLatin1Char('1');
            files.append(QJsonObject{{"f", id}, {"have", bits}});
        }
    }
    if (files.isEmpty())
        return;
    const QJsonObject msg{{"t", "file-have"}, {"files", files}};
    if (only)
        only->send(msg);
    else
        for (PeerConnection *p : fileCapablePeers())
            p->send(msg);
}

void GroupNode::onFileHave(PeerConnection *c, const QJsonObject &o)
{
    const QString peer = m_active.key(c);
    if (peer.isEmpty())
        return;
    static const QRegularExpression idRe(QStringLiteral("^[0-9a-f]{64}$"));
    auto &known = m_peerFiles[peer];
    for (const QJsonValue &v : o.value("files").toArray()) {
        const QJsonObject f = v.toObject();
        const QString id = f.value("f").toString();
        if (!idRe.match(id).hasMatch() || (known.size() >= 5000 && !known.contains(id)))
            continue; // garde-fou : un membre ne peut pas nous faire tout retenir
        const QString have = f.value("have").toString();
        if (have == QLatin1String("all")) {
            known.insert(id, QBitArray());
        } else if (have.size() <= Files::FileInfo::expectedChunks(Files::MaxFileBytes)) {
            QBitArray bits(have.size());
            for (int i = 0; i < have.size(); ++i)
                bits.setBit(i, have.at(i) == QLatin1Char('1'));
            known.insert(id, bits);
        }
    }
    pumpDownloads();
}

void GroupNode::onFileWant(PeerConnection *c, const QJsonObject &o)
{
    const QString peer = m_active.key(c);
    if (peer.isEmpty())
        return;
    auto &queue = m_serveQueue[peer];
    const QPair<QString, int> request{o.value("f").toString(), o.value("i").toInt(-1)};
    if (queue.size() < 8 && !queue.contains(request)) // il ne demande qu'un morceau à la fois
        queue.append(request);
    serveQueued();
}

void GroupNode::serveQueued()
{
    for (auto it = m_serveQueue.begin(); it != m_serveQueue.end();) {
        PeerConnection *c = m_active.value(it.key());
        if (!c) {
            it = m_serveQueue.erase(it);
            continue;
        }
        // Un morceau à la fois, et seulement quand la connexion a écoulé son retard : les messages passent
        if (!it->isEmpty() && c->pendingBytes() < m_timing.fileBacklogBytes) {
            const QPair<QString, int> request = it->takeFirst();
            const QByteArray data = m_files->readChunk(request.first, request.second);
            if (data.isEmpty()) {
                c->send({{"t", "file-none"}, {"f", request.first}});
            } else if (c->peerVersion() >= Protocol::BinaryVersion) { // brut : un tiers de moins qu'en base64
                char index[4];
                qToBigEndian<quint32>(quint32(request.second), index);
                c->sendBinary(Protocol::FileChunkFrame,
                              QByteArray::fromHex(request.first.toLatin1()) + QByteArray(index, 4) + data);
            } else {
                c->send({{"t", "file-chunk"}, {"f", request.first}, {"i", request.second},
                         {"d", Protocol::toB64(data)}});
            }
        }
        it = it->isEmpty() ? m_serveQueue.erase(it) : std::next(it);
    }
}

void GroupNode::onFileChunk(PeerConnection *c, const QJsonObject &o)
{
    handleChunk(c, o.value("f").toString(), o.value("i").toInt(-1), Protocol::fromB64(o.value("d")));
}

void GroupNode::onBinary(PeerConnection *c, quint8 type, const QByteArray &payload)
{
    if (type != Protocol::FileChunkFrame || payload.size() < 36)
        return; // type inconnu (version plus récente) : ignoré
    ++m_binaryChunks;
    const QString id = QString::fromLatin1(payload.left(32).toHex());
    const int index = int(qFromBigEndian<quint32>(payload.constData() + 32));
    handleChunk(c, id, index, payload.mid(36));
}

void GroupNode::handleChunk(PeerConnection *c, const QString &id, int index, const QByteArray &data)
{
    const QString peer = m_active.key(c);
    const auto it = m_downloads.find(id);
    if (peer.isEmpty() || it == m_downloads.end())
        return;
    const auto req = std::find_if(it->inFlight.begin(), it->inFlight.end(),
                                  [&](const Request &r) { return r.source == peer && r.chunk == index; });
    if (req == it->inFlight.end())
        return; // pas demandé (ou plus attendu)
    const qint64 took = now() - req->at;
    it->inFlight.erase(req);
    switch (data.size() > Files::ChunkBytes ? Files::FileStore::ChunkResult::Rejected
                                            : m_files->writeChunk(id, index, data)) {
    case Files::FileStore::ChunkResult::Accepted: {
        // Débit de cette source (moyenne glissante), et le même morceau demandé ailleurs n'est plus attendu
        const double ms = double(std::max<qint64>(took, 1));
        m_chunkMs[peer] = m_chunkMs.contains(peer) ? 0.7 * m_chunkMs.value(peer) + 0.3 * ms : ms;
        it->inFlight.erase(std::remove_if(it->inFlight.begin(), it->inFlight.end(),
                                          [&](const Request &r) { return r.chunk == index; }),
                           it->inFlight.end());
        // Notre progression : nous devenons source pour les autres
        if (++it->sinceAnnounce >= 8 || now() - it->announcedAt > 5000) {
            it->sinceAnnounce = 0;
            it->announcedAt = now();
            advertiseFiles({id});
        }
        break;
    }
    case Files::FileStore::ChunkResult::Duplicate:
        break;
    case Files::FileStore::ChunkResult::Completed:
        m_downloads.erase(it);
        advertiseFiles({id}); // les autres peuvent désormais le demander aussi à nous
        break;
    case Files::FileStore::ChunkResult::Rejected:
        qWarning().noquote() << "[fichiers] morceau faux reçu de" << peer.left(8) << ": source écartée";
        it->bad.insert(peer);
        dropRequests(peer, id);
        break;
    case Files::FileStore::ChunkResult::Unknown:
        m_downloads.erase(it); // fiche perdue (magasin vidé) : abandon
        break;
    }
    emit fileChanged(id);
    pumpDownloads();
}

void GroupNode::onFileNone(PeerConnection *c, const QJsonObject &o)
{
    const QString peer = m_active.key(c);
    const QString id = o.value("f").toString();
    if (peer.isEmpty())
        return;
    if (m_peerFiles.contains(peer))
        m_peerFiles[peer].remove(id);
    dropRequests(peer, id);
    pumpDownloads();
}

void GroupNode::dropRequests(const QString &peer, const QString &fileId)
{
    for (auto it = m_downloads.begin(); it != m_downloads.end(); ++it) {
        if (!fileId.isEmpty() && it.key() != fileId)
            continue;
        const auto before = it->inFlight.size();
        it->inFlight.erase(std::remove_if(it->inFlight.begin(), it->inFlight.end(),
                                          [&](const Request &r) { return r.source == peer; }),
                           it->inFlight.end());
        if (it->inFlight.size() != before)
            emit fileChanged(it.key());
    }
}

void GroupNode::pumpDownloads()
{
    // Morceaux en vol par membre, tous fichiers confondus
    QHash<QString, int> load;
    for (const Download &d : std::as_const(m_downloads))
        for (const Request &r : d.inFlight)
            ++load[r.source];
    // Les plus rapides d'abord ; une source trois fois plus lente que la meilleure n'a qu'un morceau en vol
    QStringList peers;
    for (PeerConnection *p : fileCapablePeers())
        peers << m_active.key(p);
    std::sort(peers.begin(), peers.end(), [this](const QString &a, const QString &b) {
        return m_chunkMs.value(a, 0) < m_chunkMs.value(b, 0); // inconnue : essayée en priorité
    });
    double best = 0;
    for (const QString &p : std::as_const(peers))
        if (m_chunkMs.contains(p) && (best == 0 || m_chunkMs.value(p) < best))
            best = m_chunkMs.value(p);
    auto capacity = [&](const QString &peer) {
        const bool slow = best > 0 && m_chunkMs.value(peer, 0) > 3 * best;
        return (slow ? 1 : PerSource) - load.value(peer);
    };

    for (auto it = m_downloads.begin(); it != m_downloads.end(); ++it) {
        const QString id = it.key();
        const QBitArray have = m_files->received(id);
        if (have.isEmpty())
            continue;
        QHash<int, int> requested; // morceau -> demandes en vol
        QSet<QString> sources;
        for (const Request &r : std::as_const(it->inFlight)) {
            ++requested[r.chunk];
            sources.insert(r.source);
        }
        // Rareté de chaque morceau manquant : combien de membres connectés l'ont
        QHash<int, int> holders;
        for (int i = 0; i < have.size(); ++i)
            if (!have.testBit(i))
                for (const QString &p : std::as_const(peers))
                    if (!it->bad.contains(p) && peerHasChunk(p, id, i))
                        ++holders[i];
        bool sent = false;
        for (const QString &peer : std::as_const(peers)) {
            if (it->bad.contains(peer))
                continue;
            while (capacity(peer) > 0 && (sources.contains(peer) || sources.size() < MaxSources)) {
                // Le plus rare de ceux qu'il a et que personne n'envoie encore ; à défaut (fin du
                // téléchargement), un morceau déjà demandé à un autre, une seule fois de plus
                int pick = -1, pickHolders = 0, pickRequests = 0;
                for (auto h = holders.cbegin(); h != holders.cend(); ++h) {
                    const int i = h.key(), asked = requested.value(i);
                    if (asked >= 2 || !peerHasChunk(peer, id, i))
                        continue;
                    bool mine = false;
                    for (const Request &r : std::as_const(it->inFlight))
                        mine = mine || (r.source == peer && r.chunk == i);
                    if (mine)
                        continue;
                    const bool better = pick < 0 || asked < pickRequests
                                        || (asked == pickRequests && (h.value() < pickHolders
                                                                      || (h.value() == pickHolders && i < pick)));
                    if (better) {
                        pick = i;
                        pickHolders = h.value();
                        pickRequests = asked;
                    }
                }
                if (pick < 0 || (pickRequests > 0 && requested.size() < holders.size()))
                    break; // rien pour lui, ou il reste des morceaux que personne n'envoie
                m_active.value(peer)->send({{"t", "file-want"}, {"f", id}, {"i", pick}});
                it->inFlight.append({peer, pick, now()});
                ++requested[pick];
                ++load[peer];
                sources.insert(peer);
                sent = true;
            }
        }
        if (sent)
            emit fileChanged(id);
    }
}

void GroupNode::filesTick()
{
    // Morceau demandé resté sans réponse : on le redemande, éventuellement à un autre
    const qint64 t = now();
    bool expired = false;
    for (auto it = m_downloads.begin(); it != m_downloads.end(); ++it) {
        const auto before = it->inFlight.size();
        it->inFlight.erase(std::remove_if(it->inFlight.begin(), it->inFlight.end(),
                                          [&](const Request &r) { return t - r.at > m_timing.fileRequestTimeoutMs; }),
                           it->inFlight.end());
        expired = expired || it->inFlight.size() != before;
    }
    if (expired)
        pumpDownloads();
    if (!m_serveQueue.isEmpty())
        serveQueued();
    if (t - m_lastCleanup > 12LL * 3600 * 1000) { // fichiers de plus de 30 jours
        m_lastCleanup = t;
        m_files->cleanup(t);
    }
}

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
