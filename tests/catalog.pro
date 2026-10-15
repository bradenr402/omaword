QT += core network testlib sql concurrent
QT -= gui
CONFIG += c++17 testcase console
TEMPLATE = app
TARGET = tst_catalog
include(../src/core.pri)
SOURCES += tst_catalog.cpp
HEADERS += testenvironment.h
