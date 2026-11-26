QT += widgets network
CONFIG += c++17
TARGET = p2pchat
macx: TARGET = P2PChat

# Toute l'application (cœur, conversation, fenêtre, Tor, notifications, console) : cf. src/, un dossier par
# fonctionnalité ; le cœur et la conversation sont partagés avec les tests
include(src/app.pri)
SOURCES += src/app/main.cpp

# Traductions (textes écrits en français) : compilées par lrelease et embarquées (:/i18n/p2pchat_en.qm).
# Après avoir ajouté ou modifié des textes : lupdate p2pchat.pro, puis traduire avec Qt Linguist.
TRANSLATIONS += translations/p2pchat_en.ts
CONFIG += lrelease embed_translations

macx {
    QMAKE_MACOSX_DEPLOYMENT_TARGET = 11.0
    QMAKE_TARGET_BUNDLE_PREFIX = org.p2pchat   # identifiant : org.p2pchat.P2PChat
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
