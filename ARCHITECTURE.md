# OmaWord architecture

Durable, non-obvious knowledge for developers. Code wins over this file; the
tests (see [tests/README.md](tests/README.md)) pin the contracts below.

Stack: C++17, Qt 6 Quick/QML (SQL, Concurrent, DBus, Network, Multimedia),
libarchive for ZIPs, qmake (`omaword.pro`). `resources.qrc` embeds QML and the
seven-entry `resources/catalog.json`. `branding.env` feeds generated headers
(`src/brand.h.in`) and desktop files.

## Components

| File(s) | Responsibility |
| --- | --- |
| `src/usfmimporter.*` | `UsfmImporter`: inspect metadata, parse USFM, stage/validate/publish text DBs |
| `src/biblebooks.*` | Stable 80-book code/name/alias registry (ID = index + 1) |
| `src/versioncatalog.*` | Async HTTPS ZIP download to cache; progress, cancel, errors |
| `src/biblebackend.*` | Per-reader `bible`: active text, references, annotations, tags, Desk, prefs, library ops, backup/export |
| `src/scripturesearch.*` | Normalized, cancellable Bible-wide search on a worker pool |
| `src/searchdocument.*` | Offset mapping into rich text; LTR forcing helpers |
| `src/blockdocument.*`, `qml/BlockReader.qml` | Flowing Paragraph/Block layout, verse↔document mapping, overlays |
| `src/windowmanager.*` | One backend + catalog + QML engine + window per reader; request routing |
| `src/singleinstance.*` | One process per data dir: `omaword.lock` + bounded local-socket IPC |
| `src/desktop.*` | Fonts/scaling, live Omarchy theme, async Hyprland corner radius |
| `src/soundeffects.*`, `src/keyhold.*` | Synthesized UI sounds + reduce-motion setting; key-repeat handling |
| `qml/Main.qml` | Reader, palettes, Notes, Annotations, Desk, settings, catalog, About |
| `qml/TagsPanel.qml` | Tag picker/management (modal), Annotations Tags summary, Related drawer |
| `qml/RangeEditor.qml` | Annotation range editing session |
| `qml/AnnotationGlyph.qml`, `qml/HandDrawn.js` | Shared bookmark/tag/note glyphs; seeded hand-drawn paths |
| `qml/AppContextMenu.qml`, `qml/AppToolTip.qml` | Shared context menu and themed tooltip |
| `qml/ChapterFind.qml`, `qml/SearchHighlights.qml` | `/` chapter find; match overlay painting |

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
- Text DB schema is **3** (independent of personal schema): `metadata` (incl.
  digests), `books(...,ordinal)`, `book_aliases`, `source_chapters`,
  `source_keys(book,chapter,number,retained)`, `verses(...,html,heading,poetry,paragraphStart)`.
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
  `\w` keeps the word, drops attributes. Footnotes, xrefs, alt numbers, intros
  excluded. Unknown markers / bad UTF-8 fail. Archive limits: 2,048 entries,
  32 MiB/entry, 256 MiB total, nesting 32, 65,536 units per verse/heading;
  unsafe/duplicate paths, links, encryption rejected. `copr.htm` entities are
  decoded without an XML/HTML engine.

## Versions and attribution

- Seven catalog entries (`engnet`, `eng-kjv2006`, `eng-kjv` = **KJV+**, `engojb`,
  `engylt`, `eng-asv`, `engbsb`) from `https://ebible.org/Scriptures/<id>_usfm.zip`.
- The reader and About show the imported `copr.htm` title/rights (site footer
  stripped), not the catalog summary. OJB is CC BY 4.0 (attribution required).
- Personal data keys on source ID: same-ID reimport reconnects it.

## Persistence

- Data root: Qt app-data for storage ID `omaword` (`~/.local/share/omaword/`).
- `personal.sqlite`: `PRAGMA user_version` **6**, WAL, `synchronous=FULL`.
  Newer versions refuse to open. Migrations run in one transaction in
  `BibleBackend` (2: cross-chapter endpoints; 3: version scoping + library;
  4: MRU; 5: 80 books / 999 limits; 6: Desk). `tags`/`tag_assignments` are
  created `IF NOT EXISTS` independent of the version number.
- Tables: `annotations`, `reading` (per `version_id`); `tags(id,name,name_key)`;
  `tag_assignments` (PK version + both endpoints + tag); `desk` (global ordered
  list, each item version-scoped); `preferences` (global); `installed`, `library`.
- Removal keeps annotations/tags/Desk by default (hidden until reimport);
  `removeVersion(id, true)` drops them. Global tag definitions always survive.
- Backups: JSON `format: "omaword-backup"`, version **5** (3 = annotations,
  4 = + tags, 5 = + Desk). Scoped to the active source; restore requires matching
  `versionId`/edition, exact keys, endpoint validation, 32 MiB bound. Tag names
  reuse existing global identities; colliding IDs are remapped; existing global
  definitions are never renamed/deleted. Preferences and text are excluded.
- Markdown export: `omaword-notes.md`, includes tag-only ranges, escapes tag names.
- Sound/reduce-motion settings live in a separate `QSettings` file, global to all windows.
- Peer sync: readers in one process share committed annotations, tags, Desk,
  prefs, and library; each keeps its own version, passage, selection, scroll.
  Commits publish to peers without navigating them. Imports use a shared busy
  guard; replace/remove/restore flush affected note drafts, and a failed flush
  vetoes. Note saves compare-and-set against the observed baseline; conflicts
  offer **Save my draft** / **Use saved text**.
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
- Chapter find (`/`) is literal; `n`/`shift N` never wrap.
- `ScriptureSearch` keeps grapheme-safe maps to original UTF-16 offsets; one
  shared corpus cache (one generation, ≤ 64 MiB); stale requests dropped by token.
  `SearchDocument`/`BlockDocument` map those offsets into each layout, skipping
  decoration cells and verse-number labels.

### Copy
- Preserves stored internal whitespace. Verse joins follow source paragraph
  starts, poetry lines, and chapter breaks regardless of current layout.
- `selectionText()` strips only generated decoration cells; original joiners and
  object characters are kept.

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
  - Highlights, cursor-range context shading, search matches.
  - Focus-mode dimming: follows the cursor instantly; only entering/leaving fades.
  - Jump pulse and copy flash rects. Reduce motion disables pulses/flashes/fades.
- Focus mode hides decoration cells, rails, and context shading.
- Desktop corners come from async `hyprctl` polling; failure resets to 0.
  Highlight segments stay square.

## Verification

See [tests/README.md](tests/README.md) for `bin/test` selectors, filters,
environment variables, and fixtures.
