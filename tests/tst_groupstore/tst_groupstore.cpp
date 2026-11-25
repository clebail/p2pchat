#include "crypto.h"
#include "groupstore.h"

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
};

QTEST_GUILESS_MAIN(TestGroupStore)
#include "tst_groupstore.moc"
