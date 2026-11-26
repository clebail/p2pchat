#pragma once
#include <QHash>
#include <QObject>

class QSystemTrayIcon;

// Notifications natives du système, avec l'icône de l'application :
//  - Linux   : org.freedesktop.Notifications (D-Bus)
//  - macOS   : UserNotifications (centre de notifications) + pastille sur l'icône du Dock
//  - Windows : bulle de l'icône de la zone de notification
class Notifier : public QObject
{
    Q_OBJECT
public:
    explicit Notifier(QObject *parent = nullptr);
    ~Notifier() override;

    void notify(const QString &title, const QString &body);
    void setBadge(int count);              // macOS : pastille du Dock
    void setTray(QSystemTrayIcon *tray);   // Windows : support des bulles

signals:
    void activated(); // clic sur une notification

private slots:
    void onActionInvoked(uint id, const QString &action); // Linux (D-Bus)

private:
    QSystemTrayIcon *m_tray = nullptr;
    QHash<QString, uint> m_ids; // Linux : une notification par auteur, remplacée au fil des messages
    QString m_iconPath;
    void *m_platform = nullptr; // macOS : délégué Objective-C
};
