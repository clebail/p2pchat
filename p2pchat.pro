QT += widgets network
CONFIG += c++17
TARGET = p2pchat
macx: TARGET = P2PChat

# Cœur sans interface (protocole, crypto, groupe, historique), partagé avec les tests
include(src/core.pri)

# Composants de la conversation (liste, bulles, saisie), partagés avec les tests
include(src/ui.pri)

# Fenêtre principale, Tor, notifications natives (cf. src/, un dossier par fonctionnalité)
SOURCES += src/app/main.cpp src/app/mainwindow.cpp src/app/mainwindow_dialogs.cpp src/app/appstyle.cpp \
           src/network/torservice.cpp
HEADERS += src/app/mainwindow.h src/app/appstyle.h src/notifications/notifier.h src/network/torservice.h

RESOURCES += resources/resources.qrc

# Traductions (textes écrits en français) : compilées par lrelease et embarquées (:/i18n/p2pchat_en.qm).
# Après avoir ajouté ou modifié des textes : lupdate p2pchat.pro, puis traduire avec Qt Linguist.
TRANSLATIONS += translations/p2pchat_en.ts
CONFIG += lrelease embed_translations

# Notifications natives
macx {
    OBJECTIVE_SOURCES += src/notifications/notifier_mac.mm
    QMAKE_OBJECTIVE_CFLAGS += -fobjc-arc
    LIBS += -framework AppKit -framework UserNotifications
    QMAKE_MACOSX_DEPLOYMENT_TARGET = 11.0
    QMAKE_TARGET_BUNDLE_PREFIX = org.p2pchat   # identifiant : org.p2pchat.P2PChat
} else {
    SOURCES += src/notifications/notifier.cpp
    unix: QT += dbus
}

# Console partagée (Linux et macOS) : pseudo-terminal + fenêtre de terminal
unix {
    DEFINES += P2P_TERMINAL
    INCLUDEPATH += src/terminal
    SOURCES += src/terminal/terminalsession.cpp src/terminal/terminalsharing.cpp src/terminal/terminalclient.cpp
    HEADERS += src/terminal/terminalsession.h src/terminal/terminalsharing.h src/terminal/terminalclient.h
    linux: LIBS += -lutil   # forkpty (glibc < 2.34) ; macOS : dans la libc
}

# Linux : « make install » => menu des applications + icône (notifications, barre des tâches)
unix:!macx {
    isEmpty(PREFIX): PREFIX = $$(HOME)/.local
    target.path = $$PREFIX/bin
    desktop.path = $$PREFIX/share/applications
    desktop.files = resources/p2pchat.desktop
    appicon.path = $$PREFIX/share/icons/hicolor/256x256/apps
    appicon.files = resources/icons/p2pchat.png
    INSTALLS += target desktop appicon
}

# Icône de l'application
macx: ICON = resources/icons/p2pchat.icns
win32: RC_ICONS = resources/icons/p2pchat.ico
QMAKE_TARGET_DESCRIPTION = "P2P Chat"
# OpenSSL (libcrypto) et Hunspell : configurés dans src/core.pri
