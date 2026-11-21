QT += core gui quick quickcontrols2 testlib
CONFIG += c++17 testcase
TARGET = readstr_tests
TEMPLATE = app

include(../src/sources.pri)

HEADERS += fakehttpserver.h fakerelay.h
SOURCES += readstr_tests.cpp
