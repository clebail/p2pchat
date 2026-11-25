#include "attachment.h"

#include <QBuffer>
#include <QGuiApplication>
#include <QImageReader>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QtTest>

namespace {

// Bruit aléatoire : le pire cas pour la compression JPEG
QImage noise(int w, int h)
{
    QImage img(w, h, QImage::Format_RGB32);
    auto *rng = QRandomGenerator::global();
    for (int y = 0; y < h; ++y) {
        auto *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < w; ++x)
            line[x] = rng->generate() | 0xff000000;
    }
    return img;
}

QByteArray encoded(const QImage &img, const char *format)
{
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    img.save(&buffer, format);
    return out;
}

} // namespace

class TestAttachment : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

private slots:
    void encodeFitsInBudget()
    {
        const QByteArray jpeg = Attachment::encode(noise(3000, 2000));
        QVERIFY(!jpeg.isEmpty());
        QVERIFY(jpeg.size() <= Attachment::MaxBytes);
        const QImage back = Attachment::decode(jpeg);
        QVERIFY(!back.isNull());
        QVERIFY(qMax(back.width(), back.height()) <= Attachment::MaxDimension);
        QVERIFY(qAbs(double(back.width()) / back.height() - 1.5) < 0.02); // proportions gardées
        QVERIFY(jpeg.startsWith("\xff\xd8")); // JPEG
    }

    void smallImageIsNotEnlarged()
    {
        QImage img(200, 100, QImage::Format_RGB32);
        img.fill(Qt::red);
        const QImage back = Attachment::decode(Attachment::encode(img));
        QCOMPARE(back.size(), QSize(200, 100));
    }

    void transparencyBecomesWhite()
    {
        QImage img(50, 50, QImage::Format_ARGB32);
        img.fill(Qt::transparent);
        const QImage back = Attachment::decode(Attachment::encode(img));
        const QColor c = back.pixelColor(25, 25);
        QVERIFY(c.red() > 245 && c.green() > 245 && c.blue() > 245);
    }

    void encodeRejectsNullImage()
    {
        QVERIFY(Attachment::encode(QImage()).isEmpty());
    }

    void decodeAcceptsOnlySafeImages()
    {
        QImage small(100, 80, QImage::Format_RGB32);
        small.fill(Qt::blue);
        QCOMPARE(Attachment::decode(encoded(small, "PNG")).size(), QSize(100, 80));
        QVERIFY(Attachment::decode(encoded(small, "BMP")).isNull()); // format non autorisé
        QVERIFY(Attachment::decode("pas une image").isNull());
        QVERIFY(Attachment::decode({}).isNull());

        // PNG uni de quelques Ko mais immense une fois décodé : refusé sans être décodé
        QImage huge(2 * Attachment::MaxDimension + 1, 10, QImage::Format_Mono);
        huge.fill(0);
        const QByteArray bomb = encoded(huge, "PNG");
        QVERIFY(bomb.size() < 10000);
        QVERIFY(Attachment::decode(bomb).isNull());
        QVERIFY(Attachment::decode(QByteArray(Attachment::MaxBytes * 2 + 1, 'x')).isNull());
    }

    void loadFromFile()
    {
        QVERIFY(m_dir.isValid());
        const QString path = m_dir.filePath("photo.png");
        QImage img(300, 200, QImage::Format_RGB32);
        img.fill(Qt::green);
        QVERIFY(img.save(path));
        QString error;
        QCOMPARE(Attachment::load(path, &error).size(), QSize(300, 200));
        QVERIFY(error.isEmpty());

        QVERIFY(Attachment::load(m_dir.filePath("absent.png"), &error).isNull());
        QVERIFY(!error.isEmpty());

        const QString text = m_dir.filePath("faux.jpg");
        QFile f(text);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("ceci n'est pas une image");
        f.close();
        error.clear();
        QVERIFY(Attachment::load(text, &error).isNull());
        QVERIFY(!error.isEmpty());
    }

    void thumbnail()
    {
        QImage big(2000, 1000, QImage::Format_RGB32);
        QCOMPARE(Attachment::thumbnail(big).size(), QSize(Attachment::ThumbnailSide, Attachment::ThumbnailSide / 2));
        QImage small(100, 50, QImage::Format_RGB32);
        QCOMPARE(Attachment::thumbnail(small).size(), QSize(100, 50));
        QVERIFY(Attachment::thumbnail(QImage()).isNull());
    }

    void imageFileExtensions()
    {
        QVERIFY(Attachment::isImageFile("/tmp/IMG_0001.JPG"));
        QVERIFY(Attachment::isImageFile("capture.png"));
        QVERIFY(!Attachment::isImageFile("notes.txt"));
        QVERIFY(!Attachment::isImageFile("sans-extension"));
    }
};

int main(int argc, char *argv[])
{
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    TestAttachment test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_attachment.moc"
