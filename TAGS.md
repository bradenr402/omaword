# Passage tags contract

Global, flat tag names; version-specific exact passage assignments. This describes
the current backend and UI. See [IMPLEMENTATION.md](IMPLEMENTATION.md) for storage,
transaction, reader geometry, and desktop integration details.

## Names and identity

- Display names are edge-trimmed and NFC normalized. Identity is Unicode
  case-insensitive through a normalized case-folded key.
- Internal spaces are preserved. Unicode punctuation, emoji, and emoji joiners
  are accepted; `/` does not create a hierarchy. Tags have no colors.
- Empty names, tabs/newlines and other controls, line/paragraph separators,
  invalid strings, and names over 4096 UTF-16 code units are rejected.
- Creating an equivalent name reuses its ID and display name. Rename preserves
  the ID; a collision with another tag requires an explicit merge.
- Definitions are global, including unused tags and tags retained for removed
  versions. Counts include assignments in installed versions only. Backend global
  tag operations work without installed text; the UI's Tags and Manage tags
  commands require a ready reader.

## Scope and backend API

A scope is `{versionId,reference,book,chapter,start,endBook,endChapter,end}`.
Identity is the source ID and both integer book/chapter/verse endpoints, not the
display reference. Endpoints must exist in the installed source and run forward
in its actual book/chapter/verse order. Cross-chapter/book ranges are supported.
An overlapping range remains a separate assignment.

Properties are `QVariantList` values:

| Property | Contents |
| --- | --- |
| `tags` | `{id:QString,name:QString,count:int}`, ordered by name key then ID |
| `selectionTags` | The same tag maps for the exact current selection |
| `overlappingTags` | All intersecting range records, including an exact match |

All notify through `tagsChanged()`. Selection changes emit it locally; committed
tag/library changes notify readers sharing the data directory and refresh their
collections/verse indicators without changing independent passages.

Invokables:

- `QVariantMap tagScope() const`: snapshot the current selection; empty without one.
- `QVariantList tagsForScope(QVariantMap scope, bool overlapping=false)`: exact
  tag maps by default, intersecting range records when true; invalid scopes return
  no rows.
- `QString createTag(QString name)`: create/reuse a global definition; empty on error.
- `bool setTag(QVariantMap scope, QString tagId, bool assigned)`: idempotently set
  exact membership after validating the source, endpoints, and existing tag ID.
- `QString createAndAssignTag(QVariantMap scope, QString name)`: atomically
  create/reuse and assign; failure leaves no new definition or assignment.
- `bool renameTag(QString id, QString name)`: rename globally, rejecting collisions.
- `bool deleteTag(QString id)`: delete the definition and all its assignments.
- `bool mergeTags(QString sourceId, QString targetId)`: union assignments into
  the target, deduplicate exact memberships, then remove the source. The same
  existing ID as both arguments is a successful no-op.
- `QVariantList taggedPassages(QVariantList tagIds, bool matchAll=false,
  QString versionId=QString())`: any/all matching on each exact range; an empty
  version ID means all installed versions. Empty filters return no results.
- `QVariantList relatedPassages(QVariantMap source, QVariantList tagIds,
  bool matchAll=false, bool allVersions=false)`: the same query, excluding every
  same-version range overlapping the source. Default scope is the captured
  source's version, regardless of the reader's active version.
- `bool openTaggedPassage(QVariantMap scope)`: flush drafts, revalidate, switch
  versions if needed, and open the full endpoint range in this reader.

Range records contain the scope fields, `versionAbbreviation`, `tags:[{id,name}]`,
`sharedCount`, `excerpt`, and exact `bookmark`, `highlight`, and `note` values.
Query results include all tags on each matching range; `sharedCount` counts the
chosen IDs. Excerpts are bounded and populated for passage queries. Results sort
by shared count descending, then source-ordered start/end, then version ID.
Queries do not navigate or parse displayed references to recover identity.

Tag mutations are transactional, verify saved state before publication, and
return failure through their result plus `bible.error`. Missing tags, removed
versions, stale endpoints, busy shared operations, or SQL failures cannot silently
retarget a write. Global rename/delete/merge preserve marks and notes.

## Picker and Saved tags

`TagsPanel.qml` receives explicit `app`, `bible`, and `desktop` properties. Its
entry points are `open(scope=null)`, `openBrowser(manage=false)`, `openRelated()`,
and `closeForFocusMode()`; `visibleModal` exposes popup visibility, and `scopePinned`
indicates a visible captured editing scope. Main owns action completion through
`tagActionCompleted(scope)` and `editingPanelClosed()`.

- `t` / **Tag passage** opens a picker pinned to a copy of the exact scope.
  Selection or version changes never redirect its toggles.
- Click or Space/Enter on a tag saves immediately. Down from search focuses the
  tag list; Space in search is a normal space. Enter in search with a valid name,
  or **Create / assign**, creates/reuses and applies it atomically.
