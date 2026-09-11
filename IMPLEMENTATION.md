# OmaWord implementation contract

C++17, Qt6 Quick/QML + SQL + Concurrent + DBus, libarchive. qmake6 build.
Source root src/, QML qml/. Build, packaging, and desktop integration live alongside the reader and backend.

## Backend (src/biblebackend.h/.cpp, src/epubimporter.h/.cpp)

QObject class BibleBackend, exposed as context property `bible`. Constructor QObject* parent=nullptr.
Properties (NOTIFY as appropriate):
- bool ready, busy; double importProgress (0..1); QString error (user-readable), status
- QString passage (e.g. John 3), selectionReference (e.g. John 3:16–18), edition
- int book (1..66), chapter, selectedStart (0 means none), selectedEnd, chapterCount
- int cursorVerse; bool crossChapterSelection. Visible selection endpoints are the current chapter intersection; selectionReference describes the full span.
- QVariantList verses: maps {number:int, text:QString plain, html:QString safe inline/body markup, heading:QString, poetry:bool, highlighted:bool, bookmarked:bool, hasNote:bool}
- QVariantList books: maps {id:int, name:QString, chapters:int}
- QVariantList collection: maps {book:int, chapter:int, start:int, end:int, reference:QString, bookmark:bool, highlight:bool, note:QString}
- QString noteText (READ/WRITE via setNoteText, autosave), bool selectionBookmarked, selectionHighlighted
- double readingPosition (READ/WRITE; fraction through chapter 0..1, persisted)
- READ/WRITE preferences with preferencesChanged: bool showVerseNumbers=true,
  showHeadings=true, versePicker=true, focusMode=false, blockMode=false; double lineHeight=1.5
  (1.0..3.0 in 0.1 steps). Persist synchronously in personal.sqlite's additive
  preferences table; schema and annotation backup version remain 2. Annotation
  backups exclude preferences, and restore preserves local preferences.

Invokables:
- void importEpub(QUrl file), clearError()
- bool navigate(QString reference) [validate/aliases, sets error on failure]
- QVariantMap resolveReference(QString reference) [pure canonical preview: valid, reference, error]
- void openChapter(int book,int chapter), previousChapter(), nextChapter()
- void selectVerses(int start,int end), clearSelection(), moveSelection(int delta,bool extend=false)
- void moveByVerses(int delta,bool extend=false) [bulk movement stops at
  the chapter edge; the next press crosses one verse into the adjacent chapter]
- QVariantList chapterVerses(int book,int chapter) [verse picker, skips omitted numbers]
- void toggleBookmark(), toggleHighlight(), setNoteText(QString)
- bool deleteAnnotation(QString reference) [delete exactly one annotation matching
  canonical endpoints, including all its flags and note; preserve overlapping entries.
  Validate, DELETE, read back, and commit transactionally before publishing models;
  missing exact entries or SQL/read-back/commit errors roll back without state changes]
- QString copySelection(bool plainText=false) [copy full canonical range, or chapter
  if no selection, to clipboard; preserve selection]
- QString copyVerse(int number,bool plainText=false) [copy full canonical selection
  if number is inside it, otherwise just that verse; after reading succeeds, persist
  and publish the clicked verse as the single selection before publishing clipboard.
  UI inside-range copying uses copySelection instead to preserve range and cursor]
- void copyText(QString text) [free text clipboard copy]
- bool exportNotes(QUrl directory), exportBackup(QUrl file), importBackup(QUrl file)
- void refreshCollection()
- QString dataDirectory()
Signals: navigationChanged() (new chapter or requested verse jump), annotationChanged().
Persist synchronously for reliable saves, debounce may live in UI. Imports background thread, atomic validated DB replacement. Import only explicitly selected local NET Bible 2.1 .epub file (CLI main supports `omaword --import <file>`, also through the compatibility `omabible` symlink).
Personal schema and exported backups are version 2 with canonical range endpoints;
version-1 data migrates transactionally and version-1 backups remain importable.
Verse maps include bookmark range start/end/continuation flags and overlapping
bookmarkRanges and highlightRanges metadata. Logical highlight ranges carry their
identity and continuation endpoints for continuous paint without merging adjacent
independent annotations. Collection references reopen cross-chapter/book spans.
Both passage-copy formats simplify each verse's whitespace and join verses with
spaces in canonical order, skipping omissions. Default quotation format is
`“¹⁶ … ¹⁷ …”\n\n—John 3:16–17`: Unicode superscript verse numbers, typographic quotes,
blank line, em-dash reference, and no edition/version suffix. plainText=true omits
numbers, quotation wrapper, and reference. Formatting is independent of display
preferences; copyText preserves free selected text verbatim.
NET 2.1 EPUB is supplied by the user through a browser download. No bundled EPUB,
Bible text, or generated Bible database; no Bible download server or automatic
Bible.org fetch. After successful validation/import, reading uses local SQLite
and the source EPUB may be moved or deleted. Optional real-EPUB UI coverage
imports a temporary copy, removes it, navigates, and reopens the reader to verify
source independence. Do not copy Bible content into the repository.

