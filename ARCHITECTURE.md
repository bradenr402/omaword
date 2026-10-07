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
| `src/textselection.*` | Character selection (`v`) over a chapter's source text: Vim motions/text objects, quotation/plain copy text |
| `src/blockdocument.*`, `qml/BlockReader.qml` | Flowing Paragraph/Block layout, verse↔document mapping, overlays |
| `src/windowmanager.*` | One backend + catalog + QML engine + window per reader; request routing |
| `src/singleinstance.*` | One process per data dir: `omaword.lock` + bounded local-socket IPC |
| `src/desktop.*` | Fonts/scaling, live Omarchy theme, async Hyprland corner radius |
| `src/soundeffects.*`, `src/keyhold.*` | Synthesized UI sounds (`lastSound`/`playCount` test hook) + reduce-motion setting; key-repeat handling (j/k/h/l holds) |
| `qml/Main.qml` | Reader, palettes, Notes, Annotations (one list for all four tabs; Tags rows grouped under tag headings), Desk, settings, catalog, About |
| `qml/TagsPanel.qml` | Tag picker/management (modal; opens over the Annotations and Study drawers, which stay open) and the Study drawer (tabs Cross-references / Footnotes / Tags; the Tags tab keeps the internal id `related`, as do `relatedDrawer` and the `related` action). Study → Tags roots at the reader's passage on open and follows it, except while the panel has focus after opening one of its own rows |
| `qml/PanelHeader.qml` | Shared side-panel header: title plus a muted one-line subtitle (Annotations and Study: the reader's passage `reference · ABBR`, the chapter when nothing is selected; Desk and History: their count) |
| `qml/RangeEditor.qml` | Annotation range editing session |
| `qml/AnnotationGlyph.qml`, `qml/HandDrawn.js` | Shared bookmark/tag/note glyphs; seeded hand-drawn paths (incl. margin braces/arrows) |
| `qml/Margins.js`, `qml/ChapterOutline.qml` | Pure margin geometry (space split, note placement, connector tips, heading steps); sticky outline |
| `qml/ExitGhost.qml` | Exit motion for popups that close at once (menus, modals): a frozen `ShaderEffectSource` picture that fades and shrinks toward the trigger, never taking input |
| `qml/AppContextMenu.qml`, `qml/AppToolTip.qml`, `qml/RowMenu.js` | Shared context menu (scrolls when taller than the window; also replaces Qt's text-field menu via `ContextMenu.menu: null` → `openTextMenu`) and themed tooltip; `RowMenu.entries` builds every list row's menu (Open · Copy · Copy plain text · Desk, then the row's extras) |
| `qml/PanelTabs.qml` | Shared tab row (Annotations, Study, palette Passage/Search/Command): sizing, selected accent tint, focus outline |
| `qml/ChapterFind.qml`, `qml/SearchHighlights.qml` | `/` chapter find; match overlay painting |
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
- Text DB schema is **4** (independent of personal schema): `metadata` (incl.
  digests), `books(...,ordinal)`, `book_aliases`, `source_chapters`,
  `source_keys(book,chapter,number,retained)`, `verses(...,html,heading,poetry,paragraphStart)`,
  `notes(book,chapter,verse,seq,kind,caller,offset,text,targets)` (`\f`/`\fe`/`\x`
  as plain text; `offset` is UTF-16 into the verse's plain text; `targets` = `\xt`)
  and `words_of_jesus(book,chapter,verse,start,end)`; `\wj` also renders as
  `<span class="wj">` in `html`. Schema 3 files still validate (own digest) and
  open without notes; Versions flags them (`needsReimport`) for Re-import.
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
  source's wins); absent targets and the passage itself are hidden. Rows carry
  `openBible` and `votes`. OpenBible numbers verses as the KJV does; other
  versifications are looked up by the same numbers. `bin/crossrefs` converts
  its `cross_references.txt` at development time (OSIS → USFM codes → book IDs,
  votes ≤ 0 dropped, columnar + delta/varint, `qCompress`ed: ≈ 0.9 MB for
  341k rows) into `resources/crossrefs.bin`, embedded in the core library
  (`src/core.qrc`) and decoded once per process. `hasCrossReferences` (the
  source has `\x` notes) words Study's empty state.

- Seven catalog entries (`engnet`, `eng-kjv2006`, `eng-kjv` = **KJV+**, `engojb`,
  `engylt`, `eng-asv`, `engbsb`) from `https://ebible.org/Scriptures/<id>_usfm.zip`.
- The reader and About show the imported `copr.htm` title/rights (site footer
  stripped), not the catalog summary. OJB is CC BY 4.0 (attribution required).
- Personal data keys on source ID: same-ID reimport reconnects it.

## Persistence

- Data root: Qt app-data for storage ID `omaword` (`~/.local/share/omaword/`).
- `personal.sqlite`: `PRAGMA user_version` **7**, WAL, `synchronous=FULL`.
  Newer versions refuse to open. Migrations run in one transaction in
  `BibleBackend` (2: cross-chapter endpoints; 3: version scoping + library;
  4: MRU; 5: 80 books / 999 limits; 6: Desk; 7: History). `tags`/`tag_assignments` are
  created `IF NOT EXISTS` independent of the version number.
- Tables: `annotations`, `reading` (per `version_id`); `tags(id,name,name_key)`;
  `tag_assignments` (PK version + both endpoints + tag); `desk` (global ordered
  list, each item version-scoped); `history` (global, newest first by `seq`,
  unique per version + endpoints, start/end 0 = a whole chapter, scroll
  position, visit time; capped at 500) and `recent_queries` (last 5 Bible
  searches); `preferences` (global, incl. `historyEnabled`); `installed`, `library`.
- History: each window keeps an in-memory jump stack in QML (`[`/`]` browser
  semantics, `-` swaps with the previous location; never saved). Jumps
  (palette, search, panel opens, `g`/`G`/counts, version switches) push it; verse
  and chapter steps do not. A location enters the shared `history` list only
  after the window stays on that chapter for `historyDwellMs` (5 s). Disabling
  History clears both tables and stops recording; it is excluded from backups.
- Removal keeps annotations/tags/Desk by default (hidden until reimport);
  `removeVersion(id, true)` drops them. Global tag definitions always survive.
- Backups: JSON `format: "omaword-backup"`, version **5** (3 = annotations,
  4 = + tags, 5 = + Desk). Scoped to the active source; restore requires matching
  `versionId`/edition, exact keys, endpoint validation, 32 MiB bound. Tag names
  reuse existing global identities; colliding IDs are remapped; existing global
  definitions are never renamed/deleted. Preferences and text are excluded.
- Markdown export: `omaword-notes.md`, includes tag-only ranges, escapes tag names.
- Sound/reduce-motion settings live in a separate `QSettings` file, global to all windows.
- Sound: each cue plays on its own PipeWire stream (`SoundEffects::Output`, a
  `pw_thread_loop`), so it reaches whatever output is the default at that
  moment; a drained stream goes inactive, so an idle app is never woken. A
  missing or restarted PipeWire is retried after 5 s, never given up. `error`
  and `boundary` sound once per 500 ms burst (a held key), other cues per
  50 ms. The first cue ever heard (`sound/introduced`) shows how to turn them
  off. Offscreen sessions and `OMAWORD_SOUND=0` never open PipeWire.
- Peer sync: readers in one process share committed annotations, tags, Desk, History,
  prefs, and library; each keeps its own version, passage, selection, scroll.
  Commits publish to peers without navigating them. Imports use a shared busy
  guard; replace/remove/restore flush affected note drafts, and a failed flush
  vetoes. Note saves compare-and-set against the observed baseline; conflicts
  offer **Save my draft** / **Use saved text**.
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

### Range editing (`RangeEditor` + `changeAnnotationRange`)
- Moves selected components (bookmark/highlight/note/individual tags) between
  two exact ranges in the same active version. Unselected components and
  overlapping ranges stay.
- Collisions: flags union, tags dedupe; differing nonempty notes need explicit
  confirmation → `destination + "\n\n" + source`; equal notes dedupe.
- Snapshot token = SHA-256 of endpoints, source ID, every component (incl. tag
  names) and the text generation. Save flushes drafts, re-validates source and
  preview destination token in-transaction; any drift rejects. Same-range save is
  a true no-op (no flush/write/publish).
- Atomic: rows + assignments commit together or not at all.
- Cancel restores locally via `restoreSelection` (no peer flush, no version
  switch); failed restore keeps the session for Esc retry; window close vetoed.
  Source removal ends the session automatically.

### Search
- Active version only; per-window state. Case-insensitive, whitespace-collapsed,
  partial words; punctuation and accents significant. Headings, numbers, notes,
  tags, decorations never searched.
- Bible search: unquoted terms AND within a verse; quoted phrases may span
  adjacent verses; mixed queries anchor on the minimal phrase range. Never
  crosses chapters. Max 4096 units; unbalanced quotes error.
- Chapter find (`/`) is a regular expression (`ScriptureSearch::findPattern`,
  `QRegularExpression` with Unicode properties) over each verse's source text:
  matches never cross verses (the old literal find could), empty matches are
  skipped, no whitespace collapse or full case folding. Neovim smart case:
  insensitive unless an unescaped capital appears; `\c`/`\C` force it (`\c`
  wins). A bad pattern returns `error: "invalid pattern"` and no matches; enter
  then sounds the error and the prompt stays. `findInChapter` stays the literal
  / Bible-syntax engine (reapplying a Bible search's matches). `n`/`shift N`
  never wrap.
- `ScriptureSearch` keeps grapheme-safe maps to original UTF-16 offsets; one
  shared corpus cache (one generation, ≤ 64 MiB); stale requests dropped by token.
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
  touched — `ctrl shift C`/`shift Y` plain text, never with ellipses. Joins
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

### Keys
- Panel- and mode-local keys are catalog rows with a non-global `scope`
  (`characters`, `tags`, `rangeEditor`, `importGuide`, `versions`, `list`, …):
  listed in Keybindings and the README, bound by their owner, never globally.
- `larger` lists `ctrl =`; `ctrl +` is an unlisted alias (`aliases`). Reading
  settings → Layout (`readingMode`, still the id) draws `paintLayoutGlyph`, the
  footer layout button's icons.
- With a drawer side panel open (Annotations, Desk, History, Study), the keys
  of `panelLeavingActions` (palettes, dialogs, reading toggles) move the
  keyboard to the reader, close the panel, then act (`runKey`) — so the
  drawer's exit cannot hand focus back over what opened. Notes is not modal:
  they act over it. Dispatch from Keybindings or Commands never closes panels. Catalog shortcuts
  recheck `shortcutEnabled` on activation (a popup's focus change can leave the
  binding stale).
- Notes in Write mode keeps the keyboard in the editor (`notesWriting` /
  `keepNoteEditorFocus`): clicks on the panel's chrome and closing menus or
  dialogs hand it back. `tab` types a tab (accepts a `[[` suggestion instead),
  `ctrl ↓` moves to Linked from, `esc` returns.

### LTR forcing
- All Scripture layouts are LTR even when a paragraph begins in Hebrew:
  `SearchGeometry::forceLeftToRight` pins every block; plain `Text` gets a
  display-only LRM per line (`leftToRightText` in Main). Never copied.

## Reader rendering

- Verse layout: one row per verse; annotation rails packed by
  `packAnnotationLanes` into nearest free gutter lanes by source-row ordinal
  (not verse labels), tie priority bookmark/tag/note, lane kept for an interval's life.
- Paragraph/Block: one `BlockDocument`. Annotation endpoints are reserved
  transparent inline cells (+ word joiners, no font merging) where QML paints
  brackets and superscript glyphs. Outer starts first, inner ends first;
  continuation chevron for ranges entering the chapter. Bracket height follows
  endpoint font ink, not line height.
- `AnnotationGlyph`: `filled` for reader marks, outline for controls; tag icon
  is mirrored to point left.
- Paint-only overlays (never alter text formats, layout, selection, or copy):
  - Hand-drawn marks (`HandDrawn.js`): deterministic seeded paths (seed from
    kind/version/reference/segment, no `Math.random`) so repaint/scroll is stable.
  - Annotation tints: every bookmark/highlight/note/tag range covering a verse
    adds one hand-drawn accent layer (`HandDrawn.tintCount`/`tintRuns`, 4.5%
    each, capped at 3), joined across verses that keep the layer; highlights,
    then search matches, paint over them.
  - Highlights are a highlighter pen (`HandDrawn.highlighterStroke`): one
    stroke per text line in the theme's yellow (`desktop.color3`, Omarchy
    `color3`/`yellow`, else the accent), from a little above the x-height to a
    little below the baseline (`HandDrawn.highlightBand`, from the reader
    font's metrics; text sits at the top of its line box) — a feathered pass,
    a chisel-tipped core that tapers at its ends, and a shorter second pass
    darker where they overlap; seeded per range and line. No accent bar. The
    Highlights tab draws the same stroke under each row's reference.
  - Focus-mode dimming: follows the cursor instantly; only entering/leaving fades.
  - Jump pulse and copy flash rects. Reduce motion disables pulses/flashes/fades.
  - Programmatic scrolls to a verse (`scrollToVerse`, `zz`/`zt`/`zb`, search,
    jumps) slide via one `readerSlide` animation (75 ms, ease-out) that
    retargets from the current position; `readerTargetY()` is the logical
    position (saved positions use it). User drags stop it. Focus mode keeps the
    cursor verse 40% down. Instant when animations are disabled.
  - Popup motion (interfaces.dev: exits subtler than entrances, from their
    trigger; frequent popups open instantly): menus and modals (palette,
    dialogs, tag picker, range dialogs) open and close at once, so a closed
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
  the cells. Clicking one selects the verse and opens Study → Footnotes there.
- Words of Jesus: `<span class="wj">` gets an inline color (desktop `color1`,
  i.e. Omarchy `color1`/`red`, else accent) only while the setting is on;
  layout is unchanged.
- Focus mode hides decoration cells (incl. footnote markers), rails, and annotation tints.
- Focus mode's layout (no headings, verse numbers, decoration cells, margins,
  scrollbar or footer room) follows `focusLayout`, which `applyFocusLayout`
  flips in one step that settles the new layout before its first frame: the
  single rebuild runs at once (`BlockReader.settle`; `BlockDocument::build`
  is one edit block, so the TextEdit lays out once), margins are re-placed,
  and the reading position (the cursor verse while on screen, else the top
  verse) keeps its place on screen through the reflow, even past the content's
  ends for a moment; entering then slides it to the typewriter line, and a
  reader at the chapter top slides back to it. Paint-only effects (dimming,
  the quiet footer) follow `bible.focusMode`. The column and the chapter
  buttons (re-centred without the footer's room) glide (`marginSlide`,
  `chapterNavGlide`) from wherever they are, so toggles retarget.
- Focus mode keeps the footer quiet: only the focus button shows; the others keep
  their places hidden and disabled. Hovering the footer reveals them at once (their
  clicks wait 300 ms, so a click aimed at the focus button is never caught); hiding
  waits 200 ms so moving between the zone and a button never flickers; keyboard
  focus entering the footer reveals them too. The footer's left group is focus,
  mode, Keybindings.
- Chapter buttons (`chapterPreviousButton`/`chapterNextButton`): chevrons at the
  reader's outer edges, centred in the viewport, mouse only (h/l are the keys),
  disabled at the Bible's first/last chapter; steps are not History jumps. The
  margins reserve their width (`chapterNavReserve` in `Margins.space` insets) and
  they hide when the column would reach them; in focus mode they appear only
  under the pointer (300 ms click guard).
- Range editing marks the moving endpoint verse (`rangeEndpointGeometry`) with
  an accent selection handle at the edge of its text (after the number in
  flowing layouts): a caret spanning the line and a round knob in the line gap,
  above the start's first line or below the end's last line; the knob is sized
  to the gap (`readerLineGap`) and tucked a quarter into the line.
- Keyboard focus hand-off: a `QuietButton`/`FloatingButton` pressed by key that
  hides itself passes the keyboard to its `focusTarget` (else `restorePanelFocus`).
  Modal outside clicks: `RangeEditor`'s dialogs close on press outside and route
  it to esc's action (`outside`); the tag picker's confirmation watches
  `Overlay.onPressed` with a passive `PointHandler` marking presses inside.
- Margins (off by default; hidden in focus mode and range editing): one
  `Margins.space()` split. Margin notes win: they may shift the reader column
  left (only in chapters that have notes) and sit right after their connector;
  the outline takes what is left and never moves the column. `updateMargins()`
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
  noteMarks)`: one reflow per toggle rather than blank cells in the text). The Notes drawer is a popup, so it
  covers them. The outline's current section is the cursor's when it is on
  screen, else the one at a reading line ¼ down (the chapter title above the
  headings is current before the first one, and jumps to the chapter top);
  `{`/`}` step headings from the same verse (History jumps), `{` ending at the
  chapter top (`Margins.headingStep`'s `top`), even without headings. Toggling notes (`applyMarginNotes`, driven
  by an actual `bible.marginNotes` change) fades them in while the column slides left,
  or fades them out before it slides back (`marginSlideOffset`, 100 ms
  ease-out). Entering focus mode, shown margins keep their split
  (`leavingSpace`, `marginsLeaving`) and fade out (`marginExit`, 85 ms
  ease-in) while moving with the gliding column (`marginDrift`); leaving it,
  they are placed before they show and fade in where they settle
  (`marginEnter`, 100 ms). Opacity is the notes' toggle level times that focus
  level. A connector is clipped where the column's text, still gliding into
  place beside waiting margins, would cover it (`marginCover`). Resizes,
  chapter changes, range editing, and Disable animations end margin motion at
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

## Verification

See [tests/README.md](tests/README.md) for `bin/test` selectors, filters,
environment variables, and fixtures.
