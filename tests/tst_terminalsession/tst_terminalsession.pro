include(../tests.pri)
TARGET = tst_terminalsession
DEFINES += P2P_TERMINAL
R = $$PWD/../../src/terminal
INCLUDEPATH += $$R
# terminalclient : l'exécutable de test joue aussi la fenêtre de console (`--terminal`)
SOURCES += tst_terminalsession.cpp $$R/terminalsession.cpp $$R/terminalclient.cpp
HEADERS += $$R/terminalsession.h $$R/terminalclient.h
linux: LIBS += -lutil
