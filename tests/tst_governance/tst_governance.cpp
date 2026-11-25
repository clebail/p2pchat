#include "crypto.h"
#include "governance.h"
#include "protocol.h"

#include <QJsonArray>
#include <QtTest>

using namespace Governance;

namespace {
QString onion(char c)
{
    return QString(56, c) + ".onion";
}
} // namespace

class TestGovernance : public QObject
{
    Q_OBJECT

private:
    const QByteArray m_alice = Ed25519::generateSeed(), m_bob = Ed25519::generateSeed();
    QByteArray key(const QByteArray &seed) const { return Ed25519::publicKey(seed); }

    Record genesis(const QByteArray &secret = "secret 1")
    {
        return *parse(create(m_alice, onion('a'), nullptr, {}, {}, secret, 1000));
    }

private slots:
    void genesisMakesAuthorAdmin()
    {
        const QJsonObject g = create(m_alice, onion('a'), nullptr, {onion('b')}, {onion('c')}, "secret 1", 1000);
        QCOMPARE(checkNext(std::nullopt, g, key(m_alice)), Check::Ok);
        const Record r = *parse(g);
        QCOMPARE(r.seq, 0);
        QVERIFY(r.prev.isEmpty());
        QCOMPARE(r.admins, QStringList({onion('a')})); // seul l'auteur, quoi qu'on demande
        QVERIFY(r.excluded.isEmpty());
        QCOMPARE(r.secretHash, secretHash("secret 1"));
        QVERIFY(r.secretHash != secretHash("secret 2"));
        QCOMPARE(r.hash().size(), 64);
        QCOMPARE(r.hash(), parse(g)->hash()); // stable : c'est l'identifiant du groupe
        // Code d'invitation : le premier enregistrement doit être celui qu'il annonce
        QCOMPARE(checkNext(std::nullopt, g, key(m_alice), r.hash()), Check::Ok);
        QCOMPARE(checkNext(std::nullopt, g, key(m_alice), QString(64, '0')), Check::WrongGroup);
        QCOMPARE(checkNext(std::nullopt, g, key(m_bob)), Check::BadSignature);
    }

    void adminRenewsAndExcludes()
    {
        const Record g = genesis();
        const QJsonObject next = create(m_alice, onion('a'), &g, {onion('a')}, {onion('m')}, "secret 2", 2000);
        QCOMPARE(checkNext(g, next, key(m_alice)), Check::Ok);
        const Record r = *parse(next);
        QCOMPARE(r.seq, 1);
        QCOMPARE(r.prev, g.hash());
        QVERIFY(r.isExcluded(onion('m')));
        QVERIFY(r.isAdmin(onion('a')));
        QCOMPARE(r.secretHash, secretHash("secret 2"));
        // Et le suivant s'enchaîne sur lui
        const QJsonObject third = create(m_alice, onion('a'), &r, {onion('a'), onion('b')}, {}, "secret 3", 3000);
        QCOMPARE(checkNext(r, third, key(m_alice)), Check::Ok);
        QVERIFY(parse(third)->isAdmin(onion('b')));
    }

    void onlyAdminsCanChangeTheGroup()
    {
        const Record g = genesis();
        // Bob, simple membre, tente d'exclure Alice et de prendre la main
        const QJsonObject coup = create(m_bob, onion('b'), &g, {onion('b')}, {onion('a')}, "secret pirate", 2000);
        QCOMPARE(checkNext(g, coup, key(m_bob)), Check::NotAdmin);
        // Et ne peut pas non plus présenter son propre « premier enregistrement » à la place de celui d'Alice
        const QJsonObject fakeGenesis = create(m_bob, onion('b'), nullptr, {}, {}, "x", 1);
        QCOMPARE(checkNext(g, fakeGenesis, key(m_bob)), Check::NotNext);
        QCOMPARE(checkNext(std::nullopt, fakeGenesis, key(m_bob), g.hash()), Check::WrongGroup);
    }

    void chainMustBeContiguous()
    {
        const Record g = genesis();
        const Record r1 = *parse(create(m_alice, onion('a'), &g, {onion('a')}, {}, "s2", 2000));
        const QJsonObject r2 = create(m_alice, onion('a'), &r1, {onion('a')}, {}, "s3", 3000);
        QCOMPARE(checkNext(g, r2, key(m_alice)), Check::NotNext); // saute r1
        QCOMPARE(checkNext(std::nullopt, r1.json, key(m_alice)), Check::NotNext); // pas un premier
        QCOMPARE(checkNext(r1, r1.json, key(m_alice)), Check::NotNext); // rejoué
    }

    void tamperingIsDetected()
    {
        const Record g = genesis();
        const QJsonObject next = create(m_alice, onion('a'), &g, {onion('a')}, {onion('m')}, "secret 2", 2000);
        for (const char *field : {"seq", "prev", "adm", "exc", "sh", "a", "ts"}) {
            QJsonObject t = next;
            const QString f = QString::fromLatin1(field);
            if (f == "seq")
                t[f] = 7;
            else if (f == "prev")
                t[f] = QString(64, 'f');
            else if (f == "adm")
                t[f] = QJsonArray{onion('a'), onion('z')};
            else if (f == "exc")
                t[f] = QJsonArray{};
            else if (f == "sh")
                t[f] = Protocol::toB64(secretHash("autre"));
            else if (f == "a")
                t[f] = onion('b');
            else
                t[f] = 9999.0;
            QVERIFY2(checkNext(g, t, key(m_alice)) != Check::Ok, field);
        }
    }

    void malformedRecords()
    {
        QVERIFY(!parse({}));
        const Record g = genesis();
        QJsonObject t = g.json;
        t["adm"] = QJsonArray{"pas un onion"};
        QVERIFY(!parse(t));
        t = g.json;
        t["adm"] = QJsonArray{};
        QVERIFY(!parse(t)); // il faut au moins un administrateur
        // Un administrateur ne peut pas être exclu en même temps
        const QJsonObject contradictory = create(m_alice, onion('a'), &g, {onion('a')}, {onion('a')}, "s", 1);
        QCOMPARE(checkNext(g, contradictory, key(m_alice)), Check::Malformed);
    }
};

QTEST_GUILESS_MAIN(TestGovernance)
#include "tst_governance.moc"
