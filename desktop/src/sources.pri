# Shared by readstr.pro and tests/readstr_tests.pro.
QT += network sql

VERSION = 0.1.0
DEFINES += READSTR_VERSION=\\\"$$VERSION\\\"

INCLUDEPATH += $$PWD

HEADERS += \
    $$PWD/backend.h \
    $$PWD/cli.h \
    $$PWD/discovery.h \
    $$PWD/feedparser.h \
    $$PWD/fetcher.h \
    $$PWD/opml.h \
    $$PWD/paths.h \
    $$PWD/smallfile.h \
    $$PWD/store.h \
    $$PWD/theme.h \
    $$PWD/translator.h

SOURCES += \
    $$PWD/backend.cpp \
    $$PWD/cli.cpp \
    $$PWD/discovery.cpp \
    $$PWD/feedparser.cpp \
    $$PWD/fetcher.cpp \
    $$PWD/opml.cpp \
    $$PWD/paths.cpp \
    $$PWD/store.cpp \
    $$PWD/theme.cpp \
    $$PWD/translator.cpp

RESOURCES += $$PWD/resources.qrc
