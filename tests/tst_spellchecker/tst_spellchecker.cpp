#include "spellchecker.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

// Petits dictionnaires de test (dict/), dans trois encodages : le résultat ne dépend pas de ceux du système
class TestSpellChecker : public QObject
{
    Q_OBJECT

private:
    static QString dict(const char *name) { return QStringLiteral(DICT_DIR "/%1.dic").arg(QLatin1String(name)); }

private slots:
    void initTestCase()
    {
        if (!SpellChecker::supported())
            QSKIP("compilé sans Hunspell");
    }

    void withoutDictionaryEverythingIsCorrect()
    {
        SpellChecker s;
        QVERIFY(!s.isLoaded());
        QVERIFY(s.isCorrect("nimportekoi"));
        QVERIFY(s.misspelled("nimportekoi").isEmpty());
        QVERIFY(!s.load(DICT_DIR "/absent.dic"));
        QVERIFY(!s.isLoaded());
    }

    void wordsAndSuggestions()
    {
        SpellChecker s;
        QVERIFY(s.load(dict("utf8")));
        QVERIFY(s.isCorrect("bonjour"));
        QVERIFY(s.isCorrect("Bonjour")); // majuscule en début de phrase
        QVERIFY(s.isCorrect("été"));
        QVERIFY(s.isCorrect("cœur"));
        QVERIFY(s.isCorrect("aujourd'hui"));
        QVERIFY(s.isCorrect(QStringLiteral("aujourd’hui"))); // apostrophe typographique
        QVERIFY(!s.isCorrect("bonjur"));
        QVERIFY(s.suggestions("bonjur").contains("bonjour"));
        QVERIFY(s.suggestions("ete").contains("été"));
        QVERIFY(s.suggestions("bonjur", 0).isEmpty());

        QVERIFY(!s.isCorrect("zorglub"));
        s.addWord("zorglub");
        QVERIFY(s.isCorrect("zorglub"));
        QVERIFY(s.load(dict("utf8"))); // rechargé : mots ajoutés oubliés (l'appelant les rajoute)
        QVERIFY(!s.isCorrect("zorglub"));
    }

    void legacyEncodings()
    {
        SpellChecker latin1;
        QVERIFY(latin1.load(dict("latin1")));
        QVERIFY(latin1.isCorrect("été"));
        QVERIFY(latin1.isCorrect("crème"));
        QVERIFY(!latin1.isCorrect("creme"));
        QVERIFY(latin1.suggestions("creme").contains("crème"));
        QVERIFY(latin1.isCorrect("cœur")); // œ n'existe pas en latin 1 : le dictionnaire ne peut pas en juger
        latin1.addWord("œil");
        QVERIFY(latin1.isCorrect("œil"));

        SpellChecker latin9;
        QVERIFY(latin9.load(dict("latin9")));
        QVERIFY(latin9.isCorrect("cœur"));
        QVERIFY(latin9.isCorrect("œuvre"));
        QVERIFY(!latin9.isCorrect("coeur"));
        QVERIFY(latin9.suggestions("cuer").contains("cœur") || latin9.suggestions("coeur").contains("cœur"));
    }

    void misspelledSkipsWhatIsNotProse()
    {
        SpellChecker s;
        QVERIFY(s.load(dict("utf8")));
        const QString text = QStringLiteral("mnde chat `xyzt` https://exemple.fr/qqqq @zorglub NASA abc3 crème "
                                            "```\nblocdecode\n``` www.bidule.fr bonjur");
        const auto words = s.misspelled(text);
        QStringList found;
        for (const auto &w : words)
            found << text.mid(w.first, w.second);
        QCOMPARE(found, QStringList({"mnde", "bonjur"}));
        QCOMPARE(words.first(), qMakePair(0, 4)); // en tout début de texte
        QVERIFY(s.misspelled("").isEmpty());
        QVERIFY(s.misspelled("😀 !! ...").isEmpty());
    }

    // Avec les vrais dictionnaires (paquets hunspell-fr, hunspell-en-us), s'ils sont installés
    void systemDictionaries()
    {
        const QString fr = SpellChecker::findDictionary("fr"), en = SpellChecker::findDictionary("en");
        if (fr.isEmpty() || en.isEmpty())
            QSKIP("dictionnaires français et anglais absents de ce système");
        SpellChecker s;
        QVERIFY(s.load(fr));
        const QString phrase = QStringLiteral("Bonjour ! Aujourd’hui, l'avion est peut-être arrivé à l’heure, "
                                              "n'est-ce pas ? C'était un arc-en-ciel.");
        QVERIFY2(s.misspelled(phrase).isEmpty(), qPrintable(phrase));
        const QString wrong = QStringLiteral("Bonjour, aujourdhui l'avoin est arivé.");
        QStringList found;
        for (const auto &w : s.misspelled(wrong))
            found << wrong.mid(w.first, w.second);
        QCOMPARE(found, QStringList({"aujourdhui", "l'avoin", "arivé"}));
        QVERIFY(s.suggestions("arivé").contains("arrivé"));

        QVERIFY(s.load(en));
        QVERIFY(s.misspelled("Hello, isn't it a beautiful day? I've been waiting.").isEmpty());
        const QList<QPair<int, int>> helo{{0, 4}};
        QCOMPARE(s.misspelled("Helo world"), helo);
    }

    void findDictionary()
    {
        QTemporaryDir a, b;
        auto touch = [](const QString &path) {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
        };
        touch(a.filePath("en_GB.dic"));
        touch(a.filePath("en_GB.aff"));
        touch(a.filePath("fr.dic")); // sans .aff : ignoré
        touch(b.filePath("fr.dic"));
        touch(b.filePath("fr.aff"));
        touch(b.filePath("en_US.dic"));
        touch(b.filePath("en_US.aff"));
        const QStringList dirs{a.path(), b.path()};
        QCOMPARE(SpellChecker::findDictionary("fr", dirs), QDir(b.path()).filePath("fr.dic"));
        QCOMPARE(SpellChecker::findDictionary("en", dirs), QDir(a.path()).filePath("en_GB.dic")); // premier dossier
        QCOMPARE(SpellChecker::findDictionary("de", dirs), QString());
        QVERIFY(!SpellChecker::searchPaths().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestSpellChecker)
#include "tst_spellchecker.moc"
