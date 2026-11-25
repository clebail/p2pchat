#include "crypto.h"
#include "protocol.h"
#include "reactions.h"

#include <QtTest>

using namespace Reactions;

namespace {
const QString Alice = QString(56, 'a') + ".onion", Bob = QString(56, 'b') + ".onion";

QJsonObject record(const QString &author, const QString &emoji, bool on, qint64 ts, const QString &id = {})
{
    QJsonObject r{{"id", id.isEmpty() ? QString::number(ts) : id}, {"a", author}, {"m", "msg"},
                  {"re", emoji}, {"on", on}, {"ts", double(ts)}};
    return r;
}
} // namespace

class TestReactions : public QObject
{
    Q_OBJECT

private slots:
    void signedRecord()
    {
        const QByteArray seed = Ed25519::generateSeed();
        const QByteArray key = Ed25519::publicKey(seed);
        const QJsonObject r = create(seed, Alice, "msg", QStringLiteral("👍"), true, 1000);
        QVERIFY(isReaction(r));
        QCOMPARE(target(r), QString("msg"));
        QVERIFY(verify(r, key));
        QVERIFY(!verify(r, Ed25519::publicKey(Ed25519::generateSeed())));
        QVERIFY(!isReaction(QJsonObject{{"x", "un message"}}));

        // Rien ne peut être changé : message visé, émoji, ajout/retrait, heure, auteur
        const QList<QPair<QString, QJsonValue>> changes{{"m", "autre"}, {"re", QStringLiteral("❤️")},
                                                        {"on", false}, {"ts", 1001.0}, {"a", Bob}};
        for (const auto &c : changes) {
            QJsonObject t = r;
            t[c.first] = c.second;
            QVERIFY2(!verify(t, key), qPrintable(c.first));
        }
    }

    void onlyAllowedEmojis()
    {
        const QByteArray seed = Ed25519::generateSeed();
        // Signée correctement, mais texte arbitraire : refusée
        const QJsonObject r = create(seed, Alice, "msg", QStringLiteral("ACHETEZ"), true, 1);
        QVERIFY(!verify(r, Ed25519::publicKey(seed)));
        QJsonObject noTarget = create(seed, Alice, QString(), QStringLiteral("👍"), true, 1);
        QVERIFY(!verify(noTarget, Ed25519::publicKey(seed)));
        QCOMPARE(Allowed.size(), 6);
    }

    void latestRecordWins()
    {
        State s;
        QVERIFY(s.apply(record(Alice, QStringLiteral("👍"), true, 10), false));
        QVERIFY(!s.apply(record(Alice, QStringLiteral("👍"), true, 20), false)); // déjà là : rien ne change
        QVERIFY(s.apply(record(Alice, QStringLiteral("👍"), false, 30), false)); // retirée
        QVERIFY(s.summary("msg").isEmpty());
        // Arrivé en retard (synchronisation) : plus ancien que le retrait, ignoré
        QVERIFY(!s.apply(record(Alice, QStringLiteral("👍"), true, 25), false));
        QVERIFY(s.summary("msg").isEmpty());
        // Même heure : départagé par l'identifiant, pareil sur tous les postes
        QVERIFY(s.apply(record(Alice, QStringLiteral("👍"), true, 30, "zz"), false));
        QVERIFY(!s.apply(record(Alice, QStringLiteral("👍"), false, 30, "aa"), false));
        QCOMPARE(s.summary("msg").size(), 1);
    }

    void summary()
    {
        State s;
        s.apply(record(Bob, QStringLiteral("🎉"), true, 1), false);
        s.apply(record(Alice, QStringLiteral("👍"), true, 3), false);
        s.apply(record(Bob, QStringLiteral("👍"), true, 2), false);
        s.apply(record("moi", QStringLiteral("👍"), true, 4), true);
        const QList<Summary> sum = s.summary("msg");
        QCOMPARE(sum.size(), 2);
        QCOMPARE(sum[0].emoji, QStringLiteral("👍")); // ordre de Allowed, pas d'arrivée
        QCOMPARE(sum[0].authors, QStringList({Bob, Alice, QString()})); // la nôtre : sans onion
        QVERIFY(sum[0].mine);
        QCOMPARE(sum[1].emoji, QStringLiteral("🎉"));
        QVERIFY(!sum[1].mine);
        QVERIFY(s.mineOn("msg", QStringLiteral("👍")));
        QVERIFY(!s.mineOn("msg", QStringLiteral("🎉")));
        QVERIFY(s.summary("autre").isEmpty());
        s.clear();
        QVERIFY(s.summary("msg").isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestReactions)
#include "tst_reactions.moc"
