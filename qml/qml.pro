# resources.qrc with its QML and JavaScript compiled ahead of time
# (qmlcachegen), so nothing is compiled at launch and the tests run the same
# compiled code the app ships; plus the C++ types the QML imports
# (OmaWord.Reader), which register themselves at startup.
QT += qml quick
CONFIG += c++17 staticlib qtquickcompiler
include(../branding.pri)
TEMPLATE = lib
TARGET = omawordqml
INCLUDEPATH += ../src
SOURCES += ../src/blockdocument.cpp ../src/searchdocument.cpp ../src/keyhold.cpp ../src/exitsnapshot.cpp
HEADERS += ../src/blockdocument.h ../src/searchdocument.h ../src/keyhold.h ../src/exitsnapshot.h
RESOURCES += ../resources.qrc
