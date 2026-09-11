# OmaWord implementation contract

C++17, Qt6 Quick/QML + SQL + Concurrent + DBus, libarchive. qmake6 build.
Source root src/, QML qml/. Build, packaging, and desktop integration live alongside the reader and backend.

## Backend (src/biblebackend.h/.cpp, src/epubimporter.h/.cpp)

QObject class BibleBackend, exposed as context property `bible`. Constructor QObject* parent=nullptr.
Properties (NOTIFY as appropriate):
- bool ready, busy; double importProgress (0..1); QString error (user-readable), status
- QString passage (e.g. John 3), selectionReference (e.g. John 3:16–18)
- QString edition, versionId, versionAbbreviation, versionCopyright use
  versionChanged; edition is dynamic, not CONSTANT. QVariantList versions uses
  versionsChanged, with maps {id, abbreviation, name, active}.
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
  global preferences table; personal schema and annotation backup version are 3. Annotation
  backups exclude preferences, and restore preserves local preferences.

Invokables:
- void importEpub(QUrl file), clearError()
- bool switchVersion(QString id), removeVersion(QString id, bool removeAnnotations)
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
Persist synchronously for reliable saves; debounce may live in UI. Imports run on
a background thread and identify explicitly selected local official NET 2.1 or
structured Haiola/eBible KJV 2006 EPUBs. `omaword --import <file>` (also through
the compatibility `omabible` symlink) invokes the same importer; src/main.cpp help
explicitly names NET 2.1 and Haiola/eBible KJV 2006 support.

Root personal.sqlite uses schema 3: annotations and reading are keyed by
version_id; preferences are global; installed(version_id,path,abbreviation,name,
copyright) records root-relative text filenames, and library(id=1,active) records
the active ID. New imports create immutable `bible-<UUID>.sqlite` generations in
the data root. Registration, active ID, and personal state commit in one root
transaction after validation and read-back; obsolete files are removed afterward.
A crash before registration or during cleanup can leave an unreferenced file,
not a partially installed edition. Legacy root bible.sqlite is validated and
registered in place as net-2.1; v1/v2 personal annotations and reading state migrate
transactionally to that ID, preserving preferences and cross-chapter references.
Corrupt or unsafe legacy text stays unregistered without blocking personal-data
migration: NET annotations remain archived and supported NET reimport reconnects
them. Ownership checks restrict text paths to root `bible-<UUID>.sqlite` generations
or `bible.sqlite` for NET. Existing files and sidecars must be regular, owned by
the current user, and singly linked; symlinks, hardlinks, personal-store aliases,
wrong-edition targets, and ambiguous registry targets are rejected. Cleanup
rechecks ownership after commit before unlinking.

Switching and successful imports retain the current canonical passage/selection,
using the destination's actual SQL verse keys. A missing selection endpoint clears
the selection and resets position to the chapter start, with fallback feedback.
Startup restores the active version's persisted reading state. All navigation,
copy, annotation, Saved, and export operations use active-version keys; offsets
are instance-local and never persisted in place of references. Removing an active
version selects another installed version, or publishes ready=false and empty
models if none remain. removeAnnotations=false retains that ID's annotations and
reading state invisibly until same-ID reimport; true deletes both. Global
preferences survive either choice and removal of the last version. Failed
validation, SQL writes, read-back, or commit preserve the previous library/models.

New JSON backups have format omabible-backup, version 3, edition, versionId,
created (UTC), reading, and annotations. They contain only active-version personal
data and canonical endpoints, excluding preferences and Bible text. Restore
requires an installed, active matching versionId and edition name; v1/v2 backups
are NET-scoped and require active net-2.1 with the NET Bible 2.1 edition name.
Validation precedes transactional replacement of only that version's annotations
and reading state. Other versions and global preferences survive. Markdown export
likewise contains only active-version annotations and identifies its edition/ID.
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
Supported EPUBs are supplied by the user through browser downloads. No bundled EPUB,
Bible text, or generated Bible database; no Bible download server or automatic
Bible.org/eBible.org fetch. After successful validation/import, reading uses local SQLite
and the source EPUB may be moved or deleted. Optional real-EPUB UI coverage
imports a temporary copy, removes it, navigates, and reopens the reader to verify
source independence. Do not copy Bible content into the repository.

EpubImporter::Edition contains QString id, abbreviation, name, copyright;
inspectFile(path,error) identifies supported structure without opening or writing
a database and returns an empty ID on failure. Result includes Edition edition.
Stable IDs are net-2.1 and kjv-2006. Text DB metadata stores version_id,
abbreviation, edition, copyright, and schema=1. Legacy NET metadata with edition
only (plus schema) remains valid. Existing canon/chapterMaximum/chapterVerseCount/
omittedVerse helpers retain NET semantics; validateDatabase uses edition-specific
counts. NET validates 31,087 verses; KJV validates 31,102, both over 66 books and
1,189 chapters. The KJV adapter preserves supplied words/italics, Psalm headings,
and poetry while stripping explicit note subtrees. See tests/README.md for exact
metadata identification and validation coverage.

