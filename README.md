# Omabible

A keyboard-first, offline NET Bible 2.1 reader for Linux, built with C++17 and
Qt 6 Quick. Read by chapter, keep bookmarks and highlights, and write Markdown
notes alongside a selected passage.

## Features

- Passage palette with live resolved-reference previews and book/chapter browsing.
- Selectable text, verse-range selection, chapter navigation, and saved reading position.
- Cross-chapter bookmarks, a single highlight color, and autosaved Markdown notes with preview.
- Verse-based Vim navigation, visual range selection, and searchable keybindings.
- Saved-passages drawer filtered by bookmarks, highlights, or notes.
- Markdown annotation export and JSON personal-data backup/restore.
- System fonts and desktop text scaling, live Omarchy theme integration, and a
  standard Qt palette fallback on other desktops.

Omabible supports NET 2.1 only. Full-text search, parallel translations, reading
plans, and cloud sync are outside the current scope. The reader preserves headings,
poetry, and within-verse breaks; publisher translator notes are not displayed.

## Build and install

Requirements: a C++17 compiler, GNU Make, `qmake6`, `pkg-config`, libarchive,
fontconfig, and Qt 6 Core, GUI, QML, Quick, Quick Controls 2, SQL (including the
SQLite driver), Concurrent, DBus, and Widgets. Backend tests also require Qt Test.
Qt's Quick Dialogs and Layouts QML modules must be available at runtime.

On Arch Linux / Omarchy:

```sh
sudo pacman -S --needed base-devel qt6-base qt6-declarative qt6-wayland libarchive fontconfig
```

`qt6-wayland` provides native Wayland support. `qt6-base` and `qt6-declarative`
provide the Qt libraries, QML modules, SQLite plugin, and qmake used here.

From the repository root:

```sh
bin/build
./build/omabible
```

`bin/build` runs `qmake6` in `build/` and then parallel Make. Set `JOBS=4 bin/build`
to override the detected CPU count. Scripts locate the checkout from their own
path, so they can also be invoked from another working directory.

For a user-local installation, after building:

```sh
bin/install
```

This requires no sudo. It installs the compiled binary to `~/.local/bin/omabible`,
the desktop launcher to `~/.local/share/applications/omabible.desktop`, and SVG
icons under `~/.local/share/icons/hicolor/`. Desktop files and icons honor
`XDG_DATA_HOME` when set. Ensure `~/.local/bin` is on your desktop session's
`PATH`; then launch **Omabible** from your application launcher or run `omabible`.
The desktop ID and icon name are both `omabible`.

### Arch package from the local checkout

```sh
cd packaging
makepkg -s
sudo pacman -U ./omabible-0.1.0-1-*.pkg.tar.zst
```

Run `makepkg` as your regular user. The recipe builds the enclosing source
checkout, including uncommitted changes, into makepkg's `src/build/`; no downloaded
source archive or manually generated tarball is required. Keep `packaging/` inside
the checkout: the recipe is for local builds, not a standalone source package.
Make honors `MAKEFLAGS` from your makepkg configuration. The package installs to
`/usr/bin`, `/usr/share/applications`, and `/usr/share/icons/hicolor`, with the
license and README under `/usr/share/licenses/omabible` and `/usr/share/doc/omabible`.

## Bring your NET 2.1 EPUB

