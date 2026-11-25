include(../tests.pri)
TARGET = tst_terminalsharing
DEFINES += P2P_TERMINAL
SOURCES += tst_terminalsharing.cpp $$PWD/../../terminalsession.cpp $$PWD/../../terminalsharing.cpp
HEADERS += $$PWD/../../terminalsession.h $$PWD/../../terminalsharing.h
linux: LIBS += -lutil
