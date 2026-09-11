# Omabible implementation contract

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

Invokables:
- void importEpub(QUrl file), clearError()
- bool navigate(QString reference) [validate/aliases, sets error on failure]
- QVariantMap resolveReference(QString reference) [pure canonical preview: valid, reference, error]
- void openChapter(int book,int chapter), previousChapter(), nextChapter()
- void selectVerses(int start,int end), clearSelection(), moveSelection(int delta,bool extend=false)
- void toggleBookmark(), toggleHighlight(), setNoteText(QString)
- QString copySelection() [also copy to clipboard, use chapter if no selection]
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

Main.qml ApplicationWindow; context `bible` above and `desktop` below. Design premium restrained keyboard-first reader, header, footer, palette Ctrl+K, on-demand collections drawer and Markdown notes side panel, setup EPUB import dialog, keyboard help, backup/export dialogs. All bindings robust on empty data. QML embedded /qml/Main.qml.
System font throughout. Good margins, theme-aware, responsive >=640px. Standard and vim controls context-safe. Verse number click selects; shift-click range; text selectable. Navigate verse scroll on bible.navigationChanged. Single highlight color. Backend produces one row per verse with safe html preserving poetry/within-verse breaks. UI can present as flowing compact verse rows. Notes editor plain Markdown plus preview via Text.MarkdownText. Clipboard via backend. QML Settings optional window state.

## Desktop (src/desktop.h/.cpp)

QObject context `desktop`: QColor background, foreground, surface, muted, accent, border; QString fontFamily; double textScale; bool dark. Live Omarchy theme reads, system fontconfig family + desktop text scaling. Q_INVOKABLE void openExternal(QUrl), QString version (property or constant). Standard Qt palette fallback outside Omarchy.

Normal j/k moves the verse cursor; v anchors visual selection; visual j/k and
shifted J/K extend it across chapters. h/l navigates chapters, x highlights, m
bookmarks. Keybindings are searchable. UI uses square corners and system fonts.

## Scope

Offline NET 2.1 only, personal annotations + export/restore. No ESV, parallel, fulltext search, plans, cloud. Code MIT, Bible separate copyright. Public-ready build scripts/Arch package, .desktop icon. No git commits unless user requests.