1. Visit the official [Bible.org NET Bible download page](https://bible.org/downloads)
   and download **Full Notes Version 2.1 ePub** (`NETBIBLE21.epub`) yourself.
2. Start Omabible and choose **Choose NET EPUB…**, then select that local file.
   You can also use the command palette's **Import NET 2.1 EPUB** action.
3. Wait for import and validation to finish. Subsequent launches open the local
   library without importing again or needing a network connection.

The command-line import option is also available:

```sh
omabible --import "$HOME/Downloads/NETBIBLE21.epub"
```

Only your explicitly selected file is imported. Import validates the complete
NET 2.1 edition before replacing the local Bible database. The EPUB, Bible text,
and generated Bible database are not bundled with the application or package.

## Reading and references

Press **Ctrl+K**, enter a reference, and press Enter; or choose a book and chapter
from the palette. Examples accepted by the backend:

| Reference | Opens |
| --- | --- |
| `John 3` | A whole chapter |
| `Jn 3:16–18` | A same-chapter verse range |
| `John 3:35–4:2` | A range across chapters |
| `Mal 4:6–Matt 1:2` | A range across books |
| `1Jn3:16` | A compact numbered-book reference |
| `1 Cor 13`, `First Corinthians 13`, `I Corinthians 13` | Numbered-book aliases |
| `Psalm 23`, `Ps 23` | Psalms aliases |
| `Song of Songs 2` | Song of Solomon |
| `Jude 5–7` | Verses in a single-chapter book |
| `3:16` | A reference within the current book |

Names are case-insensitive; common abbreviations, spaces, and periods are
accepted. Ranges can cross chapters and books. Both endpoints must exist in NET 2.1;
a range may span verse numbers intentionally omitted by this edition.

Click a **verse number** to select a verse; Shift-click another number to select
a range. Drag over the verse text for ordinary text selection. Annotation commands
use the numbered verse selection. Copy passage copies that range, or the entire
chapter when no verse range is selected.

Normal mode moves the verse cursor with `j` / `k`. Press `v` to anchor a visual
selection, then continue with `j` / `k` to extend or shrink it. `J` / `K` enters
visual mode and extends immediately. At chapter boundaries, `j` continues to the
next chapter and `k` to the previous one, skipping omitted NET verse numbers.
Press `x` to highlight the entire range, including its off-screen chapters.
Press `v` again to collapse the range to its cursor. Saved ranges reopen at their
active ending chapter; the full reference remains visible in the footer and notes.

The palette previews the canonical destination before you press Enter. When
browsing books, Right Arrow also opens the selected book's chapter list.

### Notes, bookmarks, and highlights

Notes save as you type and belong to an **exact verse range**. A note for
`John 3:16–18` is distinct from a note for `John 3:16`; selecting an overlapping
range does not load or overwrite the other note. Verse note indicators include
overlapping notes. Reopen the exact range from **Saved → Notes** to edit it.

Bookmarks and highlights cover verses. A selection is marked active when all its
actual verses are covered. Toggling a partially covered selection covers all of
it; toggling a fully covered selection removes that flag within the selection,
splitting existing ranges when necessary and preserving other flags and notes.
One bookmark ribbon marks the start of a saved range; its margin line continues
through the range, including across chapter boundaries. Hover for the full
reference, or click the ribbon/line to reopen that range. Page icons mark notes.

### Keyboard shortcuts

Reader keys apply while reading, with no editable text field or modal dialog
active. **?** or **F1** opens the searchable **Keybindings** panel.

| Action | Shortcut |
| --- | --- |
| Passage palette / command palette | Ctrl+K / Ctrl+Shift+P |
| Saved passages | Ctrl+L |
| Previous / next chapter | `h` / `l`, `[` / `]`, Alt+Left / Alt+Right |
| Previous / next verse | `k` / `j`, Up / Down |
| Page down | Space, PageDown, Ctrl+F |
| Page up | Shift+Space, PageUp, Ctrl+B |
| Select chapter beginning / end | `g` / `G`, Home / End |
| Toggle visual selection / collapse to cursor | `v` |
| Extend or shrink visual selection | `j` / `k`, Down / Up |
| Enter visual mode and extend | `J` / `K`, Shift+Down / Shift+Up |
| Bookmark selected verses | `m`, Ctrl+D |
| Highlight selected verses | `x`, Ctrl+H |
| Write a note | `n`, Ctrl+N |
| Toggle notes panel | Ctrl+Shift+N |
| Copy passage (or chapter) | `y`, Ctrl+Shift+C |
| Larger / smaller reader text | Ctrl++ (also Ctrl+=) / Ctrl+- |
| Reset reader text size | Ctrl+0 |
| Leave editor / exit visual / close notes / collapse range | Escape |
| Search keybindings | `?`, F1 |

Use Up/Down and Enter in the palette, or prefix its query with `>` to find
commands such as import, export, backup, restore, and About.

## Data, export, and backup

On Linux, application data lives in **`~/.local/share/omabible/`**, or
`$XDG_DATA_HOME/omabible/` when set. This is Qt's `QStandardPaths::AppDataLocation`
with an empty organization name and application name `omabible`.

- `bible.sqlite`: the imported, validated NET edition.
- `personal.sqlite`: bookmarks, highlights, exact-range notes, and reading state.

Existing version-1 personal databases upgrade transactionally to support
cross-chapter ranges. New backups use version 2; restoring version-1 backups is
also supported. Older Omabible binaries cannot read the upgraded database.

Use **About & attribution → Open local data folder** to find the actual location.

From the command palette or Saved drawer:

- **Export notes as Markdown** writes `omabible-notes.md` in the chosen existing
  folder, replacing that export file. It contains references, flags, and your
  Markdown notes.
- **Save annotation backup** writes a JSON backup of personal annotations and
  reading state. It does not include the Bible text or your EPUB.
- **Restore annotation backup** validates and then **replaces all current personal
  data**, including reading state, in one transaction; it does not merge. Save a
  backup of the current data first if you want to retain it. Import the NET edition
  on the destination installation before restoring so verse ranges can be checked.

Keep your original user-provided EPUB separately for setup on another machine.

## Tests

```sh
bin/test
OMABIBLE_TEST_EPUB="$HOME/Downloads/NETBIBLE21.epub" bin/test
```

The script builds backend and integrated UI tests in `build/tests/` and
`build/ui-tests/`, then runs both offscreen. The EPUB environment variable is
optional; real-edition backend integration tests are skipped without it, and UI
tests use original synthetic text. Tests isolate application data in temporary directories.
`JOBS` controls build parallelism, and arguments are passed to the test executable
(for example, `bin/test -functions`). See [tests/README.md](tests/README.md) for the
backend behavior covered. UI tests exercise keyboard navigation, verse selection,
notes, restore confirmation, scroll restoration, and narrow/scaled layouts.

## License and Bible attribution

Application code, documentation, and icons are licensed under the [MIT License](LICENSE),
copyright Omabible contributors.

NET Bible® copyright © 1996–2019 by Biblical Studies Press, L.L.C. All rights
reserved. The Bible is separately copyrighted and is not covered by Omabible's
MIT license. Obtain your own EPUB from the publisher and follow the
[NET Bible permissions](https://netbible.com/copyright/). Omabible distributes no
Bible text or EPUB.
