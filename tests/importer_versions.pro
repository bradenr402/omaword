QT += core sql xml testlib
QT -= gui
CONFIG += console testcase c++17 link_pkgconfig
CONFIG -= app_bundle
TEMPLATE = app
include(../branding.pri)
TARGET = tst_importer_versions
PKGCONFIG += libarchive
INCLUDEPATH += ../src
SOURCES += tst_importer_versions.cpp ../src/epubimporter.cpp
HEADERS += ../src/epubimporter.h
