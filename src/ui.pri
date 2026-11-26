# Composants d'interface de la conversation (vue, saisie, Conversation), sans la fenêtre principale ni Tor :
# utilisés par l'application et par les tests graphiques (tests/)
!isEmpty(P2P_UI_PRI): return() # déjà inclus (tests qui chargent aussi app.pri)
P2P_UI_PRI = 1
QT += widgets
INCLUDEPATH += $$PWD/chat
DEPENDPATH += $$PWD/chat

SOURCES += \
    $$PWD/chat/chatdelegate.cpp \
    $$PWD/chat/chatmodel.cpp \
    $$PWD/chat/chatstyle.cpp \
    $$PWD/chat/chatview.cpp \
    $$PWD/chat/composer.cpp \
    $$PWD/chat/conversation.cpp \
    $$PWD/chat/mentioncompleter.cpp

HEADERS += \
    $$PWD/chat/chatdelegate.h \
    $$PWD/chat/chatmodel.h \
    $$PWD/chat/chatstyle.h \
    $$PWD/chat/chatview.h \
    $$PWD/chat/composer.h \
    $$PWD/chat/conversation.h \
    $$PWD/chat/hoverbar.h \
    $$PWD/chat/mentioncompleter.h
