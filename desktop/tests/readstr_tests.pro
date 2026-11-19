QT += core gui quick quickcontrols2 testlib
CONFIG += c++17 testcase
TARGET = readstr_tests
TEMPLATE = app

include(../src/sources.pri)

SOURCES += readstr_tests.cpp
