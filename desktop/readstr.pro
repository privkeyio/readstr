QT += core gui qml quick quickcontrols2

CONFIG += c++17 release
TARGET = readstr
TEMPLATE = app

include(src/sources.pri)

SOURCES += src/main.cpp
