#include "mentioncompleter.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCompleter>
#include "composer.h"
#include <QtTest>

#include <memory>

// Autocomplétion dans une vraie zone de saisie, frappes simulées (rendu hors écran)
class TestMentionCompleter : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<Composer> m_edit;
    MentionCompleter *m_completer = nullptr;

    QAbstractItemView *popup() const { return m_edit->findChild<QCompleter *>()->popup(); }

private slots:
    void init()
    {
        m_edit = std::make_unique<Composer>();
        m_completer = new MentionCompleter(m_edit.get());
        m_completer->setNames({"Alice", "Albert", "Bob", "tous"});
        m_edit->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_edit.get()));
        m_edit->activateWindow();
        m_edit->setFocus();
    }

    void cleanup()
    {
        m_edit.reset();
    }

    void proposesMatchingNames()
    {
        QTest::keyClicks(m_edit.get(), "salut @al");
        QTRY_VERIFY(m_completer->isPopupVisible());
        QCOMPARE(popup()->model()->rowCount(), 2); // Albert, Alice
        QCOMPARE(popup()->model()->index(0, 0).data().toString(), QString("Albert"));

        QTest::keyClick(m_edit.get(), Qt::Key_I); // « @ali »
        QTRY_COMPARE(popup()->model()->rowCount(), 1);
    }

    void enterInsertsSelection()
    {
        QTest::keyClicks(m_edit.get(), "@b");
        QTRY_VERIFY(m_completer->isPopupVisible());
        QTest::keyClick(popup(), Qt::Key_Return);
        QCOMPARE(m_edit->text(), QString("@Bob "));
        QCOMPARE(m_edit->cursorPosition(), 5);
        QVERIFY(!m_completer->isPopupVisible());
    }

    void arrowsChooseAnotherName()
    {
        QTest::keyClicks(m_edit.get(), "@al");
        QTRY_VERIFY(m_completer->isPopupVisible());
        QTest::keyClick(popup(), Qt::Key_Down);
        QTest::keyClick(popup(), Qt::Key_Tab);
        QCOMPARE(m_edit->text(), QString("@Alice "));
    }

    // Frappe livrée comme une vraie : à la fenêtre active, pas directement à la liste
    void realTabKey()
    {
        QTest::keyClicks(m_edit.get(), "@al");
        QTRY_VERIFY(m_completer->isPopupVisible());
        QTest::keyClick(QGuiApplication::focusWindow(), Qt::Key_Tab);
        QCOMPARE(m_edit->text(), QString("@Albert "));
    }

    // Liste fermée : Tab complète le seul pseudo possible, sans quitter la zone de saisie
    void tabCompletesSingleCandidate()
    {
        QTest::keyClicks(m_edit.get(), "salut @bo");
        QTRY_VERIFY(m_completer->isPopupVisible());
        QTest::keyClick(popup(), Qt::Key_Escape);
        QVERIFY(!m_completer->isPopupVisible());
        QTest::keyClick(m_edit.get(), Qt::Key_Tab);
        QCOMPARE(m_edit->text(), QString("salut @Bob "));
        QVERIFY(m_edit->hasFocus());
    }

    // Liste fermée, plusieurs possibilités : Tab ouvre la liste, Tab suivant insère
    void tabOpensListWhenAmbiguous()
    {
        QTest::keyClicks(m_edit.get(), "@al");
        QTRY_VERIFY(m_completer->isPopupVisible());
        QTest::keyClick(popup(), Qt::Key_Escape);
        QTest::keyClick(m_edit.get(), Qt::Key_Tab);
        QTRY_VERIFY(m_completer->isPopupVisible());
        QCOMPARE(m_edit->text(), QString("@al"));
        QTest::keyClick(m_edit.get(), Qt::Key_Tab);
        QCOMPARE(m_edit->text(), QString("@Albert "));
    }

    // Pas de mention en cours : Tab n'est pas intercepté (passage au bouton d'envoi)
    void tabWithoutMentionIsUntouched()
    {
        QTest::keyClicks(m_edit.get(), "bonjour");
        QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
        QVERIFY(!static_cast<QObject *>(m_completer)->eventFilter(m_edit.get(), &tab)); // public via QObject
        QCOMPARE(m_edit->text(), QString("bonjour"));
    }

    void noPopupWithoutMatch()
    {
        QTest::keyClicks(m_edit.get(), "bob@al @zz");
        QTest::qWait(50);
        QVERIFY(!m_completer->isPopupVisible());
    }

    void escapeClosesPopup()
    {
        QTest::keyClicks(m_edit.get(), "@");
        QTRY_VERIFY(m_completer->isPopupVisible());
        QCOMPARE(popup()->model()->rowCount(), 4);
        QTest::keyClick(popup(), Qt::Key_Escape);
        QVERIFY(!m_completer->isPopupVisible());
        QCOMPARE(m_edit->text(), QString("@"));
    }
};

int main(int argc, char *argv[])
{
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    TestMentionCompleter test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_mentioncompleter.moc"
