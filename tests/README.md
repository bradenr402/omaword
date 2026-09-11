# OmaBible tests

From the repository root, build and run backend, integrated UI, and dedicated
block-annotation tests offscreen in `build/tests/`, `build/ui-tests/`, and
`build/blockannotations/`:

```sh
bin/test
# Optional integration cases using your own downloaded edition:
OMABIBLE_TEST_EPUB="$HOME/Downloads/NETBIBLE21.epub" bin/test
```

`OMABIBLE_TEST_EPUB` is optional. Without it, real-edition integration cases are
skipped. Tests isolate application data in a temporary directory and read the
user-supplied EPUB in place. No Bible content is included in the test sources.
Block-annotation tests cover logical marker geometry, overlapping-range choosers,
note navigation, layout changes, and focus-mode suppression using original fixtures.

## Backend behavior

- Data lives under Qt's `QStandardPaths::AppDataLocation`: `bible.sqlite` is the
  replaceable, validated edition; `personal.sqlite` holds annotations and reading
  state and reading preferences with synchronous SQLite commits. Existing valid
  data opens without import.
- Preferences default to `showVerseNumbers=true`, `showHeadings=true`,
  `versePicker=true`, `lineHeight=1.5`, `focusMode=false`, and `blockMode=false`.
  Coverage includes restart persistence, additive schema-2 initialization, invalid stored values,
  line-height rounding/clamping to `1.0`–`3.0` in `0.1` steps, non-finite input
  rejection, and write/commit failures preserving published and persisted state.
- References support full names, standard aliases, ordinal/roman numbered books,
  compact forms, chapter-only references, cross-chapter/book ranges, and single-chapter
  book shorthand. A range may span omitted verse numbers, but its endpoints must
  exist. Pure reference previews use the same parser as navigation.
- Flags cover verses: a selection is flagged only when **all** its actual verses
  are covered. Toggling a partially covered selection flags all of it. Toggling
  a fully covered selection removes that flag within the selection, splitting
  existing ranges as needed, while preserving the other flag and all notes.
- Notes belong to an **exact range**. Selecting an overlapping range does not
  expose or overwrite that note. `hasNote` on each verse includes overlapping
  notes. Empty notes remove their entry unless a flag remains.
- Exact Saved deletion removes one annotation's bookmark, highlight, and note by
  canonical endpoints, preserving overlapping and other entries. Coverage includes
  aliases, cross-chapter/book ranges, missing exact entries, restart persistence,
  and rollback on DELETE, read-back, and deferred commit failures with no published
  model/selection changes.
- No-selection `selectionReference` is the chapter reference; `copySelection()`
  copies that chapter as a quotation, `copySelection(true)` as plain text, preserving
  selection. Annotation commands require an explicit verse selection.
- The verse picker returns actual chapter verse numbers, skipping NET omissions.
  `copyVerse(number, plainText=false)` copies the full canonical selection for an
  inside hit, otherwise only that verse, then persists the clicked verse as the
  single selection. The UI uses `copySelection` for inside hits to preserve its range.
  Quotation copy uses Unicode superscript verse numbers and the exact wrapper
  `“¹⁶ … ¹⁷ …”\n\n—John 3:16–17`, with no version suffix. Plain copy omits numbers,
  quotes, and reference. Both simplify whitespace and join verses with spaces,
  independent of display preferences. Coverage includes multi-digit numbers,
  poetry, reversed/cross-chapter/book ranges, omissions, and read/state-save failures
  preserving clipboard and selection. Free text copying preserves the supplied text.
- Five- and ten-verse movement stops at chapter edges; a subsequent move from the
  edge crosses by one verse. Coverage includes direction reversal and omissions.
- Backups contain personal data only, with format `omabible-backup`, version `2`,
  edition, UTC creation time, reading state, and annotation records. Restore
  replaces annotations and reading state in one transaction after strict
  field/type/range validation. Restore requires the imported edition to validate
  verse endpoints.
  Version-1 backups remain importable; schema-v1 personal databases migrate
  transactionally. Directional anchor/cursor endpoints survive restart and restore.
  Reading preferences are excluded from annotation backups and retained on restore.
- Markdown export writes `omabible-notes.md` atomically in the chosen existing
  directory, containing references, flags, and unchanged Markdown notes.
- Import derives references from embedded verse markers, including cross-file
  chapter endings. It validates 66 book totals, all 1,189 chapters, 31,087 verses,
  and the NET's 16 intentionally omitted verse numbers (including the empty
  publisher marker for Acts 24:7). Translator superscripts
  are removed; editorial headings/psalm superscriptions/acrostic and speaker
  labels are kept in `heading`, separate from verse text. Poetry and intra-verse
  breaks are retained; HTML is generated from escaped text and safe inline tags.

## Integrated UI coverage

UI tests use original synthetic text and temporary application data. Coverage includes:

- Book → chapter → optional verse browsing, direct reference previews, keyboard
  back navigation, omitted verse numbers, and browsing chevrons. Space resumes query,
  browsing location, and selected result, including the snapshot before Esc clears
  nonempty text; a second Esc closes. Command clearing retains `>`. Ctrl k and
  Ctrl Space establish fresh passage/command snapshots; Ctrl Shift P is unbound.
