QT += widgets network
CONFIG += c++17
TARGET = p2pchat
macx: TARGET = P2PChat

SOURCES += main.cpp mainwindow.cpp chatview.cpp peerconnection.cpp crypto.cpp invitation.cpp torservice.cpp
HEADERS += mainwindow.h chatview.h notifier.h peerconnection.h crypto.h invitation.h torservice.h

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

# OpenSSL (libcrypto) : X25519, HKDF, ChaCha20-Poly1305
macx {
    # Homebrew : /opt/homebrew (Apple Silicon) ou /usr/local (Intel)
    OPENSSL_DIR = $$(OPENSSL_DIR)
    isEmpty(OPENSSL_DIR): exists(/opt/homebrew/opt/openssl@3): OPENSSL_DIR = /opt/homebrew/opt/openssl@3
    isEmpty(OPENSSL_DIR): OPENSSL_DIR = /usr/local/opt/openssl@3
    INCLUDEPATH += $$OPENSSL_DIR/include
    LIBS += -L$$OPENSSL_DIR/lib -lcrypto
} else: win32-msvc* {
    INCLUDEPATH += $$(OPENSSL_DIR)/include
    LIBS += -L$$(OPENSSL_DIR)/lib -llibcrypto
} else {
    LIBS += -lcrypto
}
