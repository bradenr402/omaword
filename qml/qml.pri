# Links the QML library (qml.pro) whole: its resources, compiled units, and
# OmaWord.Reader types register through static initializers nothing references.
qml_library = $$OUT_PWD/../qml/libomawordqml.a
LIBS += -Wl,--whole-archive $$qml_library -Wl,--no-whole-archive
PRE_TARGETDEPS += $$qml_library
INCLUDEPATH += $$PWD/../src
