#include "groupnode.h"
#include "invitation.h"
#include "terminalsession.h"
#include "terminalsharing.h"

#include <QNetworkProxy>
#include <QSettings>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

// Négociation de la console partagée entre deux membres réels reliés en local (sans Tor). Aucune fenêtre
// de terminal ni aucun shell n'est lancé : le démarrage est remplacé.
class TestTerminalSharing : public QObject
{
    Q_OBJECT

private:
    struct Peer
    {
        QString onion, nick;
        std::unique_ptr<QSettings> settings;
        std::unique_ptr<GroupStore> store;
        std::unique_ptr<GroupNode> node;
        std::unique_ptr<TerminalSharing> sharing;
        QStringList messages; // systemMessage
        QList<QVariantList> offers;
        int withdrawn = 0;
        bool startOk = true;
    };

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<Peer> m_a, m_b;
    QHash<QString, Peer *> m_byOnion;

    std::unique_ptr<Peer> makePeer(char name)
    {
        auto p = std::make_unique<Peer>();
        p->onion = QString(56, QLatin1Char(name)) + ".onion";
        p->nick = QStringLiteral("Membre %1").arg(QChar(name).toUpper());
        p->settings = std::make_unique<QSettings>(m_dir->filePath(QStringLiteral("%1.ini").arg(name)),
                                                  QSettings::IniFormat);
        p->store = std::make_unique<GroupStore>(*p->settings);
        p->store->ensureIdentity();
        p->store->setNickname(p->nick);
        GroupNode::Timing t;
        t.firstDialDelayMs = 0;
        t.redialDelayMs = 50;
        t.retryIntervalMs = 200;
        p->node = std::make_unique<GroupNode>(*p->store, m_dir->filePath(QStringLiteral("%1.jsonl").arg(name)), t);
        if (!p->node->listen())
            return nullptr;
        p->node->setDialer([this](const QString &onion) {
            auto *s = new QTcpSocket;
            s->setProxy(QNetworkProxy::NoProxy);
            Peer *target = m_byOnion.value(onion);
            s->connectToHost(QHostAddress::LocalHost, target ? target->node->port() : 1);
            return s;
        });
        Peer *pp = p.get();
        p->sharing = std::make_unique<TerminalSharing>(*p->node, [this](const QString &onion) {
            return m_byOnion.contains(onion) ? m_byOnion.value(onion)->nick : QString();
        });
        p->sharing->setStarter([pp](TerminalSession *, QString *error) { // ni fenêtre, ni shell
            if (!pp->startOk)
                *error = "terminal introuvable";
            return pp->startOk;
        });
        connect(p->sharing.get(), &TerminalSharing::systemMessage, this, [pp](const QString &m) { pp->messages << m; });
        connect(p->sharing.get(), &TerminalSharing::offerReceived, this,
                [pp](const QString &onion, const QString &nick, bool control) { pp->offers << QVariantList{onion, nick, control}; });
        connect(p->sharing.get(), &TerminalSharing::offerWithdrawn, this, [pp] { ++pp->withdrawn; });
        m_byOnion.insert(p->onion, p.get());
        return p;
    }

