QT += core gui widgets qml quick quickcontrols2 sql concurrent dbus network multimedia testlib
CONFIG += c++17 testcase link_pkgconfig
PKGCONFIG += libarchive fontconfig
TEMPLATE = app
include(../branding.pri)
TARGET = tst_motion
INCLUDEPATH += ../src
SOURCES += tst_motion.cpp ../src/biblebackend.cpp ../src/scripturesearch.cpp ../src/usfmimporter.cpp ../src/biblebooks.cpp ../src/versioncatalog.cpp ../src/desktop.cpp ../src/blockdocument.cpp ../src/searchdocument.cpp ../src/keyhold.cpp ../src/exitsnapshot.cpp
HEADERS += ../src/biblebackend.h ../src/scripturesearch.h ../src/usfmimporter.h ../src/biblebooks.h ../src/versioncatalog.h ../src/desktop.h ../src/blockdocument.h ../src/searchdocument.h ../src/keyhold.h ../src/exitsnapshot.h
RESOURCES += ../resources.qrc
SOURCES += ../src/soundeffects.cpp
HEADERS += ../src/soundeffects.h
SOURCES += ../src/textselection.cpp
HEADERS += ../src/textselection.h
