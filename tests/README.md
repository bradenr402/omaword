# OmaWord tests

## Runner and selectors

`bin/test --suite <selector>` selects a suite; repeat `--suite` for multiple
targets. Omitting the selector or selecting `all` includes all available suites.
Duplicate selections run once. Qt arguments, including function names, data-row
filters, and `-functions`, require exactly one selected suite.

| Selector | Project | Build directory | Executable |
| --- | --- | --- | --- |
| `backend` | `tests/backend.pro` | `build/tests/` | `tst_backend` |
| `ui` | `tests/ui.pro` | `build/ui-tests/` | `tst_ui` |
| `tags` | `tests/tags.pro` | `build/tags-tests/` | `tst_tags` |
| `tagsui` | `tests/tagsui.pro` | `build/tagsui-tests/` | `tst_tagsui` |
| `desktop` | `tests/desktop.pro` | `build/desktop-tests/` | `tst_desktop` |
| `importer` | `tests/usfm.pro` | `build/usfm-tests/` | `tst_usfm` |
| `catalog` | `tests/catalog.pro` | `build/catalog-tests/` | `tst_catalog` |
| `annotations` | `tests/blockannotations.pro` | `build/blockannotations/` | `tst_blockannotations` |
| `instance` | `tests/singleinstance.pro` | `build/singleinstance-tests/` | `tst_singleinstance` |
| `windows` | `tests/windows.pro` | `build/windows-tests/` | `tst_windows` |

`importer` selects the USFM suite. Useful targeted cases include
`renderAndMetadata`, `sourceOrdering`, `standalone`, `malformed`, `corruption`,
and `actualArchives`. Catalog cases include `metadataAndAllowlist`,
`streamingAtomicSuccess`, `cancelAndReuse`, `timeouts`, and `commitFailureAndRetry`.

Tag targets include `namesAndDurability`, `sourceOrderVersionsRelatedAndPeers`,
`backupsScopedLegacyAndMarkdown`, and `rollbackAndPublication` in `tags`;
`pickerPinnedKeyboardAndLiteralNames`, `relatedPinnedUnionAndUseCurrent`, and
`mainIntegrationVisualCompletionAndDraft` in `tagsui`. Desktop targets include
`effectiveInteger`, `timeoutDoesNotFreezeOrOverlap`, `toolTipThemeAndWrapping`, and
`sharedAcrossWindows`. For example, `bin/test --suite tags namesAndDurability`
selects one function; `bin/test --suite tags --suite tagsui --suite desktop`
selects the three complete suites.

Builds are incremental. `JOBS` caps total Make jobs, defaulting to CPU count;
`TEST_JOBS` caps concurrent suites, defaulting to 2. Concurrency is clamped to
selected suites and `JOBS`; each worker gets `floor(JOBS / concurrency)` jobs.
Both settings require positive integers. `BUILD_DIR` overrides the build root.
Each suite reports status/timing and writes complete output to
`build/test-logs/<selector>.log`. Qt totals in current logs provide pass/fail/skip
counts. GUI suites use offscreen Qt with software rendering.

## Fixtures and isolated source archives

The test suites use USFM fixtures and generated, small original text tailored to
the behavior under test. Backend, UI, and window fixtures cover selected references
without generating a whole Bible. Parser fixtures exercise formatting, sparse
chapters, source ordering, metadata, and malformed input. Catalog tests use fake
network replies and a local HTTP server; ordinary checks need no public download.
Tag suites generate small USFM editions in temporary application-data directories;
backend tag fixtures include sparse/source-ordered Apocrypha and cross-book ranges.
Desktop tests inject a fake `hyprctl` command and temporary environment/theme state
to exercise compositor responses without changing the running desktop.

Set `OMAWORD_TEST_USFM_DIR` to a readable directory containing the seven source
ZIPs, such as a local Downloads directory. The runner normalizes this path before
workers change directories; it does not discover Downloads automatically.

Expected filenames are:

- `engnet_usfm.zip`
- `eng-kjv2006_usfm.zip`
- `eng-kjv_usfm.zip`
- `engojb_usfm.zip`
- `engylt_usfm.zip`
- `eng-asv_usfm.zip`
- `engbsb_usfm.zip`

