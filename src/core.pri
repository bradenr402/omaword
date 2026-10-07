# Links the core library (src.pro), built in the sibling build directory src.
# A static library: each consumer takes only the objects it references.
INCLUDEPATH += $$PWD
core_library = $$OUT_PWD/../src/libomawordcore.a
LIBS = $$core_library $$LIBS
PRE_TARGETDEPS += $$core_library
CONFIG += link_pkgconfig
PKGCONFIG += libarchive fontconfig libpipewire-0.3
