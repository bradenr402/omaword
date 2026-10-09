# OmaWord

[![Built for Omarchy: App](https://raw.githubusercontent.com/tcballard/omarchy-badges/75975e5b5bf75e7ede3764bcd2950046f7abfe2c/badges/v1/omarchy-app.svg)](https://github.com/tcballard/omarchy-badges)

A minimal, keyboard-first Bible reader built with Qt Quick and C++ that follows your Omarchy theme, system font, and desktop corners. Read offline, highlight and tag passages, and keep Markdown notes alongside the text.

![OmaWord reading John 1 with a highlighted range and inline annotation brackets](screenshots/reader.png)

## Install

OmaWord is built for [Omarchy](https://omarchy.org) and assumes its Hyprland session, theme files, and fonts. Other desktops are not supported.

```sh
sudo pacman -S --needed base-devel qt6-declarative libarchive libpipewire
git clone https://github.com/bradenr402/omaword.git
cd omaword
bin/build
bin/install
```

Launch **OmaWord** from your application launcher or run `omaword`. From a terminal, a script, or a Hyprland binding:

```sh
omaword "John 3:16"                  # a new window at a passage
omaword open jn316 --here            # the window you were reading, at a passage
omaword lookup                       # look up a passage in a small window, where you are
omaword quote jn316                  # print it as a quotation (--plain: the text alone)
omaword quote ps23 -t KJV | wl-copy  # in another translation
omaword search "living water"        # search the version you read (--all: every one)
omaword search '/lov(e|ed) the/'     # a regular expression, in slashes
omaword translations                 # the installed translations
omaword link jn316                   # an omaword: link to a passage
```

`quote`, `search`, `translations`, and `link` print without a window and never move your reading place; `-t` (`--translation`) names a translation by its abbreviation or ID. `omaword help` lists every command, `omaword help <command>` explains one, and `man omaword` has the manual. `bin/install` installs bash completion; `omaword completion zsh` and `omaword completion fish` print the others.

Links like `omaword:John%203:16`, or `omaword:jn316?translation=KJV`, open where you were reading: the launcher registers the `omaword:` scheme, so `xdg-open` and browsers hand them to OmaWord. The launcher's menu has **Look Up a Passage** and **New Window**.

`omaword lookup` opens a small window on the workspace you are on: type a passage (`jn316` works), `enter` shows it, `y` copies it (`shift Y` the plain text), `enter` again opens it in the window you were reading, and `esc` closes it. With more than one version installed, `tab` and `shift tab` (or a click on the version's name) read the passage in another, and `enter` opens it in that version. It suits a key binding; in Omarchy, add one to `~/.config/hypr/bindings.lua`:

```lua
o.bind("SUPER + ALT + B", "Look up a passage", "omaword lookup")
```

Hyprland floats the window (its size is fixed). To center it and show and hide it at once, as Omarchy's menus do, add a rule there too:

```lua
o.window({ class = "^omaword$", title = "^Look up · OmaWord$" }, { float = true, center = true, no_anim = true })
```

If you copy from it with OmaWord otherwise closed, OmaWord stays running without a window until something else is copied, so what you copied can still be pasted.

To update, `git pull` and run `bin/build` and `bin/install` again. `bin/install` keeps the files it replaces from one previous install in `~/.local/state/omaword/previous-install`.

To install it as an Arch package instead (to `/usr`, managed by pacman), build the recipe in `packaging/`; it builds the latest commit on GitHub:

```sh
cd packaging
makepkg -si
```

## Bible versions

Choose **Add version** to install NET, KJV, KJV with Apocrypha, OJB, YLT, ASV, or BSB directly from eBible.org. Each version's card says what it is, its year, download size, and license, and what it carries (footnotes, section headings, parallel passages, words of Jesus, the Apocrypha); the download's progress shows in its card. You can also import a local USFM ZIP.

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
- `]` · `mouse forward`: Forward to the next jump
- `-`: Swap with the previous jump
- `shift R`: Place the ribbon here
- `r`: Back to the ribbon
- `ctrl shift R`: Remove the ribbon
- `mouse back`: Back a jump, back to the palette's books, or close the open panel

### Select

- `click`: Select verse
- `shift click`: Select a range
- `shift V`: Toggle visual selection
- `v`: Select characters (Vim charwise visual)
- `h` · `l`: Characters: left, right (across verses)
- `w` · `b` · `e` · `ge`: Characters: by word
- `shift W` · `shift B` · `shift E`: Characters: by WORD (no spaces)
- `(` · `)`: Characters: by sentence
- `j` · `k`: Characters: by line
- `0` · `^` · `$`: Characters: line start, first ink, end
- `gg` · `shift G`: Characters: chapter start, end
- `iw` · `aw` · `is` · `as`: Characters: word, sentence objects
- `o`: Characters: swap ends
- `y` · `ctrl c`: Characters: copy quotation
- `shift Y` · `ctrl shift C`: Characters: copy plain text
- `esc`: Characters: leave
- `shift J` · `shift ↓`: Extend to next verse
- `shift K` · `shift ↑`: Extend to previous verse

### Search

- `/`: Find in chapter
- `n`: Next search match
- `shift N`: Previous search match
- `ctrl /`: Search Bible
- `alt a`: Bible search: every version or the one you read
- `alt r`: Bible search: words or a regular expression
- `ctrl ↓` · `ctrl ↑`: Bible search: next book, previous book

### Annotate

- `x`: Highlight selection
- `w`: Write a note
- `t`: Tag passage
- `d`: Add to Desk
- `ctrl shift D`: Clean Desk
- `u` · `ctrl z`: Undo a mark, note, or tag change
- `ctrl shift Z` · `ctrl r`: Redo the change undone
- `ctrl t`: Manage tags
- `enter`: Manage tags: rename the chosen tag
- `delete`: Manage tags: delete a tag
- `ctrl m`: Manage tags: choose a tag to merge into
- `backspace`: Manage tags: back to the picker (empty filter)
- `[[`: Suggest a passage to link (in a note)
- `alt enter`: Open the passage or web link at the cursor (in a note or the Desk)
- `ctrl click`: Open the passage or web link clicked (in a note or the Desk)
- `alt v`: Switch a note or the Desk between Write and Preview
- `ctrl s`: Note or Desk changed in another window: save my draft
- `ctrl shift S`: Note or Desk changed in another window: use the saved text
- `ctrl ↓`: From a note to its Linked from list (esc returns)
- `esc`: Close a note or the Desk, the draft saved (its link suggestions first)

### Copy

- `y` · `middle click`: Copy passage as quotation
- `ctrl c`: Copy selected text as quotation (else the passage)
- `shift Y`: Copy passage as plain text
- `ctrl shift C`: Copy selected text as plain text (else the passage)
- `menu` · `shift F10` · `right click`: Open context menu

### Panels

- `shift M`: Marks panel
- `shift D`: Open Desk
- `shift S`: See also panel
- `ctrl w`: Keyboard to the open side panel, or back to the text
- `i`: Peek at the verse's footnotes
- `c`: Compare the selection across versions
- `o`: Compare: open the highlighted version in a window beside, following this one

### Palettes and windows

- `ctrl k`: Navigate to a passage
- `ctrl h`: History
- `ctrl space`: Open commands
- `space`: Resume last palette
- `ctrl ,`: Reading settings
- `ctrl v`: Switch version
- `shift C`: Open a version beside this window
- `y` · `shift Y`: Copy the highlighted version's text (Versions, comparing)
- `?`: Keybindings
- `ctrl n`: New window
- `ctrl i`: Add version from Versions
- `ctrl enter`: Open the highlighted version beside this window (Versions)
- `enter`: Add version: install the highlighted version
- `shift enter`: Add version: its Details & Attribution
- `ctrl i`: Add version: import a USFM ZIP
- `backspace`: Back a step in Remove version (Versions)
- `ctrl shift I`: Re-import the highlighted version (Versions)
- `delete`: History: remove a passage
- `shift delete`: History: clear it

### Reading

- `f`: Focus mode
- `alt n`: Show verse numbers
- `alt h`: Show headings
- `alt o`: Chapter outline
- `alt w`: Show notes in margin
- `alt j`: Highlight words of Jesus
- `alt l`: Layout
- `alt shift L`: Default layout (Paragraph)
- `alt s`: Sound effects
- `alt m`: Animations
- `ctrl =`: Larger text
- `ctrl -`: Smaller text
- `ctrl 0`: Reset text size
- `ctrl alt =`: Increase line height
- `ctrl alt -`: Decrease line height
- `ctrl alt 0`: Reset line height
- `esc`: Dismiss current panel or selection

### Lists

- `j` · `↓`: Next item
- `k` · `↑`: Previous item
- `g` · `home`: First item
- `shift G` · `end`: Last item
- `ctrl d`: Down 5 items
- `ctrl u`: Up 5 items
- `ctrl f` · `pgdn`: Down 10 items
- `ctrl b` · `pgup`: Up 10 items
- `enter`: Open item
- `y`: Copy item as quotation
- `shift Y`: Copy item as plain text
- `d`: Add item to Desk
- `delete` · `backspace`: Remove item
- `t`: Edit the item's tags (Marks, See also)
- `w`: Write the item's note in place (Marks)
- `/`: Filter the list, #name for a tag (Marks with more than 10 items, See also)

### Commands only

- Add version
- Remove version
- Export your marks as Markdown
- Back up your marks
- Restore a backup of your marks
- Clear History
- Stop following the window beside
- Disable / Enable History
- About & Attribution

### Aliases

These also work, but stay out of the in-app list:

- `20G` (Go to verse number): Vim's go-to-line.
- `F6` (Keyboard to the open side panel, or back to the text): the desktop's key for moving between a window's panes.
- `ctrl shift P` (Open commands): the command palette key in VS Code and its kin.
- `F1` (Keybindings): the help key in most desktop apps.
- `ctrl +` (Larger text): the zoom-in key in most apps (shift = on many layouts).

<!-- shortcuts:end -->

## Features

### Reading
- Paragraph, Verse, and Block layouts (`alt l`, or Reading settings → Layout; `alt shift L` returns to Paragraph, the default), with optional verse numbers and headings.
- **Aa** in the footer sets text size, line height, and layout at hand. It sits above the footer and the text makes room for it, so it never covers what you are reading; its changes show as you make them. Reading settings (`ctrl ,`) has them too, as sliders with − and + on either side.
- A comfortable page: the column is about 66 characters wide and grows with the Text size; the title, headings, verse numbers, and notes are steps of one type scale; poetry lines are indented by level (`\q1`, `\q2`, …) with hanging wrapped lines in the Paragraph and Verse layouts, and run on as prose in Block.
- Focus mode (`f`) dims everything but the current verse and keeps it 40% down the window, like a typewriter. The footer then shows only the focus button; rest the pointer on it (or tab to it) to reveal the rest in place.
- **Highlight words of Jesus** (`alt j`) colors them with your theme's palette, where the version marks them (KJV).
- Wide windows add margins: a sticky **Chapter outline** of the chapter and its section headings on the left (`alt o`; `{` / `}` jump between headings, `{` ending at the top of the chapter), and notes in the right margin (**Show notes in margin**, `alt w`), each joined to its passage by hand-drawn braces and an arrow. Margin notes take priority and may move the text left, keeping it there from chapter to chapter, notes or not, but leave the outline its room where both fit; both are off by default.

### Finding passages
- The passage palette understands compact references like `jn316`, `1co13`, or `rev 3 14-22`, and lists likely matches, best first. Choosing a book (`enter` or `→`) shows its chapters as a grid of numbers, five to a row: arrows move, typing a number narrows the grid, `3:16` goes to that verse, and `backspace` returns to the books. A chapter opens on its first verse ("Galatians 2" selects 2:1), ready for `j` and `k`.
- Search Scripture in the version you read, or in every version you installed (`alt a` in the search palette): results come in Bible order under each book's name and count (and its version's, searching them all), and `ctrl ↓` / `ctrl ↑` jump between books. A search index kept in `~/.cache/omaword/` makes searches quick from the second one on. Words find verses holding them all, and "an exact phrase" may run across verses; a query in slashes (`/lov(e|ed) the/`), or any query once `alt r` (in the search palette) switches the search to regular expressions, is a regular expression matched within each verse. Find in the current chapter (`/`) takes regular expressions too. Both use Neovim-style smart case: lowercase ignores case, any capital matches it, `\c` / `\C` force either way.
- Click the chapter title for the book's chapter grid, the current chapter chosen; the `<` `>` buttons at the window's edges step chapters (`h` `l`). The next chapter is read ahead while you read, so turning to it is quick.
- `ctrl` + mouse wheel and a touchpad pinch change the Text size, like `ctrl =` and `ctrl -`.
- `[` and `]` (or the mouse's back and forward buttons) step back and forward through the passages you jumped to in this window; `-` swaps with the previous one.
- One ribbon keeps your reading place: `shift R` places it at the selected verses (or the chapter, scrolled as it is) and it stays there until you place it again; `r` returns to it from any window, in the version you are reading. Its chapter shows an accent ribbon at the top right of the text, which drops in as you place it; elsewhere the footer offers a small ribbon and "Mark 5" and the passage palette starts with **Continue**. `ctrl shift R` removes it (so do the right-click menus of the ribbon and of its chapter in the footer, and `delete` on **Continue**), and none comes back until you place one.
- History keeps the passages you stayed on for a few seconds, shared by every window. The passage palette lists the latest five above the books, then **All History**; `ctrl h` opens it directly: newest first under Today, Yesterday, and Earlier, typing narrows it, `delete` removes a passage and `shift delete` clears it. It never goes into backups, and Commands can clear or disable it.

### Studying
- Highlight, tag, and write Markdown notes; notes save when you pause typing, and before you move on. Highlights are paint: `x` paints the selected verses, joining any highlight they overlap or touch, and on verses already all highlighted it erases them, keeping the rest. Bookmarks are a tag, **Bookmarked**: bookmarks made in earlier versions carry it, and `t` adds it like any tag. `alt v` switches a note between Write and Preview; the editor keeps the keyboard (`tab` types a tab, `ctrl ↓` reaches Linked from). Notes (`w`) sits beside the text where the window is wide enough, the text making room for it (and margin notes stepping aside until it closes), and over the text's right edge in narrower windows; either way the text stays in view and clickable. `esc` in a note saves and closes it, as does its `×`; `ctrl w` (or `F6`) instead hands the keyboard back to the text with Notes still open, where `j` and `k` move on and Notes follows the selection, and `ctrl w` again returns to the note. The panel with the keyboard has an accent edge toward the text; while the text has it, the panel's current row turns grey and its hints say `ctrl w keys here`. Every side panel works this way, Notes, the Desk, Marks, and See also, in the same place beside the text (the Margin), under the same header: their keys turn the Margin from one to another, and palettes and dialogs open over them and hand the keyboard back.
- `u` or `ctrl z` undoes this window's last change to marks, notes, or tags, deletions included, or its last addition to the Desk, so deleting asks nothing first; `ctrl shift Z` or `ctrl r` redoes. It works from the side panels and while selecting characters, and the passage menu, row menus, and Commands name the step ("Undo highlight on John 3:16"). A passage changed since in another window is left as it is, and a toast says so.
- Previews keep your line breaks (a single Enter is a new line, as in GitHub comments and Obsidian), set paragraphs, lists, and quotes a blank line apart, and draw block quotes (`>`) with a small indent and a muted bar beside them. In a note, `[[` suggests passages and inserts the full reference. Exact references such as `John 3:16` become links (`alt enter` or `ctrl click` follows one, or opens a web address), and Notes lists the other notes that link to a passage.
- Quote part of a passage: `v` selects characters with Vim motions (`w`, `e`, `)`, `j`, `$`, `iw`, `as`, counts, …), or drag with the mouse; `y` or `ctrl c` copies a quotation with `…` where verses are cut and the reference, `shift Y` or `ctrl shift C` the plain text.
- **See also** (`shift S`) follows the passage you are reading with one list, in sections: the version's own cross-references, its parallel passages (where its source lists them under headings, as the BSB does), the passages you tagged alike, in every version, most shared tags first, and last OpenBible.info's community-ranked cross-references. Click a tag in that section's heading, or type `#grace` in the filter (`/`), to keep only its passages; `t` edits a row's tags. Footnotes appear as small marks in the text: click one (or press `i` on its verse) to peek at it in place, where `↑` `↓` step through the chapter's notes, `y` copies one, and `enter` opens See also for its verse.
- Compare a passage across your versions (two or more): `c` shows each version's text for the selection in a card under it, "Not in this version" where its numbering differs; `j` and `k` choose a version, `y` copies its quotation (`shift Y` plain text), and `enter` switches to it at the same passage. Versions (`ctrl v`) shows the same text beside each version while verses are selected.
- Read two versions side by side: `shift C` (or a right click on the version button, or Commands) opens a window reading your other version next to yours (Hyprland tiles it); with three or more, a card asks which (showing each one's text for the selection), and `enter` opens it. `o` in the Compare card and `ctrl enter` in Versions open the highlighted version at once. The two follow each other, whichever you move: the same passage, the same verse at the top, frame by frame as you scroll. The version button shows the pair (`NET ⇄ KJV`); closing either window, or **Stop following** in the version button's menu or Commands, ends it.
- The Desk is one Markdown page for study, shared by every window: `d` quotes the selected passage at its end (the text as a blockquote, then "— John 3:16 (NET)", a link back), `u` takes it off again, and `shift D` shows the page in the Notes margin, where you write around the quotations as in a note (`alt v` previews it, links and all). `w` turns the margin back to the note, `shift D` to the Desk. **Clean Desk** (`ctrl shift D`, its header's **Clean…**, or Commands) empties it after asking; `u` brings it back.
- **Marks** (`shift M`) lists everything you have marked, one row per passage: its highlight, tags, and note (or the verse, where there is no note), in Bible order, starting at the passage you are reading (`here`). `enter` opens a row's passage, Marks staying beside it. Marks kept in another version say "from KJV". Edit a row where it is: `w` writes its note right in the row (`esc` is done), `t` edits its tags, `delete` removes the mark (`u` undoes). Past ten marks a filter appears (`/`): words match the reference, note, and verse, and `#grace` keeps a tag's marks; clicking a row's tag does the same. **Manage tags** (`ctrl t`) renames, deletes, or merges tags; names are shared across versions, assignments belong to one version.

### Feel
- Notes and tags look like pen marks; highlights like a highlighter on paper, in your theme's yellow.
- Moving to a verse slides briefly, jumps pulse, copies flash, the text slides aside for margin notes, and the chapter outline slides in and out. Menus and dialogs open instantly and fade out toward where they came from; side panels slide in and ease out. **Disable animations** (`alt m`) and **Disable sound effects** (`alt s`) turn these off for all windows, and nothing moves while Hyprland's own animations are off.
- Colors come from your Omarchy theme: secondary text uses the theme's quietest text color that still reads (4.5:1 contrast), errors its red, and selected text its selection color. Screen readers hear each button's action, with its keys as the description.
- The footer holds focus, Aa, mode, and help on the left; search, passage, the way back to the ribbon (where there is room), and version in the center; and Marks, See also, and Desk on the right (Notes opens with `w`). Buttons in each group sit edge to edge. In narrow windows focus and help give way first, then the right group stacks into a column and folds into a `…` menu, Aa with it when even that is not enough.

### Keyboard and mouse
- Selecting verses with the mouse brings up a small bar above them: **x highlight · w note · t tag · y copy** and `⋯` for Desk, Compare versions, plain text, and See also. Any key or `esc` puts it away. Dragging over the text selects the verses it runs over as well as the text, so the bar's actions take those verses while `ctrl c` copies the text.
- Keybindings (`?`) opens on the essentials, then lists every key under its group's header, which stays in view as you scroll. A key that waits for another (`z`, a count, character selection's `g`, `i`, `a`) shows what can follow after a moment, a pill above the footer names the selection mode (Visual, Characters), and the first time a version is installed a strip shows the keys to start with.
- Every key hint is clickable where its action works now (hover shows a hand); every list row has the same right-click menu (Open · Copy · Copy plain text · Add to Desk), and `menu` or `shift F10` opens it from the keyboard.
- Footer and dialog buttons take the keyboard by `tab` only; a focused button takes `enter` and `space`. Scrollable dialogs scroll with `j` `k`, `pgup` `pgdn`, `ctrl d` `ctrl u`, `home` `end`.
- Clicking outside a panel, dialog, or picker only closes it (in confirmations it steps back, like `esc`); right-clicking elsewhere while a menu is open opens the new menu there at once. Keys and disabled rows that cannot act sound an error and say why ("Select a verse first"). Mouse back closes the top dialog.
- Text fields use the app's own menu (Cut · Copy · Paste · Select all; in a note also Open link and Preview). Right-clicking selected text keeps it only when the click is on it; the passage menu (`menu`) keeps a `v` selection, and its copy entries copy it.
- If a note changes in another window while you type, typing is not interrupted: `ctrl s` keeps your draft, `ctrl shift S` takes the saved text.
- With a side panel open, palette, dialog, and reading keys (`ctrl k`, `?`, `ctrl ,`, `alt n`, …) act over it, from its list and while you write in a note or on the Desk too; `f` (focus mode) closes it. `ctrl w` moves the keyboard between the text and the panel, and whichever does not have it recedes a little. Double-click a verse number for Notes, a word to select it; margin notes and the search match counter have right-click menus. Mark menus offer **Move here**: select the verses a note or tag belongs on, then move it there from its marker or its row in Marks (`u` undoes). Tooltips on the outline, margin notes, and verse numbers name their keys.

### Your data
Bible versions, marks, the Desk, History, and the ribbon stay on this device, in `~/.local/share/omaword/`. Commands back up your marks in every installed version (with the Desk), restore them, and export them as Markdown; backups from earlier versions of OmaWord still restore (their Desk passages become quotations on the page). Reader windows have independent passages and share one library.

Versions installed before footnotes or poetry indents were supported (or, for a source that lists them, parallel passages) show **Re-import** in Versions (`ctrl shift I` on the highlighted row) and Reading settings; it replaces the text and keeps your annotations.

## Requirements

- Omarchy (Hyprland on Arch Linux)
- `base-devel` (C++17 compiler, `make`, `pkg-config`) and `qmake6`, provided by `qt6-base`
- Qt 6: `qt6-base`, `qt6-declarative`, and `qt6-wayland`
- `libarchive`, `fontconfig`, and `libpipewire` (sound effects)

Omarchy already provides most of these; the install command above adds the rest.

## Reporting bugs

Found a problem or have an idea? [Open a GitHub issue](https://github.com/bradenr402/omaword/issues) with what you did, what you expected, and what happened. Screenshots and the passage or version involved help.

Cross-references come from each version's own source files and, after them, from [OpenBible.info](https://www.openbible.info/labs/cross-references/) (CC BY), labeled as theirs. Application code is [MIT licensed](LICENSE). See [ARCHITECTURE.md](ARCHITECTURE.md) and [tests/README.md](tests/README.md) for development.
