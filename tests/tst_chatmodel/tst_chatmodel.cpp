#include "chatview.h"

#include <QApplication>
#include <QMouseEvent>
#include <QSignalSpy>
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

    void mentionsAreKept()
    {
        ChatView view;
        const QDateTime ts = QDateTime::currentDateTime();
        view.addMessage("Alice", "@Bob regarde", ts, false, ChatEntry::Verified, {{0, 4}}, true);
        view.addMessage("Bob", "@tous coucou", ts, true, ChatEntry::Verified, {{0, 5}}, true);
        const ChatEntry &received = view.chatModel()->entry(0);
        QCOMPARE(received.mentions, (QList<QPair<int, int>>{{0, 4}}));
        QVERIFY(received.mentionsMe);
        QVERIFY(!view.chatModel()->entry(1).mentionsMe); // nos propres messages ne sont pas mis en avant
        view.grab(); // dessin des bulles avec mentions : ne doit pas planter
    }

    void replyQuotesOriginal()
    {
        ChatModel model;
        const QDateTime t0 = QDateTime::currentDateTime();
        ChatEntry original = message("Question ?\nSur deux lignes", t0);
        original.id = "q";
        model.insertSorted(original);
        ChatEntry reply = message("Réponse", t0.addSecs(5));
        reply.id = "r";
        reply.replyTo = "q";
        model.insertSorted(reply);
        QVERIFY(model.entry(1).replyFound);
        QCOMPARE(model.entry(1).replyAuthor, QString("Alice"));
        QCOMPARE(model.entry(1).replyText, original.text);
        QCOMPARE(model.rowOf("q"), 0);
        QCOMPARE(model.rowOf("absent"), -1);
        QCOMPARE(model.rowOf(QString()), -1);
    }

    void replyArrivingBeforeOriginal()
    {
        // Synchronisation dans le désordre : la citation se complète quand l'original arrive
        ChatModel model;
        const QDateTime t0 = QDateTime::currentDateTime();
        ChatEntry reply = message("Réponse", t0.addSecs(5));
        reply.replyTo = "q";
        model.insertSorted(reply);
        QVERIFY(!model.entry(0).replyFound);

        QSignalSpy changed(&model, &ChatModel::dataChanged);
        ChatEntry original = message("Question", t0);
        original.id = "q";
        original.mine = true;
        model.insertSorted(original);
        QCOMPARE(model.entry(1).text, QString("Réponse"));
        QVERIFY(model.entry(1).replyFound);
        QCOMPARE(model.entry(1).replyAuthor, QString("Vous")); // c'était notre message
        QVERIFY(!changed.isEmpty());
    }

    void excerpt()
    {
        QCOMPARE(ChatView::excerpt("  un\n  deux  "), QString("un deux"));
        QCOMPARE(ChatView::excerpt(QString(10, 'x'), 5), QString("xxxx") + QChar(0x2026));
        QCOMPARE(ChatView::excerpt("court", 5), QString("court"));
    }

    void clickOnQuoteShowsOriginal()
    {
        ChatView view;
        view.resize(480, 600);
        const QDateTime t0 = QDateTime::currentDateTime();
        ChatEntry original = message("Le message d'origine", t0);
        original.id = "q";
        view.addMessage(original);
        ChatEntry reply = message("Je réponds", t0.addSecs(5));
        reply.author = "Bob";
        reply.id = "r";
        reply.replyTo = "q";
        view.addMessage(reply);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));

        auto *delegate = qobject_cast<ChatDelegate *>(view.itemDelegate());
        QSignalSpy quote(delegate, &ChatDelegate::quoteClicked);
        // Balaye la ligne : un clic dans l'encadré de citation, et seulement là, ouvre l'original
        auto clicksOn = [&](int row) {
            const QModelIndex idx = view.chatModel()->index(row);
            QStyleOptionViewItem opt;
            opt.rect = view.visualRect(idx);
            int hits = 0;
            for (int y = opt.rect.top(); y < opt.rect.bottom(); y += 3)
                for (int x = opt.rect.left(); x < opt.rect.right(); x += 10) {
                    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(x, y), Qt::LeftButton, Qt::NoButton,
                                        Qt::NoModifier);
                    hits += delegate->editorEvent(&release, view.model(), opt, idx) ? 1 : 0;
                }
            return hits;
        };
        QCOMPARE(clicksOn(0), 0); // message sans citation
        QVERIFY(clicksOn(1) > 0);
        QCOMPARE(quote.first().first().toString(), QString("q"));

        QVERIFY(view.showMessage("q"));
        QCOMPARE(view.flashedId(), QString("q"));
        QTRY_VERIFY_WITH_TIMEOUT(view.flashedId().isEmpty(), 3000); // la mise en évidence s'efface
        QVERIFY(!view.showMessage("absent"));
        view.grab(); // dessin de la citation (trouvée ou non) : ne doit pas planter
    }

    void imageMessage()
    {
        ChatView view;
        view.resize(480, 600);
        const QDateTime t0 = QDateTime::currentDateTime();
        ChatEntry photo = message(QString(), t0); // image sans légende
        photo.id = "p";
        photo.image = QImage(800, 600, QImage::Format_RGB32);
        photo.image.fill(Qt::darkGreen);
        view.addMessage(photo);
        ChatEntry reply = message("Belle photo", t0.addSecs(5));
        reply.replyTo = "p";
        view.addMessage(reply);
        QCOMPARE(view.chatModel()->entry(1).replyText, QString("📷 Image")); // citation d'une image
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));

        // La bulle est à la taille de l'image réduite (300 px au plus), et un clic dessus l'ouvre
        const QModelIndex idx = view.chatModel()->index(0);
        QStyleOptionViewItem opt;
        opt.rect = view.visualRect(idx);
        QVERIFY(opt.rect.height() >= 225 && opt.rect.height() < 300);
        auto *delegate = qobject_cast<ChatDelegate *>(view.itemDelegate());
        QSignalSpy clicked(delegate, &ChatDelegate::imageClicked);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(opt.rect.left() + 150, opt.rect.center().y()),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QVERIFY(delegate->editorEvent(&release, view.model(), opt, idx));
        QCOMPARE(clicked.count(), 1);
        QCOMPARE(clicked.first().first().toInt(), 0);
        view.grab(); // dessin de l'image arrondie
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