`actualArchives` in the importer suite has a row for each source. Without the
directory, these cases skip; with it, each expected input must exist. Tests copy
archives to temporary names, import into isolated databases, and compare source
words/keys with an independent lexical oracle. They check imported metadata,
formatting, counts, and database integrity, including NET's 1996–2016 attribution,
OJB's names/order and license, KJV+'s 80 books, and ESG chapters 10–16.

Backend `optionalSourceArchives` also covers all seven sources. Backend-only
single-file overrides are `OMAWORD_TEST_NET_ZIP` and `OMAWORD_TEST_KJV_ZIP`.
UI source checks use the directory for NET/KJV; `OMAWORD_TEST_KJV` can select the
UI's KJV USFM ZIP directly. Supplied file paths are normalized and must be readable.
`OMAWORD_TEST_APP` selects a built executable for optional actual-launch checks.

Source archive checks use temporary application data and do not populate the
normal installed library. Supplied archives are read or copied, not modified.
Source-independence coverage reads and reopens SQLite after deleting a temporary
source copy. All-seven validation belongs in this isolated setup; regular library
installation remains a reader action through Add version.

## Coverage

- **Importer:** stable 80-book identities; source names/order; standalone USFM;
  `copr.htm` title/rights decoding; nested notes and word attributes; paragraphs,
  headings, poetry, omissions; explicit bridge/suffix errors; unsafe archives;
  malformed markers; staged publication and corruption detection.
- **Catalog:** seven-entry metadata/URL validation; streamed atomic cache writes;
  HTTP/size/ZIP-envelope failures; cancellation, timeout, retry, cache failures,
  and destruction during a transfer. Transport completion is separate from import.
- **Backend:** empty-library preferences; actual chapter lists; source-order ranges;
  aliases; copying and annotations; source-ID backups; schema-5 preservation;
  transactional failures; file ownership; MRU/fallback; peer commits and draft vetoes.
- **UI:** empty/About content; catalog search and attribution; download lifecycle
  and guarded import handoff; source-ordered browsing; version picker/removal;
  keyboard contexts; reading layouts/settings; selection/copy; Saved; Markdown
  draft conflicts; backup scope and source-independent reading; success-only Visual
  completion, failed marks, pinned Notes/Tags and stale panels; tag markers,
  shortcuts, Saved integration, and square highlight geometry.
- **Tags:** Unicode NFC/case-folded identity and literal names; durable global
  definitions; exact membership versus overlap; tag-only Saved entries and exact
  whole-entry deletion; any/all queries, excerpts and source ordering; pinned
  Related exclusions; cross-version opening and independent peer selections;
  rename/delete/merge; removal keep/drop and stale scopes; scoped v4 backup,
  v3 assignment preservation, ID/name collisions, escaped Markdown; injected SQL
  failures, commit rollback, read-back verification, publication and flush vetoes.
- **Tags UI:** immediate Space/Enter toggles and create/apply; captured picker
  scopes, overlap Open/Edit, literal labels, invalid names and failed flushes;
  any/all and installed-version filters; Related's overlapping-tag union, pinned
  source and Use current; management confirmations and peer invalidation; removed
  versions; real Main integration for Visual completion and unsaved note drafts.
- **Desktop:** effective integer rounding, malformed/oversized output, process
  failure, missing session/command, timeout responsiveness, nonoverlapping queries,
  recovery and separate five-second polling; themed plain-text tooltip wrapping,
  window-width bounds, and shared live radius updates across reader engines.
- **Annotations:** flowing-text verse mapping, paragraph/layout changes, range
  painting, bookmark/note/tag geometry, overlapping entries, exact tag marker
  scopes and continuation metadata, and focus-mode visibility.
- **Windows:** independent readers with shared commits; compare-and-set note
  conflicts; flush vetoes; replacement/removal; shared import guards; closing one
  reader; last-window exit; secondary launches opening a new reader.
- **Instance:** process handoff and request routing, startup races, bounded IPC,
  stale endpoint recovery, and optional actual-application launch checks.

`tests/test-runner.sh` separately checks runner selection, argument forwarding,
fixture paths, concurrency, logs, failure propagation, and interruption cleanup
using fake build/test tools. `tests/test-install.sh` exercises installation with
temporary homes. Test results are reported from actual runs, not fixed totals in
documentation.
