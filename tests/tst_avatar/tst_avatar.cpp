#include "avatar.h"
#include "crypto.h"
#include "protocol.h"

#include <QBuffer>
#include <QImageWriter>
#include <QtTest>

namespace {
QString onion(char c)
{
    return QString(56, c) + ".onion";
}

QByteArray encode(const QImage &image, const char *format)
{
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter(&buffer, format).write(image);
    return out;
}
} // namespace

class TestAvatar : public QObject
{
    Q_OBJECT

private:
    const QByteArray m_alice = Ed25519::generateSeed(), m_bob = Ed25519::generateSeed();

private slots:
    void chosenImageIsSquaredAndSmall()
    {
        // Rectangle : seul le carré central est gardé (bandes bleues sur les côtés, rouge au centre)
        QImage source(600, 200, QImage::Format_RGB32);
        source.fill(Qt::blue);
        for (int x = 200; x < 400; ++x)
            for (int y = 0; y < 200; ++y)
                source.setPixel(x, y, qRgb(255, 0, 0));
        const QByteArray jpeg = Avatar::fromImage(source);
        QVERIFY(!jpeg.isEmpty());
        QVERIFY(jpeg.size() <= Avatar::MaxBytes);
        const QImage back = Avatar::decode(jpeg);
        QCOMPARE(back.size(), QSize(Avatar::Side, Avatar::Side));
        const QColor corner = back.pixelColor(2, 2);
        QVERIFY2(corner.red() > 200 && corner.blue() < 60, qPrintable(corner.name()));

        // Grande photo bruitée : réduite sous la limite
        QImage noise(2000, 2000, QImage::Format_RGB32);
        for (int y = 0; y < noise.height(); ++y)
            for (int x = 0; x < noise.width(); ++x)
                noise.setPixel(x, y, qRgb((x * 37 + y * 11) % 256, (x * y) % 256, (x ^ y) % 256));
        QVERIFY(Avatar::fromImage(noise).size() <= Avatar::MaxBytes);

        QVERIFY(Avatar::fromImage(QImage()).isEmpty());
    }

    void receivedImageIsChecked()
    {
        QImage small(Avatar::Side, Avatar::Side, QImage::Format_RGB32);
        small.fill(Qt::green);
        QVERIFY(!Avatar::decode(encode(small, "jpeg")).isNull());
        QVERIFY(Avatar::decode(encode(small, "png")).isNull()); // JPEG seulement
        QImage big(4 * Avatar::Side, 4 * Avatar::Side, QImage::Format_RGB32);
        big.fill(Qt::green);
        QVERIFY(Avatar::decode(encode(big, "jpeg")).isNull()); // plus grand que ce que nous envoyons
        QVERIFY(Avatar::decode("pas une image").isNull());
        QVERIFY(Avatar::decode({}).isNull());
        QVERIFY(Avatar::decode(QByteArray(Avatar::MaxBytes + 1, 'x')).isNull());
    }

    void signedByItsAuthor()
    {
        QImage img(Avatar::Side, Avatar::Side, QImage::Format_RGB32);
        img.fill(Qt::yellow);
        const QByteArray jpeg = Avatar::fromImage(img);
        const QJsonObject r = Avatar::create(m_alice, onion('a'), jpeg, 1000);
        QCOMPARE(r.value("t").toString(), QString("avatar"));
        QCOMPARE(Avatar::author(r), onion('a'));
        QCOMPARE(Avatar::timestamp(r), qint64(1000));
        QCOMPARE(Avatar::image(r), jpeg);
        QVERIFY(Avatar::verify(r, Ed25519::publicKey(m_alice)));
        QVERIFY(!Avatar::verify(r, Ed25519::publicKey(m_bob)));
        QVERIFY(!Avatar::verify(r, {}));

        // Toute modification casse la signature
        for (const char *field : {"a", "ts", "img"}) {
            QJsonObject changed = r;
            if (QByteArray(field) == "a")
                changed["a"] = onion('b');
            else if (QByteArray(field) == "ts")
                changed["ts"] = 2000.0;
            else
                changed["img"] = Protocol::toB64(Avatar::fromImage(QImage(10, 10, QImage::Format_RGB32)));
            QVERIFY2(!Avatar::verify(changed, Ed25519::publicKey(m_alice)), field);
        }

        // Avatar retiré : image vide, toujours signé
        const QJsonObject removed = Avatar::create(m_alice, onion('a'), {}, 3000);
        QVERIFY(Avatar::image(removed).isEmpty());
        QVERIFY(Avatar::verify(removed, Ed25519::publicKey(m_alice)));

        // Image illisible, même signée : refusée
        QVERIFY(!Avatar::verify(Avatar::create(m_alice, onion('a'), "n'importe quoi", 4000),
                                Ed25519::publicKey(m_alice)));
        // Sans date, ou adresse invalide
        QVERIFY(!Avatar::verify(Avatar::create(m_alice, onion('a'), jpeg, 0), Ed25519::publicKey(m_alice)));
        QVERIFY(!Avatar::verify(Avatar::create(m_alice, "pas-une-adresse", jpeg, 1000), Ed25519::publicKey(m_alice)));
    }

    void conversationAvatarIsBoundToItsGroup()
    {
        QImage img(Avatar::Side, Avatar::Side, QImage::Format_RGB32);
        img.fill(Qt::cyan);
        const QByteArray jpeg = Avatar::fromImage(img);
        const QString group(64, 'g'), other(64, 'h');
        const QJsonObject r = Avatar::createForConversation(m_alice, onion('a'), group, jpeg, 1000);
        QCOMPARE(r.value("t").toString(), QString("conv-avatar"));
        QCOMPARE(Avatar::image(r), jpeg);
        QVERIFY(Avatar::verifyForConversation(r, Ed25519::publicKey(m_alice), group));
        QVERIFY(!Avatar::verifyForConversation(r, Ed25519::publicKey(m_alice), other)); // autre groupe
        QVERIFY(!Avatar::verifyForConversation(r, Ed25519::publicKey(m_alice), {}));
        QVERIFY(!Avatar::verifyForConversation(r, Ed25519::publicKey(m_bob), group));
        // Pas interchangeable avec l'avatar d'un membre, dans un sens comme dans l'autre
        QVERIFY(!Avatar::verify(r, Ed25519::publicKey(m_alice)));
        QVERIFY(!Avatar::verifyForConversation(Avatar::create(m_alice, onion('a'), jpeg, 1000),
                                               Ed25519::publicKey(m_alice), group));
        QVERIFY(Avatar::verifyForConversation(Avatar::createForConversation(m_alice, onion('a'), group, {}, 2000),
                                              Ed25519::publicKey(m_alice), group)); // retiré
    }
};

QTEST_GUILESS_MAIN(TestAvatar)
#include "tst_avatar.moc"
