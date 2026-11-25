#include "markup.h"

#include <QtTest>

namespace {
// « contenu|bloc » de chaque code, puis les délimiteurs masqués, pour comparer d'un coup d'œil
QString describe(const QString &text)
{
    const Markup::Result r = Markup::parse(text);
    QStringList parts;
    for (const Markup::Code &c : r.code)
        parts << text.mid(c.start, c.length) + (c.block ? "|bloc" : "");
    QStringList hidden;
    for (const auto &d : r.delimiters)
        hidden << text.mid(d.first, d.second);
    return parts.join(" ; ") + " / " + hidden.join("").replace('\n', "⏎");
}
} // namespace

class TestMarkup : public QObject
{
    Q_OBJECT

private slots:
    void parse_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("expected");
        QTest::newRow("rien") << "texte normal" << " / ";
        QTest::newRow("en ligne") << "lance `make -j8` puis" << "make -j8 / ``";
        QTest::newRow("deux en ligne") << "`a` et `b`" << "a ; b / ````";
        QTest::newRow("non fermé") << "un ` seul" << " / ";
        QTest::newRow("vide") << "``" << " / ";
        QTest::newRow("pas sur deux lignes") << "`a\nb`" << " / ";
        QTest::newRow("bloc") << "```\nls -l\ncd /\n```" << "ls -l\ncd /|bloc / ```⏎⏎```";
        QTest::newRow("bloc sur une ligne") << "```sudo reboot```" << "sudo reboot|bloc / ``````";
        QTest::newRow("bloc puis en ligne") << "```x```, puis `y`" << "x|bloc ; y / ````````";
        QTest::newRow("accent grave dans un bloc") << "```a`b```" << "a`b|bloc / ``````";
        QTest::newRow("bloc non fermé") << "```pas fini `ok`" << "ok / ``";
    }

    void emojiRuns_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QStringList>("expected");
        QTest::newRow("aucun") << "bonjour à tous" << QStringList();
        QTest::newRow("un") << QStringLiteral("ok 👍 merci") << QStringList{QStringLiteral("👍")};
        QTest::newRow("suite") << QStringLiteral("bravo 🎉🎉!") << QStringList{QStringLiteral("🎉🎉")};
        QTest::newRow("cœur avec sélecteur") << QStringLiteral("je ❤️ ça") << QStringList{QStringLiteral("❤️")};
        QTest::newRow("famille (ZWJ)") << QStringLiteral("👨‍👩‍👧 !") << QStringList{QStringLiteral("👨‍👩‍👧")};
        QTest::newRow("teinte de peau") << QStringLiteral("👍🏽") << QStringList{QStringLiteral("👍🏽")};
        QTest::newRow("drapeau") << QStringLiteral("🇫🇷 et 🇧🇪") << QStringList{QStringLiteral("🇫🇷"), QStringLiteral("🇧🇪")};
        QTest::newRow("liant seul") << QStringLiteral("a\u200Db") << QStringList();
    }

    void emojiRuns()
    {
        QFETCH(QString, text);
        QFETCH(QStringList, expected);
        QStringList found;
        for (const auto &run : Markup::emojiRuns(text))
            found << text.mid(run.first, run.second);
        QCOMPARE(found, expected);
    }

    void parse()
    {
        QFETCH(QString, text);
        QFETCH(QString, expected);
        QCOMPARE(describe(text), expected);
    }
};

QTEST_GUILESS_MAIN(TestMarkup)
#include "tst_markup.moc"
