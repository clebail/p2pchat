#include "avatar.h"
#include "conversation.h"
#include "crypto.h"
#include "filestore.h"
#include "notifications.h"

#include <QApplication>
#include <QFile>
#include <QNetworkProxy>
#include <QSettings>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

#include <map>
#include <memory>

// Ce que la fenêtre montre d'une conversation (cf. conversation.h), sur de vrais groupes reliés en local
class TestConversation : public QObject
{
    Q_OBJECT

private:
    struct Person
    {
        QString onion, nick;
        std::unique_ptr<QSettings> settings;
        std::unique_ptr<Conversation> conv;
    };
    std::unique_ptr<QTemporaryDir> m_dir;
    std::map<char, Person> m_people;

    static GroupNode::Timing fastTiming()
    {
        GroupNode::Timing t;
        t.retryIntervalMs = 200;
        t.firstDialDelayMs = 0;
        t.redialDelayMs = 50;
        t.presenceGraceMs = 300;
        t.presenceBatchMs = 30;
        t.typingShownMs = 500;
        t.fileTickMs = 20;
        return t;
    }

    Person &person(char name)
    {
        Person &p = m_people[name];
        if (p.conv)
            return p;
        p.onion = QString(56, QLatin1Char(name)) + ".onion";
        p.nick = QStringLiteral("Membre %1").arg(QChar(name).toUpper());
        p.settings = std::make_unique<QSettings>(m_dir->filePath(QStringLiteral("%1.ini").arg(name)),
                                                 QSettings::IniFormat);
        GroupStore(*p.settings).ensureIdentity();
        GroupStore(*p.settings).setNickname(p.nick);
        p.conv = std::make_unique<Conversation>(*p.settings, QString(16, '1'),
                                                m_dir->filePath(QStringLiteral("%1.jsonl").arg(name)), fastTiming());
        GroupNode &n = p.conv->node();
        [&] { QVERIFY(n.listen()); }();
        n.setFilesDir(m_dir->filePath(QStringLiteral("%1-fichiers").arg(name)));
        n.setDialer([this](const QString &onion) {
            auto *s = new QTcpSocket;
            s->setProxy(QNetworkProxy::NoProxy);
            quint16 port = 1;
            for (auto &[k, other] : m_people)
                if (other.onion == onion && other.conv)
                    port = other.conv->node().port();
            s->connectToHost(QHostAddress::LocalHost, port);
            return s;
        });
        return p;
    }

    Invitation invitation(Person &p)
    {
        GroupStore &s = p.conv->store();
        return {s.secret(), p.onion, p.nick, s.identityKey(), p.conv->node().groupId()};
    }

    // A crée la conversation, B la rejoint
    void pair()
    {
        Person &a = person('a'), &b = person('b');
        a.conv->node().createGroup();
        a.conv->node().setOnline(a.onion);
        QCOMPARE(b.conv->node().join(invitation(a)), GroupNode::JoinResult::Joined);
        b.conv->node().setOnline(b.onion);
        QTRY_VERIFY(a.conv->node().isConnected(b.onion) && b.conv->node().isConnected(a.onion));
    }

    static bool hasSystemLine(const Conversation &c, const QString &part)
    {
        const ChatModel *m = c.view()->chatModel();
        for (int i = 0; i < m->rowCount(); ++i)
            if (m->entry(i).kind == ChatEntry::System && m->entry(i).text.contains(part))
                return true;
        return false;
    }

