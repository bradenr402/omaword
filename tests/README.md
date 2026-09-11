# OmaWord tests

## Fast targeted runs

From the repository root, select the suite relevant to the change:

```sh
bin/test --suite backend -functions
bin/test --suite importer genericStructured
bin/test --suite importer genericRejections
bin/test --suite importer unsupportedNumbering
bin/test --suite importer reservedIdentity
bin/test --suite ui versionPickerKeyboardAndGeometry
bin/test --suite windows independentReadersAndSharedCommits
bin/test --suite windows actualWindowDraftConflict
bin/test --suite instance -functions
JOBS=8 bin/test --suite backend --suite annotations
```

`--suite` accepts `backend`, `ui`, `importer`, `annotations`, `instance`, `windows`,
or `all`. Repeat it to select several suites; duplicate selections run once.
Omitting it selects all. Qt arguments, including function names, data-row filters,
`-functions`, and output options, require **exactly one suite**. An unqualified
`bin/test -functions` is therefore not a valid targeted invocation.

Use targeted cases during iteration, then run the full suite **once for final
validation**:

```sh
bin/test --suite all
```

The runner uses incremental qmake/Make builds and defaults to **two concurrent
suites** (`TEST_JOBS=2`). `JOBS` caps **total build jobs**, defaulting to the
detected CPU count. Worker concurrency is the minimum of TEST_JOBS, JOBS, and the
selected suite count; each worker receives `floor(JOBS / concurrency)` Make jobs.
For example, `JOBS=8 TEST_JOBS=2` gives two workers four build jobs each.
`TEST_JOBS=1` runs serially. Both settings must be positive integers.

Each suite prints START and PASS/FAIL with timing. Full build/test output is saved
to `build/test-logs/<suite>.log`, under `BUILD_DIR` when overridden. Failure output
is also printed, remaining suites finish, and the runner returns a nonzero status.
Interruption terminates active worker process groups. Qt's `Totals:` lines in the
logs provide pass/fail/skip counts; report those from the final run, including
fixture skips, rather than maintaining a fixed count here.

| Selector | Project | Build directory | Executable |
| --- | --- | --- | --- |
| `backend` | `tests/backend.pro` | `build/tests/` | `tst_backend` |
| `ui` | `tests/ui.pro` | `build/ui-tests/` | `tst_ui` |
| `importer` | `tests/importer_versions.pro` | `build/importer-versions/` | `tst_importer_versions` |
| `annotations` | `tests/blockannotations.pro` | `build/blockannotations/` | `tst_blockannotations` |
| `instance` | `tests/singleinstance.pro` | `build/singleinstance-tests/` | `tst_singleinstance` |
| `windows` | `tests/windows.pro` | `build/windows-tests/` | `tst_windows` |

GUI suites run with `QT_QPA_PLATFORM=offscreen` and `QT_QUICK_BACKEND=software`.
Tests isolate application data in temporary directories. Synthetic fixtures use
original text; no Bible content is included in test sources.

## Optional local fixtures

For final validation with your own downloaded NET/KJV examples:

```sh
OMAWORD_TEST_EPUB="$HOME/Downloads/NETBIBLE21.epub" \
  OMAWORD_TEST_KJV="$HOME/Downloads/eng-kjv2006.epub" bin/test --suite all
```

| Variable | Purpose |
| --- | --- |
| `OMAWORD_TEST_EPUB` | Official NET 2.1 importer/backend/UI/window integration |
| `OMAWORD_TEST_KJV` | Structured Haiola/eBible KJV 2006 backend/UI/window integration |
| `OMAWORD_TEST_KJV_EPUB` | KJV importer fixture; defaults to OMAWORD_TEST_KJV when unset/empty |
| `OMAWORD_TEST_NKJV_ORIGINAL_EPUB` | Optional original OCR rejection fixture |
| `OMAWORD_TEST_NKJV_EPUB` | Optional replacement OCR rejection fixture |
| `OMAWORD_TEST_APP` | Built application executable for actual-launch instance checks |

