QT += core sql network testlib
QT -= gui
CONFIG += c++17 console testcase
TEMPLATE = app
TARGET = tst_migration
INCLUDEPATH += ../src
SOURCES += tst_migration.cpp ../src/storagemigration.cpp
HEADERS += ../src/storagemigration.h
