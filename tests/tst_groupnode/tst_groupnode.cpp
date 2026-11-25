#include "crypto.h"
#include "groupnode.h"
#include "invitation.h"

#include <QNetworkProxy>
#include <QFile>
#include <QSettings>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

// Tests de bout en bout : plusieurs membres (GroupNode) reliés en local par de vraies connexions
// chiffrées, sans Tor. Le Dialer associe chaque adresse onion au port local du membre visé.
class TestGroupNode : public QObject
{
    Q_OBJECT

private:
    struct Peer
    {
        QString onion, nick;
        std::unique_ptr<QSettings> settings;
        std::unique_ptr<GroupStore> store;
        std::unique_ptr<GroupNode> node;
        QList<QJsonObject> added;   // messageAdded
        QList<QJsonObject> received; // messagesReceived
        int catchUpBatches = 0;
        QStringList system;          // memberAdded, presenceChanged, identityRejected, invalidMessage
    };

    std::unique_ptr<QTemporaryDir> m_dir; // un par test : historiques et réglages repartent de zéro
    std::map<char, std::unique_ptr<Peer>> m_peers;
    QHash<QString, Peer *> m_byOnion;      // adresse -> membre qui y répond
    QSet<QPair<QString, QString>> m_cut;   // liens impossibles (dans un sens comme dans l'autre)

    static GroupNode::Timing fastTiming()
    {
        GroupNode::Timing t;
        t.retryIntervalMs = 200;
        t.firstDialDelayMs = 0;
        t.redialDelayMs = 50;
        t.presenceGraceMs = 400;
        t.presenceBatchMs = 30;
        t.typingShownMs = 500;
        t.fileTickMs = 20;
        t.fileRequestTimeoutMs = 1000;
        return t;
    }

    Peer &peer(char name)
    {
        auto &p = m_peers[name];
        if (p)
            return *p;
        p = std::make_unique<Peer>();
        p->onion = QString(56, QLatin1Char(name)) + ".onion";
        p->nick = QStringLiteral("Membre %1").arg(QChar(name).toUpper());
        p->settings = std::make_unique<QSettings>(m_dir->filePath(QStringLiteral("%1.ini").arg(name)),
                                                  QSettings::IniFormat);
        p->store = std::make_unique<GroupStore>(*p->settings);
        p->store->ensureIdentity();
        p->store->setNickname(p->nick);
        start(*p, name);
        return *p;
    }

    // (Re)crée le nœud d'un membre, sur ses réglages et son historique
    void start(Peer &p, char name)
    {
        p.node.reset();
        p.node = std::make_unique<GroupNode>(*p.store, m_dir->filePath(QStringLiteral("%1.jsonl").arg(name)),
                                             fastTiming());
        QVERIFY(p.node->listen());
        p.node->setFilesDir(m_dir->filePath(QStringLiteral("%1-fichiers").arg(name)));
        const QString self = p.onion;
        p.node->setDialer([this, self](const QString &onion) {
            auto *s = new QTcpSocket;
            s->setProxy(QNetworkProxy::NoProxy);
            Peer *target = m_byOnion.value(onion);
            const bool cut = m_cut.contains({self, onion}) || m_cut.contains({onion, self});
            // Injoignable : port fermé (connexion refusée tout de suite)
            s->connectToHost(QHostAddress::LocalHost, target && !cut ? target->node->port() : 1);
            return s;
        });
        m_byOnion.insert(p.onion, &p);
        Peer *pp = &p;
        connect(p.node.get(), &GroupNode::messageAdded, this,
                [pp](const QJsonObject &m, bool, GroupNode::Trust) { pp->added << m; });
        connect(p.node.get(), &GroupNode::messagesReceived, this, [pp](const QList<QJsonObject> &l, bool catchUp) {
            pp->received << l;
            pp->catchUpBatches += catchUp ? 1 : 0;
        });
        connect(p.node.get(), &GroupNode::memberAdded, this,
                [pp](const QString &, const QString &nick, bool) { pp->system << "membre " + nick; });
        connect(p.node.get(), &GroupNode::presenceChanged, this,
                [pp](const QString &, const QString &nick, bool online, const QString &) {
                    pp->system << (online ? "en ligne " : "hors ligne ") + nick;
                });
        connect(p.node.get(), &GroupNode::identityRejected, this,
                [pp](const QString &, const QString &nick) { pp->system << "identité refusée " + nick; });
        connect(p.node.get(), &GroupNode::invalidMessage, this,
                [pp](const QString &author) { pp->system << "message falsifié " + author; });
    }

