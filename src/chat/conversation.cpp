#include "conversation.h"

#include "attachment.h"
#include "avatar.h"
#include "filestore.h"
#include "mentions.h"
#include "notifications.h"
#include "protocol.h"

#include <QDateTime>
#include <QImage>
#include <QSettings>

namespace {

ChatEntry::File::State fileEntryState(int kind)
{
    switch (GroupNode::FileState::Kind(kind)) {
    case GroupNode::FileState::NotDownloaded:
        return ChatEntry::File::NotDownloaded;
    case GroupNode::FileState::Downloading:
        return ChatEntry::File::Downloading;
    case GroupNode::FileState::Waiting:
        return ChatEntry::File::Waiting;
    case GroupNode::FileState::Complete:
        return ChatEntry::File::Complete;
    case GroupNode::FileState::Unknown:
        break;
    }
    return ChatEntry::File::NotDownloaded;
}

} // namespace

Conversation::Conversation(QSettings &settings, const QString &id, const QString &historyPath,
                           const GroupNode::Timing &timing, QObject *parent)
    : QObject(parent), m_id(id), m_store(std::make_unique<GroupStore>(settings, id)),
      m_node(std::make_unique<GroupNode>(*m_store, historyPath, timing)), m_view(new ChatView)
{
    connectNode();
    connectView();
    m_view->setAvatarProvider([this](const QString &onion) { return avatarOf(onion); });
}

Conversation::~Conversation()
{
    m_node.reset(); // avant la vue et le GroupStore, qu'il utilise
    delete m_view;  // si la fenêtre ne l'a pas déjà supprimée
}

void Conversation::loadHistory()
{
    m_view->setModerator(m_node->isAdmin());
    m_node->loadHistory();
    m_view->scrollToBottom();
    rememberName();
}

void Conversation::leave()
{
    rememberName();
    keepName();
    m_node->leaveGroup();
    emit changed();
}

void Conversation::rememberName()
{
    if (m_store->inGroup() && (!m_store->members().isEmpty() || !m_node->sharedName().isEmpty()))
        m_knownName = displayName();
}

void Conversation::keepName()
{
    // Sans membres ni nom partagé, elle s'appellerait « Ancienne conversation » : on garde le dernier nom
    if (!m_knownName.isEmpty())
        m_store->setName(m_knownName);
}

void Conversation::connectNode()
{
    GroupNode *n = m_node.get();
    connect(n, &GroupNode::messageAdded, this, &Conversation::appendMessage);
    connect(n, &GroupNode::messagesReceived, this, &Conversation::onMessagesReceived);
    connect(n, &GroupNode::reactionsChanged, this,
            [this](const QString &id) { m_view->setReactions(id, reactionSummary(id)); });
    connect(n, &GroupNode::memberAdded, this, [this](const QString &, const QString &nick, bool direct) {
        appendSystem(direct ? tr("%1 a rejoint la conversation.").arg(nick)
                            : tr("%1 fait partie de la conversation.").arg(nick));
    });
    connect(n, &GroupNode::presenceChanged, this,
            [this](const QString &onion, const QString &nick, bool online, const QString &reason) {
                m_view->addPresence(nick.isEmpty() ? memberNick(onion) : nick, online, reason);
            });
    connect(n, &GroupNode::identityRejected, this, [this](const QString &, const QString &nick) {
        appendSystem(tr("⚠ Connexion refusée : « %1 » ne présente pas la clé d'identité connue pour ce membre.")
                         .arg(nick));
    });
    connect(n, &GroupNode::invalidMessage, this, [this](const QString &author) {
        appendSystem(tr("⚠ Message attribué à « %1 » rejeté : signature invalide.").arg(author));
    });
    connect(n, &GroupNode::memberExcluded, this, [this](const QString &, const QString &nick, const QString &by) {
        appendSystem(tr("%1 ne fait plus partie du groupe (exclusion par %2).").arg(nick, by));
    });
    connect(n, &GroupNode::secretRenewed, this, [this](const QString &by) {
        appendSystem(tr("Nouveau secret du groupe (renouvelé par %1) : les anciens codes d'invitation ne "
                        "fonctionnent plus.").arg(by));
    });
    connect(n, &GroupNode::memberLeft, this, [this](const QString &, const QString &nick) {
        appendSystem(tr("%1 a quitté le groupe.").arg(nick));
    });
    connect(n, &GroupNode::excludedFromGroup, this, [this](const QString &by) {
        keepName(); // groupe déjà quitté : le nom retenu avant l'exclusion
        appendSystem(tr("⚠ %1 vous a exclu du groupe. L'historique est conservé ; il faudra une nouvelle "
                        "invitation pour revenir.").arg(by));
        emit excludedFromGroup(by);
        emit changed();
    });
    connect(n, &GroupNode::adminChanged, this,
            [this](const QString &, const QString &nick, bool admin, const QString &by) {
                appendSystem(admin ? tr("%1 est maintenant administrateur (nommé par %2).").arg(nick, by)
                         : nick == by ? tr("%1 n'est plus administrateur.").arg(nick)
                                      : tr("%1 n'est plus administrateur (décision de %2).").arg(nick, by));
            });
    connect(n, &GroupNode::sharedNameChanged, this, [this](const QString &name, const QString &by) {
        appendSystem(name.isEmpty() ? tr("%1 a retiré le nom de la conversation.").arg(by)
                                    : tr("%1 a nommé la conversation « %2 ».").arg(by, name));
    });
    connect(n, &GroupNode::conversationAvatarChanged, this, [this](bool removed, const QString &by) {
        m_conversationAvatar.reset();
        appendSystem(removed ? tr("%1 a retiré l'avatar de la conversation.").arg(by)
                             : tr("%1 a changé l'avatar de la conversation.").arg(by));
        emit changed();
    });
    connect(n, &GroupNode::directInvitation, this,
            [this](const QString &, const QString &nick, const Invitation &inv, const QString &name) {
                emit directInvitation(nick, inv, name);
            });
    connect(n, &GroupNode::stateChanged, this, [this] {
        m_conversationAvatar.reset(); // groupe quitté : effacé avec lui
        m_view->setModerator(m_node->isAdmin());
        rememberName();
        emit changed();
    });
    connect(n, &GroupNode::avatarChanged, this, [this](const QString &onion) {
        forgetAvatar(onion);
        emit avatarChanged(onion);
    });
    connect(n, &GroupNode::typingChanged, this, &Conversation::changed);
    connect(n, &GroupNode::fileChanged, this, &Conversation::updateFileEntry);
    connect(n, &GroupNode::messageEdited, this, &Conversation::onMessageEdited);
}

