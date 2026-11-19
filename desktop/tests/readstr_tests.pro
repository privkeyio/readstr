QT += core gui quick quickcontrols2 testlib
CONFIG += c++17 testcase
TARGET = readstr_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += ../src/backend.h ../src/cli.h ../src/paths.h ../src/theme.h ../src/translator.h
SOURCES += readstr_tests.cpp ../src/backend.cpp ../src/cli.cpp ../src/paths.cpp ../src/theme.cpp ../src/translator.cpp
RESOURCES += ../src/resources.qrc