`bin/test` normalizes NET, KJV, and KJV_EPUB paths before workers change directory.
Use absolute paths for the OCR fixtures and application executable. Missing
optional inputs skip dependent cases; specified EPUB paths must be readable.
Backend multi-version and some window cases require both NET and KJV; UI KJV
cases can use synthetic NET data. The supplied EPUBs are not modified.
Source-independence cases import and delete temporary copies before reading and
reopening from SQLite.

Targeted examples:

```sh
OMAWORD_TEST_KJV="$HOME/Downloads/eng-kjv2006.epub" \
  bin/test --suite importer kjvAllWordsAndFormatting
OMAWORD_TEST_NKJV_ORIGINAL_EPUB="/absolute/path/to/original-nkjv.epub" \
  OMAWORD_TEST_NKJV_EPUB="/absolute/path/to/replacement-nkjv.epub" \
  bin/test --suite importer unsupportedOcr
OMAWORD_TEST_APP="/absolute/path/to/omaword/build/omaword" \
  bin/test --suite instance
```

## Importer coverage

The importer suite uses Qt Core, SQL, XML, Test, and libarchive, without a GUI.
The independent KJV DOM whitespace oracle needs Qt 6.5+ in a Qt 6 build.
The contract is a NET adapter plus a generic Haiola/eBible structured EPUB adapter
with compatible OSIS/USFM book codes and anchors. It has no filename or exact
known-file UUID admission gate. See [VERSIONS.md](../VERSIONS.md) for the full
structure and identity requirements.

- `genericStructured` exercises original synthetic Haiola and OSIS-anchor EPUBs,
  with complete NET 2.1 or KJV numbering profiles across all 66 books and 1,189
  chapters. It checks exact maxima/omissions, recorded profile metadata, stable
  `epub-<hash>` identity after renaming, supplied-word italics, explicit-note
  stripping, and independent detection of missing chapter-final keys and metadata
  tampering during database validation.
- `genericIdentity` checks stable identity and abbreviations for unfamiliar
  titles, including synthetic NKJV, NIV, WEB, and original-translation metadata.
  Unknown IDs hash the package identifier, or case-folded title without an
  identifier: `epub-` plus 32 hexadecimal SHA-256 digits. This is not a file-byte
  hash. With the real KJV fixture, changing its UUID passes generic import with an
  `epub-<hash>` ID and the supplied title preserved, rather than taking `kjv-2006`.
- `reservedIdentity` checks that `kjv-2006` requires the exact title
  `King James (Authorized) Version` and identifier
  `urn:uuid:51004fba-2cea-50c8-8e37-fcebe15c315a`. Other UUIDs, Modern King James
  Version (including with the original identifier), and NET title/identifier
  near-matches remain generic editions with preserved titles. `net-2.1` is reserved
  for `NET Bible 2.1` with exact identifier `urn:uuid:NETBIBLE21`. These identity
  rules protect existing editions and their annotations, not admission by UUID.
- `unsupportedNumbering` rejects missing final chapter/Bible markers even when
  their prose remains, extra verses, compensating chapter endings, mixed profiles,
  partial omissions, and NET omissions combined with KJV endings. Inspection and
  import reject these inputs while preserving the installed destination bytes.
- `genericRejections` checks missing/ambiguous titles and identifiers, absent,
  duplicate, decreasing, wrong-book, and out-of-range verse markers, missing
  chapters, wrong spine order, conflicting labels, ambiguous inline notes,
  unknown spans, and markerless OCR. Failure preserves destination bytes.
- `editions` inspects without database writes/connections, imports/reimports with
  progress, and checks known identity, metadata, exact counts/omissions, schema 2,
  `paragraph_layout=1`, and source paragraph metadata. Missing verses and invalid
  metadata fail database validation.
- `kjvAllWordsAndFormatting` independently compares all 31,102 verses against the
  supplied EPUB DOM, including inline tails, supplied words/italics, Psalm headings,
  and poetry.
- `kjvNotesAndHtml` checks explicit-note stripping without losing adjacent words,
  safe inline HTML, `.html` spine paths, duplicate marker rejection preserving
  installed data, and wrong marker prefixes.
