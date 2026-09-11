QT += core gui widgets qml quick quickcontrols2 sql concurrent dbus network
CONFIG += c++17 link_pkgconfig
PKGCONFIG += libarchive fontconfig
include(branding.pri)
TARGET = $$BRAND_ID
TEMPLATE = app
SOURCES += src/main.cpp src/singleinstance.cpp src/storagemigration.cpp src/desktop.cpp src/biblebackend.cpp src/epubimporter.cpp
HEADERS += src/singleinstance.h src/storagemigration.h src/desktop.h src/biblebackend.h src/epubimporter.h
RESOURCES += resources.qrc
SOURCES += src/blockdocument.cpp
HEADERS += src/blockdocument.h

brand_desktop.input = $$PWD/assets/launcher.desktop.in
brand_desktop.output = $$OUT_PWD/$${BRAND_ID}.desktop
QMAKE_SUBSTITUTES += brand_desktop
