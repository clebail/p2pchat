#include "singleinstance.h"

#include <QCoreApplication>
#include <QFile>
#include <QLocalServer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class TestSingleInstance : public QObject
{
    Q_OBJECT

private slots:
    void secondInstanceActivatesFirst()
    {
        QTemporaryDir dir;
        SingleInstance first(dir.path());
        QVERIFY(first.acquire());
        QSignalSpy shown(&first, &SingleInstance::activationRequested);

        SingleInstance second(dir.path());
        QVERIFY(!second.acquire()); // déjà ouverte
        QVERIFY(second.activateRunning());
        QTRY_COMPARE(shown.count(), 1); // la première se montre
        QVERIFY(second.activateRunning());
        QTRY_COMPARE(shown.count(), 2);
    }

    void releasedWhenFirstQuits()
    {
        QTemporaryDir dir;
        {
            SingleInstance first(dir.path());
            QVERIFY(first.acquire());
        }
        SingleInstance next(dir.path());
        QVERIFY(next.acquire());
        SingleInstance other(dir.path());
        QVERIFY(!other.acquire());
    }

    void otherProfileIsIndependent()
    {
        QTemporaryDir a, b;
        SingleInstance first(a.path()), second(b.path());
        QVERIFY(first.serverName() != second.serverName());
        QVERIFY(first.acquire());
        QVERIFY(second.acquire()); // autre utilisateur ou autre dossier de données
        QSignalSpy shown(&first, &SingleInstance::activationRequested);
        QVERIFY(second.activateRunning()); // se joint lui-même, pas l'autre
        QTest::qWait(100);
        QCOMPARE(shown.count(), 0);
    }

    void staleLockAfterCrashIsTaken()
    {
        QTemporaryDir dir;
        // Verrou laissé par un processus qui n'existe plus (plantage)
        QFile lock(dir.filePath("instance.lock"));
        QVERIFY(lock.open(QIODevice::WriteOnly));
        lock.write("999999\np2pchat\n" + QSysInfo::machineHostName().toUtf8() + "\n");
        lock.close();
        // …et socket abandonné : lancer quand même
        SingleInstance crashed(dir.path());
        QLocalServer leftover;
        QVERIFY(leftover.listen(crashed.serverName()));

        SingleInstance next(dir.path());
        QVERIFY(next.acquire());
        QSignalSpy shown(&next, &SingleInstance::activationRequested);
        SingleInstance later(dir.path());
        QVERIFY(!later.acquire());
        QVERIFY(later.activateRunning());
        QTRY_COMPARE(shown.count(), 1);
    }

    void nothingToActivate()
    {
        QTemporaryDir dir;
        SingleInstance alone(dir.path());
        QVERIFY(!alone.activateRunning(200)); // personne n'écoute
    }
};

QTEST_GUILESS_MAIN(TestSingleInstance)
#include "tst_singleinstance.moc"
