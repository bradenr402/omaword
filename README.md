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

Search results follow your Omarchy theme, light or dark:

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

### Copy

- `y` · `middle click`: Copy passage as quotation
- `ctrl c`: Copy selected text
- `shift Y` · `ctrl shift C`: Copy passage as plain text
- `menu` · `shift F10` · `right click`: Open context menu

### Panels

- `shift A`: Annotations panel
- `shift D`: Open Desk
- `shift L`: Related passages

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
- `alt l`: Reading mode
- `alt s`: Sound effects
- `alt m`: Reduce motion
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
- `enter`: Open item
- `y`: Copy item as quotation
- `shift Y`: Copy item as plain text
- `d`: Add item to Desk, or remove it
- `shift E`: Edit item's annotation range
- `delete` · `backspace`: Remove item
- `/`: Search the list (Annotations, Related)
- `shift delete` · `shift backspace`: Clean Desk

### Commands only

- Line height
- Tags
- Add version
- Remove version
- Export notes as Markdown
- Save annotation backup
- Restore annotation backup
- About & Attribution

### Aliases

These also work, but stay out of the in-app list:

- `20G` (Go to verse number): Vim's go-to-line.
- `ctrl shift P` (Open commands): the command palette key in VS Code and its kin.
- `F1` (Keybindings): the help key in most desktop apps.

<!-- shortcuts:end -->

**Reduce motion** (`alt m`) turns off pulses, flashes, and fades; **Disable sound effects** (`alt s`) silences the subtle sounds. Both are also in Commands and apply to all windows.

Notes save as you type. Reader windows have independent passages and share the local library. Reading preferences and your place are restored when you return.

Annotations → Tags summarizes exact tags, overlapping tagged ranges, and other related passages; click a tag to seed Related, or press `shift E` to move the assignments of the focused range. Use `t` to edit exact-range assignments and **Manage tags** (`ctrl t`) to rename, delete, or merge global names. Names are shared across versions; assignments belong to one version.

The footer puts `?` before mode on the left, search, passage, and version in the center, and Related, Annotations, Desk, and Notes on the right. Annotations are drawn like pen and highlighter marks: hand-drawn Verse rails or inline Paragraph/Block brackets, with subtle cursor-range shading.

All your Bible versions, highlights, bookmarks, notes, and tags stay on this device. Personal data lives in `~/.local/share/omaword/`; backup and Markdown export are available in Commands.

## Requirements

- Omarchy (Hyprland on Arch Linux)
- `base-devel` (C++17 compiler, `make`, `pkg-config`) and `qmake6`, provided by `qt6-base`
- Qt 6: `qt6-base`, `qt6-declarative`, `qt6-multimedia` (sound effects), and `qt6-wayland`
- `libarchive` and `fontconfig`

Omarchy already provides most of these; the install command above adds the rest.

## Reporting bugs

Found a problem or have an idea? [Open a GitHub issue](https://github.com/bradenr402/omaword/issues) with what you did, what you expected, and what happened. Screenshots and the passage or version involved help.

Application code is [MIT licensed](LICENSE). See [ARCHITECTURE.md](ARCHITECTURE.md) and [tests/README.md](tests/README.md) for development.
