# Annotation range editing

Move selected components of an existing exact annotation range by visually
adjusting its endpoints in the reader. This documents the current working-tree
implementation in `qml/RangeEditor.qml`, `qml/Main.qml`, and `src/biblebackend.*`.

## User decisions and workflow

1. Start with `Shift E` or Commands → **Edit annotation range**. If saved ranges
   intersect the current selection, choose the exact range to edit; overlapping
   ranges stay separate. With no matching annotation, the reader reports
   **No annotations on this range**.
2. If only one annotation type exists on that range, proceed directly to endpoint
   editing with that type selected (including all tags when tags are the only type).
   Otherwise, choose from the components actually present: bookmark, highlight,
   note, and individual tags. General entry preselects all; a marker or Annotations
   tab preselects its kind. `j`/`k` or arrow keys select a row and Space toggles it. `m`, `x`,
   `n`, and `t` toggle bookmark, highlight, note, and all tags respectively. `a`
   switches between **Select all components** and **Deselect all components**.
   Enter continues with a nonempty selection; Esc cancels. The footer lists only
   shortcuts relevant to the present annotation types.

Both chooser states use a prominent title, padded list, quiet action buttons, and
a separated keyboard-hint footer. The exact-range chooser explains that overlapping
annotations can have different boundaries. Component rows show their shortcuts
inline; tag rows identify that `t` toggles all tags together.
3. Extend, shrink, or relocate the range using verse motions. `j` / `k`, Up/Down,
   counts such as `2j`, `J` / `K`, Shift+Up/Down, Ctrl+u/d, Ctrl+b/f, and
   PageUp/PageDown adjust the active endpoint. Chapter-edge motions and verse
   clicks also adjust it. `o` swaps the active endpoint; move each endpoint to
   relocate the whole range. The banner shows the source, proposed destination,
   source version, and active endpoint. The endpoint label updates with movement
   and swaps, including single-verse selections and chapter crossings.
4. Enter previews and saves. Esc requests cancellation of the entire session,
   including from either chooser or note confirmation. When the original version
   is still active and installed, it locally restores the original selection and
   direction, Visual state, and reading position, including an unselected chapter.
   Restoration does not flush peer drafts. If its reading-state SQL save fails,
   the session, original snapshot, and proposed selection remain available; the
   error prompts **Press Esc to retry cancellation.** Window closing is also
   vetoed while cancellation remains incomplete.

Movement follows actual verse keys, including omissions, across chapters and
source-ordered books **within the same installed version**. The source version is
pinned during editing; an explicit entry from another version opens that source
before snapshotting. Cancellation does not switch versions: if the original
version is no longer active/installed or the reader is not ready, it ends the
session and reports that the original reading version is no longer active.
Removing the pinned source version automatically ends the edit and closes its
dialogs, even when restoration is impossible, so the reader is not left trapped
in edit mode. It attempts local restoration only if the original version is
still active and installed.

This is a temporary range-edit mode: verse motions extend even though ordinary
Visual mode is off. Main closes Notes, tag surfaces, and Annotations on entry and
guards annotation mutations, passage/chapter jumps, imports, restore, removal,
and version switching. Copying, text-size changes, and opening an independent
window remain available outside modal choosers. A successful save leaves the
destination selected in Normal mode. A failed save retains the proposed range
and active edit with an error; retry is possible after a transient failure. A
stale source must be chosen again by canceling and reopening.

### Entry points

- **Annotations → Bookmarks / Highlights / Notes:** row **Edit range**, or
  `Shift E` on the focused row (falling back to the current list item).
- **Reader bookmark, note, and tag markers:** right-click context menu → **Open** / **Edit range**,
  or Shift-click to edit directly, in Verse, Paragraph, and Block layouts. Verse
  markers sharing coverage pass their exact choices to the chooser. Ordinary
  left-click keeps the marker's Open behavior; the verse-text context menu and
  middle-click copy remain separate from marker actions.
- **Annotations → Tags:** **Edit range** on exact tagged scopes, overlapping
  ranges, and related rows; `Shift E` uses the focused row's scope or the summary
  scope. This is an explicit new requirement allowing range-edit entry from the
  summary. Tag assignment toggles/creation still belong to the modal `t` picker;
  global name management remains separate.

## Collision and persistence contract

Only selected components move between the two exact endpoint records. Unselected
source components and other overlapping ranges keep their own assignments.
Destination bookmarks/highlights union, and duplicate tag IDs deduplicate.
Moving tags does not rename or delete global definitions.

If a moved note meets a different nonempty destination note, **Combine notes?**
shows both as plain text. **Combine and save** produces exactly:

```text
destination note

source note
```

An empty destination accepts the source note; identical notes deduplicate without
confirmation. Combined text must fit the backend note-size limit. Canceling makes
no range mutation. Saving to the same exact range is a validated no-op, without
draft flushing, writes, or publication.

Source snapshots include every exact component, tag IDs/names, canonical
endpoints/source ID, and a SHA-256 token tied to the immutable installed database
generation. Display references are not identity. Save flushes shared drafts,
then re-reads and validates the source inside the transaction, rejecting altered
tokens or mismatched caller-supplied component data. Even a change to an unselected source
component invalidates the snapshot; replacement/reimport also invalidates it.