    static Invitation invitation(Peer &p)
    {
        return {p.store->secret(), p.onion, p.nick, p.store->identityKey()};
    }

    static bool linked(Peer &a, Peer &b)
    {
        return a.node->isConnected(b.onion) && b.node->isConnected(a.onion);
    }

    // A crée le groupe ; B rejoint avec le code de A, C avec celui de B
    void mesh()
    {
        Peer &a = peer('a'), &b = peer('b'), &c = peer('c');
        a.node->createGroup();
        a.node->setOnline(a.onion);
        QCOMPARE(b.node->join(invitation(a)), GroupNode::JoinResult::Joined);
        b.node->setOnline(b.onion);
        QTRY_VERIFY(linked(a, b));
        QCOMPARE(c.node->join(invitation(b)), GroupNode::JoinResult::Joined);
        c.node->setOnline(c.onion);
        QTRY_VERIFY(linked(b, c));
        QTRY_VERIFY(linked(a, c)); // C a découvert A par la liste des membres de B
    }

    static int countId(const QList<QJsonObject> &list, const QString &id)
    {
        return int(std::count_if(list.cbegin(), list.cend(),
                                 [&](const QJsonObject &m) { return Protocol::Message::id(m) == id; }));
    }

private slots:
    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_dir->isValid());
    }

    void cleanup()
    {
        for (auto &p : m_peers)
            if (p.second->node)
                p.second->node->closeAll();
        m_peers.clear();
        m_byOnion.clear();
        m_cut.clear();
        m_dir.reset();
    }

    void joinBuildsFullMesh()
    {
        mesh();
        Peer &a = peer('a'), &b = peer('b'), &c = peer('c');
        for (Peer *p : {&a, &b, &c})
            QCOMPARE(p->store->members().size(), 2);
        QVERIFY(c.store->isMember(a.onion));
        QVERIFY(a.system.contains("membre Membre B"));
        QVERIFY(a.system.contains("membre Membre C"));
        QVERIFY(c.node->relayedOnline().isEmpty()); // tout le monde est relié directement
        QVERIFY(a.node->canSend());
    }

    void messageReachesEveryoneOnce()
    {
        mesh();
        Peer &a = peer('a'), &b = peer('b'), &c = peer('c');
        const QJsonObject sent = a.node->sendMessage("Bonjour à tous");
        QVERIFY(!sent.isEmpty());
        const QString id = Protocol::Message::id(sent);
        QTRY_COMPARE(countId(b.added, id), 1);
        QTRY_COMPARE(countId(c.added, id), 1);
        QTest::qWait(200); // les relais arrivent aussi : toujours une seule fois
        QCOMPARE(countId(b.added, id), 1);
        QCOMPARE(countId(c.added, id), 1);
        QCOMPARE(countId(a.added, id), 1); // le nôtre aussi, affiché une fois
        QCOMPARE(b.received.size(), 1);
        QCOMPARE(b.catchUpBatches, 0); // reçu en direct
        QCOMPARE(b.node->verify(b.added.last(), false), GroupNode::Trust::Verified);
    }

    void messageIsRelayedWhenNoDirectLink()
    {
        // A et C ne peuvent pas se joindre : le message de A passe par B
        m_cut.insert({QString(56, 'a') + ".onion", QString(56, 'c') + ".onion"});
        Peer &a = peer('a'), &b = peer('b'), &c = peer('c');
        a.node->createGroup();
        a.node->setOnline(a.onion);
        b.node->join(invitation(a));
        b.node->setOnline(b.onion);
        QTRY_VERIFY(linked(a, b));
        c.node->join(invitation(b));
        c.node->setOnline(c.onion);
        QTRY_VERIFY(linked(b, c));
        QTRY_COMPARE(c.node->relayedOnline().value(a.onion), b.onion); // « en ligne via B »
        QVERIFY(!c.node->isConnected(a.onion));

        const QString id = Protocol::Message::id(a.node->sendMessage("par relais"));
        QTRY_COMPARE(countId(c.added, id), 1);
        QCOMPARE(c.node->verify(c.added.last(), false), GroupNode::Trust::Verified); // signé par A, pas par B
    }

    void reactionsPropagate()
    {
        mesh();
        Peer &a = peer('a'), &b = peer('b'), &c = peer('c');
        const QString id = Protocol::Message::id(a.node->sendMessage("On vote ?"));
        QTRY_COMPARE(countId(b.added, id), 1);
        b.node->react(id, QStringLiteral("👍"));
        QTRY_COMPARE(a.node->reactionSummary(id).size(), 1);
        QTRY_COMPARE(c.node->reactionSummary(id).size(), 1);
        QCOMPARE(c.node->reactionSummary(id).first().authors, QStringList({b.onion}));
        QVERIFY(b.node->reactionSummary(id).first().mine);

        b.node->react(id, QStringLiteral("👍")); // la retirer
        QTRY_VERIFY(a.node->reactionSummary(id).isEmpty());
        QTRY_VERIFY(c.node->reactionSummary(id).isEmpty());
        b.node->react(id, QStringLiteral("pas un émoji autorisé"));
        QTest::qWait(100);
        QVERIFY(a.node->reactionSummary(id).isEmpty());
    }

    void offlineMemberCatchesUp()
    {
        mesh();
        Peer &a = peer('a'), &b = peer('b'), &c = peer('c');
        c.node->setOffline();
        QTRY_VERIFY(!a.node->isConnected(c.onion));
        const QString id1 = Protocol::Message::id(a.node->sendMessage("pendant ton absence"));
        const QString id2 = Protocol::Message::id(b.node->sendMessage("moi aussi"));
        const QString react = id1;
        a.node->react(react, QStringLiteral("🎉"));
        QTest::qWait(100);
        QCOMPARE(countId(c.added, id1), 0);

        c.node->setOnline(c.onion);
        QTRY_COMPARE(countId(c.added, id1), 1);
        QTRY_COMPARE(countId(c.added, id2), 1);
        QTRY_COMPARE(c.node->reactionSummary(react).size(), 1); // les réactions aussi
        QVERIFY(c.catchUpBatches > 0);                           // signalé comme rattrapage
        QTest::qWait(200); // A et B envoient tous deux leur inventaire : pas de doublon
        QCOMPARE(countId(c.added, id1), 1);
    }

    void typingIndicator()
    {
        mesh();
        Peer &a = peer('a'), &b = peer('b');
        QSignalSpy typing(b.node.get(), &GroupNode::typingChanged);
        a.node->typing();
        QTRY_COMPARE(b.node->typingMembers(), QStringList({a.onion}));
        a.node->sendMessage("fini"); // son message arrive : il n'écrit plus
        QTRY_VERIFY(b.node->typingMembers().isEmpty());
        a.node->typing();
        QTRY_COMPARE(b.node->typingMembers(), QStringList({a.onion}));
        QTRY_VERIFY_WITH_TIMEOUT(b.node->typingMembers().isEmpty(), 2000); // expire tout seul
        QVERIFY(!typing.isEmpty());
    }

    void forgedMessageIsRejected()
    {
        mesh();
        Peer &a = peer('a'), &b = peer('b'), &c = peer('c');
        // B fabrique un message au nom de C, signé avec sa propre clé
        const QJsonObject forged = Protocol::Message::create(b.store->identitySeed(), c.onion, c.nick,
                                                             "Je démissionne", QDateTime::currentMSecsSinceEpoch());
        b.node->sendTo(a.onion, forged);
        QTRY_VERIFY(a.system.contains("message falsifié " + c.nick));
        QCOMPARE(countId(a.added, Protocol::Message::id(forged)), 0);
    }

    void impostorIsRejected()
    {
        Peer &a = peer('a'), &b = peer('b');
        a.node->createGroup();
        a.node->setOnline(a.onion);
        b.node->join(invitation(a)); // B mémorise la clé de A
        b.node->setOnline(b.onion);
        QTRY_VERIFY(linked(a, b));
        b.node->setOffline();
        QTRY_VERIFY(!a.node->isConnected(b.onion));

        // M connaît le secret du groupe et se fait passer pour A (même adresse), avec sa propre clé
        Peer &m = peer('m');
        m.store->setSecret(a.store->secret());
        m.store->setNickname(a.nick);
        m.store->setMembers({{b.onion, b.nick}});
        a.node->setOffline();
        m.onion = a.onion;
        m_byOnion.insert(a.onion, &m); // l'adresse de A mène à M
        m.node->setOnline(a.onion);
        b.node->setOnline(b.onion);
        QTRY_VERIFY(b.system.contains("identité refusée " + a.nick));
        QTest::qWait(100);
        QVERIFY(!b.node->isConnected(a.onion));
    }

    void wrongSecretCannotConnect()
    {
        Peer &a = peer('a'), &b = peer('b');
        a.node->createGroup();
        a.node->setOnline(a.onion);
        Invitation bad = invitation(a);
        bad.secret = Crypto::randomBytes(16); // code d'un autre groupe, ou falsifié
        b.node->join(bad);
        b.node->setOnline(b.onion);
        QTRY_VERIFY(!b.node->lastError(a.onion).isEmpty());
        QVERIFY(!b.node->isConnected(a.onion));
        QVERIFY(!a.node->isConnected(b.onion));
    }

    void departureAnnouncedAfterGrace()
    {
        mesh();
        Peer &a = peer('a'), &b = peer('b');
        // Coupure brève : B revient avant la fin du délai de grâce, rien n'est annoncé
        b.node->setOffline();
        b.node->setOnline(b.onion);
        QTRY_VERIFY(linked(a, b));
        QTest::qWait(600);
        QVERIFY(!a.system.contains("hors ligne " + b.nick));

        // Départ : annoncé, mais seulement après le délai de grâce
        b.node->leaveGroup();
        QTRY_VERIFY(!a.node->isConnected(b.onion));
        QVERIFY(!a.system.contains("hors ligne " + b.nick));
        QTRY_VERIFY(a.system.contains("hors ligne " + b.nick));
    }

    void historyIsReloaded()
    {
        mesh();
        Peer &a = peer('a'), &b = peer('b');
        const QString id = Protocol::Message::id(a.node->sendMessage("à garder"));
        QTRY_COMPARE(countId(b.added, id), 1);
        b.node->react(id, QStringLiteral("❤️"));
        QTRY_COMPARE(a.node->reactionSummary(id).size(), 1);

        // Redémarrage de A : messages et réactions relus et vérifiés
        a.added.clear();
        start(a, 'a');
        a.node->loadHistory();
        QCOMPARE(countId(a.added, id), 1);
        QCOMPARE(a.node->verify(a.added.last(), true), GroupNode::Trust::Verified);
        QVERIFY(a.node->isMine(id));
        QCOMPARE(a.node->reactionSummary(id).size(), 1);

        // Historique vidé : plus rien au prochain démarrage, ni par synchronisation
        a.node->clearHistory();
        a.added.clear();
        start(a, 'a');
        a.node->loadHistory();
        QVERIFY(a.added.isEmpty());
    }

    // ---------------------------------------------------------------- Fichiers

    QString sourceFile(const QString &name, const QByteArray &content)
    {
        const QString path = m_dir->filePath("envoi-" + name);
        QFile f(path);
        if (f.open(QIODevice::WriteOnly))
            f.write(content);
        return path;
    }

    static QByteArray contentOf(Peer &p, const QString &id)
    {
        QFile f(p.node->files().path(id));
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }

    static QString fileId(const QJsonObject &msg)
    {
        const auto info = Files::FileInfo::fromJson(msg.value("f").toObject());
        return info ? info->id() : QString();
    }

    void smallFileIsDownloadedAutomatically()
    {
        mesh();
        Peer &a = peer('a'), &b = peer('b'), &c = peer('c');
        const QByteArray content = Crypto::randomBytes(300 * 1024);
        QString error;
        const QJsonObject msg = a.node->sendFile(sourceFile("photo.raw", content), "Regardez", {}, &error);
        QVERIFY2(!msg.isEmpty(), qPrintable(error));
        const QString id = fileId(msg);
        QCOMPARE(a.node->fileState(id).kind, GroupNode::FileState::Complete);
        QTRY_COMPARE(b.node->fileState(id).kind, GroupNode::FileState::Complete);
        QTRY_COMPARE(c.node->fileState(id).kind, GroupNode::FileState::Complete);
        QCOMPARE(contentOf(b, id), content);
        QCOMPARE(contentOf(c, id), content);
        // L'annonce est un message authentique, avec sa légende
        QCOMPARE(countId(b.added, Protocol::Message::id(msg)), 1);
        QVERIFY(b.node->verifiedFile(b.added.last(), false));
        QCOMPARE(Protocol::Message::caption(b.added.last(), true), QString("Regardez"));
    }

    void bigFileOnlyOnDemand()
    {
        mesh();
        Peer &a = peer('a'), &b = peer('b'), &c = peer('c');
        const QByteArray content = Crypto::randomBytes(int(Files::AutoDownloadBytes) + 500 * 1024);
        QString error;
        const QString id = fileId(a.node->sendFile(sourceFile("video.mp4", content), {}, {}, &error));
        QVERIFY2(!id.isEmpty(), qPrintable(error));
        QTRY_COMPARE(countId(b.added, Protocol::Message::id(a.added.last())), 1);
        QTest::qWait(200);
        QCOMPARE(b.node->fileState(id).kind, GroupNode::FileState::NotDownloaded); // plus de 2 Mo : sur demande
        QCOMPARE(b.node->fileState(id).total, Files::FileInfo::expectedChunks(content.size()));

        QSignalSpy progress(b.node.get(), &GroupNode::fileChanged);
        b.node->download(id);
        QTRY_COMPARE(b.node->fileState(id).kind, GroupNode::FileState::Complete);
        QCOMPARE(contentOf(b, id), content);
        QVERIFY(progress.count() > 3); // progression signalée en route
        QCOMPARE(c.node->fileState(id).kind, GroupNode::FileState::NotDownloaded); // C n'a rien demandé
        QVERIFY(!c.node->files().has(id));
    }

    void servedByAnotherMember()
    {
        mesh();
        Peer &a = peer('a'), &b = peer('b'), &c = peer('c');
        const QByteArray content = Crypto::randomBytes(3 * 1024 * 1024);
        const QString id = fileId(a.node->sendFile(sourceFile("archive.zip", content)));
        QTRY_COMPARE(b.node->fileState(id).kind, GroupNode::FileState::NotDownloaded);
        b.node->download(id);
        QTRY_COMPARE(b.node->fileState(id).kind, GroupNode::FileState::Complete);

        a.node->setOffline(); // l'auteur n'est plus là : B, qui l'a, le fournit
        QTRY_VERIFY(!c.node->isConnected(a.onion));
        QTRY_COMPARE(c.node->fileState(id).kind, GroupNode::FileState::NotDownloaded);
        c.node->download(id);
        QTRY_COMPARE(c.node->fileState(id).kind, GroupNode::FileState::Complete);
        QCOMPARE(contentOf(c, id), content);
    }

    void resumesAfterCutAndRestart()
    {
        mesh();
        Peer &a = peer('a'), &c = peer('c');
        const QByteArray content = Crypto::randomBytes(4 * 1024 * 1024);
        const QString id = fileId(a.node->sendFile(sourceFile("gros.iso", content)));
        QTRY_COMPARE(c.node->fileState(id).kind, GroupNode::FileState::NotDownloaded);

        // Coupure au milieu : C passe hors ligne après 5 morceaux
        int cutAt = -1;
        auto cut = connect(c.node.get(), &GroupNode::fileChanged, this, [&] {
            if (cutAt < 0 && c.node->fileState(id).received >= 5) {
                cutAt = c.node->fileState(id).received;
                QTimer::singleShot(0, c.node.get(), &GroupNode::setOffline);
            }
        });
        c.node->download(id);
        QTRY_VERIFY(cutAt >= 0);
        disconnect(cut);
        QTRY_VERIFY(!c.node->hasConnections());
        const int afterCut = c.node->fileState(id).received;
        QVERIFY(afterCut >= cutAt && afterCut < c.node->fileState(id).total);
        QCOMPARE(c.node->fileState(id).kind, GroupNode::FileState::Waiting);

        // Redémarrage complet de C : le téléchargement reprend là où il en était
        start(c, 'c');
        c.node->loadHistory();
        QCOMPARE(c.node->fileState(id).received, afterCut);
        c.node->setOnline(c.onion);
        QTRY_COMPARE(c.node->fileState(id).kind, GroupNode::FileState::Complete);
        QCOMPARE(contentOf(c, id), content);
    }

    void corruptedSourceIsAvoided()
    {
        mesh();
        Peer &a = peer('a'), &b = peer('b'), &c = peer('c');
        const QByteArray content = Crypto::randomBytes(3 * 1024 * 1024);
        const QString id = fileId(a.node->sendFile(sourceFile("donnees.bin", content)));
        QTRY_COMPARE(b.node->fileState(id).kind, GroupNode::FileState::NotDownloaded);
        b.node->download(id);
        QTRY_COMPARE(b.node->fileState(id).kind, GroupNode::FileState::Complete);
        // Le fichier de B s'abîme (ou B triche) ; A s'absente
        {
            QFile f(b.node->files().path(id));
            QVERIFY(f.open(QIODevice::ReadWrite) && f.seek(Files::ChunkBytes + 10));
            f.write("abime");
        }
        a.node->setOffline();
        QTRY_VERIFY(!c.node->isConnected(a.onion));
        QTRY_COMPARE(c.node->fileState(id).kind, GroupNode::FileState::NotDownloaded);
        c.node->download(id);
        // Le morceau faux de B est refusé, B est écarté : C attend une autre source
        QTRY_COMPARE(c.node->fileState(id).kind, GroupNode::FileState::Waiting);
        QTest::qWait(200);
        QVERIFY(c.node->fileState(id).kind != GroupNode::FileState::Complete);
        a.node->setOnline(a.onion); // l'auteur revient : C termine auprès de lui
        QTRY_COMPARE_WITH_TIMEOUT(c.node->fileState(id).kind, GroupNode::FileState::Complete, 10000);
        QCOMPARE(contentOf(c, id), content);
    }

    void fileLimitsAndClearing()
    {
        mesh();
        Peer &a = peer('a'), &b = peer('b');
        // Plus de 100 Mo : refusé avant tout envoi (fichier creux, rien n'est écrit)
        const QString big = sourceFile("enorme.bin", {});
        QFile f(big);
        QVERIFY(f.open(QIODevice::ReadWrite) && f.resize(Files::MaxFileBytes + 1));
        f.close();
        QString error;
        QVERIFY(a.node->sendFile(big, {}, {}, &error).isEmpty());
        QVERIFY(error.contains("100 Mo"));

        const QString id = fileId(a.node->sendFile(sourceFile("petit.txt", "contenu")));
        QTRY_COMPARE(b.node->fileState(id).kind, GroupNode::FileState::Complete);
        b.node->clearHistory(); // vider la conversation efface aussi les fichiers
        QVERIFY(!b.node->files().has(id));
        QCOMPARE(b.node->fileState(id).kind, GroupNode::FileState::Unknown);
    }

    void cannotSendWithoutNetworkOrGroup()
    {
        Peer &a = peer('a');
        QVERIFY(a.node->sendMessage("x").isEmpty()); // pas de groupe
        a.node->createGroup();
        QVERIFY(a.node->sendMessage("x").isEmpty()); // pas de réseau, pas de membres
        a.node->setOnline(a.onion);
        QVERIFY(a.node->sendMessage("x").isEmpty()); // personne d'autre dans le groupe
        QCOMPARE(a.node->join(invitation(a)), GroupNode::JoinResult::OwnCode);
    }
};

QTEST_GUILESS_MAIN(TestGroupNode)
#include "tst_groupnode.moc"
