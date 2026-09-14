# OmaWord architecture

C++17 and Qt 6 Quick/QML, SQL, Concurrent, DBus, and Network; libarchive handles
USFM ZIPs. `omaword.pro` builds the application with qmake. `resources.qrc` embeds
QML and the seven-entry `resources/catalog.json` metadata catalog.

## Components

| Component | Responsibility |
| --- | --- |
| `src/usfmimporter.*` | `UsfmImporter` namespace: inspect edition metadata, parse USFM, stage and validate text databases |
| `src/biblebooks.*` | Stable 80-book code/name/alias registry; IDs are independent of source ordering |
| `src/versioncatalog.*` | Asynchronous direct HTTPS ZIP transfer, cache publication, cancellation, progress, and errors |
| `src/biblebackend.*` | Per-reader active text, references, annotations, preferences, transactional library operations, export/restore |
| `src/singleinstance.*` | One process coordinator per data directory, `omaword.lock`, and bounded launch-request IPC |
| `src/windowmanager.*` | A backend, catalog object, QML engine, and window per reader; lifecycle and request routing |
| `src/blockdocument.*`, `qml/BlockReader.qml` | Flowing layouts, verse/document mapping, selection and annotation geometry |
| `qml/Main.qml` | Reader, passage/command palettes, notes, Saved, settings, catalog, and attribution |
| `src/desktop.*` | System fonts/scaling, live Omarchy theme integration, Qt palette fallback, external links |

## Import and catalog lifecycle

`catalog` exposes `entries` with `{id,name,abbreviation,url,detailsUrl,copyright}`,
plus `busy`, `progress`, and `error`. `download(id)`, `cancel()`, and `clearError()`
control one transfer per catalog object. Download URLs come from the bundled
catalog; production transfers use HTTPS without a service API or account.

Downloads have a 64 MiB bound, a 30-second stall timeout, a five-minute total
timeout, and bounded redirects without HTTPS downgrade. `QSaveFile` publishes
`catalog/<id>_usfm.zip` under Qt's cache location only after HTTP/length checks
and nonempty ZIP-envelope screening. Failed or canceled transfers discard the
pending write and permit retry. Content parsing belongs to the importer.

`downloadFinished(QUrl)` reaches only the requesting reader. QML retains a pending
cached file for retry when shared import activity or any affected reader's note
flush blocks import, including a peer flush veto. Otherwise it
calls `bible.importUsfm(file)` once. Download progress/errors and backend import
progress/status are separate; installed feedback requires backend success.
Local `.zip`, `.usfm`, and `.sfm` selection and CLI `--import` use the same backend.

`UsfmImporter::inspectFile` checks archive resources and edition metadata without
creating a database. `importFile` performs full marker parsing and validation in
a staged database, then publishes atomically. See [USFM.md](USFM.md) for details.

## Dynamic reader model

`BibleBackend` is exposed as `bible`. `edition`, `versionId`,
`versionAbbreviation`, `versionCopyright`, and `hasParagraphs` notify through
`versionChanged`. `versionsChanged` publishes active-first/MRU installed editions.
`booksChanged` publishes `{id,code,name,abbreviation,chapters,chapterNumbers}` rows.
`chapterCount` is a count; `chapterNumbers` and `chaptersForBook(id)` contain the
actual chapter integers.

The active database supplies verse keys ordered by source book ordinal, chapter,
and verse. References resolve canonical aliases, USFM codes, and imported source
names. Navigation, range ordering, copy, Saved, and annotations use those keys;
persistent endpoints use stable book IDs and chapter/verse numbers. Source
omissions are preserved. Switching retains available endpoints and reports a
fallback when the destination lacks them.

Verse models contain plain text, safe HTML, headings, poetry, paragraph starts,
and annotation flags/ranges. `noteRanges` metadata and its renderer follow source
book order rather than numeric book IDs. Paragraph honors source boundaries; Verse uses rows;
Block flows text with visible headings separating paragraphs. `BlockDocument`
maps UTF-16 document spans back to verse identity for copying, hit testing, and
continuous range painting.

## Persistence and shared operations

The data root is Qt's application-data location for storage ID `omaword`.
`personal.sqlite` uses schema **5**, WAL, and synchronous durable writes:

- `annotations` and `reading` are scoped by source `version_id`.
- `preferences` is global; defaults are Paragraph, font size 18, line height 1.5,
  numbers/headings/verse picker on, and focus mode off. Font size is 8–40 in steps
  of 1; line height is 1.0–3.0 in steps of 0.1.
- `installed` records metadata, root-relative text paths, and `last_used` ranks.
  `library` records the startup active ID and usage counter.
- Annotation endpoint storage allows book IDs 1–80 and chapter/verse integers
  1–999. Ordering is checked against source keys. Schema updates are transactional
  and preserve personal data and preferences.

Text imports create immutable `bible-<UUID>.sqlite` generations. Validation and
read-back precede transactional registry publication. A replacement refreshes
affected reader connections before obsolete-file cleanup. Ownership and registry
checks guard text files and sidecars, including aliases to personal data,
symlinks, hardlinks, and unrelated targets. Failed operations preserve published
library and reader state.

Readers share committed annotations, preferences, and library changes while
keeping independent active versions, passages, directional selections, cursors,
and scroll positions. Shared imports use a busy guard. Replacement, removal, and
restore flush affected drafts; failure vetoes the mutation. Note saves compare
the exact-range saved text with the reader's observed baseline in a transaction.
Conflicts preserve dirty drafts for **Save my draft** / **Use saved text**.

Removal keeps annotations by default; they reconnect on same-ID reimport. Removing
an active edition selects another installed version or returns affected readers
to the empty state. Preferences remain available with no installed text.

Backups use `omaword-backup`, version **3**, with active source `versionId`,
`edition`, creation time, reading state, and annotations. Restore requires matching
active identity/name and valid endpoints, then replaces only that edition's
personal data transactionally. Preferences and text are outside backup scope.
Markdown export identifies the active edition and source ID.

## User interface and verification

The empty view uses the application name, catalog/local-import guidance, and local
privacy text. Ready-only Saved/Notes footer controls avoid empty reader actions.
About & Attribution shows dynamic source name/rights, source details links, local
storage information, and **No version installed** when empty.

`branding.env` supplies shared branding; build-local generated headers and desktop
files consume it. [tests/README.md](tests/README.md) documents the seven suite
selectors, generated original fixtures, and optional isolated source archives.
