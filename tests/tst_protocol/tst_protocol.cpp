#include "crypto.h"
#include "protocol.h"

#include <QtTest>

using namespace Protocol;

class TestProtocol : public QObject
{
    Q_OBJECT

private slots:
    void onionValidation_data()
    {
        QTest::addColumn<QString>("onion");
        QTest::addColumn<bool>("valid");
        QTest::newRow("v3") << QString(56, 'a') + ".onion" << true;
        QTest::newRow("chiffres base32") << QString(28, '2') + QString(28, '7') + ".onion" << true;
        QTest::newRow("v2 (16 caractères)") << QString(16, 'a') + ".onion" << false;
        QTest::newRow("57 caractères") << QString(57, 'a') + ".onion" << false;
        QTest::newRow("majuscules") << QString(56, 'A') + ".onion" << false;
        QTest::newRow("chiffre hors base32") << QString(55, 'a') + "1.onion" << false;
        QTest::newRow("sans suffixe") << QString(56, 'a') << false;
        QTest::newRow("suffixe en trop") << QString(56, 'a') + ".onion.com" << false;
        QTest::newRow("vide") << QString() << false;
    }

    void onionValidation()
    {
        QFETCH(QString, onion);
        QFETCH(bool, valid);
        QCOMPARE(isValidOnion(onion), valid);
    }

    void base64()
    {
        const QByteArray raw("\xfb\xff\x00\x01", 4);
        const QString enc = toB64(raw);
        QVERIFY(!enc.contains('+') && !enc.contains('/') && !enc.contains('='));
        QCOMPARE(fromB64(enc), raw);
        QCOMPARE(fromB64(QJsonValue(enc)), raw);
        QCOMPARE(fromB64(QJsonValue(42)), QByteArray()); // pas une chaîne
    }

    void messageIsSignedAndVerifiable()
    {
        const QByteArray seed = Ed25519::generateSeed();
        const QString onion = QString(56, 'b') + ".onion";
        const QJsonObject m = Message::create(seed, onion, "Bob", "Bonjour", 1700000000123);
        QCOMPARE(m.value("t").toString(), QString("msg"));
        QCOMPARE(m.value("a").toString(), onion);
        QCOMPARE(m.value("n").toString(), QString("Bob"));
        QCOMPARE(m.value("x").toString(), QString("Bonjour"));
        QCOMPARE(Message::timestamp(m), qint64(1700000000123));
        QCOMPARE(Message::id(m).size(), 24);
        QVERIFY(Message::verify(m, Ed25519::publicKey(seed)));
        QVERIFY(!Message::verify(m, Ed25519::publicKey(Ed25519::generateSeed())));

        // Les identifiants sont aléatoires
        QVERIFY(Message::id(Message::create(seed, onion, "Bob", "Bonjour", 1)) != Message::id(m));
    }

    void tamperingIsDetected_data()
    {
        QTest::addColumn<QString>("field");
        QTest::addColumn<QJsonValue>("value");
        QTest::newRow("identifiant") << "id" << QJsonValue("autre");
        QTest::newRow("auteur") << "a" << QJsonValue(QString(56, 'c') + ".onion");
        QTest::newRow("pseudo") << "n" << QJsonValue("Mallory");
        QTest::newRow("texte") << "x" << QJsonValue("Bonsoir");
        QTest::newRow("heure") << "ts" << QJsonValue(1700000000124.0);
        QTest::newRow("signature") << "s" << QJsonValue(toB64(QByteArray(64, 'x')));
    }

    void tamperingIsDetected()
    {
        QFETCH(QString, field);
        QFETCH(QJsonValue, value);
        const QByteArray seed = Ed25519::generateSeed();
        QJsonObject m = Message::create(seed, QString(56, 'b') + ".onion", "Bob", "Bonjour", 1700000000123);
        m.insert(field, value);
        QVERIFY(!Message::verify(m, Ed25519::publicKey(seed)));
    }

    void unsignedMessageIsRejected()
    {
        const QByteArray seed = Ed25519::generateSeed();
        QJsonObject m = Message::create(seed, QString(56, 'b') + ".onion", "Bob", "x", 1);
        m.remove("s");
        QVERIFY(!Message::verify(m, Ed25519::publicKey(seed)));
    }

    void extraFieldsAreNotSigned()
    {
        // "t" et "mine" sont ajoutés ou retirés en route (historique local) : la signature tient
        const QByteArray seed = Ed25519::generateSeed();
        QJsonObject m = Message::create(seed, QString(56, 'b') + ".onion", "Bob", "x", 1);
        m.remove("t");
        m.insert("mine", true);
        QVERIFY(Message::verify(m, Ed25519::publicKey(seed)));
    }
};

QTEST_GUILESS_MAIN(TestProtocol)
#include "tst_protocol.moc"