## UI (qml/)

Main.qml ApplicationWindow; context `bible` above and `desktop` below. Restrained
keyboard-first reader, floating footer, Search for a passage (s / Ctrl k),
Open commands (Ctrl Space / Ctrl Shift P), Saved panel (Ctrl l), and animated
Notes panel (Ctrl n) at every width without reader reflow. Setup NET 2.1 EPUB import, keyboard help, and
backup/export dialogs. All bindings robust on empty data. QML embedded /qml/Main.qml.
System fonts, good margins, theme-aware responsive layout. Standard and Vim
controls are context-safe. Clicking the reading surface restores reader keyboard
focus. Verse number click selects; Shift Click selects a range. Free text dragging
spans verses, with Ctrl c copying the selected text normally. y copies a quotation;
Y (Shift Y) or Ctrl Shift C copies plain text. With a verse selection and no free
text selection, Ctrl c copies the quotation; editable fields retain normal copying.
Right Click copies a quotation, Middle Click plain text. Inside-range hits use
copySelection to preserve the full canonical range and cursor; outside hits use
copyVerse to focus the clicked verse only after a successful read and state save.
Navigate verse scroll on bible.navigationChanged. Single highlight color. Backend
produces one row per verse with safe html preserving poetry/within-verse breaks.
Notes editor uses plain Markdown plus Text.MarkdownText preview; n writes a note,
Ctrl n toggles the overlay even from the editor; Esc closes it from the editor or
panel, and outside clicking also dismisses it. Clipboard via backend.
Opening/closing Notes produces no Notes on/off toast. Larger/smaller/reset reader
text commands show `Text size N` feedback only on an actual size change, including
in focus mode; no new toast at a size limit or when already at the reset size.

Setup and the unbound **Add a version** command open an instructional import guide,
not a generic translation picker. It states that only the official NET Bible 2.1
EPUB (`.epub`) is supported and links to `https://bible.org/downloads` via
`desktop.openExternal`; downloading is the user's browser action. The guide shows
the local data path, SQLite import/offline behavior, and same-edition replacement
semantics (preserving personal annotations and settings). **Choose EPUB…** closes
the guide before opening the NET 2.1-only local file picker; acceptance invokes
the existing async importer. Draft notes flush before transitions, and
Cancel/Esc returns focus to the reader or Notes. The guide scrolls at small/scaled
sizes while keeping its action buttons accessible.

blockMode=false uses separate verse delegates. blockMode=true loads BlockReader.qml:
one rich TextEdit built by src/blockdocument.h/.cpp (BlockDocument, registered as
OmaWord.Reader 1.0, with OmaBible.Reader retained as a compatibility registration).
Verses flow inline, optional numbers are superscript, and only
visible section headings start new paragraphs. Safe inline formatting and internal
poetry/verse breaks survive flattening of source paragraph wrappers. UTF-16 document
spans map hit testing, free-text drag/autoscroll, verse navigation, and range paint
back to canonical verse numbers. Highlight/selection paint follows wrapped lines,
leading, headings, and bidi runs; distinct adjacent highlights retain a gap.

