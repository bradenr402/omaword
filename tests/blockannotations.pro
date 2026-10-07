QT += core gui qml quick quickcontrols2 testlib
CONFIG += c++17 testcase
TEMPLATE = app
include(../branding.pri)
TARGET = tst_blockannotations
include(../qml/qml.pri)
SOURCES += tst_blockannotations.cpp
HEADERS += testenvironment.h
