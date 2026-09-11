# OmaWord tests

The single-instance suite (`tests/singleinstance.pro`, included in `bin/test`)
checks process handoff, argument forwarding, startup races, stale socket/lock
recovery, and bounded IPC validation. Set `OMAWORD_TEST_APP` to the built executable
to include its actual-application launch checks.

From the repository root, `bin/test` runs migration and single-instance suites,
then backend, integrated UI, block-annotation, and importer-version tests in
`build/tests/`, `build/ui-tests/`,
`build/blockannotations/`, and `build/importer-versions/`. GUI suites run offscreen:

```sh
bin/test
# Optional integration cases using your own downloaded editions:
OMAWORD_TEST_EPUB="$HOME/Downloads/NETBIBLE21.epub" bin/test
OMAWORD_TEST_EPUB="$HOME/Downloads/NETBIBLE21.epub" \
  OMAWORD_TEST_KJV="$HOME/Downloads/eng-kjv2006.epub" bin/test
```

`OMAWORD_TEST_EPUB` enables real NET integration. `OMAWORD_TEST_KJV` enables
structured Haiola/eBible KJV 2006 backend/UI cases; backend multi-version cases
require both files, while UI KJV cases can use the synthetic NET fixture. Missing
optional inputs skip their dependent cases. `bin/test` normalizes NET and KJV
paths before changing build directories and exports `OMAWORD_TEST_KJV_EPUB`
from `OMAWORD_TEST_KJV` when unset or empty. Explicit importer/NKJV fixture
overrides should use absolute paths; those variables are not normalized.
Tests isolate application data in temporary directories
and do not modify the supplied EPUBs. Source-independence UI cases import and
delete temporary copies. No Bible content is included in the test sources.
Block-annotation tests cover logical marker geometry, overlapping-range choosers,
note navigation, layout changes, and focus-mode suppression using original fixtures.

Use current test output for pass/fail/skip totals; validation counts are not fixed
in this document. `bin/test -functions` lists the available test functions.

## Importer-version suite

`tests/importer_versions.pro` builds `tst_importer_versions` from
`tests/tst_importer_versions.cpp` and the importer. It uses Qt Core, SQL, XML,
Test, and libarchive (no GUI); the independent KJV DOM whitespace oracle requires
Qt 6.5+ in a Qt 6 build. `bin/test` runs this project automatically after the
other suites. To build/run it independently from the repository root:

```sh
mkdir -p build/importer-versions
(
  cd build/importer-versions
  qmake6 ../../tests/importer_versions.pro
  make -j"${JOBS:-$(getconf _NPROCESSORS_ONLN)}"
)
OMAWORD_TEST_EPUB="$HOME/Downloads/NETBIBLE21.epub" \
  OMAWORD_TEST_KJV_EPUB="$HOME/Downloads/eng-kjv2006.epub" \
  ./build/importer-versions/tst_importer_versions
./build/importer-versions/tst_importer_versions -functions
```

The KJV variable here is **`OMAWORD_TEST_KJV_EPUB`**, distinct from
`OMAWORD_TEST_KJV` used by backend/UI tests; `bin/test` supplies the fallback
described above. Optional rejection-only fixtures must be supplied explicitly and
use `OMAWORD_TEST_NKJV_ORIGINAL_EPUB` and `OMAWORD_TEST_NKJV_EPUB` (the exact
replacement). For example, with each variable set to its absolute local path:

```sh
OMAWORD_TEST_NKJV_ORIGINAL_EPUB="/absolute/path/to/original-nkjv.epub" \
  OMAWORD_TEST_NKJV_EPUB="/absolute/path/to/replacement-nkjv.epub" \
  ./build/importer-versions/tst_importer_versions unsupportedOcr
```

Both assessed NKJV files are unsupported OCR layouts. `unsupportedOcr` verifies
rejection, no new database, unchanged existing destination bytes, and no leaked
SQL connection; it does not import NKJV Scripture. NIV is not imported or used as
an import fixture. `boundedMetadataAndUnsafeArchive` uses generated non-Scripture
fixtures and runs without any local EPUB variables. Other cases skip if their
fixture variable is absent.

