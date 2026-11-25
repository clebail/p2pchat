#include "connectionpolicy.h"

#include <QtTest>

using namespace ConnectionPolicy;

class TestConnectionPolicy : public QObject
{
    Q_OBJECT

private slots:
    void backoffDoublesUpToCeiling()
    {
        Backoff b;
        QVERIFY(b.due(0));
        QList<int> delays;
        for (int i = 0; i < 6; ++i) {
            b.failed(1000, false);
            delays << b.delay;
        }
        QCOMPARE(delays, QList<int>({10, 20, 40, 80, 90, 90}));
        QCOMPARE(b.nextTry, qint64(1000 + 90 * 1000));
        QVERIFY(!b.due(b.nextTry - 1));
        QVERIFY(b.due(b.nextTry));
    }

    void fastRetryKeepsMinimumDelay()
    {
        Backoff b;
        for (int i = 0; i < 4; ++i)
            b.failed(0, true);
        QCOMPARE(b.delay, BackoffMinSec);
        b.failed(0, false); // fin du mode rapide : l'espacement reprend
        QCOMPARE(b.delay, 2 * BackoffMinSec);
    }

    void crossedCallsAgreeOnSameConnection()
    {
        const QString low = QString(56, 'a') + ".onion", high = QString(56, 'b') + ".onion";
        // La connexion ouverte par l'onion le plus petit est gardée, quel que soit l'ordre d'arrivée
        QVERIFY(keepExisting(low, high));
        QVERIFY(!keepExisting(high, low));
        // Même initiateur (reconnexion) : la nouvelle remplace l'ancienne
        QVERIFY(!keepExisting(low, low));
    }

    void relayedOnline()
    {
        const QString me = "moi", a = "a", b = "b", c = "c", d = "d";
        QHash<QString, QSet<QString>> presence;
        presence.insert(a, {me, b, c});
        presence.insert(b, {a, c, d});
        const QHash<QString, QString> via = ConnectionPolicy::relayedOnline(presence, {a, b}, me);
        // b est relié directement, « moi » n'est jamais relayé ; c vu par a et b : relais stable (a)
        QCOMPARE(via.size(), 2);
        QCOMPARE(via.value(c), a);
        QCOMPARE(via.value(d), b);
        QVERIFY(ConnectionPolicy::relayedOnline({}, {a}, me).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestConnectionPolicy)
#include "tst_connectionpolicy.moc"