    static bool contains(const QStringList &messages, const QString &part)
    {
        return std::any_of(messages.cbegin(), messages.cend(), [&](const QString &m) { return m.contains(part); });
    }

private slots:
    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        m_a = makePeer('a');
        m_b = makePeer('b');
        QVERIFY(m_a && m_b);
        m_a->node->createGroup();
        m_a->node->setOnline(m_a->onion);
        m_b->node->join({m_a->store->secret(), m_a->onion, m_a->nick, m_a->store->identityKey(), {}});
        m_b->node->setOnline(m_b->onion);
        QTRY_VERIFY(m_a->node->isConnected(m_b->onion) && m_b->node->isConnected(m_a->onion));
    }

    void cleanup()
    {
        m_byOnion.clear();
        m_a.reset();
        m_b.reset();
        m_dir.reset();
    }

    void offerAcceptedThenStopped()
    {
        QVERIFY(m_a->sharing->propose(m_b->onion, true));
        QVERIFY(!m_a->sharing->canShare()); // une proposition à la fois
        QTRY_COMPARE(m_b->offers.size(), 1);
        QCOMPARE(m_b->offers.first(), QVariantList({m_a->onion, m_a->nick, true}));

        m_b->sharing->answer(true);
        QTRY_VERIFY(m_a->sharing->session());
        QVERIFY(m_b->sharing->session());
        QCOMPARE(m_a->sharing->session()->role(), TerminalSession::Host);
        QCOMPARE(m_b->sharing->session()->role(), TerminalSession::Guest);
        QCOMPARE(m_a->sharing->session()->sid(), m_b->sharing->session()->sid());
        QVERIFY(m_a->sharing->session()->control());
        QVERIFY(contains(m_a->messages, "Console partagée avec Membre B (peut taper des commandes)"));

        m_a->sharing->stop(); // l'hôte arrête : l'invité est prévenu
        QVERIFY(!m_a->sharing->session());
        QTRY_VERIFY(!m_b->sharing->session());
        QVERIFY(contains(m_b->messages, "Console partagée fermée"));
        QVERIFY(m_a->sharing->canShare());
    }

    void offerRefused()
    {
        m_a->sharing->propose(m_b->onion, false);
        QTRY_COMPARE(m_b->offers.size(), 1);
        m_b->sharing->answer(false);
        QTRY_VERIFY(contains(m_a->messages, "Membre B a refusé le partage de console"));
        QVERIFY(!m_a->sharing->session() && !m_b->sharing->session());
        QVERIFY(m_a->sharing->canShare());
    }

    void offerCancelledBeforeAnswer()
    {
        m_a->sharing->propose(m_b->onion, false);
        QTRY_COMPARE(m_b->offers.size(), 1);
        m_a->sharing->stop(); // A se ravise
        QTRY_COMPARE(m_b->withdrawn, 1); // la question posée à B se ferme
        QVERIFY(contains(m_b->messages, "Membre A a annulé sa proposition"));
        m_b->sharing->answer(true); // trop tard : sans effet
        QTest::qWait(100);
        QVERIFY(!m_a->sharing->session() && !m_b->sharing->session());
    }

    void crossedOffersAreBusy()
    {
        // Chacun propose en même temps : chacun reçoit « occupé »
        m_a->sharing->propose(m_b->onion, false);
        m_b->sharing->propose(m_a->onion, false);
        QTRY_VERIFY(contains(m_a->messages, "Membre B a déjà une console partagée en cours"));
        QTRY_VERIFY(contains(m_b->messages, "Membre A a déjà une console partagée en cours"));
        QVERIFY(m_a->offers.isEmpty() && m_b->offers.isEmpty());
    }

    void unansweredOfferExpires()
    {
        m_a->sharing->setOfferTimeoutMs(200);
        m_a->sharing->propose(m_b->onion, false);
        QTRY_COMPARE(m_b->offers.size(), 1);
        QTRY_VERIFY(contains(m_a->messages, "Membre B n'a pas répondu"));
        QTRY_COMPARE(m_b->withdrawn, 1);
        QVERIFY(m_a->sharing->canShare());
    }

    void peerLeavingCancelsOffer()
    {
        m_a->sharing->propose(m_b->onion, false);
        QTRY_COMPARE(m_b->offers.size(), 1);
        m_b->node->setOffline();
        QTRY_VERIFY(contains(m_a->messages, "Proposition de console annulée : Membre B s'est déconnecté(e)"));
        QCOMPARE(m_b->withdrawn, 1);
        QVERIFY(m_a->sharing->canShare());
    }

    void sessionSurvivesShortCutAndEndsWhenGroupClosed()
    {
        m_a->sharing->propose(m_b->onion, true);
        QTRY_COMPARE(m_b->offers.size(), 1);
        m_b->sharing->answer(true);
        QTRY_VERIFY(m_a->sharing->session());
        // Coupure : la session attend le retour de l'autre membre…
        m_b->node->setOffline(); // côté B, plus aucune connexion : sa session se termine
        QVERIFY(!m_b->sharing->session());
        QTRY_VERIFY(!m_a->node->isConnected(m_b->onion));
        QVERIFY(m_a->sharing->session()); // …chez A, elle attend
        m_a->node->setOffline();          // plus aucune connexion chez A non plus
        QVERIFY(!m_a->sharing->session());
    }

    void startFailureIsReported()
    {
        m_b->startOk = false; // pas de terminal chez B
        m_a->sharing->propose(m_b->onion, false);
        QTRY_COMPARE(m_b->offers.size(), 1);
        m_b->sharing->answer(true);
        QVERIFY(!m_b->sharing->session());
        QVERIFY(contains(m_b->messages, "terminal introuvable"));
        QTRY_VERIFY(contains(m_a->messages, "a refusé")); // A n'attend pas indéfiniment
        QVERIFY(m_a->sharing->canShare());
    }
};

QTEST_GUILESS_MAIN(TestTerminalSharing)
#include "tst_terminalsharing.moc"
