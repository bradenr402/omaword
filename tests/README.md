# OmaWord tests

## Run

```sh
bin/test                                       # all suites
bin/test --suite tags --suite tagsui           # selected suites
bin/test --suite ranges noteConfirmationAndDestinationDrift
bin/test --suite ui -functions                 # discover test functions
```

Qt arguments (functions, data-row filters, `-functions`) require exactly one
suite. `bin/test` builds the shared libraries once, then each suite; GUI tests
run offscreen with software rendering. Logs: `$BUILD_DIR/test-logs/<suite>.log`
(`build/` by default). `bin/test` is the source of truth for build paths.

## Suite map

| Selector | Coverage / source when naming differs |
| --- | --- |
| `backend` | Persistence, references, annotations, migrations, undo, export/backup, and source-text selection |
| `ui` | Packaged Main integration: reader layouts, navigation, panels, palettes, settings, and action catalog |
| `search` | Query semantics, offset normalization, index correctness, and asynchronous lifecycle |
| `searchui` | Bible-search palette and chapter-find interaction |
| `searchrender` | Rendered match geometry and source/document mapping |
| `deskui` | Shared Desk editing, conflicts, quotations, and Notes/Desk Markdown previews |
| `historyui` | Jump stack, persisted History, ribbon, and multi-window behavior |
| `studyui` | Comparison, OpenBible See also, footnote peeks, words of Jesus, and explicit replacement |
| `a11yui` | Keyboard/mouse parity, focus hand-off, hints, and character selection |
| `marginsui` | Outline, margin geometry, dock/focus transitions, and panel-open selection centering |
| `motion` | Stepped animations, pulses, dimming, repeat keys, and reduced motion |
| `contextmenu` | Shared menus, actions, keyboard navigation, and verse selection bar |
| `tags` | Tag identity, queries, persistence, backups, and rollback |
| `tagsui` | Tag picker/management and related Main integration |
| `ranges` | Atomic range moves, collision confirmation, stale tokens, rollback, and undo |
| `sound` | Sound cues, throttling, and preferences |
| `desktop` | Theme/font events, compositor settings, colors, and tooltips |
| `importer` | AO standard JSON, metadata, offsets, validation, and staged publication (`tst_ao.cpp`, `usfm.pro`) |
| `catalog` | Catalog validation, streamed downloads, cancellation, timeout, and retry |
| `annotations` | Flowing annotation paint, geometry, hit tests, and copy mapping (`tst_blockannotations.cpp`, `blockannotations.pro`) |
| `instance` | Lock/IPC bounds, hand-off, stale recovery, and executable replacement (`tst_singleinstance.cpp`, `singleinstance.pro`) |
| `windows` | Reader isolation, peer changes, Follow, lookup, request routing, and offline QML engine |
| `cli` | Headless commands, output/JSON, terminal escaping, and GUI request parsing |

Other suites use `tests/<selector>.pro` and `tests/tst_<selector>.cpp`.
`bash tests/test-runner.sh` checks the runner itself with fake tools.

## Environment and fixtures

- `BUILD_DIR`: build/log root; `JOBS`: total compiler jobs; `TEST_JOBS`:
  concurrent suites (default 2, bounded by selected suites and `JOBS`). Avoid
  heavy parallel work during motion/UI tests.
- The runner sandboxes `HOME` and XDG paths and unsets `QT_QPA_PLATFORMTHEME`.
  Never run against the user's app data. Fixtures are small original AO JSON in
  temporary directories; network behavior uses fake replies/local servers.
- `OMAWORD_TEST_APP=<built executable>` enables real-launch IPC/CLI checks.
- Optional source checks: `OMAWORD_TEST_AO_DIR` contains explicitly supplied
  `<id>_complete.json` files for the seven migration targets; ordinary runs use
  only synthetic text, never the network.
- `OMAWORD_TEST_AO_MIGRATION_SCRIPT=<explicit path>` enables the backend's
  synthetic one-off conversion/install/backup scenario; the script stays outside runtime.
- Offscreen tests never open audio devices; `OMAWORD_SOUND=0` also disables sound.
- Visual diagnostics: `OMAWORD_A11Y_SHOTS=<dir>` saves accessibility screenshots;
  `OMAWORD_FOCUS_TIMELINE=<dir>` saves focus-transition timelines/frames.
- Some geometry suites load QML directly from the source tree. Don't edit QML
  during a run. Wait for layout conditions rather than extending sleeps;
  motion tests step animation time explicitly.

New-suite registration and required checks are in [AGENTS.md](../AGENTS.md).
Behavior contracts and format versions are in [ARCHITECTURE.md](../ARCHITECTURE.md).
