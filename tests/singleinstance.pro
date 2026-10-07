QT += core gui network testlib
CONFIG += c++17 testcase
TEMPLATE = app
TARGET = tst_singleinstance
include(../src/core.pri)
SOURCES += tst_singleinstance.cpp
HEADERS += testenvironment.h
