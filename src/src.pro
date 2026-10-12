# Everything but main() and the QML's own C++ types (qml.pro), compiled once
# for the app and every test suite.
QT += core gui widgets qml quick quickcontrols2 sql concurrent dbus network multimedia
CONFIG += c++17 link_pkgconfig staticlib
PKGCONFIG += libarchive fontconfig libpipewire-0.3
include(../branding.pri)
TEMPLATE = lib
TARGET = omawordcore
SOURCES += singleinstance.cpp windowmanager.cpp desktop.cpp biblebackend.cpp usfmimporter.cpp biblebooks.cpp versioncatalog.cpp
HEADERS += singleinstance.h windowmanager.h desktop.h biblebackend.h usfmimporter.h biblebooks.h versioncatalog.h
SOURCES += soundeffects.cpp scripturesearch.cpp textselection.cpp commandline.cpp executablewatch.cpp
HEADERS += soundeffects.h scripturesearch.h textselection.h commandline.h executablewatch.h
# Data the core reads itself (BibleBackend and `omaword completion` register it: Q_INIT_RESOURCE(core)).
RESOURCES += core.qrc
