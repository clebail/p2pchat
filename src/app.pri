# Fenêtre principale, Tor, notifications natives, console partagée : tout l'application sauf main.cpp.
# Utilisé par p2pchat.pro et par le test de la fenêtre (tests/tst_mainwindow).
include($$PWD/core.pri)
include($$PWD/ui.pri)
QT += widgets network
INCLUDEPATH += $$PWD/app $$PWD/network $$PWD/notifications
RESOURCES += $$PWD/../resources/resources.qrc # icône de l'application (fenêtre, page d'accueil)

SOURCES += $$PWD/app/mainwindow.cpp $$PWD/app/mainwindow_ui.cpp $$PWD/app/mainwindow_dialogs.cpp \
           $$PWD/app/appstyle.cpp $$PWD/network/torservice.cpp
HEADERS += $$PWD/app/mainwindow.h $$PWD/app/appstyle.h $$PWD/notifications/notifier.h $$PWD/network/torservice.h

# Notifications natives
macx {
    OBJECTIVE_SOURCES += $$PWD/notifications/notifier_mac.mm
    QMAKE_OBJECTIVE_CFLAGS += -fobjc-arc
    LIBS += -framework AppKit -framework UserNotifications
} else {
    SOURCES += $$PWD/notifications/notifier.cpp
    unix: QT += dbus
}

# Console partagée (Linux et macOS) : pseudo-terminal + fenêtre de terminal
unix {
    DEFINES += P2P_TERMINAL
    INCLUDEPATH += $$PWD/terminal
    SOURCES += $$PWD/terminal/terminalsession.cpp $$PWD/terminal/terminalsharing.cpp $$PWD/terminal/terminalclient.cpp
    HEADERS += $$PWD/terminal/terminalsession.h $$PWD/terminal/terminalsharing.h $$PWD/terminal/terminalclient.h
    linux: LIBS += -lutil   # forkpty (glibc < 2.34) ; macOS : dans la libc
}
