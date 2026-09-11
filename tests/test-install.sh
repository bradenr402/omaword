#!/usr/bin/env bash
# Installer regression checks; all writes and package staging stay in a temp tree.
set -euo pipefail
shopt -s nullglob
root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
workspace=$(mktemp -d "${TMPDIR:-/tmp}/omaword-install-test.XXXXXX")
running=
trap 'if [[ -n $running ]]; then kill "$running" 2>/dev/null || true; wait "$running" 2>/dev/null || true; fi; rm -rf -- "$workspace"' EXIT
fixture="$workspace/checkout"
mkdir -p "$fixture"
cp -a "$root/bin" "$root/assets" "$root/src" "$root/qml" "$root/packaging" \
    "$root/branding.env" "$root/branding.pri" "$root/omaword.pro" \
    "$root/resources.qrc" "$root/LICENSE" "$root/README.md" "$fixture/"
source "$fixture/branding.env"
original_id=$BRAND_ID
export BUILD_DIR="$workspace/build"
mkdir -p "$BUILD_DIR"
(cd "$BUILD_DIR"; qmake6 "$fixture/omaword.pro")
cp /usr/bin/true "$BUILD_DIR/$BRAND_ID"
desktop-file-validate "$BUILD_DIR/$BRAND_ID.desktop"

