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
SOURCES += ../src/blockdocument.cpp ../src/searchdocument.cpp ../src/keyhold.cpp ../src/exitsnapshot.cpp ../src/markdownquotes.cpp ../src/annotationpaint.cpp
HEADERS += ../src/blockdocument.h ../src/searchdocument.h ../src/keyhold.h ../src/exitsnapshot.h ../src/markdownquotes.h ../src/annotationpaint.h
RESOURCES += ../resources.qrc
# qmake reads resources.qrc to set up the compiled QML and the filtered
# resource list: an edited resources.qrc must regenerate this Makefile.
QMAKE_INTERNAL_INCLUDED_FILES += $$PWD/../resources.qrc
