#include "notifier.h"

#include <QFile>
#include <QIcon>
#include <QStandardPaths>
#include <QSystemTrayIcon>

#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
#  define P2P_DBUS_NOTIFY
#  include <QDBusConnection>
#  include <QDBusInterface>
#  include <QDBusPendingCallWatcher>
#  include <QDBusPendingReply>
#endif

#ifndef Q_OS_MACOS // la version macOS est dans notifier_mac.mm

namespace {
#ifdef P2P_DBUS_NOTIFY
const QString Service = QStringLiteral("org.freedesktop.Notifications");
const QString Path = QStringLiteral("/org/freedesktop/Notifications");
#endif
} // namespace

Notifier::Notifier(QObject *parent) : QObject(parent)
{
#ifdef P2P_DBUS_NOTIFY
    // Le serveur de notifications attend un fichier pour l'icône
    m_iconPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/p2pchat.png";
    if (!QFile::exists(m_iconPath))
        QFile::copy(":/icons/p2pchat.png", m_iconPath);
    QDBusConnection::sessionBus().connect(Service, Path, Service, "ActionInvoked", this,
                                          SLOT(onActionInvoked(uint, QString)));
#endif
}

Notifier::~Notifier() = default;

void Notifier::setTray(QSystemTrayIcon *tray)
{
    m_tray = tray;
#ifndef P2P_DBUS_NOTIFY
    if (tray)
        connect(tray, &QSystemTrayIcon::messageClicked, this, &Notifier::activated);
#endif
}

void Notifier::notify(const QString &title, const QString &body)
{
#ifdef P2P_DBUS_NOTIFY
    QDBusInterface iface(Service, Path, Service, QDBusConnection::sessionBus());
    if (iface.isValid()) {
        const QVariantMap hints{{"desktop-entry", "p2pchat"},
                                {"image-path", m_iconPath},
                                {"category", "im.received"}};
        const QDBusPendingCall call = iface.asyncCall("Notify", QStringLiteral("P2P Chat"), m_ids.value(title, 0u),
                                                      m_iconPath, title, body.toHtmlEscaped(),
                                                      QStringList{"default", tr("Ouvrir")}, hints, -1);
        auto *watcher = new QDBusPendingCallWatcher(call, this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, title](QDBusPendingCallWatcher *w) {
            const QDBusPendingReply<uint> reply = *w;
            if (reply.isValid())
                m_ids.insert(title, reply.value());
            w->deleteLater();
        });
        return;
    }
#endif
    if (m_tray && m_tray->isVisible())
        m_tray->showMessage(title, body, QIcon(":/icons/p2pchat.png"), 6000);
}

void Notifier::onActionInvoked(uint id, const QString &)
{
    for (auto it = m_ids.cbegin(); it != m_ids.cend(); ++it)
        if (it.value() == id) {
            emit activated();
            return;
        }
}

void Notifier::setBadge(int) {}

#endif // !Q_OS_MACOS
