#include "person.h"

#include "crypto.h"
#include "protocol.h"

namespace {

QByteArray signingBytes(const QJsonObject &c)
{
    return Crypto::pack({"p2pchat-device-v1", Protocol::fromB64(c.value("p")), c.value("o").toString().toUtf8(),
                         Protocol::fromB64(c.value("k")), QByteArray::number(qint64(c.value("ts").toDouble()))});
}

QByteArray revocationBytes(const QJsonObject &r)
{
    return Crypto::pack({"p2pchat-unlink-v1", Protocol::fromB64(r.value("p")), r.value("o").toString().toUtf8(),
                         QByteArray::number(qint64(r.value("ts").toDouble()))});
}

} // namespace

QJsonObject Person::createCertificate(const QByteArray &personSeed, const QByteArray &deviceSeed,
                                      const QString &deviceOnion, qint64 timestampMs)
{
    QJsonObject c{{"t", "device"},
                  {"p", Protocol::toB64(Ed25519::publicKey(personSeed))},
                  {"o", deviceOnion},
                  {"k", Protocol::toB64(Ed25519::publicKey(deviceSeed))},
                  {"ts", double(timestampMs)}};
    const QByteArray bytes = signingBytes(c);
    c.insert("ps", Protocol::toB64(Ed25519::sign(personSeed, bytes)));
    c.insert("ds", Protocol::toB64(Ed25519::sign(deviceSeed, bytes)));
    return c;
}

bool Person::verifyCertificate(const QJsonObject &c, const QByteArray &deviceKey)
{
    const QByteArray person = personKey(c), key = Protocol::fromB64(c.value("k"));
    if (person.size() != 32 || deviceKey.isEmpty() || key != deviceKey || !Protocol::isValidOnion(deviceOnion(c))
        || timestamp(c) <= 0)
        return false;
    const QByteArray bytes = signingBytes(c);
    return Ed25519::verify(person, bytes, Protocol::fromB64(c.value("ps")))
           && Ed25519::verify(key, bytes, Protocol::fromB64(c.value("ds")));
}

QByteArray Person::personKey(const QJsonObject &c)
{
    return Protocol::fromB64(c.value("p"));
}

QString Person::deviceOnion(const QJsonObject &c)
{
    return c.value("o").toString();
}

qint64 Person::timestamp(const QJsonObject &c)
{
    return qint64(c.value("ts").toDouble());
}

QJsonObject Person::createRevocation(const QByteArray &personSeed, const QString &deviceOnion, qint64 timestampMs)
{
    QJsonObject r{{"t", "unlink"},
                  {"p", Protocol::toB64(Ed25519::publicKey(personSeed))},
                  {"o", deviceOnion},
                  {"ts", double(timestampMs)}};
    r.insert("ps", Protocol::toB64(Ed25519::sign(personSeed, revocationBytes(r))));
    return r;
}

bool Person::verifyRevocation(const QJsonObject &r)
{
    const QByteArray person = personKey(r);
    return r.value("t").toString() == "unlink" && person.size() == 32 && Protocol::isValidOnion(deviceOnion(r))
           && timestamp(r) > 0 && Ed25519::verify(person, revocationBytes(r), Protocol::fromB64(r.value("ps")));
}
