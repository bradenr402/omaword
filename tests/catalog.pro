QT += core network testlib
QT -= gui
CONFIG += c++17 testcase console
TEMPLATE = app
TARGET = tst_catalog
include(../src/core.pri)
SOURCES += tst_catalog.cpp
catalog_resource.files = ../resources/catalog.json
catalog_resource.prefix = /resources
catalog_resource.base = ../resources
RESOURCES += catalog_resource
HEADERS += testenvironment.h
