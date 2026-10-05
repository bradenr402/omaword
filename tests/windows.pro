QT += core gui widgets qml quick quickcontrols2 sql concurrent dbus network multimedia testlib
CONFIG += c++17 testcase link_pkgconfig
PKGCONFIG += libarchive fontconfig
TEMPLATE = app
include(../branding.pri)
TARGET = tst_windows
INCLUDEPATH += ../src
SOURCES += tst_windows.cpp ../src/windowmanager.cpp ../src/singleinstance.cpp ../src/biblebackend.cpp ../src/usfmimporter.cpp ../src/biblebooks.cpp ../src/versioncatalog.cpp ../src/desktop.cpp ../src/blockdocument.cpp
HEADERS += ../src/windowmanager.h ../src/singleinstance.h ../src/biblebackend.h ../src/usfmimporter.h ../src/biblebooks.h ../src/versioncatalog.h ../src/desktop.h ../src/blockdocument.h
RESOURCES += ../resources.qrc
SOURCES += ../src/scripturesearch.cpp ../src/searchdocument.cpp ../src/keyhold.cpp
HEADERS += ../src/scripturesearch.h ../src/searchdocument.h ../src/keyhold.h
SOURCES += ../src/soundeffects.cpp
HEADERS += ../src/soundeffects.h
SOURCES += ../src/textselection.cpp
HEADERS += ../src/textselection.h
