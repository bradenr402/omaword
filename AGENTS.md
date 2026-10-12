# AGENTS.md

Notes for coding agents working on OmaWord, a keyboard-first Bible reader for Omarchy (C++17, Qt 6 Quick/QML, qmake). `README.md` covers features and shortcuts, `ARCHITECTURE.md` the durable internals, `tests/README.md` the test suites; this file only covers what costs agents time when they don't know it. Treat it as defaults: the engineer directing you can override anything here.

**Keep this file current.** When your change makes anything here wrong or incomplete (commands, paths, conventions, traps), update this file in the same change. Always tell the user explicitly when you edit this file, and state EXACTLY what changed: quote each removed or replaced line and its replacement (or show the diff). Never update it silently.

## Hard rules

1. **No GitHub writes** (`git push`, PRs, issues, comments, via any channel) without an explicit instruction. Read operations are always fine.
2. **Never touch the user's real data** in `~/.local/share/omaword/` (Bible versions, annotations, Desk, History). Tests sandbox `HOME` and `XDG_*`; keep it that way, and launch the app manually only with a throwaway data dir (every `XDG_*` and `HOME`). Offscreen, also unset `QT_QPA_PLATFORMTHEME`: Omarchy's `gtk3` theme exits the app when there is no display.
3. **Never kill OmaWord processes by name**; stop only the PID you started. The user may be reading in it, and it is single-instance (`omaword.lock`).
4. **Never commit Bible content or local data**: USFM ZIPs, `*.sqlite`, backups, exported notes (see `.gitignore`). Bible text has its own copyright.

## Behavior

- State assumptions; if multiple interpretations exist, present them. Push back when a simpler approach exists.
- Minimum code that solves the problem. No speculative abstractions or error handling for impossible cases.
- Surgical changes: touch only what you must, match the style of the file you're in, no drive-by reformatting. Remove code your change made unused; mention pre-existing dead code instead of deleting it.
- If evidence contradicts something you said, retract it plainly.

## Dev environment

