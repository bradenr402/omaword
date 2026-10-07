QT += core gui widgets qml quick quickcontrols2 sql concurrent dbus network multimedia testlib
CONFIG += c++17 testcase link_pkgconfig
TEMPLATE = app
include(../branding.pri)
TARGET = tst_a11yui
include(../src/core.pri)
include(../qml/qml.pri)
SOURCES += tst_a11yui.cpp
HEADERS += testenvironment.h
