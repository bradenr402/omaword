QT += core gui sql concurrent testlib
QT -= widgets
CONFIG += console testcase c++17 link_pkgconfig
CONFIG -= app_bundle
TEMPLATE = app
include(../branding.pri)
TARGET = tst_backend
PKGCONFIG += libarchive
INCLUDEPATH += ../src
SOURCES += tst_backend.cpp ../src/biblebackend.cpp ../src/epubimporter.cpp
HEADERS += ../src/biblebackend.h ../src/epubimporter.h
