QT += core gui widgets qml quick quickcontrols2 sql concurrent dbus network testlib
CONFIG += c++17 testcase link_pkgconfig
TEMPLATE = app
include(../branding.pri)
TARGET = tst_windows
include(../src/core.pri)
include(../qml/qml.pri)
SOURCES += tst_windows.cpp
HEADERS += testenvironment.h
