QT += widgets network
CONFIG += c++17
TARGET = p2pchat
macx: TARGET = P2PChat

# Cœur sans interface (protocole, crypto, groupe, historique), partagé avec les tests
include(core.pri)

# Composants de la conversation (liste, bulles, saisie), partagés avec les tests
include(ui.pri)

SOURCES += main.cpp mainwindow.cpp torservice.cpp
HEADERS += mainwindow.h notifier.h torservice.h

RESOURCES += resources.qrc

# Notifications natives
macx {
    OBJECTIVE_SOURCES += notifier_mac.mm
    QMAKE_OBJECTIVE_CFLAGS += -fobjc-arc
    LIBS += -framework AppKit -framework UserNotifications
    QMAKE_MACOSX_DEPLOYMENT_TARGET = 11.0
    QMAKE_TARGET_BUNDLE_PREFIX = org.p2pchat   # identifiant : org.p2pchat.P2PChat
} else {
    SOURCES += notifier.cpp
    unix: QT += dbus
}

# Console partagée (Linux et macOS) : pseudo-terminal + fenêtre de terminal
unix {
    DEFINES += P2P_TERMINAL
    SOURCES += terminalsession.cpp terminalclient.cpp
    HEADERS += terminalsession.h terminalclient.h
    linux: LIBS += -lutil   # forkpty (glibc < 2.34) ; macOS : dans la libc
}

# Linux : « make install » => menu des applications + icône (notifications, barre des tâches)
unix:!macx {
    isEmpty(PREFIX): PREFIX = $$(HOME)/.local
    target.path = $$PREFIX/bin
    desktop.path = $$PREFIX/share/applications
    desktop.files = p2pchat.desktop
    appicon.path = $$PREFIX/share/icons/hicolor/256x256/apps
    appicon.files = icons/p2pchat.png
    INSTALLS += target desktop appicon
}

# Icône de l'application
macx: ICON = icons/p2pchat.icns
win32: RC_ICONS = icons/p2pchat.ico
QMAKE_TARGET_DESCRIPTION = "P2P Chat"
# OpenSSL (libcrypto) : configuré dans core.pri
