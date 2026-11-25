#include "chatview.h"

#include <QApplication>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QScrollBar>
#include <QToolButton>
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

    void editUpdatesMessageAndQuotes()
    {
        ChatModel model;
        const QDateTime t0 = QDateTime::currentDateTime();
        ChatEntry original = message("Version 1", t0);
        original.id = "q";
        original.imageData = "image";
        model.insertSorted(original);
        ChatEntry reply = message("Réponse", t0.addSecs(5));
        reply.replyTo = "q";
        model.insertSorted(reply);

        ChatEntry updated;
        updated.text = "Version 2";
        updated.edited = true;
        QVERIFY(model.applyEdit("q", updated));
        QCOMPARE(model.entry(0).text, QString("Version 2"));
        QVERIFY(model.entry(0).edited);
        QCOMPARE(model.entry(1).replyText, QString("Version 2")); // la citation suit
        QVERIFY(!model.applyEdit("absent", updated));

        ChatEntry gone;
        gone.text = "🚫 Message supprimé par Bob";
        gone.deleted = true;
        gone.editedBy = "Bob";
        QVERIFY(model.applyEdit("q", gone));
        QVERIFY(model.entry(0).deleted);
        QVERIFY(model.entry(0).imageData.isEmpty()); // plus d'image ni de fichier
        QCOMPARE(model.entry(0).editedBy, QString("Bob"));
        QCOMPARE(model.entry(1).replyText, gone.text);
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

    void foldAndSearch()
    {
        QCOMPARE(ChatModel::fold("Été À ÇA œuf"), QString("ete a ca œuf"));
        QCOMPARE(ChatModel::fold("Été").size(), 3); // positions inchangées
        ChatModel model;
        const QDateTime t0 = QDateTime::currentDateTime();
        model.insertSorted(message("Il a fait très chaud cet été", t0));
        model.insertSorted(message("rien à voir", t0.addSecs(1)));
        ChatEntry sys;
        sys.kind = ChatEntry::System;
        sys.text = "été système";
        sys.ts = t0.addSecs(2);
        model.insertSorted(sys); // les lignes système ne sont pas cherchées
        model.insertSorted(message("ETE 2024", t0.addSecs(3)));
        QCOMPARE(model.search("ete"), QList<int>({0, 3}));
        QCOMPARE(model.search("  Été "), QList<int>({0, 3}));
        QVERIFY(model.search("").isEmpty());
        QVERIFY(model.search("hiver").isEmpty());
    }

    void searchNavigation()
    {
        ChatView view;
        view.resize(480, 400);
        const QDateTime t0 = QDateTime::currentDateTime();
        for (int i = 0; i < 6; ++i)
            view.addMessage(message(i % 2 ? QStringLiteral("chat %1").arg(i) : QStringLiteral("autre %1").arg(i),
                                    t0.addSecs(i)));
        QSignalSpy result(&view, &ChatView::searchResultChanged);
        view.setSearchTerm("chat"); // lignes 1, 3, 5 : on commence par la plus récente
        QCOMPARE(result.last(), QVariantList({1, 3}));
        QCOMPARE(view.chatModel()->entry(5).text, QString("chat 5"));
        QVERIFY(view.isFlashed(view.chatModel()->index(5)));
        view.searchNext(true);
        QCOMPARE(result.last(), QVariantList({2, 3}));
        QVERIFY(view.isFlashed(view.chatModel()->index(3)));
        view.searchNext(true);
        view.searchNext(true); // reboucle sur le plus récent
        QCOMPARE(result.last(), QVariantList({1, 3}));
        view.searchNext(false); // plus récent que le plus récent : le plus ancien
        QCOMPARE(result.last(), QVariantList({3, 3}));
        view.setSearchTerm("introuvable");
        QCOMPARE(result.last(), QVariantList({0, 0}));
        view.setSearchTerm(QString());
        QCOMPARE(view.searchTerm(), QString());
    }

    void unreadMarker()
    {
        ChatView view;
        ChatModel *model = view.chatModel();
        const QDateTime t0 = QDateTime::currentDateTime();
        view.addMessage(message("lu", t0));
        view.addMessage(message("non lu 1", t0.addSecs(10)));
        view.markUnread(t0.addSecs(10));
        view.addMessage(message("non lu 2", t0.addSecs(20)));
        view.markUnread(t0.addSecs(20)); // même absence : le séparateur ne bouge pas
        QCOMPARE(model->markerRow(), 1);
        QCOMPARE(model->entry(1).kind, ChatEntry::Marker);
        QCOMPARE(model->entry(2).text, QString("non lu 1"));
        QVERIFY(model->search("Nouveaux").isEmpty()); // pas un message

        view.revealUnreadMarker(); // retour sur la fenêtre : il reste affiché…
        QCOMPARE(model->markerRow(), 1);
        view.addMessage(message("plus tard", t0.addSecs(30)));
        view.markUnread(t0.addSecs(30)); // …et se déplace à la prochaine absence
        QCOMPARE(model->rowCount(), 5);
        QCOMPARE(model->markerRow(), 3);
        QCOMPARE(model->entry(4).text, QString("plus tard"));

        view.clearUnreadMarker(); // on a écrit : plus de séparateur
        QCOMPARE(model->markerRow(), -1);
        QCOMPARE(model->rowCount(), 4);
        view.clearUnreadMarker(); // sans effet
        QCOMPARE(model->rowCount(), 4);
    }

    void downButtonCountsNewMessages()
    {
        ChatView view;
        view.resize(480, 300);
        const QDateTime t0 = QDateTime::currentDateTime();
        for (int i = 0; i < 30; ++i)
            view.addMessage(message(QStringLiteral("message %1").arg(i), t0.addSecs(i * 600)));
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        view.scrollToBottom();
        QTRY_VERIFY(!view.isDownButtonVisible());

        view.scrollToTop(); // on remonte lire l'historique
        QTRY_VERIFY(view.isDownButtonVisible());
        QCOMPARE(view.newMessagesBelow(), 0);
        view.addMessage(message("nouveau", t0.addSecs(30 * 600)));
        view.addMessage(message("encore", t0.addSecs(31 * 600)));
        QCOMPARE(view.newMessagesBelow(), 2);
        QTRY_VERIFY(view.verticalScrollBar()->value() < 100); // pas de défilement forcé

        view.scrollToBottom();
        QTRY_VERIFY(!view.isDownButtonVisible());
        QCOMPARE(view.newMessagesBelow(), 0);
    }

    void clickOnLink()
    {
        ChatView view;
        view.resize(480, 300);
        view.addMessage(message("Le site : https://exemple.fr/page, à voir", QDateTime::currentDateTime()));
        QCOMPARE(view.chatModel()->entry(0).links.size(), 1);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));

        auto *delegate = qobject_cast<ChatDelegate *>(view.itemDelegate());
        QSignalSpy link(&view, &ChatView::linkClicked);
        const QModelIndex idx = view.chatModel()->index(0);
        QStyleOptionViewItem opt;
        opt.rect = view.visualRect(idx);
        int hits = 0, misses = 0;
        for (int y = opt.rect.top(); y < opt.rect.bottom(); y += 2)
            for (int x = opt.rect.left(); x < opt.rect.right(); x += 4) {
                const auto kind = delegate->hitTest(idx, opt.rect, QPoint(x, y)).kind;
                (kind == ChatDelegate::Hit::Link ? hits : misses)++;
            }
        QVERIFY(hits > 0);
        QVERIFY(misses > hits); // seul le lien est cliquable, pas toute la bulle

        // Clic au centre du premier point cliquable trouvé
        for (int y = opt.rect.top(); y < opt.rect.bottom() && link.isEmpty(); y += 2)
            for (int x = opt.rect.left(); x < opt.rect.right() && link.isEmpty(); x += 4)
                if (delegate->hitTest(idx, opt.rect, QPoint(x, y)).kind == ChatDelegate::Hit::Link) {
                    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(x, y), Qt::LeftButton, Qt::NoButton,
                                        Qt::NoModifier);
                    delegate->editorEvent(&release, view.model(), opt, idx);
                }
        QCOMPARE(link.count(), 1);
        QCOMPARE(link.first().first().toString(), QString("https://exemple.fr/page"));
    }

    void reactions()
    {
        ChatView view;
        view.resize(480, 400);
        ChatEntry m = message("À voter !", QDateTime::currentDateTime());
        m.id = "m";
        view.addMessage(m);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        const QModelIndex idx = view.chatModel()->index(0);
        const int before = view.visualRect(idx).height();

        view.setReactions("m", {{QStringLiteral("👍"), 2, true, "Vous, Bob"}, {QStringLiteral("🎉"), 1, false, "Bob"}});
        QCOMPARE(view.chatModel()->entry(0).reactions.size(), 2);
        QTRY_VERIFY(view.visualRect(idx).height() > before); // les pastilles prennent de la place

        // Clic sur la pastille 🎉 : bascule notre réaction ; l'infobulle dit qui a réagi
        auto *delegate = qobject_cast<ChatDelegate *>(view.itemDelegate());
        QSignalSpy toggled(&view, &ChatView::reactionToggled);
        QStyleOptionViewItem opt;
        opt.rect = view.visualRect(idx);
        QStringList seen;
        for (int y = opt.rect.top(); y < opt.rect.bottom(); y += 2)
            for (int x = opt.rect.left(); x < opt.rect.right(); x += 3) {
                const ChatDelegate::Hit hit = delegate->hitTest(idx, opt.rect, QPoint(x, y));
                if (hit.kind == ChatDelegate::Hit::Reaction && !seen.contains(hit.emoji)) {
                    seen << hit.emoji;
                    QCOMPARE(hit.who, hit.emoji == QStringLiteral("👍") ? QString("Vous, Bob") : QString("Bob"));
                    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(x, y), Qt::LeftButton, Qt::NoButton,
                                        Qt::NoModifier);
                    delegate->editorEvent(&release, view.model(), opt, idx);
                }
            }
        QCOMPARE(seen, QStringList({QStringLiteral("👍"), QStringLiteral("🎉")}));
        QCOMPARE(toggled.count(), 2);
        QCOMPARE(toggled.at(1), QVariantList({"m", QStringLiteral("🎉")}));

        view.setReactions("inconnu", {{QStringLiteral("👍"), 1, false, "x"}}); // sans effet
        view.setReactions("m", {});
        QTRY_COMPARE(view.visualRect(idx).height(), before);
        view.grab();
    }

    void multilineMessageIsTaller()
    {
        ChatView view;
        view.resize(480, 400);
        // Même jour, assez espacés pour ne pas être regroupés : seules les lignes de texte diffèrent
        const QDateTime t0(QDate::currentDate(), QTime(9, 0));
        view.addMessage(message("premier", t0));
        view.addMessage(message("une ligne", t0.addSecs(3600)));
        view.addMessage(message("trois\nlignes\nici", t0.addSecs(7200)));
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        const int one = view.visualRect(view.chatModel()->index(1)).height();
        const int three = view.visualRect(view.chatModel()->index(2)).height();
        QVERIFY2(three > one + 20, qPrintable(QString("%1 / %2").arg(one).arg(three)));
    }

    void codeMarkup()
    {
        ChatView view;
        view.addMessage(message("lance `make` puis ```\nsudo reboot\n```", QDateTime::currentDateTime()));
        const ChatEntry &e = view.chatModel()->entry(0);
        QCOMPARE(e.markup.code.size(), 2);
        QVERIFY(e.markup.code.at(1).block);
        QCOMPARE(e.text.mid(e.markup.code.at(0).start, e.markup.code.at(0).length), QString("make"));
        view.grab(); // dessin du code et des délimiteurs masqués
    }

    void verifiedAuthor()
    {
        ChatView view;
        const QDateTime t0 = QDateTime::currentDateTime();
        ChatEntry fromAlice = message("bonjour", t0);
        fromAlice.authorOnion = "alice";
        view.addMessage(fromAlice);
        ChatEntry fromBob = message("salut", t0.addSecs(1));
        fromBob.author = "Bob";
        fromBob.authorOnion = "bob";
        view.addMessage(fromBob);
        QSignalSpy changed(view.chatModel(), &ChatModel::dataChanged);
        view.setAuthorVerified("alice", true);
        QVERIFY(view.chatModel()->entry(0).authorVerified);
        QVERIFY(!view.chatModel()->entry(1).authorVerified);
        QCOMPARE(changed.count(), 1);
        view.setAuthorVerified("alice", true); // déjà vérifié : rien à redessiner
        QCOMPARE(changed.count(), 1);
        view.setAuthorVerified(QString(), true); // nos messages n'ont pas d'onion
        QVERIFY(!view.chatModel()->entry(1).authorVerified);
        view.grab();
    }

    void hoverBar()
    {
        ChatView view;
        view.resize(480, 400);
        const QDateTime t0 = QDateTime::currentDateTime();
        ChatEntry m = message("Survolez-moi", t0);
        m.id = "m";
        m.reactions = {{QStringLiteral("❤️"), 1, true, "Vous"}};
        view.addMessage(m);
        view.addSystem("ligne système");
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        auto hover = [&](int row) {
            const QPoint pos = view.visualRect(view.chatModel()->index(row)).center();
            QMouseEvent move(QEvent::MouseMove, pos, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(view.viewport(), &move);
        };

        hover(0);
        QVERIFY(view.isHoverBarVisible());
        // À cheval sur le haut de la bulle, dans la vue
        QVERIFY(view.viewport()->rect().contains(view.hoverBar()->geometry()));
        const auto buttons = view.hoverBar()->findChildren<QToolButton *>();
        QCOMPARE(buttons.size(), 7); // 6 réactions + Répondre
        QToolButton *heart = nullptr;
        for (QToolButton *b : buttons)
            if (b->property("emoji").toString() == QStringLiteral("❤️"))
                heart = b;
        QVERIFY(heart && heart->isChecked()); // notre réaction est cochée

        QSignalSpy toggled(&view, &ChatView::reactionToggled);
        buttons.first()->click();
        QCOMPARE(toggled.count(), 1);
        QCOMPARE(toggled.first(), QVariantList({"m", QStringLiteral("👍")}));
        QVERIFY(!view.isHoverBarVisible()); // fermée après le choix

        hover(0);
        QSignalSpy reply(&view, &ChatView::replyRequested);
        view.hoverBar()->findChild<QToolButton *>("hoverReply")->click();
        QCOMPARE(reply.count(), 1);
        QCOMPARE(reply.first(), QVariantList({"m", "Alice", "Survolez-moi"}));

        // Pas de barre sur une ligne système ; elle disparaît peu après avoir quitté le message
        hover(0);
        QVERIFY(view.isHoverBarVisible());
        hover(1);
        QVERIFY(view.isHoverBarVisible()); // le temps d'aller jusqu'à la barre
        QTRY_VERIFY(!view.isHoverBarVisible());
        hover(1);
        QVERIFY(!view.isHoverBarVisible());

        // Défilement : la barre serait mal placée, elle est masquée
        hover(0);
        QVERIFY(view.isHoverBarVisible());
        emit view.verticalScrollBar()->valueChanged(1);
        QVERIFY(!view.isHoverBarVisible());
        view.grab();
    }

    void animatedGif()
    {
        // GIF de 2 images de 1×1 dans un écran de 2×2 (cf. tst_attachment)
        QByteArray g("GIF89a\x02\x00\x02\x00\x80\x00\x00\xff\x00\x00\x00\x00\xff", 19);
        g.append("\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00", 19);
        for (int i = 0; i < 2; ++i) {
            g.append("\x21\xf9\x04\x04\x0a\x00\x00\x00", 8);
            g.append("\x2c\x00\x00\x00\x00\x01\x00\x01\x00\x00", 10);
            g.append("\x02\x02\x44\x01\x00", 5);
        }
        g.append(char(0x3b));

        ChatView view;
        view.resize(480, 400);
        ChatEntry e = message(QString(), QDateTime::currentDateTime());
        e.id = "gif";
        e.image = QImage::fromData(g);
        e.imageData = g;
        e.animated = true;
        QVERIFY(!e.image.isNull());
        view.addMessage(e);
        QVERIFY(view.animationFrame(view.chatModel()->entry(0)).isNull()); // pas affichée : pas animée
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        view.grab(); // le dessin lance l'animation
        QCOMPARE(view.runningAnimations(), 1);
        QVERIFY(!view.animationFrame(view.chatModel()->entry(0)).isNull());
        view.hide(); // fenêtre masquée : arrêtée
        QCOMPARE(view.runningAnimations(), 0);
        view.show();
        view.clearAll();
        QCOMPARE(view.runningAnimations(), 0);
    }

    void fileCard()
    {
        ChatView view;
        view.resize(480, 400);
        const QDateTime t0 = QDateTime::currentDateTime();
        ChatEntry m = message(QString(), t0); // fichier sans légende
        m.id = "m";
        m.file.id = QString(64, 'a');
        m.file.name = "rapport.pdf";
        m.file.mime = "application/pdf";
        m.file.size = 3 * 1024 * 1024;
        m.file.state = ChatEntry::File::NotDownloaded;
        m.file.total = 20;
        view.addMessage(m);
        ChatEntry reply = message("Merci !", t0.addSecs(5));
        reply.replyTo = "m";
        view.addMessage(reply);
        QCOMPARE(view.chatModel()->entry(1).replyText, QString("📎 rapport.pdf")); // citation d'un fichier
        QCOMPARE(ChatView::quotedText(view.chatModel()->entry(0)), QString("📎 rapport.pdf"));
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));

        auto *delegate = qobject_cast<ChatDelegate *>(view.itemDelegate());
        QSignalSpy requested(&view, &ChatView::fileRequested);
        const QModelIndex idx = view.chatModel()->index(0);
        auto clickCard = [&] {
            QStyleOptionViewItem opt;
            opt.rect = view.visualRect(idx);
            for (int y = opt.rect.top(); y < opt.rect.bottom(); y += 3)
                for (int x = opt.rect.left(); x < opt.rect.right(); x += 6)
                    if (delegate->hitTest(idx, opt.rect, QPoint(x, y)).kind == ChatDelegate::Hit::File) {
                        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(x, y), Qt::LeftButton, Qt::NoButton,
                                            Qt::NoModifier);
                        return delegate->editorEvent(&release, view.model(), opt, idx);
                    }
            return false;
        };
        QVERIFY(clickCard()); // pas encore là : télécharger
        QCOMPARE(requested.last(), QVariantList({m.file.id, QVariant::fromValue(ChatView::FileAction::Download)}));

        view.setFileState(m.file.id, ChatEntry::File::Downloading, 7, 20, "Alice, Bob");
        QCOMPARE(view.chatModel()->entry(0).file.received, 7);
        QCOMPARE(view.chatModel()->entry(0).file.sources, QString("Alice, Bob")); // affichées sous le nom
        const int before = requested.count();
        QVERIFY(clickCard()); // en cours : rien de plus
        QCOMPARE(requested.count(), before);
        view.grab(); // carte avec barre de progression

        view.setFileState(m.file.id, ChatEntry::File::Complete, 20, 20);
        QVERIFY(clickCard()); // arrivé : ouvrir
        QCOMPARE(requested.last(), QVariantList({m.file.id, QVariant::fromValue(ChatView::FileAction::Open)}));
        view.setFileState(QString(64, 'z'), ChatEntry::File::Complete, 1, 1); // autre fichier : sans effet
        QCOMPARE(view.chatModel()->entry(0).file.state, ChatEntry::File::Complete);
        view.grab();
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
