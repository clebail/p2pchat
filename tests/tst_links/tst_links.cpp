#include "links.h"

#include <QtTest>

namespace {
QStringList urls(const QString &text)
{
    QStringList out;
    for (const Links::Span &s : Links::find(text)) {
        out << QStringLiteral("%1:%2").arg(text.mid(s.start, s.length), s.url);
    }
    return out;
}
} // namespace

class TestLinks : public QObject
{
    Q_OBJECT

private slots:
    void find_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QStringList>("expected"); // « texte du lien:adresse ouverte »
        QTest::newRow("simple") << "voir https://exemple.fr" << QStringList{"https://exemple.fr:https://exemple.fr"};
        QTest::newRow("http") << "http://a.b/c?d=1&e=2" << QStringList{"http://a.b/c?d=1&e=2:http://a.b/c?d=1&e=2"};
        QTest::newRow("www complété") << "www.exemple.fr" << QStringList{"www.exemple.fr:https://www.exemple.fr"};
        QTest::newRow("ponctuation finale") << "C'est là : https://exemple.fr/page." << QStringList{"https://exemple.fr/page:https://exemple.fr/page"};
        QTest::newRow("entre parenthèses") << "(https://exemple.fr)" << QStringList{"https://exemple.fr:https://exemple.fr"};
        QTest::newRow("parenthèse du lien") << "https://fr.wikipedia.org/wiki/Tor_(réseau)"
                                            << QStringList{"https://fr.wikipedia.org/wiki/Tor_(réseau):https://fr.wikipedia.org/wiki/Tor_(réseau)"};
        QTest::newRow("guillemets français") << "«https://exemple.fr»" << QStringList{"https://exemple.fr:https://exemple.fr"};
        QTest::newRow("deux liens") << "https://a.fr et https://b.fr" << QStringList{"https://a.fr:https://a.fr", "https://b.fr:https://b.fr"};
        QTest::newRow("majuscules") << "HTTPS://EXEMPLE.FR" << QStringList{"HTTPS://EXEMPLE.FR:HTTPS://EXEMPLE.FR"};
        QTest::newRow("préfixe seul") << "https:// et www." << QStringList();
        QTest::newRow("pas un lien") << "fichier.txt, ftp://x.fr, mailto:a@b.fr" << QStringList();
        QTest::newRow("collé à un mot") << "voirhttps://exemple.fr" << QStringList();
    }

    void find()
    {
        QFETCH(QString, text);
        QFETCH(QStringList, expected);
        QCOMPARE(urls(text), expected);
    }

    void positions()
    {
        const QList<Links::Span> spans = Links::find("ab https://x.fr cd");
        QCOMPARE(spans.size(), 1);
        QCOMPARE(spans.first().start, 3);
        QCOMPARE(spans.first().length, 12);
    }
};

QTEST_GUILESS_MAIN(TestLinks)
#include "tst_links.moc"
