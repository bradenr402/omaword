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
| `qml/Main.qml` | Reader, passage/command palettes, notes, Annotations, Visual action completion, settings, catalog, and attribution |
| `qml/TagsPanel.qml` | Reusable tag UI: pinned exact-range picker, embedded Annotations browser, Related side panel, and global management |
| `src/desktop.*`, `qml/AppToolTip.qml` | Shared fonts/scaling, live Omarchy theme, asynchronous compositor corners, themed tooltips, external links |

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
names. Navigation, range ordering, copy, Annotations, and reader marks use those keys;
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
- `tags(id,name,name_key)` holds global flat definitions with unique normalized
  name keys. `tag_assignments` has a composite primary key of `version_id`, both
  book/chapter/verse endpoints, and `tag_id`, plus a `(tag_id,version_id)` index.
  These independent tables are created transactionally with `IF NOT EXISTS`;
  the personal schema version remains **5**.
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

Readers share committed annotations, tag definitions/assignments, preferences, and
library changes while keeping independent active versions, passages, directional
selections, cursors, and scroll positions. Shared imports use a busy guard. Replacement, removal, and
restore flush affected drafts; failure vetoes the mutation. Note saves compare
the exact-range saved text with the reader's observed baseline in a transaction.
Conflicts preserve dirty drafts for **Save my draft** / **Use saved text**.

Removal keeps annotations and tag assignments by default; they reconnect on
same-ID reimport. Retained assignments are hidden from tag results and installed-use
counts while their source is absent. `removeVersion(id, true)` also deletes that
version's annotations, reading state, and assignments. Global tag definitions
remain in either case. Removing an active edition selects another installed
version or returns affected readers to the empty state. Preferences and backend
global tag operations remain available with no installed text; the UI's Tags and
Manage tags commands require a ready reader.

### Backup and Markdown export

Backups use `omaword-backup`, version **4**, with active source `versionId`,
`edition`, creation time, reading state, annotations, `tags` definitions
`{id,name}`, and `tagAssignments` with both endpoints and `tagId`. Only definitions
used by that source and its assignments are exported, from a consistent database
transaction. Preferences and Bible text are outside backup scope.

Restore accepts versions **3** and **4**, requires matching active source ID/name,
and validates fields, endpoint existence/source order, duplicate identities and
assignments before writing. Version 4 requires every supplied definition to be
used. The 32 MiB backup bound and per-array limits also apply. Affected drafts
must flush; the captured source is checked again before the transaction.

Version 4 replaces that source's assignments together with its annotations and
reading state. Global names define identity: existing normalized names are reused;
portable IDs survive when free, and a colliding ID with a different name is
remapped. Restore never renames or deletes existing global definitions to fit a
backup, preserving other versions' tags and unused definitions. Version 3 replaces
annotations/reading state while preserving existing tags and assignments. Read-back
and commit precede publication to peers; failures roll back the replacement.

Markdown export remains `omaword-notes.md`, identifies the active edition/source
ID, and includes tag-only saved ranges. `Tags:` lines escape tag names as literal
Markdown/HTML text; note bodies retain their authored Markdown.

## Passage tags

Names are edge-trimmed and NFC normalized, with Unicode case-folded identity.
Internal spaces, punctuation, emoji, and format characters such as emoji joiners
are preserved; empty names, controls, line/paragraph separators, invalid strings,
and names exceeding 4096 UTF-16 code units are rejected. There is no hierarchy or
tag color. Creating an equivalent name returns the existing ID without changing
its display spelling. Rename preserves the ID and rejects another tag's identity;
merge explicitly unions assignments into the target and removes the source.
Delete removes a definition and its assignments across installed and retained
versions. These operations preserve bookmarks, highlights, and notes.

`tagScope()` snapshots `{versionId,reference,book,chapter,start,endBook,endChapter,end}`.
Assignments match the source ID and both endpoints exactly, including cross-chapter
and cross-book ranges. Overlap does not imply exact membership. Writes validate
the installed source and integer endpoints against its actual text keys; the
display reference is not the authority. `setTag` is idempotent, and
`createAndAssignTag` commits definition creation and assignment atomically.
Transaction/read-back failures publish no successful tag mutation.

