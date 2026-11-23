QT += widgets network
CONFIG += c++17
TARGET = p2pchat
macx: TARGET = P2PChat

SOURCES += main.cpp mainwindow.cpp chatview.cpp peerconnection.cpp crypto.cpp invitation.cpp torservice.cpp
HEADERS += mainwindow.h chatview.h peerconnection.h crypto.h invitation.h torservice.h

RESOURCES += resources.qrc

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
