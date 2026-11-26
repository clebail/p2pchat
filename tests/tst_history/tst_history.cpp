#include "history.h"

#include <QJsonDocument>
#include <QTimeZone>
#include <QtTest>

namespace {

QJsonObject msg(const QString &id, qint64 ts, const QString &text = "x")
{
    return {{"t", "msg"}, {"id", id}, {"a", "auteur"}, {"n", "Alice"}, {"x", text}, {"ts", double(ts)}, {"s", "sig"}};
}

History::Store storeOf(const QList<QJsonObject> &messages)
{
    History::Store store;
    for (const QJsonObject &m : messages)
        store.insert(m.value("id").toString(), History::stored(m, false));
    return store;
}

QStringList idsOf(const QJsonArray &arr)
{
    QStringList ids;
    for (const QJsonValue &v : arr)
        ids << (v.isString() ? v.toString() : v.toObject().value("id").toString());
    return ids;
}

} // namespace

class TestHistory : public QObject
{
    Q_OBJECT

private slots:
    void storedForm()
    {
        const QJsonObject s = History::stored(msg("a", 1), true);
        QVERIFY(!s.contains("t"));
        QCOMPARE(s.value("mine").toBool(), true);
        QCOMPARE(s.value("id").toString(), QString("a"));

        const QByteArray line = History::toLine(msg("a", 1), false);
        QVERIFY(line.endsWith('\n'));
        QCOMPARE(line.count('\n'), 1);
        QCOMPARE(QJsonDocument::fromJson(line).object(), History::stored(msg("a", 1), false));
    }

    void parseSortsDeduplicatesAndFilters()
    {
        QByteArray file;
        file += History::toLine(msg("b", 300), false);
        file += History::toLine(msg("a", 200), true);
        file += "ligne abîmée\n\n";
        file += History::toLine(msg("b", 300, "doublon"), false);
        file += History::toLine(msg("old", 50), false); // avant l'effacement
        file += History::toLine(msg("c", 100), false);  // sans retour à la ligne final :
        file.chop(1);

        const QList<History::Row> rows = History::parse(file, 100);
        QCOMPARE(rows.size(), 3);
        QCOMPARE(rows[0].msg.value("id").toString(), QString("c"));
        QCOMPARE(rows[1].msg.value("id").toString(), QString("a"));
        QCOMPARE(rows[2].msg.value("id").toString(), QString("b"));
        QCOMPARE(rows[2].msg.value("x").toString(), QString("x")); // le premier exemplaire est gardé
        QCOMPARE(rows[1].mine, true);
        QCOMPARE(rows[1].ts, qint64(200));
        QVERIFY(!rows[0].legacy);
    }

    void parseLegacyFormat()
    {
        const QByteArray file = R"({"from":"Bob","text":"vieux","ts":"2020-01-02T03:04:05Z","mine":true})";
        const QList<History::Row> rows = History::parse(file, 0);
        QCOMPARE(rows.size(), 1);
        QVERIFY(rows[0].legacy);
        QVERIFY(rows[0].mine);
        QCOMPARE(rows[0].msg.value("n").toString(), QString("Bob"));
        QCOMPARE(rows[0].msg.value("x").toString(), QString("vieux"));
        QCOMPARE(rows[0].ts, QDateTime(QDate(2020, 1, 2), QTime(3, 4, 5), QTimeZone::utc()).toMSecsSinceEpoch());
    }

    void cutoffs()
    {
        const qint64 now = 100LL * 24 * 3600 * 1000;
        QCOMPARE(History::syncCutoff(now, 0), now - History::SyncWindowMs);
        QCOMPARE(History::syncCutoff(now, now - 1000), now - 1000); // historique vidé récemment
        QCOMPARE(History::cutoffFor(500, 0), qint64(500));           // membre ancien ou historique partagé
        const qint64 joined = 10 * History::NewMemberMarginMs;
        QCOMPARE(History::cutoffFor(500, joined), joined - History::NewMemberMarginMs); // marge d'horloge
        QCOMPARE(History::cutoffFor(joined, joined), joined);                          // fenêtre de 30 jours
    }

    void inventoryNewestFirstWithinLimits()
    {
        const History::Store store = storeOf({msg("a", 10), msg("b", 30), msg("c", 20), msg("vieux", 5)});
        QCOMPARE(idsOf(History::inventory(store, 10)), QStringList({"b", "c", "a"}));
        QCOMPARE(idsOf(History::inventory(store, 10, 2)), QStringList({"b", "c"}));
        QVERIFY(History::inventory(store, 100).isEmpty());
    }

    void missingMessages()
    {
        const History::Store store = storeOf({msg("a", 1), msg("b", 2), msg("c", 3)});
        const QList<QJsonObject> result = History::missing(store, QJsonArray{"a", "c", "inconnu"});
        QCOMPARE(result.size(), 1);
        QCOMPARE(result.first().value("id").toString(), QString("b"));
        QCOMPARE(History::missing(store, {}).size(), 3);
    }

    void batchesFilterSortAndStrip()
    {
        QList<QJsonObject> messages;
        for (const QJsonObject &m : {msg("c", 30), msg("a", 10), msg("trop-vieux", 5), msg("b", 20)})
            messages << History::stored(m, true);
        const QList<QJsonArray> batches = History::batches(messages, 10);
        QCOMPARE(batches.size(), 1);
        QCOMPARE(idsOf(batches.first()), QStringList({"a", "b", "c"}));
        for (const QJsonValue &v : batches.first())
            QVERIFY(!v.toObject().contains("mine")); // information locale, pas envoyée
        QVERIFY(History::batches({}, 0).isEmpty());
        QVERIFY(History::batches({msg("a", 1)}, 2).isEmpty());
    }

    void batchesRespectSizeLimit()
    {
        QList<QJsonObject> messages;
        for (int i = 0; i < 50; ++i)
            messages << msg(QString::number(i).rightJustified(3, '0'), i, QString(1000, 'x'));
        const int maxBytes = 5000;
        const QList<QJsonArray> batches = History::batches(messages, 0, maxBytes);
        QVERIFY(batches.size() > 1);
        QStringList all;
        for (const QJsonArray &b : batches) {
            QVERIFY(!b.isEmpty());
            qsizetype bytes = 0;
            for (const QJsonValue &v : b)
                bytes += QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact).size();
            QVERIFY(bytes <= maxBytes);
            all << idsOf(b);
        }
        QCOMPARE(all.size(), 50); // rien de perdu, dans l'ordre
        QStringList sorted = all;
        sorted.sort();
        QCOMPARE(all, sorted);
    }

    void oversizedMessageGetsItsOwnBatch()
    {
        const QList<QJsonArray> batches =
            History::batches({msg("petit", 1), msg("énorme", 2, QString(10000, 'x')), msg("petit2", 3)}, 0, 1000);
        QCOMPARE(batches.size(), 3);
        QCOMPARE(idsOf(batches[1]), QStringList({"énorme"}));
    }
};

QTEST_GUILESS_MAIN(TestHistory)
#include "tst_history.moc"
