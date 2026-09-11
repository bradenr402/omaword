QT += core gui network testlib
CONFIG += c++17 testcase
TEMPLATE = app
TARGET = tst_singleinstance
INCLUDEPATH += ../src
SOURCES += tst_singleinstance.cpp ../src/singleinstance.cpp
HEADERS += ../src/singleinstance.h
