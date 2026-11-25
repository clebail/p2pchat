include(../tests.pri)
QT += widgets
TARGET = tst_mentioncompleter
SOURCES += tst_mentioncompleter.cpp $$PWD/../../mentioncompleter.cpp $$PWD/../../composer.cpp
HEADERS += $$PWD/../../mentioncompleter.h $$PWD/../../composer.h
