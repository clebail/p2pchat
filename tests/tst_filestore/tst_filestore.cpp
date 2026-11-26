#include "crypto.h"
#include "filestore.h"
#include "protocol.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

using namespace Files;

class TestFileStore : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    int m_counter = 0;

    QString source(const QString &name, const QByteArray &content)
    {
        QDir().mkpath(m_dir.filePath("sources"));
        const QString path = m_dir.filePath("sources/" + name);
        QFile f(path);
        if (f.open(QIODevice::WriteOnly))
            f.write(content);
        return path;
    }

    QString newStoreDir() { return m_dir.filePath(QStringLiteral("store%1").arg(++m_counter)); }

    static QByteArray randomBytes(int n) { return Crypto::randomBytes(n); }

    static void age(const QString &path, qint64 ms)
    {
        QFile f(path);
        if (f.open(QIODevice::ReadWrite))
            f.setFileTime(QDateTime::currentDateTime().addMSecs(-ms), QFileDevice::FileModificationTime);
    }

private slots:
    void importComputesFingerprints()
    {
        FileStore store(newStoreDir());
        const QByteArray content = randomBytes(2 * ChunkBytes + 1234);
        QString error;
        const auto info = store.import(source("rapport final.pdf", content), &error);
        QVERIFY2(info, qPrintable(error));
        QCOMPARE(info->name, QString("rapport final.pdf"));
        QCOMPARE(info->size, qint64(content.size()));
        QCOMPARE(info->sha256, QCryptographicHash::hash(content, QCryptographicHash::Sha256));
        QCOMPARE(info->chunkCount(), 3);
        QCOMPARE(info->chunkSize(0), qint64(ChunkBytes));
        QCOMPARE(info->chunkSize(2), qint64(1234));
        QCOMPARE(info->chunkSize(3), qint64(0));
        QCOMPARE(info->chunkHashes.at(2), QCryptographicHash::hash(content.mid(2 * ChunkBytes), QCryptographicHash::Sha256));
        QCOMPARE(info->mime, QString("application/pdf"));

        QVERIFY(store.has(info->id()));
        QCOMPARE(store.complete(), QStringList({info->id()}));
        QFile stored(store.path(info->id()));
        QVERIFY(stored.open(QIODevice::ReadOnly));
        QCOMPARE(stored.readAll(), content);
#ifndef Q_OS_WIN
        // Sous Windows, Qt ne fait que simuler ces droits Unix (seul l'attribut lecture seule compte) : la
        // confidentialité y vient des droits d'accès du dossier de l'utilisateur
        QCOMPARE(stored.permissions() & (QFile::ReadOther | QFile::ReadGroup), QFile::Permissions());
#endif
        QCOMPARE(store.readChunk(info->id(), 1), content.mid(ChunkBytes, ChunkBytes));
        QCOMPARE(store.received(info->id()).count(true), 3);
        QCOMPARE(store.info(info->id())->sha256, info->sha256);

        // Même contenu, autre nom : même fichier dans le magasin
        const auto again = store.import(source("copie.pdf", content));
        QCOMPARE(again->id(), info->id());
        QCOMPARE(store.complete().size(), 1);
    }

    void importLimits()
    {
        FileStore store(newStoreDir());
        QString error;
        QVERIFY(!store.import(m_dir.filePath("absent.txt"), &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!store.import(source("vide.txt", {}), &error));
        QVERIFY(error.contains("vide"));
        // 100 Mo + 1 octet (fichier creux : rien n'est écrit sur le disque)
        const QString big = source("gros.bin", {});
        QFile f(big);
        QVERIFY(f.open(QIODevice::ReadWrite) && f.resize(MaxFileBytes + 1));
        f.close();
        QVERIFY(!store.import(big, &error));
        QVERIFY(error.contains("100 Mo"));
        QVERIFY(store.complete().isEmpty());
        QVERIFY(QDir(store.dir()).entryList(QDir::Files).isEmpty()); // pas de copie abandonnée
    }

    void infoRoundTripAndValidation()
    {
        FileStore store(newStoreDir());
        const FileInfo info = *store.import(source("a.txt", randomBytes(ChunkBytes + 1)));
        const auto back = FileInfo::fromJson(info.toJson());
        QVERIFY(back);
        QCOMPARE(back->id(), info.id());
        QCOMPARE(back->chunkHashes, info.chunkHashes);
        QCOMPARE(back->mime, QString("text/plain"));

        auto tampered = [&](const QString &key, const QJsonValue &value) {
            QJsonObject o = info.toJson();
            o[key] = value;
            return FileInfo::fromJson(o);
        };
        QVERIFY(!tampered("z", 5.0));                            // taille incompatible avec le nombre de morceaux
        QVERIFY(!tampered("z", double(MaxFileBytes + 1)));       // trop gros
        QVERIFY(!tampered("z", 1.5));                            // pas un entier
        QVERIFY(!tampered("z", 0.0));
        QVERIFY(!tampered("h", Protocol::toB64("court")));       // empreinte invalide
        QVERIFY(!tampered("c", QJsonArray{Protocol::toB64("x"), Protocol::toB64("y")}));
        QCOMPARE(tampered("m", "<script>")->mime, QString("application/octet-stream"));
        QCOMPARE(tampered("n", "../../.bashrc")->name, QString("bashrc"));
    }

    void downloadInAnyOrderAndResume()
    {
        FileStore author(newStoreDir());
        const QByteArray content = randomBytes(5 * ChunkBytes + 77);
        const FileInfo info = *author.import(source("video.mp4", content));
        const QString id = info.id();

        const QString dir = newStoreDir();
        {
            FileStore receiver(dir);
            QVERIFY(!receiver.has(id));
            QCOMPARE(receiver.writeChunk(id, 0, author.readChunk(id, 0)), FileStore::ChunkResult::Unknown);
            QVERIFY(receiver.begin(info));
            QCOMPARE(receiver.received(id).count(true), 0);
            for (int i : {4, 1, 5}) // dans le désordre
                QCOMPARE(receiver.writeChunk(id, i, author.readChunk(id, i)), FileStore::ChunkResult::Accepted);
            QCOMPARE(receiver.writeChunk(id, 1, author.readChunk(id, 1)), FileStore::ChunkResult::Duplicate);
            // Un fichier en cours peut déjà fournir les morceaux reçus
            QCOMPARE(receiver.readChunk(id, 4), author.readChunk(id, 4));
            QVERIFY(receiver.readChunk(id, 0).isEmpty());
            QVERIFY(!receiver.has(id));
        }
        // Redémarrage : la reprise repart des morceaux déjà reçus
        FileStore receiver(dir);
        QVERIFY(receiver.begin(info)); // ne repart pas de zéro
        const QBitArray have = receiver.received(id);
        QCOMPARE(have.count(true), 3);
        QVERIFY(have.testBit(4) && have.testBit(1) && have.testBit(5) && !have.testBit(0));
        QCOMPARE(receiver.writeChunk(id, 0, author.readChunk(id, 0)), FileStore::ChunkResult::Accepted);
        QCOMPARE(receiver.writeChunk(id, 2, author.readChunk(id, 2)), FileStore::ChunkResult::Accepted);
        QCOMPARE(receiver.writeChunk(id, 3, author.readChunk(id, 3)), FileStore::ChunkResult::Completed);
        QVERIFY(receiver.has(id));
        QFile done(receiver.path(id));
        QVERIFY(done.open(QIODevice::ReadOnly));
        QCOMPARE(done.readAll(), content);
        QVERIFY(!QFile::exists(receiver.dir() + "/" + id + ".part"));
        QCOMPARE(receiver.writeChunk(id, 0, author.readChunk(id, 0)), FileStore::ChunkResult::Duplicate);
    }

    void badChunksAreRejected()
    {
        FileStore author(newStoreDir());
        const FileInfo info = *author.import(source("doc.odt", randomBytes(2 * ChunkBytes)));
        FileStore receiver(newStoreDir());
        QVERIFY(receiver.begin(info));
        const QString id = info.id();
        QByteArray forged = author.readChunk(id, 0);
        forged[100] = char(forged[100] ^ 1);
        QCOMPARE(receiver.writeChunk(id, 0, forged), FileStore::ChunkResult::Rejected);
        QCOMPARE(receiver.writeChunk(id, 0, author.readChunk(id, 0).left(10)), FileStore::ChunkResult::Rejected);
        QCOMPARE(receiver.writeChunk(id, 0, author.readChunk(id, 1)), FileStore::ChunkResult::Rejected); // mauvais rang
        QCOMPARE(receiver.writeChunk(id, 7, author.readChunk(id, 0)), FileStore::ChunkResult::Rejected);
        QCOMPARE(receiver.writeChunk(id, -1, author.readChunk(id, 0)), FileStore::ChunkResult::Rejected);
        QCOMPARE(receiver.received(id).count(true), 0);
        // Le bon morceau passe toujours
        QCOMPARE(receiver.writeChunk(id, 0, author.readChunk(id, 0)), FileStore::ChunkResult::Accepted);
    }

    void inconsistentAnnouncementIsDropped()
    {
        // Annonce piégée : empreintes de morceaux justes, empreinte du fichier entier fausse
        FileStore author(newStoreDir());
        FileInfo info = *author.import(source("piege.bin", randomBytes(ChunkBytes + 10)));
        const QString realId = info.id();
        info.sha256 = QCryptographicHash::hash("autre chose", QCryptographicHash::Sha256);
        FileStore receiver(newStoreDir());
        QVERIFY(receiver.begin(info));
        QCOMPARE(receiver.writeChunk(info.id(), 0, author.readChunk(realId, 0)), FileStore::ChunkResult::Accepted);
        QCOMPARE(receiver.writeChunk(info.id(), 1, author.readChunk(realId, 1)), FileStore::ChunkResult::Rejected);
        QVERIFY(!receiver.has(info.id()));
        QVERIFY(QDir(receiver.dir()).entryList(QDir::Files).isEmpty()); // tout est effacé
    }

    void sanitizeName_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("expected");
        QTest::newRow("ordinaire") << "Compte rendu.pdf" << "Compte rendu.pdf";
        QTest::newRow("chemin unix") << "../../.bashrc" << "bashrc";
        QTest::newRow("chemin windows") << "C:\\Windows\\system32\\evil.dll" << "evil.dll";
        QTest::newRow("interdits") << "a<b>c:d\"e|f?g*h.txt" << "a_b_c_d_e_f_g_h.txt";
        QTest::newRow("contrôle") << QString("nom\nfaux\t.txt") << "nom_faux_.txt";
        QTest::newRow("points finaux") << "fichier.txt. . ." << "fichier.txt";
        QTest::newRow("réservé") << "CON.txt" << "_CON.txt";
        QTest::newRow("réservé sans extension") << "nul" << "_nul";
        QTest::newRow("vide") << "" << "fichier";
        QTest::newRow("que des points") << "..." << "fichier";
        QTest::newRow("accents") << "Été à la plage.jpg" << "Été à la plage.jpg";
    }

    void sanitizeName()
    {
        QFETCH(QString, name);
        QFETCH(QString, expected);
        QCOMPARE(Files::sanitizeName(name), expected);
    }

    void longNameKeepsExtension()
    {
        const QString n = Files::sanitizeName(QString(300, 'a') + ".tar.gz");
        QVERIFY(n.size() <= 120);
        QVERIFY(n.endsWith(".gz"));
    }

    void riskyFiles()
    {
        for (const char *name : {"setup.exe", "install.SH", "script.ps1", "app.AppImage", "lien.lnk", "x.desktop"})
            QVERIFY2(Files::isRisky(name), name);
        for (const char *name : {"photo.jpg", "doc.pdf", "notes.txt", "archive.zip", "sans-extension"})
            QVERIFY2(!Files::isRisky(name), name);
    }

    void cleanupRemovesOldFiles()
    {
        FileStore store(newStoreDir());
        const FileInfo old = *store.import(source("vieux.txt", randomBytes(100)));
        const FileInfo recent = *store.import(source("recent.txt", randomBytes(100)));
        FileStore author(newStoreDir());
        const FileInfo partial = *author.import(source("partiel.bin", randomBytes(2 * ChunkBytes)));
        QVERIFY(store.begin(partial));
        QCOMPARE(store.writeChunk(partial.id(), 0, author.readChunk(partial.id(), 0)),
                 FileStore::ChunkResult::Accepted);

        const qint64 day = 24LL * 3600 * 1000;
        for (const QString &suffix : {QString(), QString(".json")})
            age(store.dir() + "/" + old.id() + suffix, 31 * day);
        for (const QString &suffix : {QString(".part"), QString(".json")})
            age(store.dir() + "/" + partial.id() + suffix, 40 * day);
        QCOMPARE(store.cleanup(QDateTime::currentMSecsSinceEpoch()), 2);
        QVERIFY(!store.has(old.id()));
        QVERIFY(store.received(partial.id()).isEmpty()); // téléchargement abandonné, effacé aussi
        QVERIFY(store.has(recent.id()));

        store.clear();
        QVERIFY(QDir(store.dir()).entryList(QDir::Files).isEmpty());
    }

    void invalidIdsAreHarmless()
    {
        FileStore store(newStoreDir());
        QVERIFY(!store.has("../etc/passwd"));
        QVERIFY(store.path("..").isEmpty());
        QVERIFY(store.readChunk("../../x", 0).isEmpty());
        store.remove("../sources/a.txt"); // aucun effet hors du magasin
        QCOMPARE(store.writeChunk(QString(64, 'z'), 0, "x"), FileStore::ChunkResult::Unknown);
    }
};

QTEST_GUILESS_MAIN(TestFileStore)
#include "tst_filestore.moc"
