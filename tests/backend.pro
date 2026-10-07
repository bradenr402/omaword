QT += core gui sql concurrent testlib
QT -= widgets
CONFIG += console testcase c++17 link_pkgconfig
CONFIG -= app_bundle
TEMPLATE = app
include(../branding.pri)
TARGET = tst_backend
include(../src/core.pri)
SOURCES += tst_backend.cpp
HEADERS += testenvironment.h