- `boundedMetadataAndUnsafeArchive` uses generated fixtures to check bounded
  inspection of OCR metadata and rejection of unsafe archive paths.
- `unsupportedOcr` is rejection-only coverage for the explicitly supplied OCR
  files: no new database, unchanged existing destination, and no SQL connection
  leaks. Their lack of reliable markers does not establish that OCR in general
  contains study notes.

### Evidence and validation limits

NET 2.1 (`NETBIBLE21.epub`, `net-2.1`, 31,087 verses) and structured KJV 2006
(`eng-kjv2006.epub`, `kjv-2006`, 31,102 verses) remain verified real-file examples.
No real NIV file is verified or used as a successful import fixture. Synthetic
NIV-title tests establish metadata behavior, not support for an arbitrary NIV EPUB.

OmaWord's verse-notebook standard requires dependable book/chapter/verse identity
before annotations, quotations, and navigation can target text. Page-based OCR
without semantic markers fails that standard because boundaries and reading order
cannot be established. OCR alone does not imply study notes. The selected scope
provides no general ebook-reader fallback.

Generic structural validation requires all 66 books and 1,189 chapters in order,
increasing unique nonempty verse rows, and an exact match to one independently
defined complete NET 2.1 or KJV profile. Chapter maxima, counts, omissions, and all
verse keys must match that same profile, totaling 31,087 or 31,102 verses.
Actual keys select `net-2.1-v1` or `kjv-v1`, not title or abbreviation. Marker formats
and edition IDs accommodate broader translations; other numbering is unsupported.
Database validation repeats the independent profile check alongside source-key
and metadata digest checks. Numbering completeness does not independently verify
an unfamiliar edition's words against its publisher's text.

Known NET/KJV imports have exact chapter/count validation. The NET numeric helpers
derive from explicit source verse markers, including cross-file chapter endings.
The NET/KJV maxima differ in three chapters:

| Chapter | NET maximum | KJV maximum |
| --- | --- | --- |
| 2 Corinthians 13 | 13 | 14 |
| 3 John 1 | 15 | 14 |
| Revelation 12 | 18 | 17 |

