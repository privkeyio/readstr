# Shared by readstr.pro and tests/readstr_tests.pro.
QT += network sql dbus qml

VERSION = 0.1.0
DEFINES += READSTR_VERSION=\\\"$$VERSION\\\"

INCLUDEPATH += $$PWD

HEADERS += \
    $$PWD/articlemodel.h \
    $$PWD/backend.h \
    $$PWD/cli.h \
    $$PWD/discovery.h \
    $$PWD/feedparser.h \
    $$PWD/fetcher.h \
    $$PWD/filepicker.h \
    $$PWD/library.h \
    $$PWD/localnetwork.h \
    $$PWD/opml.h \
    $$PWD/paths.h \
    $$PWD/portalfilepicker.h \
    $$PWD/sanitizer.h \
    $$PWD/smallfile.h \
    $$PWD/sourcemodel.h \
    $$PWD/store.h \
    $$PWD/theme.h \
    $$PWD/translator.h

SOURCES += \
    $$PWD/articlemodel.cpp \
    $$PWD/backend.cpp \
    $$PWD/cli.cpp \
    $$PWD/discovery.cpp \
    $$PWD/feedparser.cpp \
    $$PWD/fetcher.cpp \
    $$PWD/library.cpp \
    $$PWD/localnetwork.cpp \
    $$PWD/opml.cpp \
    $$PWD/paths.cpp \
    $$PWD/portalfilepicker.cpp \
    $$PWD/sanitizer.cpp \
    $$PWD/sourcemodel.cpp \
    $$PWD/store.cpp \
    $$PWD/theme.cpp \
    $$PWD/translator.cpp

RESOURCES += $$PWD/resources.qrc
