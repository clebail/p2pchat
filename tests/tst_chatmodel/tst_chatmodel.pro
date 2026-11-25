include(../tests.pri)
QT += widgets
TARGET = tst_chatmodel
SOURCES += tst_chatmodel.cpp $$PWD/../../chatview.cpp
HEADERS += $$PWD/../../chatview.h
