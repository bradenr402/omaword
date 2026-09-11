QT += core gui widgets qml quick quickcontrols2 sql concurrent dbus network
CONFIG += c++17 link_pkgconfig
PKGCONFIG += libarchive fontconfig
include(branding.pri)
TARGET = $$BRAND_ID
TEMPLATE = app
SOURCES += src/main.cpp src/singleinstance.cpp src/desktop.cpp src/biblebackend.cpp src/epubimporter.cpp
HEADERS += src/singleinstance.h src/desktop.h src/biblebackend.h src/epubimporter.h
RESOURCES += resources.qrc
SOURCES += src/blockdocument.cpp
HEADERS += src/blockdocument.h

brand_desktop.input = $$PWD/assets/launcher.desktop.in
brand_desktop.output = $$OUT_PWD/$${BRAND_ID}.desktop
QMAKE_SUBSTITUTES += brand_desktop
# Keep scripts that invoke the old build executable working.
QMAKE_POST_LINK = ln -sfn $$shell_quote($$TARGET) $$shell_quote($$OUT_PWD/$$BRAND_STORAGE_ID)
