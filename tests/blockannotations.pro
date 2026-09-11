QT += core gui qml quick quickcontrols2 testlib
CONFIG += c++17 testcase
TEMPLATE = app
TARGET = tst_blockannotations
INCLUDEPATH += ../src
SOURCES += tst_blockannotations.cpp ../src/blockdocument.cpp
HEADERS += ../src/blockdocument.h