void Conversation::connectView()
{
    connect(m_view, &ChatView::fileRequested, this, [this](const QString &id, ChatView::FileAction action) {
        switch (action) {
        case ChatView::FileAction::Download:
            m_node->download(id);
            break;
        case ChatView::FileAction::Cancel:
            m_node->cancelDownload(id);
            break;
        case ChatView::FileAction::Open:
            emit openFileRequested(id);
            break;
        case ChatView::FileAction::SaveAs:
            emit saveFileRequested(id);
            break;
        }
    });
    connect(m_view, &ChatView::replyRequested, this, &Conversation::replyRequested);
    connect(m_view, &ChatView::editRequested, this, &Conversation::editRequested);
    connect(m_view, &ChatView::deleteRequested, this, &Conversation::deleteRequested);
    connect(m_view, &ChatView::linkClicked, this, &Conversation::linkClicked);
    connect(m_view, &ChatView::reactionToggled, this,
            [this](const QString &id, const QString &emoji) { m_node->react(id, emoji); });
}

// ---------------------------------------------------------------- Ce que la fenêtre en montre

bool Conversation::isBlank() const
{
    return !m_store->inGroup() && m_view->chatModel()->rowCount() == 0;
}

QString Conversation::displayName() const
{
    // Nom gardé au départ (seulement hors du groupe : en groupe, seul celui de l'administrateur compte)
    if (!m_store->inGroup() && !m_store->name().isEmpty())
        return m_store->name();
    if (const QString shared = m_node->sharedName(); !shared.isEmpty())
        return shared; // choisi par un administrateur
    QStringList names;
    for (const Member &m : m_store->members())
        names << (m.nick.isEmpty() ? tr("?") : m.nick);
    if (!names.isEmpty())
        return names.join(", ");
    return m_store->inGroup() ? tr("Nouvelle conversation") : isBlank() ? tr("Conversation vierge")
                                                                         : tr("Ancienne conversation");
}

QImage Conversation::avatarOf(const QString &onion) const
{
    auto it = m_avatars.constFind(onion);
    if (it == m_avatars.constEnd())
        it = m_avatars.insert(onion, Avatar::decode(Avatar::image(m_store->avatarRecord(onion))));
    return *it;
}

