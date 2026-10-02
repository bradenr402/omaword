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

Press `?` or `F1` in the app for the full, searchable list; Enter there performs the selected action.

### Navigate

- `j` / `k` or `Down` / `Up`: next / previous verse (hold to keep moving)
- `h` / `l`: previous / next chapter
- `4j`: move four verses (any count works)
- `20g`: go to verse 20
- `zz` / `zt` / `zb`: center the current verse, or scroll it to the top / bottom
- `Ctrl k`: navigate to a passage by reference

### Search

- `s`: search Scripture in the current version
- `/`: find text in the current chapter
- `n` / `N`: next / previous match (`3n` works)

### Palette

- `Ctrl Space` or `Ctrl Shift P`: open Commands
- `Space`: resume the last palette
- `Tab` / `Shift Tab`: switch between Passage, Search, and Command

### Select

- `v`: start visual selection, then `j` / `k` to extend it
- `J` / `K`: start and extend a range immediately

### Annotate

- `m`: bookmark
- `x`: highlight
- `w`: write a note
- `t`: tag the passage (in the tag editor, `Space` / `Enter` toggles a tag; Enter in search creates and applies one)
- `Shift E`: edit an existing annotation's range (`o` swaps ends, Enter saves, Esc restores)
- `d`: add the passage to the Desk, or remove it
- `Shift L`: Related passages that share tags

### Copy

- `y`: copy as a quotation
- `Y`: copy as plain text
- `Ctrl c`: copy selected text, or the passage when no text is selected
- Middle-click: copy the clicked verse or selection as a quotation
- Right-click, `Menu`, or `Shift F10`: context menu for whatever you clicked

### Panels and windows

- `Ctrl n`: Notes
- `Ctrl a`: Annotations (Bookmarks, Highlights, Notes, Tags)
- `Shift D`: Desk, a study list shared by all windows and versions (in the Desk, `x` removes a passage and `Shift X` cleans it)
- `Shift V`: switch versions
- `Shift R`: Reading settings
- `Ctrl Shift N`: new reader window

### Reading

- `b`: cycle Paragraph, Verse, and Block layouts
- `f`: focus mode, which dims everything except the current verse or selection
- `Shift H`: show or hide headings

Commands → **Reduce motion** turns off pulses, flashes, and fades; **Disable sound effects** silences the subtle sounds. Both apply to all windows.

Notes save as you type. Reader windows have independent passages and share the local library. Reading preferences and your place are restored when you return.

Annotations → Tags summarizes exact tags, overlapping tagged ranges, and other related passages; click a tag to seed Related, or press `Shift E` to move the assignments of the focused range. Use `t` to edit exact-range assignments and Commands → **Manage tags** to rename, delete, or merge global names. Names are shared across versions; assignments belong to one version.

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
