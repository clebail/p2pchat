#include "chatview.h"

#include <QApplication>
#include <QtTest>

namespace {

ChatEntry message(const QString &text, const QDateTime &ts)
{
    ChatEntry e;
    e.author = "Alice";
    e.text = text;
    e.ts = ts;
    return e;
}

QStringList texts(const ChatModel *m)
{
    QStringList out;
    for (int i = 0; i < m->rowCount(); ++i)
        out << m->entry(i).text;
    return out;
}

} // namespace

class TestChatModel : public QObject
{
    Q_OBJECT

private slots:
    void insertSortedKeepsChronologicalOrder()
    {
        ChatModel model;
        const QDateTime t0 = QDateTime::currentDateTime();
        QCOMPARE(model.insertSorted(message("b", t0.addSecs(20))), 0);
        QCOMPARE(model.insertSorted(message("d", t0.addSecs(40))), 1);
        QCOMPARE(model.insertSorted(message("a", t0)), 0);                 // rattrapé, plus ancien
        QCOMPARE(model.insertSorted(message("c", t0.addSecs(30))), 2);
        QCOMPARE(model.insertSorted(message("d2", t0.addSecs(40))), 4);    // même heure : après
        QCOMPARE(texts(&model), QStringList({"a", "b", "c", "d", "d2"}));
        QCOMPARE(model.data(model.index(2), Qt::DisplayRole).toString(), QString("c"));
        QVERIFY(!model.data(model.index(99), Qt::DisplayRole).isValid());

        model.clear();
        QCOMPARE(model.rowCount(), 0);
    }

    void presenceEventsAreGrouped()
    {
        ChatView view;
        ChatModel *model = view.chatModel();
        view.addPresence("Alice", true);
        QCOMPARE(model->rowCount(), 1);
        QCOMPARE(model->entry(0).text, QString("Alice est en ligne."));

        view.addPresence("Bob", true);
        view.addPresence("Alice", false, "réseau perdu");
        QCOMPARE(model->rowCount(), 1); // une seule ligne
        const ChatEntry &e = model->entry(0);
        QCOMPARE(e.events.size(), 3);
        QVERIFY(e.text.startsWith("Bob est en ligne · Alice s'est déconnecté(e)"));
        QVERIFY(e.text.contains("3 évènements"));

        model->toggleExpanded(0); // dépliée : le détail de chaque évènement
        QVERIFY(model->entry(0).text.contains("réseau perdu"));
        model->toggleExpanded(0);
        QVERIFY(!model->entry(0).text.contains("réseau perdu"));
    }

    void messageBreaksPresenceGroup()
    {
        ChatView view;
        view.addPresence("Alice", true);
        view.addMessage("Alice", "coucou", QDateTime::currentDateTime(), false);
        view.addPresence("Bob", true);
        QCOMPARE(view.chatModel()->rowCount(), 3);
        QCOMPARE(view.chatModel()->entry(2).text, QString("Bob est en ligne."));
    }

    void singlePresenceCannotExpand()
    {
        ChatView view;
        view.addPresence("Alice", false);
        QCOMPARE(view.chatModel()->entry(0).text, QString("Alice s'est déconnecté(e)."));
        view.chatModel()->toggleExpanded(0);
        QVERIFY(!view.chatModel()->entry(0).expanded);
    }

    void messageEntry()
    {
        ChatView view;
        const QDateTime ts = QDateTime::currentDateTime();
        view.addMessage("Bob", "salut", ts, true, ChatEntry::Invalid);
        const ChatEntry &e = view.chatModel()->entry(0);
        QCOMPARE(e.kind, ChatEntry::Message);
        QCOMPARE(e.author, QString("Bob"));
        QVERIFY(e.mine);
        QCOMPARE(e.trust, ChatEntry::Invalid);
        view.addSystem("info");
        QCOMPARE(view.chatModel()->entry(1).kind, ChatEntry::System);
        view.clearAll();
        QCOMPARE(view.chatModel()->rowCount(), 0);
    }

    void avatarColorIsStable()
    {
        QCOMPARE(ChatView::colorFor("Alice"), ChatView::colorFor("Alice"));
        QVERIFY(ChatView::colorFor("Alice").isValid());
    }
};

int main(int argc, char *argv[])
{
    // Pas besoin d'écran : rendu hors écran
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    TestChatModel test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_chatmodel.moc"
