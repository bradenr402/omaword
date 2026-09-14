QT += core gui widgets qml quick quickcontrols2 sql concurrent dbus network testlib
CONFIG += c++17 testcase link_pkgconfig
PKGCONFIG += libarchive fontconfig
TEMPLATE = app
include(../branding.pri)
TARGET = tst_tagsui
INCLUDEPATH += ../src
DEFINES += TAGS_QML_DIR=\"\\\"$$PWD/../qml\\\"\"
SOURCES += tst_tagsui.cpp ../src/biblebackend.cpp ../src/usfmimporter.cpp ../src/biblebooks.cpp
HEADERS += ../src/biblebackend.h ../src/usfmimporter.h ../src/biblebooks.h
SOURCES += ../src/desktop.cpp ../src/versioncatalog.cpp ../src/blockdocument.cpp
HEADERS += ../src/desktop.h ../src/versioncatalog.h ../src/blockdocument.h
RESOURCES += ../resources.qrc
