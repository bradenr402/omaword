# OmaWord architecture

Non-obvious contracts and a map to their implementations. Code and tests win.
Working rules are in [AGENTS.md](AGENTS.md); test commands and suites are in
[tests/README.md](tests/README.md). Keybindings and user instructions belong in
the app and README, not here.

## Code map

C++17, Qt 6 Quick/QML, SQLite, PipeWire; qmake builds two static
libraries shared by the app and tests:

- `src/src.pro`: C++ core except `main.cpp` and QML types. Core resources are
  in `src/core.qrc`.
- `qml/qml.pro`: `resources.qrc` (ahead-of-time compiled QML/JS) and
  `OmaWord.Reader` types, linked whole to retain static registrations.
- `app/app.pro`: executable. `branding.env` generates `Brand::` and desktop assets.

| Area | Start here |
| --- | --- |
| Import, text validation | `src/aoimporter.*`, `src/biblebooks.*` |
| Downloads | `src/versioncatalog.*`, `resources/catalog.json` |
| Persistence, references, annotations, library | `src/biblebackend.*` |
| Search | `src/scripturesearch.*`, `src/searchdocument.*` |
| Source-text selection and quotation | `src/textselection.*` |
| Flowing text and offset mapping | `src/blockdocument.*`, `qml/BlockReader.qml` |
| Markdown previews | `src/markdownquotes.*`, `qml/QuoteBars.qml`, Main's `previewMarkdown` |
| Windows, IPC, CLI | `src/windowmanager.*`, `src/singleinstance.*`, `src/commandline.*`, `src/main.cpp` |
| Reader and action dispatch | `qml/Main.qml` |
| Adjacent chapters | `qml/ContextChapter.qml` |
| Tags and See also | `qml/TagsPanel.qml` |
| Margin layout and outline | `qml/Margins.js`, `qml/ChapterOutline.qml` |
| Annotation painting | `src/annotationpaint.*`, `qml/HandDrawn.js` |
| Desktop integration and sound | `src/desktop.*`, `src/soundeffects.*`, `src/keyhold.*` |

## Storage and compatibility

The data root is Qt's app-data path for `Brand::storageId()`. Never use the
user's real data for development or tests.

| Format | Current | Compatibility |
| --- | --- | --- |
| `personal.sqlite` | `PRAGMA user_version = 11` | Transactional migrations; newer schemas refuse to open |
| Text generations (`bible-<UUID>.sqlite`) | Schema 7 (`ao-standard`) | Superseded USFM generations reject; replace through an explicit AO installation |
| JSON backup | `format: "omaword-backup"`, version 7 | Imports versions 3–7; maximum 32 MiB |

- Personal DB uses WAL and `synchronous=FULL`. Schema 9 migrated bookmarks to
  the global Bookmarked tag and added timestamps; schema 10 converted ordered
  Desk passages into one Markdown `desk_page` row; schema 11 bounds book IDs at
  999, as chapters and verses, so new books need no migration (the app checks
  them against `BibleBooks`). Tag tables are created independently before these
  migrations.
- Main tables: `annotations`, `tags`, `tag_assignments`, `reading`, `desk_page`,
  `history`, `recent_queries`, `ribbon`, `preferences`, `installed`, `library`.
  Reading positions are per version; Desk, tags, preferences, History, and
  ribbon are global.
- AO text tables:
  `metadata`, `books`, `book_aliases`, `source_chapters`, `source_keys`, `verses`,
  `notes`, `words_of_jesus`, `indents`. Note offsets and text spans
  are UTF-16 into plain verse text. `source_keys` records omitted verses too.
- Re-download is explicit in Versions. Never silently rewrite a text generation.
- Backup 7 includes installed versions' annotations, tag assignments, reading
  places, used tag definitions, and `deskPage`. It excludes Bible text,
  preferences, History, and ribbon. Restore validates every range against its
  installed version, then replaces only the included versions' personal data
  and the global Desk in one transaction. Unknown versions reject the restore.
- Backup 6's Desk passages become quotations. Formats 3–5 restore into the
  matching active version; old bookmark flags become tag assignments. A format
  3 restore replaces only Bookmarked assignments. Conflicting tag IDs remap;
  existing definitions are never renamed by restore.
