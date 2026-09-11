# OmaWord

Public branding is defined in `branding.env`. The launcher also matches **bible**
and **scripture**. The local installer preserves changed installation files in
`~/.local/share/omaword/install-backups/` before replacing them; existing Bible
and personal data continue to live in `~/.local/share/omabible/`.

A keyboard-first, offline NET Bible 2.1 reader for Linux, built with C++17 and
Qt 6 Quick. Read by chapter, keep bookmarks and highlights, and write Markdown
notes in an overlay for a selected passage.

## Features

- Passage palette with live reference previews, book/chapter/optional verse browsing,
  and Space to resume the last palette.
- Free text selection across verses, verse-range selection, chapter navigation,
  saved reading position, and mouse copying of passages or formatted quotations.
- Cross-chapter bookmarks, a single highlight color, and autosaved Markdown notes
  with preview in an animated overlay at every window width.
- Verse-based Vim navigation, visual range selection, and searchable, executable keybindings.
- Persisted reading preferences for verse numbers, headings, verse picking, flowing
  block mode, line height, and distraction-free focus mode.
- Saved panel filtered by bookmarks, highlights, or notes.
- Markdown annotation export and JSON personal-data backup/restore.
- System fonts and desktop text scaling, live Omarchy theme integration, and a
  standard Qt palette fallback on other desktops.

