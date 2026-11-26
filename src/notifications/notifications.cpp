#include "notifications.h"

#include <QTimeZone>

Notifications::Mode Notifications::modeFromString(const QString &s)
{
    if (s == QLatin1String("forMe"))
        return ForMe;
    if (s == QLatin1String("silent"))
        return Silent;
    return All;
}

QString Notifications::modeToString(Mode mode)
{
    switch (mode) {
    case ForMe:
        return QStringLiteral("forMe");
    case Silent:
        return QStringLiteral("silent");
    case All:
        break;
    }
    return QStringLiteral("all");
}

bool Notifications::shouldNotify(Mode mode, qint64 pausedUntilMs, qint64 nowMs, bool forMe)
{
    if (nowMs < pausedUntilMs)
        return false;
    switch (mode) {
    case All:
        return true;
    case ForMe:
        return forMe;
    case Silent:
        return false;
    }
    return true;
}

QDateTime Notifications::tomorrowMorning(const QDateTime &now)
{
    return QDateTime(now.date().addDays(1), QTime(8, 0), now.timeZone()); // même fuseau que « maintenant »
}
