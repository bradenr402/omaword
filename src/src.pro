# Everything but main() and the QML's own C++ types (qml.pro), compiled once
# for the app and every test suite.
QT += core gui widgets qml quick quickcontrols2 sql concurrent dbus network multimedia
CONFIG += c++17 link_pkgconfig staticlib
PKGCONFIG += libarchive fontconfig
include(../branding.pri)
TEMPLATE = lib
TARGET = omawordcore
SOURCES += singleinstance.cpp windowmanager.cpp desktop.cpp biblebackend.cpp usfmimporter.cpp biblebooks.cpp versioncatalog.cpp
HEADERS += singleinstance.h windowmanager.h desktop.h biblebackend.h usfmimporter.h biblebooks.h versioncatalog.h
SOURCES += soundeffects.cpp scripturesearch.cpp textselection.cpp
HEADERS += soundeffects.h scripturesearch.h textselection.h
