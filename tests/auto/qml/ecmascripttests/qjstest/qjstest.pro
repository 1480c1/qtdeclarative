TEMPLATE = app
TARGET = qjstest
QT += qml-private
INCLUDEPATH += .

CONFIG += c++14

DEFINES += QT_DEPRECATED_WARNINGS

HEADERS += test262runner.h
SOURCES += main.cpp test262runner.cpp

QMAKE_TARGET_DESCRIPTION = Javascript test runner

load(qt_tool)