QImage Conversation::conversationAvatar() const
{
    if (!m_conversationAvatar)
        m_conversationAvatar = Avatar::decode(m_node->conversationAvatar());
    return *m_conversationAvatar;
}

QPixmap Conversation::avatarPixmap(int side, qreal ratio) const
{
    const QString shared = m_node->sharedName();
    const QString name = shared.isEmpty() ? displayName() : shared;
    const QString group = m_node->groupId();
    return ChatView::roundAvatar(conversationAvatar(), name, side, ratio, group.isEmpty() ? name : group);
}

void Conversation::forgetAvatar(const QString &onion)
{
    if (m_avatars.remove(onion) > 0 && m_view)
        m_view->viewport()->update();
}

QString Conversation::subtitle() const
{
    const QList<Member> list = m_store->members();
    const QString dot = QStringLiteral("<span style='color:%1'>●</span> ");
    if (!m_store->inGroup())
        return isBlank() ? tr("Nouvelle conversation") : tr("Vous avez quitté cette conversation");
    if (list.isEmpty())
        return dot.arg("#e37400") + tr("En attente de la première personne invitée…");
    if (const QString typing = typingText(); !typing.isEmpty())
        return dot.arg("#188038") + QStringLiteral("<i>%1</i>").arg(typing.toHtmlEscaped());
    // Les membres vus en ligne par un autre sont joignables : nos messages passent par lui
    const QHash<QString, QString> relayed = m_node->relayedOnline();
    QStringList online;
    bool allDirect = true;
    for (const Member &m : list) {
        if (m_node->isConnected(m.onion)) {
            online << m.nick.toHtmlEscaped();
        } else if (relayed.contains(m.onion)) {
            online << tr("%1 (via %2)").arg(m.nick, memberNick(relayed.value(m.onion))).toHtmlEscaped();
            allDirect = false;
        }
    }
    if (online.isEmpty())
        return dot.arg("#9aa0a6") + tr("Personne n'est en ligne");
    if (online.size() == list.size() && allDirect)
        return dot.arg("#188038") + (list.size() == 1 ? tr("En ligne") : tr("Tout le monde est en ligne"));
    return dot.arg("#188038") + tr("En ligne : %1").arg(online.join(", "));
}

QString Conversation::subtitleToolTip() const
{
    const QHash<QString, QString> relayed = m_node->relayedOnline();
    QStringList tips;
    for (const Member &m : m_store->members()) {
        const bool connected = m_node->isConnected(m.onion), attempting = m_node->isAttempting(m.onion);
        QString state = connected                   ? tr("en ligne")
                        : relayed.contains(m.onion) ? tr("en ligne via %1").arg(memberNick(relayed.value(m.onion)))
                        : attempting                ? tr("connexion…")
                                                    : tr("hors ligne");
        if (relayed.contains(m.onion) && attempting)
            state += tr(", connexion directe…");
        if (const QStringList missing = Protocol::missingFeatures(m_node->peerVersion(m.onion));
            connected && !missing.isEmpty())
            state += tr(" — ancienne version de P2P Chat (ne voit pas %1)").arg(missing.join(", "));
        if (!connected && !m_node->lastError(m.onion).isEmpty())
            state += " — " + m_node->lastError(m.onion);
        if (!connected && !attempting && m_node->nextTry(m.onion) > QDateTime::currentMSecsSinceEpoch())
            state += tr(" (nouvel essai à %1)")
                         .arg(QDateTime::fromMSecsSinceEpoch(m_node->nextTry(m.onion)).toString("HH:mm:ss"));
        const QString name = m_store->isVerified(m.onion) ? tr("%1 ✔").arg(m.nick) : m.nick;
        tips << QStringLiteral("%1 : %2").arg(name, state).toHtmlEscaped();
    }
    return tips.isEmpty() ? QString() : "<p>" + tips.join("<br>") + "</p>";
}

// ---------------------------------------------------------------- Non-lus et notifications

void Conversation::addUnread(int count, qint64 firstTs)
{
    m_view->markUnread(QDateTime::fromMSecsSinceEpoch(firstTs));
    m_unread += count;
    emit changed();
}

void Conversation::markRead()
{
    m_view->revealUnreadMarker();
    if (m_unread == 0)
        return;
    m_unread = 0;
    emit changed();
}

bool Conversation::notificationsAllowed(bool forMe, qint64 pausedUntil, qint64 nowMs) const
{
    return Notifications::shouldNotify(Notifications::modeFromString(m_store->notifyMode()), pausedUntil, nowMs,
                                       forMe);
}