- Checked tags belong only to the exact pinned range. **Other overlapping ranges**
  lists separate assignments. **Open** opens that full range and closes the popup;
  **Edit tags** opens it and pins its own picker.
- Esc cancels a pending management confirmation, otherwise clears the query,
  otherwise closes. **Done** closes. Neither closing action undoes saved toggles.
- Saved has **All**, **Bookmarks**, **Highlights**, **Notes**, and **Tags** tabs.
  Tags opens its browser and closes Saved; Commands → **Tags** does the same.
  The browser starts with no filters, **Any**, and **Current version**. **All**
  requires every chosen tag on the same exact range; **All versions** includes
  installed sources only. Enter/click on a result opens it and closes the browser.
- `Shift H` toggles headings. `Shift L` / **Related passages** opens Related.
  **Tag passage**, **Tags**, **Related passages**, and **Manage tags** are commands.
- User names/references/excerpts render as plain text. Peer updates refresh lists,
  prune deleted filter IDs, and disable actions for removed source versions.
  Each edit/navigation action checks busy state and flushes the note draft.

## Related and management

Related pins the source scope on opening and selects the union of tags on all
overlapping source ranges, including exact membership. It starts in Any/current
source-version mode and excludes all same-version overlaps from results. Tags may
be adjusted with the same filters as the browser. Opening a result keeps Related
open and the source pinned, even across versions. **Use current** explicitly
captures the reader's new range and resets the selected tags from its overlaps.
Peer refreshes keep the chosen filters except for deleted IDs; they do not silently
replace the pinned source or reselect its tag union.

**Manage** opens global management with Back navigation; Commands → **Manage tags**
opens it directly. Rename, delete, and merge each show a confirmation describing
the global operation. Delete includes assignments in every version; merge keeps
the target name and removes the source. A peer rename/deletion of either involved
tag invalidates the pending confirmation. Failures leave the operation available
for correction/retry and preserve saved marks and notes.

## Saved entries, removal, and backup

- Saved **All** includes tag-only ranges and deduplicates tags/marks/notes at exact
  endpoints in the active version. Deleting an entry through
  `deleteAnnotation(reference, expectedVersionId)` removes its bookmark,
  highlight, note, and all tags atomically. Overlapping entries and global tag
  names survive; a tag-only entry is a valid deletion target.
- `removeVersion(id, false)` retains assignments, hidden from results/counts until
  same-ID reimport. `removeVersion(id, true)` drops that version's assignments
  along with its annotations/reading state. Both retain global definitions.
- Backup format `omaword-backup` **4** stores the active source's reading state,
  annotations, used `{id,name}` definitions in `tags`, and exact endpoint records
  with `tagId` in `tagAssignments`. It excludes other sources, unused definitions,
  preferences, and Bible text.
- Version-4 restore validates the matching active source and all records, then
  transactionally replaces its assignments/annotations/reading state. Global
  normalized names are reused; free portable IDs survive, and conflicting IDs
  are remapped. Existing global definitions are never renamed or deleted to fit
  a backup, preserving tags used elsewhere. Version-3 restore preserves existing
  tags and assignments while restoring annotations/reading state.
- Markdown export includes tag-only entries and safely escaped `Tags:` lines.
  Verse models expose `hasTags` and `tagRanges` with exact scopes/tags and
  `tagStart`, `tagEnd`, `continuesBefore`, `continuesAfter` metadata for reader rails
  and flowing annotation geometry.

## Visual actions and corners

Successful full-range bookmark/highlight actions and nonempty passage copies
switch Visual to Normal and collapse to the cursor when no editing panel pins
the range. Successful note/tag editing switches to Normal but retains the original
selection until Notes and pinned tag editing close. Opening/canceling without a
successful edit, failed writes, or failed draft flushes preserve Visual and its
range. Completion captures source/endpoints/cursor; changed context invalidates
deferred collapse. No extra Normal-mode toast is emitted.

One shared `desktop.cornerRadius` follows asynchronous Hyprland effective
`decoration:rounding`, with zero fallback. Queries have a one-second deadline,
bounded output, and a separate five-second poll; unavailable/invalid responses
reset to zero and later queries can recover. Application containers and control
backgrounds, including BlockReader, use the shared value with dimension clamps
where needed. `AppToolTip.qml` supplies themed, plain wrapped text and the same
corners for tooltips. Passage highlight segments remain square.

## Verification map

[tests/README.md](tests/README.md) maps `bin/test` selectors to projects/builds.
`tags` covers backend identity, exact/overlap queries, persistence, peers, removal,
backup/export and rollback; `tagsui` covers picker/browser/Related/management and
Main integration; `desktop` covers asynchronous rounding and shared themed
tooltips. `ui` and `annotations` also cover Visual completion and reader tag
geometry. These are coverage descriptions, not test-run results.