new_home() {
    export HOME="$workspace/$1" XDG_DATA_HOME="$workspace/$1/data"
    mkdir -p "$HOME"
}
run_install() { bash "$fixture/bin/install"; }
assert_installed() {
    cmp "$BUILD_DIR/$BRAND_ID" "$HOME/.local/bin/$BRAND_ID"
    [[ $(stat -c '%a' "$HOME/.local/bin/$BRAND_ID") == 755 ]]
    [[ ! -L $HOME/.local/bin/$BRAND_ID ]]
    cmp "$BUILD_DIR/$BRAND_ID.desktop" "$XDG_DATA_HOME/applications/$BRAND_ID.desktop"
    cmp "$fixture/assets/app.svg" "$XDG_DATA_HOME/icons/hicolor/scalable/apps/$BRAND_ID.svg"
    cmp "$fixture/assets/app-symbolic.svg" "$XDG_DATA_HOME/icons/hicolor/symbolic/apps/$BRAND_ID-symbolic.svg"
}
single_backup() {
    local backups=("$XDG_DATA_HOME/$BRAND_ID/install-backups/"*)
    [[ ${#backups[@]} == 1 ]]
    backup=${backups[0]}
}

new_home fresh
run_install
assert_installed
run_install
[[ ! -e $XDG_DATA_HOME/$BRAND_ID/install-backups ]]
printf 'PASS fresh and unchanged reinstall\n'

new_home customized
mkdir -p "$HOME/.local/bin" "$XDG_DATA_HOME/applications" \
    "$XDG_DATA_HOME/icons/hicolor/scalable/apps" "$XDG_DATA_HOME/icons/hicolor/symbolic/apps"
printf '#!/bin/sh\nexit 42\n' > "$workspace/wrapper"
chmod 751 "$workspace/wrapper"
printf '[Desktop Entry]\nName=Custom launcher\n' > "$workspace/custom.desktop"
cp "$workspace/custom.desktop" "$XDG_DATA_HOME/applications/$BRAND_ID.desktop"
ln -s "$workspace/missing-icon" "$XDG_DATA_HOME/icons/hicolor/scalable/apps/$BRAND_ID.svg"
printf 'custom symbolic icon\n' > "$XDG_DATA_HOME/icons/hicolor/symbolic/apps/$BRAND_ID-symbolic.svg"
cp /usr/bin/sleep "$HOME/.local/bin/$BRAND_ID"
"$HOME/.local/bin/$BRAND_ID" 120 &
running=$!
run_install
assert_installed
kill -0 "$running"
kill "$running"
wait "$running" 2>/dev/null || true
running=
single_backup
cmp /usr/bin/sleep "$backup/bin/$BRAND_ID"
cmp "$workspace/custom.desktop" "$backup/applications/$BRAND_ID.desktop"
[[ $(readlink "$backup/icons/scalable/$BRAND_ID.svg") == "$workspace/missing-icon" ]]
[[ $(<"$backup/icons/symbolic/$BRAND_ID-symbolic.svg") == 'custom symbolic icon' ]]
run_install
single_backup
printf 'PASS custom collisions, symlinks, running binary, unchanged reinstall\n'

new_home custom-wrapper
mkdir -p "$HOME/.local/bin" "$XDG_DATA_HOME/$BRAND_ID/install-backups/prior"
cp -a "$workspace/wrapper" "$HOME/.local/bin/$BRAND_ID"
printf 'preserved archive\n' > "$XDG_DATA_HOME/$BRAND_ID/install-backups/prior/entry"
run_install
assert_installed
[[ $(<"$XDG_DATA_HOME/$BRAND_ID/install-backups/prior/entry") == 'preserved archive' ]]
mv "$XDG_DATA_HOME/$BRAND_ID/install-backups/prior" "$workspace/prior"
single_backup
cmp "$workspace/wrapper" "$backup/bin/$BRAND_ID"
[[ $(stat -c '%a' "$backup/bin/$BRAND_ID") == 751 ]]
printf 'PASS wrapper mode and prior archive preservation\n'

new_home symlink
mkdir -p "$HOME/.local/bin" "$XDG_DATA_HOME/applications"
ln -s "$workspace/wrapper" "$HOME/.local/bin/$BRAND_ID"
ln -s "$workspace/custom.desktop" "$XDG_DATA_HOME/applications/$BRAND_ID.desktop"
run_install
assert_installed
single_backup
[[ $(readlink "$backup/bin/$BRAND_ID") == "$workspace/wrapper" ]]
[[ $(readlink "$backup/applications/$BRAND_ID.desktop") == "$workspace/custom.desktop" ]]
[[ $(<"$workspace/wrapper") == $'#!/bin/sh\nexit 42' ]]
[[ $(<"$workspace/custom.desktop") == $'[Desktop Entry]\nName=Custom launcher' ]]
printf 'PASS command and desktop symlink backups preserve targets\n'

new_home missing-source
mv "$fixture/assets/app-symbolic.svg" "$fixture/assets/held.svg"
if run_install; then exit 1; fi
[[ ! -e $HOME/.local && ! -e $XDG_DATA_HOME ]]
mv "$fixture/assets/held.svg" "$fixture/assets/app-symbolic.svg"
new_home invalid-destination
mkdir -p "$XDG_DATA_HOME/icons/hicolor/symbolic/apps/$BRAND_ID-symbolic.svg"
if run_install; then exit 1; fi
[[ ! -e $HOME/.local && ! -e $XDG_DATA_HOME/applications ]]
new_home invalid-parent
mkdir -p "$XDG_DATA_HOME/icons/hicolor"
ln -s "$workspace/missing-directory" "$XDG_DATA_HOME/icons/hicolor/symbolic"
if run_install; then exit 1; fi
[[ ! -e $HOME/.local && ! -e $XDG_DATA_HOME/applications ]]
printf 'PASS source and destination preflight before partial installation\n'

# Change metadata only in the copied checkout. Stable asset source names must
# work for qmake, the local installer, and the package recipe with a new ID.
BRAND_ID=alternateword
printf 'BRAND_NAME="AlternateWord"\nBRAND_ID="%s"\nBRAND_VERSION="%s"\nBRAND_KEYWORDS="%s"\nBRAND_STORAGE_ID="%s"\n' \
    "$BRAND_ID" "$BRAND_VERSION" "$BRAND_KEYWORDS" "$BRAND_STORAGE_ID" > "$fixture/branding.env"
(cd "$BUILD_DIR"; qmake6 "$fixture/omaword.pro")
cp /usr/bin/true "$BUILD_DIR/$BRAND_ID"
desktop-file-validate "$BUILD_DIR/$BRAND_ID.desktop"
new_home alternate
run_install
assert_installed
[[ ! -e $HOME/.local/bin/$original_id ]]
(
    export startdir="$fixture/packaging" srcdir="$workspace/package-src" pkgdir="$workspace/package-stage"
    mkdir -p "$srcdir" "$pkgdir"
    ln -s "$BUILD_DIR" "$srcdir/build"
    source "$fixture/packaging/PKGBUILD"
    [[ $pkgname == alternateword ]]
    package
    cmp "$fixture/assets/app.svg" "$pkgdir/usr/share/icons/hicolor/scalable/apps/$BRAND_ID.svg"
    cmp "$fixture/assets/app-symbolic.svg" "$pkgdir/usr/share/icons/hicolor/symbolic/apps/$BRAND_ID-symbolic.svg"
    desktop-file-validate "$pkgdir/usr/share/applications/$BRAND_ID.desktop"
)
printf 'PASS alternate brand ID generation, installation, and package staging\n'
