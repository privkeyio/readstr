QT += core gui qml quick quickcontrols2

CONFIG += c++17 release
TARGET = readstr
TEMPLATE = app

HEADERS += \
    src/backend.h \
    src/cli.h \
    src/paths.h \
    src/theme.h \
    src/translator.h

SOURCES += \
    src/main.cpp \
    src/backend.cpp \
    src/cli.cpp \
    src/paths.cpp \
    src/theme.cpp \
    src/translator.cpp

RESOURCES += src/resources.qrc