`tags` exposes `{id,name,count}` sorted by name key/ID, with counts for installed
assignments only. `selectionTags` contains exact membership; `overlappingTags`
contains all intersecting range records, including an exact match. All three
notify through `tagsChanged`; selection changes also emit that signal locally.
Committed tag changes refresh collection/verse metadata and notify every reader
sharing the data directory without navigating peers.

`taggedPassages(tagIds, matchAll, versionId)` filters exact range records by any
or all selected IDs. Empty filters return no rows; an empty version ID includes
all installed versions. Records contain source ID/abbreviation, full endpoints,
reference, all assigned `{id,name}` tags, exact bookmark/highlight/note values,
bounded excerpt, and `sharedCount`. Sorting is descending shared count, then start
and end in each source's book/chapter/verse order, then source ID. Read-only source
connections query endpoints and bounded text without loading entire Bibles.

`relatedPassages(source, tagIds, matchAll, allVersions)` uses the same query and
excludes every same-source range intersecting the captured source. Current-version
mode means that captured source's version. Querying never navigates;
`openTaggedPassage(scope)` explicitly flushes drafts, revalidates, switches source
if necessary, and opens the complete endpoint range in the requesting reader.

The backend `collection` unions annotations and tag-only ranges, deduplicating exact
endpoints within the active source. Annotations has **Bookmarks**, **Highlights**,
**Notes**, and **Tags** tabs in one row, with no **All** tab. The first three filter
the collection by their mark/note type; tag-only ranges are browsed through Tags.
Deleting an Annotations entry atomically removes its bookmark, highlight, note,
and all tag assignments for that exact range, leaving overlapping entries and global
definitions intact. `deleteAnnotation(reference, expectedVersionId)` guards the
confirmation's source identity and rejects absent exact entries.

Verse rows expose `hasTags` and `tagRanges`, carrying full scopes/tags plus
`tagStart`, `tagEnd`, `continuesBefore`, and `continuesAfter`. Verse-layout rails
preserve exact scopes and offer choices for overlapping tag ranges.

Paragraph and Block share a continuous `BlockReader` text layout. `BlockDocument`
reserves transparent inline image cells and word joiners at bookmark/tag endpoints;
QML paints opening/closing brackets within those cells, with a superscript icon
to the left of each opening bracket. Tag icons are horizontally mirrored to point
left. Each exact tagged range has one marker pair regardless of its tag count.
A range entering the chapter has a continuation chevron instead of a false start;
a range continuing beyond it has no false closing bracket. Notes remain in the
margin, with nearby note anchors grouped into a range chooser.

`inlineDecorations()` supplies cell geometry and captured scopes; verse spans keep
`textStart`/`textEnd` separate from decorated endpoints. `selectionText()` omits
only generated decoration segments, preserving original Unicode, brackets, joiners,
and object characters in copied text. Hit testing maps cells and text to their
owning verse. `contextRanges()` unions bookmark/tag ranges containing the cursor
verse and subtly shades their other visible members, excluding the cursor and
selected verses. It clears when the cursor leaves those ranges. Focus mode removes
inline cells, margin markers, and context shading; selection and highlight painting
remain independent. Width, font, and layout changes recalculate the shared geometry.

### Picker, browser, and Related

`TagsPanel` receives explicit `app`, `bible`, and `desktop` properties plus an
`embeddedHost`. A single reusable `tagsBody` is reparented by `hostKind` into the
picker's modal popup, Annotations' `annotationsTagsHost`, or Related's full-height
right-edge drawer. Management uses the current host with Back navigation.
`browserRequested`/`browserClosed` let Main select the Tags tab and open/close
Annotations. `browserVisible` tracks embedded content; `visibleModal` covers the
picker and Related, and `contentVisible` combines them for peer refreshes.
`scopePinned` tracks a visible picker editing scope. `open(scope)` copies
the scope; later selection/version changes do not retarget picker writes. Checked
tags reflect only that exact range. Other overlapping ranges are listed separately:
**Open** navigates and closes the picker; **Edit tags** navigates and pins that full
range for editing.