Only those two structured EPUB layouts are supported. The original and exact
replacement local NKJV study EPUBs were assessed as unsupported OCR layouts;
neither is a native Scripture import. Reliable edition metadata and semantic
verse anchors are required, not merely an EPUB extension. NIV remains unimported
and unsupported. See VERSIONS.md for the current support contract.

## UI (qml/)

Main.qml ApplicationWindow; context `bible` above and `desktop` below. Restrained
keyboard-first reader, floating footer, Search for a passage (s / Ctrl k),
Open commands (Ctrl Space / Ctrl Shift P), Saved panel (Ctrl l), and animated
Notes panel (Ctrl n) at every width without reader reflow. Setup supported-EPUB import, keyboard help, and
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
readerSize defaults/resets to 18 and clamps to 8..40 in steps of 1, before desktop
scaling. It is session-local, separate from persisted backend preferences.

Setup and the unbound **Add version** command open an instructional import guide.
It describes official NET 2.1 and structured Haiola/eBible KJV 2006 EPUB layouts,
required metadata/verse anchors, and OCR rejection. Links to
`https://bible.org/downloads` and `https://ebible.org/eng-kjv2006/` use
`desktop.openExternal`; downloading is the user's browser action. The guide shows
the local data path, SQLite import/offline behavior, and same-ID replacement
semantics (preserving personal annotations and settings). **Choose EPUB…** closes
the guide before opening the local EPUB picker; acceptance invokes the async
importer, activating the imported edition and preserving other editions.
The guide confirms both assessed NKJV study EPUBs, including the replacement,
are unsupported OCR layouts that cannot be imported as native Scripture.
Draft notes flush before transitions, and
Cancel/Esc returns focus to the reader or Notes. The guide scrolls at small/scaled
sizes while keeping its action buttons accessible.

The footer abbreviation immediately follows the location; the combined controls
are centered, responsive, and use elision/full-reference tooltips. Click the
abbreviation, press Shift V, or execute **Switch version** to open the searchable
installed-version picker. Arrow/Home/End/Page keys navigate; unmodified
Enter/Return selects and closes, while Shift Enter/Return does nothing. **Remove
version** is an unbound command opening three steps: version → Remove annotations
too? (Keep annotations by default) → final confirmation (Cancel initially focused).
Cancel/Esc cancels removal at every step. Empty/busy states and failed note flushes
are guarded. The reader edition label and About name/rights use the active
metadata; About hides empty rights and shows the NET permissions link only for NET.

Save annotation backup opens an active-edition scope panel before the file dialog.
It names the edition, lists exclusions, explains separate backups per version and
same-ID reimport before backing up retained annotations. Choose file… closes the
panel, rechecks availability/flushes notes, then opens the save dialog. Restore
confirmation likewise names the active edition and its replacement scope.

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
`Ctrl \` for Reset line height and Shift V for Switch version are included;
unbound commands such as Add version and Remove version
are command-palette-only. Keybinding hints show only keys, without On/Off status;
command-palette hints still show current setting state. One action
catalog supplies bindings, command entries, setting controls, and dynamic hints.
Display shortcut labels with spaces and lowercase letters without Shift (Ctrl k,
Ctrl c, Ctrl n); uppercase with Shift (Ctrl Shift C, Shift Y, or Y meaning Shift Y).
Qt sequences retain syntax such as `Ctrl+K`, which does not imply Shift. UI uses
square corners and system fonts.
Floating bottom icon controls overlay the full-height reader. Saved overlays on
the right with its own keyboard context; Notes overlays at all widths. The center
footer shows the current selected range, or the chapter when no range is selected,
with the active version abbreviation immediately to its right.
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
renaming public branding must not fork data or locking. Legacy `bible.sqlite` and
root `personal.sqlite`, JSON format `omabible-backup`, Markdown filename
`omabible-notes.md`, legacy QML registration, and `OMABIBLE_*` test environment
variables remain compatible. Branding alone requires no data migration.

`bin/test` runs backend, UI, block-annotation, then importer-version suites. It
normalizes OMABIBLE_TEST_EPUB and OMABIBLE_TEST_KJV and exports the latter as
OMABIBLE_TEST_KJV_EPUB when that variable is unset/empty. Optional NKJV rejection
fixtures still require explicit environment variables; see tests/README.md.

## Scope

Offline multi-version library for official NET 2.1 and structured Haiola/eBible
KJV 2006 EPUBs, version-specific personal annotations + export/restore. Add version
is the supported-layout import guide. NKJV OCR sources are assessed unsupported;
NIV is unimported and unsupported. No ESV,
parallel, fulltext search, plans, cloud. Code MIT, Bible separate copyright.
Public-ready build scripts/Arch package, .desktop icon. Validation results and
test totals come from current runs, not fixed counts in this contract.
No git commits unless user requests.
