QT += core gui qml quick quickcontrols2 dbus testlib
CONFIG += c++17 testcase link_pkgconfig
PKGCONFIG += fontconfig
TEMPLATE = app
include(../branding.pri)
TARGET = tst_desktop
INCLUDEPATH += ../src
SOURCES += tst_desktop.cpp ../src/desktop.cpp
HEADERS += ../src/desktop.h
HEADERS += testenvironment.h
