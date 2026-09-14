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

## Annotations summary and modal tag picker

`TagsPanel.qml` is the reusable tag UI, with explicit `app`, `bible`, `desktop`, and
`embeddedHost` properties. Main supplies an embedded host for the read-only
Annotations summary. The shared body also serves the modal picker/management and
Related right-side drawer. Entry points are `open(scope=null)`,
`openSummary(scope=null)`, `openBrowser(manage=false)`, `openRelated()`,
`openRelatedForTag(id, source)`, and `closeForFocusMode()`.
`browserRequested`/`browserClosed` control the Annotations drawer;
`embeddedClosed()` clears the summary scope. `browserVisible` tracks the embedded
summary (`hostKind: "browser"`, `page: "summary"`), `visibleModal` covers the picker
popup and Related, and `contentVisible` combines them. `scopePinned` requires a
visible picker host and captured `editingScope`; the summary never pins editing.
Main owns action completion through
`tagActionCompleted(scope)` and `editingPanelClosed()`.

- `t` / **Tag passage** opens the modal picker (`hostKind: "picker"`, `page: "picker"`)
  for the focused exact range, even with an embedded host. It closes Annotations,
  adds/removes existing tags, and creates/applies new ones. A reader marker supplies
  its complete captured range without navigating to the clicked verse.
- The picker snapshot stays pinned until reopened; every write uses those exact
  endpoints. `followSelection()` updates only the visible summary after a successful
  draft flush. Even an initially empty picker must be reopened to capture a range.
  With no selection, it prompts **Select a passage to add or remove tags.** and
  disables assignment/creation.
- Click or Space/Enter on a tag saves immediately. Down from search focuses the
  tag list; Space in **Find or create a tag…** is a normal space. Enter with a valid name,
  or **Create / assign**, creates/reuses and applies it atomically.
- Checked tags belong only to the exact editing range. **Other overlapping ranges**
  lists separate assignments. **Open** opens that full range and closes the editor;
  **Edit tags** opens it and edits its complete scope.
- Esc cancels a pending management confirmation, otherwise clears the query,
  otherwise closes. **Done** closes. Neither closing action undoes saved toggles.
- `Ctrl a` toggles Annotations in reader/panel navigation contexts; editable fields
  retain Select All. Its single tab row is **Bookmarks**, **Highlights**, **Notes**,
  **Tags**, with no **All** tab. Tags and Commands → **Tags** open the read-only
  summary below the tab divider via `openSummary()` / `openBrowser(false)`.
  **Tags on this range** shows exact membership; **Tags on overlapping ranges**
  lists separate intersecting assignments; **Other passages with these tags**
  matches Any of their combined tags in the summary source version and excludes
  all same-version overlaps. References open full ranges and refresh the open
  summary. Empty states distinguish no tags, no exact tags, and no other passages.
  There are no search, filter, assignment, creation, Manage, or Done controls.
- Clicking any summary tag chip opens Related seeded with only that tag ID and
  the summary's captured source/version, including chips on overlapping or related
  rows. This closes Annotations and does not change assignments or complete a
  Visual editing action.
- `Shift H` toggles headings. `Shift L` / **Related passages**, or the bottom-right
  outline left-pointing tag icon, opens Related's full-height right-side panel.
  **Tag passage**, **Tags**, **Related passages**, and **Manage tags** are commands.
- User names/references/excerpts render as plain text. Peer updates refresh lists,
  prune deleted filter IDs, and disable actions for removed source versions.
  Each edit/navigation action checks busy state and flushes the note draft.

## Related and management

Related is discovery-only, with `24 * desktop.textScale` padding on every side.
A large **Related** heading (`25 * desktop.textScale`) sits above a smaller muted
source reference/version label. Normal opening pins the source scope and selects
the union of tags on all overlapping source ranges, including exact membership;
opening from a summary chip selects only the clicked ID. Both start with **Any** and
the pinned source's **\<abbr\> only**, excluding all same-version overlaps from results.
Related adds passage-discovery search (**Filter tags…**) and adjustable results via
the exclusive **Any** / **All** and **\<abbr\> only** / **All versions** filter pairs,
separated by a vertical divider. **All** requires every chosen tag on the same
exact range; **All versions** searches installed sources only. Search covers all
global names, not just the source's tags, so an untagged source can still discover
passages. Tag toggles adjust filters without adding/removing assignments. Related
has no **Manage**, creation, or tag-mutation controls; `manage()` only accepts the
picker host.
Opening a result keeps the side panel open and the source pinned, even across
versions; the version label still names the source. The full-width, wrapping
**Find related to current passage** button (`tagUseCurrent`, calling `useCurrent()`)
explicitly captures the reader's new range, clears search, and resets selected tags
from its overlaps while retaining the Any/All and version choices.
Peer refreshes keep the chosen filters except for deleted IDs; they do not silently
replace the pinned source or reselect its tag union.

