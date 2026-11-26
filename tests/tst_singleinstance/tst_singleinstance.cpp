#include "singleinstance.h"

#include <QCoreApplication>
#include <QFile>
#include <QLocalServer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <chrono>
#include <future>

namespace {
// Une instance lancée ensuite, comme un second processus : son appel bloquant part d'un autre thread pendant que
// celui-ci traite ses événements. Sous Windows, l'écriture sur le tube nommé n'est confirmée qu'une fois lue :
// depuis le thread du serveur, elle attendrait jusqu'au délai.
bool activateFromOtherProcess(const QString &dataDir, int timeoutMs = 2000)
{
    auto result = std::async(std::launch::async, [dataDir, timeoutMs] {
        SingleInstance other(dataDir);
        return other.activateRunning(timeoutMs);
    });
    while (result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return result.get();
}
} // namespace

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
        QVERIFY(activateFromOtherProcess(dir.path()));
        QTRY_COMPARE(shown.count(), 1); // la première se montre
        QVERIFY(activateFromOtherProcess(dir.path()));
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
        QVERIFY(activateFromOtherProcess(b.path())); // joint « second », pas « first »
        QTest::qWait(100);
        QCOMPARE(shown.count(), 0);
    }

    void staleLockAfterCrashIsTaken()
    {
        QTemporaryDir dir;
        // Verrou laissé par un processus qui n'existe plus (plantage). Nom de machine vide : QLockFile le
        // tient pour local. Sous Windows, il compare à %COMPUTERNAME% (majuscules, 15 caractères au plus),
        // pas à QSysInfo::machineHostName() : avec ce dernier, le verrou ne serait jamais repris.
        QFile lock(dir.filePath("instance.lock"));
        QVERIFY(lock.open(QIODevice::WriteOnly));
        lock.write("999999\np2pchat\n\n");
        lock.close();
#ifndef Q_OS_WIN
        // …et socket abandonné (fichier, sous Unix) : lancer quand même. Sous Windows, le tube nommé d'un
        // processus planté disparaît avec lui, et un serveur gardé ouvert ici capterait l'appel de « later »
        SingleInstance crashed(dir.path());
        QLocalServer leftover;
        QVERIFY(leftover.listen(crashed.serverName()));
#endif

        SingleInstance next(dir.path());
        QVERIFY(next.acquire());
        QSignalSpy shown(&next, &SingleInstance::activationRequested);
        SingleInstance later(dir.path());
        QVERIFY(!later.acquire());
        QVERIFY(activateFromOtherProcess(dir.path()));
        QTRY_COMPARE(shown.count(), 1);
    }

    void nothingToActivate()
    {
        QTemporaryDir dir;
        SingleInstance alone(dir.path());
        QVERIFY(!activateFromOtherProcess(dir.path(), 200)); // personne n'écoute
    }
};

QTEST_GUILESS_MAIN(TestSingleInstance)
#include "tst_singleinstance.moc"
