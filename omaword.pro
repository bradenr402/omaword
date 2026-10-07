# The app links two static libraries, the C++ core (src) and the QML compiled
# ahead of time (qml); the test suites link the same two (src/core.pri,
# qml/qml.pri), so every source and QML file compiles once.
TEMPLATE = subdirs
SUBDIRS = src qml app
app.depends = src qml
