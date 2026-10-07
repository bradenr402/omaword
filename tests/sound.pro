QT += core gui testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TEMPLATE = app
include(../branding.pri)
TARGET = tst_sound
include(../src/core.pri)
SOURCES += tst_sound.cpp
HEADERS += testenvironment.h
