#include "terminalclient.h"
#include "terminalsession.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <cstring>

// Console partagée de bout en bout, sans fenêtre : un vrai shell dans un pseudo-terminal chez l'hôte, et pour
// chaque « fenêtre de terminal » un faux terminal (P2PCHAT_TERMINAL) qui lance `<ce test> --terminal …`
// avec un scénario de frappes en entrée et garde ce qui s'affiche. Hôte et invité sont reliés directement.
class TestTerminalSession : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    // Scénario de frappes d'une fenêtre (script shell dont la sortie est tapée), puis ce qu'elle a affiché
    void setKeys(const QString &window, const QByteArray &script)
    {
        QFile f(m_dir.filePath(window + ".in"));
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(script);
    }
    QString screen(const QString &window) const
    {
        QFile f(m_dir.filePath(window + ".txt"));
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    }

    struct Pair
    {
        std::unique_ptr<TerminalSession> host, guest;
        std::unique_ptr<QSignalSpy> hostEnded, guestEnded;
    };
    Pair startPair(bool control)
    {
        Pair p;
        p.host = std::make_unique<TerminalSession>(TerminalSession::Host, "sid-1", QString(56, 'b') + ".onion",
                                                   "Bob", control);
        p.guest = std::make_unique<TerminalSession>(TerminalSession::Guest, "sid-1", QString(56, 'a') + ".onion",
                                                    "Alice", control);
        connect(p.host.get(), &TerminalSession::sendToPeer, p.guest.get(), &TerminalSession::handlePeerMessage);
        connect(p.guest.get(), &TerminalSession::sendToPeer, p.host.get(), &TerminalSession::handlePeerMessage);
        p.hostEnded = std::make_unique<QSignalSpy>(p.host.get(), &TerminalSession::finished);
        p.guestEnded = std::make_unique<QSignalSpy>(p.guest.get(), &TerminalSession::finished);
        QString error;
        if (!p.host->start(&error) || !p.guest->start(&error))
            qWarning() << "démarrage impossible :" << error;
        return p;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        const QString terminal = m_dir.filePath("faux-terminal");
        QFile f(terminal);
        QVERIFY(f.open(QIODevice::WriteOnly));
        // Appelé comme un terminal : « -e <programme> --terminal <socket> <titre> <langue> »
        f.write("#!/bin/sh\n"
                "shift\n"
                "case \"$4\" in\n"
                "  *partagée*) w=hote ;;\n"
                "  *) w=invite ;;\n"
                "esac\n"
                "sh \"$FAKE_DIR/$w.in\" | \"$@\" > \"$FAKE_DIR/$w.txt\" 2>&1\n");
        f.close();
        QVERIFY(f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        qputenv("P2PCHAT_TERMINAL", terminal.toUtf8());
        qputenv("FAKE_DIR", m_dir.path().toUtf8());
        qputenv("SHELL", "/bin/sh"); // le même partout, sans configuration de l'utilisateur
    }

    void commandsRunAndAreMirrored()
    {
        // L'hôte tape une commande ; l'invité, sans le contrôle, aussi : la sienne est ignorée
        setKeys("hote", "sleep 1; printf 'echo marqueur-$((40+2))\\n'; sleep 8\n");
        setKeys("invite", "sleep 2; printf 'echo intrus-$((1+1))\\n'; sleep 3; printf 'echo invite-$((6*7))\\n'; sleep 4\n");
        Pair p = startPair(false);
        QTRY_VERIFY_WITH_TIMEOUT(screen("hote").contains("marqueur-42"), 10000); // exécutée par le shell
        QTRY_VERIFY_WITH_TIMEOUT(screen("invite").contains("marqueur-42"), 10000); // et montrée à l'invité
        QTest::qWait(1500);
        QVERIFY2(!screen("hote").contains("intrus"), qPrintable(screen("hote")));

        // Le contrôle donné : ses frappes arrivent dans le shell
        p.host->setControl(true);
        QTRY_VERIFY(p.guest->control());
        QTRY_VERIFY_WITH_TIMEOUT(screen("hote").contains("invite-42"), 10000);
        QTRY_VERIFY(screen("invite").contains("invite-42"));
        QVERIFY(!screen("hote").contains("intrus-2"));

        p.host->end("vous avez arrêté le partage");
        QCOMPARE(p.hostEnded->count(), 1);
        QTRY_COMPARE(p.guestEnded->count(), 1);
        QVERIFY(p.host->isEnded() && p.guest->isEnded());
    }

    void shellExitEndsTheSession()
    {
        setKeys("hote", "sleep 1; printf 'exit\\n'; sleep 5\n");
        setKeys("invite", "sleep 6\n");
        Pair p = startPair(false);
        QTRY_COMPARE_WITH_TIMEOUT(p.hostEnded->count(), 1, 10000);
        QCOMPARE(p.hostEnded->at(0).at(0).toString(), QString("le shell s'est terminé"));
        QTRY_COMPARE(p.guestEnded->count(), 1); // l'invité est prévenu
        QTest::qWait(200);
        QCOMPARE(p.hostEnded->count(), 1); // une seule fois
    }
};

int main(int argc, char *argv[])
{
    // Rôle de fenêtre de console, comme l'application : `--terminal <socket> [titre] [langue]`
    if (argc >= 3 && std::strcmp(argv[1], "--terminal") == 0)
        return runTerminalClient(argv[2], argc >= 4 ? argv[3] : nullptr, argc >= 5 ? argv[4] : nullptr);
    QCoreApplication app(argc, argv);
    TestTerminalSession test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_terminalsession.moc"
