# Shared by readstr.pro and tests/readstr_tests.pro.
VERSION = 0.1.0
DEFINES += READSTR_VERSION=\\\"$$VERSION\\\"

INCLUDEPATH += $$PWD

HEADERS += \
    $$PWD/backend.h \
    $$PWD/cli.h \
    $$PWD/paths.h \
    $$PWD/smallfile.h \
    $$PWD/theme.h \
    $$PWD/translator.h

SOURCES += \
    $$PWD/backend.cpp \
    $$PWD/cli.cpp \
    $$PWD/paths.cpp \
    $$PWD/theme.cpp \
    $$PWD/translator.cpp

RESOURCES += $$PWD/resources.qrc