**Manage** in the modal picker opens global management with Back navigation.
The separate Commands → **Manage tags** entry (`openBrowser(true)`) opens that modal
management page directly, closing Annotations. Confirmations are in the shared body.
Rename, delete, and merge each describe the global operation. Delete
includes assignments in every version; merge keeps the target name and removes
the source. A peer rename/deletion of either involved
tag invalidates the pending confirmation. Failures leave the operation available
for correction/retry and preserve saved marks and notes.

## Annotation entries, removal, and backup

- The backend collection includes tag-only ranges and deduplicates tags/marks/notes
  at exact endpoints in the active version. Annotations' Bookmarks, Highlights,
  and Notes tabs filter by type; Tags summarizes exact/overlapping tags and related
  passages. Main deletes through captured `deleteAnnotationScope(scope)`;
  `deleteAnnotation(reference, expectedVersionId)` remains a guarded backend entry
  point. Deleting an entry removes its bookmark,
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
  `tagStart`, `tagEnd`, `continuesBefore`, `continuesAfter` metadata for Verse rails
  and flowing annotation geometry.

## Footer and range indicators

The footer puts **?** before Normal/Visual mode on the left, passage/version in
the center, and **Related**, **Annotations**, **Notes** on the right. Related uses
the shared outline left-pointing tag icon.

Bookmarks, tags, and notes use a unified kind-icon-plus-range-bracket language.
Verse dynamically packs rails into the nearest available gutter lane before the
verse number. Each kind's continuous coverage is unioned into intervals in source-row
order; numeric verse labels do not determine placement. Intervals are allocated by
start, with bookmark/tag/note priority for simultaneous starts, and keep their lane
through the whole interval. Later starts/ends cannot bend an existing rail; free
lanes are reused by subsequent intervals. Segments connect across rows/headings and
retain endpoint caps. All three kinds share `RangeMarker` and overlap choices.
Note clicks flush drafts and open the complete captured scope before Notes.

Paragraph and Block keep a continuous text layout with inline bookmark/note/tag start
and end brackets. Opening brackets have a superscript icon on their left; tag
icons are mirrored to point left. Multiple tags on one exact range share a marker
pair, while overlapping ranges keep independent scopes. Chapter continuations use
a chevron instead of a false opening and omit a closing bracket until the true end.
Markers open the bookmark range, exact tag editor, or exact note scope. Notes are
inline too, with no margin-note rail or grouped margin chooser. `AnnotationGlyph.qml`
supplies all three kinds with `filled: false` by default. Verse and flowing-reader
icons set `filled: true`; footer and other annotation controls use outlines. The
filled note is a solid sheet with a transparent pencil; its outline variant retains
the footer's pencil-in-an-open-square path. Nesting follows source-order extents,
with outer starts first, inner ends first, and reversed kind order for equal-range
closing brackets.

Reserved document cells keep decorations clear of verse text through wrapping and
relayout. Copy filters only generated decoration positions, preserving original
text and Unicode. In Paragraph/Block, while the cursor verse is inside a bookmark,
note, or tag range, subtle context shading marks its other visible members,
excluding the cursor and current
selection. Overlapping active ranges are unioned; moving outside clears the shade.
Focus mode hides the inline/row indicators and context shading, closes tag
surfaces, and hides the footer controls.

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
backup/export and rollback; `tagsui` covers the modal picker with/without an embedded
host, read-only summary, tag-seeded discovery-only Related, management, and Main
integration; `desktop` covers asynchronous rounding and shared themed tooltips.
`ui` and `annotations` also cover Visual completion, Annotations shortcut
contexts, footer order, padded Related geometry, interval-packed row rails and inline
endpoints/copy, exact note clicks, shared filled/outline glyph rendering, and cursor-context
shading for bookmark/note/tag ranges. These describe assertions present in the
test sources, not test-run results; remaining manual visual checks are listed in
the test guide.
