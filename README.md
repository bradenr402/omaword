# OmaWord

[![Built for Omarchy: App](https://raw.githubusercontent.com/tcballard/omarchy-badges/75975e5b5bf75e7ede3764bcd2950046f7abfe2c/badges/v1/omarchy-app.svg)](https://github.com/tcballard/omarchy-badges)

A minimal, keyboard-first Bible reader built with Qt Quick and C++ that follows your Omarchy theme, system font, and desktop corners. Read offline, bookmark, highlight, and tag passages, and keep Markdown notes alongside the text.

![OmaWord reading John 1 with a highlighted range and inline annotation brackets](screenshots/reader.png)

## Install

OmaWord is built for [Omarchy](https://omarchy.org) and assumes its Hyprland session, theme files, and fonts. Other desktops are not supported.

```sh
sudo pacman -S --needed base-devel qt6-declarative qt6-multimedia libarchive
git clone https://github.com/bradenr402/omaword.git
cd omaword
bin/build
bin/install
```

Launch **OmaWord** from your application launcher or run `omaword`. To update, `git pull` and run `bin/build` and `bin/install` again.

## Bible versions

Choose **Add version** to install NET, KJV, KJV with Apocrypha, OJB, YLT, ASV, or BSB directly from eBible.org. You can also import a local USFM ZIP.

Internet is needed only to download a version. Once installed, reading and annotations work offline, and the original ZIP is no longer needed. Bible text has its own copyright and attribution, shown in About & Attribution.

## Screenshots

The palette navigates to passages, searches Scripture, and runs commands:

![Passage palette over the reader](screenshots/palette.png)

OmaWord follows your Omarchy theme, colors, and fonts:

![Bible search results in a light theme](screenshots/search.png)

## Shortcuts

<!-- shortcuts:start -->

Press `?` in the app for the full, searchable list; `enter` there performs the selected action.
Generated from the action catalog by `bin/shortcuts`; alternatives are separated by ·.

### Navigate

- `k` · `↑`: Previous verse
- `j` · `↓`: Next verse
- `4j`: Repeat a verse motion by a count
- `20g`: Go to verse number
- `g` · `home`: Select chapter beginning
- `shift G` · `end`: Select chapter end
- `ctrl u`: Previous 5 verses
- `ctrl d`: Next 5 verses
- `ctrl b` · `pgup`: Previous 10 verses
- `ctrl f` · `pgdn`: Next 10 verses
- `zz`: Center current verse
- `zt`: Scroll current verse to top
- `zb`: Scroll current verse to bottom
- `h` · `←`: Previous chapter
- `l` · `→`: Next chapter
- `{`: Previous heading or chapter top
- `}`: Next section heading

### History

- `[`: Back to the previous jump
- `]`: Forward to the next jump
- `-`: Swap with the previous jump

### Select

- `click`: Select verse
- `shift click`: Select a range
- `v`: Toggle visual selection
- `shift J` · `shift ↓`: Extend to next verse
- `shift K` · `shift ↑`: Extend to previous verse

### Search

- `/`: Find in chapter
- `n`: Next search match
- `shift N`: Previous search match
- `ctrl /`: Search Bible

### Annotate

- `m`: Bookmark selection
- `x`: Highlight selection
- `w`: Write a note
- `t`: Tag passage
- `d`: Add to Desk
- `shift E`: Edit annotation range
- `ctrl t`: Manage tags
- `[[`: Suggest a passage to link (in a note)
- `alt enter`: Open the passage link at the cursor (in a note)

### Copy

- `y` · `middle click`: Copy passage as quotation
- `ctrl c`: Copy selected text
- `shift Y` · `ctrl shift C`: Copy passage as plain text
- `menu` · `shift F10` · `right click`: Open context menu

### Panels

- `shift A`: Annotations panel
- `shift D`: Open Desk
- `shift S`: Study panel
- `shift H`: History

### Palettes and windows

- `ctrl k`: Navigate to a passage
- `ctrl space`: Open commands
- `space`: Resume last palette
- `ctrl r`: Reading settings
- `ctrl v`: Switch version
- `?`: Keybindings
- `ctrl n`: New window
- `ctrl i`: Add version from Versions

### Reading

- `f`: Focus mode
- `alt n`: Show verse numbers
- `alt h`: Show headings
- `alt p`: Verse picker
- `alt j`: Highlight words of Jesus
- `alt o`: Chapter outline
- `alt w`: Margin notes
- `alt l`: Reading mode
- `alt s`: Sound effects
- `alt m`: Animations
- `ctrl +` · `ctrl =`: Larger text
- `ctrl -`: Smaller text
- `ctrl 0`: Reset text size
- `ctrl alt =`: Increase line height
- `ctrl alt -`: Decrease line height
- `ctrl alt 0`: Reset line height
- `esc`: Dismiss current panel or selection

### Lists

- `j` · `↓`: Next item
- `k` · `↑`: Previous item
- `g`: First item
- `shift G`: Last item
- `ctrl d`: Down 5 items
- `ctrl u`: Up 5 items
- `ctrl f` · `pgdn`: Down 10 items
- `ctrl b` · `pgup`: Up 10 items
- `enter`: Open item
- `y`: Copy item as quotation
- `shift Y`: Copy item as plain text
- `d`: Add item to Desk, or remove it
- `shift E`: Edit item's annotation range
- `delete` · `backspace`: Remove item
- `t`: Edit the item's tags (Annotations and Study Tags)
- `tab` · `shift tab`: Between Study Tags results and tag chips
- `space`: Turn a Study tag chip on or off
- `shift space`: Show only this chip's tag, or every tag again
- `0`: Turn every Study tag chip back on
- `a`: Study Tags in all installed versions, or this one
- `/`: Search the list (Annotations, Study's Tags)
- `h` · `←`: Previous tab (Annotations, Study)
- `l` · `→`: Next tab (Annotations, Study)
- `shift delete` · `shift backspace`: Clean Desk
- `shift delete` · `shift backspace`: Clear History

### Commands only

- Add version
- Remove version
- Export notes as Markdown
- Save annotation backup
- Restore annotation backup
- Clear History
- Disable / Enable History
- About & Attribution

### Aliases

These also work, but stay out of the in-app list:

- `20G` (Go to verse number): Vim's go-to-line.
- `ctrl shift P` (Open commands): the command palette key in VS Code and its kin.
- `F1` (Keybindings): the help key in most desktop apps.

<!-- shortcuts:end -->

## Features

### Reading
- Paragraph, Verse, and Block layouts (`alt l`), with optional verse numbers and headings.
- Focus mode (`f`) dims everything but the current verse and keeps it 40% down the window, like a typewriter.
- **Highlight words of Jesus** (`alt j`) colors them with your theme's palette, where the version marks them (KJV).
- Wide windows add margins: a sticky **Chapter outline** of the chapter and its section headings on the left (`alt o`; `{` / `}` jump between headings, `{` ending at the top of the chapter), and **Margin notes** on the right (`alt w`), each joined to its passage by hand-drawn braces and an arrow. Margin notes take priority and may move the text left; both are off by default.

### Finding passages
- The passage palette understands compact references like `jn316`, `1co13`, or `rev 3 14-22`, and lists likely matches, best first.
- Search Scripture across the current version, or find text in the current chapter.
- `[` and `]` step back and forward through the passages you jumped to in this window; `-` swaps with the previous one.
- History (`shift H`) lists passages you stayed on for a few seconds, shared by every window. It never goes into backups, and Commands can clear or disable it.

### Studying
- Bookmark, highlight, tag, and write Markdown notes; notes save as you type.
- In a note, `[[` suggests passages and inserts the full reference. Exact references such as `John 3:16` become links (`alt enter` follows one), and Notes lists the other notes that link to a passage.
- The Study panel (`shift S`) shows **Related** passages that share tags, **Cross-references**, and the chapter's **Footnotes**. Footnotes also appear as small marks in the text.
- The Desk (`d`, `shift D`) collects passages for side-by-side study across windows and versions.
- Annotations → Tags summarizes tags on the current range. **Manage tags** (`ctrl t`) renames, deletes, or merges them; names are shared across versions, assignments belong to one version.

### Feel
- Annotations look like pen and highlighter marks.
- Moving to a verse slides briefly, jumps pulse, copies flash, and the text slides aside for margin notes. **Disable animations** (`alt m`) and **Disable sound effects** (`alt s`) turn these off for all windows.
- The footer holds help and mode on the left; search, passage, version, and layout in the center; and Annotations, Study, History, and Desk on the right (Notes opens with `w`). Buttons in each group sit edge to edge. In narrow windows the right group stacks into a column, then folds into a `…` menu.

### Your data
Bible versions, annotations, the Desk, and History stay on this device, in `~/.local/share/omaword/`. Backup, restore, and Markdown export are in Commands. Reader windows have independent passages and share one library.

Versions installed before footnotes were supported show **Re-import** in Versions and Reading settings; it replaces the text and keeps your annotations.

## Requirements

- Omarchy (Hyprland on Arch Linux)
- `base-devel` (C++17 compiler, `make`, `pkg-config`) and `qmake6`, provided by `qt6-base`
- Qt 6: `qt6-base`, `qt6-declarative`, `qt6-multimedia` (sound effects), and `qt6-wayland`
- `libarchive` and `fontconfig`

Omarchy already provides most of these; the install command above adds the rest.

## Reporting bugs

Found a problem or have an idea? [Open a GitHub issue](https://github.com/bradenr402/omaword/issues) with what you did, what you expected, and what happened. Screenshots and the passage or version involved help.

Cross-references come from each version's own source files. Application code is [MIT licensed](LICENSE). See [ARCHITECTURE.md](ARCHITECTURE.md) and [tests/README.md](tests/README.md) for development.