Importer coverage includes:

- `editions`: inspect without DB writes/connections; import/reimport, progress,
  stable identity, metadata, 66 books/1,189 chapters, edition-specific counts and
  omissions; missing verses/unsupported IDs fail validation, and legacy NET
  edition-only identity metadata remains valid.
  New imports assert schema 2, paragraph_layout=1, and source paragraphStart
  metadata. KJV Genesis 1 groups are 1–2 | 3–5 | 6–8 | 9–13 | 14–19 | 20–23 |
  24–25 | 26–28 | 29–31, independently checked against source div.p groups;
  starts 3 and 20 lack ¶, so pilcrows alone are insufficient. Layout pilcrows are
  stripped from imported text and HTML. Synthetic NET cases verify source p
  boundaries alongside retained within-verse breaks.
- `kjvAllWordsAndFormatting`: independent DOM comparison of all 31,102 verses,
  including inline tails, supplied-word italics, Psalm headings, and poetry.
- `kjvNotesAndHtml`: explicit note subtrees are stripped without losing adjacent
  words; safe inline HTML and `.html` spine paths work; duplicate markers leave the
  installed database unchanged and wrong marker prefixes fail inspection.
- `boundedMetadataAndUnsafeArchive`: OCR metadata rejection before inflating an
  oversized body, and unsafe archive-path rejection.

## Branding and compatibility

The public app is **OmaWord**, the primary command/executable is `omaword` /
`build/omaword`, and `packaging/PKGBUILD` builds package `omaword`.
`branding.env` supplies the name, ID, version, bible/scripture launcher keywords,
and active storage identity to qmake and shell scripts. qmake generates the
`Brand` header (`brand.h`) via `branding.pri` and the `omaword.desktop` launcher
from templates in each application build. Desktop/QML properties expose the
generated public name and ID. App icons are open books; the Notes panel control
uses a pencil.

The active project path is `~/Projects/omaword/omaword.pro`, storage/application
name is `omaword`, and data defaults to `~/.local/share/omaword` (or
`$XDG_DATA_HOME/omaword`). QML uses `OmaWord.Reader`, and environment variables use
`OMAWORD_`. Build/install provide only the new command, with no `omabible` aliases.
The package conflicts with/replaces historical
`omabible`. Local installation archives and removes the old binary entry plus
byte-identical historical launcher/icons; custom launchers/icons are preserved.
`bash tests/test-install.sh` exercises fresh/repeated installation, upgrade,
backup preservation, custom collisions, symlinks, and alternate package branding.
Historical names are explicit exceptions for migration, old backup signatures,
package upgrades, archived installer assets, and history. Checks should verify
active identities and these exceptions rather than demand literal elimination
from all code/history.

The migration suite (`tests/migration.pro`, first in `bin/test`) uses temporary
XDG-style sibling directories exclusively. It covers merging into an existing
install-backup directory, notes/reading/global preferences/library preservation,
live old-lock refusal, stale old socket/lock recovery, a real uncheckpointed WAL,
both-store conflicts, unsafe files/sidecars/registry paths, and interrupted
copy/publication/archival recovery. Original bundles are retained under
`omaword/.storage-migration/originals/`; the durable journal permits restart.
The primary holds both identities' locks before backend creation and keeps the
historical lock for the backend lifetime. All user data remains isolated in tests.

## Backend behavior

- Data lives under Qt's `QStandardPaths::AppDataLocation`. Root `personal.sqlite`
  uses schema 3, with version-specific `annotations`/`reading`, global
  `preferences`, an `installed` table (stable IDs, root-relative paths and edition
  metadata and `last_used`), and `library` (active ID and MRU `use_counter`).
  New text is installed as
  `bible-<UUID>.sqlite` generations in the root; legacy NET `bible.sqlite` is
  validated and registered in place during transactional v1/v2 migration. Legacy
  annotations and reading state migrate to `net-2.1`. Existing valid data opens
  without import.
