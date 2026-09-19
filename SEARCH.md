# Offline Scripture search

## Entry points

- `s` or the footer search icon opens the palette with `/ ` prefilled for text search.
- `Ctrl k` or the current-passage button opens reference navigation.
- `>` selects commands; Space resumes the last palette query, row, and scroll position.
- `/` in the reader opens a temporary bottom command line in place of the footer.
- `n` / `N` navigates accepted matches; counts such as `3n` work without wrapping.
- `w` writes a note. `Ctrl n` still toggles Notes.

Passage, Search, and Command occupy equal-width tabs below the palette input.
Tab cycles them in that order; Shift Tab cycles backward and keeps input focus.
Typing a bare `/` or `>` appends a space; the recognized prefix is dimmed.
Backspace removes an empty prefix and returns to Passage mode.
Result matches use accent-colored boxes with an accent border.

Search is offline, Scripture-only, and limited to the active version, including
all its books. Each window owns its query, results, and current match.

## Matching

Both searches ignore case and collapse whitespace for matching, including line
and paragraph breaks. Partial words match by default. Punctuation and accents
remain significant. Headings, verse numbers, notes, tags, and inline annotation
decorations are never searchable Scripture.

Bible queries use AND: `love neighbor` requires both terms in one verse. A quoted
phrase such as `"love your neighbor"` can cross adjacent verses within a chapter.
For mixed queries, quoted phrase occurrences anchor the smallest matching verse
range; all unquoted terms must occur within that range. Multiple quoted phrases
must all occur in the minimal range. Queries never cross chapter boundaries.
Unfinished quotation marks produce an error; escaped quotes/backslashes are
supported. Queries are limited to 4096 UTF-16 units.

Results contain the full matching verse or range, with matched text emphasized.
Relevance favors exact words, closer terms, and shorter ranges, with source Bible
order breaking ties. Results load in batches of 100. Enter opens and selects the
result range. Space restores the palette.

Chapter find treats the entire input as literal consecutive text, including
quotation marks. Previous chapter-find text is selected when the prompt opens.
Preview begins at the focused verse and never falls back to earlier matches.
Typing scrolls the preview without modifying the annotation selection. Enter
accepts and focuses its verse/range. When matches exist only before the cursor,
Enter accepts the first match in the chapter; this does not change the nonwrapping
behavior of `n` / `N`. With no matches anywhere, the prompt stays open.
Escape restores the prior viewport and search state.

Opening `/` from Visual mode first collapses the range to its active endpoint and
enters Normal mode. Cancelling does not restore Visual mode or that former range.

## Accepted search state

The most recently accepted Bible search or chapter find owns temporary reader
highlights and `n` / `N`. Bible searches retain their query syntax for local
navigation; chapter searches remain literal. Starting a preview temporarily
replaces those highlights; cancellation restores the prior accepted search.

Accepted queries reapply on chapter/version changes without jumping the viewport.
The accepted query appears above the footer on the left, with its counter on the right.
Counted motions stop at the first/last available match. Escape in the reader
clears search before exiting focus mode. The prompt and highlights are available
in focus mode. Editing text retains ordinary typing keys. Annotation-range
editing disables new search and search motions.

## Implementation

`ScriptureSearch` normalizes text with grapheme-safe original UTF-16 mappings.
Bible-wide scans run on Qt's worker pool with an independent read-only SQLite
connection. Request tokens and cancellation flags reject obsolete work and
version-generation results. A shared immutable corpus cache retains at most one
generation, capped at 64 MiB; per-window queries and results remain independent.

`SearchDocument` maps original offsets into rich text; `BlockDocument` also
excludes reserved inline decoration cells and verse-number labels. Canvas layers
paint inactive and active matches without changing text formats, layout,
annotation data, selections, or clipboard text. Verse, Paragraph, and Block
layouts use the same source offsets.

Copying a passage preserves stored internal whitespace in quotation and plain
text. Between verses it respects source paragraph starts, poetry lines, and
chapter boundaries, independently of the current reading layout.

## Verification

Run `JOBS=2 TEST_JOBS=1 nice -n 10 bin/test --suite search --suite searchui
--suite searchrender` for matching, asynchronous lifecycle, keyboard/palette
behavior, and rendered offset/geometry coverage. The backend suite also tests
source-whitespace copying. Tests use generated original Scripture fixtures.
