#include "groupnode.h"
#include "groupnode_p.h"

#include "peerconnection.h"

#include <QDebug>
#include <QtEndian>

#include <algorithm>

// GroupNode : fichiers joints (annonces, morceaux, sources, reprise) (le reste de GroupNode : groupnode.cpp)

using GroupNodePrivate::now;

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
        for (PeerConnection *p : activePeers(Protocol::FilesVersion))
            p->send(msg);
}

void GroupNode::onFileHave(PeerConnection *c, const QJsonObject &o)
{
    const QString peer = m_active.key(c);
    if (peer.isEmpty())
        return;
    auto &known = m_peerFiles[peer];
    for (const QJsonValue &v : o.value("files").toArray()) {
        const QJsonObject f = v.toObject();
        const QString id = f.value("f").toString();
        if (!Protocol::isHexDigest(id) || (known.size() >= 5000 && !known.contains(id)))
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
    for (PeerConnection *p : activePeers(Protocol::FilesVersion))
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
