#include "crypto.h"
#include "safetynumber.h"

#include <QRegularExpression>
#include <QtTest>

namespace {
const QString OnionA = QString(56, 'a') + ".onion", OnionB = QString(56, 'b') + ".onion";
}

class TestSafetyNumber : public QObject
{
    Q_OBJECT

private slots:
    void sameOnBothSides()
    {
        const QByteArray a = Ed25519::publicKey(Ed25519::generateSeed());
        const QByteArray b = Ed25519::publicKey(Ed25519::generateSeed());
        const QString fromA = SafetyNumber::compute(OnionA, a, OnionB, b);
        const QString fromB = SafetyNumber::compute(OnionB, b, OnionA, a);
        QCOMPARE(fromA, fromB);
        QVERIFY(QRegularExpression("^\\d{5}( \\d{5}){5}$").match(fromA).hasMatch()); // 6 groupes de 5 chiffres
        QCOMPARE(SafetyNumber::compute(OnionA, a, OnionB, b), fromA); // stable
    }

    void anyChangeChangesTheCode()
    {
        const QByteArray a = Ed25519::publicKey(Ed25519::generateSeed());
        const QByteArray b = Ed25519::publicKey(Ed25519::generateSeed());
        const QByteArray mallory = Ed25519::publicKey(Ed25519::generateSeed());
        const QString genuine = SafetyNumber::compute(OnionA, a, OnionB, b);
        // Clé substituée lors de l'invitation : chacun voit un code différent
        QVERIFY(SafetyNumber::compute(OnionA, a, OnionB, mallory) != genuine);
        QVERIFY(SafetyNumber::compute(OnionA, mallory, OnionB, b) != genuine);
        // Même clé annoncée pour une autre adresse
        QVERIFY(SafetyNumber::compute(OnionA, a, QString(56, 'c') + ".onion", b) != genuine);
    }

    void invalidKeys()
    {
        const QByteArray a = Ed25519::publicKey(Ed25519::generateSeed());
        QVERIFY(SafetyNumber::compute(OnionA, a, OnionB, QByteArray()).isEmpty());
        QVERIFY(SafetyNumber::compute(OnionA, "court", OnionB, a).isEmpty());
        QVERIFY(SafetyNumber::compute(OnionA, a, OnionB, a).isEmpty()); // soi-même
    }

    // Entre deux personnes (postes liés) : le même des deux côtés, différent de celui de deux postes
    void betweenPersons()
    {
        const QByteArray a = Ed25519::publicKey(Ed25519::generateSeed()), b = Ed25519::publicKey(Ed25519::generateSeed());
        const QString code = SafetyNumber::forPersons(a, b);
        QCOMPARE(code.size(), 35);
        QCOMPARE(SafetyNumber::forPersons(b, a), code);
        QVERIFY(SafetyNumber::compute(OnionA, a, OnionB, b) != code);
        QVERIFY(SafetyNumber::forPersons(a, a).isEmpty());
        QVERIFY(SafetyNumber::forPersons(a, {}).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestSafetyNumber)
#include "tst_safetynumber.moc"
