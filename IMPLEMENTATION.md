# OmaBible implementation contract

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
Persist synchronously for reliable saves, debounce may live in UI. Imports background thread, atomic validated DB replacement. Import only explicitly selected user file (CLI main supports --import).
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
NET EPUB is supplied by the user. Do not copy Bible content into the repository.

## UI (qml/)

Main.qml ApplicationWindow; context `bible` above and `desktop` below. Restrained
keyboard-first reader, floating footer, passage palette Ctrl k, command palette
Ctrl Space (no Ctrl Shift P binding), on-demand collections drawer, and animated
Markdown notes overlay at every width without reader reflow. Setup EPUB import, keyboard help, and
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

blockMode=false uses separate verse delegates. blockMode=true loads BlockReader.qml:
one rich TextEdit built by src/blockdocument.h/.cpp (BlockDocument, registered as
OmaBible.Reader 1.0). Verses flow inline, optional numbers are superscript, and only
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
starts a fresh passage snapshot; Ctrl Space starts a fresh command snapshot.
Shift Space has no reader action. Reading settings and individual command palette
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

QObject context `desktop`: QColor background, foreground, surface, muted, accent, border; QString fontFamily; double textScale; bool dark. Live Omarchy theme reads, system fontconfig family + desktop text scaling. Q_INVOKABLE void openExternal(QUrl), QString version (property or constant). Standard Qt palette fallback outside Omarchy.

Normal j/k moves the verse cursor; v anchors visual selection; visual j/k and
shifted J/K extend it across chapters. h/l navigates chapters, x highlights, m
bookmarks. Keybindings are searchable by action or shortcut; Up/Down selects
results and Enter executes available actions. Home/End selects first/last result;
PageUp/PageDown moves the list one page while preserving a sensible selected row.
Only actions with bound sequences and non-executable context/gesture guidance
appear in Keybindings: p for Verse picker and `Ctrl \` for Reset line height are included;
unbound commands such as Reading settings are command-palette-only. One action
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

Visible application branding is OmaBible, including titles, errors, documentation,
launcher/icon metadata, and installer output. Binary, applicationName, desktop/icon
IDs, data paths, and schema/backup identifiers remain lowercase omabible for
compatibility.

## Scope

Offline NET 2.1 only, personal annotations + export/restore. No ESV, parallel, fulltext search, plans, cloud. Code MIT, Bible separate copyright. Public-ready build scripts/Arch package, .desktop icon. No git commits unless user requests.
