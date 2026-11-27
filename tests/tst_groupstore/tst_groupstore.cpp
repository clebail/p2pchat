#include "conversations.h"
#include "crypto.h"
#include "groupstore.h"
#include "person.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

namespace {
QString onion(char c)
{
    return QString(56, c) + ".onion";
}
} // namespace

class TestGroupStore : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    std::unique_ptr<QSettings> m_settings;
    std::unique_ptr<GroupStore> m_store;
    int m_counter = 0;

    // Rouvre le même fichier de réglages : vérifie que tout est bien persistant
    void reopen()
    {
        const QString path = m_settings->fileName();
        m_store.reset();
        m_settings->sync();
        m_settings = std::make_unique<QSettings>(path, QSettings::IniFormat);
        m_store = std::make_unique<GroupStore>(*m_settings);
    }

private slots:
    void init()
    {
        QVERIFY(m_dir.isValid());
        m_settings = std::make_unique<QSettings>(m_dir.filePath(QString("s%1.ini").arg(++m_counter)),
                                                 QSettings::IniFormat);
        m_store = std::make_unique<GroupStore>(*m_settings);
    }

    void cleanup()
    {
        m_store.reset();
        m_settings.reset();
    }

    void nicknameIsTruncated()
    {
        m_store->setNickname(QString(60, 'x'));
        reopen();
        QCOMPARE(m_store->nickname(), QString(40, 'x'));
    }

    void secretAndLeave()
    {
        QVERIFY(!m_store->inGroup());
        m_store->setSecret("0123456789abcdef");
        m_store->mergeMember(onion('a'), "Alice", {});
        m_store->setShareHistoryUntil(1000);
        reopen();
        QVERIFY(m_store->inGroup());
        QCOMPARE(m_store->secret(), QByteArray("0123456789abcdef"));

        m_store->pinKey(onion('a'), QByteArray(32, 'k'), 1);
        m_store->setClearedBefore(5);
        m_store->leaveGroup();
        QVERIFY(!m_store->inGroup());
        QVERIFY(m_store->members().isEmpty());
        QCOMPARE(m_store->shareHistoryUntil(), qint64(0));
        // Conservés : trousseau (vérification de l'historique) et date d'effacement
        QCOMPARE(m_store->pinnedKey(onion('a')), QByteArray(32, 'k'));
        QCOMPARE(m_store->clearedBefore(), qint64(5));
    }

    void identityIsStable()
    {
        QVERIFY(m_store->identitySeed().isEmpty());
        m_store->ensureIdentity();
        const QByteArray seed = m_store->identitySeed();
        QCOMPARE(seed.size(), 32);
        QCOMPARE(m_store->identityKey(), Ed25519::publicKey(seed));
        reopen();
        m_store->ensureIdentity(); // ne régénère pas une identité existante
        QCOMPARE(m_store->identitySeed(), seed);
    }

    // Postes liés : une clé de personne, distincte de celle du poste, créée avec l'identité
    void personIdentity()
    {
        m_store->ensureIdentity();
        const QByteArray person = m_store->personSeed();
        QCOMPARE(person.size(), 32);
        QVERIFY(person != m_store->identitySeed());
        QCOMPARE(m_store->personKey(), Ed25519::publicKey(person));
        QVERIFY(m_store->personSince() > 0);
        reopen();
        m_store->ensureIdentity();
        QCOMPARE(m_store->personSeed(), person);
        // Poste lié : la personne d'un autre poste
        const QByteArray other = Ed25519::generateSeed();
        m_store->setPersonSeed(other, 1234);
        QCOMPARE(m_store->personKey(), Ed25519::publicKey(other));
        QCOMPARE(m_store->personSince(), qint64(1234));
        // Certificat montré : définitif
        QVERIFY(!m_store->personAnnounced());
        m_store->setPersonAnnounced();
        reopen();
        QVERIFY(m_store->personAnnounced());
    }

    void devicesAndVerification()
    {
        m_store->ensureIdentity();
        const QByteArray deviceA = Ed25519::generateSeed(), deviceB = Ed25519::generateSeed(),
                         deviceC = Ed25519::generateSeed(), alice = Ed25519::generateSeed();
        for (const auto &[o, seed] : {std::pair{onion('a'), deviceA}, std::pair{onion('b'), deviceB},
                                      std::pair{onion('c'), deviceC}})
            QVERIFY(m_store->pinKey(o, Ed25519::publicKey(seed), 1));
        QVERIFY(m_store->personOf(onion('a')).isEmpty()); // pas encore de certificat
        // Alice a deux postes (a et b) ; c est un autre de nos postes
        m_store->setCertificate(onion('a'), Person::createCertificate(alice, deviceA, onion('a'), 1));
        m_store->setCertificate(onion('b'), Person::createCertificate(alice, deviceB, onion('b'), 1));
        m_store->setCertificate(onion('c'), Person::createCertificate(m_store->personSeed(), deviceC, onion('c'), 1));
        QCOMPARE(m_store->personOf(onion('a')), Ed25519::publicKey(alice));
        QCOMPARE(m_store->personOf(onion('b')), Ed25519::publicKey(alice));
        QVERIFY(!m_store->isMyDevice(onion('a')));
        QVERIFY(m_store->isMyDevice(onion('c')));
        QVERIFY(!m_store->isMyDevice(onion('d')));
        // Vérifier un poste d'Alice, c'est vérifier Alice : tous ses postes
        QVERIFY(!m_store->isVerified(onion('b')));
        m_store->setVerified(onion('a'), true);
        QVERIFY(m_store->isVerified(onion('a')));
        QVERIFY(m_store->isVerified(onion('b')));
        reopen();
        QVERIFY(m_store->isVerified(onion('b')));
        m_store->setVerified(onion('b'), false);
        QVERIFY(!m_store->isVerified(onion('a')));
    }

    void mergeMember()
    {
        QVERIFY(m_store->mergeMember(onion('a'), "Alice", onion('z')));
        QVERIFY(!m_store->mergeMember(onion('a'), "Alice", onion('z')));   // déjà connu
        QVERIFY(!m_store->mergeMember(onion('a'), "Alicia", onion('z')));  // pseudo mis à jour
        QVERIFY(!m_store->mergeMember(onion('a'), QString(), onion('z'))); // pseudo vide : ignoré
        QVERIFY(!m_store->mergeMember(onion('z'), "Moi", onion('z')));     // soi-même
        QVERIFY(!m_store->mergeMember("pas-un-onion", "X", onion('z')));
        QVERIFY(m_store->mergeMember(onion('b'), QString(50, 'b'), onion('z')));
        reopen();

        const QList<Member> list = m_store->members();
        QCOMPARE(list.size(), 2);
        QCOMPARE(list[0].onion, onion('a'));
        QCOMPARE(list[0].nick, QString("Alicia"));
        QCOMPARE(list[1].nick.size(), 40);
        QVERIFY(m_store->isMember(onion('b')));
        QVERIFY(!m_store->isMember(onion('c')));
        QCOMPARE(m_store->nickOf(onion('a')), QString("Alicia"));
        QCOMPARE(m_store->nickOf(onion('c')), QString());
    }

    void memberLimit()
    {
        const QString base32 = "abcdefghijklmnopqrstuvwxyz234567";
        for (int i = 0; i < GroupStore::MaxMembers; ++i)
            QVERIFY(m_store->mergeMember(QString(55, 'a') + base32[i] + ".onion", "m", {}));
        QVERIFY(!m_store->mergeMember(onion('b'), "de trop", {}));
        QCOMPARE(m_store->members().size(), GroupStore::MaxMembers);
    }

    void membersIgnoreInvalidEntries()
    {
        const QJsonArray arr{QJsonObject{{"o", "invalide"}, {"n", "X"}}, QJsonObject{{"o", onion('a')}, {"n", "A"}}, 3};
        m_settings->setValue("group/members", QJsonDocument(arr).toJson());
        QCOMPARE(m_store->members().size(), 1);
        m_settings->setValue("group/members", "pas du JSON");
        QVERIFY(m_store->members().isEmpty());
    }

    void keyPinning()
    {
        const QByteArray k1(32, '1'), k2(32, '2');
        QVERIFY(!m_store->pinKey(onion('a'), "court", 100));
        QVERIFY(!m_store->pinKey("invalide", k1, 100));
        QVERIFY(m_store->pinnedKey(onion('a')).isEmpty());

        QVERIFY(m_store->pinKey(onion('a'), k1, 100));
        QVERIFY(m_store->pinKey(onion('a'), k1, 200)); // même clé : accepté
        QVERIFY(!m_store->pinKey(onion('a'), k2, 300)); // autre clé : refusé
        reopen();
        QCOMPARE(m_store->pinnedKey(onion('a')), k1);
        QCOMPARE(m_store->firstSeen(onion('a')), qint64(100)); // date de la première rencontre
        QCOMPARE(m_store->firstSeen(onion('b')), qint64(0));
    }

    void verification()
    {
        const QString a = onion('a');
        QVERIFY(!m_store->isVerified(a));
        m_store->setVerified(a, true); // clé inconnue : rien à vérifier
        QVERIFY(!m_store->isVerified(a));
        QVERIFY(m_store->pinKey(a, QByteArray(32, '1'), 1));
        m_store->setVerified(a, true);
        reopen();
        QVERIFY(m_store->isVerified(a));
        QVERIFY(!m_store->isVerified(onion('b')));
        m_store->setVerified(a, false);
        QVERIFY(!m_store->isVerified(a));

        // La vérification porte sur la clé : si le trousseau changeait, elle ne vaudrait plus
        m_store->setVerified(a, true);
        m_settings->setValue("keyring", QJsonDocument(QJsonObject{{a, "AAAA"}}).toJson()); // autre clé
        QVERIFY(!m_store->isVerified(a));
    }

    void previousSecrets()
    {
        QVERIFY(m_store->acceptedSecrets(0).isEmpty());
        m_store->setSecret("s1");
        QCOMPARE(m_store->acceptedSecrets(0), QList<QByteArray>({"s1"}));
        // Renouvellement : l'ancien reste accepté jusqu'à sa date
        m_store->retireSecret("s1", 1000);
        m_store->setSecret("s2");
        m_store->retireSecret("s2", 2000);
        m_store->setSecret("s3");
        reopen();
        QCOMPARE(m_store->acceptedSecrets(500), QList<QByteArray>({"s3", "s2", "s1"})); // du plus récent
        QCOMPARE(m_store->acceptedSecrets(1500), QList<QByteArray>({"s3", "s2"}));
        QCOMPARE(m_store->acceptedSecrets(2500), QList<QByteArray>({"s3"}));
        for (int i = 0; i < 5; ++i)
            m_store->retireSecret(QByteArray::number(i), 10000);
        QCOMPARE(m_store->acceptedSecrets(0).size(), 4); // l'actuel et trois précédents au plus
        m_store->leaveGroup();
        QVERIFY(m_store->acceptedSecrets(0).isEmpty());
    }

    void governanceRecordsAndGroupId()
    {
        QVERIFY(m_store->governanceRecords().isEmpty());
        m_store->appendGovernanceRecord({{"seq", 0}});
        m_store->appendGovernanceRecord({{"seq", 1}});
        m_store->setExpectedGroupId(QString(64, 'a'));
        reopen();
        QCOMPARE(m_store->governanceRecords().size(), 2);
        QCOMPARE(m_store->governanceRecords().at(1).value("seq").toInt(), 1);
        QCOMPARE(m_store->expectedGroupId(), QString(64, 'a'));
        m_store->leaveGroup(); // la gouvernance appartient au groupe quitté
        QVERIFY(m_store->governanceRecords().isEmpty());
        QVERIFY(m_store->expectedGroupId().isEmpty());
    }

    void sharedHistoryMakesFirstSeenZero()
    {
        m_store->setShareHistoryUntil(1000);
        QVERIFY(m_store->pinKey(onion('a'), QByteArray(32, '1'), 999)); // pendant le partage
        QVERIFY(m_store->pinKey(onion('b'), QByteArray(32, '2'), 1000)); // expiré
        QCOMPARE(m_store->firstSeen(onion('a')), qint64(0));
        QCOMPARE(m_store->firstSeen(onion('b')), qint64(1000));
        m_store->setShareHistoryUntil(0);
        QVERIFY(!m_settings->contains("group/shareHistoryUntil"));
    }

    // Synchronisation entre nos postes : date du pseudo, postes nommés administrateurs, conversations
    void deviceSyncState()
    {
        m_store->setNickname("Alice", 1234);
        QCOMPARE(m_store->nicknameTime(), qint64(1234));
        const qint64 before = QDateTime::currentMSecsSinceEpoch();
        m_store->setNickname("Alice B");
        QVERIFY(m_store->nicknameTime() >= before); // maintenant, par défaut

        m_store->setSecret(QByteArray(16, 's'));
        QVERIFY(m_store->promotedDevices().isEmpty());
        m_store->setPromotedDevices({onion('e')});
        reopen();
        QCOMPARE(m_store->promotedDevices(), QStringList({onion('e')}));
        m_store->leaveGroup(); // propre à la conversation
        QVERIFY(m_store->promotedDevices().isEmpty());

        const QJsonObject records{{QString(64, '1'), QJsonObject{{"in", true}, {"ts", 5.0}}}};
        m_store->setSyncedConversations(records);
        reopen();
        QCOMPARE(m_store->syncedConversations(), records);
    }

    // Un autre de nos postes (liaison) : les 30 derniers jours, même rencontré avant
    void historySharedWithMyDevice()
    {
        QVERIFY(m_store->pinKey(onion('a'), QByteArray(32, '1'), 5000));
        QCOMPARE(m_store->firstSeen(onion('a')), qint64(5000));
        m_store->shareHistoryWith(onion('a'));
        QCOMPARE(m_store->firstSeen(onion('a')), qint64(0));
        QVERIFY(m_store->pinKey(onion('a'), QByteArray(32, '1'), 6000)); // reconnu ensuite : pas réécrit
        QCOMPARE(m_store->firstSeen(onion('a')), qint64(0));
        m_store->shareHistoryWith("pas-une-adresse");
        QCOMPARE(m_store->firstSeen("pas-une-adresse"), qint64(0));
    }

    void migratesTwoPartyConfig()
    {
        m_settings->setValue("contact/secret", QByteArray("ancien secret"));
        m_settings->setValue("contact/onion", onion('a'));
        m_settings->setValue("contact/nick", "Alice");
        m_store->migrateLegacy();
        QCOMPARE(m_store->secret(), QByteArray("ancien secret"));
        QCOMPARE(m_store->members().size(), 1);
        QCOMPARE(m_store->members().first().nick, QString("Alice"));
        QVERIFY(!m_settings->contains("contact/secret"));

        // Sans ancienne configuration : rien ne change
        m_store->setSecret("nouveau");
        m_store->migrateLegacy();
        QCOMPARE(m_store->secret(), QByteArray("nouveau"));
    }

    // ---------------------------------------------------------------- Plusieurs conversations

    void conversationsAreSeparate()
    {
        Conversations convs(*m_settings, m_dir.filePath(QString("data%1").arg(m_counter)));
        const QString x = convs.create(), y = convs.create();
        QCOMPARE(convs.ids(), QStringList({x, y}));
        QVERIFY(x != y && x.size() == 16);
        QVERIFY(QDir(convs.dir(x)).exists());
        GroupStore gx(*m_settings, x), gy(*m_settings, y);
        gx.setNickname("Moi");
        gx.ensureIdentity();
        gx.setSecret("secret de X");
        gx.setMembers({{onion('a'), "Alice"}});
        gx.setClearedBefore(1234);
        // Identité, pseudo, trousseau : communs ; le reste est propre à chaque conversation
        QCOMPARE(gy.nickname(), QString("Moi"));
        QCOMPARE(gy.identityKey(), gx.identityKey());
        QVERIFY(!gy.inGroup());
        QVERIFY(gy.members().isEmpty());
        QCOMPARE(gy.clearedBefore(), 0);
        QVERIFY(gx.pinKey(onion('a'), QByteArray(32, 'k'), 1000));
        QCOMPARE(gy.pinnedKey(onion('a')), QByteArray(32, 'k'));
        QVERIFY(!gy.pinKey(onion('a'), QByteArray(32, 'z'), 2000)); // une autre clé : refusée partout
        // Première rencontre notée par conversation, même pour une clé déjà connue
        QCOMPARE(gx.firstSeen(onion('a')), 1000);
        QVERIFY(gy.pinKey(onion('a'), QByteArray(32, 'k'), 5000));
        QCOMPARE(gy.firstSeen(onion('a')), 5000);
        QVERIFY(gx.pinKey(onion('a'), QByteArray(32, 'k'), 9000));
        QCOMPARE(gx.firstSeen(onion('a')), 1000); // pas réécrite
        gy.leaveGroup();
        QCOMPARE(gx.secret(), QByteArray("secret de X"));

        QFile f(convs.historyPath(y));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
        convs.remove(y);
        QCOMPARE(convs.ids(), QStringList({x}));
        QVERIFY(!QDir(convs.dir(y)).exists());
        QVERIFY(gx.inGroup());
    }

    void migratesSingleGroupLayout()
    {
        const QString data = m_dir.filePath(QString("data%1").arg(m_counter));
        QDir().mkpath(data + "/fichiers");
        QFile h(data + "/history.jsonl");
        QVERIFY(h.open(QIODevice::WriteOnly));
        h.write("{}\n");
        h.close();
        QVERIFY(QFile(data + "/fichiers/x").open(QIODevice::WriteOnly));
        m_store->setSecret("secret");
        m_store->setMembers({{onion('a'), "Alice"}, {onion('b'), "Bob"}});
        m_store->appendGovernanceRecord({{"seq", 0}});
        m_store->setClearedBefore(42);
        m_store->pinKey(onion('a'), QByteArray(32, 'a'), 777);
        m_store->pinKey(onion('b'), QByteArray(32, 'b'), 888);
        // Clé mémorisée avant la notion de première rencontre
        QJsonObject seen = QJsonDocument::fromJson(m_settings->value("firstSeen").toByteArray()).object();
        seen.remove(onion('b'));
        m_settings->setValue("firstSeen", QJsonDocument(seen).toJson());

        Conversations convs(*m_settings, data);
        const QString id = convs.migrate();
        QCOMPARE(convs.ids(), QStringList({id}));
        QVERIFY(!m_settings->contains("group/secret"));
        QVERIFY(!m_settings->contains("firstSeen"));
        GroupStore g(*m_settings, id);
        QCOMPARE(g.secret(), QByteArray("secret"));
        QCOMPARE(g.members().size(), 2);
        QCOMPARE(g.governanceRecords().size(), 1);
        QCOMPARE(g.clearedBefore(), 42);
        QCOMPARE(g.firstSeen(onion('a')), 777);
        QCOMPARE(g.firstSeen(onion('b')), 0);
        QVERIFY(g.pinKey(onion('b'), QByteArray(32, 'b'), 999));
        QCOMPARE(g.firstSeen(onion('b')), 0); // reste « avant cette notion »
        QCOMPARE(g.pinnedKey(onion('a')), QByteArray(32, 'a')); // trousseau resté commun
        QVERIFY(QFile::exists(convs.historyPath(id)));
        QVERIFY(!QFile::exists(data + "/history.jsonl"));
        QVERIFY(QFile::exists(convs.dir(id) + "/fichiers/x"));

        // Une seule fois
        QVERIFY(convs.migrate().isEmpty());
        QCOMPARE(convs.ids().size(), 1);
    }

    void migratesNothingOnFreshInstall()
    {
        m_store->setNickname("Moi");
        m_store->ensureIdentity();
        Conversations convs(*m_settings, m_dir.filePath(QString("data%1").arg(m_counter)));
        QVERIFY(convs.migrate().isEmpty());
        QVERIFY(convs.ids().isEmpty());
    }

    void migratesTwoPartyConfigIntoConversation()
    {
        m_settings->setValue("contact/secret", QByteArray("ancien secret"));
        m_settings->setValue("contact/onion", onion('a'));
        Conversations convs(*m_settings, m_dir.filePath(QString("data%1").arg(m_counter)));
        const QString id = convs.migrate();
        QCOMPARE(GroupStore(*m_settings, id).secret(), QByteArray("ancien secret"));
        QCOMPARE(GroupStore(*m_settings, id).members().size(), 1);
    }
};

QTEST_GUILESS_MAIN(TestGroupStore)
#include "tst_groupstore.moc"