- Omarchy (Hyprland on Arch) only. Dependencies are in README → Requirements.
- `bin/build` builds out of tree into `build/` (`BUILD_DIR`, `JOBS` override); `bin/install` installs. Never run qmake in the source tree.
- The user runs the installed app (`~/.local/bin/omaword`), not your test builds. When a feature, or a group of related features that belong together, passes its suites, run `bin/build` and `bin/install` so the user can try it, and say so; not after every commit. Install fixes the user is waiting on at once. A running OmaWord keeps the old binary until the user quits it and relaunches (it is single-instance: a launch opens a window in the running process); never stop it yourself. Quitting means closing every OmaWord window, on every workspace; `bin/install` names a process still running the previous build, so pass that on.
- `omaword.pro` builds two static libraries and the app: `src/src.pro` (every C++ source but `main.cpp` and the QML's own types), `qml/qml.pro` (`resources.qrc`, QML compiled ahead of time, plus the `OmaWord.Reader` types), and `app/app.pro`. A new C++ source goes in one of those two `.pro` files, not in the test projects.
- QML is embedded via `resources.qrc` and compiled at build time: a new QML/JS file must be added there or it won't load. Data the C++ core reads itself goes in `src/core.qrc` instead (registered by `Q_INIT_RESOURCE(core)`), so suites that link only the core have it.
- `branding.env` feeds `src/brand.h.in` and the desktop file; use `Brand::` rather than hardcoding the app name or storage ID.
- Personal DB schema (`PRAGMA user_version`), text DB schema, and backup format versions are documented in `ARCHITECTURE.md`. Bumping one needs a migration, tests, and an `ARCHITECTURE.md` update.

## Tests

- `bin/test` builds the two libraries once, then builds and runs every suite (offscreen, software rendering, sandboxed home, sound off). Select with `--suite <name>` (repeatable); Qt args such as a function name need exactly one suite: `bin/test --suite ranges noteConfirmationAndDestinationDrift`. While a run is going, build elsewhere (`BUILD_DIR=/tmp/...`): its suites link the libraries in `build/`.
- Full output lands in `build/test-logs/<suite>.log`.
- Run the suites your change touches, then the rest before calling work done. CI runs `bin/shortcuts --check`, `bin/build`, `bash tests/test-runner.sh`, and the fast suites (`backend search tags ranges importer catalog sound cli`) in one job, and the GUI suites in parallel shards (`tst_ui` split three ways by function); a new suite must be added to a shard in `.github/workflows/ci.yml`. CI only runs on GitHub, so run everything locally before calling work done.
- A new suite needs a `tests/<name>.pro` (include `../src/core.pri`, and `../qml/qml.pri` if it loads QML or the reader types), entries in `bin/test` (usage, `available`, the `case`, `projects`, `directories`) and in both project lists of `tests/test-runner.sh`, a place in a CI shard, and rows in `tests/README.md`.
- Fixtures are small, original USFM generated in temporary dirs. Never add real Bible text or network access to tests.
- Motion/timing tests drive animations deterministically; don't "fix" flakes with longer sleeps — find the missing settle condition.

## Keybindings

All actions live in the `actions` catalog in `qml/Main.qml` (`id`, `group`, `title`, `sequences`, optional `aliases`/`aliasReason`, `command`, `explanation`). It feeds the in-app Keybindings list (`?`), the command palette, and README.

- **After changing the catalog, run `bin/shortcuts`** to regenerate README's shortcut section (between the `shortcuts:` markers). Never edit that section by hand; CI fails on `--check`.
- Display every key through `qml/KeyNotation.js` (lowercase modifiers, `shift G`, `ctrl k`, arrows as `↑`). `bin/shortcuts` mirrors it; keep them in sync.
- Conventions (with deliberate exceptions; check the catalog for prior art before choosing):
  - **Single letters for common actions**, Vim-flavored: `j`/`k`/`h`/`l` move, `y` copies, `x` highlight, `w` note, `t` tag, `d` Desk, `f` focus, `/` find, `n`/`shift N` next/previous match, `u` undo. Multi-key sequences follow Vim (`zz`, `gg` and `20gg`, `[[`); a key after a lone `g` is spent, as in Vim, leaving `g` + key free for more.
  - **`shift` + letter opens panels** (side drawers): `shift M` Marks, `shift S` See also, `shift D` Desk. Shift also gives the "bigger" variant of a letter: `shift Y` plain-text copy, `shift G` last, `shift J`/`shift K` extend, `shift V` verse visual, `shift R` places the ribbon that `r` returns to, `shift C` opens a version beside (the bigger Compare).
  - **`ctrl` + letter opens modals** (palettes and dialogs): `ctrl k` passage palette, `ctrl h` History (in the palette), `ctrl space` commands, `ctrl ,` Reading settings, `ctrl v` versions, `ctrl t` Manage tags, `ctrl /` Bible search. Ctrl also keeps standard desktop meanings (`ctrl c`, `ctrl s`, `ctrl z`/`ctrl shift Z`, `ctrl =`/`ctrl -`/`ctrl 0`, `ctrl n` new window) and Vim's (`ctrl d`/`u`/`f`/`b` scrolling, `ctrl r` redo, `ctrl w` between the text and the open side panel). `ctrl shift` + letter takes away what the letter puts there: `ctrl shift D` Clean Desk, `ctrl shift R` removes the ribbon.
  - **`alt` + letter toggles a setting**: `alt n` verse numbers, `alt h` headings, `alt o` outline, `alt w` margin notes, `alt j` words of Jesus, `alt s` sounds, `alt m` animations, `alt l` layout, `alt v` Write/Preview, `alt a` Bible search in every version, `alt r` Bible search by regular expression. `alt shift` + the letter restores the setting's default (`alt shift L`: Paragraph).
  - `esc` dismisses the top panel, selection, or step; `enter` performs; `backspace` steps back; `delete` removes.
- Keys are contextual: the same key can mean different things in lists, notes, or character selection; document each meaning as its own catalog entry with the context in the title.
- Conventional keys from other apps (`F1`, `ctrl shift P`, `ctrl +`) go in `aliases` with an `aliasReason`, so they work without cluttering the list.
- Every key hint shown in a panel or modal goes through `qml/HintRow.qml` and is clickable where the action works. Keys that cannot act call `rejected(reason)`: the error sound plus a toast saying why (`unavailableReason(action)` for catalog actions).

## UI conventions

- Follow the live Omarchy theme, system font, and Hyprland corner radius (`src/desktop.*`); never hardcode colors, fonts, or radii.
- Reuse shared components: `PanelHeader`, `PanelTabs`, `HintRow`, `AppContextMenu`/`RowMenu.js` (every list row has the same menu), `AppToolTip`, `ExitGhost`, and Main's controls (`QuietButton`, `FloatingButton`, `ModalCard`, `KeyScrollView`, …).
- A side panel is a mode of the Margin (`marginMode`): a `SidePanel` headed by `SidePanelHeader`, its content at the shell's `inset`, its keys in a `HintRow` (ending with Main's `panelReturnHint` and `esc` close, and only `panelAwayHints` while the reader has the keyboard, `!marginKeys`); a modal is a `ModalCard` with a `HintRow`. `tst_ui`'s `panelsAndModalsShareTheirShells` checks both.
- A QML file that needs the window takes `controller` and `theme` (not `app`/`desktop`), and Main binds them once in a same-named inline component (`component QuietButton: Shared.QuietButton { controller: app; theme: desktop }`). Inside an inline component a property named like an outer id binds to itself (`app: app` is undefined), so name it differently.
- Everything must work by keyboard and by mouse; `a11yui` pins this. Respect **Disable animations** and **Disable sound effects**.
- User-visible feature changes update README's Features section; durable internal contracts update `ARCHITECTURE.md`; suite changes update `tests/README.md`.

## Code style

- C++17, Qt idioms: `QStringLiteral`, `auto` for obvious types, `m_` members, early returns, braces on their own line for functions. Match the surrounding file.
- QML: 4-space indent, logic in small functions or `.js` libraries (pure JS like `Margins.js` stays testable).
- A list or map property from C++ (`bible.versions`) is a live reference in QML: rows kept from it (`Array.from(...)`) read the property again later, as it is then. Copy the values you keep.
- Comments explain why, never what. Fail loudly at boundaries (import, catalog, backup validation); don't swallow errors.
- Keep temporary scripts, notes, and screenshots out of the repo. `screenshots/` holds only README images.

## Git & commits

- Small imperative commits with capitalized descriptive titles, no prefixes, no trailing period, each a working state. Titles describe the user-visible behavior, and may run long: "Open a key's hold window after its step's deferred work, so a tap over a chapter layout never repeats".
- Wrap code references in backticks.
- Always sign yourself as a co-author with a `Co-authored by:` trailer at the end of the commit message, naming the model you're running as (e.g., `Co-authored by: Claude Opus 5.5 <noreply@anthropic.com>`)
