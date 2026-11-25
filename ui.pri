# Composants d'interface de la conversation, sans la fenêtre principale ni Tor :
# utilisés par l'application et par les tests graphiques (tests/)
QT += widgets
INCLUDEPATH += $$PWD
DEPENDPATH += $$PWD

SOURCES += \
    $$PWD/chatdelegate.cpp \
    $$PWD/chatmodel.cpp \
    $$PWD/chatstyle.cpp \
    $$PWD/chatview.cpp \
    $$PWD/composer.cpp \
    $$PWD/mentioncompleter.cpp

HEADERS += \
    $$PWD/chatdelegate.h \
    $$PWD/chatmodel.h \
    $$PWD/chatstyle.h \
    $$PWD/chatview.h \
    $$PWD/composer.h \
    $$PWD/hoverbar.h \
    $$PWD/mentioncompleter.h
