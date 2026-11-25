#include "mentions.h"

#include <QtTest>

using namespace Mentions;

namespace {
const QStringList Names{"Alice", "Bob", "Jean Paul", "Jean", Everyone};

QStringList found(const QString &text)
{
    QStringList out;
    for (const Span &s : find(text, Names))
        out << QStringLiteral("%1:%2:%3").arg(s.start).arg(s.length).arg(s.name);
    return out;
}
} // namespace

class TestMentions : public QObject
{
    Q_OBJECT

private slots:
    void find_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QStringList>("expected");
        QTest::newRow("début") << "@Alice salut" << QStringList{"0:6:Alice"};
        QTest::newRow("milieu, ponctuation") << "Salut @Bob, ça va ?" << QStringList{"6:4:Bob"};
        QTest::newRow("casse indifférente") << "@alice" << QStringList{"0:6:Alice"};
        QTest::newRow("plusieurs") << "@Alice et @Bob" << QStringList{"0:6:Alice", "10:4:Bob"};
        QTest::newRow("pseudo avec espace") << "@Jean Paul tu viens ?" << QStringList{"0:10:Jean Paul"};
        QTest::newRow("préfixe d'un pseudo plus long") << "@Jean tu viens ?" << QStringList{"0:5:Jean"};
        QTest::newRow("tous") << "@tous réunion" << QStringList{"0:5:tous"};
        QTest::newRow("pseudo plus long inconnu") << "@Alicia" << QStringList();
        QTest::newRow("adresse mail") << "écris à bob@Alice.fr" << QStringList();
        QTest::newRow("inconnu") << "@Charlie" << QStringList();
        QTest::newRow("@ seul") << "@ Alice @" << QStringList();
        QTest::newRow("double @") << "@@Bob" << QStringList{"1:4:Bob"};
        QTest::newRow("après saut de ligne") << "ok\n@Bob" << QStringList{"3:4:Bob"};
    }

    void find()
    {
        QFETCH(QString, text);
        QFETCH(QStringList, expected);
        QCOMPARE(found(text), expected);
    }

    void emptyNamesAreIgnored()
    {
        QVERIFY(Mentions::find("@ tout", {"", " "}).isEmpty());
    }

    void mentionsNick()
    {
        QVERIFY(mentions(Mentions::find("@bob ici", Names), "Bob"));
        QVERIFY(!mentions(Mentions::find("@Alice ici", Names), "Bob"));
        QVERIFY(mentions(Mentions::find("@tous ici", Names), "Bob")); // tout le monde
        QVERIFY(!mentions({}, "Bob"));
        QVERIFY(!mentions(Mentions::find("@Alice", Names), QString()));
    }

    void tokenAt_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<int>("cursor");
        QTest::addColumn<int>("start"); // -1 : pas de saisie de mention en cours
        QTest::addColumn<QString>("prefix");
        QTest::newRow("@ seul") << "@" << 1 << 0 << "";
        QTest::newRow("début de pseudo") << "salut @Al" << 9 << 6 << "Al";
        QTest::newRow("curseur au milieu") << "salut @Alice !" << 9 << 6 << "Al";
        QTest::newRow("pseudo avec espace") << "@Jean P" << 7 << 0 << "Jean P";
        QTest::newRow("pas de @") << "bonjour" << 7 << -1 << "";
        QTest::newRow("adresse mail") << "bob@al" << 6 << -1 << "";
        QTest::newRow("trop loin") << "@" + QString(50, 'x') << 51 << -1 << "";
        QTest::newRow("autre ligne") << "@Al\nsuite" << 9 << -1 << "";
        QTest::newRow("curseur avant le @") << "a @Bob" << 1 << -1 << "";
    }

    void tokenAt()
    {
        QFETCH(QString, text);
        QFETCH(int, cursor);
        QFETCH(int, start);
        QFETCH(QString, prefix);
        const auto token = Mentions::tokenAt(text, cursor);
        QCOMPARE(token.has_value(), start >= 0);
        if (token) {
            QCOMPARE(token->start, start);
            QCOMPARE(token->prefix, prefix);
        }
    }

    void candidates()
    {
        QCOMPARE(Mentions::candidates("j", Names), QStringList({"Jean", "Jean Paul"}));
        QCOMPARE(Mentions::candidates("", {"bob", "Alice", "", "BOB"}), QStringList({"Alice", "bob"}));
        QCOMPARE(Mentions::candidates("al", Names), QStringList({"Alice"}));
        QVERIFY(Mentions::candidates("z", Names).isEmpty());
    }

    void complete()
    {
        const QString text = "salut @al ça va";
        const auto token = Mentions::tokenAt(text, 9);
        QVERIFY(token);
        int cursor = 0;
        QCOMPARE(Mentions::complete(text, 9, *token, "Alice", &cursor), QString("salut @Alice  ça va"));
        QCOMPARE(cursor, 13);
    }
};

QTEST_GUILESS_MAIN(TestMentions)
#include "tst_mentions.moc"
