# Shared by readstr.pro and tests/readstr_tests.pro.
QT += network sql dbus qml websockets

# BIP-340 signatures on Nostr events.
LIBS += -lsecp256k1

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
    $$PWD/nip19.h \
    $$PWD/nostrevent.h \
    $$PWD/nostrsource.h \
    $$PWD/localnetwork.h \
    $$PWD/markdown.h \
    $$PWD/opml.h \
    $$PWD/paths.h \
    $$PWD/portalfilepicker.h \
    $$PWD/relaypool.h \
    $$PWD/sanitizer.h \
    $$PWD/smallfile.h \
    $$PWD/sourcemodel.h \
    $$PWD/store.h \
    $$PWD/sync.h \
    $$PWD/theme.h \
    $$PWD/thumbnails.h \
    $$PWD/translator.h

SOURCES += \
    $$PWD/articlemodel.cpp \
    $$PWD/backend.cpp \
    $$PWD/cli.cpp \
    $$PWD/discovery.cpp \
    $$PWD/feedparser.cpp \
    $$PWD/fetcher.cpp \
    $$PWD/library.cpp \
    $$PWD/nip19.cpp \
    $$PWD/nostrevent.cpp \
    $$PWD/nostrsource.cpp \
    $$PWD/localnetwork.cpp \
    $$PWD/markdown.cpp \
    $$PWD/opml.cpp \
    $$PWD/paths.cpp \
    $$PWD/portalfilepicker.cpp \
    $$PWD/relaypool.cpp \
    $$PWD/sanitizer.cpp \
    $$PWD/sourcemodel.cpp \
    $$PWD/store.cpp \
    $$PWD/sync.cpp \
    $$PWD/theme.cpp \
    $$PWD/thumbnails.cpp \
    $$PWD/translator.cpp

RESOURCES += $$PWD/resources.qrc
