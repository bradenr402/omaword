# AGENTS.md

Notes for coding agents working on OmaWord, a keyboard-first Bible reader for Omarchy (C++17, Qt 6 Quick/QML, qmake): only what costs time when you don't know it. `ARCHITECTURE.md` has the internals, `tests/README.md` the suites. These are defaults: the engineer directing you can override anything here.

**Keep this file current.** Update it when a change makes it wrong or incomplete, and show the user the diff.

## Hard rules

1. **No GitHub writes** (`git push`, PRs, issues, comments, by any channel) without an explicit instruction. Reads are fine.
2. **Never touch the user's real data** in `~/.local/share/omaword/`. Tests sandbox `HOME` and every `XDG_*`; keep it so, and launch the app yourself only with throwaway ones. Offscreen, also unset `QT_QPA_PLATFORMTHEME` (Omarchy's `gtk3` theme quits without a display).
3. **Never kill OmaWord by name**; stop only a PID you started. The user may be reading in it.
4. **Never commit Bible text or local data** (AO complete downloads, `*.sqlite`, backups, exported notes; see `.gitignore`).

## Behavior

- State your assumptions; where a request reads more than one way, say so. Push back when something simpler would do.
- Write the least code that solves the problem: no speculative abstractions, no handling for impossible cases.
- Touch only what you must, in the file's own style, with no drive-by reformatting. Remove code your change leaves unused; mention older dead code instead of deleting it.
- Update the docs your change makes wrong. `README.md` stays short, in the spirit of [Omawrite's](https://github.com/omacom/omawrite/blob/master/README.md): a sentence for what a new user needs, the details left to the app. Internal contracts go in `ARCHITECTURE.md`, suites in `tests/README.md`.
- Keep agent docs concise: record durable contracts, non-obvious traps, and where to look. Replace outdated text instead of appending history. Don't duplicate code, key lists, test cases, UI walkthroughs, or benchmarks; link to their source. Suite descriptions stay one short sentence each.
- If evidence contradicts something you said, retract it plainly.

## Dev environment

- Omarchy (Hyprland on Arch) only; dependencies are in README's Requirements.
- `bin/build` builds out of tree into `build/` (`BUILD_DIR`, `JOBS`); never run qmake in the source tree. The code is in two static libraries, `src/src.pro` (all C++ but `main.cpp` and the QML's types) and `qml/qml.pro` (`resources.qrc`, compiled ahead of time, and the `OmaWord.Reader` types): a new C++ source goes in one of them, never in a test project.
- A new QML or JS file must be listed in `resources.qrc` or it won't load. Data the C++ core reads goes in `src/core.qrc`, so suites linking only the core have it.
- Use `Brand::` (from `branding.env`) for the app's name and storage ID, never a literal.
- Personal DB, text DB, and backup format versions are in `ARCHITECTURE.md`; bumping one needs a migration, tests, and an update there.
- The user runs the installed app (`~/.local/bin/omaword`), not your builds. Once a feature, or a group that belongs together, passes its suites, run `bin/build` and `bin/install` and say so; install a fix the user is waiting on at once. OmaWord is single-instance (a launch opens a window in the running process) and runs its old build until every window, on every workspace, is closed: never stop it yourself, and pass on `bin/install`'s note naming a process still on the previous build.

## Tests

- `bin/test` builds the libraries once, then runs every suite offscreen in a sandboxed home, logging to `build/test-logs/<suite>.log`. `--suite <name>` (repeatable) selects; test function names need exactly one suite (`bin/test --suite ranges noteConfirmationAndDestinationDrift`). While it runs, build elsewhere (`BUILD_DIR=/tmp/...`).
- Run the suites your change touches, then all of them before calling work done: CI runs only on GitHub (`.github/workflows/ci.yml`).
- A new suite needs `tests/<name>.pro` (with `../src/core.pri`, and `../qml/qml.pri` for QML), entries in `bin/test` and in both project lists of `tests/test-runner.sh`, a CI shard, and rows in `tests/README.md`.
- Fixtures are small, original AO standard JSON made in temporary directories: never real Bible text or the network.
- Never fix a flaky test with a longer sleep: find the condition it should wait for (motion tests step their animations).

## Keybindings

Every action is in the `actions` catalog in `qml/Main.qml`, which feeds Keybindings (`?`), Commands, and the README.

- After changing it, run `bin/shortcuts` to regenerate README's full key list (never edit it by hand; CI checks), and keep README's hand-written Shortcuts list current.
- Show keys through `qml/KeyNotation.js` (`shift G`, `ctrl k`, `↑`); `bin/shortcuts` mirrors it.
- Conventions (the catalog has the exceptions):
  - single letters for common actions, Vim-style: `j` `k` move, `x` highlight, `w` note, `y` copy, `u` undo;
  - `g` + letter goes somewhere or reaches back (`gb` next book, `gm` next mark, `gv` earlier selection);
  - `shift` + letter opens a panel (`shift M` Marks) or the letter's bigger variant (`shift Y` plain copy);
  - `ctrl` + letter opens a modal (`ctrl k` palette), besides desktop and Vim meanings (`ctrl c`, `ctrl d`); `ctrl shift` + letter takes away what the letter puts there (`ctrl shift D` Clean Desk);
  - `alt` + letter toggles a setting (`alt b` whole book), and `alt shift` + it restores the default;
  - `esc` dismisses, `enter` performs, `backspace` steps back, `delete` removes.
- The reader's `z` and `g` are prefixes (`zPending`, `gPending`): a catalog sequence `"G, X"` is bound as `X` while `g` waits. Never add multi-key Qt sequences to the reader.
- A key with another meaning in a context (lists, notes, character selection) gets its own catalog entry, the context in its title. Other apps' keys (`F1`, `ctrl +`) go in `aliases` with an `aliasReason`.
- Key hints go through `qml/HintRow.qml`, clickable where they act. A key that cannot act calls `rejected(reason)`: the error sound and a toast saying why (`unavailableReason` for catalog actions).

## UI conventions

- Follow the live Omarchy theme, font, and corner radius (`src/desktop.*`); never hardcode colors, fonts, or radii.
- Reuse the shared pieces: `HintRow`, `AppContextMenu` with `RowMenu.js` (every list row gets the same menu), `AppToolTip`, `ExitGhost`, `PanelHeader`, `PanelTabs`, `ModeHeader` (a selection mode's keys), and Main's controls (`QuietButton`, `ModalCard`, …). A side panel is a mode of the Margin (`marginMode`): a `SidePanel` under a `SidePanelHeader`; a modal is a `ModalCard`; each has a `HintRow`, and `tst_ui`'s `panelsAndModalsShareTheirShells` checks the details.
- A QML file that needs the window takes `controller` and `theme`, which Main binds once in a same-named inline component. Inside one, a property named like an outer id binds to itself (`app: app` is undefined): name it differently.
- Everything works by keyboard and by mouse (`a11yui` checks), and respects Disable animations and Disable sound effects.

## Code style

- C++17 with Qt idioms: `QStringLiteral`, `auto` for obvious types, `m_` members, early returns, function braces on their own line. QML: 4-space indent, logic in small functions or `.js` libraries (pure JS stays testable). Match the file.
- A C++ list or map property is a live reference in QML: rows kept from it read it again later, and each element read reads the property again (`Array.from` over a list its getter builds rebuilds it once per element). Copy the values you keep.
- Comments explain why, never what. Fail loudly at boundaries (import, catalog, backups); don't swallow errors.
- Keep scratch scripts, notes, and screenshots out of the repo; `screenshots/` holds only README images.

## Git & commits

- Small commits, each a working state, with imperative, capitalized titles that describe the user-visible behavior (they may run long), no prefix or period. Wrap code in backticks.
- End every Git commit message with a `Co-authored by:` trailer naming the model you run as (`Co-authored by: Claude Opus 5.5 <noreply@anthropic.com>`).
