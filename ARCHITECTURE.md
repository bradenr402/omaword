# OmaWord architecture

Durable, non-obvious knowledge for developers. Code wins over this file; the
tests (see [tests/README.md](tests/README.md)) pin the contracts below.

Stack: C++17, Qt 6 Quick/QML (SQL, Concurrent, DBus, Network), libarchive for
ZIPs, libpipewire for sound, qmake. `omaword.pro` builds two static libraries, linked by
the app (`app/app.pro`) and every test suite alike: `src/src.pro` (the C++
core: everything but `main.cpp` and the QML types) and `qml/qml.pro`
(`resources.qrc` with its QML and JavaScript compiled ahead of time by
qmlcachegen, plus the `OmaWord.Reader` types, linked whole so their static
registrations survive).
`resources.qrc` embeds the QML, the seven-entry `resources/catalog.json`, and
the icon. `branding.env` feeds generated headers (`src/brand.h.in`) and desktop
files.

## Components

| File(s) | Responsibility |
| --- | --- |
| `src/usfmimporter.*` | `UsfmImporter`: inspect metadata, parse USFM, stage/validate/publish text DBs |
| `src/biblebooks.*` | Stable 80-book code/name/alias registry (ID = index + 1) |
| `src/versioncatalog.*` | Async HTTPS ZIP download to cache; progress, cancel, errors |
| `src/biblebackend.*` | Per-reader `bible`: active text, references, annotations, tags, Desk, prefs, library ops, backup/export |
| `src/scripturesearch.*` | Normalized, cancellable Bible-wide search on a worker pool |
| `src/searchdocument.*` | Offset mapping into rich text (both directions); LTR forcing helpers |
| `qml/Rule.qml`, `qml/SectionTitle.qml` | Shared looks: the hairline rule between sections and a section's small title (Main's `FineRule`/`SectionLabel` and TagsPanel's `FineRule` wrap them with their theme and scale) |
| `qml/QuietButton.qml`, `qml/FloatingButton.qml`, `qml/ModalCard.qml`, `qml/SettingSwitch.qml`, `qml/SquareSlider.qml`, `qml/OverlayScrollbar.qml`, `qml/OverlayScrollView.qml`, `qml/KeyScrollView.qml`, `qml/RangeMarker.qml`, `qml/VerseTint.qml`, `qml/VerseHighlights.qml`, `qml/FootnoteMarks.qml`, `qml/VerseItem.qml`, `qml/SavedEntry.qml` | Main's controls, each in its own file: panel and footer buttons, modal cards, Reading settings' switches and sliders, overlay scrollbars and key-scrolled panes, the Verse layout's range rails, tints, highlighter strokes, and footnote marks (the chapter read's and those shown beside it), and the Verse and Marks row types. Those that need the window take `controller` (Main) and `theme` (Desktop), and `backend` where they read the Bible; Main binds them once in a same-named inline component (`component QuietButton: Shared.QuietButton { controller: app; theme: desktop }`, `Shared` being `import "."`), so its uses set neither |
| `src/markdownquotes.*`, `qml/QuoteBars.qml` | The Markdown previews' layout (Notes, the Desk): a single Enter breaks the line (`previewMarkdown` turns soft breaks into hard ones, except in lists, tables, and code, where Qt would make a break a new item), a gap of one line of the preview's text (its font's line spacing) above each paragraph, list, quote, heading, and code block (none between a paragraph's lines, a list's items, or a code block's lines; Qt sets none in a TextEdit), and block quotes narrowed from Qt's 40 px on both sides to 14 px on the left with a muted bar per level. Qt keeps no trace of where a quote starts or which blocks a line break split, so zero-width spaces mark them; `MarkdownQuotes` reads them into each block's user state and deletes them, once Qt has finished importing the text |
| `src/textselection.*` | Character selection (`v`) over a chapter's source text: Vim motions/text objects, quotation/plain copy text |
| `src/blockdocument.*`, `qml/BlockReader.qml` | Flowing Paragraph/Block layout, verse↔document mapping, overlays |
| `qml/ContextChapter.qml` | A chapter a selection reaches into, shown whole beside the chapter read with its marks (Main's `contextBefore`/`contextAfter`), see Reader rendering |
| `src/annotationpaint.*` | The flowing layouts' annotation marks (`AnnotationPaint`, a `QQuickPaintedItem`): tint bands and highlighter strokes from HandDrawn.js's shapes ported to C++ (same seeds and arithmetic). BlockReader lays them out (`layOut`, once per event loop turn); a selection move only refreshes `blockSelectionBackground` (plain rectangles), and a changed shape list repaints only the changed shapes' area (Psalm 119 in Block with 88 highlights, 58 noted and 44 tagged verses, offscreen software rendering: a verse move went from 4.3 s to 86 ms, a highlight toggle from 4.3 s to 0.47 s). `BlockDocument::rectangles` walks every character only on lines with right-to-left text |
| `src/windowmanager.*` | One backend + catalog + QML engine + window per reader; request routing; Follow (`openBeside`): links two windows, relaying each one's `followMoved` to the other's `followTo` until either closes or unfollows |
| `src/singleinstance.*` | One process per data dir: `omaword.lock` + bounded local-socket IPC (a request: reference, translation, import, activation token, `newWindow`, `lookup`) |
| `src/executablewatch.*` | After its last window closes, the process (`main.cpp`) stays windowless while it owns the clipboard (a Wayland clipboard lives in the process that copied), so later launches reach it; once an install renames a new executable over its own (`ExecutableWatch`: its directory watched, the file's identity against `/proc/self/exe`), it quits with its last window, or at once if windowless, so the next launch runs the new build |
| `src/commandline.*`, `src/main.cpp` | The command line (`omaword help`). `quote`, `search`, `translations`, `link`, `completion`, `help`, and `--version` run headless: a `QCoreApplication` and one `BibleBackend` (`readPassage`; `readVersion` for `--translation`, which records neither the version as the one you read nor a place in it), with no window, instance, clipboard, or saved place. `open [--here]`, `lookup [text] [-t]` (the lookup window), `import`, an `omaword:` link (`omaword:John%203:16?translation=KJV`; the scheme is the desktop file's `x-scheme-handler/` + `BRAND_ID`, opened where you read), or a bare passage (a new window) become a request for the running instance, whose window reads the passage as the palette does (compact forms too) and switches to the translation asked for. Exit status 0, 1 (failure), 2 (usage). The completion scripts (`assets/completions/`, in `core.qrc`) and the manual (`assets/omaword.1`) are installed by `bin/install` (bash's) and `packaging/`. The app does not quit when its last window closes while it owns the clipboard (a Wayland clipboard lives in the process that copied): it stays windowless until something else is copied |
| `qml/LookupWindow.qml` | The lookup window (`WindowManager::openLookup`, opened afresh on each `lookup` request, so on the workspace you are on): its own `BibleBackend` (`backend`; the version you read, or `readVersion` of the one asked for), `referenceCandidates` as you type, `readPassage` for the passage shown, `y`/`shift Y` copies, `tab`/`shift tab` or the version's menu reading another installed version (`readVersion`, in the order they had when it opened, kept as copies; the candidates follow, and the passage shown is read again by its scope, `readScope`, whatever the version calls the book), and `enter` opening it in the reader you used last (`lookupOpened(reference, versionId)` → `openRequest`, in the version chosen there, if any, else the one asked for). A fixed size, which Hyprland floats; title "Look up · OmaWord" for window rules |
| `src/desktop.*` | Fonts/scaling, live Omarchy theme, async Hyprland corner radius |
| `src/soundeffects.*`, `src/keyhold.*` | Synthesized UI sounds (`lastSound`/`playCount` test hook) + reduce-motion setting; key-repeat handling (j/k/h/l holds) |
| `qml/Main.qml` | Reader, palettes, the Margin (`notesDrawer`, one side panel at a time, `marginMode`: Notes, the Desk page, Marks (`marksPanel`: every version's marks from `marks()`, one row per exact range, filtered, notes written in place), or See also (`relatedPanel`, laid out by TagsPanel)), settings, catalog, About |
| `qml/SidePanel.qml` | The Margin's panel shell: background, the hairline toward the text, and the `inset` and `gap` its content keeps, shown while the Margin is on its `mode`. Every side panel is one, headed by `SidePanelHeader`, with its keys in a `HintRow`; every modal is a `ModalCard` with a `HintRow` (`tst_ui`'s `panelsAndModalsShareTheirShells` checks both) |
| `qml/TagsPanel.qml` | Tag picker/management (modal; opens over the Margin, which stays open) and See also's list (the `related` action, laid out in the Margin's `relatedPanel`, `relatedHost`: one list, in sections: the version's own cross references, its parallel passages, the passages tagged alike (`relatedPassages`, every version), and OpenBible.info's last; filtered by words or `#tag`). See also roots at the reader's passage on open and follows it, except while the panel has the keyboard after opening one of its own rows |
| `qml/PanelHeader.qml`, `qml/SidePanelHeader.qml` | Shared side-panel header: title plus a muted one-line subtitle (Marks, See also, and Notes: the reader's passage `reference · ABBR`, the chapter when nothing is selected; the Desk: its passages and words, `deskSummary`); `SidePanelHeader` adds the panel's buttons and its × |
| `qml/AnnotationGlyph.qml`, `qml/HandDrawn.js` | Shared tag/note glyphs; seeded hand-drawn paths (incl. margin braces/arrows: `wavyArrow`'s shaft is a cubic S, `curvePoints`, leaving and arriving level) |
| `qml/Margins.js`, `qml/ChapterOutline.qml` | Pure margin geometry (space split, note placement, connector tips, heading steps); sticky outline |
| `qml/ExitGhost.qml` | Exit motion for popups that close at once (menus, modals): a frozen `ShaderEffectSource` picture that fades and shrinks toward the trigger, never taking input |
| `qml/AppContextMenu.qml`, `qml/AppToolTip.qml`, `qml/RowMenu.js` | Shared context menu (scrolls when taller than the window; also replaces Qt's text-field menu via `ContextMenu.menu: null` → `openTextMenu`) and themed tooltip; `RowMenu.entries` builds every list row's menu (Open · Copy · Copy plain text · Add to Desk, then the row's extras) |
| `qml/PanelTabs.qml` | Shared tab row (palette Passage/Search/Command): sizing, selected accent tint, focus outline |
| `qml/ChapterFind.qml`, `qml/SearchHighlights.qml` | `/` chapter find; match overlay painting |
| Passage palette (`Main.qml`) | One input over a list (`paletteResults`: books, candidates, recents, commands, search results) or, while a book is chosen, a five-column chapter grid (`paletteChapters`); `paletteList` is whichever shows, so keys, snapshots, and resume treat them alike |
| `qml/HintRow.qml` | Every panel/modal key hint row: parts `[keys, label, run, enabled]`; parts with `run` are clickable while enabled (hover tint, pointing hand), navigation parts never; `text` keeps the plain styled markup |
| `qml/KeyNotation.js`, `bin/shortcuts` | One key notation for every displayed key; README shortcuts generated from Main's `actions` catalog (`--check` in CI) |

## USFM import and catalog lifecycle

- Catalog: one transfer per catalog object; HTTPS only, no API/account. Bounds:
  64 MiB, 30 s stall, 5 min total, no HTTPS downgrade on redirect. `QSaveFile`
  publishes `catalog/<id>_usfm.zip` in the cache only after HTTP/length/ZIP-envelope
  checks. Download and import are separate stages with separate errors; only
  the requesting reader receives `downloadFinished`.
- Local `.zip`/`.usfm`/`.sfm` and CLI `--import` use the same `importUsfm` path.
- Import parses into a staged SQLite DB, validates, reads back, then commits a
  registry pointer to a fresh immutable `bible-<UUID>.sqlite` generation. A crash
  leaves only an unreferenced file. Replacement refreshes affected readers before
  old-file cleanup; ownership checks reject symlinks/hardlinks/foreign targets.
- Opening a generation validates it fully (`quick_check`, counts, both digests)
  once per file identity: `validated-bibles.json` in the cache directory records
  device, inode, size, mtime, and ctime after a pass, and a matching file opens
  without revalidation. Any write in place advances the ctime, so an edited file
  is validated again; a lost or unwritable record only costs a validation.
- Text DB schema is **6** (independent of personal schema): `metadata` (incl.
  digests), `books(...,ordinal)`, `book_aliases`, `source_chapters`,
  `source_keys(book,chapter,number,retained)`, `verses(...,html,heading,poetry,paragraphStart)`,
  `notes(book,chapter,verse,seq,kind,caller,offset,text,targets)` (`\f`/`\fe`/`\x`
  as plain text; `offset` is UTF-16 into the verse's plain text; `targets` = `\xt`),
  `words_of_jesus(book,chapter,verse,start,end)`; `\wj` also renders as
  `<span class="wj">` in `html`; and `indents(book,chapter,verse,levels)`, one
  digit per line of the verse's text (`0` prose, `1`–`4` for `\q1`–`\q4`/`\qm#`,
  other `\q*` at 1), for verses with a poetry line; and
  `parallels(book,chapter,verse,seq,text)`, the `\r` lines under a heading,
  on the verse that opens its section. Schema 3–5 files still validate (own
  digests) and open without notes (3), with every poetry line at level 1 (4),
  or without parallel passages (5). Below schema 5 Versions flags them
  (`needsReimport`, `ReimportTextSchema`) for Re-import; a schema 5 version
  is offered one only when the catalog lists `parallels` among its features.
- Identity: source ID from numbered internal filename suffixes, cross-checked
  with `copr.htm`; else `usfm-<first 32 hex of SHA-256(title)>`. Outer ZIP name
  never matters.
- Books follow **source order** (numbered filenames), not numeric IDs. IDs 1–66
  are GEN–REV, 67–80 deuterocanon (TOB…2ES). `FRT`/`INT` are ignored; KJV+ has
  80 books; ESG keeps chapters 10–16.
- Aliases: `toc1/2/3`, header names, canonical aliases, and USFM codes all
  resolve references (preserves e.g. OJB names).
- Chapters/verses: strictly increasing integers 1–999. Gaps/omissions are kept
  (never manufactured); bridges and suffix labels are explicit errors.
- Sanitization: text is HTML-escaped; only supported inline emphasis survives.
  `\w` keeps the word, drops attributes. Footnotes and xrefs leave verse
  text (kept in `notes`); alt numbers, intros excluded. Notes in headings or
  before a verse anchor at offset 0 of the next verse. Unknown markers / bad UTF-8 fail. Archive limits: 2,048 entries,
  32 MiB/entry, 256 MiB total, nesting 32, 65,536 units per verse/heading;
  unsafe/duplicate paths, links, encryption rejected. `copr.htm` entities are
  decoded without an XML/HTML engine.

## Versions and attribution

- Cross references: the version's own source first (`\x` notes, schema 4),
  then OpenBible.info's community set (CC BY, credited in About and README,
  every row labeled). `crossReferences(scope)` reads the `\x` targets of the
  selection's verses (≤ 50) in source order (verse, note, list order; parsed
  with book/chapter carry-over), then appends OpenBible's targets for the same
  verses by votes (ties in canonical order), each exact range once (the
  source's wins); absent targets and the passage itself are hidden. Between
  them come the source's parallel passages (schema 6): the `\r` lines of the
  section the passage starts in (back to its heading, within its book) and of
  any section starting inside it, parsed like `\xt` lists. Rows carry
  `source` (`own`, `parallel`, `openBible`), `openBible`, and `votes`.
  OpenBible numbers verses as the KJV does; other versifications are looked up
  by the same numbers. `bin/crossrefs` converts
  its `cross_references.txt` at development time (OSIS → USFM codes → book IDs,
  votes ≤ 0 dropped, columnar + delta/varint, `qCompress`ed: ≈ 0.9 MB for
  341k rows) into `resources/crossrefs.bin`, embedded in the core library
  (`src/core.qrc`) and decoded once per process. `hasCrossReferences` (the
  source has `\x` notes) words See also's empty state.

- Seven catalog entries (`engnet`, `eng-kjv2006`, `eng-kjv` = **KJV+**, `engojb`,
  `engylt`, `eng-asv`, `engbsb`) from `https://ebible.org/Scriptures/<id>_usfm.zip`.
  Each also carries a one-line `description`, `year`, `license`, the ZIP's
  `size` in bytes, and `features` (`footnotes`, `headings`, `wordsOfJesus`,
  `apocrypha`), measured from the source when the entry was written (the
  Add version cards show the size as approximate); `VersionCatalog` rejects
  the catalog when any is missing or unknown. Add version lists only the
  entries not installed, and an installed one while it downloads again
  (`downloadId`: Versions' Re-import), showing its progress.
- The reader and About show the imported `copr.htm` title/rights (site footer
  stripped), not the catalog summary. OJB is CC BY 4.0 (attribution required).
- Personal data keys on source ID: same-ID reimport reconnects it.

## Persistence

- Data root: Qt app-data for storage ID `omaword` (`~/.local/share/omaword/`).
- `personal.sqlite`: `PRAGMA user_version` **10**, WAL, `synchronous=FULL`.
  Newer versions refuse to open. Migrations run in one transaction in
  `BibleBackend` (2: cross-chapter endpoints; 3: version scoping + library;
  4: MRU; 5: 80 books / 999 limits; 6: Desk; 7: History; 8: ribbon; 9:
  bookmarks become tags, times; 10: the Desk becomes a page). `tags`/`tag_assignments` are created
  `IF NOT EXISTS` independent of the version number; schemas 9 and 10 run after them.
- Schema 9 (`migrateBookmarks`): every bookmarked range gets the tag
  "Bookmarked" (an existing equivalent name is reused), rows left with no
  highlight or note go, and `annotations` is rebuilt without its `bookmark`
  column. Annotations gain `created`/`updated` and assignments `created` (UTC
  ISO 8601 with ms; `''` = saved before schema 9). Saves stamp them; undo
  restores them with the row, but a step that changes only times is no step.
- Schema 10 (`migrateDeskPage`): the Desk's passages, in their order, become
  quotations on one page (`desk_page`, a single row), and `desk` goes. A
  quotation (`deskQuotation`) is the passage laid out as `copySelection()`
  lays it out (verse numbers only when it spans verses; poetry lines end in a
  Markdown hard break) as a blockquote, then `> — John 3:16 (NET)`; a version
  no longer installed gives just its reference, under its ID.
- Highlights are paint (`toggleHighlight`): a selection not all highlighted
  is painted, and the highlight rows it overlaps or touches (transitively)
  give way to one row over the union; an all-highlighted selection is erased,
  leaving the parts of those rows outside it. Notes keep their own exact
  ranges: a shared row only gains or loses its highlight flag. Older
  overlapping highlight rows elsewhere are left as they are.
- `m` (`markAround`): the smallest mark of this version (an annotation row
  with a highlight or note, or a tag assignment's range) that holds the whole
  selection and more; so pressed again, the next larger. Selected in place
  (`selectVerses`) within the chapter, through `restoreSelection` across
  chapters; otherwise its `reason` (none around it, or the selection is it).
- `gm`/`gM`, `gn`/`gN`, `gt`/`gT` (`nearMark(direction, kind)`): the same
  marks (only notes, only tag ranges), one stop per exact range, ordered by
  (start, end): the first after (before) the selection's, else after the
  chapter's start; selected as `m` selects, as a History jump. `gi`
  (`lastNote`): the note row with the latest `updated` (else `created`).
  `gv` (`earlierRange`, kept by `trackRange` on every selection change): the
  last range of several verses the selection left, unless it only grew or
  shrank from one end.
- Tables: `annotations` (highlight + note per exact range), `reading` (per
  `version_id`); `tags(id,name,name_key)`;
  `tag_assignments` (PK version + both endpoints + tag); `desk_page` (the
  Desk: one Markdown page, global); `history` (global, newest first by `seq`,
  unique per version + endpoints, start/end 0 = a whole chapter, scroll
  position, visit time; capped at 500) and `recent_queries` (last 5 Bible
  searches); `ribbon` (one row: version, endpoints as in `history`, position,
  placed time); `preferences` (global, incl. `historyEnabled`, `wholeBook`); `installed`, `library`.
- History: each window keeps an in-memory jump stack in QML (`[`/`]` browser
  semantics, `-` swaps with the previous location; never saved). Jumps
  (palette, search, panel opens, `gg`/`G`/counts, `gb`, `gm`/`gn`/`gt`, `gi`,
  `gv`, version switches) push it; verse
  and chapter steps do not. A location enters the shared `history` list only
  after the window stays on that chapter for `historyDwellMs` (5 s). Disabling
  History clears both tables and stops recording; it is excluded from backups.
  It has no panel: the passage palette shows up to five recent rows and an
  "All History" row above the books, and `paletteHistory` (ctrl h, or that
  row) lists every row grouped by local day, filtered by the typed query, with
  `delete` / `shift delete` to remove one or clear all (the latter confirms).
- Ribbon: one global reading place, placed only by `placeRibbon()` (`shift R`,
  at `currentLocation()`) and never moved otherwise, removed only by
  `removeRibbon()` (`ctrl shift R`, the tab's and footer's menus, `delete`
  on Continue); shared live by every window (`publishRibbon`), excluded from
  backups and untouched by Clear History. It is
  version-independent: `ribbonLocation()` opens it in the active version when
  that has the passage, else in the version it was placed in, else its chapter
  here. `r` opens it as a jump (`beginJump`/`endJump`). QML shows an accent tab
  pinned at the top right of the column in its chapter, its ribbon and "Mark
  5" in the footer elsewhere (only where it fits without folding anything), and
  "Continue · …" first among the palette's recents.
- Removal keeps annotations/tags by default (hidden until reimport);
  `removeVersion(id, true)` drops them. Global tag definitions and the Desk's
  text always survive.
- The Desk page: `appendToDesk(scope)` flushes every window's drafts, adds the
  scope's quotation at the end (a blank line apart), and records an undo step
  (undone only while the page still reads as it left it); `setDeskPage(text,
  expected)` writes only over the text the editor started from (else "changed
  in another window"), like a note save it runs while windows flush, and both
  publish to every window (`deskChanged`). `cleanDesk()` (Clean Desk, after a
  confirmation) empties it, an undo step too. 4 MiB bound.
- Backups: JSON `format: "omaword-backup"`, version **7**: `tags` (the
  definitions in use), `versions` (each installed version: `versionId`,
  `edition`, `annotations` with highlight, note, and times, `tagAssignments`
  with times, and its `reading` place when it has one), and `deskPage` (the
  Desk's text). Restoring checks every key and range against each version's
  own text (`TagBible`) before writing, and refuses a backup holding a version
  that is not installed; it then replaces, for each version in the backup, its
  marks and reading place, and the Desk page, in one transaction, leaving
  other versions alone. Every range is checked again on export, so a backup
  always restores. Format 6 (the same, with `desk`: the Desk's passages, each
  with its `versionId`) restores its passages as the page's quotations, in
  order. Formats 3–5 (one version: 3 = annotations, 4 = + tags, 5 = + Desk
  passages, added to the page's end) still restore, into the version being
  read, which must match their `versionId`/edition; their `bookmark: true`
  rows assign the Bookmarked tag, and a format 3 restore replaces only that
  tag's assignments. 32 MiB bound. Tag names reuse existing global
  identities; colliding IDs are remapped; existing global definitions are
  never renamed/deleted. Preferences, History, the ribbon, and text are excluded.
- Markdown export: `omaword-notes.md`, every installed version's marks (from
  `marks()`) under a `## Name (ABBR)` heading, tag-only ranges included, tag
  names escaped.
- Sound/reduce-motion settings live in a separate `QSettings` file, global to all windows.
- Sound: each cue plays on its own PipeWire stream (`SoundEffects::Output`, a
  `pw_thread_loop`), so it reaches whatever output is the default at that
  moment; a drained stream goes inactive, so an idle app is never woken. A
  missing or restarted PipeWire is retried after 5 s, never given up. `error`
  and `boundary` sound once per 500 ms burst (a held key), other cues per
  50 ms. The cues: `tick` (a setting toggled), `chime` (copied, saved),
  `remove`, `select` (`m` taking in a mark), `boundary`, `error`. The first cue ever heard (`sound/introduced`) shows how to turn them
  off. Offscreen sessions and `OMAWORD_SOUND=0` never open PipeWire.
- Windows: one QML engine for every reader window (`WindowManager`: Main.qml
  compiled once, one JavaScript heap); each window is created in its own
  child context holding its own `bible` and `catalog`, while `desktop`,
  `sounds`, and `windows` are shared in the root context. A closed window is
  deleted before its context.
- Peer sync: readers in one process share committed annotations, tags, Desk, History,
  prefs, and library; each keeps its own version, passage, selection, scroll.
  Commits publish to peers without navigating them; each peer rereads its
  version's marks (`refreshShared`: about 90–110 ms for 756 marks, offscreen,
  of which reading tag ranges, `tagRangeRows`, is about 30 ms a call, twice).
  Measured before a shared store (one process-wide copy announcing single-row
  changes): a highlight took 1.1 s with one window and 4.6 s with four, almost
  all in `collectionRows`, which merged tag ranges into the marks by a linear
  search; with a keyed merge and one prepared endpoint query per `TagBible`
  it takes 0.31 s with one window and 1.3 s with four, so the shared store
  waits. Each extra window costs about 100 MiB. Imports use a shared busy
  guard; replace/remove/restore flush affected note drafts, and a failed flush
  vetoes. Note saves compare-and-set against the observed baseline; conflicts
  offer **Save my draft** / **Use saved text**.
- Reading ahead: after each chapter opens, the next chapter in source order is
  read on the next idle turn (`prefetchNextChapter`, signals blocked, errors
  kept back) and kept with its generation; opening it uses that read once.
- Note drafts save after a 400 ms pause in typing (`noteSaveDelay`, 0 in the
  GUI test harnesses); every `flushNote()` (selection, navigation, panels,
  window close, peer flushes) saves a pending draft first. `setNoteText`
  reads, writes, and verifies only its own row, and note text is not part of
  the flowing layouts' rebuild key, so typing never rebuilds the chapter.
- Single instance: `omaword.lock` per data dir; secondary launches send a bounded
  request and the primary opens a new reader window.

## Key contracts

### Canonical scopes
- `{versionId,reference,book,chapter,start,endBook,endChapter,end}`. Identity is
  source ID + integer endpoints; `reference` is display only. Endpoints must
  exist and run forward in the source's actual key order. Cross-book OK.
- Unselected chapter: `{versionId,book,chapter,start:0,end:0}`.

### Tags
- Global flat names: trimmed, NFC, Unicode case-folded `name_key`. No hierarchy,
  colors. Reject empty, controls, line/para separators, > 4096 UTF-16 units.
- Creating an equivalent name returns the existing ID (spelling unchanged).
  Rename keeps ID, rejects collisions (merge is explicit). Delete removes all
  assignments in all versions.
- Assignments are version-specific and **exact-range**; overlap ≠ membership.
- `tags` order: name key, then ID. Counts include installed versions only.
- Query results sort by shared count desc, source-ordered start/end, version ID.
- All writes are transactional with read-back before publishing.

### Move here (`changeAnnotationRange`)
- A mark's note and tags move from their exact range to the verses selected
  now, in the same active version (`moveMark`: marker menus, Marks and
  Study rows). Overlapping ranges stay; highlights are paint and never move.
- Collisions: tags dedupe; a different note already there comes first, the
  moved one after a blank line (`needsNoteMerge` with the previewed
  destination token); equal notes dedupe. Undo restores both ranges.
- Snapshot token = SHA-256 of endpoints, source ID, every component (incl. tag
  names) and the text generation. Saving flushes drafts, re-validates the
  source and the preview's destination token in-transaction; any drift
  rejects. A move onto the same range is a true no-op.
- Atomic: rows + assignments commit together or not at all.

### Undo (`BibleBackend::undo`/`redo`)
- Per window and in memory only (`UndoLimit` 100 steps). Recorded: flag
  toggles, note saves, tag assignment (`setTags`, `createAndAssignTag`),
  `deleteAnnotationScope`, `deleteNoteScope`, `changeAnnotationRange`, and
  Desk additions (the page before and after; no scopes). Not recorded: tag
  rename/delete/merge, backups, version removal, typing on the Desk, History;
  deleting or merging a tag and restoring a backup clear this window's steps.
- A step stores, for every exact range it touched, the whole saved state
  before and after (annotation row or none, plus sorted tag IDs), and the tags
  it created. Undo runs in one transaction: every range must still read
  "after" (redo: "before") and every tag to write must exist, else the step is
  dropped with "That changed in another window" and nothing is written. It then
  writes "before", removes created tags left unused (redo recreates them with
  the same ID), reads back, commits, and publishes to every window. A step in
  another version applies there without switching; a removed version's steps
  are dropped.
- Successive `setNoteText`/`setScopeNote` saves on one range extend one step
  (`group`) until `closeUndoGroup()` (the Notes drawer or the in-place editor
  closing), an undo or redo, or any other step, and only while nothing else
  changed the range in between; a session that returns to its starting text
  leaves no step. A new step
  clears redo. Labels read after "Undo"/"Redo" ("highlight on John 3:16").
- QML: `u`/`ctrl z` and `ctrl shift Z`/`ctrl r` in the reader, side panels,
  and character selection (`charKey`); the passage and Marks row menus
  and Commands name the step; failures toast instead of the error banner.
  Deleting from Marks or a marker menu has no confirmation.

### Marks (`marks`, `setScopeNote`)
- `marks()` reads every installed version's marks on demand: one row per exact
  range with its highlight, note, tags, times, and opening verse, ordered by
  the reading version's book order (other versions' marks sort among its
  own). `marksChanged` (any window, any version) and the reader's own change
  signals reread it, once per turn, while Marks is open.
- A row's note is written in the row (`markEditKey`): the draft lives in the
  window (`markEditText`), saves after `noteSaveDelay` with `setScopeNote`
  against the text it started from, and `flushNote()` saves it first, so
  navigation, panels, and window close wait for it. A change from another
  window keeps the draft (`ctrl s` saves it over, `ctrl shift S` takes the
  saved text), and Marks stays open until it is resolved. The rows are not
  reread while the editor is open, which would recreate it.

### Peek
- `PeekCard` is a popover for what the text points to, read in place: a
  modal popup without dim (an outside click only closes it) that opens at
  once below its anchor (above when there is no room), keeps the keyboard,
  and exits through `menuGhost`. Its actions are a buttons-mode HintRow; keys
  it does not take go to the owner (`keyPressed`). Footnotes use it: a
  marker's click or `i` on the cursor verse shows the note, `↑`/`↓` step
  through the chapter's notes (`bible.footnotes`) without moving the reader,
  `y` copies, `enter` opens See also on the note's verse. Compare uses it as
  a list (`rows`, `currentRow`; `peekKind` "compare"): each version's text
  for the selection, opened under its last verse.

### Selection bar
- A verse selection made with the mouse (`mouseSelected`, from Verse rows and
  BlockReader releases) shows a bar above the selection's first line: a
  HintRow in buttons mode (`partHeight`: every part a full-height button,
  edge to edge), inside the reader's flickable so it scrolls with the text;
  it flips below at the top and is placed again after relayouts. Any catalog
  key (`runKey`), `esc`, a new chapter, or losing the reader's keys hides it;
  its own parts and its ⋯ menu keep it up, except those that open a panel or
  dialog (note, tag, Compare, See also).

### Compare
- `compareVersions(scope)` reads the scope's endpoints in every installed
  generation (`TagBible`, read-only), in library order; a version missing
  either endpoint is `available: false`. `c` shows them in a peek under the
  selection (`openCompareCard`: `j`/`k` choose, starting on the first other
  version that has the passage; `y`/`shift Y` copy, `enter` switches), and the
  Versions dialog shows them under each row while a selection exists; copying
  a row is `copyScope` with that version's ID, and switching keeps the
  passage.

### Follow
- `openBeside(window, versionId)` opens a window reading another version
  (`readVersion`: the version you read stays as it was) at the first one's
  place, and links the two: each window's `followMoved(location)` reaches
  the other's `followTo`. A location is the passage (the selection's scope,
  or the chapter) and the verse at the top of the view with how far into it
  the view begins, or how far above the first verse (`followTop`), so
  versions whose verses differ in length stay aligned verse by verse. A
  window sends its place as its selection or scroll changes, at most once a
  frame (`queueFollow`: `Qt.callLater` gathers a frame's changes), so the
  other scrolls along with it, mid-slide too; never while restoring a
  position or placing a moved selection (it sends the place it ends at) or
  applying the other's (`followApplying`). Every frame counts: `followTop`
  bisects the verses' starts (`verseStartAt`, numbers from `verseRows`,
  never converting `bible.verses`' rows). A followed selection in the
  chapter shown moves alone, at once (`followSelect`: `selectVerses`, or no
  selection where this version lacks the verses), with no reload or
  restore; another chapter lands through `restoreSelection` (its chapter
  where the verses are missing) and its scroll once laid out
  (`followRestored`), unless this window has moved on since.
  Each window shows the other's version (`followPartner`, kept current on
  version changes); closing either, or `unfollow`, unlinks both.
- QML asks for it with `besideRequested(versionId)`: `openBeside` (`shift C`,
  Commands, the version button's right-click menu, with Stop following)
  opens the one other version at once, or with more asks which in
  `besideDialog` (the selection's text in each, `enter` opens it); `ctrl
  enter` on a Versions row and `o` in the Compare card open the highlighted
  version.

### Search
- The active version, or every installed version (`searchAllVersions`, a
  global preference: the active one first, then the others most recently
  used first); per-window state. Its keys, `alt a` and `searchRegex`'s `alt r`,
  are catalog scope `search`: live only while the palette holds a Bible search
  (`shortcutContext`); Commands runs them anywhere. Case-insensitive, whitespace-collapsed,
  partial words; punctuation and accents significant. Headings, numbers, notes,
  tags, decorations never searched.
- Bible search: unquoted terms AND within a verse; quoted phrases may span
  adjacent verses; mixed queries anchor on the minimal phrase range. Never
  crosses chapters. Max 4096 units; unbalanced quotes error. A query in
  slashes (`/lov(e|ed)/`), or any query with the `searchRegex` preference
  (`alt r`), is a regular expression (`ScriptureSearch::Syntax::Pattern`,
  chapter find's `compilePattern`: smart case, `\c`/`\C`) matched within each
  verse, a row per verse with all its matches; the index cannot narrow it, so
  every chapter is read. Results come in
  Bible order (the edition's book order, chapter, verse), never ranked;
  `searchBooks` sums them per version and book ({versionId,
  versionAbbreviation, book, name, count, index}) for the palette's book
  headings (with the version's abbreviation while searching every version)
  and `ctrl ↓`/`ctrl ↑` book jumps. A result in another version opens there
  (`openTaggedPassage`).
- The search index (`ScriptureSearch::searchEditions`): `search-index.sqlite`
  in the cache directory, an FTS5 trigram table (case-sensitive: the text is
  already folded) of each chapter's normalized text, its verses joined as the
  scan joins them, per edition, with the edition's identity (path, size,
  mtime) when indexed. Any substring the scan can find in a chapter is a
  substring of that text, so the chapters a query's terms of three or more
  characters MATCH are every chapter it can match in; shorter terms narrow
  nothing (every chapter is scanned). Matching itself is unchanged. An
  edition is indexed on its first search (while it is scanned in full) and
  again when its file changes; editions no longer installed are dropped; a
  file of another format, or one SQLite cannot read, is started over. It is a
  cache: deleting it costs one slower search per version.
- Chapter find (`/`) is a regular expression (`ScriptureSearch::findPattern`,
  `QRegularExpression` with Unicode properties) over each verse's source text:
  matches never cross verses (the old literal find could), empty matches are
  skipped, no whitespace collapse or full case folding. Neovim smart case:
  insensitive unless an unescaped capital appears; `\c`/`\C` force it (`\c`
  wins). A bad pattern returns `error: "invalid pattern"` and no matches; enter
  then sounds the error and the prompt stays. `findInChapter` stays the literal
  / Bible-syntax engine (reapplying a Bible search's matches). `n`/`shift N`
  never wrap.
- `ScriptureSearch` keeps grapheme-safe maps to original UTF-16 offsets; a
  shared corpus cache of the two most recently searched generations (each ≤ 64
  MiB, so two windows on different versions keep theirs); stale requests
  dropped by token.
  `SearchDocument`/`BlockDocument` map those offsets into each layout, skipping
  decoration cells and verse-number labels.

### Passage recognition
- One parser in `BibleBackend`: `referenceCandidates` (palette when the query
  has digits past a leading ordinal; note `[[` suggestions) ranks the exact
  `parseReference` result first, then fuzzy books (exact key > name prefix >
  key prefix > subsequence) × number readings. A digit run is a chapter, or
  splits into chapter:verse only where that verse exists; ties follow source order.
- Links are exact only (`passageLinks`/`findLinks`): capitalized book name or
  standard/source abbreviation, whitespace, existing chapter or
  chapter:verse(-verse). Compact forms and chapter-crossing ranges never link.
  The preview rewrites them to `omaword-passage:` Markdown links; following one
  (click, `alt enter`) is a History jump and keeps Notes open.
- `linkedNotes()`: active-version notes, other than the current exact range,
  with a link overlapping the selection (or the chapter without one).

### Copy
- Preserves stored internal whitespace. Verse joins follow source paragraph
  starts, poetry lines, and chapter breaks regardless of current layout.
- `selectionText()` strips only generated decoration cells; original joiners and
  object characters are kept.
- Selected text (mouse drag or character selection) is mapped back to source
  `{verse, offset}` (`SourcePositionMap::sourceStart/sourceEnd`) and copied by
  `TextSelection::text`: `ctrl c`/`y` a quotation — `…` where the selection
  starts after a verse's first ink or ends before its last (whitespace and
  punctuation trimmed at the cut; spaced from the words, never from the
  quotation marks: `“… so loved`, `world …”`), superscript numbers only for verses whose
  beginning is selected and never for one verse, reference over the verses
  touched after an em dash and a space (`— John 3:16`, as every quotation
  ends: `copySelection`, `copyScope`, `readPassage`) — `ctrl shift C`/`shift Y`
  plain text, never with ellipses. Joins
  follow the source as above; headings and decorations never copy.
- Character selection: positions are source `{verse, offset}` in all layouts;
  each layout maps them to its document and paints its native selection.
  Motions crossing verse breaks (breaks are whitespace) and text objects are
  pure C++ (`TextSelection`); lines (`j k 0 ^ $`) use the layout's geometry.
  Leaving the chapter, layout, or reader keys (a modal) ends it. A relayout
  (text size, line height, any document rebuild) keeps it: the source
  positions are reselected in the new layout (`reapplyCharSelection`).

- A finished mouse selection (drag end, double-clicked word) is offered as the
  PRIMARY selection (`bible.setPrimarySelection`, plain text).
- A drag over the text or the verse numbers, in every layout, selects the
  verses it runs over as it goes (`dragSelectVerses`: from the verse it began
  in to the one under the pointer, persisted like any selection); the
  selection bar waits for the release.

### Keys
- Panel- and mode-local keys are catalog rows with a non-global `scope`
  (`characters`, `tags`, `importGuide`, `versions`, `list`, …):
  listed in Keybindings and the README, bound by their owner, never globally.
- Key discovery: Keybindings (`?`) opens on an Essentials card (`essentialParts`,
  clickable where they act) above the list, sorted by group (the order groups
  first appear in the catalog) under sticky section headers (rows reached by
  key scroll clear of them, `clearKeybindingHeader`). A key waiting for
  another (`keyPending`: `z`, `g`, a count, character selection's `g`/`i`/`a`)
  shows `whichKeyParts` in `keyStrip` after 400 ms; the same strip shows the
  first keys (`introParts`) once, when the library goes from empty to ready,
  remembered in `discovery.ini` (`AppConfigLocation`). `modePill` names Visual
  or Characters above the footer's mode button.
- The reader's `z` and `g` are prefixes (`zPending`, `gPending`), not Qt's
  two-key sequences, whose partial match swallows unrelated keys across focus
  changes: the catalog's `"Z, …"` and `"G, …"` rows are bound as single keys
  enabled only while their prefix waits, and `shortcutContext` turns off the
  catalog keys they would collide with (for `g`, every reader key: a key
  after a lone `g` is spent, as in Vim). `g` keeps a count for `gg` (`20gg`,
  `first`); `esc`, a focus change, or 2 s (`countTimer`) end it. The
  lists' `gg` (`ListMotionKeys`) stays a two-key Qt sequence; Notes' Linked
  from list, which handles its keys itself, spends a key after `g` too
  (`linkedFirstPending`).
- `larger` lists `ctrl =`; `ctrl +` is an unlisted alias (`aliases`). Reading
  settings → Layout and the Aa popover (`readingMode`, still the id) draw
  `paintLayoutGlyph`'s icons. In Reading settings `↑` `↓` and `j` `k` step
  through the controls in tab's order (`settingsControls`, `moveSetting`:
  disabled ones passed over, a boundary sound at the ends); the sliders take
  only `←` `→`, and the page keys still scroll the dialog.
- The side panels are not modal: with one open, the keys of
  `panelLeavingActions` (palettes, dialogs, reading toggles) act over it,
  from its list too, and while you write in a note or on the Desk page, the
  launchers among them (`noteEditorLaunchActions`: palettes and dialogs on
  `ctrl` or `F1`) save the draft and open over it, the keyboard returning
  to the page when they close. Focus mode closes the Margin. Dispatch from Keybindings or Commands never closes panels. Catalog shortcuts
  recheck `shortcutEnabled` on activation (a popup's focus change can leave the
  binding stale).
- The Margin (`notesDrawer`) is a non-modal drawer that never dims: it
  docks beside the text where the window holds the comfortable column and the
  reader's widest insets beside it (`notesDocked`), and is a sheet over the
  reader's right side when narrower. Docked, it is 390 px (× scale) or wider:
  it takes the room the text does not need, up to 620 px, and margin notes
  step aside until it closes (`marginSpace`'s `wantNotes`). The window's content gives up
  `notesDock` at once and the reader lays out once at its new width, then
  glides there from where it was (`dockMargin`/`glideFromDock`, the margins
  moving with it); the footer and bottom chrome follow `chromeDock`, which
  glides alongside. Resizes jump. It shows one side panel (`marginMode`):
  the note (`w`), the Desk page (`shift D`), Marks (`shift M`), or See also
  (`shift S`); each panel's key turns it there, and from the reader brings
  the keyboard back to an open one. Marks and See also open a row's passage
  in the reader and stay. The Desk page saves like a note: after a pause and on every `flushNote`,
  only over the page it loaded (`setDeskPage`), keeping a draft another
  window's change lands under (`deskConflict`, `ctrl s` / `ctrl shift S`). The keyboard belongs either
  to the Margin or to the reader (`marginKeys`): focus entering it sets it, and
  only the reader taking the keyboard (`readerFocus`, a text drag) clears
  it, so menus, dialogs, and Qt's own hand-back as a popup closes leave it
  as it was and `restorePanelFocus` returns the keyboard where it came from.
  The drawer's `focus` follows it, so Qt hands the keyboard back to the
  Margin only when it had it. The reader's keys work whenever the keyboard is
  outside it (`readerBase`), and a panel's own keys only while it has it
  (`marksKeys`, TagsPanel's `relatedMoveKeys`); selection changes resync the
  note (`syncNote`) and re-root See also. `esc` in a panel saves and closes
  it (`closePanel`, through `leaveSidePanel("")`; Marks' and See also's after
  leaving their filter or an edit in place); the reader's `esc` closes the
  Margin after Aa, the selection bar, a text selection, and a count, and
  before search or the selection. `ctrl w` (`F6`, `switchPane`) moves the
  keyboard between the reader and the open panel, which stays; it works while
  typing in the panel, and saves its drafts first. `SidePanel` draws its edge
  toward the text in the accent while `marginKeys`; otherwise a panel's
  current row is `inactiveRowColor`, its hints are `panelAwayHints`
  ("ctrl w keys here"), and it recedes to 90% opacity. The text recedes as
  much while the panel has the keyboard, under `readerDim` (a 10% veil of the
  background, so the bottom fade stays opaque over the text); the footer never.
- Notes in Write mode keeps the keyboard in the editor while it is in Notes
  (`notesWriting` / `keepNoteEditorFocus`): clicks on the panel's chrome and
  closing menus or dialogs hand it back. `tab` types a tab (accepts a `[[`
  suggestion instead), `ctrl ↓` moves to Linked from, `esc` returns.

### LTR forcing
- All Scripture layouts are LTR even when a paragraph begins in Hebrew:
  `SearchGeometry::forceLeftToRight` pins every block; plain `Text` gets a
  display-only LRM per line (`leftToRightText` in Main; the chapter outline's
  entries alike). Never copied. The
  Desk page is the reader's own Markdown, like a note: not pinned.

## Reader rendering

- The page: one type scale (`qml/TypeScale.js`) in steps of the body text
  (Text size × system scale) sets the title, headings (body size, semi-bold, no
  marker), verse numbers, margin notes, footnote marks, heading spacing, and
  the poetry indent, in every layout. A heading right under the chapter title
  takes no space above (a document's first block takes no top margin; the
  Verse layout's first row matches). The column's measure is 66 zeros of the
  reader font (CSS `66ch`, `readerMeasure`) plus the verse-number gutter, so it
  follows the Text size, narrowed only by the window.
- Poetry: Paragraph and Verse layouts set each line of a poetry verse as its
  own block (U+2028 becomes a block break, one position either way, so offsets,
  copy, and search maps are unchanged), `level × unit` in from the left with
  wrapped lines one unit further (`SearchGeometry::poetryFormat`; BlockDocument
  at build, `SearchDocument::indentLines` for Verse rows). Block layout stays
  dense, a poem's lines run on as prose: each break is a space there (one
  position too, and source maps match any whitespace run to any other).
- Verse layout: one row per verse; annotation rails packed by
  `packAnnotationLanes` into nearest free gutter lanes by source-row ordinal
  (not verse labels), tie priority tag/note, lane kept for an interval's life.
  The rows (`verseRows`) are made again only when the chapter or its text
  changes; a change of marks (which changes `bible.verses`) updates them in
  place, each row reading `bible.verses[index]` through values that change
  only with their value (the text's HTML, the number, the highlight and
  footnote lists as JSON), so only rows whose marks changed repaint. Psalm
  119 (176 verses, 88 highlighted, 58 noted, 44 tagged; offscreen software
  rendering): a highlight toggle went from 835 ms to 105–127 ms (a 40-verse
  chapter: 183 ms to 33 ms), against Block's 488 ms; opening the chapter is
  still about 2.9 s (Block: 3.3 s), a highlight repaint of every row 1.4 s.
- Chapters a selection reaches into (`contextChapters`: the nearest three
  before and after the chapter read, which is the cursor's) are shown whole
  around it by `ContextChapter`, in the layout chosen, from `chapterRows`
  (one call for every chapter missing; rows with their marks, `markRows`, as
  `updateAnnotations` gives the chapter read; cached per version and text):
  the title and the text with its marks and footnotes, as the reader draws
  them (Paragraph and Block: a `BlockReader` with `beside`, given the
  chapter's rows, place, and selected verses, showing no motion, search, or
  character selection of the chapter read; Verse: rows with `RangeMarker`,
  `VerseTint`, `VerseHighlights`, and `FootnoteMarks`, as the reader's, read
  in place), the selected verses tinted, all but those dimmed in focus mode.
  Their notes are in the margin too (`marginNoteEntries` covers every chapter
  shown, placed by `shownVerseSpans`). A change of marks in any window
  (`collectionChanged`) reads the shown chapters' rows again
  (`refreshContextMarks`): those whose marks changed show again, the verse at
  the top of the view staying put (`verseAtViewTop`); the rest of the cache is
  dropped. A click there (`besideClick`) selects through `selectThrough`
  (shift: from the anchor), its chapter becoming the one read, then does what
  the button does there (passage menu, copy); a footnote mark selects its
  verse, then peeks. The chapters shown keep their place on screen as they
  change (`captureViewAnchor`/`restoreViewAnchor` around a step, a click
  there, or a selection collapsing): the first still shown goes back where
  it was; where the content cannot reach that (it now begins or ends nearer),
  the column is held there and glides into place (`columnHold`). The reading
  position stays the chapter read's (`chapterTopY`, `chapterTravel`).
- Whole book (`wholeBook`, `alt b`): the book's other chapters are shown the
  same way, above and below the chapter read, which stays the selection's
  (moving or clicking into another makes it the one read). `bookWindow`
  holds the chapters loaded, `contextChapters(first, last)` gives them with
  any a selection reaches into; `extendBook` (on scroll, layout, restore)
  adds one at an end within 1.5 screens of the view at a time, under the
  view anchor, and parks chapters more than five screens away
  (`bookParked`: their height; `ContextChapter.parked` keeps the room and
  lays out nothing, reading no rows), unparking those within two. Opening a
  chapter outside the window, or another book, starts it again from that
  chapter. Scrolling 150 screens down Psalms from Psalm 23 on BSB (offscreen
  software rendering) kept a frame at a median 18–26 ms and memory within
  40–90 MB of where it started, where keeping every chapter laid out grew it
  by 0.5–1 GB (Block least, Verse most).
- A verse step (`j`, `k`, `5j`; `moveSelection`, `moveByVerses`) within the
  chapter shown moves only the selection (`stepSelection`): the chapter is
  not read again and no row is announced (`versesChanged`), so no layout
  reads its rows again; a step past the chapter's edge opens the next or
  previous one (`goToSelection`). Holding `j` steps every 33 ms, so a step
  touches nothing that does not change: Block's focus veil (a canvas the size
  of the chapter) keeps its holes outside focus mode, the hidden selection
  bar builds no buttons, and Verse rows read no other row and no selection
  for their opacity outside focus mode. A step on BSB Psalm 119 (offscreen
  software rendering, the whole frame) takes 3.5 ms in Paragraph and Block,
  6.5 ms in Verse (7, 7, and 17.5 ms with a window beside), where re-reading
  every row and repainting the veil took 35–63 ms (78–143 ms beside).
- Paragraph/Block: one `BlockDocument`. Annotation endpoints are reserved
  transparent inline cells (+ word joiners, no font merging) where QML paints
  brackets and superscript glyphs. Outer starts first, inner ends first;
  continuation chevron for ranges entering the chapter. Bracket height follows
  endpoint font ink, not line height.
- `AnnotationGlyph`: the reader's filled marks, supersampled: a pencil for a
  note, a hash for tags. Controls draw their own glyphs (`FloatingButton`,
  `QuietButton`'s `glyph`: close, plus, minus).
- Paint-only overlays (never alter text formats, layout, selection, or copy):
  - Hand-drawn marks (`HandDrawn.js`): deterministic seeded paths (seed from
    kind/version/reference/segment, no `Math.random`) so repaint/scroll is stable.
  - Annotation tints: every highlight/note/tag range covering a verse
    adds one hand-drawn accent layer (`HandDrawn.tintCount`/`tintRuns`, 4.5%
    each, capped at 3), joined across verses that keep the layer; highlights,
    then search matches, paint over them.
  - Highlights are a highlighter pen (`HandDrawn.highlighterStroke`): one
    stroke per text line in the theme's yellow (`desktop.color3`, Omarchy
    `color3`/`yellow`, else the accent), from a little above the x-height to a
    little below the baseline (`HandDrawn.highlightBand`, from the reader
    font's metrics; text sits at the top of its line box) — a feathered pass,
    a chisel-tipped core that tapers at its ends, and a shorter second pass
    darker where they overlap; seeded per range and line. No accent bar. A
    highlighted row in Marks draws the same stroke under its reference.
  - Focus-mode dimming: follows the cursor instantly; only entering/leaving fades.
  - Jump pulse and copy flash rects. Reduce motion disables pulses/flashes/fades.
    A counted verse step (`5j`, `10k`, `↓` `↑` with a count) pulses the verse it
    lands on (`pulseCursor`; in Visual mode that verse, not the range).
  - Programmatic scrolls to a verse (`scrollToVerse`, `zz`/`zt`/`zb`, search,
    jumps) slide via one `readerSlide` animation (75 ms, ease-out) that
    retargets from the current position; `readerTargetY()` is the logical
    position (saved positions use it). User drags stop it. Focus mode keeps the
    cursor verse 40% down. Instant when animations are disabled.
  - Popup motion (interfaces.dev: exits subtler than entrances, from their
    trigger; frequent popups open instantly): menus and modals (palette,
    dialogs, tag picker, range dialogs, and chapter find's bar, which fades
    and sinks out in 85 ms) open and close at once, so a closed
    one stops counting as open, focus returns, and it (or another) reopens
    immediately; `ExitGhost` (`menuGhost`, 75 ms toward the pointer or anchor
    point; `modalGhost`, 85 ms toward the centre, with its dim) plays the
    exit. A right click while a menu is open shows the new menu as the old
    one's ghost leaves. Side panels enter with the full slide and a fade
    (125 ms, ease-out) and exit with a short slide (to position 0.85) and a
    quicker fade (85 ms, ease-in). Hover colors never animate. Disable
    animations: no ghosts, instant panels.
- Footnote markers reuse decoration cells at each note's plain-text offset
  (word joiner + transparent image): `BlockDocument` inserts them in
  Paragraph/Block; Verse rows insert `<img src="qrc:/resources/cell.png">` in
  the row HTML and overlay marks at `positionToRectangle`. `SourcePositionMap`
  skips U+FFFC/U+2060 in rendered text, so search offsets survive; copy strips
  the cells. Clicking one peeks at its note (Peek).
- Words of Jesus: `<span class="wj">` gets an inline color (desktop `color1`,
  i.e. Omarchy `color1`/`red`, else accent) only while the setting is on;
  layout is unchanged. In a version that does not mark them, Reading settings'
  switch is off, disabled, and drawn muted, with the reason under its title
  (`SettingSwitch.note`); the title keeps the other switches' color.
- Focus mode hides decoration cells (incl. footnote markers), rails, and annotation tints.
- Focus mode's layout (no headings, verse numbers, decoration cells, margins,
  scrollbar or footer room) follows `focusLayout`, which `applyFocusLayout`
  flips in one step that settles the new layout before its first frame: the
  single rebuild runs at once (`BlockReader.settle`; `BlockDocument::build`
  is one edit block, so the TextEdit lays out once), margins are re-placed,
  and the reading position (the cursor verse while on screen, else the top
  verse) keeps its place on screen through the reflow, even past the content's
  ends for a moment; entering then slides it to the typewriter line, and a
  reader at the chapter top slides back to it. The chapter title is held as
  far as that hold would move it (`titleHold`), let go with the reader's slide
  (`titleSettle`), so it moves only as the text scrolls. Paint-only effects (dimming,
  the quiet footer) follow `bible.focusMode`. The column and the chapter
  buttons (re-centred without the footer's room) glide (`marginSlide`,
  `chapterNavGlide`) from wherever they are, so toggles retarget.
- Focus mode keeps the footer quiet: only the focus button shows; the others keep
  their places hidden and disabled. Hovering the footer reveals them at once (their
  clicks wait 300 ms, so a click aimed at the focus button is never caught); hiding
  waits 200 ms so moving between the zone and a button never flickers; keyboard
  focus entering the footer reveals them too. The footer's left group is focus,
  Aa, mode, Keybindings; the center group search, passage, ribbon, version.
  Cramped, the footer sheds focus (`footerLeftFits(2)`), then Keybindings
  (`footerLeftFits(1)`), then stacks the right group (`footerStacked`) and
  folds it into "…" (`footerCollapsed`), Aa with it last (`footerShowsText`).
- Aa (`textPopover`): a non-modal popup docked above the left group that keeps
  the keyboard in the reader (its controls all have keys). While it is open
  the reader's view ends above it (`reading`'s bottom margin,
  `textPopoverReserve`), so it never covers text. Aa, `esc`, focus mode, and
  any panel or dialog close it; so does a click anywhere outside it, which
  does nothing else (`textPopoverOutside`, just under it in the overlay and
  over the side panels; a right-click falls through to its menu, the wheel
  to the text).
- Chapter buttons (`chapterPreviousButton`/`chapterNextButton`): chevrons at the
  reader's outer edges, centred in the viewport, mouse only (h/l are the keys),
  disabled at the Bible's first/last chapter; steps are not History jumps. The
  margins reserve their width (`chapterNavReserve` in `Margins.space` insets) and
  they hide when the column would reach them; in focus mode they appear only
  under the pointer (300 ms click guard). The next one keeps the scrollbar's
  width whether it shows or not (`chapterNavScrollbar`), so it never moves as
  the scrollbar comes and goes.
- Keyboard focus hand-off: a `QuietButton`/`FloatingButton` pressed by key that
  hides itself passes the keyboard to its `focusTarget` (else `restorePanelFocus`).
  Modal outside clicks: the tag picker's confirmation watches
  `Overlay.onPressed` with a passive `PointHandler` marking presses inside.
- Margins (off by default; hidden in focus mode): one
  `Margins.space()` split. Margin notes win: they may shift the reader column
  left and sit right after their connector; while they are on, their room is
  kept in every chapter, notes or not, and the scrollbar's width is reserved
  whether it shows or not, so the column keeps its place between chapters.
  The outline takes what is left and never moves the column; where both fit
  at their minimums, the notes shift the column only as far as leaves the
  outline its own. The outline's room is likewise kept while it is on, in
  chapters without headings too (it draws only where there are headings,
  `outlineShown`). `updateMargins()`
  measures verse spans (Verse rows or `BlockReader.verseRangeRects`, re-run on
  `layoutRefreshed`/size changes) into `verseExtents`, then `Margins.place()`
  lays notes out in content coordinates: aligned to the passage top, pushed
  down on overlap, a crowded run rises ≤ one line; each note must share one
  viewport (minus the footer) with its passage, else the longest note in the
  offending run loses one line at a time (elided "…"), else it hides. Braces
  split lines shared by neighbouring passages; tips lean toward each other
  (`Margins.tips`, 0.35 × centre offset, clamped to 20%–80% of each brace's
  height; a one-line note `{` or one-line passage `}` keeps its tip centred);
  their spines are a steady pen stroke
  (`HandDrawn.linePoints` with `accentAmp`). Note text
  is StyledText with exact `passageLinks`. While margin notes show
  (`inlineNoteMarks` false), the reader drops its inline note marks: Verse
  hides note rails, Paragraph/Block rebuild without note cells (`build(...,
  noteMarks)`: one reflow per toggle rather than blank cells in the text). Docked
  Notes narrows the reader, so the margins lay out beside it; as a sheet it
  covers them. The outline's current section is the cursor's when it is on
  screen, else the one at a reading line ¼ down (the chapter title above the
  headings is current before the first one, and jumps to the chapter top);
  `{`/`}` step headings from the same verse (History jumps), `{` ending at the
  chapter top (`Margins.headingStep`'s `top`), even without headings. In the
  whole book the outline is the book's (`bookOutline`: each chapter, then its
  headings, keyed "chapter:verse"), its current entry wherever the reading
  line falls among the chapters shown (`bookSection`), and `{`/`}` step
  through it across chapters (`bookHeadingStep`; a chapter's top is a stop
  where no heading opens it), an entry in another chapter selecting there
  (`outlineJump`). Toggling notes (`applyMarginNotes`, driven
  by an actual `bible.marginNotes` change) fades them in while the column slides left,
  or fades them out before it slides back (`marginSlideOffset`, 100 ms
  ease-out). Toggling the outline (`applyOutline`, from an actual
  `bible.chapterOutline` change; `outlineOn` lags it as `marginNotesOn` does)
  slides it in from the left as it fades in (`outlineLevel`, a panel's 125 ms,
  ease-out) and out the same way (85 ms, ease-in) before its room goes; the
  column moves only where notes yield it room (`slideMarginsTo`). Entering focus mode, shown margins keep their split
  (`leavingSpace`, `marginsLeaving`) and fade out (`marginExit`, 85 ms
  ease-in) while moving with the gliding column (`marginDrift`); leaving it,
  they are placed before they show and fade in where they settle
  (`marginEnter`, 100 ms). Opacity is each margin's toggle level times that
  focus level. A connector is clipped where the column's text, still gliding into
  place beside waiting margins, would cover it (`marginCover`). Resizes,
  chapter changes, and Disable animations end margin motion at
  once (`settleMargins`).
- `Desktop` follows events, never a timer. The theme: `omarchy-theme-set`
  replaces `~/.local/state/omarchy/current/theme` (`rm -rf`, then `mv`), so
  the watcher also watches `current/` and re-arms the theme directory and
  `colors.toml` after every refresh; while `colors.toml` is missing mid-swap
  the last theme stays. Fonts: a change to `fontconfig/fonts.conf` (what
  `omarchy-font-set` writes) reinitializes fontconfig and re-resolves
  `monospace`. Bursts of events make one refresh (50 ms).
- Desktop corners come from an async `hyprctl getoption decoration:rounding`
  at startup and after each `configreloaded>>` on Hyprland's event socket
  (`$XDG_RUNTIME_DIR/hypr/<instance>/.socket2.sock`); requests during a query
  make one more query after it. Failure resets to 0.
  Highlight segments stay square.
- Colors: secondary text (`muted`) is the least prominent of the theme's
  `muted`, `dark_foreground`, `light_foreground` with a WCAG contrast of at
  least 4.5:1 on `background` and no more than `foreground` has; else
  `foreground`. Errors use `red` (else `color1`, else the accent); selected
  text uses `selection` (else the accent mixed into the background).
- Motion: `desktop.animations` is Hyprland's `animations:enabled`, asked over
  its request socket (`.socket.sock`, `j/getoption animations:enabled`) at
  startup and on each config reload; true outside Hyprland or on a bad reply
  (0.56 answers `bool`, older releases `int`). Hyprland reads a request on its
  main thread as soon as a client connects, so the query is written and
  flushed in the same call that connects: left for the event loop, it stalled
  the compositor, and the window's creation with it, for Hyprland's 5 s read
  timeout.
  The app animates only while that and its own Animations setting are on.
- Accessibility: buttons name their action (`label`) and describe their keys
  (`keys`), and key hints name their label with the keys as the description;
  tooltips show both.

## Verification

See [tests/README.md](tests/README.md) for `bin/test` selectors, filters,
environment variables, and fixtures.
