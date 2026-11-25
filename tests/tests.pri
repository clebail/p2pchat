# Réglages communs à chaque test (un exécutable par fichier tst_*.cpp)
QT += testlib
QT -= gui
CONFIG += testcase console no_testcase_installs
CONFIG -= app_bundle
include($$PWD/../core.pri)
