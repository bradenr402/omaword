# OmaWord tests

## Running

`bin/test` builds (incrementally) and runs every suite. It first builds the two
libraries every suite links (`src/core.pri`, `qml/qml.pri`) once, with all
jobs; the suites then compile only their own test file. Select suites with
`--suite <selector>`; repeat it for several. `all` or no selector runs all;
duplicates run once.

```sh
bin/test                                   # everything
bin/test --suite tags --suite tagsui       # two complete suites
bin/test --suite ranges noteConfirmationAndDestinationDrift   # one function
bin/test --suite tags -functions           # list functions
```

Qt arguments (function names, data-row filters, `-functions`) require exactly
one selected suite. GUI suites run offscreen with software rendering. Each
suite reports status/timing; full output goes to `build/test-logs/<selector>.log`.

| Selector | Project | Build directory | Executable |
| --- | --- | --- | --- |
| `backend` | `tests/backend.pro` | `build/tests/` | `tst_backend` |
| `ui` | `tests/ui.pro` | `build/ui-tests/` | `tst_ui` |
| `search` | `tests/search.pro` | `build/search-tests/` | `tst_search` |
| `searchui` | `tests/searchui.pro` | `build/searchui-tests/` | `tst_searchui` |
| `deskui` | `tests/deskui.pro` | `build/deskui-tests/` | `tst_deskui` |
| `historyui` | `tests/historyui.pro` | `build/historyui-tests/` | `tst_historyui` |
| `studyui` | `tests/studyui.pro` | `build/studyui-tests/` | `tst_studyui` |
| `a11yui` | `tests/a11yui.pro` | `build/a11yui-tests/` | `tst_a11yui` |
| `marginsui` | `tests/marginsui.pro` | `build/marginsui-tests/` | `tst_marginsui` |
| `motion` | `tests/motion.pro` | `build/motion-tests/` | `tst_motion` |
| `contextmenu` | `tests/contextmenu.pro` | `build/contextmenu-tests/` | `tst_contextmenu` |
| `searchrender` | `tests/searchrender.pro` | `build/searchrender-tests/` | `tst_searchrender` |
| `tags` | `tests/tags.pro` | `build/tags-tests/` | `tst_tags` |
| `tagsui` | `tests/tagsui.pro` | `build/tagsui-tests/` | `tst_tagsui` |
| `ranges` | `tests/ranges.pro` | `build/ranges-tests/` | `tst_ranges` |
| `sound` | `tests/sound.pro` | `build/sound-tests/` | `tst_sound` |
| `desktop` | `tests/desktop.pro` | `build/desktop-tests/` | `tst_desktop` |
| `importer` | `tests/usfm.pro` | `build/usfm-tests/` | `tst_usfm` |
| `catalog` | `tests/catalog.pro` | `build/catalog-tests/` | `tst_catalog` |
| `annotations` | `tests/blockannotations.pro` | `build/blockannotations/` | `tst_blockannotations` |
| `instance` | `tests/singleinstance.pro` | `build/singleinstance-tests/` | `tst_singleinstance` |
| `windows` | `tests/windows.pro` | `build/windows-tests/` | `tst_windows` |

## Environment

- `JOBS`: total Make jobs (default: CPU count).
- `TEST_JOBS`: concurrent suites (default 2). Clamped to the selected suites and
  `JOBS`; each worker gets `floor(JOBS / concurrency)` jobs. Both must be
  positive integers.
- `BUILD_DIR`: overrides the build root.
- `OMAWORD_TEST_USFM_DIR`: directory with the seven real source ZIPs
  (`engnet_usfm.zip`, `eng-kjv2006_usfm.zip`, `eng-kjv_usfm.zip`,
  `engojb_usfm.zip`, `engylt_usfm.zip`, `eng-asv_usfm.zip`, `engbsb_usfm.zip`).
  Enables `importer` `actualArchives` and backend `optionalSourceArchives`;
  skipped when unset, and every file must exist when set. Archives are copied
  into temporary data dirs and never modified or installed.
  Single-file overrides: `OMAWORD_TEST_NET_ZIP`, `OMAWORD_TEST_KJV_ZIP`
  (backend), `OMAWORD_TEST_KJV` (UI). `OMAWORD_TEST_APP` selects a built
  executable for optional real-launch checks.
- `OMAWORD_SOUND=0|off|false`: silences sound effects (offscreen runs never
  open an audio device).

Low-impact example: `JOBS=2 TEST_JOBS=1 nice -n 10 bin/test --suite search`.

## Fixtures

Suites generate small, original USFM text in temporary application-data
directories (sparse chapters, omitted verses, noncanonical source order such as
ESG/GEN/TOB, cross-book ranges). No whole Bible or network download is needed:
catalog tests use fake replies and a local HTTP server, and desktop tests inject
a fake `hyprctl`, a fake Hyprland event socket, and temporary theme and
fontconfig state.

## Suites

