#include "crypto.h"
#include "devicelink.h"
#include "invitation.h"

#include <QtTest>

// Code de liaison d'un poste (cf. devicelink.h) ; l'échange lui-même est testé de bout en bout dans tst_groupnode
class TestDeviceLink : public QObject
{
    Q_OBJECT

private:
    static DeviceLink::Code sample()
    {
        DeviceLink::Code c;
        c.secret = Crypto::randomBytes(32);
        c.onion = QString(56, 'a') + ".onion";
        c.identityKey = Ed25519::publicKey(Ed25519::generateSeed());
        return c;
    }

private slots:
    void roundTrip()
    {
        const DeviceLink::Code c = sample();
        const QString text = c.toCode();
        QVERIFY(text.startsWith("P2PLIEN-"));
        const auto back = DeviceLink::Code::fromCode(text);
        QVERIFY(back);
        QCOMPARE(back->secret, c.secret);
        QCOMPARE(back->onion, c.onion);
        QCOMPARE(back->identityKey, c.identityKey);
        // Coupé par un logiciel de messagerie, entouré de texte
        QString wrapped = "Voici le code : " + text;
        wrapped.insert(30, "\n  ");
        QVERIFY(DeviceLink::Code::fromCode(wrapped + " (10 minutes)"));
    }

    void invalidCodes()
    {
        QVERIFY(!DeviceLink::Code::fromCode(QString()));
        QVERIFY(!DeviceLink::Code::fromCode("P2PLIEN-"));
        QVERIFY(!DeviceLink::Code::fromCode("P2PLIEN-@@@"));
        DeviceLink::Code shortSecret = sample();
        shortSecret.secret.truncate(8);
        QVERIFY(!DeviceLink::Code::fromCode(shortSecret.toCode()));
        DeviceLink::Code badOnion = sample();
        badOnion.onion = "exemple.com";
        QVERIFY(!DeviceLink::Code::fromCode(badOnion.toCode()));
        DeviceLink::Code badKey = sample();
        badKey.identityKey.chop(1);
        QVERIFY(!DeviceLink::Code::fromCode(badKey.toCode()));
    }

    // Ne se confond pas avec un code d'invitation, dans un sens comme dans l'autre
    void notAnInvitation()
    {
        const DeviceLink::Code c = sample();
        const Invitation inv{c.secret, c.onion, "Alice", c.identityKey, {}};
        QVERIFY(!Invitation::fromCode(c.toCode()));
        QVERIFY(!DeviceLink::Code::fromCode(inv.toCode()));
        QVERIFY(DeviceLink::Code::looksLikeCode(c.toCode()));
        QVERIFY(!DeviceLink::Code::looksLikeCode(inv.toCode()));
    }
};

QTEST_GUILESS_MAIN(TestDeviceLink)
#include "tst_devicelink.moc"
