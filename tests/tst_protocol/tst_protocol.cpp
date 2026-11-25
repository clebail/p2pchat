#include "crypto.h"
#include "protocol.h"

#include <QJsonArray>
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

    void missingFeatures()
    {
        QCOMPARE(Protocol::missingFeatures(Protocol::Version), QStringList());
        QCOMPARE(Protocol::missingFeatures(4),
                 QStringList({"les citations", "les images", "les réactions", "les GIF animés", "les fichiers",
                              "les exclusions et le renouvellement du secret"}));
        QCOMPARE(Protocol::missingFeatures(7), QStringList({"les GIF animés", "les fichiers",
                                                            "les exclusions et le renouvellement du secret"}));
        QCOMPARE(Protocol::missingFeatures(10), QStringList({"les exclusions et le renouvellement du secret"}));
        QCOMPARE(Protocol::missingFeatures(9), // la 10 n'apporte rien de visible
                 QStringList({"les exclusions et le renouvellement du secret"}));
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

    void replyIsSigned()
    {
        const QByteArray seed = Ed25519::generateSeed();
        const QByteArray key = Ed25519::publicKey(seed);
        const QString onion = QString(56, 'b') + ".onion";
        const QJsonObject m = Message::create(seed, onion, "Bob", "d'accord", 5, {"id-cite", {}, {}});
        QVERIFY(Message::verify(m, key));
        QCOMPARE(Message::verifiedReplyTo(m, key), QString("id-cite"));
        QVERIFY(Message::verifiedReplyTo(m, Ed25519::publicKey(Ed25519::generateSeed())).isEmpty());

        // Un relais ne peut ni changer la citation ni en ajouter une
        QJsonObject changed = m;
        changed["r"] = "autre";
        QVERIFY(Message::verify(changed, key)); // le message reste authentique…
        QVERIFY(Message::verifiedReplyTo(changed, key).isEmpty()); // …mais la citation est ignorée
        QJsonObject added = Message::create(seed, onion, "Bob", "x", 5);
        added["r"] = "id-cite";
        added["sr"] = m.value("sr"); // signature d'un autre message
        QVERIFY(Message::verifiedReplyTo(added, key).isEmpty());
        // La retirer est possible : le message redevient un message ordinaire
        QJsonObject removed = m;
        removed.remove("r");
        QVERIFY(Message::verify(removed, key));
        QVERIFY(Message::verifiedReplyTo(removed, key).isEmpty());
    }

    void replyIsReadableByPreviousVersions()
    {
        // Les versions précédentes vérifient "s" sur ces six parties, sans regarder "r" ni "sr"
        const QByteArray seed = Ed25519::generateSeed();
        const QJsonObject m = Message::create(seed, "a", "n", "x", 42, {"cite", {}, {}});
        const QByteArray legacy = Crypto::pack({"p2pchat-msg-v1", Message::id(m).toUtf8(), "a", "n", "x", "42"});
        QCOMPARE(Message::signingBytes(m), legacy);
        QVERIFY(Ed25519::verify(Ed25519::publicKey(seed), legacy, fromB64(m.value("s"))));

        const QJsonObject plain{{"id", "i"}, {"a", "a"}, {"n", "n"}, {"x", "x"}, {"ts", 42.0}};
        QVERIFY(!Message::create(seed, "a", "n", "x", 1).contains("sr"));
        QVERIFY(Message::verifiedReplyTo(plain, Ed25519::publicKey(seed)).isEmpty());
        QCOMPARE(Message::create(seed, "a", "n", "x", 1, {QString(100, 'r'), {}, {}}).value("r").toString().size(),
                 Message::MaxIdLength);
    }

    void imageIsSigned()
    {
        const QByteArray seed = Ed25519::generateSeed();
        const QByteArray key = Ed25519::publicKey(seed);
        const QString onion = QString(56, 'b') + ".onion";
        const QByteArray image("\xff\xd8 faux jpeg", 12);
        const QJsonObject m = Message::create(seed, onion, "Bob", QString(), 5, {QString(), image, {}});
        QVERIFY(Message::verify(m, key));
        QCOMPARE(Message::verifiedImage(m, key), image);
        // Sans légende : les versions précédentes voient un texte explicite, nous une légende vide
        QCOMPARE(m.value("x").toString(), Message::ImagePlaceholder);
        QCOMPARE(Message::caption(m, true), QString());
        QCOMPARE(Message::caption(m, false), Message::ImagePlaceholder); // image refusée : on montre le texte
        const QJsonObject captioned = Message::create(seed, onion, "Bob", "Regardez", 5, {QString(), image, {}});
        QCOMPARE(Message::caption(captioned, true), QString("Regardez"));

        // Image remplacée, ou signature reprise d'un autre message : refusée
        QJsonObject swapped = m;
        swapped["img"] = toB64("autre image");
        QVERIFY(Message::verify(swapped, key));
        QVERIFY(Message::verifiedImage(swapped, key).isEmpty());
        QJsonObject stolen = Message::create(seed, onion, "Bob", "x", 6);
        stolen["img"] = m.value("img");
        stolen["si"] = m.value("si");
        QVERIFY(Message::verifiedImage(stolen, key).isEmpty());
        QVERIFY(Message::verifiedImage(m, Ed25519::publicKey(Ed25519::generateSeed())).isEmpty());
        QVERIFY(!Message::create(seed, onion, "Bob", "x", 6).contains("img"));
    }

    void fileIsSigned()
    {
        const QByteArray seed = Ed25519::generateSeed();
        const QByteArray key = Ed25519::publicKey(seed);
        const QString onion = QString(56, 'b') + ".onion";
        const QJsonObject fileInfo{{"n", "rapport.pdf"}, {"z", 10.0}, {"m", "application/pdf"},
                                   {"h", toB64(QByteArray(32, 'h'))}, {"c", QJsonArray{toB64(QByteArray(32, 'c'))}}};
        const QJsonObject m = Message::create(seed, onion, "Bob", QString(), 5, {QString(), {}, fileInfo});
        QVERIFY(Message::verify(m, key)); // lisible par les versions précédentes…
        QVERIFY(m.value("x").toString().startsWith(QStringLiteral("📎 rapport.pdf, 10 octets"))); // …avec ce texte
        QCOMPARE(Message::caption(m, true), QString());
        const auto info = Message::verifiedFile(m, key);
        QVERIFY(info);
        QCOMPARE(info->name, QString("rapport.pdf"));
        QVERIFY(!Message::verifiedFile(m, Ed25519::publicKey(Ed25519::generateSeed())));

        // Rien de la fiche ne peut être changé en route
        for (const char *name : {"n", "z", "m", "h", "c"}) {
            const QString field = QString::fromLatin1(name);
            QJsonObject tampered = m;
            QJsonObject f = fileInfo;
            f[field] = field == "z" ? QJsonValue(11.0)
                       : field == "c" ? QJsonValue(QJsonArray{toB64(QByteArray(32, 'd'))})
                       : field == "h" ? QJsonValue(toB64(QByteArray(32, 'i')))
                                      : QJsonValue("autre");
            tampered["f"] = f;
            QVERIFY2(!Message::verifiedFile(tampered, key), qPrintable(field));
        }
        QJsonObject noSig = m;
        noSig.remove("sf");
        QVERIFY(!Message::verifiedFile(noSig, key));
        QVERIFY(!Message::verifiedFile(Message::create(seed, onion, "Bob", "x", 5), key));
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
