#include "torservice.h"

#include <QElapsedTimer>
#include <QFile>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

// TorService avec un faux tor (script shell écrit par le test) : il lit son torrc (-f), écrit l'adresse onion
// dans HiddenServiceDir et imite la progression. Rien ne part sur le réseau.
class TestTorService : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_home;
    QString m_fakeTor;
    const QString m_onion = QString(56, 'a') + ".onion";

    void setMode(const QByteArray &mode) { qputenv("FAKE_TOR_MODE", mode); }

    // Lance le service et attend son issue (prêt ou échec)
    struct Run
    {
        std::unique_ptr<QSignalSpy> ready, failed, progress;
    };
    Run start(TorService &tor)
    {
        Run r;
        r.ready = std::make_unique<QSignalSpy>(&tor, &TorService::ready);
        r.failed = std::make_unique<QSignalSpy>(&tor, &TorService::failed);
        r.progress = std::make_unique<QSignalSpy>(&tor, &TorService::progress);
        tor.start();
        return r;
    }

private slots:
    void initTestCase()
    {
#ifdef Q_OS_WIN
        QSKIP("faux tor en script shell : Linux et macOS");
#endif
        QVERIFY(m_home.isValid());
        // Réglages isolés : le chemin de tor forcé (torPath) désigne le faux
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_home.filePath("config"));
        QCoreApplication::setOrganizationName("p2pchat-tests");
        QCoreApplication::setApplicationName("tst_torservice");

        m_fakeTor = m_home.filePath("faux tor");
        QFile f(m_fakeTor);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QStringLiteral(
                    "#!/bin/sh\n"
                    "printf '%s\\n' \"$@\" > \"$(dirname \"$0\")/arguments\"\n"
                    "hs=$(sed -n 's/^HiddenServiceDir \"\\(.*\\)\"$/\\1/p' \"$2\")\n"
                    "case \"$FAKE_TOR_MODE\" in\n"
                    "  fail) echo 'Nov 26 12:00:00.000 [warn] Could not bind to 127.0.0.1:9050: Address already in use'; exit 1 ;;\n"
                    "esac\n"
                    "echo 'Nov 26 12:00:00.000 [notice] Bootstrapped 0% (starting): Starting'\n"
                    "echo 'Nov 26 12:00:00.000 [notice] Bootstrapped 45% (requesting_descriptors): Asking for relay descriptors'\n"
                    "[ \"$FAKE_TOR_MODE\" = nohost ] || { mkdir -p \"$hs\"; echo %1 > \"$hs/hostname\"; }\n"
                    "echo 'Nov 26 12:00:00.000 [notice] Bootstrapped 100% (done): Done'\n"
                    "echo 'Nov 26 12:00:00.000 [notice] Bootstrapped 100% (done): Done'\n" // répété : ignoré
                    "exec sleep 60\n")
                    .arg(m_onion)
                    .toUtf8());
        f.close();
        QVERIFY(f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QSettings().setValue("torPath", m_fakeTor);
        QCOMPARE(TorService::findExecutable(), m_fakeTor);
    }

    void readyWithOnionAddress()
    {
        setMode("ok");
        const QString dataDir = m_home.filePath("données avec espaces/tor");
        TorService tor(dataDir, 4242);
        Run r = start(tor);
        QTRY_COMPARE(r.ready->count(), 1);
        QCOMPARE(r.ready->at(0).at(0).toString(), m_onion);
        QVERIFY(tor.isReady());
        QCOMPARE(tor.onionAddress(), m_onion);
        QCOMPARE(r.failed->count(), 0);

        // Progression : lancement, puis les étapes de tor (une seule fois 100 %)
        QList<int> percents;
        for (const auto &args : std::as_const(*r.progress))
            percents << args.at(0).toInt();
        QCOMPARE(percents, QList<int>({0, 0, 45, 100}));
        QCOMPARE(r.progress->at(2).at(1).toString(), QString("Asking for relay descriptors"));

        // torrc : ports, service onion, connexions sortantes limitées, chemins entre guillemets (espaces)
        QFile torrc(dataDir + "/torrc");
        QVERIFY(torrc.open(QIODevice::ReadOnly));
        const QString conf = QString::fromUtf8(torrc.readAll());
        QVERIFY(tor.socksPort() > 0);
        QVERIFY(conf.contains(QStringLiteral("SocksPort 127.0.0.1:%1\n").arg(tor.socksPort())));
        QVERIFY(conf.contains(QStringLiteral("HiddenServicePort %1 127.0.0.1:4242\n").arg(TorService::OnionPort)));
        QVERIFY(conf.contains("ReachableAddresses accept *:443, accept *:80, reject *:*\n"));
        QVERIFY(conf.contains(QStringLiteral("DataDirectory \"%1\"\n").arg(QDir(dataDir).absolutePath())));
        QVERIFY(conf.contains(QStringLiteral("__OwningControllerProcess %1\n").arg(QCoreApplication::applicationPid())));
        // Configuration système ignorée : torrc par défaut vide
        QCOMPARE(QFileInfo(dataDir + "/torrc-defaults").size(), qint64(0));
        QFile args(m_home.filePath("arguments"));
        QVERIFY(args.open(QIODevice::ReadOnly));
        // macOS passe les arguments en forme décomposée (NFD, QFile::encodeName) : comparés en forme composée
        QCOMPARE(QString::fromUtf8(args.readAll())
                     .normalized(QString::NormalizationForm_C)
                     .split('\n', Qt::SkipEmptyParts),
                 QStringList({"-f", dataDir + "/torrc", "--defaults-torrc", dataDir + "/torrc-defaults"}));
        // Dossier de données réservé à l'utilisateur
        QCOMPARE(QFileInfo(dataDir).permissions() & (QFile::ReadGroup | QFile::ReadOther), QFile::Permissions());
    }

    void stopsTorWhenDestroyed()
    {
        setMode("ok");
        auto tor = std::make_unique<TorService>(m_home.filePath("arrêt"), 4242);
        Run r = start(*tor);
        QTRY_COMPARE(r.ready->count(), 1);
        QElapsedTimer t;
        t.start();
        tor.reset(); // le faux tor dort 60 s : arrêté, pas attendu
        QVERIFY(t.elapsed() < 3000);
    }

    void exitReportsLastProblem()
    {
        setMode("fail");
        TorService tor(m_home.filePath("échec"), 4242);
        Run r = start(tor);
        QTRY_COMPARE(r.failed->count(), 1);
        QVERIFY2(r.failed->at(0).at(0).toString().contains("Could not bind to 127.0.0.1:9050"),
                 qPrintable(r.failed->at(0).at(0).toString()));
        QVERIFY(!tor.isReady());
        QTest::qWait(100);
        QCOMPARE(r.failed->count(), 1); // une seule fois
        QCOMPARE(r.ready->count(), 0);
    }

    void missingOnionAddress()
    {
        setMode("nohost");
        TorService tor(m_home.filePath("sans adresse"), 4242);
        Run r = start(tor);
        QTRY_COMPARE(r.failed->count(), 1);
        QCOMPARE(r.failed->at(0).at(0).toString(), QString("adresse onion introuvable"));
        QCOMPARE(r.ready->count(), 0);
        QVERIFY(!tor.isReady());
    }
};

QTEST_GUILESS_MAIN(TestTorService)
#include "tst_torservice.moc"