- Normal/visual verse selection across chapters, counted verse jumps, bounded
  five-/ten-verse moves, and reader focus and keyboard-context isolation.
- Free text selection across verses with Ctrl c and drag autoscroll in row and block
  layouts, including forward/reverse Unicode selection and hidden verse numbers.
  Quotation copy via y, Ctrl c without free text but with a verse selection, right-click,
  command entry, and the Notes Copy button; plain copy via Y (Shift Y), Ctrl Shift C,
  middle-click, and command entry. Inside mouse hits preserve the full canonical range
  and cursor; outside hits focus only the copied verse. Failed copies preserve the
  clipboard/selection and do not report success.
- Flowing block layout (`b`, default off): inline verse starts, optional superscript
  numbers and section paragraphs, retained poetry breaks, verse hit mapping/navigation,
  autoscroll, notes/annotations, and continuous highlight geometry including wrapped
  lines, headings, bidi runs, and gaps between independent adjacent highlights.
- Runtime verse-number, heading, verse-picker (`p`), block (`b`), focus-mode, and
  line-height preferences, including persistence across UI reload. Focus temporarily
  hides numbers, headings, chapter/edition titles, and controls in both layouts while
  preserving number/heading preferences and restoring their visibility on exit.
  Line-height changes clear free selection coherently.
- Reading settings at small/scaled sizes: left labels, shortcut hints before
  right-aligned square switches; toggle keys and Ctrl [ / Ctrl ] work while the dialog
  and slider have focus; `Ctrl \` resets line height to 1.5 in settings and the reader.
  Dynamic command hints show current state. Reset line height is an executable entry.
- Searchable Keybindings includes only bound actions plus disabled context/gesture
  guidance: p finds Verse picker, Reset line height is present, and unbound Reading
  settings is absent. Arrow keys select and Enter executes; Home/End selects the
  first/last result, PageUp/PageDown one page at a time.
  Shortcut display convention is lowercase without Shift (Ctrl k, Ctrl c, Ctrl n),
  uppercase with Shift (Ctrl Shift C, Y meaning Shift Y); Qt key enums/sequences in
  test code use their required syntax.
- Animated notes overlays at wide, narrow, and scaled layouts without reader
  reflow; n to write, Ctrl n to toggle; Esc/outside dismissal, exact-range note isolation,
  autosave, and save-failure handling.
- Current-range footer, bookmark markers, Saved filtering and keyboard navigation,
  stacked bookmark/note glyphs and highlight accent stripe. Delete/Backspace opens
  exact-entry deletion confirmation; Cancel initially has focus, Esc cancels back to
  Saved, confirmation removes all flags/note for that entry while overlapping entries
  survive. Coverage includes editor synchronization, failure/retry, index clamping,
  empty-list behavior, and keyboard isolation during confirmation.
- Close-X removal and Esc dismissal in app-owned panels/dialogs; shared custom overlay
  scrollbars across app-owned scrollable surfaces, with full-width content, a broad
  hit area, hover widening, and activity fade. Reader scrollbar is hidden in focus mode.
  Restore confirmation, saved scroll positions, and narrow/system-scaled layouts.

## Import limits and chapter metadata provenance

XML is preflighted token-by-token, including discarded subtrees: maximum depth
64, formatting (`b`/`i`) depth 16, 100,000 elements per document, and entity
expansion limit 1,024. Expanded XML text and generated output each have cumulative
budgets of 2 Mi UTF-16 code units/document and 64 Mi/import. Generated verse
output has a 64 Ki budget (headings included); escaping is conservatively charged
before allocating output. Oversized headings are rejected before concatenation.
All failures precede installation of the staged database.

The numeric-only `chapterMaximum()` table was derived from the user's trusted
`NETBIBLE21.epub` by collecting embedded `chapter:verse` markers across archive
entries, rather than assigning markers according to XHTML titles. Every chapter
must have its exact maximum and exact count (maximum minus known omissions).
Metadata was independently checked on 2026-09-10 against the explicitly numbered
verses in [scrollmapper's KJV dataset](https://github.com/scrollmapper/bible_databases/blob/master/formats/json/KJV.json).
All 1,189 maxima match except these three NET versification differences, each
independently confirmed against BibleGateway's NET chapter markers:

- [2 Corinthians 13](https://www.biblegateway.com/passage/?search=2+Corinthians+13&version=NET): NET 13, KJV 14.
- [3 John 1](https://www.biblegateway.com/passage/?search=3+John+1&version=NET): NET 15, KJV 14.
- [Revelation 12](https://www.biblegateway.com/passage/?search=Revelation+12&version=NET): NET 18, KJV 17.

The older labs.bible.org API uses different numbering in the latter two chapters
and was therefore not used as the NET 2.1 authority. No downloaded Scripture text
is embedded in the sources. Tests additionally check each book's independent
total, all chapter counts, and a compensating Genesis 1:31 → Genesis 2:26
corruption that preserves the book total.

Navigation reads and validates the entire destination chapter before saving or
publishing state. Restore stages that chapter plus an annotation read-back within
its transaction, then publishes the already-staged models after commit without
additional SQL reads. Fault tests exercise chapter-body and annotation read-back
errors, verifying no signals/state changes and persistence after reopening.
Markdown and JSON exports share destination protection, including symlinks to
live databases and existing or dangling SQLite sidecars.
