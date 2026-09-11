# Multi-version implementation contract

OmaWord is an offline verse notebook. Annotations belong to a stable edition and
canonical verse range. Switching versions keeps the current reference, falling
back to the chapter with feedback if a selection endpoint is absent. Retained
annotations reconnect when the same stable version ID is reimported.

## Structured EPUB import

`src/epubimporter.*` provides a NET adapter and a generic Haiola/eBible structured
EPUB adapter with OSIS/USFM-compatible book codes and anchors. Admission depends
on metadata and semantic structure, not a filename or exact known-file UUID gate.
An EPUB extension, translation title, or readable page is insufficient.

### Verified real-file examples

| Edition | Example local file | Stable ID | Verse count |
| --- | --- | --- | --- |
| Official NET Bible 2.1 | `NETBIBLE21.epub` | `net-2.1` | 31,087 |
| Haiola/eBible King James (Authorized) Version | `eng-kjv2006.epub` | `kjv-2006` | 31,102 |

Both contain 66 books and 1,189 chapters. Reserved edition IDs require exact
title/identifier pairs:

- `net-2.1`: `NET Bible 2.1` with `urn:uuid:NETBIBLE21`.
- `kjv-2006`: `King James (Authorized) Version` with
  `urn:uuid:51004fba-2cea-50c8-8e37-fcebe15c315a`.

These pairs protect the existing editions and their annotations. A KJV EPUB with
another UUID can be accepted generically with an `epub-<hash>` ID and its supplied
title preserved. Modern King James Version is also a separate generic edition,
even if it supplies the original KJV identifier. Matching an ID never bypasses
structure validation. Generic markers and identities accommodate broader
translations, but their numbering must match one complete supported profile.

### Generic structure and identity

- The OPF must have one nonempty edition title and unambiguous identifier metadata.
  A declared package `unique-identifier` must resolve. With no identifier, the title
  supplies the identity. Manifest/spine entries must resolve safely and define an
  unambiguous reading order.
- Explicit `div.main` book containers identify canonical books. Supported body
  paragraph/poetry containers hold `span.verse` markers such as Haiola `GN1_1` or
  OSIS-style `Gen.1.1`; compatible OSIS/USFM book codes are accepted. Filenames and
  prose titles do not supply missing book or verse identity. This is an EPUB XHTML
  structure contract, not unrestricted OSIS XML or raw USFM-file support.
- Unknown editions receive `epub-<hash>` IDs: the first 32 hexadecimal digits of
  SHA-256 over `identifier:<package identifier>`, or `title:<case-folded title>`
  without an identifier. This is an identity-metadata hash, not an archive-byte
  hash. Renaming the EPUB does not change its ID. The displayed name, abbreviation,
  and copyright come from metadata.
- All 66 books and 1,189 chapters must occur in canonical order, with increasing,
  unique verse markers and nonempty verse text. Generic imports must exactly match
  one complete, independently known **NET 2.1** or **KJV** versification profile:
  every verse key, chapter maximum, count, and omission, and the full total of
  31,087 or 31,102 verses respectively. Actual keys select the profile, independently
  of translation title or abbreviation. Other numbering schemes are unsupported.
- Missing chapter-final markers, extra verses, mixed profiles, and partial omission
  patterns fail even if the prose or total verse count appears complete. Database
  validation repeats the independent profile check; source-key and metadata
  digests provide additional consistency checks. The profile is recorded as
  `net-2.1-v1` or `kjv-v1`. This validates numbering completeness, not every word
  against the publisher's text.
- Supplied words/italics, headings, poetry, within-verse breaks, and paragraph
  boundaries survive import. Explicit note subtrees are stripped; unsupported or
  ambiguous inline elements are rejected rather than guessed into Scripture.

### Why an NIV EPUB still needs evidence

