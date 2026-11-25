include(../tests.pri)
QT += widgets
TARGET = tst_mentioncompleter
SOURCES += tst_mentioncompleter.cpp $$PWD/../../mentioncompleter.cpp
HEADERS += $$PWD/../../mentioncompleter.h
