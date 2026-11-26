include(../tests.pri)
TARGET = tst_terminalsharing
DEFINES += P2P_TERMINAL
INCLUDEPATH += $$PWD/../../src/terminal
SOURCES += tst_terminalsharing.cpp $$PWD/../../src/terminal/terminalsession.cpp $$PWD/../../src/terminal/terminalsharing.cpp
HEADERS += $$PWD/../../src/terminal/terminalsession.h $$PWD/../../src/terminal/terminalsharing.h
linux: LIBS += -lutil
