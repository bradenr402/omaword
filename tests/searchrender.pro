QT += core gui qml quick quickcontrols2 testlib
CONFIG += c++17 testcase
TEMPLATE = app
include(../branding.pri)
TARGET = tst_searchrender
INCLUDEPATH += ../src
SOURCES += tst_searchrender.cpp ../src/blockdocument.cpp ../src/searchdocument.cpp
HEADERS += ../src/blockdocument.h ../src/searchdocument.h
