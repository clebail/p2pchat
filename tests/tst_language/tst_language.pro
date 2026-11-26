include(../tests.pri)
TARGET = tst_language
SOURCES += tst_language.cpp
# La traduction de l'application, compilée et embarquée comme dans celle-ci (:/i18n/p2pchat_en.qm)
TRANSLATIONS += ../../translations/p2pchat_en.ts
CONFIG += lrelease embed_translations
DEFINES += TS_FILE=\\\"$$PWD/../../translations/p2pchat_en.ts\\\"
