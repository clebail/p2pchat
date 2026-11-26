# Cœur de P2P Chat sans interface graphique : utilisé par l'application et par les tests (tests/)
QT += network gui # gui : QImage (attachment, avatar)
CONFIG += c++17
# Sources en UTF-8 : sans cette option, MSVC les lit en Windows-1252 et abîme les littéraux larges
# (QStringLiteral, u"…") qui contiennent des accents ou des émojis
win32-msvc*: QMAKE_CXXFLAGS += /utf-8
INCLUDEPATH += $$PWD
DEPENDPATH += $$PWD

SOURCES += \
    $$PWD/attachment.cpp \
    $$PWD/avatar.cpp \
    $$PWD/connectionpolicy.cpp \
    $$PWD/conversations.cpp \
    $$PWD/crypto.cpp \
    $$PWD/edits.cpp \
    $$PWD/filestore.cpp \
    $$PWD/governance.cpp \
    $$PWD/groupnode.cpp \
    $$PWD/groupstore.cpp \
    $$PWD/history.cpp \
    $$PWD/invitation.cpp \
    $$PWD/language.cpp \
    $$PWD/links.cpp \
    $$PWD/markup.cpp \
    $$PWD/notifications.cpp \
    $$PWD/mentions.cpp \
    $$PWD/peerconnection.cpp \
    $$PWD/protocol.cpp \
    $$PWD/reactions.cpp \
    $$PWD/safetynumber.cpp \
    $$PWD/singleinstance.cpp \
    $$PWD/spellchecker.cpp \
    $$PWD/switchboard.cpp

HEADERS += \
    $$PWD/attachment.h \
    $$PWD/avatar.h \
    $$PWD/connectionpolicy.h \
    $$PWD/conversations.h \
    $$PWD/crypto.h \
    $$PWD/edits.h \
    $$PWD/filestore.h \
    $$PWD/governance.h \
    $$PWD/groupnode.h \
    $$PWD/groupstore.h \
    $$PWD/history.h \
    $$PWD/invitation.h \
    $$PWD/language.h \
    $$PWD/links.h \
    $$PWD/markup.h \
    $$PWD/notifications.h \
    $$PWD/mentions.h \
    $$PWD/peerconnection.h \
    $$PWD/protocol.h \
    $$PWD/reactions.h \
    $$PWD/safetynumber.h \
    $$PWD/singleinstance.h \
    $$PWD/spellchecker.h \
    $$PWD/switchboard.h

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

# Hunspell (correcteur orthographique), facultatif : sans lui, l'application se compile sans correction.
# HUNSPELL_DIR (dossier avec include/ et lib/, celui de vcpkg sous Windows), sinon pkg-config (Linux, Homebrew).
HUNSPELL_DIR = $$(HUNSPELL_DIR)
!isEmpty(HUNSPELL_DIR) {
    for(name, $$list(hunspell-1.7 libhunspell hunspell)) {
        isEmpty(HUNSPELL_LIB): exists($$HUNSPELL_DIR/lib/$${name}.lib): HUNSPELL_LIB = $$name
        isEmpty(HUNSPELL_LIB): exists($$HUNSPELL_DIR/lib/lib$${name}.so): HUNSPELL_LIB = $$name
        isEmpty(HUNSPELL_LIB): exists($$HUNSPELL_DIR/lib/lib$${name}.dylib): HUNSPELL_LIB = $$name
    }
    isEmpty(HUNSPELL_LIB) {
        warning("Hunspell introuvable dans $$HUNSPELL_DIR/lib : pas de correcteur orthographique")
    } else {
        INCLUDEPATH += $$HUNSPELL_DIR/include/hunspell
        LIBS += -L$$HUNSPELL_DIR/lib -l$$HUNSPELL_LIB
        DEFINES += P2P_SPELLCHECK
    }
} else:!win32-msvc* { # pkg-config : Linux, Homebrew, MSYS2
    CONFIG += link_pkgconfig
    packagesExist(hunspell) {
        PKGCONFIG += hunspell
        DEFINES += P2P_SPELLCHECK
    } else {
        message("Hunspell absent (paquet libhunspell-dev, ou brew install hunspell) : pas de correcteur orthographique")
    }
}
