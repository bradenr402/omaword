# OmaWord

A keyboard-first, offline Bible reader for Linux with local bookmarks, highlights,
and Markdown notes. OmaWord imports USFM into SQLite and supports independent
reader windows sharing one local library.

## Add a version

A new library starts with no installed versions. **Add version** opens a searchable
catalog of seven editions: NET, KJV, KJV with Apocrypha, OJB, YLT, ASV, and BSB.
The catalog is bundled metadata and can be browsed offline. **Install** downloads
the selected USFM ZIP directly from eBible.org over HTTPS; no service API or account
is involved. See [VERSIONS.md](VERSIONS.md) for sources and attribution.

The download has its own progress, errors, **Cancel**, and **Retry** controls.
After transport checks and ZIP-envelope screening, the reader separately parses,
validates, and imports the archive. Installation is reported only after the backend
succeeds. If another import or an unresolved note draft blocks the handoff, the
cached file stays available for retry, including when a peer window's note flush
blocks import.

**Import USFM ZIP…** opens a local picker accepting `.zip`, `.usfm`, and `.sfm`.
The command-line option `omaword --import <file>` uses the same USFM importer.
Input must carry usable edition metadata and explicit book/chapter/verse markers;
standalone files and partial collections are supported. [USFM.md](USFM.md)
describes the parser and source identity rules.

Successful import activates the edition in the requesting window. Reading and
annotations then work offline, independently of the source file. Reimporting the
same source ID replaces its text while preserving personal annotations and settings.
Catalog availability does not install any versions by itself.

## Reading

- Search references with **s** or **Ctrl k**, or browse books, chapters, and verses.
  Names, USFM codes, common aliases, and imported table-of-contents names are
  searchable, including OJB names.
- Navigation follows the edition's source book order and actual chapter/verse
  numbers. KJV with Apocrypha has 80 reader books; Greek Esther (`ESG`) has chapters
  10–16. Front matter (`FRT`) and introductions (`INT`) are excluded from the reader.
- **h / l** moves between chapters; **j / k** moves between verses. **v** starts
  a range selection; **J / K** extends it. Ranges can cross chapters and books.
- **m** bookmarks, **x** highlights, and **n** writes a note for the exact range.
  **Ctrl n** toggles the Markdown editor/preview overlay; **Ctrl l** opens Saved.
- **y** copies a quotation; **Shift Y** copies plain passage text. Dragging selects
  free text, and **Ctrl c** copies it. Without free text, a selected verse range
  copies as a quotation.
- **b** cycles Paragraph, Verse, and Block layouts. Source paragraphs, poetry,
  headings, and supported inline formatting are preserved; publisher notes are
  excluded from Scripture text.
- **Shift R** opens Reading settings; **?** or **F1** opens searchable, executable
  keybindings. **Ctrl Space** opens Commands, and **Space** resumes the last palette.

Reading preferences save globally, including with an empty library:

| Preference | Default | Control |
| --- | --- | --- |
| Verse numbers / headings / verse picker | On | `r` / `t` / `p` |
| Reading mode | Paragraph | `b` |
| Font size | 18; range 8–40 | Ctrl + / Ctrl −; Ctrl 0 resets |
| Line height | 1.5; range 1.0–3.0 | Ctrl [ / Ctrl ]; Ctrl backslash resets |
| Focus mode | Off | `f` |

## Versions and windows

Click the footer abbreviation or press **Shift V** to switch versions. The picker
places the active edition first, then others by persisted recent use. Its **+**
button opens Add version. Switching preserves an available passage/range and gives
fallback feedback when destination keys are missing.

**Remove version** asks for the edition, whether to retain annotations (the default),
and final confirmation. Retained annotations reconnect on same-source-ID reimport.
Removing the last edition returns readers to setup; global preferences persist.

Each launcher invocation opens a new reader in one coordinated process. **Ctrl
Shift N** also opens a window. Readers share versions, preferences, and
version-specific annotations, with independent active editions, passages,
selections, and scroll positions. Closing the last reader exits.

Concurrent note edits use a transactional saved-text check. A conflicting draft
stays in the editor until **Save my draft** or **Use saved text** resolves it.
Shared replacement, removal, and restore wait for affected drafts to save.

## Local data and attribution

Application data uses `$XDG_DATA_HOME/omaword/`, defaulting to
`~/.local/share/omaword/`. **About & Attribution → Open local data folder** opens
the actual location. `personal.sqlite` holds annotations, reading state,
preferences, and the installed-version registry. `bible-<UUID>.sqlite` files hold
validated text generations. Downloads are cached separately under Qt's cache
location in `catalog/`.

**Save annotation backup** exports the active edition's annotations and reading
state as `omaword-backup` JSON, version 3, with its source `versionId` and edition
name. Restore requires that matching edition to be installed and active, validates
endpoints, and transactionally replaces only its personal data. Bible text,
source files, other editions, and global preferences are excluded. **Export notes
as Markdown** writes `omaword-notes.md` for the active edition.

The empty screen displays **OmaWord**, Add version guidance, and the privacy message
“All your Bible versions, highlights, bookmarks, and notes stay on this device.”
Saved and Notes footer controls appear when a reader is ready. **About &
Attribution** shows the active imported edition and copyright, or **No version
installed**, plus direct-download/local-storage information and source permissions.

Application code, documentation, and icons use the [MIT License](LICENSE). Bible
text has separate rights, shown in the catalog and imported from source metadata.
The application bundles catalog metadata, not Bible text or text databases.

## Build and install

Requirements: C++17, GNU Make, `qmake6`, `pkg-config`, libarchive, fontconfig, and
Qt 6 Core, GUI, Widgets, QML, Quick, Quick Controls 2, SQL with SQLite, Concurrent,
DBus, and Network. Quick Dialogs and Layouts QML modules are required at runtime;
tests also use Qt Test. Native Wayland support uses `qt6-wayland`.

From the checkout, `bin/build` builds `build/omaword`; `JOBS` controls build
parallelism. `bin/install` installs the binary, desktop launcher, and icons for
the current user and preserves replaced installation entries in install backups.
`packaging/PKGBUILD` builds an Arch package from the enclosing checkout.
`branding.env` supplies shared application, launcher, package, and storage names.

See [IMPLEMENTATION.md](IMPLEMENTATION.md) for component boundaries and
[tests/README.md](tests/README.md) for targeted suites and isolated source checks.
