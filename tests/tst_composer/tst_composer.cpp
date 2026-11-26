#include "composer.h"
#include "spellchecker.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QMenu>
#include <QTimer>
#include <QClipboard>
#include <QMimeData>
#include <QSignalSpy>
#include <QTemporaryFile>
#include <QTextBlock>
#include <QTextLayout>
#include <QtTest>

#include <memory>

class TestComposer : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<Composer> m_edit;

    using Spans = QList<QPair<int, int>>;
    // Mots soulignés par le correcteur : positions (début, longueur) dans le premier paragraphe
    Spans underlined() const
    {
        Spans out;
        for (const QTextLayout::FormatRange &r : m_edit->document()->firstBlock().layout()->formats())
            if (r.format.underlineStyle() == QTextCharFormat::SpellCheckUnderline) {
                if (!out.isEmpty() && out.last().first + out.last().second == r.start)
                    out.last().second += r.length; // souligné lettre par lettre : regroupé
                else
                    out << qMakePair(r.start, r.length);
            }
        return out;
    }

private slots:
    void init()
    {
        m_edit = std::make_unique<Composer>();
        m_edit->resize(300, 40);
        m_edit->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_edit.get()));
        m_edit->setFocus();
    }

    void cleanup() { m_edit.reset(); }

    void enterSendsShiftEnterBreaksLine()
    {
        QSignalSpy send(m_edit.get(), &Composer::returnPressed);
        QTest::keyClicks(m_edit.get(), "ligne 1");
        QTest::keyClick(m_edit.get(), Qt::Key_Return, Qt::ShiftModifier);
        QTest::keyClicks(m_edit.get(), "ligne 2");
        QCOMPARE(send.count(), 0);
        QCOMPARE(m_edit->text(), QString("ligne 1\nligne 2")); // un vrai saut de ligne
        QTest::keyClick(m_edit.get(), Qt::Key_Return);
        QTest::keyClick(m_edit.get(), Qt::Key_Enter); // pavé numérique
        QCOMPARE(send.count(), 2);
        QCOMPARE(m_edit->text(), QString("ligne 1\nligne 2")); // Entrée n'ajoute rien
    }

    void tabIsNeverInserted()
    {
        QTest::keyClicks(m_edit.get(), "a");
        QTest::keyClick(m_edit.get(), Qt::Key_Tab);
        QCOMPARE(m_edit->text(), QString("a"));
    }

    void heightFollowsText()
    {
        const int one = m_edit->height();
        QCOMPARE(m_edit->visibleLines(), 1);
        for (int i = 0; i < 3; ++i)
            QTest::keyClick(m_edit.get(), Qt::Key_Return, Qt::ShiftModifier);
        QCOMPARE(m_edit->visibleLines(), 4);
        QVERIFY(m_edit->height() > one);
        for (int i = 0; i < 10; ++i)
            QTest::keyClick(m_edit.get(), Qt::Key_Return, Qt::ShiftModifier);
        QCOMPARE(m_edit->visibleLines(), Composer::MaxLines); // plafonnée, ascenseur au-delà
        const int max = m_edit->height();
        m_edit->clear();
        QCOMPARE(m_edit->visibleLines(), 1);
        QVERIFY(m_edit->height() < max);
    }

    void textEditedOnlyForUserChanges()
    {
        QSignalSpy edited(m_edit.get(), &Composer::textEdited);
        m_edit->setText("posé par le programme");
        QCOMPARE(edited.count(), 0);
        QCOMPARE(m_edit->cursorPosition(), m_edit->text().size()); // curseur à la fin
        QTest::keyClicks(m_edit.get(), "x");
        QCOMPARE(edited.count(), 1);
        QCOMPARE(edited.last().first().toString(), QString("posé par le programmex"));
        m_edit->setCursorPosition(4);
        QCOMPARE(m_edit->cursorPosition(), 4);
    }

    void maxLength()
    {
        m_edit->setMaxLength(10);
        QMimeData mime;
        mime.setText(QString(50, 'a'));
        QApplication::clipboard()->setMimeData(new QMimeData);
        QApplication::clipboard()->setText(QString(50, 'a'));
        m_edit->paste();
        QCOMPARE(m_edit->text().size(), 10);
    }

    void pastedTextIsPlain()
    {
        auto *mime = new QMimeData;
        mime->setHtml("<b>gras</b>");
        mime->setText("gras");
        QApplication::clipboard()->setMimeData(mime);
        m_edit->paste();
        QCOMPARE(m_edit->text(), QString("gras"));
    }

    void misspelledWordsAreUnderlined()
    {
        SpellChecker checker;
        if (!checker.load(DICT_DIR "/utf8.dic"))
            QSKIP("compilé sans Hunspell");
        m_edit->setSpellChecker(&checker);
        QSignalSpy edited(m_edit.get(), &Composer::textEdited);

        // En cours de frappe : le mot n'est souligné qu'une fois quitté
        QTest::keyClicks(m_edit.get(), "bonjour mnde");
        QVERIFY(underlined().isEmpty());
        QTest::keyClicks(m_edit.get(), " ");
        QCOMPARE(underlined(), (Spans{{8, 4}}));
        QTest::keyClicks(m_edit.get(), "chta");
        QCOMPARE(underlined(), (Spans{{8, 4}}));
        QTest::keyClick(m_edit.get(), Qt::Key_Home); // curseur ailleurs : vérifié
        QCOMPARE(underlined(), (Spans{{8, 4}, {13, 4}}));
        const int edits = edited.count();
        QTest::keyClick(m_edit.get(), Qt::Key_End);
        QCOMPARE(edited.count(), edits); // la mise en forme n'est pas une frappe

        // Ajouté au dictionnaire : plus souligné
        checker.addWord("chta");
        m_edit->recheckSpelling();
        QCOMPARE(underlined(), (Spans{{8, 4}}));

        // Sans correcteur : rien
        m_edit->setSpellChecker(nullptr);
        QVERIFY(underlined().isEmpty());
        QCOMPARE(edited.count(), edits);
    }

    // Clic droit sur un mot souligné : corrections, puis « Ajouter au dictionnaire » (le menu est piloté
    // depuis une minuterie, car il s'ouvre en attendant un choix)
    void contextMenuOffersCorrections()
    {
        SpellChecker checker;
        if (!checker.load(DICT_DIR "/utf8.dic"))
            QSKIP("compilé sans Hunspell");
        m_edit->setSpellChecker(&checker);
        m_edit->setText("le bonjur chat");
        QSignalSpy added(m_edit.get(), &Composer::wordAdded);

        auto openMenuOn = [this](int position, const QString &choice) {
            QTextCursor c = m_edit->textCursor();
            c.setPosition(position);
            const QPoint at = m_edit->cursorRect(c).center();
            QStringList shown;
            QTimer::singleShot(100, this, [&shown, choice] {
                auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                if (!menu)
                    return;
                for (QAction *a : menu->actions())
                    shown << a->text();
                for (QAction *a : menu->actions())
                    if (a->text() == choice) {
                        menu->close();
                        a->trigger();
                        return;
                    }
                menu->close();
            });
            QContextMenuEvent event(QContextMenuEvent::Mouse, at, m_edit->viewport()->mapToGlobal(at));
            QCoreApplication::sendEvent(m_edit->viewport(), &event);
            return shown;
        };

        // Sur « bonjur » : la correction en tête du menu
        QStringList shown = openMenuOn(5, "bonjour");
        QVERIFY2(shown.value(0) == "bonjour", qPrintable(shown.join(" | ")));
        QVERIFY(shown.contains("Ajouter « bonjur » au dictionnaire"));
        QCOMPARE(m_edit->text(), QString("le bonjour chat"));

        // Sur un mot correct : le menu habituel seulement
        shown = openMenuOn(13, QString());
        QVERIFY(!shown.isEmpty());
        QVERIFY(!shown.join(' ').contains("dictionnaire"));

        // Ajout au dictionnaire : signalé (la fenêtre le garde et l'ajoute au correcteur)
        m_edit->setText("zorglub");
        shown = openMenuOn(2, "Ajouter « zorglub » au dictionnaire");
        QVERIFY(shown.contains("(aucune suggestion)"));
        QCOMPARE(added.count(), 1);
        QCOMPARE(added.at(0).at(0).toString(), QString("zorglub"));
        QCOMPARE(m_edit->text(), QString("zorglub")); // texte inchangé
    }

    void fileIsHandedOver()
    {
        QSignalSpy attached(m_edit.get(), &Composer::attachmentInserted);
        QTemporaryFile file;
        QVERIFY(file.open());
        file.write("contenu");
        file.close();
        auto *mime = new QMimeData;
        mime->setUrls({QUrl::fromLocalFile(file.fileName())});
        QApplication::clipboard()->setMimeData(mime);
        m_edit->paste();
        QCOMPARE(attached.count(), 1);
        QVERIFY(m_edit->text().isEmpty()); // pas le chemin du fichier
    }

    void imageIsHandedOver()
    {
        QSignalSpy image(m_edit.get(), &Composer::attachmentInserted);
        QImage img(10, 10, QImage::Format_RGB32);
        img.fill(Qt::red);
        auto *mime = new QMimeData;
        mime->setImageData(img);
        QApplication::clipboard()->setMimeData(mime);
        QVERIFY(m_edit->canPaste());
        m_edit->paste();
        QCOMPARE(image.count(), 1);
        QVERIFY(m_edit->text().isEmpty()); // rien d'inséré dans le texte
    }
};

int main(int argc, char *argv[])
{
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    TestComposer test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_composer.moc"
