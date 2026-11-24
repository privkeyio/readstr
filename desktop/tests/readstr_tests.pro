QT += core gui quick quickcontrols2 testlib
CONFIG += c++17 testcase
TARGET = readstr_tests
TEMPLATE = app

include(../src/sources.pri)

HEADERS += fakehttpserver.h fakerelay.h
SOURCES += readstr_tests.cpp
DISTFILES += nip44.vectors.json

# The hooks that only tests may use, such as Fetcher::setResolverForTests.
# The app build (readstr.pro) never defines this.
DEFINES += READSTR_TESTS
