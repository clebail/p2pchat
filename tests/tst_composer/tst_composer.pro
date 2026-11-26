include(../tests.pri)
include(../../ui.pri)
TARGET = tst_composer
SOURCES += tst_composer.cpp

# Dictionnaires de test du correcteur orthographique
DEFINES += DICT_DIR=\\\"$$PWD/../tst_spellchecker/dict\\\"
