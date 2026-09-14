# OmaWord

A minimal, keyboard-first Bible reader built with Qt Quick and C++ that follows your Omarchy theme, system font, and desktop corners. Read offline, bookmark, highlight, and tag passages, and keep Markdown notes alongside the text.

## Install

Build and install from the repository:

```sh
bin/build
bin/install
```

Launch **OmaWord** from your application launcher or run `omaword`. An Arch package can also be built with `makepkg` from `packaging/`.

## Bible versions

Choose **Add version** to install NET, KJV, KJV with Apocrypha, OJB, YLT, ASV, or BSB directly from eBible.org. You can also import a local USFM ZIP.

Internet is needed only to download a version. Once installed, reading and annotations work offline, and the original ZIP is no longer needed. Bible text has its own copyright and attribution, available in the app and [version guide](VERSIONS.md).

## Shortcuts

- `s` or `Ctrl k` searches for a passage.
- `Ctrl Space` or `Ctrl Shift P` opens Commands. `Space` resumes the last palette.
- `j` / `k` moves between verses; `h` / `l` moves between chapters. Arrow keys work too.
- `v` starts visual selection; `j` / `k` extends it. `J` / `K` starts and extends a range immediately.
- `4j` moves four verses; `20g` goes to verse 20.
- `m` bookmarks; `x` highlights; `n` writes a note; `t` opens the passage tag editor in Annotations → Tags.
- In the tag editor, `Space` / `Enter` toggles a tag; Enter in search creates and applies it.
- `Ctrl n` toggles Notes; `Ctrl a` toggles Annotations (Bookmarks, Highlights, Notes, Tags).
- `y` copies a quotation; `Y` copies plain text. `Ctrl c` copies selected text, or the passage when no text is selected.
- `b` cycles Paragraph, Verse, and Block layouts; `f` toggles focus mode.
- `Shift V` switches versions; `Shift R` opens Reading settings; `Shift H` toggles headings.
- `Shift L` opens Related passages: search tags, match Any/All, and choose the source version or all installed versions. The source stays pinned until **Use current**.
- `Ctrl Shift N` opens another reader window.
- `?` or `F1` opens the searchable keybindings list, where Enter performs the selected action.

Notes save as you type. Reader windows have independent passages and share the local library. Reading preferences and your place are restored when you return.

Annotations → Tags adds, removes, or creates flat named tags for the focused exact range. Names are shared across versions; assignments belong to one version. **Manage** there, or Commands → **Manage tags**, renames, deletes, or merges names; Related is discovery-only. See the [tag contract](TAGS.md).

The footer puts `?` before mode on the left and Related (overlapping passages), Annotations, and Notes on the right. Bookmarks, tags, and notes share kind icons and range brackets: row rails in Verse, inline in Paragraph/Block, with superscript icons and subtle cursor-range shading.

All your Bible versions, highlights, bookmarks, notes, and tags stay on this device. Personal data lives in `~/.local/share/omaword/`; backup and Markdown export are available in Commands.

## Requirements

- A C++17 compiler, `make`, `qmake6`, and `pkg-config`
- Qt 6: `qt6-base`, `qt6-declarative`, and `qt6-wayland` for Wayland
- `libarchive` and `fontconfig`

Application code is [MIT licensed](LICENSE). See [tests/README.md](tests/README.md) for development checks.