- Markdown export is `omaword-notes.md`, grouped by installed version, including
  tag-only ranges. Both exports use `QSaveFile`, create new files at 0600, retain
  replacement permissions, and reject live databases/sidecars and symlinks owned
  by another user (`protectDestination`).

## Import and library lifecycle

- Identity is AO's case-sensitive provider ID, a bounded safe path component.
  The input filename has no meaning. Same-ID replacement reconnects marks.
- Book IDs are stable (1–66 GEN–REV; 67–86 deuterocanon and apocrypha), but
  navigation follows source `order`. Codes match exactly (`BibleBooks::codeId`:
  PSS is the Psalms of Solomon, though "pss" is a Psalms alias). Chapters/verses
  are increasing integers 1–999; gaps remain gaps. Unknown book codes and
  content types explicitly fail.
- A verse number that does not increase (Brenton's split verses, Onkelos's
  verse 0, a retelling's reordered verses) continues the verse before it, in
  reading order: after a space, or on a new line across a break or poetry. It
  has no source key of its own; headings between move up to the verse.
- Standard AO complete JSON imports English LTR editions. Other catalog languages
  remain discoverable with compatibility reasons; no RTL or numbering conversion.
- Text is escaped; words-of-Jesus markup is generated locally. Plain fragments
  retain source whitespace; structural edge whitespace is trimmed with UTF-16
  offsets remapped. Poetry pieces start lines; trailing line breaks do not create
  prose paragraphs. Chapter separators establish paragraph starts.
- Superscriptions and inline headings use the existing heading model; subtitle
  notes anchor at the following retained verse's offset zero, outside copied text.
  A heading after a chapter's last verse (the KJV's subscriptions) is dropped.
  Empty verses retain source keys but have no reader rows. A chapter without a
  retained verse (none at all, or only a note, as Brenton's Proverbs 30) retains
  source metadata but stays out of navigation; empty books reject.
- Limits: 64 MiB input/download, nesting 32, 86 known books, 65,536 UTF-16 units
  per text field. Counts include omitted verses and continuing parts and exclude
  apocryphal books at translation level. Source revision metadata is not a raw
  JSON-byte checksum.
- Imports stage and validate a fresh immutable generation, then atomically
  commit its registry pointer. Refresh readers before removing old generations.
  Ownership checks reject symlink, hardlink, or foreign cleanup targets.
- Opening a generation validates integrity and content digests once per file
  identity. `validated-bibles.json` caches device/inode/size/mtime/ctime;
  missing/unwritable cache only costs validation. A changed ctime invalidates it.
- Next-chapter prefetch is generation-bound and consumed once; it changes no
  signals or reading state and suppresses speculative errors.
- Version removal keeps personal marks unless explicitly asked to delete them.
  Global tag definitions and Desk survive either way.
- Downloads use a separate `VersionCatalog` network manager: HTTPS only, no
  downgrade, 64 MiB, 30-second stall and five-minute total limits. Validate the
  HTTP response and AO JSON identity, then worker-side full import validation before
  publishing to cache; library installation is separate and revalidates its input.
- Catalog precedence is a valid app-data `ao-catalog.json`, then the bundled
  metadata snapshot (`bin/ao-catalog`). Manual refresh validates and atomically
  saves before activation. Failed/corrupt overrides remain recoverable; updates
  never overwrite them. Browsing and ordinary startup make no network request.
- Personal ID conversion is a one-off explicit-path SQLite backup transformation
  outside runtime; orphan marks remain until matching AO editions are installed.
- Versions offers Re-import only while the catalog's `sha256` for an edition
  differs from the one its installed `ao_translation` metadata records
  (`revision`). A stale catalog keeps offering it until refreshed.

## Scopes, writes, and multi-window state

- Canonical scope: `{versionId, book, chapter, start, endBook, endChapter, end}`.
  `reference` is display only. Endpoints must exist and run forward in the
  source's actual order; cross-book ranges are valid. An unselected chapter has
  `start: 0, end: 0`.
- Highlights are paint: adding merges touching/overlapping highlight ranges;
  erasing subtracts only the selected portion. Notes and tags keep exact ranges.
  Overlap is not tag membership.
- Tags are global, flat names: trimmed, NFC, case-folded `name_key`; reject
  controls, line/paragraph separators, empty names, and >4,096 UTF-16 units.
  Equivalent creation reuses the existing ID; rename rejects collisions; merge
  is explicit. Assignments are version-specific. Writes verify before publishing.
- Notes, Marks' inline editor, and Desk save after a 400 ms pause and on
  `flushNote()`. Navigation, panel changes, destructive library operations, and
  window close must respect a failed flush. Saves compare-and-set against the
  loaded baseline; conflicts retain drafts. Never rebuild a chapter for typing.
- `changeAnnotationRange` moves exact-range notes/tags, not highlight paint.
  Destination-note merging requires confirmation. Its snapshot token includes
  endpoints, version, components, and text generation; validate both source and
  destination again inside the transaction. Drift rejects the whole move.
- Undo/redo is per-window, in memory, capped at 100 steps. A step keeps complete
  before/after rows and tag IDs. Apply only if current state matches the expected
  side; otherwise drop it with a conflict, changing nothing. Other-version steps
  apply without switching; removed-version steps drop. Continuous note saves
  group until the editor closes, another action intervenes, or the row changes.
  Tag management and backup restore clear incompatible steps.
- One process shares one QML engine across readers and lookup. Each reader has
  its own child context, backend, catalog, version, selection, and scroll state;
  desktop and sound are shared. Destroy windows before their contexts/backends.
- Committed changes publish to peer readers without navigating them. Imports
  share a busy guard. Cross-window operations flush affected drafts first.
- Follow relays logical location plus top verse/offset, not raw scroll pixels.
  Updates coalesce once per frame and are suppressed while applying a peer's
  update or restoring. Same-chapter selection changes must not reload text.
- History has an in-memory per-window jump stack and a shared persisted list
  after five seconds' dwell. Verse/chapter steps aren't jumps. Disabling History
  clears its tables; ribbon is independent and moves only by explicit placement.
- Desk is one global Markdown page, bounded to 4 MiB. Appending quotations is
  undoable only while the page still matches the result of that action.

## Reader geometry and text mapping

- Three layouts share source text: Verse uses per-verse `SearchDocument` rows;
  Paragraph/Block use one `BlockDocument`. Source positions are `{verse, offset}`,
  with UTF-16 offsets, never HTML or document offsets.
- Inline decoration cells occupy document positions (U+FFFC/U+2060). Offset maps
  skip generated cells and verse numbers; copy strips only generated decorations,
  not original joiners/object characters. Keep overlays paint-only.
- All Scripture blocks are forced LTR, including Hebrew-first paragraphs.
  Plain display lines use a display-only LRM, never copied. User Markdown isn't
  forced LTR.
- Poetry in Verse/Paragraph preserves per-line indentation with hanging wraps.
  Block runs lines together. Replacing a break with a space must preserve mapping.
  In Paragraph, and in copies (`TextSelection::verseJoin`), a poem line always
  has a line of its own: AO editions mark no paragraphs between poetry verses.
  Copies use source paragraph starts and poetry breaks regardless of layout.
- Verse steps inside a chapter only move selection: no reload or `versesChanged`.
  Annotation changes update existing rows/paint; avoid rebuilding all documents
  for a cursor move. Paragraph/Block marks (`AnnotationPaint`) cover only their
  own lines (`area`), and the search layer shows only with matches: never a
  chapter-tall texture. A `Canvas` paints itself whole on every size change,
  even hidden: one as tall as a chapter lives in a Loader active only while it
  paints, or is plain Rectangles (the focus veil, `veilRects`). Qt's
  antialiasing rasterizer costs about the square of a long, nearly level edge,
  so `AnnotationPaint` fills its marks in slices cut on whole device pixels
  (`fillSliced`).
- Cross-chapter selections show whole context chapters around the cursor's
  chapter, using `chapterRows` with marks. Refresh their marks on peer changes
  while preserving the visible top verse.
- Chapter views (Verse: Main's `VerseView`; Paragraph/Block: `BlockReader`) hold a
  chapter's laid-out text. The chapter read's shows in `verseHost`/`blockLoader`;
  a `ContextChapter`'s is `beside`, without the active chapter's
  search/motion/character state. Moving into a chapter shown beside moves views
  between them (`showChapterRead`, `adoptChapterView`) instead of laying either
  out again; keep crossings that way, as their cost must not grow with chapters.
  Views are created on `viewShelf`, their QObject owner: `createObject(parent)`
  would make a `ContextChapter` destroy the view it gave away. `createObject`
  also copies a list it is given: `newChapterView(properties, verses)` gives a
  view its chapter's own rows after (a flowing one is made with none, not the
  chapter read's), or showing it beside reads every row again.
  All views of a layout share one width, and a view keeps its rows unless marks
  differ (`marksKey`): rows kept from `bible.verses` (a live reference) are
  copies parsed from `versesJson`, and code a crossing runs reads the chapter
  read's from its view (`chapterRows`), as each read of `bible.verses` converts
  every row.
  Paint keyed on lists compares values, so a moved view repaints nothing.
  Nested Columns don't lay out with the column: use `layOutReader()`/`settle()`.
- Views not yet shown are made ahead on the shelf (`prepareChapterView`), a few
  verses a turn (`VerseView` rows; `BlockDocument::beginBuild`/`buildMore`),
  one turn after each frame (`nextTurn`, on `frameSwapped`, which it requests:
  turns chained with `Qt.callLater` run ahead of a key's frame), so no turn lays
  out a whole chapter or holds a frame back; taking one finishes it. Views no
  longer wanted go one a frame, after the frame showing what took their place
  (`retireView`: frozen, named apart).
  Without Whole book, `prepareNeighbours` makes the chapters either side of the
  one read, across books, a moment after it changes, then those `[`, `]`, and
  `r` would go to (`jumpTargets`); a step into one moves its view into the
  reader, and the one left is kept (`keepPrepared`) for a step back. Views not
  shown are named apart (`namePrefix`), as tests find the reader's items by bare
  names; tests find the reader's `BlockDocument` through `blockReader`. Views
  wait in the window, invisible (out of it, their text would be laid out only
  once shown), so making them renders frames: `preparing()` says whether any is
  still being made, the chapter read's (its window, or its rows relaid)
  included. A Verse view waiting keeps the text size, spacing, width,
  and focus layout it was made with (`freeze`, `thaw`); any change to them
  (`viewLayoutKey`) drops the views waiting. Only the chapter read's
  `BlockReader` reports selected text or relays out a selection (`beside` ones
  never do).
- A Verse row is laid out at the text size, spacing, width, and focus layout of
  its model entry (`rowEntry`), not the view's: a change reaches the rows
  showing before the next frame and the others a few each frame after
  (`relaySome`, on `frameSwapped`), outward from them, the cursor verse (else
  the first showing) kept in place. `layOutReader()` and `settle()` lay out the
  rows showing first, so measure after them.
- A `BlockReader`'s text takes a new size and width only with its rebuild,
  its old text cleared first (`textSize`, `textWidth`; a width alone a turn
  later), so a text size change lays the chapter out once. Its TextEdit makes
  nodes only for the lines in view (`ItemObservesViewport`, set by
  `BlockDocument`). Inline markers that only move keep their delegates
  (`markerModel`, replaced only as markers come or go; `inlineMarkers` holds
  their places); those of other verses, or where there were none, come a frame
  after the text (`markersWaiting`). Its key splits the verses' part
  (`versesKey`) from the settings', which a setting changing reads again.
- A text size change (`setTextSize`) keeps the reading anchor and lays the
  reader out before its first frame; the rest of the UI keeps its scale
  (`heldTextSize`) until that frame is synced (`afterAnimating`), then follows.
  Code timing frames hooks `afterRendering`: handlers on `frameSwapped` (the
  relay, markers) run their work before later connections.
- A long chapter jumped into in the Verse layout (over `windowRows` verses),
  any read after one (its many rows kept rather than unmade), or any the layout
  switches to with a verse read, gets a view of its own whose rows are made
  about the verse read first
  (`windowTarget`, `makeWindow`), then the rest a few a turn, below then above
  (`growSome`, `makeAbove`), the reader shifted as rows come above so the text
  stays put (`growingRows`: no Follow is sent for it). Rows are verse indexes
  [`rowsFirst`, `rowsEnd`); `rowItem(i)` is verse index i's, if made; `makeTo`
  makes those up to one now. A place as a share of the chapter's height, saved
  or restored, waits for or makes all of its rows.
- Whole-book view reuses context chapters. `bookWindow` loads nearby chapters;
  `bookParked` preserves distant chapters' heights without laying them out, but
  never the chapters either side of the one read. A chapter is added (or
  unparked) only once its view is ready, the next one each way made as soon as
  its neighbour loads.
  `captureViewAnchor`/`restoreViewAnchor` keep retained text stable as chapters
  enter/leave; measure only after `settleReaderLayout()` (a chapter back from
  parked has a stale height until its own `settle()`). Saved positions remain
  relative to the chapter read.
- Verse-layout rows read their view's `verses[row]` in place, so marks and
  another chapter update rows; a view's row model only grows or shrinks. A row
  costs ~1.5 ms to make: its heading, highlights, footnote marks, tint, rails,
  motion and selection rectangles, search canvas, and number tooltip are made
  (Loaders) only where and while they show. A restore places the chapter
  before the next frame (`runRestore`), never after one showing it at the old
  scroll.
- The outline's current section is the cursor's while a step places itself
  (`viewAnchor`) or while the cursor shows at `readerTargetY()`, never a section
  a slide only passes. Its own scrolling glides (`slideMs`).
- `readerSlide` owns programmatic scrolling; `readerTargetY()` is its logical
  destination. User scrolling stops it. Focus layout keeps the cursor 40% down.
- Opening Notes, Desk, or Marks always checks selection visibility, including
  overlay panels and switching between open panels. Any vertically clipped
  selection centers; fully visible selections stay put. Width changes check too.
  `keepSelectionInView` defers until layout settles, skips position restoration,
  and puts an oversized range's start near the top instead of hiding it.
- Character selection begins with `v`, a text drag, or a double-clicked word.
  Mouse entry is quiet, keeps the pointer's end as cursor, and offers no selection
  bar. Escape returns to the underlying verse selection. Gutter drags and plain
  heading selections retain the verse action bar. Positions carry their book and
  chapter; the cursor is always in the chapter read: a motion past its ends
  (`textMotionAcross`) goes on into the next one, which the reader then reads
  (`crossChars`, as a verse step across), the anchor's chapter shown beside
  with its part selected (`paintBeside`); `characterTextAcross` copies across.
- Key headers (`ModeHeader`): one at a time (`headerMode`): keys waiting on
  another (after 300 ms), character selection, Visual mode, a range outside it,
  chapter find's matches, Follow. They float over the reader's top
  (`modeHeaders`), moving nothing; what reveals a verse or character keeps it
  below them (`topSafety`), and the ribbon hangs over them.
- Selection paint: the cursor's verse is tinted (`focusTint`, when `cursorTinted`),
  as is character selection's focused character; a range's verses, and Visual
  mode's from its first (`rangeUnderlined`), and text selected by character get `SelectionMarks`: a
  faint wash (`rangeTint`; Verse rows of a range have a joined band instead) and
  dots under the highlight band. The native TextEdit selection stays as state
  (copy, PRIMARY) but paints nothing: its color is the background at alpha 0,
  since Qt veils selected inline cells with it at half alpha.
- PRIMARY gets dragged text as shown before character conversion. Character
  plain-copy uses source text; quotation adds cut-verse ellipses, numbers only
  for included verse starts (never a single verse), and an em-dash reference.
- Relayout reselects character source positions **without revealing** them.
  Cursor motions reveal explicitly. The character-status bar compensates scroll
  in `workspace.onYChanged`, in the same layout pass; changing scroll immediately
  on bar visibility creates mismatched geometry between events and polish.
- Focus layout changes settle the text shown synchronously before the first
  frame (a Verse view's other rows follow, `relaySome`). Preserve the visible
  verse through reflow; dimming is paint-only. Respect reduced motion.
- Nothing of this window's own may sit in `Overlay.overlay` above the popups:
  Qt offers a press to each overlay child in turn and counts the first pass as
  the popup's one chance to close on a press outside it (6.12), so an item over
  them leaves dialogs open on a click beside them. The mouse's Back and Forward
  buttons, which must act over dialogs too, are watched by `MouseNavigation`
  (an application event filter, scoped to this window) rather than caught by an
  item.
- Margin notes and outline use `Margins.space`/`place`: notes win space, outline
  takes the remainder. Keep reserved room stable between chapters. Measure after
  layout (`BlockReader.settle` where synchronous geometry is required).

## UI, previews, and dispatch

- Main's `actions` catalog drives bindings, Commands, Keybindings, and generated
  README keys. Scoped/explanation entries are bound by their owner, not globally.
  Reader `g`/`z` prefixes use pending state, never Qt multi-key sequences.
- The Margin is one non-modal `notesDrawer`, selected by `marginMode`.
  `marginKeys` tracks keyboard ownership independently of popup focus changes.
  Drafts flush before switching; modal launchers preserve and restore panel focus.
- Shared shells and controls are listed in AGENTS.md. `RowMenu.js` supplies list
  row actions; `HintRow` takes `[keys, label, run, enabled]`. Disabled actions
  explain rejection rather than silently doing nothing.
- `previewMarkdown` prepares user Markdown before Qt imports it. Images become
  links with alt text and a drawn image glyph; HTML displays literally. Preserve
  code, autolinks, and link destinations. Markers carry quote/paragraph structure
  that Qt's importer loses; `MarkdownQuotes` consumes them once import completes.
  It also fixes heading formatting across inline spans and narrows Qt's indents.
- The shared QML engine rejects network requests. Version downloads have their
  own manager. Never let untrusted preview text load local or remote images.
- Search results, Markdown previews, and selection geometry use original UTF-16
  offsets; preserve grapheme boundaries through normalization and display mapping.

## Search, links, and external data

- Bible search: unquoted terms AND within a verse; quoted phrases may span
  adjacent verses, never chapters. Partial words; punctuation/accents matter.
  Whitespace collapses. Smart case; `\c`/`\C` override. Slash-delimited regex
  searches each verse. Results stay in source order, not relevance order.
- Chapter find is regex on each verse without whitespace collapse/full folding;
  empty matches skip, invalid patterns keep the prompt open, next/previous don't
  wrap. Reapplied Bible-search matches use Bible syntax instead.
- `search-index.sqlite` is disposable FTS5 trigram acceleration, not truth.
  Short terms and regex scan all chapters. Indexed text must be a superset of
  every possible matching chapter. Workers drop stale requests by token.
- `referenceCandidates` ranks exact references before fuzzy book/number readings.
  Passage links are narrower: capitalized names/recognized abbreviations plus
  existing chapter/verse ranges; no compact or cross-chapter links. Display
  references must never substitute for canonical scope identity.
- See also retains bundled OpenBible.info references and tagged related passages;
  AO source cross-references and parallel passages are not imported. Absent/self targets hide.
  OpenBible uses KJV numbering, not automatic versification conversion.
- `resources/crossrefs.bin` is **CC BY 4.0, not MIT**. `bin/crossrefs` converts
  OpenBible.info data; About, README, and packaging retain attribution/license.
  Imported version rights come from its source, not the catalog summary.
- Headless CLI uses `QCoreApplication`, no window/clipboard/reading mutation.
  Bare passages open new readers; `omaword:` links use the active reader, even
  without a passage. Lookup uses a separate backend and opens into the last reader.
  Terminal output replaces C0/DEL/C1 controls except tabs/newlines; JSON preserves
  values with escapes (Qt alone leaves DEL/C1 raw).
- Single-instance IPC is bounded and keyed by data directory. A process may stay
  windowless while owning the Wayland clipboard. `ExecutableWatch` detects an
  installed replacement and quits when its final window closes, so a new launch
  doesn't reopen an obsolete build.

## Desktop integration traps

- Theme changes replace the current theme directory: watch its parent and re-arm
  watches after replacement; retain the old colors during transient absence.
  Font changes reinitialize fontconfig and resolve monospace again.
- Hyprland rounding/animation settings refresh asynchronously on config reload.
  Write/flush a request immediately on socket connection: deferring it can block
  the compositor until its read timeout. App motion requires both desktop and
  application animations enabled.
- Sound uses per-cue PipeWire streams, follows the default output, retries after
  server failure, and idles after draining. Offscreen and `OMAWORD_SOUND=0` never
  open PipeWire. Sound/reduced-motion preferences use shared `QSettings`.
