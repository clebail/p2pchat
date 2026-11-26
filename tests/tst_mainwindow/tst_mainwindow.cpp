#include "appstyle.h"
#include "mainwindow.h"

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QtTest>

#include <memory>

// La fenêtre principale, hors écran, sur des réglages et un dossier de données à part. Tor ne démarre pas
// (exécutable forcé qui échoue aussitôt) : rien ne part sur le réseau.
class TestMainWindow : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<MainWindow> m_window;

    QMenu *mainMenu() const
    {
        for (QToolButton *b : m_window->findChildren<QToolButton *>())
            if (b->menu() && b->menu()->actions().size() > 10)
                return b->menu();
        return nullptr;
    }
    static QAction *action(QMenu *menu, const QString &text)
    {
        for (QAction *a : menu->actions())
            if (a->text() == text)
                return a;
        return nullptr;
    }
    // Ce que le menu montre à son ouverture (états recalculés par aboutToShow)
    QMenu *openedMainMenu() const
    {
        QMenu *menu = mainMenu();
        if (menu)
            emit menu->aboutToShow();
        return menu;
    }
    QWidget *named(const QString &objectName) const { return m_window->findChild<QWidget *>(objectName); }

private slots:
    void initTestCase()
    {
        QSettings s;
        s.setValue("nickname", "Testeur");
        s.setValue("torPath", QFileInfo::exists("/bin/false") ? QStringLiteral("/bin/false") : QStringLiteral("absent"));
        s.setValue("language", "fr");
    }

    void init()
    {
        m_window = std::make_unique<MainWindow>();
        m_window->resize(900, 700);
        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
        // Fenêtre active : sans elle, les raccourcis (Ctrl+F) ne partent pas (Qt 5 hors écran)
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
    }

    void cleanup() { m_window.reset(); }

    void startsOnTheHomePage()
    {
        QVERIFY(named("home")->isVisible());
        QCOMPARE(qobject_cast<QLabel *>(named("title"))->text(), QString("P2P Chat"));
        auto *list = m_window->findChild<QListWidget *>("convList");
        QVERIFY(list);
        QCOMPARE(list->count(), 1);
        QCOMPARE(list->item(0)->text(), QString("Conversation vierge"));
        QVERIFY(!list->item(0)->icon().isNull()); // rond avec l'initiale
        // Tor n'a pas pu démarrer : c'est dit dans la ligne d'état
        QTRY_VERIFY(qobject_cast<QLabel *>(named("subtitle"))->text().contains("Tor"));
    }

    // Hors d'un groupe : rien à administrer, personne à inviter directement
    void unavailableActionsAreDisabled()
    {
        QMenu *menu = openedMainMenu();
        QVERIFY(menu);
        for (const char *text : {"Renommer la conversation…", "Avatar de la conversation…", "Nouvelle conversation avec…"}) {
            QAction *a = action(menu, QString::fromUtf8(text));
            QVERIFY2(a, text);
            QVERIFY2(!a->isEnabled(), text);
        }
        QVERIFY(action(menu, "Créer la conversation et inviter…")); // pas encore de groupe
        QVERIFY(!action(menu, "Quitter la conversation…")->isVisible());
        QVERIFY(action(menu, "Supprimer la conversation…")->isVisible());
        QVERIFY(action(menu, "Changer d'avatar…")->isEnabled());
    }

    // Une action désactivée doit se voir : texte nettement plus clair (le style de l'application l'imposait
    // naguère à toutes les actions)
    void disabledActionsLookGreyed()
    {
        QMenu menu;
        menu.setStyleSheet(AppStyle::styleSheet());
        menu.addAction("Action disponible");
        menu.addAction("Action indisponible")->setEnabled(false);
        menu.adjustSize();
        const QImage img = menu.grab().toImage();
        auto darkest = [&img](int top, int bottom) {
            int d = 255;
            for (int y = top; y < bottom; ++y)
                for (int x = 4; x < img.width() - 4; ++x)
                    d = qMin(d, qGray(img.pixel(x, y)));
            return d;
        };
        const int half = img.height() / 2;
        const int enabled = darkest(4, half - 4), disabled = darkest(half + 4, img.height() - 4);
        QVERIFY2(disabled - enabled > 60, qPrintable(QStringLiteral("gris %1 contre %2").arg(disabled).arg(enabled)));
    }

    void submenus()
    {
        QMenu *menu = openedMainMenu();
        QMenu *language = action(menu, "Langue")->menu();
        QCOMPARE(language->actions().size(), 3);
        QVERIFY(language->actions().at(1)->isChecked()); // réglé sur le français
        QCOMPARE(language->actions().at(2)->text(), QString("English"));

        QMenu *spelling = action(menu, "Correcteur orthographique")->menu();
        emit spelling->aboutToShow();
        QVERIFY(!spelling->actions().isEmpty());
        QMenu *notifications = action(menu, "Notifications")->menu();
        emit notifications->aboutToShow();
        QVERIFY(!notifications->actions().isEmpty());
    }

    void searchOpensAndCloses()
    {
        QWidget *bar = named("searchBar");
        QVERIFY(!bar->isVisible());
        QTest::keySequence(m_window.get(), QKeySequence::Find);
        QTRY_VERIFY(bar->isVisible());
        auto *edit = m_window->findChild<QLineEdit *>("searchEdit");
        QTRY_VERIFY(edit->hasFocus());
        QTest::keyClick(edit, Qt::Key_Escape);
        QTRY_VERIFY(!bar->isVisible());
    }

    // Fenêtre étroite : la colonne se replie ; large : elle revient
    void sidebarFollowsWidth()
    {
        QWidget *sidebar = named("sidebar");
        QVERIFY(sidebar->isVisible());
        m_window->resize(500, 700);
        QTRY_VERIFY(!sidebar->isVisible());
        m_window->resize(900, 700);
        QTRY_VERIFY(sidebar->isVisible());
    }

    void drawnIcons()
    {
        for (const QIcon &icon : {AppStyle::moreIcon(), AppStyle::attachIcon(), AppStyle::sendIcon()})
            QVERIFY(!icon.pixmap(24, 24).isNull());
        QVERIFY(AppStyle::styleSheet().contains("QMenu::item:disabled"));
    }
};

int main(int argc, char *argv[])
{
    // Avant l'application : dossiers de données et réglages à part, pour ne rien toucher de l'utilisateur
    QTemporaryDir home;
#ifndef Q_OS_WIN
    qputenv("HOME", home.path().toUtf8());
    qputenv("XDG_DATA_HOME", (home.path() + "/data").toUtf8());
    qputenv("XDG_CONFIG_HOME", (home.path() + "/config").toUtf8());
#endif
    QApplication app(argc, argv);
    QApplication::setOrganizationName("p2pchat-tests");
    QApplication::setApplicationName("tst_mainwindow");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, home.filePath("settings"));
    // Tor échoue : la fenêtre le dit dans une boîte modale (« Réessayer », « Fermer »…). Toute boîte modale est
    // fermée aussitôt, comme par Échap, sinon elle attendrait une réponse indéfiniment.
    QTimer closer;
    QObject::connect(&closer, &QTimer::timeout, [] {
        if (QWidget *modal = QApplication::activeModalWidget())
            modal->close();
    });
    closer.start(50);
    TestMainWindow test;
    const int result = QTest::qExec(&test, argc, argv);
#ifdef Q_OS_WIN
    // Sous Windows, le dossier de données ne suit pas HOME : celui de ce test est retiré
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
#endif
    return result;
}

#include "tst_mainwindow.moc"