void Conversation::appendSystem(const QString &text)
{
    m_view->addSystem(text);
}

// ---------------------------------------------------------------- Messages

void Conversation::appendMessage(const QJsonObject &msg, bool mine, GroupNode::Trust nodeTrust)
{
    const ChatEntry::Trust trust = nodeTrust == GroupNode::Trust::Verified   ? ChatEntry::Verified
                                   : nodeTrust == GroupNode::Trust::Unsigned ? ChatEntry::Unsigned
                                                                             : ChatEntry::Invalid;
    const qint64 ms = qint64(msg.value("ts").toDouble());
    const QDateTime ts = ms > 0 ? QDateTime::fromMSecsSinceEpoch(ms) : QDateTime::currentDateTime();
    // Image : seulement si elle est bien signée par l'auteur et décodable sans risque
    QByteArray imageData;
    QImage image;
    if (trust == ChatEntry::Verified && msg.contains("img")) {
        imageData = m_node->verifiedImage(msg, mine);
        image = Attachment::decode(imageData);
    }
    // Fichier joint : seulement si sa fiche est bien signée par l'auteur
    std::optional<Files::FileInfo> file;
    if (trust == ChatEntry::Verified && msg.contains("f"))
        file = m_node->verifiedFile(msg, mine);
    const QString text = Protocol::Message::caption(msg, !image.isNull() || file.has_value());
    QList<QPair<int, int>> spans;
    const QList<Mentions::Span> found = Mentions::find(text, mentionNames());
    for (const Mentions::Span &m : found)
        spans.append({m.start, m.length});
    const QString id = Protocol::Message::id(msg);
    ChatEntry e;
    e.author = msg.value("n").toString().left(Protocol::MaxNickLength);
    if (!mine && trust == ChatEntry::Verified) { // le ✔ ne vaut que pour un message bien signé
        e.authorOnion = msg.value("a").toString();
        e.authorVerified = m_store->isVerified(e.authorOnion);
    }
    e.text = text;
    e.ts = ts;
    e.mine = mine;
    e.trust = trust;
    e.mentions = spans;
    e.mentionsMe = !mine && (Mentions::mentions(found, m_store->nickname()) || repliesToMe(msg));
    if (trust == ChatEntry::Verified) { // identifiants et citations : seulement pour les messages authentiques
        e.id = id;
        e.replyTo = m_node->verifiedReplyTo(msg, mine);
        if (file) {
            e.file.id = file->id();
            e.file.name = file->name;
            e.file.mime = file->mime;
            e.file.size = file->size;
            e.file.risky = Files::isRisky(file->name);
            e.file.state = fileEntryState(m_node->fileState(e.file.id).kind);
            e.file.received = m_node->fileState(e.file.id).received;
            e.file.total = file->chunkCount();
        }
        if (!image.isNull()) {
            e.image = Attachment::thumbnail(image);
            e.imageData = imageData;
            e.animated = Attachment::isAnimated(imageData);
        }
    }
    e.reactions = reactionSummary(e.id); // réactions arrivées avant le message (synchronisation)
    m_view->addMessage(e);
    m_preview = mine ? tr("Vous : %1").arg(notificationBody(msg)) : tr("%1 : %2").arg(e.author, notificationBody(msg));
    m_previewId = e.id;
    if (!e.id.isEmpty() && m_node->latestEdit(msg))
        onMessageEdited(e.id); // modifié avant même d'arriver ici (synchronisation, historique)
    emit changed();
}

void Conversation::onMessagesReceived(const QList<QJsonObject> &messages, bool catchUp)
{
    int mentioned = 0;
    qint64 firstTs = 0;
    for (const QJsonObject &m : messages) {
        mentioned += (mentionsMe(m) || repliesToMe(m)) ? 1 : 0;
        const qint64 ts = Protocol::Message::timestamp(m);
        firstTs = firstTs ? qMin(firstTs, ts) : ts;
    }
    const int count = int(messages.size());
    if (count == 1 || !catchUp)
        emit messagesArrived(count, notificationTitle(messages.last()), notificationBody(messages.last()), firstTs,
                     mentioned > 0);
    else
        emit messagesArrived(count, tr("P2P Chat"),
                     tr("%n message(s) reçu(s) pendant votre absence", nullptr, count)
                         + (mentioned ? tr(", dont %n pour vous", nullptr, mentioned) : QString()),
                     firstTs, mentioned > 0);
}

QStringList Conversation::mentionNames() const
{
    QStringList names{m_store->nickname(), Mentions::Everyone};
    for (const Member &m : m_store->members())
        names << m.nick;
    return names;
}