- `corruptLegacyRecovery` checks that damaged legacy text does not block personal
  migration: text remains unregistered, NET annotations are retained, and supported
  NET reimport restores reading and reconnects annotations.
- Text removal/replacement accepts only owned root generation filenames (or NET's
  legacy filename), with matching edition identity and no registry ambiguity.
  Coverage rejects personal-store/sidecar targets, unrelated filenames, wrong
  editions, symlinks, hardlinks, and aliased text/sidecars; cleanup rechecks ownership.
- Preferences default to `showVerseNumbers=true`, `showHeadings=true`,
  `versePicker=true`, `lineHeight=1.5`, `focusMode=false`, `readingMode="paragraph"`,
  and `fontSize=18`. Reading modes are paragraph/verse/block; migration maps old
  blockMode=true to Block and false to Paragraph when readingMode is absent.
  Coverage includes restart persistence, additive preference-table initialization
  alongside schema-3 migration, invalid stored values,
  line-height rounding/clamping to `1.0`–`3.0` in `0.1` steps, non-finite input
  rejection, font-size integer validation/clamping to 8–40, and write/commit
  failures preserving published and persisted state.
- `legacyParagraphsAndMru` (both EPUB fixtures required) checks read-only legacy
  KJV recovery from retained layout pilcrows, including safe HTML wrappers,
  clean display/copy text, preservation of literal non-layout ¶, and edition-wide
  paragraph availability. Legacy NET lacks recoverable source boundaries and
  falls back to verse rows until reimport while preserving Paragraph preference.
  Recovery cannot reconstruct unmarked source paragraphs in legacy KJV; reimport
  supplies complete source metadata. The case also checks active-first persisted
  MRU ordering, no rank change on startup/already-active selection, transactional
  rank rollback, fresh ranks after removal/reimport, and font-size persistence.
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
- The verse picker returns actual active-edition chapter verse numbers, skipping
  its omissions (KJV includes verses intentionally omitted in NET).
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
- Backups contain active-version personal data only, with format
  `omaword-backup`, version `3`, `versionId`, edition, UTC creation time, reading
  state, and annotation records. Restore replaces only that version's annotations
  and reading state in one transaction after strict field/type/range validation.
  The matching version must be installed and active; both stable ID and edition
  name must match. Both new and historical `omabible-backup` signatures are accepted
  for versions 1–3. Version-1/version-2 backups remain importable as NET-scoped
  data, requiring active NET 2.1. Directional anchor/cursor endpoints survive
  restart and restore. Other versions' data and global preferences are preserved;
  preferences and Bible text are excluded from annotation backups.
- Markdown export writes `omaword-notes.md` atomically in the chosen existing
  directory, containing the active edition name/ID, references, flags, and unchanged
  Markdown notes for that version only.
- `multiVersionLibrary` covers schema-2 NET migration/rollback, dynamic version
  properties and metadata, KJV import/activation and all verse keys, same-reference
  switching, missing-endpoint chapter fallback, version-specific notes/flags,
  scoped v3 and legacy backup acceptance/rejection, restart persistence, export
  destination protection across installed text generations, instance-local keys,
  inactive/active/last-version removal, hidden retained annotations and same-ID
  reimport, explicit annotation deletion, and global preference survival.
- `versionOperationRollback` exercises install, switch, and removal failures in
  SQL writes/read-back/commit, missing targets, malformed target annotations, and
  empty-library transitions. Failed operations preserve models, current selection,
  installed files, annotations, and persisted library state.
- NET import derives references from embedded verse markers, including cross-file
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
  Ctrl Space establish fresh passage/command snapshots; Ctrl Shift P is an
  additional Open commands binding. Action labels include Search for a passage,
  Saved panel, and Notes panel.
- Normal/visual verse selection across chapters, counted verse jumps, bounded
  five-/ten-verse moves, and reader focus and keyboard-context isolation.
