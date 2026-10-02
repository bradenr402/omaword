QT += core gui widgets qml quick quickcontrols2 sql concurrent dbus network multimedia testlib
CONFIG += c++17 testcase link_pkgconfig
PKGCONFIG += libarchive fontconfig
TEMPLATE = app
include(../branding.pri)
TARGET = tst_ui
INCLUDEPATH += ../src
SOURCES += tst_ui.cpp ../src/biblebackend.cpp ../src/usfmimporter.cpp ../src/biblebooks.cpp ../src/versioncatalog.cpp ../src/desktop.cpp
HEADERS += ../src/biblebackend.h ../src/usfmimporter.h ../src/biblebooks.h ../src/versioncatalog.h ../src/desktop.h
RESOURCES += ../resources.qrc
SOURCES += ../src/blockdocument.cpp
HEADERS += ../src/blockdocument.h
SOURCES += ../src/scripturesearch.cpp ../src/searchdocument.cpp ../src/keyhold.cpp
HEADERS += ../src/scripturesearch.h ../src/searchdocument.h ../src/keyhold.h
SOURCES += ../src/soundeffects.cpp
HEADERS += ../src/soundeffects.h
