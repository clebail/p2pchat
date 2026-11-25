# Cœur de P2P Chat sans interface graphique : utilisé par l'application et par les tests (tests/)
QT += network
CONFIG += c++17
INCLUDEPATH += $$PWD
DEPENDPATH += $$PWD

SOURCES += \
    $$PWD/connectionpolicy.cpp \
    $$PWD/crypto.cpp \
    $$PWD/groupstore.cpp \
    $$PWD/history.cpp \
    $$PWD/invitation.cpp \
    $$PWD/peerconnection.cpp \
    $$PWD/protocol.cpp

HEADERS += \
    $$PWD/connectionpolicy.h \
    $$PWD/crypto.h \
    $$PWD/groupstore.h \
    $$PWD/history.h \
    $$PWD/invitation.h \
    $$PWD/peerconnection.h \
    $$PWD/protocol.h

# OpenSSL (libcrypto) : X25519, Ed25519, HKDF, ChaCha20-Poly1305
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
