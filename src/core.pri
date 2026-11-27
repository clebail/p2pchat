# Cœur de P2P Chat sans interface graphique : utilisé par l'application et par les tests (tests/)
!isEmpty(P2P_CORE_PRI): return() # déjà inclus (tests qui chargent aussi app.pri)
P2P_CORE_PRI = 1
QT += network gui # gui : QImage (attachment, avatar)
CONFIG += c++17
# Sources en UTF-8 : sans cette option, MSVC les lit en Windows-1252 et abîme les littéraux larges
# (QStringLiteral, u"…") qui contiennent des accents ou des émojis
win32-msvc*: QMAKE_CXXFLAGS += /utf-8
# Un dossier par fonctionnalité (cf. README, « Fichiers ») ; les #include restent sans chemin
CORE_DIRS = crypto files group messages network notifications text app
for(dir, CORE_DIRS) {
    INCLUDEPATH += $$PWD/$$dir
    DEPENDPATH += $$PWD/$$dir
}

SOURCES += \
    $$PWD/messages/attachment.cpp \
    $$PWD/messages/avatar.cpp \
    $$PWD/network/connectionpolicy.cpp \
    $$PWD/group/conversations.cpp \
    $$PWD/crypto/crypto.cpp \
    $$PWD/messages/edits.cpp \
    $$PWD/files/filestore.cpp \
    $$PWD/group/governance.cpp \
    $$PWD/group/groupnode.cpp \
    $$PWD/group/groupnode_files.cpp \
    $$PWD/group/groupnode_governance.cpp \
    $$PWD/group/groupstore.cpp \
    $$PWD/group/history.cpp \
    $$PWD/group/invitation.cpp \
    $$PWD/group/person.cpp \
    $$PWD/app/language.cpp \
    $$PWD/text/links.cpp \
    $$PWD/text/markup.cpp \
    $$PWD/notifications/notifications.cpp \
    $$PWD/text/mentions.cpp \
    $$PWD/network/peerconnection.cpp \
    $$PWD/network/protocol.cpp \
    $$PWD/messages/reactions.cpp \
    $$PWD/crypto/safetynumber.cpp \
    $$PWD/app/singleinstance.cpp \
    $$PWD/text/spellchecker.cpp \
    $$PWD/network/switchboard.cpp

HEADERS += \
    $$PWD/messages/attachment.h \
    $$PWD/messages/avatar.h \
    $$PWD/network/connectionpolicy.h \
    $$PWD/group/conversations.h \
    $$PWD/crypto/crypto.h \
    $$PWD/messages/edits.h \
    $$PWD/files/filestore.h \
    $$PWD/group/governance.h \
    $$PWD/group/groupnode.h \
    $$PWD/group/groupnode_p.h \
    $$PWD/group/groupstore.h \
    $$PWD/group/history.h \
    $$PWD/group/invitation.h \
    $$PWD/group/person.h \
    $$PWD/app/language.h \
    $$PWD/text/links.h \
    $$PWD/text/markup.h \
    $$PWD/notifications/notifications.h \
    $$PWD/text/mentions.h \
    $$PWD/network/peerconnection.h \
    $$PWD/network/protocol.h \
    $$PWD/messages/reactions.h \
    $$PWD/crypto/safetynumber.h \
    $$PWD/app/singleinstance.h \
    $$PWD/text/spellchecker.h \
    $$PWD/network/switchboard.h

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
