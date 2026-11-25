#include "crypto.h"

#include <QtTest>

class TestCrypto : public QObject
{
    Q_OBJECT

private:
    // Deux canaux reliés, comme après une poignée de main réussie
    static void pair(SecureChannel &a, SecureChannel &b, const QByteArray &pskA = "secret-du-groupe",
                     const QByteArray &pskB = "secret-du-groupe")
    {
        KeyExchange ka, kb;
        QVERIFY(a.init(ka.sharedSecret(kb.publicKey()), pskA, ka.publicKey(), kb.publicKey()));
        QVERIFY(b.init(kb.sharedSecret(ka.publicKey()), pskB, kb.publicKey(), ka.publicKey()));
    }

private slots:
    void randomBytes()
    {
        const QByteArray a = Crypto::randomBytes(32), b = Crypto::randomBytes(32);
        QCOMPARE(a.size(), 32);
        QVERIFY(a != b);
        QCOMPARE(Crypto::randomBytes(0).size(), 0);
    }

    void packIsLengthPrefixed()
    {
        QCOMPARE(Crypto::pack({}), QByteArray());
        QCOMPARE(Crypto::pack({"ab", ""}), QByteArray("\x00\x00\x00\x02" "ab" "\x00\x00\x00\x00", 10));
        // Sans ambiguïté : déplacer la frontière entre deux parties change l'encodage
        QVERIFY(Crypto::pack({"ab", "c"}) != Crypto::pack({"a", "bc"}));
    }

    void ed25519SignVerify()
    {
        const QByteArray seed = Ed25519::generateSeed();
        const QByteArray pub = Ed25519::publicKey(seed);
        QCOMPARE(seed.size(), 32);
        QCOMPARE(pub.size(), 32);
        QCOMPARE(Ed25519::publicKey(seed), pub); // déterministe

        const QByteArray sig = Ed25519::sign(seed, "bonjour");
        QCOMPARE(sig.size(), 64);
        QVERIFY(Ed25519::verify(pub, "bonjour", sig));
        QVERIFY(!Ed25519::verify(pub, "bonjouR", sig));
        QVERIFY(!Ed25519::verify(Ed25519::publicKey(Ed25519::generateSeed()), "bonjour", sig));

        QByteArray tampered = sig;
        tampered[10] = char(tampered[10] ^ 1);
        QVERIFY(!Ed25519::verify(pub, "bonjour", tampered));
    }

    void ed25519RejectsBadSizes()
    {
        QVERIFY(Ed25519::publicKey("court").isEmpty());
        QVERIFY(Ed25519::sign("court", "x").isEmpty());
        const QByteArray seed = Ed25519::generateSeed();
        const QByteArray sig = Ed25519::sign(seed, "x");
        QVERIFY(!Ed25519::verify(QByteArray(31, 'a'), "x", sig));
        QVERIFY(!Ed25519::verify(Ed25519::publicKey(seed), "x", sig.left(63)));
        QVERIFY(!Ed25519::verify({}, "x", {}));
    }

    void keyExchangeAgrees()
    {
        KeyExchange a, b;
        QCOMPARE(a.publicKey().size(), 32);
        const QByteArray s1 = a.sharedSecret(b.publicKey()), s2 = b.sharedSecret(a.publicKey());
        QCOMPARE(s1.size(), 32);
        QCOMPARE(s1, s2);
        QVERIFY(a.sharedSecret("trop court").isEmpty());
        // Point d'ordre faible : le secret partagé serait nul, il est refusé
        QVERIFY(a.sharedSecret(QByteArray(32, '\0')).isEmpty());
    }

    void channelRoundTrip()
    {
        SecureChannel a, b;
        pair(a, b);
        for (const QByteArray &msg : {QByteArray("salut"), QByteArray(), QByteArray(100000, 'z')}) {
            const QByteArray cipher = a.encrypt(msg);
            QCOMPARE(cipher.size(), msg.size() + 16); // + étiquette Poly1305
            QByteArray plain;
            QVERIFY(b.decrypt(cipher, plain));
            QCOMPARE(plain, msg);
        }
        // Et dans l'autre sens, avec l'autre clé
        QByteArray plain;
        QVERIFY(a.decrypt(b.encrypt("réponse"), plain));
        QCOMPARE(plain, QByteArray("réponse"));
    }

    void channelDirectionsUseDifferentKeys()
    {
        SecureChannel a, b;
        pair(a, b);
        const QByteArray cipher = a.encrypt("x");
        QByteArray plain;
        QVERIFY(!a.decrypt(cipher, plain)); // un message émis ne peut pas nous être renvoyé
    }

    void channelRejectsTamperingReplayAndReorder()
    {
        SecureChannel a, b;
        pair(a, b);
        const QByteArray c1 = a.encrypt("un"), c2 = a.encrypt("deux");
        QByteArray plain;

        QByteArray tampered = c1;
        tampered[0] = char(tampered[0] ^ 1);
        QVERIFY(!b.decrypt(tampered, plain));
        QVERIFY(plain.isEmpty());

        QVERIFY(!b.decrypt(c2, plain)); // désordre
        QVERIFY(b.decrypt(c1, plain));
        QVERIFY(!b.decrypt(c1, plain)); // rejeu
        QVERIFY(b.decrypt(c2, plain));
        QCOMPARE(plain, QByteArray("deux"));
        QVERIFY(!b.decrypt("trop court", plain));
    }

    void channelNeedsSamePsk()
    {
        SecureChannel a, b;
        pair(a, b, "secret A", "secret B");
        QByteArray plain;
        QVERIFY(!b.decrypt(a.encrypt("x"), plain));
    }

    void channelInitRejectsBadInput()
    {
        SecureChannel c;
        const QByteArray k1(32, 'a'), k2(32, 'b');
        QVERIFY(!c.init({}, "psk", k1, k2));
        QVERIFY(!c.init("dh", {}, k1, k2));
        QVERIFY(!c.init("dh", "psk", k1, k1)); // reflet de notre propre clé
        QVERIFY(c.init("dh", "psk", k1, k2));
    }
};

QTEST_GUILESS_MAIN(TestCrypto)
#include "tst_crypto.moc"
