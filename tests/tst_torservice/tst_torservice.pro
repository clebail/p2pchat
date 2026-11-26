include(../tests.pri)
TARGET = tst_torservice
INCLUDEPATH += $$PWD/../../src/network
SOURCES += tst_torservice.cpp $$PWD/../../src/network/torservice.cpp
HEADERS += $$PWD/../../src/network/torservice.h
