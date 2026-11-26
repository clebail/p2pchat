#include "chatmodel.h"

#include <QLocale>
#include <QHash>
#include <QStringList>

namespace {

QString eventText(const PresenceEvent &ev)
{
    if (ev.online)
        return QObject::tr("%1 est en ligne.").arg(ev.who);
    return ev.reason.isEmpty() ? QObject::tr("%1 s'est déconnecté(e).").arg(ev.who)
                               : QObject::tr("%1 s'est déconnecté(e) : %2").arg(ev.who, ev.reason);
}

// Résumé d'une ligne de présence : état final de chaque membre concerné
} // namespace

QString ChatModel::presenceText(const ChatEntry &e)
{
    if (e.events.size() == 1)
        return eventText(e.events.first());

    QStringList online, offline; // dans l'ordre de première apparition
    QHash<QString, bool> last;
    for (const PresenceEvent &ev : e.events) {
        if (!last.contains(ev.who))
            (ev.online ? online : offline).append(ev.who);
        last.insert(ev.who, ev.online);
    }
    QStringList on, off;
    for (const QString &who : online + offline)
        (last.value(who) ? on : off).append(who);

    QStringList parts;
    if (!on.isEmpty())
        parts << (on.size() == 1 ? QObject::tr("%1 est en ligne") : QObject::tr("%1 sont en ligne"))
                     .arg(on.join(", "));
    if (!off.isEmpty())
        parts << (off.size() == 1 ? QObject::tr("%1 s'est déconnecté(e)") : QObject::tr("%1 se sont déconnecté(e)s"))
                     .arg(off.join(", "));
    QString text = parts.join(QStringLiteral(" · "));

    if (!e.expanded)
        return text + QObject::tr("  ▾ %1 évènements").arg(e.events.size());
    text += QStringLiteral("  ▴");
    for (const PresenceEvent &ev : e.events)
        text += QChar(QChar::LineSeparator) + ev.ts.toString("HH:mm") + QStringLiteral("  ") + eventText(ev);
    return text;
}

// ---------------------------------------------------------------- Modèle

QVariant ChatModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_entries.size())
        return {};
    if (role == Qt::DisplayRole || role == Qt::ToolTipRole)
        return role == Qt::ToolTipRole ? QLocale().toString(m_entries.at(index.row()).ts, QLocale::ShortFormat)
                                       : m_entries.at(index.row()).text;
    return {};
}

void ChatModel::append(const ChatEntry &e)
{
    const int row = int(m_entries.size());
    beginInsertRows({}, row, row);
    m_entries.append(e);
    endInsertRows();
    // La ligne précédente peut changer d'aspect (fin de groupe : heure, avatar, coins)
    if (row > 0)
        emit dataChanged(index(row - 1), index(row - 1));
}

int ChatModel::rowOf(const QString &id) const
{
    if (id.isEmpty())
        return -1;
    for (int i = int(m_entries.size()) - 1; i >= 0; --i)
        if (m_entries.at(i).id == id)
            return i;
    return -1;
}

bool ChatModel::setReactions(const QString &id, const QList<ChatEntry::Reaction> &reactions)
{
    const int row = rowOf(id);
    if (row < 0)
        return false;
    m_entries[row].reactions = reactions;
    emit dataChanged(index(row), index(row));
    return true;
}

bool ChatModel::applyEdit(const QString &id, const ChatEntry &updated)
{
    const int row = rowOf(id);
    if (row < 0)
        return false;
    ChatEntry &e = m_entries[row];
    e.text = updated.text;
    e.markup = updated.markup;
    e.links = updated.links;
    e.mentions = updated.mentions;
    e.mentionsMe = updated.mentionsMe;
    e.edited = updated.edited;
    e.deleted = updated.deleted;
    e.editedBy = updated.editedBy;
    if (e.deleted) { // plus d'image ni de fichier
        e.image = QImage();
        e.imageData.clear();
        e.animated = false;
        e.file = {};
    }
    emit dataChanged(index(row), index(row));
    // Les réponses qui le citent reprennent le nouveau texte
    for (int i = 0; i < m_entries.size(); ++i) {
        ChatEntry &other = m_entries[i];
        if (other.replyTo == id) {
            resolveReply(other);
            emit dataChanged(index(i), index(i));
        }
    }
    return true;
}

