QT += core gui widgets qml quick quickcontrols2 sql concurrent dbus network
CONFIG += c++17
include(../branding.pri)
include(../src/core.pri)
include(../qml/qml.pri)
TEMPLATE = app
TARGET = $$BRAND_ID
# The binary and its desktop file land in the build root, where bin/install looks.
DESTDIR = $$OUT_PWD/..
SOURCES += ../src/main.cpp

brand_desktop.input = $$PWD/../assets/launcher.desktop.in
brand_desktop.output = $$OUT_PWD/../$${BRAND_ID}.desktop
QMAKE_SUBSTITUTES += brand_desktop

# `make install` installs what bin/install does, and zsh's and fish's
# completions, under PREFIX, named and branded alike: packages run qmake6
# PREFIX=/usr, make, and make INSTALL_ROOT=<staging directory> install.
isEmpty(PREFIX): PREFIX = /usr/local
binary = $$shell_quote($$OUT_PWD/../$$BRAND_ID)
target.path = $$PREFIX/bin
launcher.path = $$PREFIX/share/applications
launcher.files = $$OUT_PWD/../$${BRAND_ID}.desktop
launcher.CONFIG += no_check_exist
icon.path = $$PREFIX/share/icons/hicolor/scalable/apps
icon.extra = $(QINSTALL) $$shell_quote($$PWD/../assets/app.svg) $(INSTALL_ROOT)$$icon.path/$${BRAND_ID}.svg
symbolic.path = $$PREFIX/share/icons/hicolor/symbolic/apps
symbolic.extra = $(QINSTALL) $$shell_quote($$PWD/../assets/app-symbolic.svg) $(INSTALL_ROOT)$$symbolic.path/$${BRAND_ID}-symbolic.svg
manual.path = $$PREFIX/share/man/man1
manual.extra = sed -e $$shell_quote(s/OMAWORD/$$upper($$BRAND_ID)/g;s/OmaWord/$$BRAND_NAME/g;s/omaword/$$BRAND_ID/g) \
    $$shell_quote($$PWD/../assets/omaword.1) > $(INSTALL_ROOT)$$manual.path/$${BRAND_ID}.1
# The completions are what `omaword completion <shell>` prints, as bin/install's.
bash.path = $$PREFIX/share/bash-completion/completions
bash.extra = $$binary completion bash > $(INSTALL_ROOT)$$bash.path/$$BRAND_ID
zsh.path = $$PREFIX/share/zsh/site-functions
zsh.extra = $$binary completion zsh > $(INSTALL_ROOT)$$zsh.path/_$$BRAND_ID
fish.path = $$PREFIX/share/fish/vendor_completions.d
fish.extra = $$binary completion fish > $(INSTALL_ROOT)$$fish.path/$${BRAND_ID}.fish
INSTALLS += target launcher icon symbolic manual bash zsh fish
