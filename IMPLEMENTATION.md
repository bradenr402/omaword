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
  showHeadings=true, versePicker=true, focusMode=false; double lineHeight=1.5
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
- QString copySelection() [also copy to clipboard, use chapter if no selection]
- QString copyVerse(int number,bool quotation=false) [copy full canonical selection
  if number is inside it, otherwise just that verse; preserve selection]
- void copyText(QString text) [free text clipboard copy]
- bool exportNotes(QUrl directory), exportBackup(QUrl file), importBackup(QUrl file)
- void refreshCollection()
- QString dataDirectory()
Signals: navigationChanged() (new chapter or requested verse jump), annotationChanged().
Persist synchronously for reliable saves, debounce may live in UI. Imports background thread, atomic validated DB replacement. Import only explicitly selected user file (CLI main supports --import).
Personal schema and exported backups are version 2 with canonical range endpoints;
version-1 data migrates transactionally and version-1 backups remain importable.
Verse maps include bookmark range start/end/continuation flags and overlapping
bookmarkRanges metadata. Collection references reopen cross-chapter/book spans.
NET EPUB is supplied by the user. Do not copy Bible content into the repository.

## UI (qml/)

Main.qml ApplicationWindow; context `bible` above and `desktop` below. Restrained
keyboard-first reader, floating footer, passage palette Ctrl K, command palette
Ctrl Shift P, on-demand collections drawer, and animated Markdown notes overlay
at every width without reader reflow. Setup EPUB import, keyboard help, and
backup/export dialogs. All bindings robust on empty data. QML embedded /qml/Main.qml.
System fonts, good margins, theme-aware responsive layout. Standard and Vim
controls are context-safe. Clicking the reading surface restores reader keyboard
focus. Verse number click selects; Shift Click selects a range. Free text dragging
spans verses, with Ctrl C copying the selected text. Right Click copies the clicked
verse or full selected range when clicked inside it; Middle Click copies that same
scope as a typographic quotation followed by a blank line and —reference NET.
Navigate verse scroll on bible.navigationChanged. Single highlight color. Backend
produces one row per verse with safe html preserving poetry/within-verse breaks.
Notes editor uses plain Markdown plus Text.MarkdownText preview; n writes a note,
Ctrl N toggles the overlay even from the editor. Clipboard via backend.

Passage browsing progresses book → chapter → verse by default; disabling
versePicker opens chapters directly. Direct references remain available in either
mode. Space resumes the last palette query and browsing location; Ctrl K starts
fresh. Shift Space has no reader action. Reading settings and individual command
palette toggles configure the persisted preferences. r toggles verse numbers, t
headings, f focus mode, and Ctrl [ / Ctrl ] adjusts line height. Focus mode hides
reader controls and annotation decorations; f or Escape exits it.

## Desktop (src/desktop.h/.cpp)

QObject context `desktop`: QColor background, foreground, surface, muted, accent, border; QString fontFamily; double textScale; bool dark. Live Omarchy theme reads, system fontconfig family + desktop text scaling. Q_INVOKABLE void openExternal(QUrl), QString version (property or constant). Standard Qt palette fallback outside Omarchy.

Normal j/k moves the verse cursor; v anchors visual selection; visual j/k and
shifted J/K extend it across chapters. h/l navigates chapters, x highlights, m
bookmarks. Keybindings are searchable by action or shortcut; Up/Down selects
results and Enter executes available actions. Context-only gestures are explanatory
rows. Display shortcut labels with spaces (Ctrl K), while Qt shortcut sequences
retain their required syntax. UI uses square corners and system fonts.
Floating bottom icon controls overlay the full-height reader. Saved overlays on
the right with its own keyboard context; Notes overlays at all widths. The center
footer shows the current selected range, or the chapter when no range is selected.
Reader Left/Right and [ / ] change chapters; Ctrl D / Ctrl U moves five verses;
Ctrl F / Ctrl B and PageDown / PageUp move ten. Bulk moves stop at chapter edges,
then cross one verse on the next press. Numeric prefixes such as 20g select a
verse. Bookmark modifiers use Ctrl M.

Visible application branding is OmaBible, including titles, errors, documentation,
launcher/icon metadata, and installer output. Binary, applicationName, desktop/icon
IDs, data paths, and schema/backup identifiers remain lowercase omabible for
compatibility.

## Scope

Offline NET 2.1 only, personal annotations + export/restore. No ESV, parallel, fulltext search, plans, cloud. Code MIT, Bible separate copyright. Public-ready build scripts/Arch package, .desktop icon. No git commits unless user requests.
