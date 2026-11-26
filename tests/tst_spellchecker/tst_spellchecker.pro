include(../tests.pri)
TARGET = tst_spellchecker
SOURCES += tst_spellchecker.cpp
DEFINES += DICT_DIR=\\\"$$PWD/dict\\\"
