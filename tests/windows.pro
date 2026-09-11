QT += core gui widgets qml quick quickcontrols2 sql concurrent dbus network testlib
CONFIG += c++17 testcase link_pkgconfig
PKGCONFIG += libarchive fontconfig
TEMPLATE = app
include(../branding.pri)
TARGET = tst_windows
INCLUDEPATH += ../src
SOURCES += tst_windows.cpp ../src/windowmanager.cpp ../src/singleinstance.cpp ../src/biblebackend.cpp ../src/epubimporter.cpp ../src/desktop.cpp ../src/blockdocument.cpp
HEADERS += ../src/windowmanager.h ../src/singleinstance.h ../src/biblebackend.h ../src/epubimporter.h ../src/desktop.h ../src/blockdocument.h
RESOURCES += ../resources.qrc
