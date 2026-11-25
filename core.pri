# Cœur de P2P Chat sans interface graphique : utilisé par l'application et par les tests (tests/)
QT += network gui # gui : QImage (attachment)
CONFIG += c++17
INCLUDEPATH += $$PWD
DEPENDPATH += $$PWD

SOURCES += \
    $$PWD/attachment.cpp \
    $$PWD/connectionpolicy.cpp \
    $$PWD/crypto.cpp \
    $$PWD/groupnode.cpp \
    $$PWD/groupstore.cpp \
    $$PWD/history.cpp \
    $$PWD/invitation.cpp \
    $$PWD/links.cpp \
    $$PWD/markup.cpp \
    $$PWD/notifications.cpp \
    $$PWD/mentions.cpp \
    $$PWD/peerconnection.cpp \
    $$PWD/protocol.cpp \
    $$PWD/reactions.cpp \
    $$PWD/safetynumber.cpp \
    $$PWD/singleinstance.cpp

HEADERS += \
    $$PWD/attachment.h \
    $$PWD/connectionpolicy.h \
    $$PWD/crypto.h \
    $$PWD/groupnode.h \
    $$PWD/groupstore.h \
    $$PWD/history.h \
    $$PWD/invitation.h \
    $$PWD/links.h \
    $$PWD/markup.h \
    $$PWD/notifications.h \
    $$PWD/mentions.h \
    $$PWD/peerconnection.h \
    $$PWD/protocol.h \
    $$PWD/reactions.h \
    $$PWD/safetynumber.h \
    $$PWD/singleinstance.h

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