- Free text selection across verses with Ctrl c and drag autoscroll in row and block
  layouts, including forward/reverse Unicode selection and hidden verse numbers.
  Quotation copy via y, Ctrl c without free text but with a verse selection, right-click,
  command entry, and the Notes Copy button; plain copy via Y (Shift Y), Ctrl Shift C,
  middle-click, and command entry. Inside mouse hits preserve the full canonical range
  and cursor; outside hits focus only the copied verse. Failed copies preserve the
  clipboard/selection and do not report success.
- Reading mode defaults to Paragraph; `b` cycles Paragraph → Verse → Block →
  Paragraph. Paragraph honors source paragraphStart boundaries, Verse uses separate
  rows, and Block flows continuously with visible headings starting paragraphs.
  Coverage checks metadata-free Paragraph fallback to rows, inline verse starts,
  optional superscript numbers, retained poetry breaks, verse hit mapping/navigation,
  autoscroll, notes/annotations, and continuous highlight geometry including wrapped
  lines, headings, bidi runs, and gaps between independent adjacent highlights.
  Block-annotation cases also cover paragraph boundaries/annotations and headings
  coinciding with paragraph starts without creating empty blocks.
- Runtime verse-number, heading, verse-picker (`p`), reading-mode (`b`), focus-mode,
  font-size, and line-height preferences, including persistence across UI reload.
  Focus temporarily
  hides numbers, headings, the edition label, and controls in all modes while
  retaining the chapter header. It preserves number/heading preferences and
  restores their chosen visibility on exit.
  Line-height changes clear free selection coherently.
- Reading settings via Shift R or its command entry, including small/scaled sizes:
  left labels, shortcut hints before right-aligned square switches, mode choices,
  and font-size/line-height sliders. Unmodified Left/Right do not mutate sliders,
  including after Tab focus traversal. Toggle keys, `b`, Ctrl [ / Ctrl ], and
  Ctrl + / Ctrl − (also Ctrl =) work while the dialog and sliders have focus;
  `Ctrl \` resets line height to 1.5 and Ctrl 0 resets font size to 18.
  Dynamic command hints show current state. Reset line height is an executable entry.
- Searchable Keybindings includes only bound actions plus disabled context/gesture
  guidance: p finds Verse picker, Reset line height and Shift R Reading settings
  and Shift V Switch version are present. Unbound Add version and Remove version
  remain command-palette-only. Keybinding
  hints show shortcut keys without On/Off prefixes; command-palette hints retain
  setting state. Arrow keys select and Enter executes; Home/End selects the
  first/last result, PageUp/PageDown one page at a time.
  Shortcut display convention is lowercase without Shift (Ctrl k, Ctrl c, Ctrl n),
  uppercase with Shift (Ctrl Shift C, Y meaning Shift Y); Qt key enums/sequences in
  test code use their required syntax.
- Animated notes overlays at wide, narrow, and scaled layouts without reader
  reflow; n to write, Ctrl n to toggle; Esc/outside dismissal, exact-range note isolation,
  autosave, and save-failure handling. Toggling Notes produces no Notes on/off toast.
- Text-size feedback for larger/smaller/reset commands, including in focus mode:
  `Text size N` appears only when the value changes, with no new toast at size
  limits or a redundant reset. Default/reset is 18; limits are 8 and 40, with
  steps of 1 before desktop scaling. Size is persisted globally as fontSize and
  stays synchronized with the Reading settings slider/value across reloads.
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
- The version abbreviation is immediately right of the location; the pair stays
  centered, avoids side controls, and elides long references with a full tooltip.
  `versionPickerKeyboardAndGeometry` covers small/scaled layouts, search and empty
  results, keyboard navigation, footer/Shift V/command/Keybindings entry points,
  unmodified Enter acceptance, Shift Enter/Return doing nothing, and focus mode.
  Version lists place the active edition first, then other editions in persisted
  most-recently-used order.
- `removalWizardCancellationAndLastVersion` covers version → annotations choice
  (keep by default) → final confirmation, initial Cancel focus, cancellation at
  every step, Back, scaled layout, last-version setup/empty state, and optional
  same-ID NET reimport reconnecting retained annotations.
- `versionCallbacksGuardUnsavedNotes` checks failed draft saves and failed backend
  switches leave the picker open and preserve notes/library state.
- `realKjvSwitchAndRemoveAnnotations` uses `OMAWORD_TEST_KJV` for async import,
  busy guards, source-copy deletion, active footer/reader/About name and copyright,
  same-passage switching and annotation isolation, removal with annotation deletion,
  reimport, and active-version removal selecting the remaining edition.
- Add version opens the supported-layout instruction guide before the file picker.
  Coverage checks the official `https://bible.org/downloads` link, required
  metadata/verse anchors and unsupported-layout wording, local SQLite/data-path
  and offline instructions, and
  same-edition replacement preserving notes, bookmarks, highlights, and settings.
  It also checks note-draft flushing, modal isolation, guide-to-picker handoff,
  Cancel/Esc focus restoration, and small/scaled scrolling with accessible buttons.
  This is a local-file import flow: the user downloads the text in a browser;
  OmaWord bundles no EPUB/text/database, runs no Bible download server, and does
  not automatically fetch from Bible.org or eBible.org. The guide confirms both
  assessed NKJV files are unsupported OCR; importer tests verify rejection.
  The direct `omaword --import <supported-local.epub>` CLI calls the same importer,
  and its help names NET 2.1 and Haiola/eBible KJV 2006 support.
