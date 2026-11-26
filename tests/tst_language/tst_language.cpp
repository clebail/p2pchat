#include "filestore.h"
#include "language.h"

#include <QFile>
#include <QLocale>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <QtTest>

class TestLanguage : public QObject
{
    Q_OBJECT

private slots:
    void resolve_data()
    {
        QTest::addColumn<QString>("setting");
        QTest::addColumn<QStringList>("system");
        QTest::addColumn<QString>("expected");
        QTest::newRow("système français") << QString() << QStringList{"fr-FR", "en-US"} << "fr";
        QTest::newRow("français de Belgique") << QString() << QStringList{"fr-BE"} << "fr";
        QTest::newRow("français seul") << QString() << QStringList{"fr"} << "fr";
        QTest::newRow("système anglais") << QString() << QStringList{"en-GB"} << "en";
        QTest::newRow("français en second") << QString() << QStringList{"en-US", "fr-FR"} << "en";
        QTest::newRow("autre langue") << QString() << QStringList{"de-DE"} << "en";
        QTest::newRow("rien") << QString() << QStringList{} << "en";
        QTest::newRow("frison (pas du français)") << QString() << QStringList{"fy-NL"} << "en";
        QTest::newRow("choix : français") << "fr" << QStringList{"en-US"} << "fr";
        QTest::newRow("choix : anglais") << "en" << QStringList{"fr-FR"} << "en";
        QTest::newRow("réglage inconnu") << "de" << QStringList{"fr-FR"} << "fr";
    }

    void resolve()
    {
        QFETCH(QString, setting);
        QFETCH(QStringList, system);
        QFETCH(QString, expected);
        QCOMPARE(Language::resolve(setting, system), expected);
    }

    void settingRoundTrip()
    {
        QTemporaryDir dir;
        QSettings s(dir.filePath("p2pchat.ini"), QSettings::IniFormat);
        QCOMPARE(Language::setting(s), QString());
        Language::setSetting(s, "en");
        QCOMPARE(Language::setting(s), QString("en"));
        Language::setSetting(s, "de"); // inconnu : automatique
        QCOMPARE(Language::setting(s), QString());
        QVERIFY(!s.contains("language"));
        s.setValue("language", "xx");
        QCOMPARE(Language::setting(s), QString());
    }

    // Chaque texte traduit, avec les mêmes marqueurs (%1…, %n) que l'original : sinon lupdate a trouvé de
    // nouveaux textes à traduire (lupdate p2pchat.pro, puis Qt Linguist)
    void translationFileIsComplete()
    {
        QFile f(TS_FILE);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QXmlStreamReader xml(&f);
        static const QRegularExpression marker("%[0-9n]");
        auto markers = [](const QString &s) {
            QStringList out;
            for (auto it = marker.globalMatch(s); it.hasNext();)
                out << it.next().captured();
            out.sort();
            return out;
        };
        int messages = 0;
        QString source;
        bool numerus = false;
        while (!xml.atEnd()) {
            if (xml.readNext() != QXmlStreamReader::StartElement)
                continue;
            if (xml.name() == QLatin1String("message")) {
                numerus = xml.attributes().value("numerus") == QLatin1String("yes");
                ++messages;
            } else if (xml.name() == QLatin1String("source")) {
                source = xml.readElementText();
            } else if (xml.name() == QLatin1String("translation")) {
                const QString type = xml.attributes().value("type").toString();
                QVERIFY2(type.isEmpty(), qPrintable(QStringLiteral("pas traduit (%1) : %2").arg(type, source)));
                if (numerus) {
                    int forms = 0;
                    while (xml.readNextStartElement()) {
                        const QString form = xml.readElementText();
                        QVERIFY2(form.contains("%n"), qPrintable(source));
                        ++forms;
                    }
                    QCOMPARE(forms, 2); // anglais : singulier, pluriel
                } else {
                    const QString text = xml.readElementText();
                    QVERIFY2(!text.isEmpty(), qPrintable(source));
                    QVERIFY2(markers(text) == markers(source), qPrintable(source + " → " + text));
                }
            }
        }
        QVERIFY2(!xml.hasError(), qPrintable(xml.errorString() + " ligne " + QString::number(xml.lineNumber())));
        QVERIFY(messages > 400);
    }

    void frenchIsApplied()
    {
        Language::apply("fr");
        QCOMPARE(QLocale().language(), QLocale::French);
        QCOMPARE(QCoreApplication::translate("MainWindow", "Quitter P2P Chat"), QString("Quitter P2P Chat"));
        QCOMPARE(Files::formatSize(1536), QString("1,5 Ko"));
    }

    // En dernier : les traductions installées le restent pour la suite du programme
    void englishIsApplied()
    {
        QCOMPARE(QCoreApplication::translate("MainWindow", "Quitter P2P Chat"), QString("Quitter P2P Chat"));
        Language::apply("en");
        QCOMPARE(QLocale().language(), QLocale::English);
        QCOMPARE(QCoreApplication::translate("MainWindow", "Quitter P2P Chat"), QString("Quit P2P Chat"));
        QCOMPARE(QCoreApplication::translate("Conversation", "%n message(s) reçu(s) pendant votre absence", nullptr, 1),
                 QString("1 message received while you were away"));
        QCOMPARE(QCoreApplication::translate("Conversation", "%n message(s) reçu(s) pendant votre absence", nullptr, 3),
                 QString("3 messages received while you were away"));
        QCOMPARE(Files::formatSize(1536), QString("1.5 KB")); // traduit, et nombre à l'anglaise
        QCOMPARE(Files::formatSize(10), QString("10 bytes"));
    }
};

QTEST_GUILESS_MAIN(TestLanguage)
#include "tst_language.moc"
