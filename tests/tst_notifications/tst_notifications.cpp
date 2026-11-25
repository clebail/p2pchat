#include "notifications.h"

#include <QtTest>

using namespace Notifications;

class TestNotifications : public QObject
{
    Q_OBJECT

private slots:
    void shouldNotify_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<qint64>("pausedUntil");
        QTest::addColumn<bool>("forMe");
        QTest::addColumn<bool>("expected");
        const qint64 now = 1000;
        QTest::newRow("tous") << int(All) << qint64(0) << false << true;
        QTest::newRow("pour moi, message ordinaire") << int(ForMe) << qint64(0) << false << false;
        QTest::newRow("pour moi, mention") << int(ForMe) << qint64(0) << true << true;
        QTest::newRow("aucune, même une mention") << int(Silent) << qint64(0) << true << false;
        QTest::newRow("en pause") << int(All) << now + 1 << true << false;
        QTest::newRow("pause terminée") << int(All) << now << false << true;
    }

    void shouldNotify()
    {
        QFETCH(int, mode);
        QFETCH(qint64, pausedUntil);
        QFETCH(bool, forMe);
        QFETCH(bool, expected);
        QCOMPARE(Notifications::shouldNotify(Mode(mode), pausedUntil, 1000, forMe), expected);
    }

    void settingsValues()
    {
        for (Mode m : {All, ForMe, Silent})
            QCOMPARE(modeFromString(modeToString(m)), m);
        QCOMPARE(modeFromString(QString()), All); // jamais réglé
        QCOMPARE(modeFromString("inconnu"), All);
    }

    void tomorrowMorning()
    {
        const QDateTime evening(QDate(2026, 11, 25), QTime(22, 30));
        QCOMPARE(Notifications::tomorrowMorning(evening), QDateTime(QDate(2026, 11, 26), QTime(8, 0)));
        const QDateTime night(QDate(2026, 12, 31), QTime(0, 30)); // après minuit : le lendemain quand même
        QCOMPARE(Notifications::tomorrowMorning(night), QDateTime(QDate(2027, 1, 1), QTime(8, 0)));
    }
};

QTEST_GUILESS_MAIN(TestNotifications)
#include "tst_notifications.moc"
