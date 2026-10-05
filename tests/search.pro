QT += core gui sql concurrent testlib
QT -= widgets
CONFIG += console testcase c++17 link_pkgconfig
CONFIG -= app_bundle
TEMPLATE = app
include(../branding.pri)
TARGET = tst_search
PKGCONFIG += libarchive
INCLUDEPATH += ../src
SOURCES += tst_search.cpp ../src/scripturesearch.cpp ../src/biblebackend.cpp ../src/usfmimporter.cpp ../src/biblebooks.cpp
HEADERS += ../src/scripturesearch.h ../src/biblebackend.h ../src/usfmimporter.h ../src/biblebooks.h
SOURCES += ../src/textselection.cpp
HEADERS += ../src/textselection.h