`t` opens **Tag passage**. Each click or Space/Enter in the tag list saves immediately;
Enter in search or **Create / assign** atomically creates/reuses and applies the
typed name. Space in the search field remains text input. Esc cancels a pending
management confirmation first, otherwise clears a nonempty query, then closes.
**Done** closes without undoing committed toggles. User labels render as plain text.

`Ctrl a` toggles Annotations in reader/panel navigation contexts; editable fields
retain Select All. Its fourth **Tags** tab and Commands → **Tags** open the browser
inside Annotations, below the tab divider. A new browser starts with no filters
and two exclusive pairs: **Any** / **All**, then a vertical divider, then
**\<abbr\> only** / **All versions**. The abbreviation follows the active version in
the browser; all-version queries include installed sources only. Enter/click opens
a full result and closes Annotations.

`Shift H` toggles headings; `Shift L`, Commands → **Related passages**, or the
bottom-left button beside the mode control opens Related's right-side panel.
It pins the source and initially selects the union of tags on every overlapping
range. Its version pair uses the pinned source abbreviation, even after opening
a result in another version. Opening results keeps the panel/source pinned;
**Use current** explicitly captures the new source and resets the tag filters.
Focus mode closes tag surfaces and hides the floating controls.

**Manage** is available in each tag host; **Manage tags** in Commands opens management
inside Annotations' Tags tab. Rename, delete, and merge each require confirmation
with global scope. Peer edits refresh visible lists, prune deleted IDs, and
invalidate confirmations if a source
or target tag disappears or changes name. Missing source versions disable pinned
picker actions and empty Related results; backend validation also rejects stale
endpoints. Busy-library and note-flush guards protect editing/navigation actions.

## Visual action completion

`Main.qml` captures action identity from version, exact endpoints/reference,
chapter, and cursor. Successful bookmark/highlight actions (`bool` backend results)
and nonempty passage copies use the full range, switch Visual to Normal, clear the
numeric prefix, and collapse to the cursor when no editing panel pins the range.
No extra "Visual selection off" toast is generated for action completion.

Opening Notes or Tags alone does not complete Visual mode. A successful note save
or tag assignment switches to Normal but retains the original range while Notes
or a tag editing scope remains open. `editingPanelClosed()` performs the deferred
collapse after both are released. A changed selection/version/cursor invalidates
the pending collapse so closing a stale panel cannot collapse a new selection.
Failed writes, failed draft flushes, and cancellation before a successful edit
preserve Visual mode and the range.

## Desktop surfaces

One `Desktop` object is shared across reader engines. `cornerRadius` starts at
zero and asynchronously queries `hyprctl getoption decoration:rounding -j` when
`HYPRLAND_INSTANCE_SIGNATURE` is present. A separate five-second poll tracks the
effective compositor value; theme/font refresh polls every two seconds. Only one
rounding query runs at a time, with a one-second deadline and 8192-byte output
bound. A finite, nonnegative integer JSON `int` value within the C++ integer range
is accepted. Missing session/command, invalid output, nonzero exit, or timeout
resets corners to zero; later polls can recover without blocking the UI.

Application-owned containers and control backgrounds bind to that shared radius,
including dialogs, menus, drawers, inputs, buttons, rails, scrollbars, and
`BlockReader` surfaces; small controls clamp it to their dimensions. Passage
highlight segments stay square. `AppToolTip` replaces stock tooltip instances
with explicit theme colors, system font/scaling, plain wrapped text, shared
corners, and width bounded by both 360 scaled pixels and the available window.

## User interface and verification

The empty view uses the application name, catalog/local-import guidance, and local
privacy text. Ready-only Annotations/Notes footer controls avoid empty reader actions.
About & Attribution shows dynamic source name/rights, source details links, local
storage information, and **No version installed** when empty.

`branding.env` supplies shared branding; build-local generated headers and desktop
files consume it. [tests/README.md](tests/README.md) documents suite selectors,
including `tags`, `tagsui`, and `desktop`, generated original fixtures, and optional
isolated source archives. [TAGS.md](TAGS.md) records the tag API and UI contract.