Passage browsing progresses book → chapter → verse by default; disabling
versePicker opens chapters directly. Direct references remain available in either
mode. Space resumes the last palette query, browsing location, selected row, and
arrow-navigation state. Esc snapshots then clears nonempty query text (retaining
the command prefix); the next Esc closes without replacing that snapshot. Ctrl k
starts a fresh passage snapshot; Ctrl Space or Ctrl Shift P starts a fresh command
snapshot. Shift Space has no reader action. Shift R opens Reading settings;
Reading settings and individual command palette
toggles configure persisted preferences: r verse numbers, t headings, p verse
picker, b block mode, f focus mode. Ctrl [ / Ctrl ] adjusts line height; `Ctrl \`
and the Reset line height command restore 1.5. These settings shortcuts remain
enabled within Reading settings, including slider focus. Setting rows have a left
label, shortcut hint, and right-aligned square switch. Line height opens the
slider-only view; Reading settings opens the full set of controls.

Focus mode temporarily suppresses numbers/headings using showVerseNumbers &&
!focusMode and showHeadings && !focusMode in both layouts; the chapter header stays
visible, while it hides the edition label, floating controls, reader scrollbar, and annotation markers/
accents, retaining highlight fills and selection. Stored number/heading preferences
are preserved. f or reader Esc exits focus mode; panel Esc dismisses the panel first.
App-owned panels/dialogs have no close-X controls. Palette Esc is clear-then-close;
the import busy overlay remains non-dismissible. All app-owned scrollable surfaces
share OverlayScrollbar/OverlayScrollView: overlay-only, no reserved gutter,
18-scaled-pixel hit width, thumb widening from 4 to 10 on hover/press, fading after
activity; reader scrollbar is suppressed in focus mode.

## Desktop (src/desktop.h/.cpp)

QObject context `desktop`: QColor background, foreground, surface, muted, accent, border; QString fontFamily; double textScale; bool dark. Live Omarchy theme reads, system fontconfig family + desktop text scaling. Q_INVOKABLE void openExternal(QUrl); constant QString appName, appId, version properties from the generated Brand helpers. Standard Qt palette fallback outside Omarchy.

Normal j/k moves the verse cursor; v anchors visual selection; visual j/k and
shifted J/K extend it across chapters. h/l navigates chapters, x highlights, m
bookmarks. Keybindings are searchable by action or shortcut; Up/Down selects
results and Enter executes available actions. Home/End selects first/last result;
PageUp/PageDown moves the list one page while preserving a sensible selected row.
Only actions with bound sequences and non-executable context/gesture guidance
appear in Keybindings: Shift R for Reading settings, p for Verse picker, and
`Ctrl \` for Reset line height are included; unbound commands such as Add a version
are command-palette-only. Keybinding hints show only keys, without On/Off status;
command-palette hints still show current setting state. One action
catalog supplies bindings, command entries, setting controls, and dynamic hints.
Display shortcut labels with spaces and lowercase letters without Shift (Ctrl k,
Ctrl c, Ctrl n); uppercase with Shift (Ctrl Shift C, Shift Y, or Y meaning Shift Y).
Qt sequences retain syntax such as `Ctrl+K`, which does not imply Shift. UI uses
square corners and system fonts.
Floating bottom icon controls overlay the full-height reader. Saved overlays on
the right with its own keyboard context; Notes overlays at all widths. The center
footer shows the current selected range, or the chapter when no range is selected.
Reader Left/Right and [ / ] change chapters; Ctrl d / Ctrl u moves five verses;
Ctrl f / Ctrl b and PageDown / PageUp move ten. Bulk moves stop at chapter edges,
then cross one verse on the next press. Counts compose with directional motions
(4k, 3 Down, 2J, 3l); ordinary counted verse motions traverse chapter boundaries.
Numeric prefixes such as 20g select a verse; g/Home and G (Shift G)/End select chapter beginning/end. Bookmark modifiers
use Ctrl m.

Saved owns j/k and Up/Down entry navigation, h/l and Left/Right tab filtering,
Enter opening, and Ctrl l/Esc dismissal. Delete/Backspace captures the current
entry's exact reference and opens confirmation with Cancel initially focused.
Arrow keys move button focus between Cancel and Delete without activating either.
Confirmation deletes that entire annotation (both flags and note), regardless of
active filter; other/overlapping entries survive. Esc cancels back to Saved.
Success syncs the note editor and clamps the list index (empty list: -1); failure
keeps confirmation and models intact with an error. Saved rows stack bookmark and
note glyphs when both apply, with a left accent stripe for highlights.
Normal and symbolic application icons use an open book; the Notes panel control
uses a pencil. Saved and block-annotation note markers retain their page glyphs.

## Branding, build, and compatibility

`branding.env` is the single source shared by qmake and Bash:
`BRAND_NAME=OmaWord`, `BRAND_ID=omaword`, version, launcher keywords
`bible;scripture;Bible;Scripture;`, and permanent `BRAND_STORAGE_ID=omabible`.
`branding.pri` generates build-local `brand.h` from `src/brand.h.in`, exposing
the `Brand` namespace; the application qmake project also generates
`omaword.desktop` from `assets/launcher.desktop.in`. Generated files stay in each
shadow build. Public titles, errors, About/attribution, export headings, CLI version
output, launcher/icon metadata, and installer output use OmaWord.

The executable is `build/omaword`; `omaword` is the primary installed command and
desktop/icon ID. Local installation uses `~/.local/bin/omaword` and
`$XDG_DATA_HOME/applications/omaword.desktop` (default data home
`~/.local/share`), plus regular and symbolic hicolor icons.
`packaging/PKGBUILD` builds package `omaword` from the enclosing checkout and
provides/conflicts with/replaces package `omabible`. Both local and package
installs supply an `omabible → omaword` command symlink; qmake creates the same
compatibility symlink in the build directory after linking.

The local installer removes the old `omabible.desktop` only when it is a regular,
non-symlink file byte-for-byte equal to `assets/legacy/omabible.desktop`.
Customized launchers and symlinks are preserved.

Keep the legacy checkout path `~/Projects/omabible` and project `omabible.pro`.
Qt applicationName remains `Brand::storageId()` with an empty organization name,
so application data stays at `~/.local/share/omabible` or
`$XDG_DATA_HOME/omabible`. Shared instance/import locks retain their identity;
renaming public branding must not fork data or locking. `bible.sqlite` and
`personal.sqlite`, schemas, JSON format `omabible-backup`, Markdown filename
`omabible-notes.md`, legacy QML registration, and `OMABIBLE_*` test environment
variables remain compatible. Branding alone requires no data migration.

## Scope

Offline NET 2.1 EPUB only, personal annotations + export/restore. Add a version is
the NET-only import guide, not support for other translations or formats. No ESV,
parallel, fulltext search, plans, cloud. Code MIT, Bible separate copyright.
Public-ready build scripts/Arch package, .desktop icon. Validation results and
test totals come from current runs, not fixed counts in this contract.
No git commits unless user requests.
