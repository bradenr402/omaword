QT += core gui widgets qml quick quickcontrols2 sql concurrent dbus network testlib
CONFIG += c++17 testcase link_pkgconfig
PKGCONFIG += libarchive fontconfig
TEMPLATE = app
include(../branding.pri)
TARGET = tst_rangesui
INCLUDEPATH += ../src
SOURCES += tst_rangesui.cpp ../src/biblebackend.cpp ../src/usfmimporter.cpp ../src/biblebooks.cpp ../src/versioncatalog.cpp ../src/desktop.cpp ../src/blockdocument.cpp
HEADERS += ../src/biblebackend.h ../src/usfmimporter.h ../src/biblebooks.h ../src/versioncatalog.h ../src/desktop.h ../src/blockdocument.h
RESOURCES += ../resources.qrc
SOURCES += ../src/scripturesearch.cpp ../src/searchdocument.cpp
HEADERS += ../src/scripturesearch.h ../src/searchdocument.h