The verse-notebook standard is epistemic: the importer must know which canonical
verse each text span belongs to before notes, highlights, copying, and navigation
can rely on it. A title saying NIV establishes neither semantic markers nor
reading order. No real NIV file has been verified here. Synthetic NIV-title cases
test metadata handling only and do not certify a generic NIV download.

Page-based OCR without dependable markers is rejected because verse boundaries
and reading order cannot be established. OCR does not by itself imply study notes
or commentary. The assessed original and replacement NKJV OCR inputs are
rejection-only examples, not a translation-wide exclusion. Per the selected
product scope, unstructured files have no general ebook-reader fallback.

### Importer interface and storage

`Edition { QString id, abbreviation, name, copyright; }` describes an edition.
`inspectFile(path, error)` checks metadata and supported structure without database
writes and returns an empty ID on failure. `importFile(path, target, progress)`
returns a Result containing the Edition. Failed inspection/import preserves the
existing destination. Imports are staged and validated before installation.

Text databases use schema 2 with `paragraph_layout=1`, `version_id`, `abbreviation`,
`edition`, and `copyright`, plus boolean `verses.paragraphStart`. Generic editions
also record source identity, source-key digest, versification, and metadata digest.
Source `p`/Haiola `div.p` boundaries and layout pilcrows both contribute paragraph
starts; layout ¶ is stripped from text/HTML. Verified KJV Genesis 1 groups are
1–2 | 3–5 | 6–8 | 9–13 | 14–19 | 20–23 | 24–25 | 26–28 | 29–31. Starts 3 and 20
have no pilcrow. Without paragraph metadata, Paragraph uses verse rows; reimport
supplies source boundaries while preserving the reading-mode preference.

## Library, annotations, and windows

`BibleBackend` exposes dynamic `edition`, `versionId`, `versionAbbreviation`, and
`versionCopyright` through `versionChanged`; `versionsChanged` publishes maps
`{id, abbreviation, name, active}`. Lists put that window's active version first,
then other editions by persisted most-recently-used rank.

`switchVersion(id)`, `removeVersion(id, removeAnnotations)`, and `importEpub(file)`
operate transactionally. Successful import activates the edition in the requesting
window. Navigation, copying, Saved, notes, and export use its actual SQL verse keys;
persist canonical references, never edition-local offsets. Failed validation,
writes, read-back, or commit preserve the previous library and models.

Every launcher invocation creates a new reader in one primary process, coordinated
by `omaword.lock` and IPC. A secondary forwards its reference/import request and
exits. **New window** / **Ctrl Shift N** creates another reader in-process.
WindowManager owns a backend, QML engine, and window per reader. Readers share
preferences, annotations, and installed versions, but retain independent active
versions, locations, directional selections, and scroll positions. Closing one
leaves peers usable; closing the last exits.

Shared commits refresh peers without navigating them. Library mutations and
restore flush affected drafts; failed saves veto replacement. Imports share a
busy guard. Reimport reloads affected text connections before obsolete-generation
cleanup. Removing a version affects all readers using it: another installed
edition becomes active, or the reader returns to setup. Kept annotations remain
hidden until same-ID reimport; deleting annotations also deletes that ID's saved
reading state. Global preferences survive removal of the last version.

Note writes use transactional compare-and-set (CAS) against the reader's last
observed exact-range note, including deletion. A stale baseline rejects the write
and preserves the editor draft. Peer updates never silently replace dirty drafts
or authorize overwriting a newer save. The conflict panel previews saved text;
**Save my draft** explicitly replaces it within the matching version/reference,
while **Use saved text** discards the draft and loads saved state. Conflicts veto
flush-dependent transitions and shared mutations until resolved. During a
multi-window flush, an earlier successful save remains committed if a later draft
conflicts; the shared removal/restore/replacement is vetoed.

Saved deletion captures the exact reference and version ID. Both scope and entry
existence are rechecked after flushing; stale confirmations close. The backend's
`deleteAnnotation(reference, expectedVersionId)` rejects a changed active version
before deletion, preventing confirmation from targeting another edition.

