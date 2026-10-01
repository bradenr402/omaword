QT += core gui multimedia testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TEMPLATE = app
include(../branding.pri)
TARGET = tst_sound
INCLUDEPATH += ../src
SOURCES += tst_sound.cpp ../src/soundeffects.cpp
HEADERS += ../src/soundeffects.h
