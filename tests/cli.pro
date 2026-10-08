QT += core gui sql concurrent network testlib
QT -= widgets
CONFIG += console testcase c++17 link_pkgconfig
CONFIG -= app_bundle
TEMPLATE = app
include(../branding.pri)
TARGET = tst_cli
include(../src/core.pri)
SOURCES += tst_cli.cpp
HEADERS += testenvironment.h
