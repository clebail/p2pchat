#include "crypto.h"
#include "person.h"
#include "protocol.h"

#include <QtTest>

namespace {
QString onion(char c)
{
    return QString(56, c) + ".onion";
}
} // namespace

// Certificat de poste (postes liés) : signé par la personne ET par le poste
class TestPerson : public QObject
{
    Q_OBJECT

private:
    const QByteArray m_person = Ed25519::generateSeed(), m_device = Ed25519::generateSeed();
    const QByteArray m_deviceKey = Ed25519::publicKey(m_device);

private slots:
    void validCertificate()
    {
        const QJsonObject c = Person::createCertificate(m_person, m_device, onion('a'), 1000);
        QCOMPARE(c.value("t").toString(), QString("device"));
        QCOMPARE(Person::personKey(c), Ed25519::publicKey(m_person));
        QCOMPARE(Person::deviceOnion(c), onion('a'));
        QCOMPARE(Person::timestamp(c), qint64(1000));
        QVERIFY(Person::verifyCertificate(c, m_deviceKey));
    }

    void anyChangeBreaksIt()
    {
        const QJsonObject c = Person::createCertificate(m_person, m_device, onion('a'), 1000);
        const QByteArray other = Ed25519::publicKey(Ed25519::generateSeed());
        for (const char *field : {"p", "o", "k", "ts", "ps", "ds"}) {
            QJsonObject changed = c;
            const QByteArray f(field);
            if (f == "o")
                changed["o"] = onion('b');
            else if (f == "ts")
                changed["ts"] = 2000.0;
            else if (f == "ps" || f == "ds")
                changed[field] = Protocol::toB64(QByteArray(64, 'x'));
            else
                changed[field] = Protocol::toB64(other);
            QVERIFY2(!Person::verifyCertificate(changed, f == "k" ? other : m_deviceKey), field);
        }
        // Clé de poste connue différente de celle du certificat
        QVERIFY(!Person::verifyCertificate(c, other));
        QVERIFY(!Person::verifyCertificate(c, {}));
        // Sans date, ou adresse invalide
        QVERIFY(!Person::verifyCertificate(Person::createCertificate(m_person, m_device, onion('a'), 0), m_deviceKey));
        QVERIFY(!Person::verifyCertificate(Person::createCertificate(m_person, m_device, "pas-une-adresse", 1000),
                                           m_deviceKey));
    }

    // Quelqu'un réclame le poste d'un autre (dont l'adresse et la clé sont publiques) : sans la clé de ce poste,
    // il ne peut pas produire la seconde signature
    void cannotClaimSomeoneElsesDevice()
    {
        const QByteArray attacker = Ed25519::generateSeed(), attackerDevice = Ed25519::generateSeed();
        // Signé avec son propre poste : la clé portée n'est pas celle de la victime
        QJsonObject forged = Person::createCertificate(attacker, attackerDevice, onion('a'), 5000);
        QVERIFY(!Person::verifyCertificate(forged, m_deviceKey));
        // En y mettant la clé de la victime : la signature du poste ne correspond plus
        forged["k"] = Protocol::toB64(m_deviceKey);
        QVERIFY(!Person::verifyCertificate(forged, m_deviceKey));
    }

    // Révocation (poste délié) : signée par la personne ; toute modification la rend invalide
    void revocation()
    {
        const QJsonObject r = Person::createRevocation(m_person, onion('a'), 3000);
        QCOMPARE(r.value("t").toString(), QString("unlink"));
        QCOMPARE(Person::personKey(r), Ed25519::publicKey(m_person));
        QCOMPARE(Person::deviceOnion(r), onion('a'));
        QCOMPARE(Person::timestamp(r), qint64(3000));
        QVERIFY(Person::verifyRevocation(r));
        for (const char *field : {"t", "p", "o", "ts", "ps"}) {
            QJsonObject changed = r;
            const QByteArray f(field);
            if (f == "t")
                changed["t"] = "device";
            else if (f == "o")
                changed["o"] = onion('b');
            else if (f == "ts")
                changed["ts"] = 4000.0;
            else if (f == "ps")
                changed["ps"] = Protocol::toB64(QByteArray(64, 'x'));
            else
                changed["p"] = Protocol::toB64(Ed25519::publicKey(Ed25519::generateSeed()));
            QVERIFY2(!Person::verifyRevocation(changed), field);
        }
        QVERIFY(!Person::verifyRevocation(Person::createRevocation(m_person, onion('a'), 0)));
        QVERIFY(!Person::verifyRevocation(Person::createRevocation(m_person, "pas-une-adresse", 3000)));
        // Ni un certificat pris pour une révocation
        QVERIFY(!Person::verifyRevocation(Person::createCertificate(m_person, m_device, onion('a'), 3000)));
    }
};

QTEST_GUILESS_MAIN(TestPerson)
#include "tst_person.moc"