bool Conversation::mentionsMe(const QJsonObject &msg) const
{
    return Mentions::mentions(Mentions::find(msg.value("x").toString(), mentionNames()), m_store->nickname());
}

bool Conversation::repliesToMe(const QJsonObject &msg) const
{
    const QString quoted = m_node->verifiedReplyTo(msg, false); // appelé pour les messages des autres
    return !quoted.isEmpty() && m_node->isMine(quoted);
}

QString Conversation::notificationTitle(const QJsonObject &msg) const
{
    const QString author = msg.value("n").toString().left(Protocol::MaxNickLength);
    if (repliesToMe(msg))
        return tr("%1 vous a répondu").arg(author);
    if (mentionsMe(msg))
        return tr("%1 vous a mentionné(e)").arg(author);
    return author;
}

QString Conversation::notificationBody(const QJsonObject &msg)
{
    if (msg.contains("f")) {
        const QString caption = Protocol::Message::caption(msg, true);
        const QString name = Files::sanitizeName(msg.value("f").toObject().value("n").toString());
        return caption.isEmpty() ? tr("📎 %1").arg(name) : tr("📎 %1 — %2").arg(name, caption);
    }
    if (!msg.contains("img"))
        return msg.value("x").toString();
    const QString caption = Protocol::Message::caption(msg, true);
    return caption.isEmpty() ? tr("📷 Image") : tr("📷 %1").arg(caption);
}

void Conversation::updateFileEntry(const QString &fileId)
{
    const GroupNode::FileState s = m_node->fileState(fileId);
    QStringList names;
    for (const QString &onion : s.sources)
        names << memberNick(onion);
    names.sort(Qt::CaseInsensitive);
    m_view->setFileState(fileId, fileEntryState(s.kind), s.received, s.total, names.join(", "));
}

QList<ChatEntry::Reaction> Conversation::reactionSummary(const QString &id) const
{
    QList<ChatEntry::Reaction> out;
    if (id.isEmpty())
        return out;
    for (const Reactions::Summary &s : m_node->reactionSummary(id)) {
        QStringList names;
        for (const QString &onion : s.authors)
            names << (onion.isEmpty() || onion == m_node->ownOnion() ? tr("Vous") : memberNick(onion));
        out.append({s.emoji, int(s.authors.size()), s.mine, names.join(", ")});
    }
    return out;
}

QString Conversation::typingText() const
{
    QStringList names;
    for (const QString &onion : m_node->typingMembers())
        names << memberNick(onion);
    names.sort(Qt::CaseInsensitive);
    if (names.isEmpty())
        return {};
    if (names.size() == 1)
        return tr("%1 écrit…").arg(names.first());
    if (names.size() == 2)
        return tr("%1 et %2 écrivent…").arg(names.at(0), names.at(1));
    return tr("Plusieurs personnes écrivent…");
}

QString Conversation::memberNick(const QString &onion) const
{
    const QString nick = m_store->nickOf(onion);
    return nick.isEmpty() ? tr("Un membre") : nick;
}

void Conversation::onMessageEdited(const QString &id)
{
    const QJsonObject msg = m_node->storedMessage(id);
    const auto latest = msg.isEmpty() ? std::nullopt : m_node->latestEdit(msg);
    if (!latest)
        return; // message pas (encore) là : appliqué à son arrivée
    const bool mine = msg.value("mine").toBool();
    // Modération : modifié ou supprimé par un administrateur, pas par son auteur
    QString by;
    if (latest->author != msg.value("a").toString())
        by = latest->author == m_node->ownOnion() ? tr("vous") : memberNick(latest->author);
    const QString text = !latest->deleted ? latest->text
                         : by.isEmpty()   ? tr("🚫 Message supprimé")
                                          : tr("🚫 Message supprimé par %1").arg(by);
    QList<QPair<int, int>> spans;
    const QList<Mentions::Span> found = Mentions::find(text, mentionNames());
    for (const Mentions::Span &m : found)
        spans.append({m.start, m.length});
    const bool forMe = !mine && (Mentions::mentions(found, m_store->nickname()) || repliesToMe(msg));
    m_view->applyEdit(id, text, latest->deleted, spans, forMe, by);
    if (id == m_previewId) {
        const QString author = msg.value("n").toString().left(Protocol::MaxNickLength);
        m_preview = mine ? tr("Vous : %1").arg(text) : tr("%1 : %2").arg(author, text);
        emit changed();
    }
}
