QT += core gui widgets qml quick quickcontrols2 sql concurrent dbus network multimedia
CONFIG += c++17 link_pkgconfig
PKGCONFIG += libarchive fontconfig
include(branding.pri)
TARGET = $$BRAND_ID
TEMPLATE = app
SOURCES += src/main.cpp src/singleinstance.cpp src/windowmanager.cpp src/desktop.cpp src/biblebackend.cpp src/usfmimporter.cpp src/biblebooks.cpp src/versioncatalog.cpp
HEADERS += src/singleinstance.h src/windowmanager.h src/desktop.h src/biblebackend.h src/usfmimporter.h src/biblebooks.h src/versioncatalog.h
RESOURCES += resources.qrc
SOURCES += src/soundeffects.cpp
HEADERS += src/soundeffects.h
SOURCES += src/blockdocument.cpp
HEADERS += src/blockdocument.h
SOURCES += src/scripturesearch.cpp src/searchdocument.cpp
HEADERS += src/scripturesearch.h src/searchdocument.h

brand_desktop.input = $$PWD/assets/launcher.desktop.in
brand_desktop.output = $$OUT_PWD/$${BRAND_ID}.desktop
QMAKE_SUBSTITUTES += brand_desktop
