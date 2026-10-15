QT += core sql testlib
QT -= gui
CONFIG += console testcase c++17 link_pkgconfig
CONFIG -= app_bundle
TEMPLATE = app
TARGET = tst_usfm
include(../src/core.pri)
SOURCES += tst_ao.cpp
HEADERS += testenvironment.h
