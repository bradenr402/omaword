QT += core gui widgets qml quick quickcontrols2 sql concurrent dbus network multimedia testlib
CONFIG += c++17 testcase link_pkgconfig
TEMPLATE = app
include(../branding.pri)
TARGET = tst_tagsui
include(../src/core.pri)
include(../qml/qml.pri)
DEFINES += TAGS_QML_DIR=\"\\\"$$PWD/../qml\\\"\"
SOURCES += tst_tagsui.cpp
HEADERS += testenvironment.h
