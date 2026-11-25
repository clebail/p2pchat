#include "crypto.h"
#include "groupnode.h"
#include "edits.h"
#include "governance.h"
#include "invitation.h"
#include "switchboard.h"

#include <QNetworkProxy>
#include <QFile>
#include <QDateTime>
#include <QJsonArray>
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
        connect(p.node.get(), &GroupNode::memberExcluded, this,
                [pp](const QString &, const QString &nick, const QString &by) { pp->system << "exclu " + nick + " par " + by; });
        connect(p.node.get(), &GroupNode::excludedFromGroup, this,
                [pp](const QString &by) { pp->system << "nous exclus par " + by; });
        connect(p.node.get(), &GroupNode::secretRenewed, this,
                [pp](const QString &by) { pp->system << "secret renouvelé par " + by; });
        connect(p.node.get(), &GroupNode::adminChanged, this,
                [pp](const QString &, const QString &nick, bool admin, const QString &by) {
                    pp->system << (admin ? "admin " : "plus admin ") + nick + " par " + by;
                });
        connect(p.node.get(), &GroupNode::sharedNameChanged, this,
                [pp](const QString &name, const QString &by) { pp->system << "nom " + name + " par " + by; });
        connect(p.node.get(), &GroupNode::memberLeft, this,
                [pp](const QString &, const QString &nick) { pp->system << "parti " + nick; });
    }

    static Invitation invitation(Peer &p)
    {
        return {p.store->secret(), p.onion, p.nick, p.store->identityKey(), p.node->groupId()};
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

        // Disparition : annoncée, mais seulement après le délai de grâce
        b.node->setOffline();
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
        QCOMPARE(b.node->binaryChunksReceived(), Files::FileInfo::expectedChunks(content.size())); // tout en binaire
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

    void previousVersionGetsJsonChunks()
    {
        // B se présente comme une version 9 : les morceaux lui arrivent en JSON (base64), et ça marche
        Peer &a = peer('a'), &b = peer('b');
        b.node->setAdvertisedVersion(9);
        a.node->createGroup();
        a.node->setOnline(a.onion);
        b.node->join(invitation(a));
        b.node->setOnline(b.onion);
        QTRY_VERIFY(linked(a, b));
        QCOMPARE(a.node->peerVersion(b.onion), 9);
        const QByteArray content = Crypto::randomBytes(1024 * 1024);
        const QString id = fileId(a.node->sendFile(sourceFile("compat.bin", content)));
        QTRY_COMPARE(b.node->fileState(id).kind, GroupNode::FileState::Complete);
        QCOMPARE(contentOf(b, id), content);
        QCOMPARE(b.node->binaryChunksReceived(), 0);
    }

    void multipleSourcesShareTheWork()
    {
        mesh();
        Peer &a = peer('a'), &b = peer('b'), &c = peer('c');
        const QByteArray content = Crypto::randomBytes(3 * 1024 * 1024); // 20 morceaux, pas automatique
        const QString id = fileId(a.node->sendFile(sourceFile("gros.bin", content)));
        QTRY_COMPARE(b.node->fileState(id).kind, GroupNode::FileState::NotDownloaded); // annonce reçue
        QTRY_COMPARE(c.node->fileState(id).kind, GroupNode::FileState::NotDownloaded);
        b.node->download(id);
        QTRY_COMPARE(b.node->fileState(id).kind, GroupNode::FileState::Complete);
        // A et B l'ont : C le demande aux deux à la fois
        QSet<QString> seen; // sources vues pendant le téléchargement
        QObject guard;
        connect(c.node.get(), &GroupNode::fileChanged, &guard, [&](const QString &changed) {
            if (changed == id)
                for (const QString &source : c.node->fileState(id).sources)
                    seen.insert(source);
        });
        c.node->download(id);
        QTRY_COMPARE(c.node->fileState(id).kind, GroupNode::FileState::Complete);
        QCOMPARE(contentOf(c, id), content);
        QVERIFY(seen.contains(a.onion));
        QVERIFY(seen.contains(b.onion));
    }

    void partialHolderIsASource()
    {
        // B n'a qu'un morceau sur deux et ne joint pas A ; C joint les deux
        mesh();
        Peer &a = peer('a'), &b = peer('b'), &c = peer('c');
        const QByteArray content = Crypto::randomBytes(3 * 1024 * 1024);
        const QString id = fileId(a.node->sendFile(sourceFile("gros.bin", content)));
        QTRY_COMPARE(b.node->fileState(id).kind, GroupNode::FileState::NotDownloaded);
        const auto info = a.node->files().info(id);
        QVERIFY(info && b.node->files().begin(*info));
        for (int i = 0; i < info->chunkCount(); i += 2)
            b.node->files().writeChunk(id, i, a.node->files().readChunk(id, i));
        m_cut.insert({a.onion, b.onion});
        b.node->setOffline();
        QTRY_VERIFY(!a.node->isConnected(b.onion));
        start(b, 'b');
        b.node->download(id); // reprise : rien de neuf à attendre de C, mais B annonce ce qu'il a
        b.node->setOnline(b.onion);
        QTRY_VERIFY(linked(b, c));
        QVERIFY(!b.node->isConnected(a.onion));

        QSet<QString> seen; // sources vues pendant le téléchargement
        QObject guard;
        connect(c.node.get(), &GroupNode::fileChanged, &guard, [&](const QString &changed) {
            if (changed == id)
                for (const QString &source : c.node->fileState(id).sources)
                    seen.insert(source);
        });
        c.node->download(id);
        QTRY_COMPARE(c.node->fileState(id).kind, GroupNode::FileState::Complete);
        QCOMPARE(contentOf(c, id), content);
        QVERIFY(seen.contains(b.onion)); // des morceaux pairs sont venus de B
        // Et B termine grâce à C, devenu source à son tour
        QTRY_COMPARE(b.node->fileState(id).kind, GroupNode::FileState::Complete);
        QCOMPARE(contentOf(b, id), content);
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

    // ---------------------------------------------------------------- Gouvernance

    void founderIsAdmin()
    {
        mesh();
        Peer &a = peer('a'), &b = peer('b'), &c = peer('c');
        QVERIFY(!a.node->groupId().isEmpty());
        QTRY_COMPARE(c.node->groupId(), a.node->groupId()); // reçu en passant par B
        QVERIFY(a.node->isAdmin());
        QVERIFY(c.node->isAdmin(a.onion));
        QVERIFY(!b.node->isAdmin());
        QVERIFY(a.node->canGovern());
        QVERIFY(!b.node->canGovern());
        QString error;
        QVERIFY(!b.node->exclude(c.onion, &error)); // un simple membre ne peut pas
        QVERIFY(error.contains("administrateur"));
    }

    void excludedMemberIsCutOff()
    {
        mesh();
        Peer &a = peer('a'), &b = peer('b'), &c = peer('c');
        QTRY_COMPARE(c.node->groupId(), a.node->groupId());
        const QByteArray oldSecret = a.store->secret();
        const Invitation oldCode = invitation(a);
        QVERIFY(a.node->exclude(c.onion));
        // C l'apprend (la chaîne le prouve), tout est fermé chez lui
        QTRY_VERIFY(c.system.contains("nous exclus par " + a.nick));
        QVERIFY(!c.store->inGroup());
        QTRY_VERIFY(!b.node->isConnected(c.onion));
        // B reçoit le nouveau secret et oublie C
        QTRY_COMPARE(b.store->secret(), a.store->secret());
        QVERIFY(a.store->secret() != oldSecret);
        QVERIFY(b.system.contains("exclu " + c.nick + " par " + a.nick));
        QVERIFY(!b.store->isMember(c.onion));
        QVERIFY(a.node->excludedMembers().contains(c.onion));

        // C tente de revenir avec son ancien code (ancien secret, encore accepté pour les absents) : refusé
        c.added.clear();
        QCOMPARE(c.node->join(oldCode), GroupNode::JoinResult::Joined);
        c.node->setOnline(c.onion);
        QTRY_VERIFY(c.system.count("nous exclus par " + a.nick) >= 2); // la chaîne lui est remontrée
        QTest::qWait(300);
        QVERIFY(!a.node->isConnected(c.onion) && !b.node->isConnected(c.onion));
        // Et les messages ne lui parviennent plus
        const QString id = Protocol::Message::id(a.node->sendMessage("sans C"));
        QTRY_COMPARE(countId(b.added, id), 1);
        QCOMPARE(countId(c.added, id), 0);
    }

    void absentMemberGetsNewSecret()
    {
        mesh();
        Peer &a = peer('a'), &b = peer('b'), &c = peer('c');
        QTRY_COMPARE(c.node->groupId(), a.node->groupId());
        const Invitation oldCode = invitation(a);
        b.node->setOffline();
        QTRY_VERIFY(!a.node->isConnected(b.onion));
        QVERIFY(a.node->renewSecret());
        QTRY_COMPARE(c.store->secret(), a.store->secret());
        QVERIFY(b.store->secret() != a.store->secret());

        // B revient avec l'ancien secret : accepté (membre connu), et reçoit le nouveau
        b.node->setOnline(b.onion);
        QTRY_VERIFY(linked(a, b));
        QTRY_COMPARE(b.store->secret(), a.store->secret());
        QVERIFY(b.system.contains("secret renouvelé par " + a.nick));

        // Un inconnu avec l'ancien code ne rentre plus ; avec un code récent, si
        Peer &d = peer('d'), &e = peer('e');
        QCOMPARE(d.node->join(oldCode), GroupNode::JoinResult::Joined);
        d.node->setOnline(d.onion);
        QTest::qWait(400);
        QVERIFY(!a.node->isConnected(d.onion));
        QVERIFY(!a.store->isMember(d.onion));
        QCOMPARE(e.node->join(invitation(a)), GroupNode::JoinResult::Joined);
        e.node->setOnline(e.onion);
        QTRY_VERIFY(linked(a, e));
        QTRY_COMPARE(e.node->groupId(), a.node->groupId());
    }

    void newSecretTravelsThroughRelays()
    {
        // A et C ne peuvent pas se joindre : le nouveau secret passe par B
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
        QTRY_COMPARE(c.node->groupId(), a.node->groupId());
        QVERIFY(a.node->renewSecret());
        QTRY_COMPARE(c.store->secret(), a.store->secret());
        QTRY_COMPARE(b.store->secret(), a.store->secret());
    }

    void forgedGovernanceIsIgnored()
    {
        mesh();
        Peer &a = peer('a'), &b = peer('b'), &c = peer('c');
        QTRY_COMPARE(b.node->groupId(), a.node->groupId());
        // B fabrique un enregistrement l'excluant A et se nommant administrateur
        const auto genesis = Governance::parse(a.store->governanceRecords().first());
        const QJsonObject coup = Governance::create(b.store->identitySeed(), b.onion, &*genesis, {b.onion},
                                                    {a.onion}, "secret pirate !!", QDateTime::currentMSecsSinceEpoch());
        QJsonArray records{genesis->json, coup};
        b.node->sendTo(c.onion, {{"t", "gov"}, {"records", records}});
        QTest::qWait(200);
        QVERIFY(c.node->isAdmin(a.onion));
        QVERIFY(!c.node->isAdmin(b.onion));
        QVERIFY(c.node->excludedMembers().isEmpty());
        QVERIFY(c.node->isConnected(a.onion));
        // Ni un secret qui ne correspond pas à la chaîne
        const QByteArray before = c.store->secret();
        b.node->sendTo(c.onion, {{"t", "gov-secret"}, {"seq", 0}, {"k", Protocol::toB64("secret pirate !!")}});
        QTest::qWait(100);
        QCOMPARE(c.store->secret(), before);
    }

    void legacyGroupGetsGovernance()
    {
        // Groupe d'une version précédente : pas de fondateur, pas de gouvernance
        Peer &a = peer('a'), &b = peer('b');
        a.store->setSecret(Crypto::randomBytes(16));
        a.node->setOnline(a.onion);
        QVERIFY(a.node->groupId().isEmpty());
        b.node->join(invitation(a));
        b.node->setOnline(b.onion);
        QTRY_VERIFY(linked(a, b));
        QVERIFY(b.node->canGovern()); // le premier qui agit deviendra administrateur
        QVERIFY(b.node->renewSecret());
        QTRY_COMPARE(a.node->groupId(), b.node->groupId());
        QVERIFY(a.node->isAdmin(b.onion));
        QVERIFY(!a.node->canGovern()); // désormais, seul B le peut
        QTRY_COMPARE(a.store->secret(), b.store->secret());
    }

    void voluntaryLeaveIsAnnounced()
    {
        mesh();
        Peer &a = peer('a'), &b = peer('b'), &c = peer('c');
        b.node->leaveGroup();
        QTRY_VERIFY(a.system.contains("parti " + b.nick));
        QTRY_VERIFY(c.system.contains("parti " + b.nick));
        QVERIFY(!a.store->isMember(b.onion));
        // La liste des membres d'un autre ne le réintroduit pas
        c.node->announceNickname();
        QTest::qWait(200);
        QVERIFY(!a.store->isMember(b.onion));
    }

    // ---------------------------------------------------------------- Plusieurs conversations

    void messagesAreEditedAndDeleted()
    {
        mesh();
        Peer &a = peer('a'), &b = peer('b'), &c = peer('c');
        const QJsonObject msg = a.node->sendMessage("Rendez-vous à 18 h");
        const QString id = Protocol::Message::id(msg);
        QTRY_COMPARE(countId(b.added, id), 1);
        QVERIFY(a.node->canEdit(id));
        QVERIFY(!b.node->canEdit(id)); // pas le sien
        QVERIFY(!b.node->editMessage(id, "Piraté"));

        QVERIFY(a.node->editMessage(id, "Rendez-vous à 19 h"));
        QTRY_VERIFY(b.node->latestEdit(b.node->storedMessage(id)).has_value());
        QCOMPARE(b.node->latestEdit(b.node->storedMessage(id))->text, QString("Rendez-vous à 19 h"));
        QTRY_VERIFY(c.node->latestEdit(c.node->storedMessage(id)).has_value());

        // Une « modification » signée par B ne compte pas : seule celle de l'auteur du message
        b.node->sendTo(c.onion, Edits::create(b.store->identitySeed(), b.onion, id, "Faux", false,
                                              QDateTime::currentMSecsSinceEpoch() + 1000));
        QTest::qWait(150);
        QCOMPARE(c.node->latestEdit(c.node->storedMessage(id))->text, QString("Rendez-vous à 19 h"));

        // Suppression, définitive ; C, absent, la reçoit à son retour
        c.node->setOffline();
        QTRY_VERIFY(!a.node->isConnected(c.onion));
        QVERIFY(a.node->deleteMessage(id));
        QVERIFY(!a.node->canEdit(id));
        QVERIFY(!a.node->editMessage(id, "Revenu"));
        QTRY_VERIFY(b.node->latestEdit(b.node->storedMessage(id))->deleted);
        c.node->setOnline(c.onion);
        QTRY_VERIFY(c.node->latestEdit(c.node->storedMessage(id))->deleted);

        // Conservé dans l'historique : après redémarrage, toujours supprimé
        start(c, 'c');
        c.node->loadHistory();
        QVERIFY(c.node->latestEdit(c.node->storedMessage(id))->deleted);
    }

    void adminsModerateMessages()
    {
        mesh();
        Peer &a = peer('a'), &b = peer('b'), &c = peer('c');
        QTRY_COMPARE(c.node->groupId(), a.node->groupId());
        const QString fromB = Protocol::Message::id(b.node->sendMessage("Message de B"));
        const QString fromA = Protocol::Message::id(a.node->sendMessage("Message de A"));
        QTRY_COMPARE(countId(c.added, fromB), 1);
        QTRY_COMPARE(countId(c.added, fromA), 1);
        QTRY_COMPARE(countId(a.added, fromB), 1);

        // A, administrateur, modère le message de B ; C, simple membre, ne peut pas toucher à celui de A
        QVERIFY(a.node->canEdit(fromB));
        QVERIFY(!c.node->canEdit(fromA));
        QVERIFY(a.node->deleteMessage(fromB));
        QTRY_VERIFY(c.node->latestEdit(c.node->storedMessage(fromB)).has_value());
        const auto l = c.node->latestEdit(c.node->storedMessage(fromB));
        QVERIFY(l->deleted);
        QCOMPARE(l->author, a.onion);
        QTRY_VERIFY(b.node->latestEdit(b.node->storedMessage(fromB)).has_value()); // chez l'auteur aussi

        // Une « modération » signée par C (pas administrateur) ne compte pas
        c.node->sendTo(b.onion, Edits::create(c.store->identitySeed(), c.onion, fromA, {}, true,
                                              QDateTime::currentMSecsSinceEpoch()));
        QTest::qWait(150);
        QVERIFY(!b.node->latestEdit(b.node->storedMessage(fromA)).has_value());
        // … jusqu'à ce que C soit nommé : ses modérations reçues avant comptent alors
        QVERIFY(a.node->setAdmin(c.onion, true));
        QTRY_VERIFY(b.node->latestEdit(b.node->storedMessage(fromA)).has_value());
    }

    void previousVersionGetsNoEdits()
    {
        Peer &a = peer('a'), &b = peer('b');
        b.node->setAdvertisedVersion(12);
        a.node->createGroup();
        a.node->setOnline(a.onion);
        b.node->join(invitation(a));
        b.node->setOnline(b.onion);
        QTRY_VERIFY(linked(a, b));
        const QString id = Protocol::Message::id(a.node->sendMessage("Bonjour"));
        QTRY_COMPARE(countId(b.added, id), 1);
        QVERIFY(a.node->editMessage(id, "Bonjour à tous"));
        QTest::qWait(200);
        QVERIFY(!b.node->latestEdit(b.node->storedMessage(id)).has_value()); // il ne saurait pas l'afficher
    }

    void adminsAreNamedAndRemoved()
    {
        mesh();
        Peer &a = peer('a'), &b = peer('b'), &c = peer('c');
        QTRY_COMPARE(c.node->groupId(), a.node->groupId());
        QString error;
        QVERIFY(!b.node->setAdmin(c.onion, true, &error)); // pas administrateur
        QVERIFY(!a.node->setAdmin(a.onion, false, &error)); // le dernier
        QVERIFY(error.contains("au moins un"));
        QVERIFY(!a.node->setAdmin(QString(56, 'z') + ".onion", true, &error)); // inconnu

        // A nomme B : sans nouveau secret
        const QByteArray secret = a.store->secret();
        QVERIFY(a.node->setAdmin(b.onion, true));
        QTRY_VERIFY(c.node->isAdmin(b.onion));
        QTRY_VERIFY(b.node->canGovern());
        QCOMPARE(a.store->secret(), secret);
        QCOMPARE(c.store->secret(), secret);
        QVERIFY(c.system.contains("admin " + b.nick + " par " + a.nick));
        QVERIFY(!c.system.contains("secret renouvelé par " + a.nick));

        // B retire A, qui ne peut plus rien ; B, seul administrateur, ne peut pas partir
        QVERIFY(b.node->setAdmin(a.onion, false));
        QTRY_VERIFY(!a.node->isAdmin());
        QVERIFY(!a.node->canGovern());
        QVERIFY(!a.node->setAdmin(c.onion, true, &error));
        QTRY_VERIFY(c.system.contains("plus admin " + a.nick + " par " + b.nick));
        QVERIFY(!b.node->setAdmin(b.onion, false, &error));

        // Un administrateur exclu perd son rôle
        QVERIFY(b.node->setAdmin(c.onion, true));
        QTRY_VERIFY(a.node->isAdmin(c.onion));
        QVERIFY(b.node->exclude(c.onion));
        QTRY_VERIFY(!a.node->isAdmin(c.onion));
        QCOMPARE(a.node->admins(), QStringList{b.onion});
    }

    void sharedNameFromAdmin()
    {
        mesh();
        Peer &a = peer('a'), &b = peer('b'), &c = peer('c');
        QTRY_COMPARE(c.node->groupId(), a.node->groupId());
        const QString group = a.node->groupId();
        QString error;
        QVERIFY(!b.node->setSharedName("Coup d'État", &error)); // pas administrateur
        QVERIFY(a.node->setSharedName("Les amis"));
        QTRY_COMPARE(b.node->sharedName(), QString("Les amis"));
        QTRY_COMPARE(c.node->sharedName(), QString("Les amis"));
        QVERIFY(c.system.contains("nom Les amis par " + a.nick));

        // Signé par un simple membre, ou plus ancien : ignoré
        const qint64 later = QDateTime::currentMSecsSinceEpoch() + 100000;
        b.node->sendTo(c.onion, Governance::createName(b.store->identitySeed(), b.onion, group, "Pirate", later));
        a.node->sendTo(c.onion, Governance::createName(a.store->identitySeed(), a.onion, group, "Ancien", 1));
        // Signature d'un autre groupe
        a.node->sendTo(c.onion,
                       Governance::createName(a.store->identitySeed(), a.onion, QString(64, 'e'), "Ailleurs", later));
        QTest::qWait(200);
        QCOMPARE(c.node->sharedName(), QString("Les amis"));

        // Un nouveau venu le reçoit en se connectant
        Peer &d = peer('d');
        QCOMPARE(d.node->join(invitation(c)), GroupNode::JoinResult::Joined);
        d.node->setOnline(d.onion);
        QTRY_COMPARE(d.node->sharedName(), QString("Les amis"));
    }

    void directInvitation()
    {
        mesh();
        Peer &a = peer('a'), &b = peer('b'), &c = peer('c');
        QObject guard;
        QList<Invitation> got;
        QStringList names, senders;
        connect(b.node.get(), &GroupNode::directInvitation, &guard,
                [&](const QString &from, const QString &, const Invitation &inv, const QString &name) {
                    senders << from;
                    got << inv;
                    names << name;
                });
        // A invite B à une autre conversation, par leur connexion
        const Invitation other{Crypto::randomBytes(16), a.onion, a.nick, a.store->identityKey(), QString(64, 'e')};
        QVERIFY(a.node->canInviteDirectly(b.onion));
        QVERIFY(a.node->inviteDirectly(b.onion, other, "Entre nous"));
        QTRY_COMPARE(got.size(), 1);
        QCOMPARE(senders.first(), a.onion);
        QCOMPARE(got.first().secret, other.secret);
        QCOMPARE(got.first().groupId, other.groupId);
        QCOMPARE(names.first(), QString("Entre nous"));

        // Personne ne transmet l'invitation d'un autre, ni ne se fait passer pour lui
        QVERIFY(!c.node->inviteDirectly(b.onion, other, {}));
        c.node->sendTo(b.onion, {{"t", "conv-invite"}, {"c", other.toCode()}});
        const Invitation stolenKey{Crypto::randomBytes(16), c.onion, c.nick, a.store->identityKey(), {}};
        c.node->sendTo(b.onion, {{"t", "conv-invite"}, {"c", stolenKey.toCode()}});
        // Ni le code de la conversation où l'on est déjà
        a.node->sendTo(b.onion, {{"t", "conv-invite"}, {"c", invitation(a).toCode()}});
        QTest::qWait(200);
        QCOMPARE(got.size(), 1);
    }

    void conversationsShareOneListener()
    {
        // P et Q partagent deux conversations, X et Y ; R n'est que dans Y. Une seule écoute par personne.
        struct Person
        {
            QString onion, nick;
            std::unique_ptr<QSettings> settings;
            std::unique_ptr<Switchboard> board;
            std::map<char, std::unique_ptr<GroupStore>> stores;
            std::map<char, std::unique_ptr<GroupNode>> nodes;
            std::map<char, QStringList> texts; // messages affichés, par conversation
        };
        std::map<char, Person> people;
        auto person = [&](char name) -> Person & {
            Person &p = people[name];
            if (p.board)
                return p;
            p.onion = QString(56, QLatin1Char(name)) + ".onion";
            p.nick = QStringLiteral("Personne %1").arg(name);
            p.settings = std::make_unique<QSettings>(m_dir->filePath(QStringLiteral("p%1.ini").arg(name)),
                                                     QSettings::IniFormat);
            GroupStore(*p.settings).ensureIdentity();
            GroupStore(*p.settings).setNickname(p.nick);
            p.board = std::make_unique<Switchboard>();
            [&] { QVERIFY(p.board->listen()); }();
            return p;
        };
        auto node = [&](char who, char conv) -> GroupNode & {
            Person &p = person(who);
            auto &n = p.nodes[conv];
            if (n)
                return *n;
            const QString id = QString(16, QLatin1Char(conv == 'x' ? '1' : '2'));
            p.stores[conv] = std::make_unique<GroupStore>(*p.settings, id);
            n = std::make_unique<GroupNode>(*p.stores[conv],
                                            m_dir->filePath(QStringLiteral("p%1-%2.jsonl").arg(who).arg(conv)),
                                            fastTiming());
            n->setFilesDir(m_dir->filePath(QStringLiteral("p%1-%2-fichiers").arg(who).arg(conv)));
            n->setDialer([&people](const QString &onion) {
                auto *s = new QTcpSocket;
                s->setProxy(QNetworkProxy::NoProxy);
                quint16 port = 1;
                for (auto &[name, other] : people)
                    if (other.onion == onion)
                        port = other.board->port();
                s->connectToHost(QHostAddress::LocalHost, port);
                return s;
            });
            QStringList *texts = &p.texts[conv];
            connect(n.get(), &GroupNode::messageAdded, this,
                    [texts](const QJsonObject &m, bool, GroupNode::Trust) { *texts << m.value("x").toString(); });
            p.board->add(n.get());
            return *n;
        };
        auto inv = [&](char who, char conv) -> Invitation {
            Person &p = person(who);
            return {p.stores[conv]->secret(), p.onion, p.nick, p.stores[conv]->identityKey(), node(who, conv).groupId()};
        };

        Person &pp = person('p'), &q = person('q'), &r = person('r');
        for (char conv : {'x', 'y'}) {
            node('p', conv).createGroup();
            node('p', conv).setOnline(pp.onion);
        }
        QCOMPARE(node('q', 'x').join(inv('p', 'x')), GroupNode::JoinResult::Joined);
        QCOMPARE(node('q', 'y').join(inv('p', 'y')), GroupNode::JoinResult::Joined);
        QCOMPARE(node('r', 'y').join(inv('p', 'y')), GroupNode::JoinResult::Joined);
        for (char conv : {'x', 'y'})
            node('q', conv).setOnline(q.onion);
        node('r', 'y').setOnline(r.onion);
        // Chaque conversation a ses propres connexions, toutes arrivées par la même écoute
        QTRY_VERIFY(node('p', 'x').isConnected(q.onion) && node('q', 'x').isConnected(pp.onion));
        QTRY_VERIFY(node('p', 'y').isConnected(q.onion) && node('q', 'y').isConnected(pp.onion));
        QTRY_VERIFY(node('p', 'y').isConnected(r.onion) && node('r', 'y').isConnected(pp.onion));
        QTRY_VERIFY(node('q', 'y').isConnected(r.onion)); // découverte par la liste des membres de P
        QVERIFY(!node('p', 'x').isConnected(r.onion));
        QVERIFY(!pp.stores['x']->isMember(r.onion));
        QVERIFY(node('p', 'x').groupId() != node('p', 'y').groupId());

        node('q', 'x').sendMessage("dans X");
        node('r', 'y').sendMessage("dans Y");
        QTRY_VERIFY(pp.texts['x'].contains("dans X"));
        QTRY_VERIFY(pp.texts['y'].contains("dans Y"));
        QTRY_VERIFY(q.texts['y'].contains("dans Y"));
        QTest::qWait(100);
        QVERIFY(!pp.texts['y'].contains("dans X") && !q.texts['x'].contains("dans Y"));
        QVERIFY(!r.texts['y'].contains("dans X"));

        // Quitter X ne touche pas à Y
        node('q', 'x').leaveGroup();
        QTRY_VERIFY(!node('p', 'x').isConnected(q.onion));
        QVERIFY(node('p', 'y').isConnected(q.onion));
        node('p', 'y').sendMessage("toujours là");
        QTRY_VERIFY(q.texts['y'].contains("toujours là"));

        // Les nœuds d'abord (les connexions adoptées sont à eux), puis les écoutes
        for (auto &[name, p] : people)
            p.nodes.clear();
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