- `backupScopeAndExport` covers the active-edition scope panel before the save
  file dialog, exclusions, separate per-edition backups, reimport for retained
  annotations, cancellation, and export. Restore confirmation also names the
  active edition and explains version-scoped replacement.
- Optional `asyncFirstImportLayout` uses `OMAWORD_TEST_EPUB` to import a temporary
  EPUB copy through the existing file-acceptance handler, verify async completion,
  remove that copy, navigate/read at different sizes, and reopen the reader with
  identical imported text. This verifies source-EPUB independence after successful
  import: normal use reads SQLite and permits moving or deleting the source EPUB.

## Import limits and chapter metadata provenance

Supported identity metadata is layout-specific:

| Input | OPF identity | Stable ID / abbreviation | Verses |
| --- | --- | --- | --- |
| Official NET Bible 2.1 | Title `NET Bible 2.1`, identifier containing `NETBIBLE21` | `net-2.1` / `NET` | 31,087 |
| Haiola/eBible KJV 2006 | Title `King James (Authorized) Version`, identifier `urn:uuid:51004fba-2cea-50c8-8e37-fcebe15c315a` | `kjv-2006` / `KJV` | 31,102 |

Package recognition also requires supported structure. KJV uses 66 book spine
documents and Haiola chapter/verse markers; arbitrary KJV EPUBs are not accepted.
New text databases store `schema=2`, `paragraph_layout=1`, `version_id`,
`abbreviation`, `edition`, and `copyright` (from OPF rights), plus validated boolean
`verses.paragraphStart`. Reliable boundaries come from source paragraph elements
and layout pilcrows, with layout ¶ stripped from text/HTML. Legacy schema-1 text
remains supported with the recovery/fallback described above.
The personal database's schema 3 and backup format
version 3 are separate from this text-database schema. Both edition imports are
validated across all 66 books and 1,189 chapters.

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
The public numeric helpers remain NET-specific. KJV validation uses the three
KJV maxima above and includes the 16 verse numbers omitted by NET, for 31,102
verses. Runtime navigation uses the active database's ordered verse keys rather
than applying NET omission rules to KJV.

Navigation reads and validates the entire destination chapter before saving or
publishing state. Restore stages that chapter plus an annotation read-back within
its transaction, then publishes the already-staged models after commit without
additional SQL reads. Fault tests exercise chapter-body and annotation read-back
errors, verifying no signals/state changes and persistence after reopening.
Markdown and JSON exports share destination protection, including symlinks to
live databases and existing or dangling SQLite sidecars.
