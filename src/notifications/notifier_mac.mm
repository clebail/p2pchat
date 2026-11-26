// Notifications macOS : framework UserNotifications (macOS 10.14+).
// L'icône affichée est celle du bundle (icons/p2pchat.icns).
#include "notifier.h"

#import <AppKit/AppKit.h>
#import <UserNotifications/UserNotifications.h>

#include <QMetaObject>
#include <QPointer>

@interface P2PNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@property (nonatomic, assign) QPointer<Notifier> *owner;
@end

@implementation P2PNotificationDelegate

// Afficher la bannière même si l'application est au premier plan (fenêtre masquée)
- (void)userNotificationCenter:(UNUserNotificationCenter *)center
       willPresentNotification:(UNNotification *)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler
{
    Q_UNUSED(center);
    Q_UNUSED(notification);
    // macOS 11 minimum (QMAKE_MACOSX_DEPLOYMENT_TARGET) : bannière et centre de notifications
    completionHandler(UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionList
                      | UNNotificationPresentationOptionSound);
}

// Clic sur la notification : on rouvre la fenêtre
- (void)userNotificationCenter:(UNUserNotificationCenter *)center
    didReceiveNotificationResponse:(UNNotificationResponse *)response
             withCompletionHandler:(void (^)(void))completionHandler
{
    Q_UNUSED(center);
    Q_UNUSED(response);
    if (self.owner && *self.owner) {
        Notifier *n = *self.owner;
        QMetaObject::invokeMethod(n, [n] { emit n->activated(); }, Qt::QueuedConnection);
    }
    completionHandler();
}

@end

namespace {

// UserNotifications plante hors d'un bundle .app (ex. binaire lancé directement)
bool inBundle()
{
    return [[NSBundle mainBundle] bundleIdentifier] != nil;
}

struct MacState
{
    P2PNotificationDelegate *delegate = nil;
    QPointer<Notifier> owner;
};

} // namespace

Notifier::Notifier(QObject *parent) : QObject(parent)
{
    if (!inBundle())
        return;
    auto *state = new MacState;
    state->owner = this;
    state->delegate = [[P2PNotificationDelegate alloc] init];
    state->delegate.owner = &state->owner;
    m_platform = state;

    UNUserNotificationCenter *center = [UNUserNotificationCenter currentNotificationCenter];
    center.delegate = state->delegate;
    [center requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound
                                             | UNAuthorizationOptionBadge)
                          completionHandler:^(BOOL granted, NSError *error) {
                              Q_UNUSED(granted);
                              Q_UNUSED(error);
                          }];
}

Notifier::~Notifier()
{
    auto *state = static_cast<MacState *>(m_platform);
    if (!state)
        return;
    [UNUserNotificationCenter currentNotificationCenter].delegate = nil;
    state->delegate = nil; // ARC libère le délégué
    delete state;
}

void Notifier::setTray(QSystemTrayIcon *tray)
{
    m_tray = tray;
}

void Notifier::notify(const QString &title, const QString &body)
{
    if (!m_platform)
        return;
    UNMutableNotificationContent *content = [[UNMutableNotificationContent alloc] init];
    content.title = title.toNSString();
    content.body = body.toNSString();
    content.sound = [UNNotificationSound defaultSound];
    content.threadIdentifier = title.toNSString(); // regroupement par auteur
    UNNotificationRequest *request = [UNNotificationRequest requestWithIdentifier:[[NSUUID UUID] UUIDString]
                                                                          content:content
                                                                          trigger:nil];
    [[UNUserNotificationCenter currentNotificationCenter] addNotificationRequest:request withCompletionHandler:nil];
}

void Notifier::setBadge(int count)
{
    [[NSApp dockTile] setBadgeLabel:(count > 0 ? QString::number(count).toNSString() : nil)];
}

void Notifier::onActionInvoked(uint, const QString &) {}
