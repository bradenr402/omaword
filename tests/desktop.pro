QT += core gui qml quick quickcontrols2 dbus testlib
CONFIG += c++17 testcase link_pkgconfig
TEMPLATE = app
include(../branding.pri)
TARGET = tst_desktop
include(../src/core.pri)
SOURCES += tst_desktop.cpp
HEADERS += testenvironment.h