    static const ChatEntry *lastMessage(const Conversation &c)
    {
        const ChatModel *m = c.view()->chatModel();
        for (int i = m->rowCount() - 1; i >= 0; --i)
            if (m->entry(i).kind == ChatEntry::Message)
                return &m->entry(i);
        return nullptr;
    }

private slots:
    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_dir->isValid());
    }

    void cleanup()
    {
        m_people.clear();
        m_dir.reset();
    }

    void blankThenNamed()
    {
        Person &a = person('a');
        QVERIFY(a.conv->isBlank());
        QCOMPARE(a.conv->displayName(), QString("Conversation vierge"));
        a.conv->node().createGroup();
        a.conv->node().setOnline(a.onion);
        QVERIFY(!a.conv->isBlank());
        QCOMPARE(a.conv->displayName(), QString("Nouvelle conversation"));
        QVERIFY(a.conv->subtitle().contains("En attente de la première personne invitée"));

        pair();
        Person &b = person('b');
        QCOMPARE(a.conv->displayName(), b.nick); // les pseudos des membres
        QCOMPARE(b.conv->displayName(), a.nick);
        // Nom choisi par l'administrateur, le même pour tous
        QVERIFY(!b.conv->node().setSharedName("Chez A")); // pas administrateur
        QVERIFY(a.conv->node().setSharedName("Les amis"));
        QTRY_COMPARE(b.conv->displayName(), QString("Les amis"));
        QVERIFY(hasSystemLine(*b.conv, "a nommé la conversation « Les amis »"));
        // Un nom local d'une version précédente ne masque plus celui du groupe…
        b.conv->store().setName("Chez A");
        QCOMPARE(b.conv->displayName(), QString("Les amis"));
        // …et au départ, c'est le dernier nom connu qui reste
        b.conv->leave();
        QCOMPARE(b.conv->displayName(), QString("Les amis"));
    }

    void conversationAvatar()
    {
        pair();
        Person &a = person('a'), &b = person('b');
        QVERIFY(b.conv->conversationAvatar().isNull());

        // Sans image : même couleur chez chacun (tirée du groupe), bien que chacun voie un autre nom
        QTRY_COMPARE(b.conv->node().groupId(), a.conv->node().groupId());
        QVERIFY(a.conv->displayName() != b.conv->displayName());
        auto edge = [](const Conversation &c) { return c.avatarPixmap(40, 1).toImage().pixel(5, 20); };
        QCOMPARE(edge(*a.conv), edge(*b.conv));
        // Avec un nom partagé : le même rond, initiale comprise
        QVERIFY(a.conv->node().setSharedName("Les amis"));
        QTRY_COMPARE(b.conv->displayName(), QString("Les amis"));
        QCOMPARE(a.conv->avatarPixmap(40, 1).toImage(), b.conv->avatarPixmap(40, 1).toImage());

        QImage img(Avatar::Side, Avatar::Side, QImage::Format_RGB32);
        img.fill(Qt::magenta);
        QVERIFY(!b.conv->node().setConversationAvatar(Avatar::fromImage(img))); // pas administrateur
        QVERIFY(a.conv->node().setConversationAvatar(Avatar::fromImage(img)));
        QTRY_VERIFY(!b.conv->conversationAvatar().isNull());
        QCOMPARE(b.conv->conversationAvatar().size(), QSize(Avatar::Side, Avatar::Side));
        QVERIFY(hasSystemLine(*b.conv, "a changé l'avatar de la conversation"));
        QVERIFY(a.conv->node().setConversationAvatar({}));
        QTRY_VERIFY(b.conv->conversationAvatar().isNull());
        QVERIFY(hasSystemLine(*b.conv, "a retiré l'avatar de la conversation"));
        QVERIFY(a.conv->node().setConversationAvatar(Avatar::fromImage(img)));
        QTRY_VERIFY(!b.conv->conversationAvatar().isNull());
        b.conv->leave(); // quitté : l'avatar part avec le groupe
        QVERIFY(b.conv->conversationAvatar().isNull());
    }

    void messagesPreviewAndNotifications()
    {
        pair();
        Person &a = person('a'), &b = person('b');
        QSignalSpy arrived(a.conv.get(), &Conversation::messagesArrived);
        QSignalSpy changed(a.conv.get(), &Conversation::changed);

        b.conv->node().sendMessage("Bonjour à tous");
        QTRY_COMPARE(arrived.count(), 1);
        QCOMPARE(arrived.last().at(0).toInt(), 1);
        QCOMPARE(arrived.last().at(1).toString(), b.nick); // titre : l'auteur
        QCOMPARE(arrived.last().at(2).toString(), QString("Bonjour à tous"));
        QCOMPARE(arrived.last().at(4).toBool(), false); // pas pour nous en particulier
        QCOMPARE(a.conv->preview(), b.nick + " : Bonjour à tous");
        QVERIFY(changed.count() > 0);
        QCOMPARE(lastMessage(*a.conv)->author, b.nick);

        // Mention : pour nous
        b.conv->node().sendMessage("@" + a.nick + " tu es là ?");
        QTRY_COMPARE(arrived.count(), 2);
        QCOMPARE(arrived.last().at(1).toString(), b.nick + " vous a mentionné(e)");
        QCOMPARE(arrived.last().at(4).toBool(), true);
        QVERIFY(lastMessage(*a.conv)->mentionsMe);

        // Réponse à l'un de nos messages : pour nous aussi
        const QString mine = Protocol::Message::id(a.conv->node().sendMessage("Oui !"));
        QCOMPARE(a.conv->preview(), QString("Vous : Oui !"));
        QTRY_VERIFY(lastMessage(*b.conv) && lastMessage(*b.conv)->text == "Oui !");
        b.conv->node().sendMessage("Super", {mine, {}, {}});
        QTRY_COMPARE(arrived.count(), 3);
        QCOMPARE(arrived.last().at(1).toString(), b.nick + " vous a répondu");
        QCOMPARE(arrived.last().at(4).toBool(), true);
    }

    void editedAndDeletedMessages()
    {
        pair();
        Person &a = person('a'), &b = person('b');
        const QString id = Protocol::Message::id(b.conv->node().sendMessage("Coucou @" + a.nick));
        QTRY_VERIFY(lastMessage(*a.conv) && lastMessage(*a.conv)->id == id);
        QVERIFY(lastMessage(*a.conv)->mentionsMe);

        QVERIFY(b.conv->node().editMessage(id, "Coucou tout le monde"));
        QTRY_COMPARE(lastMessage(*a.conv)->text, QString("Coucou tout le monde"));
        QVERIFY(lastMessage(*a.conv)->edited);
        QVERIFY(!lastMessage(*a.conv)->mentionsMe); // la mention a disparu
        QCOMPARE(a.conv->preview(), b.nick + " : Coucou tout le monde");
        QCOMPARE(lastMessage(*b.conv)->text, QString("Coucou tout le monde")); // chez l'auteur aussi

        QVERIFY(b.conv->node().deleteMessage(id));
        QTRY_VERIFY(lastMessage(*a.conv)->deleted);
        QCOMPARE(lastMessage(*a.conv)->text, QString("🚫 Message supprimé"));
        QVERIFY(!lastMessage(*a.conv)->edited);
        QCOMPARE(a.conv->preview(), b.nick + " : 🚫 Message supprimé");

        // Rechargé depuis l'historique : déjà supprimé à l'affichage
        Conversation reloaded(*a.settings, QString(16, '1'), m_dir->filePath("a.jsonl"), fastTiming());
        reloaded.node().setFilesDir(m_dir->filePath("a-fichiers-2"));
        reloaded.loadHistory();
        QVERIFY(lastMessage(reloaded) && lastMessage(reloaded)->deleted);
    }

    void moderationIsNamed()
    {
        pair();
        Person &a = person('a'), &b = person('b');
        QTRY_COMPARE(b.conv->node().groupId(), a.conv->node().groupId());
        const QString id = Protocol::Message::id(b.conv->node().sendMessage("Hors sujet"));
        QTRY_VERIFY(lastMessage(*a.conv) && lastMessage(*a.conv)->id == id);
        QVERIFY(a.conv->node().editMessage(id, "[modéré]"));
        QTRY_COMPARE(lastMessage(*b.conv)->text, QString("[modéré]"));
        QCOMPARE(lastMessage(*b.conv)->editedBy, a.nick);
        QVERIFY(lastMessage(*a.conv)->editedBy == "vous");
        QVERIFY(a.conv->node().deleteMessage(id));
        QTRY_COMPARE(lastMessage(*b.conv)->text, "🚫 Message supprimé par " + a.nick);
    }

    void unreadAndNotificationSettings()
    {
        Person &a = person('a');
        QSignalSpy changed(a.conv.get(), &Conversation::changed);
        a.conv->addUnread(3, QDateTime::currentMSecsSinceEpoch());
        QCOMPARE(a.conv->unread(), 3);
        QCOMPARE(changed.count(), 1);
        a.conv->markRead();
        QCOMPARE(a.conv->unread(), 0);
        QCOMPARE(changed.count(), 2);
        a.conv->markRead(); // déjà lu : rien de neuf
        QCOMPARE(changed.count(), 2);

        // Réglage propre à la conversation ; pause commune
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        QVERIFY(a.conv->notificationsAllowed(false, 0, now));
        a.conv->store().setNotifyMode(Notifications::modeToString(Notifications::ForMe));
        QVERIFY(!a.conv->notificationsAllowed(false, 0, now));
        QVERIFY(a.conv->notificationsAllowed(true, 0, now));
        QVERIFY(!a.conv->notificationsAllowed(true, now + 60000, now));
    }

    void subtitleFollowsPresenceAndTyping()
    {
        pair();
        Person &a = person('a'), &b = person('b');
        QTRY_VERIFY(a.conv->subtitle().contains("En ligne"));
        QVERIFY(a.conv->subtitleToolTip().contains(b.nick.toHtmlEscaped() + " : en ligne"));
        b.conv->node().typing();
        QTRY_COMPARE(a.conv->typingText(), b.nick + " écrit…");
        QVERIFY(a.conv->subtitle().contains(b.nick + " écrit…"));
        QTRY_VERIFY(a.conv->typingText().isEmpty()); // délai d'affichage écoulé
        b.conv->node().setOffline();
        QTRY_VERIFY(a.conv->subtitle().contains("Personne n'est en ligne"));
        a.conv->leave();
        QVERIFY(a.conv->subtitle().contains("Vous avez quitté cette conversation"));
        QCOMPARE(a.conv->displayName(), b.nick); // le nom reste, même sans membres
    }

    void exclusionIsShownOnBothSides()
    {
        pair();
        Person &a = person('a'), &b = person('b');
        QTRY_COMPARE(b.conv->node().groupId(), a.conv->node().groupId());
        QSignalSpy excluded(b.conv.get(), &Conversation::excludedFromGroup);
        QVERIFY(a.conv->node().exclude(b.onion));
        QTRY_COMPARE(excluded.count(), 1);
        QCOMPARE(excluded.first().first().toString(), a.nick);
        QVERIFY(hasSystemLine(*b.conv, "vous a exclu du groupe"));
        QVERIFY(hasSystemLine(*a.conv, b.nick + " ne fait plus partie du groupe"));
        QVERIFY(hasSystemLine(*a.conv, "Nouveau secret du groupe"));
        QCOMPARE(b.conv->displayName(), a.nick); // exclu : la conversation garde son nom
    }

    void fileActionsFromTheView()
    {
        pair();
        Person &a = person('a'), &b = person('b');
        const QString path = m_dir->filePath("gros.bin");
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        const QByteArray content = Crypto::randomBytes(3 * 1024 * 1024); // pas téléchargé automatiquement
        f.write(content);
        f.close();
        b.conv->node().sendFile(path);
        QTRY_VERIFY(lastMessage(*a.conv) && !lastMessage(*a.conv)->file.id.isEmpty());
        const QString id = lastMessage(*a.conv)->file.id;
        QCOMPARE(lastMessage(*a.conv)->file.state, ChatEntry::File::NotDownloaded);
        QCOMPARE(a.conv->preview(), b.nick + " : 📎 gros.bin");

        // Télécharger : géré par la conversation ; ouvrir : demandé à la fenêtre
        emit a.conv->view()->fileRequested(id, ChatView::FileAction::Download);
        QTRY_COMPARE(lastMessage(*a.conv)->file.state, ChatEntry::File::Complete);
        QSignalSpy open(a.conv.get(), &Conversation::openFileRequested);
        emit a.conv->view()->fileRequested(id, ChatView::FileAction::Open);
        QCOMPARE(open.count(), 1);
        QCOMPARE(open.first().first().toString(), id);
    }
};

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    TestConversation t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_conversation.moc"