Independent numeric references are the
[KJV dataset](https://github.com/scrollmapper/bible_databases/blob/master/formats/json/KJV.json)
and NET markers for [2 Corinthians 13](https://www.biblegateway.com/passage/?search=2+Corinthians+13&version=NET),
[3 John 1](https://www.biblegateway.com/passage/?search=3+John+1&version=NET), and
[Revelation 12](https://www.biblegateway.com/passage/?search=Revelation+12&version=NET).
Tests check book totals, all chapter counts, and a compensating Genesis 1:31 →
Genesis 2:26 corruption that preserves the book total. KJV includes the 16 verse
numbers omitted by NET. Runtime navigation uses active-database keys.

Paragraph tests independently check source boundaries and layout pilcrows, not
pilcrows alone: KJV Genesis 1 groups are 1–2 | 3–5 | 6–8 | 9–13 | 14–19 | 20–23 |
24–25 | 26–28 | 29–31, with starts 3 and 20 lacking ¶. Layout pilcrows are stripped
from plain text and safe HTML. Synthetic NET cases check `p` boundaries and
retained within-verse breaks. Metadata-free Paragraph uses verse rows until
reimport supplies source boundaries.

XML preflight includes discarded subtrees: maximum depth 64, formatting depth 16,
100,000 elements/document, and entity expansion limit 1,024. Expanded text and
generated output have cumulative budgets of 2 Mi UTF-16 code units/document and
64 Mi/import; each verse including headings has a 64 Ki output budget. Escaping
is charged before allocation and oversized headings fail before concatenation.
All failures precede installation of the staged database.

## Process and multi-window coverage

The `instance` suite checks process handoff, request forwarding, startup races,
stale socket/lock recovery, and bounded IPC validation. The single instance is
the **process coordinator**, not a single reader window. Every launcher invocation
requests a new reader in the primary process, applying its own reference/import
arguments rather than replaying an earlier import.

The `windows` suite exercises WindowManager and independent BibleBackend/QML
instances in one process:

- `independentReadersAndSharedCommits`: distinct readers/titles and independent
  passages, directional selections, cursor, and scroll state, with shared notes,
  flags, collection deletion, and global preferences.
- `failedCommitDoesNotPublish`: failed writes do not notify peers or alter their
  published data.
- `flushVetoRestoreAndRemoval` and `flushDoesNotPublishBeforeEveryDraftIsSaved`:
  shared destructive operations flush drafts, honor vetoes, and preserve peer
  reading locations during annotation refresh. Same-note drafts exercise
  transactional compare-and-set (CAS) during deferred peer delivery: the first
  successful save remains committed, a stale second draft conflicts, and the
  shared mutation is vetoed until explicit resolution.
- `actualWindowDraftConflict`: real QML editors preserve a failed draft through
  peer flag/note changes, show saved-text conflict guidance, block flush-dependent
  transitions, and resolve through **Save my draft** or **Use saved text**.
- `versionReplacementAndFallback`: shared library/busy guards, independent active
  versions, reimport connection refresh before old-file cleanup, and removal
  fallback preserving peer passage/selection. Requires NET and KJV fixtures.
- `importCompletionFlushVeto`: async completion rechecks drafts before publishing
  a replacement. Requires NET.
- `closingOneKeepsTheOther`, `rootSignalAndLastWindowExit`, and
  `realSecondaryCreatesAnotherWindow`: peer lifetime, in-process New window signal,
  last-window exit, secondary request creating a new reader, and lock ownership
  remaining with the primary PID.

The UI command is **New window**, bound globally to **Ctrl Shift N**. Library,
preferences, and version-specific annotations are shared; active versions and
live locations remain per-window.

## Backend behavior

- Root `personal.sqlite` uses schema 3 with version-specific annotations/reading,
  global preferences, installed-version metadata/paths/ranks, and a startup active
  version/MRU counter. New text generations use `bible-<UUID>.sqlite` filenames.
- Owned-file checks protect personal data and unrelated files during text removal,
  replacement, and export, including sidecars, symlinks, hardlinks, wrong-edition
  paths, and registry ambiguity. Cleanup rechecks ownership after commit.
- Preferences default to verse numbers/headings/verse picker on, Paragraph mode,
  font size 18, line height 1.5, and focus mode off. Tests cover persistence,
  invalid values, clamping/rounding, non-finite rejection, and write/commit rollback.
- References support aliases, numbered books, compact forms, cross-chapter/book
  ranges, chapter-only and single-chapter-book references, and pure previews.
  Endpoints must exist; ranges may span omitted numbers.
- Bookmark/highlight flags cover actual verses. Toggling partial/full coverage
  fills or splits ranges without losing other flags or notes. Notes belong to an
  exact range; overlapping selections do not overwrite them. `noteSaveRejectsStaleBaseline`
  checks transactional CAS against the reader's last observed note, including
  peer deletion, so stale drafts cannot silently overwrite newer saved state.
  Saved deletion removes one exact annotation, preserving overlaps and handling
  SQL/read-back/commit failures atomically. `deletionExpectedVersionGuard` checks
  rejection when the supplied expected version differs from the active edition.
- Quotation and plain copying cover full canonical ranges or the whole chapter
  without selection, normalize whitespace, and skip omissions. Quotation uses
  superscripts and `“¹⁶ … ¹⁷ …”\n\n—John 3:16–17` without an edition suffix.
  Free-text copying preserves text verbatim. Failed reads/state saves preserve
  clipboard and selection. Five-/ten-verse motions stop at chapter edges.
- Backups use only `omaword-backup`; version 3 includes active-edition `versionId`,
  edition, UTC time, reading state, and annotations. Restore validates identity,
  types, endpoints, and staged models before replacing only that edition's data.
  Preferences, other editions, Bible text, and EPUBs are outside backup scope.
  Markdown export writes `omaword-notes.md` atomically with active edition name/ID.
- `multiVersionLibrary` covers dynamic metadata, KJV import and keys,
  same-reference switching and missing-endpoint fallback, annotation isolation,
  scoped backups, persistence, export destination protection, removal, hidden
  retained annotations, same-ID reimport, and preference survival.
- `versionOperationRollback` checks install/switch/removal failures, missing or
  malformed targets, and empty-library transitions while preserving models,
  selection, installed files, annotations, and persisted library state.
- Navigation stages complete destination chapters before publishing. Restore
  stages chapter/annotation read-back within the transaction; fault cases check
  unchanged signals/state and persistence after reopening.

## Integrated UI and annotation coverage

- Passage palette browsing, previews, aliases/omissions, contextual keyboard back
  navigation, Space resume, clear-then-close Esc, and fresh passage/command entry.
- Normal/visual/count-based verse movement across chapters, row/flowing free-text
  selection and autoscroll, Unicode selection, keyboard-context isolation,
  quotation/plain mouse and keyboard copying, and failed-copy behavior.
- Paragraph/Verse/Block layouts, source boundaries, headings, poetry, verse hit
  mapping, optional numbers, and continuous highlight geometry. The `annotations`
  suite checks logical marker geometry, overlapping-range choosers, note navigation,
  paragraph boundaries, layout changes, and focus-mode suppression.
- Reading settings, sliders and keyboard isolation, shared preference persistence,
  font size 18 default/reset and 8..40 limits, line-height controls, text-size
  feedback only on change, and temporary focus-mode suppression.
- Searchable/executable Keybindings and command context, including New window /
  Ctrl Shift N, Switch version / Shift V, and Reading settings / Shift R.
- Animated notes overlays at wide/narrow/scaled sizes, exact-range autosave,
  failures, draft flushing, editor synchronization, and Esc/outside dismissal.
  Dirty drafts retain their version/reference/baseline through peer updates;
  conflicts preview saved text and require explicit **Save my draft** or
  **Use saved text** resolution before flush-dependent transitions proceed.
- Saved filtering/navigation, exact deletion confirmation and cancellation,
  overlapping-entry preservation, stacked markers, and failure/retry behavior.
  Confirmation captures version ID and exact reference, rechecks scope/entry
  existence before and after flushing, and invalidates on version change or entry
  disappearance; the expected version is also passed to backend deletion.
- Shared overlay scrollbars, saved scroll restoration, restore confirmation,
  app-owned dialog Esc dismissal, and narrow/system-scaled layouts.
- `versionPickerKeyboardAndGeometry`: footer abbreviation/location centering,
  elision/tooltips, search/empty results, active-first/MRU ordering, keyboard
  selection, entry points, focus mode, and small/scaled layouts. The picker's
  square **+** beside Search versions opens Add version, supports Tab/Enter, and
  closes the picker before the import guide; it is hidden during removal.
- `removalWizardCancellationAndLastVersion`, `versionCallbacksGuardUnsavedNotes`,
  and optional `realKjvSwitchAndRemoveAnnotations`: safe three-step removal,
  cancellation, save vetoes, independent annotations, reimport, and library state.
- Add version guide/file-picker handoff, local data path, browser download links,
  metadata/marker guidance, note flushing, modal isolation, and scaled scrolling.
  Optional `asyncFirstImportLayout` verifies reading and reopening after deleting
  a temporary source copy. `backupScopeAndExport` checks active-edition scope,
  separate backups, retained annotations, cancellation, and export.

## Branding and script checks

The application/command/package is **OmaWord** / `omaword`. `branding.env` supplies
qmake and shell metadata; qmake generates build-local `brand.h` and
`omaword.desktop`. Storage is `~/.local/share/omaword` or
`$XDG_DATA_HOME/omaword`, QML uses `OmaWord.Reader`, and test variables use
`OMAWORD_`. Regular/symbolic icons show verse lines, a selection bracket, and square
handles; Notes uses a pencil and annotation note markers use page glyphs.

Standalone script checks are separate from the six Qt suites:

```sh
bash tests/test-runner.sh
bash tests/test-install.sh
```

The runner check uses fake build/test tools to verify bounded parallelism, logging,
failure propagation, targeted Qt arguments, fixture path normalization, default
suite selection, and interruption cleanup. Installer checks use temporary homes
and cover fresh/repeated installs, replacement backups, collisions, symlinks, and
alternate branding. They do not require installing into your real home.
