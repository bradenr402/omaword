QT += core gui widgets qml quick quickcontrols2 sql concurrent dbus network testlib
CONFIG += c++17 testcase link_pkgconfig
TEMPLATE = app
include(../branding.pri)
TARGET = tst_marginsui
include(../src/core.pri)
include(../qml/qml.pri)
SOURCES += tst_marginsui.cpp
HEADERS += testenvironment.h
