#include "history.h"

#include "protocol.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QSet>

#include <algorithm>

using Protocol::Message::timestamp;

QList<History::Row> History::parse(const QByteArray &contents, qint64 clearedBefore)
{
    QList<Row> rows;
    QSet<QString> ids;
    for (const QByteArray &line : contents.split('\n')) {
        const QJsonObject o = QJsonDocument::fromJson(line).object();
        if (o.isEmpty())
            continue;
        Row row;
        if (o.contains("from")) {
            row.ts = QDateTime::fromString(o["ts"].toString(), Qt::ISODate).toMSecsSinceEpoch();
            row.msg = QJsonObject{{"n", o["from"]}, {"x", o["text"]}, {"ts", double(row.ts)}};
            row.legacy = true;
        } else {
            const QString id = o["id"].toString();
            if (!id.isEmpty() && ids.contains(id))
                continue;
            ids.insert(id);
            row.msg = o;
            row.ts = timestamp(o);
        }
        if (row.ts < clearedBefore)
            continue;
        row.mine = o["mine"].toBool();
        rows.append(row);
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.ts < b.ts; });
    return rows;
}

QJsonObject History::stored(const QJsonObject &msg, bool mine)
{
    QJsonObject o = msg;
    o.remove("t");
    o["mine"] = mine;
    return o;
}

QByteArray History::toLine(const QJsonObject &msg, bool mine)
{
    return QJsonDocument(stored(msg, mine)).toJson(QJsonDocument::Compact) + '\n';
}

qint64 History::syncCutoff(qint64 nowMs, qint64 clearedBefore)
{
    return qMax(nowMs - SyncWindowMs, clearedBefore);
}

qint64 History::cutoffFor(qint64 syncCutoff, qint64 firstSeen)
{
    return qMax(syncCutoff, firstSeen - NewMemberMarginMs);
}

QJsonArray History::inventory(const Store &store, qint64 cutoff, int max)
{
    QList<QPair<qint64, QString>> recent;
    for (auto it = store.cbegin(); it != store.cend(); ++it) {
        const qint64 ts = timestamp(it.value());
        if (ts >= cutoff)
            recent.append({ts, it.key()});
    }
    std::sort(recent.begin(), recent.end(), std::greater<>());
    QJsonArray ids;
    for (int i = 0; i < qMin(int(recent.size()), max); ++i)
        ids.append(recent[i].second);
    return ids;
}

QList<QJsonObject> History::missing(const Store &store, const QJsonArray &theirIds)
{
    QSet<QString> theirs;
    for (const QJsonValue &v : theirIds)
        theirs.insert(v.toString());
    QList<QJsonObject> result;
    for (auto it = store.cbegin(); it != store.cend(); ++it)
        if (!theirs.contains(it.key()))
            result.append(it.value());
    return result;
}

QList<QJsonArray> History::batches(QList<QJsonObject> messages, qint64 cutoff, int maxBytes)
{
    messages.erase(std::remove_if(messages.begin(), messages.end(),
                                  [cutoff](const QJsonObject &m) { return timestamp(m) < cutoff; }),
                   messages.end());
    std::sort(messages.begin(), messages.end(),
              [](const QJsonObject &a, const QJsonObject &b) { return timestamp(a) < timestamp(b); });

    QList<QJsonArray> result;
    QJsonArray batch;
    qsizetype bytes = 0;
    for (QJsonObject m : std::as_const(messages)) {
        m.remove("mine");
        const qsizetype size = QJsonDocument(m).toJson(QJsonDocument::Compact).size();
        if (!batch.isEmpty() && bytes + size > maxBytes) {
            result.append(batch);
            batch = QJsonArray();
            bytes = 0;
        }
        batch.append(m);
        bytes += size;
    }
    if (!batch.isEmpty())
        result.append(batch);
    return result;
}
