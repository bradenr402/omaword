QT += core gui widgets qml quick quickcontrols2 sql concurrent dbus network multimedia
CONFIG += c++17
include(../branding.pri)
include(../src/core.pri)
include(../qml/qml.pri)
TEMPLATE = app
TARGET = $$BRAND_ID
# The binary and its desktop file land in the build root, where bin/install looks.
DESTDIR = $$OUT_PWD/..
SOURCES += ../src/main.cpp

brand_desktop.input = $$PWD/../assets/launcher.desktop.in
brand_desktop.output = $$OUT_PWD/../$${BRAND_ID}.desktop
QMAKE_SUBSTITUTES += brand_desktop
