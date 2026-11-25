#include "attachment.h"

#include <QBuffer>
#include <QGuiApplication>
#include <QFile>
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

// GIF construit octet par octet (Qt sait lire les GIF, pas les écrire) : frames images de 1×1 dans un
// écran de w×h, et un commentaire de commentBytes octets pour grossir le fichier
QByteArray gif(int frames, int commentBytes = 0, quint16 w = 2, quint16 h = 2)
{
    QByteArray g("GIF89a");
    auto le16 = [&](quint16 v) {
        g.append(char(v & 0xff));
        g.append(char(v >> 8));
    };
    le16(w);
    le16(h);
    g.append(char(0x80)).append(char(0)).append(char(0)); // palette globale de 2 couleurs
    g.append("\xff\x00\x00\x00\x00\xff", 6);
    g.append("\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00", 19); // boucle infinie
    if (commentBytes > 0) {
        g.append("\x21\xfe", 2);
        for (int left = commentBytes; left > 0; left -= 255) {
            const int n = qMin(left, 255);
            g.append(char(n)).append(QByteArray(n, 'x'));
        }
        g.append(char(0));
    }
    for (int i = 0; i < frames; ++i) {
        g.append("\x21\xf9\x04\x04\x0a\x00\x00\x00", 8); // 10 centièmes de seconde par image
        g.append(char(0x2c));
        le16(0);
        le16(0);
        le16(1);
        le16(1);
        g.append(char(0));
        g.append("\x02\x02\x44\x01\x00", 5);
    }
    g.append(char(0x3b));
    return g;
}

QString writeFile(const QTemporaryDir &dir, const QString &name, const QByteArray &data)
{
    const QString path = dir.filePath(name);
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write(data);
    return path;
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
        QVERIFY(Attachment::isImageFile("chat.gif"));
        QVERIFY(!Attachment::isImageFile("notes.txt"));
        QVERIFY(!Attachment::isImageFile("logo.svg")); // dessin vectoriel : pas une photo
        QVERIFY(!Attachment::isImageFile("sans-extension"));
        // Seulement les formats que ce Qt sait lire (WebP, HEIC… selon les modules installés)
        QCOMPARE(Attachment::isImageFile("photo.webp"), QImageReader::supportedImageFormats().contains("webp"));
        QVERIFY(Attachment::fileDialogFilter().contains("*.png"));
    }

    void animatedGifIsSentAsIs()
    {
        const QByteArray data = gif(3);
        QVERIFY(Attachment::isAnimated(data));
        QVERIFY(!Attachment::isAnimated(gif(1))); // une seule image : pas animé
        QVERIFY(!Attachment::isAnimated(Attachment::encode(QImage(10, 10, QImage::Format_RGB32))));
        QCOMPARE(Attachment::decode(data).size(), QSize(2, 2)); // première image, pour la vignette

        const Attachment::Prepared p = Attachment::prepareFile(writeFile(m_dir, "anim.gif", data));
        QVERIFY(p.animated);
        QVERIFY(!p.frozen);
        QCOMPARE(p.data, data); // tel quel : l'animation est gardée
        QCOMPARE(p.preview.size(), QSize(2, 2));
    }

    void heavyGifIsFrozen()
    {
        const QByteArray heavy = gif(3, Attachment::MaxBytes + 1000);
        QVERIFY(Attachment::isAnimated(heavy));
        const Attachment::Prepared p = Attachment::prepareFile(writeFile(m_dir, "lourd.gif", heavy));
        QVERIFY(!p.animated);
        QVERIFY(p.frozen); // signalé dans l'aperçu
        QVERIFY(p.data.startsWith("\xff\xd8")); // première image en JPEG
        QVERIFY(p.data.size() <= Attachment::MaxBytes);
        QCOMPARE(p.sourceBytes, qint64(heavy.size()));
    }

    void staticGifBecomesJpeg()
    {
        const Attachment::Prepared p = Attachment::prepareFile(writeFile(m_dir, "fixe.gif", gif(1)));
        QVERIFY(!p.animated && !p.frozen);
        QVERIFY(p.data.startsWith("\xff\xd8"));
    }

    void gifBombsAreRefused()
    {
        // Trop d'images, ou écran immense : refusé à la réception, sans tout décoder
        QVERIFY(Attachment::decode(gif(Attachment::MaxGifFrames + 1)).isNull());
        QVERIFY(Attachment::decode(gif(2, 0, 2 * Attachment::MaxDimension + 1, 10)).isNull());
        QVERIFY(Attachment::decode(gif(2, 0, 65535, 65535)).isNull()); // 17 Go si décodé
        // Image plus grande que l'écran annoncé : c'est la plus grande qui compte
        QByteArray oversized = gif(2);
        const int desc = int(oversized.indexOf("\x2c\x00\x00\x00\x00\x01\x00\x01\x00"));
        QVERIFY(desc > 0);
        oversized[desc + 5] = char(0xff); // largeur de la première image : 65535
        oversized[desc + 6] = char(0xff);
        QVERIFY(Attachment::decode(oversized).isNull());
        // Fichier tronqué ou mal formé
        QVERIFY(Attachment::decode(gif(2).left(40)).isNull());
        QVERIFY(!Attachment::isAnimated(gif(3).left(50)));
        QVERIFY(Attachment::decode("GIF89a").isNull());
        // Et pas envoyé tel quel s'il dépasse la taille maximale d'envoi
        const Attachment::Prepared p =
            Attachment::prepareFile(writeFile(m_dir, "large.gif", gif(2, 0, Attachment::MaxDimension + 1, 10)));
        QVERIFY(!p.animated);
    }

    void pastedImageIsFixed()
    {
        QImage img(20, 20, QImage::Format_RGB32);
        img.fill(Qt::red);
        const Attachment::Prepared p = Attachment::prepareImage(img);
        QVERIFY(!p.animated);
        QCOMPARE(p.preview.size(), QSize(20, 20));
        QVERIFY(!Attachment::prepareImage(QImage()).error.isEmpty());
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