OmaWord supports NET 2.1 only. Full-text search, parallel translations, reading
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
./build/omaword
```

`bin/build` runs `qmake6` in `build/` and then parallel Make. Set `JOBS=4 bin/build`
to override the detected CPU count. Scripts locate the checkout from their own
path, so they can also be invoked from another working directory.
The checkout can stay at `~/Projects/omabible`; the qmake project is still
`omabible.pro`.

For a user-local installation, after building:

```sh
bin/install
```

This requires no sudo. It installs the compiled binary to `~/.local/bin/omaword`,
the desktop launcher to `~/.local/share/applications/omaword.desktop`, and SVG
icons under `~/.local/share/icons/hicolor/`. Desktop files and icons honor
`XDG_DATA_HOME` when set. Ensure `~/.local/bin` is on your desktop session's
`PATH`; then launch **OmaWord** from your application launcher or run `omaword`.
The desktop ID and icon name are both `omaword`; launcher keywords include
**bible** and **scripture**. The regular and symbolic app icons depict an open
book, and the Notes panel control uses a pencil.

The installer also creates `~/.local/bin/omabible` as a compatibility symlink to
`omaword`; the build similarly provides `build/omabible → omaword`. It removes an
old local `omabible.desktop` only if it is a regular, non-symlink file matching
the previously shipped launcher byte-for-byte. Customized launchers and symlinks
are preserved.

### Arch package from the local checkout

```sh
cd packaging
makepkg -s
sudo pacman -U ./omaword-0.1.0-1-*.pkg.tar.zst
```

Run `makepkg` as your regular user. The recipe builds the enclosing source
checkout, including uncommitted changes, into makepkg's `src/build/`; no downloaded
source archive or manually generated tarball is required. Keep `packaging/` inside
the checkout: the recipe is for local builds, not a standalone source package.
Make honors `MAKEFLAGS` from your makepkg configuration. The package installs to
`/usr/bin`, `/usr/share/applications`, and `/usr/share/icons/hicolor`, with the
license and README under `/usr/share/licenses/omaword` and `/usr/share/doc/omaword`.
`packaging/PKGBUILD` names the package `omaword`, provides/conflicts with/replaces
the legacy `omabible` package, and installs `/usr/bin/omabible → omaword` for
existing scripts.

### Branding and compatibility

`branding.env` is the central source for the public name, executable/package ID,
version, launcher keywords, and permanent storage ID. qmake uses `branding.pri`
and `src/brand.h.in` to generate a build-local `brand.h` with the `Brand` helpers,
and `assets/launcher.desktop.in` to generate `build/omaword.desktop`. Install and
packaging scripts read the same metadata. Visible titles, errors, About text,
and export headings use **OmaWord**.

The rename deliberately preserves `~/Projects/omabible`, the application-data
identity `omabible`, shared instance locks, the `omabible-backup` JSON format,
and the `omabible-notes.md` export filename. Existing data is reused in place;
no data-directory rename or migration is required by the branding change.

## Bring your NET 2.1 EPUB

1. Start OmaWord and choose **Add a version** in setup or the command palette.
   This opens an instruction guide for **NET Bible 2.1 only**, not a translation
   catalog. Other Bible versions and file formats are not supported.
2. Use the guide's link to the official [Bible.org downloads page](https://bible.org/downloads).
   In your browser, download **Full Notes Version 2.1 ePub** (`NETBIBLE21.epub`).
3. Return to the guide, choose **Choose EPUB…**, and select your local `.epub` file.
   OmaWord validates the complete NET 2.1 edition and imports it into its own local
   SQLite database. The guide shows the actual data directory.
4. After a successful import, you can move or delete the source EPUB. Normal
   reading and subsequent launches use the database without an internet connection.
   Reimporting the same supported edition replaces Bible data while preserving
   notes, bookmarks, highlights, and settings.

The command-line import option is also available:

```sh
omaword --import "$HOME/Downloads/NETBIBLE21.epub"
```

The direct `--import` CLI remains available, including through the legacy
`omabible` symlink. Only your explicitly selected local file is imported. The EPUB,
Bible text, and generated Bible database are not bundled with the application or
package. OmaWord runs no Bible download server and does not automatically fetch
anything from Bible.org; you download the text yourself in your browser. The
optional real-EPUB UI test verifies reading and reopening after removing a
temporary copy of the source EPUB.

## Reading and references

Use **Search for a passage** (**s** or **Ctrl k**), enter a reference, and press
Enter; or browse a book, chapter, and verse in the palette. Turn off **Verse picker**
with **p** or in Reading settings
to open chapters directly when browsing. Direct references work with either setting:

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

Click the reading surface to give it keyboard focus. Click a **verse number** to
select a verse; Shift-click another number to select a range. Drag over text for
free text selection, including across verse boundaries, and press **Ctrl c** to
copy exactly that text. Annotation commands use the numbered verse selection.
The two **Copy passage** commands copy that range, or the entire chapter when no
range is selected:

- **Copy passage as quotation** (**y**) includes superscript verse numbers,
  typographic quotation marks, a blank line, and an em-dash reference, with no
  translation/version suffix: `“¹⁶ … ¹⁷ … ¹⁸ …”\n\n—John 3:16–18`.
  **Ctrl c** also copies this quotation when a verse range is selected and there
  is no free text selection. With free text selected, **Ctrl c** keeps its normal
  selected-text behavior; editable fields also retain normal text copying.
- **Copy passage as plain text** (**Y**, meaning **Shift Y**, or **Ctrl Shift C**)
  omits quotation marks, reference, and verse numbers. Both passage formats
  normalize whitespace and join verses with spaces, independently of display settings.

Right-click a verse's text or number for a quotation; middle-click for plain text.
Either gesture copies the full canonical range, including off-screen chapters,
when the clicked verse is inside it, preserving that range and its cursor. An
outside click copies only the clicked verse and makes it the focused verse
selection after a successful copy.

Normal mode moves the verse cursor with `j` / `k`. Press `v` to anchor a visual
selection, then continue with `j` / `k` to extend or shrink it. `J` / `K` enters
visual mode and extends immediately. At chapter boundaries, `j` continues to the
next chapter and `k` to the previous one, skipping omitted NET verse numbers.
Press `x` to highlight the entire range, including its off-screen chapters.
Press `v` again to collapse the range to its cursor. Saved ranges reopen at their
active ending chapter. The current range appears in the center footer control
(the chapter reference when no range is selected), its tooltip, and notes.
Floating bottom controls leave the reading surface full-height: visual mode on
the left, the reference in the center, and Saved, Notes, and Keybindings icons on
the right.

The palette's **Search** tab previews the canonical destination before you press Enter. When
browsing, Right Arrow or Enter advances from books to chapters and, when enabled,
from chapters to verses. Left Arrow goes back one level when the input is empty
or its caret is at the beginning. Up from the first result selects the back
control; Enter goes back. The palette's keyboard hints change with its current
mode. **Space** reopens the last palette with its query, browsing location, and
selected result. **Esc** first clears nonempty query text (keeping the `>` command
prefix); another **Esc** closes the palette. The snapshot taken before clearing
is retained for **Space** to resume. **Ctrl k** starts a fresh passage palette;
**Ctrl Space** or **Ctrl Shift P** starts fresh in Commands. **Shift Space** has
no reader action.

Type `20g` to select verse 20 of the current chapter. Invalid or omitted verse
numbers leave your location unchanged. A numeric prefix expires after two seconds
and clears when you change focus or use another command. Verse movement keeps
three lines of context around the selected verse where space permits.

Counts also compose with motions: `4k` moves up 4 verses, `3` then Down moves down
3, and `2J` extends visual selection by 2 verses. Chapter motions accept counts
too (`3l` advances 3 chapters). Counted ordinary verse motions can cross chapters;
counts on bulk motions multiply their step while retaining the chapter-edge stop.
With no verse selected, `4j` selects the 4th existing verse and `4k` the 4th from
the chapter's end. Enter counts without an intervening Space, which opens the palette.

Bulk movement uses **Ctrl d** / **Ctrl u** for five verses and **Ctrl f** /
**Ctrl b** or **PageDown** / **PageUp** for ten. These moves stop at the current
chapter's edge; another press at the edge crosses to the adjacent chapter by one
verse. Omitted NET verse numbers are skipped.

### Reading settings

Press **Shift R**, or open Commands with **Ctrl Space** / **Ctrl Shift P** and
choose **Reading settings**.
The individual **Show verse numbers**, **Show headings**, **Verse picker**,
**Block mode**, **Focus mode**, **Line height**, and **Reset line height** commands
also configure reading directly.
Preferences save synchronously in `personal.sqlite` and survive restarts:

| Preference | Default | Control |
| --- | --- | --- |
| Verse numbers (`showVerseNumbers`) | On (`true`) | `r` |
| Headings (`showHeadings`) | On (`true`) | `t` |
| Verse picker (`versePicker`) | On (`true`) | `p` |
| Block mode (`blockMode`) | Off (`false`) | `b` |
| Line height (`lineHeight`) | `1.5` | Ctrl [ / Ctrl ]; `Ctrl \` resets to `1.5`; range `1.0`–`3.0`, steps of `0.1` |
| Focus mode (`focusMode`) | Off (`false`) | `f` |

Reading settings places each label on the left, its shortcut hint beside a
right-aligned square switch. The toggle shortcuts and line-height adjustment/reset
shortcuts work while the settings dialog is open, including when its slider has focus.
Toggles show brief on/off feedback, including when switching focus mode.
Reader text-size changes show a **Text size …** toast, including in focus mode,
only when the size actually changes; a reset at the default or a change past a
size limit produces no new feedback.

Block mode flows verses inline through one text document, with optional superscript
verse numbers. Visible section headings start paragraphs; poetry and within-verse
breaks remain. Toggle **b** to return to the default separate verse rows.

Focus mode keeps the chapter header visible while temporarily hiding verse numbers,
section headings, the edition label, floating controls, the reader scrollbar, and annotation markers/accents.
Highlight fills and selection remain visible. It preserves the stored verse-number
and heading preferences, so leaving focus mode restores their chosen visibility.
Click the reader to focus its keyboard controls; press **f** again or **Esc**
to leave focus mode. **Esc** in an open panel dismisses that panel first.

### Notes, bookmarks, and highlights

Press **n** to write a note, or **Ctrl n** to toggle **Notes panel**, including while the
editor has focus. Notes slide in as an animated overlay at every window width;
opening them does not reflow the passage. The overlay includes Markdown editing
and preview, and can be dismissed with **Esc** (also from the editor) or an outside click.
Opening or closing Notes produces no Notes on/off toast.

Notes save as you type and belong to an **exact verse range**. A note for
`John 3:16–18` is distinct from a note for `John 3:16`; selecting an overlapping
range does not load or overwrite the other note. Verse note indicators include
overlapping notes. Reopen the exact range from **Saved → Notes** to edit it.

Bookmarks and highlights cover verses. A selection is marked active when all its
actual verses are covered. Toggling a partially covered selection covers all of
it; toggling a fully covered selection removes that flag within the selection,
splitting existing ranges when necessary and preserving other flags and notes.
In the default verse-row layout, one bookmark ribbon marks the start of a saved
range; its margin line continues through the range, including across chapter
boundaries. Hover for the full
reference, or click the ribbon/line to reopen that range. Page icons mark notes.
Block mode uses a left-margin annotation rail with bookmark ribbons, continuous
range brackets, and stacked note icons. If several annotations share a line,
their marker shows a count and opens an exact-reference chooser.

### Keyboard shortcuts

Reader keys apply while reading, with no editable text field or modal dialog
active. **?** or **F1** opens **Keybindings**. Search by action or shortcut, move
through the results with Up / Down, and press Enter to execute an available action.
Home / End selects the first / last result; PageUp / PageDown moves the list one
page at a time. Only actions with bound
keys appear, including **Shift R** for Reading settings, **p** for Verse picker,
and `Ctrl \` for Reset line height. The list shows shortcut hints without On/Off
status prefixes; current setting state remains visible in command-palette hints
and Reading settings.
Mouse gestures and other context-only rows are non-executable guidance. Commands
without bound keys, such as Add a version, remain in the command palette.

Shortcut labels use lowercase letters when Shift is not required (**Ctrl k**,
**Ctrl c**, **Ctrl n**). Uppercase letters indicate Shift: **Y** means **Shift Y**,
and **Ctrl Shift C** includes Shift. The same convention applies to **J**, **K**, and **G**.

| Action | Shortcut |
| --- | --- |
| Search for a passage | `s`, Ctrl k |
| Open commands | Ctrl Space, Ctrl Shift P |
| Resume last palette | Space |
| Saved panel | Ctrl l |
| Previous / next chapter | `h` / `l`, Left / Right, `[` / `]` |
| Previous / next verse | `k` / `j`, Up / Down |
| Move forward / backward 5 verses | Ctrl d / Ctrl u |
| Move forward / backward 10 verses | Ctrl f / Ctrl b, PageDown / PageUp |
| Select numbered verse | Number then `g`, e.g. `20g` |
| Select chapter beginning / end | `g` / `G`, Home / End |
| Toggle visual selection / collapse to cursor | `v` |
| Extend or shrink visual selection | `j` / `k`, Down / Up |
| Enter visual mode and extend | `J` / `K`, Shift Down / Shift Up |
| Bookmark selected verses | `m`, Ctrl m |
| Highlight selected verses | `x`, Ctrl h |
| Write a note | `n` |
| Notes panel | Ctrl n |
| Copy passage (or chapter) as quotation | `y`; Ctrl c with a verse selection and no free text selection |
| Copy passage (or chapter) as plain text | `Y` (Shift Y), Ctrl Shift C |
| Copy free text selection | Ctrl c |
| Reading settings | Shift R |
| Toggle verse numbers / headings / focus mode | `r` / `t` / `f` |
| Toggle verse picker / block mode | `p` / `b` |
| Decrease / increase line height | Ctrl [ / Ctrl ] |
| Reset line height to `1.5` | `Ctrl \` |
| Larger / smaller reader text | Ctrl + (also Ctrl =) / Ctrl − |
| Reset reader text size | Ctrl 0 |
| Dismiss panel / leave focus mode / clear text selection or count / collapse visual range | Esc (context-dependent) |
| Search and execute keybindings | `?`, F1 |

Use Up/Down and Enter in the palette, or prefix its query with `>` to find
commands such as Add a version (the NET 2.1 import guide), export, backup, restore,
and About.

In the right-hand **Saved** panel, `j` / `k` or Down / Up moves between entries;
`h` / `l` or Left / Right selects All, Bookmarks, Highlights, or Notes. Enter opens
the selected entry; Ctrl l or Esc closes the panel. **Delete** or **Backspace**
opens confirmation for the selected entry. Confirming removes the **exact annotation
entry**, including both bookmark/highlight flags and its note, even in a filtered
tab. Other entries, including overlapping ranges, are preserved. Cancel is focused
initially; arrow keys move between Cancel and Delete, Enter activates the focused
button, and Esc cancels and returns to Saved. Bookmark and note glyphs can appear
together (stacked), and a left accent stripe marks a highlighted entry. Entry
navigation and filter keys operate on Saved while it is open, without moving the
underlying reader.

App-owned panels and dialogs have no close-X controls; use **Esc** to dismiss them
(with the palette's clear-then-close behavior above). All app-owned scrollable
surfaces use the same custom overlay scrollbar, including the reader, palettes,
Saved, Notes editing/preview, Reading settings, the import guide, setup, and About.
It reserves no content gutter, widens on hover/drag, and fades after activity.

## Data, export, and backup

On Linux, application data lives in **`~/.local/share/omabible/`**, or
`$XDG_DATA_HOME/omabible/` when set. This is Qt's `QStandardPaths::AppDataLocation`
with an empty organization name and application name `omabible`, deliberately
retained independently of the public OmaWord name and `omaword` desktop ID.

- `bible.sqlite`: the imported, validated NET 2.1 edition.
- `personal.sqlite`: bookmarks, highlights, exact-range notes, reading state, and
  reading preferences.

Existing version-1 personal databases upgrade transactionally to support
cross-chapter ranges. New backups use version 2; restoring version-1 backups is
also supported. Older OmaBible binaries cannot read the upgraded database.
Reading preferences use an additive table in schema version 2.

Use **About & attribution → Open local data folder** to find the actual location.

From the command palette or Saved panel:

- **Export notes as Markdown** writes `omabible-notes.md` in the chosen existing
  folder, replacing that export file. It contains references, flags, and your
  Markdown notes.
- **Save annotation backup** writes personal annotations and reading state as a
  JSON backup with format `omabible-backup`. It does not include reading
  preferences, Bible text, or your EPUB.
- **Restore annotation backup** validates and then **replaces all current annotations
  and reading state** in one transaction; it does not merge or change reading
  preferences. Save a backup of the current data first if you want to retain it.
  Import the NET edition
  on the destination installation before restoring so verse ranges can be checked.

You may keep your original user-provided EPUB for setup on another machine;
it is not needed for normal use after a successful import.

## Tests

```sh
bin/test
OMABIBLE_TEST_EPUB="$HOME/Downloads/NETBIBLE21.epub" bin/test
```

The script builds backend, integrated UI, and block-annotation tests in
`build/tests/`, `build/ui-tests/`, and `build/blockannotations/`, then runs them
offscreen. The legacy `OMABIBLE_TEST_EPUB` environment variable is optional;
real-edition backend and UI integration cases are skipped without it. Other UI
tests use original synthetic text. Tests isolate application data in temporary directories.
`JOBS` controls build parallelism, and arguments are passed to the test executable
(for example, `bin/test -functions`). See [tests/README.md](tests/README.md) for the
backend and UI behavior covered. UI tests exercise palette browsing and resume,
keyboard navigation, free text and verse selection in row/block layouts, quoted
and plain copying, reading preferences and settings shortcuts, actionable
keybindings, exact Saved deletion, notes overlays, restore confirmation, overlay
scrollbars, scroll restoration, the NET-only import guide and source-EPUB
independence, text-size feedback, and narrow/scaled layouts. Use the current test
output for validation results and totals rather than a fixed documented count.

## License and Bible attribution

Application code, documentation, and icons are licensed under the [MIT License](LICENSE),
copyright OmaWord contributors.

NET Bible® copyright © 1996–2019 by Biblical Studies Press, L.L.C. All rights
reserved. The Bible is separately copyrighted and is not covered by OmaWord's
MIT license. Obtain your own EPUB from the publisher and follow the
[NET Bible permissions](https://netbible.com/copyright/). OmaWord distributes no
Bible text or EPUB.