void ChatModel::setAuthorVerified(const QString &onion, bool verified)
{
    for (int i = 0; i < m_entries.size(); ++i) {
        ChatEntry &e = m_entries[i];
        if (!onion.isEmpty() && e.authorOnion == onion && e.authorVerified != verified) {
            e.authorVerified = verified;
            emit dataChanged(index(i), index(i));
        }
    }
}

void ChatModel::setFileState(const QString &fileId, ChatEntry::File::State state, int received, int total,
                             const QString &sources)
{
    for (int i = 0; i < m_entries.size(); ++i) {
        ChatEntry::File &f = m_entries[i].file;
        if (fileId.isEmpty() || f.id != fileId)
            continue;
        f.state = state;
        f.received = received;
        f.total = total;
        f.sources = sources;
        emit dataChanged(index(i), index(i));
    }
}

int ChatModel::markerRow() const
{
    for (int i = int(m_entries.size()) - 1; i >= 0; --i)
        if (m_entries.at(i).kind == ChatEntry::Marker)
            return i;
    return -1;
}

void ChatModel::removeRow(int row)
{
    if (row < 0 || row >= m_entries.size())
        return;
    beginRemoveRows({}, row, row);
    m_entries.removeAt(row);
    endRemoveRows();
    // Les voisins se regroupent peut-être à nouveau
    const int first = qMax(0, row - 1), last = qMin(int(m_entries.size()) - 1, row);
    if (first <= last)
        emit dataChanged(index(first), index(last));
}

QString ChatModel::fold(const QString &text)
{
    QString out = text;
    for (int i = 0; i < out.size(); ++i) {
        QChar c = out.at(i);
        while (c.decompositionTag() == QChar::Canonical && !c.decomposition().isEmpty())
            c = c.decomposition().at(0); // « é » -> « e »
        out[i] = c.toLower();
    }
    return out;
}

QList<int> ChatModel::search(const QString &term) const
{
    const QString t = fold(term.trimmed());
    QList<int> rows;
    if (t.isEmpty())
        return rows;
    for (int i = 0; i < m_entries.size(); ++i)
        if (m_entries.at(i).kind == ChatEntry::Message && fold(m_entries.at(i).text).contains(t))
            rows.append(i);
    return rows;
}

void ChatModel::resolveReply(ChatEntry &e) const
{
    const int row = rowOf(e.replyTo);
    if (e.replyTo.isEmpty() || row < 0)
        return;
    const ChatEntry &quoted = m_entries.at(row);
    e.replyAuthor = quoted.mine ? QObject::tr("Vous") : quoted.author;
    e.replyText = !quoted.text.isEmpty()        ? quoted.text
                  : !quoted.image.isNull()      ? QObject::tr("📷 Image")
                  : !quoted.file.id.isEmpty()   ? QObject::tr("📎 %1").arg(quoted.file.name)
                                                : quoted.text;
    e.replyFound = true;
}

int ChatModel::insertSorted(const ChatEntry &entry)
{
    ChatEntry e = entry;
    resolveReply(e);
    int row = int(m_entries.size());
    while (row > 0 && m_entries.at(row - 1).ts > e.ts)
        --row;
    beginInsertRows({}, row, row);
    m_entries.insert(row, e);
    endInsertRows();
    // Réponses arrivées avant ce message (synchronisation) : leur citation devient disponible
    if (!e.id.isEmpty()) {
        for (int i = 0; i < m_entries.size(); ++i) {
            ChatEntry &other = m_entries[i];
            if (!other.replyFound && other.replyTo == e.id) {
                resolveReply(other);
                emit dataChanged(index(i), index(i));
            }
        }
    }
    // Les voisins peuvent changer d'aspect (regroupement : nom, heure, avatar, coins)
    const int first = qMax(0, row - 1), last = qMin(int(m_entries.size()) - 1, row + 1);
    emit dataChanged(index(first), index(last));
    return row;
}

void ChatModel::addPresenceEvent(int row, const PresenceEvent &ev)
{
    ChatEntry &e = m_entries[row];
    e.events.append(ev);
    e.ts = ev.ts;
    e.text = presenceText(e);
    emit dataChanged(index(row), index(row));
}

void ChatModel::toggleExpanded(int row)
{
    ChatEntry &e = m_entries[row];
    if (e.kind != ChatEntry::Presence || e.events.size() < 2)
        return;
    e.expanded = !e.expanded;
    e.text = presenceText(e);
    emit dataChanged(index(row), index(row));
}

void ChatModel::clear()
{
    beginResetModel();
    m_entries.clear();
    endResetModel();
}