The UI passes the destination token from every preview. Save rejects destination
drift, including marks/tags as well as note text; note combining requires both
explicit consent and a matching nonempty token. A stale combine confirmation
closes with an error and leaves endpoint editing active, preserving the newer
destination note. Enter can preview it again.

Source/destination annotation rows and selected tag assignments are changed in
one transaction, read back, and committed before publication to readers. Invalid
scopes/components, removed versions, busy operations, draft vetoes, or SQL,
read-back, and commit failures cannot partially move annotations. Shared draft
saves that precede validation remain their own commits. The backend move does
not navigate readers or change their independent selections/scroll positions.

## Backend and QML API

Scopes are `{versionId,reference,book,chapter,start,endBook,endChapter,end}`.
Range-edit APIs require the active installed source and valid integer endpoints
ordered by that source's actual keys. Components are
`{bookmark:bool,highlight:bool,note:bool,tags:[ids]}`; selected components must exist
on the exact source, and at least one must be chosen.

- `QVariantList annotationRanges(QVariantMap aroundScope)` returns the exact saved
  intersecting ranges, including tag-only records, sorted by source start/end.
  Rows include the scope, bookmark/highlight flags, note, and `{id,name}` tags.
- `QVariantMap annotationSnapshot(QVariantMap scope)` returns
  `{ok,error,exists,scope,bookmark,highlight,note,tags,token}` on a valid read;
  an empty exact source is not a successful annotation snapshot.
- `QVariantMap previewRangeChange(QVariantMap sourceSnapshot, QVariantMap destination,
  QVariantMap components)` returns `{ok,error,needsNoteMerge,sourceNote,
  destinationNote,destinationToken,combinedNote,message}` without writing,
  navigating, or changing `bible.error`.
- `bool changeAnnotationRange(QVariantMap sourceSnapshot, QVariantMap destination,
  QVariantMap components, bool combineNotes=false, QString destinationToken=QString())`
  performs the atomic move; failures set `bible.error`. The token is optional for
  direct nonconflicting backend calls, checked for moves whenever supplied, and
  mandatory for combining different notes. The same-range no-op validates the
  source and choices without checking the destination token. The UI always
  supplies its preview token.
- `bool swapSelectionEnds()` exchanges anchor/cursor and displays the newly
  active endpoint's chapter, preserving the exact range while reversing direction.
- `bool restoreSelection(QVariantMap scope, bool reversed=false, double position=-1)`
  restores local reading state in the active installed version without flushing
  peer drafts or switching versions. Selected scopes use canonical endpoints;
  an unselected chapter uses `{versionId,book,chapter,start:0,end:0}`. Position is
  a finite value in `[0,1]`, or `-1` to retain the current position. It validates
  the scope and persists reading state through `goToSelection()` before updating
  the reader; failure sets `bible.error` and preserves the current selection.

`RangeEditor.qml` requires `app`, `bible`, and `desktop`. `open(scope=null, kind="")`
accepts a scope, an array of exact choices, or the current selection's intersecting
ranges; `selectAll()` selects every available source component, and `cancel()`
attempts local restoration as described above. `sessionOpen` spans the
whole session, `active` means endpoint editing, and `visibleModal` covers choosers/
confirmation. Source and original-state snapshots are deep-copied and frozen.
`finished(bool saved)` reports completion; Main owns the banner and key routing.
The endpoint binding explicitly reads selection, cursor, book, and chapter
properties alongside `tagScope()` so QML tracks changes; `coincidentEndpoint`
records which endpoint is active when both coincide. `pinnedVersion` and
`versionsChanged` drive removed-source cleanup; `restoring` prevents reentrant
cancellation. Custom `RangeButton` / `RangeCheckBox` controls use desktop theme
colors, font, corners, hover/focus/disabled states, and plain wrapping labels.
Control font/padding scale is capped at 1.4; the select-all button has a themed
tooltip, and buttons explicitly handle Enter/Return.

## Reader refinements

- Filled notes use a folded sheet, two cutout text lines, and a separate solid
  pencil, with three-times supersampling for small glyphs. Outline controls retain
  the pencil-in-an-open-square path.
- Opening Notes for editing, or explicitly focusing an already-open editor, puts
  the cursor at the end. Synchronization and autosave do not repeatedly move it.
- Annotations `h` / `l` and Left/Right tab switching works from Tags; `j` / `k`
  and Up/Down navigate items/summary buttons. Editable fields retain text navigation.
- Inline brackets use endpoint font/ink metrics: one pixel above the letter top
  and below the baseline/ink, independent of proportional line height. Baseline
  cells reserve font height plus two pixels, and generated joiners avoid taller
  fallback fonts. Copy still omits only generated decorations.

## Verification map

`bin/test` includes `ranges` and `rangesui` in `all`, with individual or repeated
`--suite` selection. Backend assertions cover exact/subset moves, sparse source
order and cross-book ranges, collisions, stale snapshots/tokens/generations,
drafts, peers, no-ops, and rollback. Packaged-Main UI assertions cover component
choices/select-all, overlap selection, reactive endpoint labels, local cancellation
without peer flushes, cancellation SQL failure/retry, removed-source cleanup, note confirmation,
failed-save retention, marker/Annotations/Tags-summary entry, note cursor behavior,
and tab navigation. `annotations` adds marker-menu and font-metric bracket checks;
`ui` adds note/marker/tab integration checks. See [tests/README.md](tests/README.md)
for projects and targeted cases. These describe source assertions; no tests or
builds were run for this documentation update.