### Data and backups

Data defaults to `~/.local/share/omaword/`, or `$XDG_DATA_HOME/omaword/`.
Root `personal.sqlite` uses schema 3: `annotations` and `reading` are keyed by
`version_id`; `preferences` is global; `installed` holds stable ID, root-relative
path, metadata, and `last_used`; `library` holds the startup active ID and monotonic
`use_counter`. Successful activation/import updates recency atomically; startup
and selecting the already-active version do not. Live windows keep independent
reading state rather than following the persisted startup active ID.

New text generations are root `bible-<UUID>.sqlite` files. Registration follows
validation and commits before obsolete text cleanup. Ownership and registry checks
reject personal-store aliases, unrelated targets, symlinks, hardlinks, and unsafe
files/sidecars; cleanup rechecks ownership.

Annotation backups use only the **`omaword-backup`** signature. Version **3** exports
contain `versionId`, `edition`, UTC creation time, reading state, and annotations
for the active edition only. Restore requires the matching edition to be installed
and active; stable ID and edition name must match. Endpoint validation and
replacement of that edition's annotations/reading commit together. Other editions
and global preferences survive. Markdown export writes `omaword-notes.md` with
the active edition's name/ID and annotations. Backups exclude Bible text, EPUBs,
and reading preferences.

## Reader controls

- Paragraph (default), Verse, and Block modes share global `readingMode`; `b`
  cycles them. Poetry and within-verse breaks remain in every mode.
- Global `fontSize` defaults/resets to 18, range 8..40, steps of 1 before desktop
  scaling. Ctrl + / Ctrl − and Ctrl 0 adjust/reset it. Ctrl [ / Ctrl ] and
  `Ctrl \` adjust/reset line height. Reading settings exposes these preferences;
  unmodified Left/Right does not alter its sliders.
- The footer version abbreviation follows the location. Clicking it, Shift V,
  or **Switch version** opens the searchable picker. Enter selects and closes;
  Shift Enter does nothing. Active-first/MRU ordering is shared through the library.
- The square **+** beside Search versions opens **Add version**, closing the picker
  before the guide. It supports Tab focus and Enter activation, is disabled while
  busy, and is hidden during removal. Setup and Commands also expose Add version.
- Removal has three steps: edition, annotations choice (keep by default), then
  confirmation (Cancel focused). Cancel/Esc cancels at every step.
- The reader label and About use active edition metadata; empty rights are hidden.
  The NET permissions link appears for NET. Backup scope/restore panels name the
  active edition and explain replacement and separate backups per edition.
- Regular/symbolic app icons show verse lines, a selection bracket, and square
  handles. Notes uses a pencil; annotation note markers use page glyphs.

## Verification workflow

`bin/test --suite backend|ui|importer|annotations|instance|windows|all` selects
suites; the default is all. Repeat --suite for multiple targets. Qt arguments
require exactly one suite:

```sh
bin/test --suite importer genericStructured
bin/test --suite importer genericIdentity
bin/test --suite windows independentReadersAndSharedCommits
bin/test --suite ui versionPickerKeyboardAndGeometry
JOBS=8 bin/test --suite backend --suite annotations
bin/test --suite all
```

Use targeted checks during iteration and one full run for final validation.
The default is two concurrent suites (`TEST_JOBS=2`). `JOBS` caps total build jobs,
defaulting to CPU count; concurrency is clamped to JOBS and selected suite count,
with floor(JOBS / concurrency) Make jobs per worker. Logs and Qt totals live in
`build/test-logs/<suite>.log` (under `BUILD_DIR` if overridden). Report current
pass/fail/skip totals rather than fixed counts.

Optional real NET/KJV and rejection-only OCR fixtures are described in
[tests/README.md](tests/README.md). No copyrighted Bible content belongs in the
repository; imports use explicitly selected local files and reading uses SQLite
afterward, independently of the source EPUB.