- **importer**: USFM parsing, metadata, source order, poetry line levels (schema 5), malformed/unsafe input, staged publication.
- **catalog**: catalog metadata, streamed atomic downloads, cancel/timeout/retry.
- **backend**: library, references, annotations, undo/redo (exact rows back, note sessions, created tags, other windows' changes, other versions), comparing versions, reading the next chapter ahead, backups, migrations (schema 9: bookmarks become the Bookmarked tag, times), peers, History, the ribbon, file ownership, character-selection motions/objects and the quotation format.
- **ui**: packaged Main: catalog, layouts (the 66ch column, the type scale, poetry indents, the next-chapter link, `ctrl` + wheel zoom), the Aa popover, footer folding, selection/copy, Annotations (immediate delete, undo from the panel), undo keys, toasts and Commands, Visual completion, footer.
- **search**: matching semantics (incl. chapter find's smart-case regular expressions), results in Bible order, the two-version corpus cache, async lifecycle, whitespace-preserving copy.
- **searchui**: palette and chapter-find keyboard behavior; Bible search results under book headings with counts, `ctrl ↓`/`ctrl ↑` book jumps across pages.
- **searchrender**: rendered match offsets/geometry across layouts.
- **deskui**: Desk add/remove, panel, peers, version removal.
- **studyui**: words-of-Jesus coloring, Re-import offers, comparing versions (`c`, text per version, copy, switch, "Not in this version"), Study panel tabs (Related, Cross-references, Footnotes), reader footnote markers and their peek (click, `i`, stepping, copy, Study).
- **a11yui**: keyboard and mouse access (accessible names and key descriptions, error and selection colors): button keys and focus, clickable key hints, shared row menus, scroll keys, outside clicks, Move here from marker menus, search paging, Re-import key, error banner, Notes Linked from, focus-mode footer, chapter-find clicks and tab, mouse back/forward, keeping text selections; Phase 3: character selection (`v`) in every layout and quotation copies of selected text, Manage tags keys, Annotations row focus, Notes `alt v` and web links, Add version guide keys, footnote markers and hit areas, slider arrows, Versions backspace, side-panel key hand-off, passage menu steps and match counter, double clicks, Study chip menus, margin note menus, PRIMARY selection, scrollbar gutters; Phase 4: outline and margin note tooltips, focus after restore cancel / note conflict / setup, Study esc, Versions caret keys, Help titles, palette tab hint, switch and TagButton focus outlines, pointing hands and hovered `[[` suggestions, Block verse-number tooltips, `ctrl click` note links, chapter title → chapter list; Phase 5: regex chapter find and Reading settings (Layout, icons, rule, keys, focus order); re-audit fixes: character selection keeps its menu and handles every key, right clicks off a selection, Keybindings rows for note keys, confirmation hints, Notes tab chain and non-interrupting conflict keys, keyboard focus after self-hiding buttons, outline keys, Study chips and rows, list home/end, text-field menus, scrolling context menus, footer hover, marker and Versions menus, mouse back in dialogs; batch 6: chapter edge buttons, highlighter look (light/dark, all layouts, Highlights tab), Notes editor keeps the keyboard (`tab`, `ctrl ↓`), character selection across relayouts, Reading settings rules.
- **marginsui**: chapter outline, `{`/`}` heading motions, margin notes (placement, truncation, connectors, reader shift, links, drawer), focus mode transitions with margins (frame by frame under a stepped animation driver: column glide, margin fades, anchored reading position, one rebuild, rapid toggles, Disable animations; `OMAWORD_FOCUS_TIMELINE=<dir>` writes the timelines and frames), and the pure `Margins.js`/`HandDrawn.js` functions.
- **historyui**: jump stack (`[` `]` `-`), dwell recording, History in the palette (recents, All History, `ctrl h`, day groups, filter, remove and clear keys, row menu), the ribbon (`shift R`, `r`, tab, footer, Continue row, another version), two windows.
- **motion**: focus dimming, jump pulses, copy flashes, held-key repeat, instant menus/modals with exit ghosts, side-panel enter/exit, hover colors, reduce motion.
- **contextmenu**: shared context menu items, keyboard navigation, actions, the mouse selection bar in every layout.
- **tags**: tag identity, exact/overlap queries, persistence, backup (older backups' bookmarks restore as the Bookmarked tag), rollback.
- **tagsui**: picker, Annotations summary, Related, management, Main integration.
- **ranges**: atomic range moves, collisions, stale tokens, rollback, undoing a move.
- **annotations**: flowing-layout brackets, hand-drawn marks, copy, hit testing, annotation tints.
- **desktop**: async compositor rounding and `animations:enabled` (re-asked on Hyprland config reloads, never polled), readable secondary text, error and selection colors, Omarchy theme swaps and font changes followed by events, themed tooltips.
- **sound**: sound effects and settings.
- **windows**: multiple readers, shared commits, draft conflicts, closing.
- **instance**: single-instance lock, handoff, IPC bounds, stale recovery.

`tests/test-runner.sh` checks `bin/test` itself (selection, argument
forwarding, concurrency, logs, failure propagation) with fake tools.
