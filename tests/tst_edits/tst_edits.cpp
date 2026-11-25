#include "crypto.h"
#include "edits.h"

#include <QtTest>

namespace {
QString onion(char c)
{
    return QString(56, c) + ".onion";
}
} // namespace

class TestEdits : public QObject
{
    Q_OBJECT

private:
    const QByteArray m_alice = Ed25519::generateSeed(), m_bob = Ed25519::generateSeed();

private slots:
    void signedByItsAuthor()
    {
        const QJsonObject e = Edits::create(m_alice, onion('a'), "message-1", "Nouveau texte", false, 1000);
        QVERIFY(Edits::isEdit(e));
        QCOMPARE(Edits::target(e), QString("message-1"));
        QVERIFY(Edits::verify(e, Ed25519::publicKey(m_alice)));
        QVERIFY(!Edits::verify(e, Ed25519::publicKey(m_bob)));
        QJsonObject changed = e;
        changed["ed"] = "Autre texte";
        QVERIFY(!Edits::verify(changed, Ed25519::publicKey(m_alice)));
        changed = e;
        changed["m"] = "message-2";
        QVERIFY(!Edits::verify(changed, Ed25519::publicKey(m_alice)));
        changed = e;
        changed["del"] = true;
        QVERIFY(!Edits::verify(changed, Ed25519::publicKey(m_alice)));
    }

    void deletionCarriesNoText()
    {
        const QJsonObject d = Edits::create(m_alice, onion('a'), "message-1", "ignoré", true, 1000);
        QVERIFY(d.value("ed").toString().isEmpty());
        QVERIFY(Edits::verify(d, Ed25519::publicKey(m_alice)));
        // Une modification vide n'a pas de sens (c'est une suppression)
        QVERIFY(!Edits::verify(Edits::create(m_alice, onion('a'), "message-1", "   ", false, 1000),
                               Ed25519::publicKey(m_alice)));
        QCOMPARE(Edits::create(m_alice, onion('a'), "m", QString(5000, 'x'), false, 1).value("ed").toString().size(),
                 Edits::MaxTextLength);
    }

    void latestWinsWhateverTheOrder_data()
    {
        QTest::addColumn<bool>("reversed");
        QTest::newRow("dans l'ordre") << false;
        QTest::newRow("à l'envers") << true;
    }

    void latestWinsWhateverTheOrder()
    {
        QFETCH(bool, reversed);
        QList<QJsonObject> records{Edits::create(m_alice, onion('a'), "m", "première", false, 1000),
                                   Edits::create(m_alice, onion('a'), "m", "seconde", false, 2000)};
        if (reversed)
            std::reverse(records.begin(), records.end());
        Edits::State state;
        for (const QJsonObject &r : records)
            state.apply(r);
        QCOMPARE(state.latest("m", {onion('a')})->text, QString("seconde"));
        QVERIFY(!state.latest("m", {onion('b')}));   // par auteur : celle d'un autre ne compte pas
        QVERIFY(!state.latest("autre", {onion('a')}));
    }

    void moderatorAmongAllowedAuthors()
    {
        Edits::State state;
        state.apply(Edits::create(m_alice, onion('a'), "m", "par l'auteur", false, 2000));
        state.apply(Edits::create(m_bob, onion('b'), "m", {}, true, 1000)); // B, administrateur, supprime
        // B pas reconnu comme administrateur : seule la modification de l'auteur compte
        QCOMPARE(state.latest("m", {onion('a')})->text, QString("par l'auteur"));
        // B administrateur : sa suppression l'emporte, même plus ancienne
        const auto l = state.latest("m", {onion('a'), onion('b')});
        QVERIFY(l->deleted);
        QCOMPARE(l->author, onion('b'));
    }

    void deletionIsFinal_data()
    {
        QTest::addColumn<bool>("reversed");
        QTest::newRow("suppression puis modification plus récente") << false;
        QTest::newRow("modification plus récente arrivée d'abord") << true;
    }

    void deletionIsFinal()
    {
        QFETCH(bool, reversed);
        QList<QJsonObject> records{Edits::create(m_alice, onion('a'), "m", {}, true, 1000),
                                   Edits::create(m_alice, onion('a'), "m", "revenu ?", false, 2000)};
        if (reversed)
            std::reverse(records.begin(), records.end());
        Edits::State state;
        for (const QJsonObject &r : records)
            state.apply(r);
        QVERIFY(state.latest("m", {onion('a')})->deleted);
    }
};

QTEST_GUILESS_MAIN(TestEdits)
#include "tst_edits.moc"
