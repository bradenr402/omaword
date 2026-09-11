pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

ApplicationWindow {
    id: app
    objectName: "readerWindow"
    width: 1120
    height: 820
    minimumWidth: 640
    minimumHeight: 480
    visible: true
    title: bible.ready ? bible.passage + " — Omabible" : "Omabible"
    color: desktop.background
    font.family: desktop.fontFamily
    font.pixelSize: Math.round(14 * desktop.textScale)
    palette.window: desktop.background
    palette.windowText: desktop.foreground
    palette.base: desktop.background
    palette.alternateBase: desktop.surface
    palette.text: desktop.foreground
    palette.button: desktop.surface
    palette.buttonText: desktop.foreground
    palette.highlight: desktop.accent
    palette.highlightedText: desktop.background
    palette.placeholderText: desktop.muted
    palette.mid: desktop.border

    property bool notesOpen: false
    property bool notePreview: false
    property bool syncingNote: true
    property string loadedNoteReference: ""
    readonly property string observedNoteText: bible.noteText
    onObservedNoteTextChanged: syncNote()
    readonly property bool observedReady: bible.ready
    onObservedReadyChanged: { if (observedReady) restorePosition() }
    property bool verseMode: false
    property bool restoringPosition: true
    property bool positionJumpToSelection: false
    property bool wideLayout: width >= 1080
    property real readerSize: 19
    property int selectionAnchor: 0
    property bool movingSelection: false
    property int collectionTab: 0
    property int paletteBook: 0
    property string paletteBookName: ""
    property bool paletteArrowUsed: false
    readonly property var paletteDestination: {
        let query = referenceInput.text.trim()
        let entry = paletteResults.model[paletteResults.currentIndex]
        if (entry && (paletteArrowUsed || !query || query.startsWith(">")
                || (paletteBook === 0 && !/\d/.test(query)))) {
            return { entry: entry, reference: entry.chapter ? entry.title : "" }
        }
        return { entry: null, reference: paletteBook > 0 ? paletteBookName + " " + query : query }
    }
    readonly property var paletteResolution: {
        // Relative references depend on the current book, even though the resolver is pure.
        let currentBook = bible.book
        let ready = bible.ready
        let destination = paletteDestination
        if (destination.entry && !destination.reference)
            return { valid: true, reference: destination.entry.book ? "Browse " + destination.entry.title : destination.entry.title }
        return bible.resolveReference(destination.reference)
    }
    readonly property real scale: desktop.textScale > 0 ? desktop.textScale : 1
    readonly property bool hasSelection: bible.ready && bible.selectedStart > 0
    readonly property bool modalOpen: passagePalette.opened || savedDrawer.opened || helpDialog.opened || aboutDialog.opened || restoreConfirmation.visible || importDialog.visible || exportDialog.visible || backupDialog.visible || restoreDialog.visible
    readonly property bool readerKeys: bible.ready && !bible.busy && !modalOpen && !textInputActive()
    readonly property real notesWidth: Math.min(390 * scale, width - 40)

    function tint(color, alpha) { return Qt.rgba(color.r, color.g, color.b, alpha) }
    function textInputActive() {
        let item = activeFocusItem
        while (item) {
            if (item instanceof TextEdit) {
                if (!(item as TextEdit).readOnly) return true
            } else if (item instanceof TextInput) {
                if (!(item as TextInput).readOnly) return true
            }
            item = item.parent
        }
        return false
    }
    function syncNote() {
        if (!noteEditor) return
        syncingNote = true
        loadedNoteReference = bible.selectionReference
        if (noteEditor.text !== bible.noteText)
            noteEditor.text = bible.noteText
        syncingNote = false
    }
    function flushNote() {
        if (!syncingNote && hasSelection && loadedNoteReference === bible.selectionReference && noteEditor.text !== bible.noteText)
            bible.setNoteText(noteEditor.text)
        return syncingNote || !hasSelection || loadedNoteReference !== bible.selectionReference || noteEditor.text === bible.noteText
    }
    function readerFocus() { readerSurface.forceActiveFocus() }
    function notify(message) { toast.text = message; toastTimer.restart() }
    function selectVerse(number, extend) {
        if (!flushNote()) return
        if (extend && bible.selectedStart > 0) {
            // Mouse ranges are chapter-local; never reuse an anchor from another chapter.
            if (bible.crossChapterSelection || !selectionAnchor) selectionAnchor = bible.cursorVerse
            bible.selectVerses(selectionAnchor, number)
        } else {
            selectionAnchor = number
            bible.selectVerses(number, number)
        }
        syncNote()
        readerFocus()
    }
    function moveVerse(delta, extend) {
        if (!flushNote()) return
        positionTimer.stop()
        restoringPosition = true
        movingSelection = true
        bible.moveSelection(delta, extend)
        movingSelection = false
        syncNote()
        selectionAnchor = bible.crossChapterSelection ? 0
            : bible.cursorVerse === bible.selectedStart ? bible.selectedEnd : bible.selectedStart
        Qt.callLater(function() {
            readerColumn.forceLayout()
            app.scrollToVerse(bible.cursorVerse, false)
            app.restoringPosition = false
            app.savePosition()
        })
    }
    function setVisualMode(enabled) {
        if (!flushNote() || !bible.ready || bible.verses.length === 0) return false
        if (hasSelection) bible.selectVerses(bible.cursorVerse, bible.cursorVerse)
        else if (enabled) bible.selectVerses(bible.verses[0].number, bible.verses[0].number)
        selectionAnchor = bible.cursorVerse
        verseMode = enabled
        syncNote()
        return true
    }
    function extendVerse(delta) {
        if (!verseMode && !setVisualMode(true)) return
        moveVerse(delta, true)
    }
    function chapterEdge(last) {
        if (!flushNote() || bible.verses.length === 0) return
        let number = bible.verses[last ? bible.verses.length - 1 : 0].number
        selectVerse(number, false)
        Qt.callLater(function() { app.scrollToVerse(bible.cursorVerse, false) })
    }
    function chapterStep(delta) {
        if (!flushNote()) return
        savePosition()
        if (delta < 0) bible.previousChapter()
        else bible.nextChapter()
        readerFocus()
    }
    function jump(reference) {
        if (!flushNote()) return
        savePosition()
        if (bible.navigate(reference)) {
            passagePalette.close()
            readerFocus()
        }
    }
    function openChapter(book, chapter) {
        if (!flushNote()) return
        savePosition()
        bible.openChapter(book, chapter)
        passagePalette.close()
        readerFocus()
    }
    function openPalette(commands) {
        if (!flushNote()) return
        paletteBook = 0
        paletteBookName = ""
        referenceInput.text = commands || !bible.ready ? "> " : ""
        paletteArrowUsed = false
        passagePalette.open()
        referenceInput.forceActiveFocus()
    }
    function openNotes(focusEditor) {
        notesOpen = true
        if (loadedNoteReference !== bible.selectionReference) syncNote()
        if (focusEditor && hasSelection) {
            notePreview = false
            Qt.callLater(function() { noteEditor.forceActiveFocus() })
        }
    }
    function requestRestore(file) {
        if (!flushNote()) return
        restoreConfirmation.backupFile = file
        restoreConfirmation.open()
    }
    function confirmRestore() {
        if (!flushNote()) return
        if (bible.importBackup(restoreConfirmation.backupFile)) {
            syncNote()
            restoreConfirmation.close()
            notify("Annotation backup restored")
        }
    }
    function copyPassage() {
        if (bible.copySelection().length > 0)
            notify((hasSelection ? bible.selectionReference : bible.passage) + " copied")
    }
    function savePosition() {
        if (!restoringPosition && bible.ready) {
            let travel = Math.max(0, reading.contentHeight - reading.height)
            bible.readingPosition = travel > 0 ? Math.max(0, Math.min(1, reading.contentY / travel)) : 0
        }
    }
    function scrollBy(amount) {
        reading.contentY = Math.max(0, Math.min(Math.max(0, reading.contentHeight - reading.height), reading.contentY + amount))
    }
    function scrollToVerse(number, alignTop) {
        let index = Array.from(bible.verses).findIndex(function(verse) { return verse.number === number })
        let row = verseRepeater.itemAt(index)
        if (!row) return
        let y = row.mapToItem(readerColumn, 0, 0).y
        let target = reading.contentY
        if (alignTop || y < target + 24) target = y - 24
        else if (y + row.height > target + reading.height - 32) target = y + row.height - reading.height + 32
        reading.contentY = Math.max(0, Math.min(Math.max(0, reading.contentHeight - reading.height), target))
    }
    function restorePosition(jumpToSelection) {
        restoringPosition = true
        positionJumpToSelection = !!jumpToSelection
        positionTimer.restart()
    }
    function filteredCollection() {
        return Array.from(bible.collection).filter(function(entry) {
            return collectionTab === 0 || (collectionTab === 1 && entry.bookmark)
                || (collectionTab === 2 && entry.highlight) || (collectionTab === 3 && entry.note.length > 0)
        })
    }
    function openBookmark(reference) {
        if (!flushNote()) return
        savePosition()
        if (bible.navigate(reference)) { syncNote(); readerFocus() }
    }
    readonly property var keybindings: [
        { action: "Open a passage", keys: "Ctrl+K" },
        { action: "Open commands", keys: "Ctrl+Shift+P" },
        { action: "Saved passages", keys: "Ctrl+L" },
        { action: "Previous / next chapter", keys: "h / l · [ / ] · Alt+Left / Alt+Right" },
        { action: "Move verse (extend in visual mode)", keys: "j / k · Down / Up" },
        { action: "Toggle visual selection (anchor / collapse)", keys: "v" },
        { action: "Enter visual mode and extend", keys: "J / K · Shift+Down / Shift+Up" },
        { action: "Select verse / extend range", keys: "Click number / Shift-click" },
        { action: "Select chapter beginning / end", keys: "g / G · Home / End" },
        { action: "Page down / up", keys: "PageDown / PageUp · Space / Shift+Space · Ctrl+F / Ctrl+B" },
        { action: "Bookmark selection", keys: "m · Ctrl+D" },
        { action: "Highlight selection", keys: "x · Ctrl+H" },
        { action: "Write a note", keys: "n · Ctrl+N" },
        { action: "Toggle notes", keys: "Ctrl+Shift+N" },
        { action: "Copy passage (or chapter)", keys: "y · Ctrl+Shift+C" },
        { action: "Larger / smaller / reset text", keys: "Ctrl++ / Ctrl+− / Ctrl+0" },
        { action: "Leave editor / exit visual / close notes / collapse range", keys: "Esc" },
        { action: "Keybindings", keys: "? · F1" }
    ]
    function filteredKeybindings() {
        let query = keybindingSearch.text.trim().toLowerCase()
        return keybindings.filter(function(binding) {
            // Single-letter searches match a key, rather than every action containing it.
            return !query || (query.length === 1
                ? binding.keys.split(/\s*[·/]\s*/).some(function(key) { return key.toLowerCase() === query })
                : (binding.action + " " + binding.keys).toLowerCase().indexOf(query) >= 0)
        })
    }
    readonly property var commands: [
        { title: "Open saved passages", hint: "Ctrl+L", action: "saved" },
        { title: "Write a note", hint: "n · Ctrl+N", action: "notes" },
        { title: "Toggle bookmark", hint: "m · Ctrl+D", action: "bookmark" },
        { title: "Toggle highlight", hint: "x · Ctrl+H", action: "highlight" },
        { title: "Copy passage", hint: "y · Ctrl+Shift+C", action: "copy" },
        { title: "Export notes as Markdown", hint: "Choose a folder", action: "export" },
        { title: "Save annotation backup", hint: "JSON file", action: "backup" },
        { title: "Restore annotation backup", hint: "Choose a file", action: "restore" },
        { title: "Import NET 2.1 EPUB", hint: "Local file", action: "import" },
        { title: "Keybindings", hint: "? · F1", action: "help" },
        { title: "About & attribution", hint: "Omabible", action: "about" }
    ]
    function paletteEntries() {
        let query = referenceInput.text.trim().toLowerCase()
        if (query.startsWith(">")) {
            query = query.substring(1).trim()
            return commands.filter(function(c) { return c.title.toLowerCase().indexOf(query) >= 0 })
                .map(function(c) { return { title: c.title, hint: c.hint, action: c.action } })
        }
        if (paletteBook > 0) {
            let book = Array.from(bible.books).find(function(b) { return b.id === paletteBook })
            let result = []
            if (book) for (let i = 1; i <= book.chapters; ++i) {
                if (!query || String(i).indexOf(query) === 0)
                    result.push({ title: paletteBookName + " " + i, hint: "Chapter " + i, book: paletteBook, chapter: i })
            }
            return result
        }
        return Array.from(bible.books).filter(function(b) { return !query || b.name.toLowerCase().indexOf(query) >= 0 })
            .map(function(b) { return { title: b.name, hint: b.chapters + " chapters", book: b.id, chapter: 0 } })
    }
    function activatePaletteEntry(entry) {
        if (!entry) return
        if (entry.action) {
            passagePalette.close()
            runCommand(entry.action)
        } else if (entry.chapter) openChapter(entry.book, entry.chapter)
        else {
            // Changing the book rebuilds the results model; snapshot the entry first.
            let book = entry.book
            let name = entry.title
            paletteBookName = name
            paletteBook = book
            referenceInput.text = ""
            paletteResults.currentIndex = 0
            referenceInput.forceActiveFocus()
        }
    }
    function acceptPalette() {
        let destination = paletteDestination
        if (destination.entry) activatePaletteEntry(destination.entry)
        else if (paletteResolution.valid) jump(destination.reference)
    }
    function runCommand(action) {
        if (!flushNote()) return
        switch (action) {
        case "saved": bible.refreshCollection(); savedDrawer.open(); break
        case "notes": openNotes(true); break
        case "bookmark": if (hasSelection) bible.toggleBookmark(); else notify("Select a verse number to bookmark a passage"); break
        case "highlight": if (hasSelection) bible.toggleHighlight(); else notify("Select a verse number to highlight a passage"); break
        case "copy": if (bible.ready) copyPassage(); break
        case "export": exportDialog.open(); break
        case "backup": backupDialog.open(); break
        case "restore": restoreDialog.open(); break
        case "import": importDialog.open(); break
        case "help": helpDialog.open(); break
        case "about": aboutDialog.open(); break
        }
    }

    component QuietButton: Button {
        id: button
        property bool emphasized: false
        property bool compact: false
        property string tip: ""
        implicitHeight: compact ? 32 * app.scale : 38 * app.scale
        implicitWidth: Math.max(implicitHeight, buttonLabel.implicitWidth + 24)
        padding: 10
        hoverEnabled: true
        font.family: desktop.fontFamily
        font.pixelSize: 13 * app.scale
        contentItem: Text {
            id: buttonLabel
            text: button.text
            font: button.font
            color: !button.enabled ? app.tint(desktop.muted, 0.55) : button.emphasized ? desktop.accent : desktop.foreground
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle {
            radius: 0
            color: button.down ? app.tint(desktop.accent, 0.16) : button.emphasized ? app.tint(desktop.accent, 0.10) : button.hovered ? desktop.surface : "transparent"
            border.width: button.activeFocus ? 1 : 0
            border.color: desktop.accent
        }
        ToolTip.visible: hovered && tip.length > 0
        ToolTip.text: tip
        ToolTip.delay: 650
        Accessible.name: text
    }
    component FineRule: Rectangle {
        color: desktop.border
        implicitHeight: 1
        opacity: 0.65
    }
    component BookmarkGlyph: Canvas {
        property color ink: desktop.accent
        implicitWidth: 11 * app.scale
        implicitHeight: 15 * app.scale
        onInkChanged: requestPaint()
        onPaint: {
            let ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            ctx.fillStyle = ink
            ctx.beginPath()
            ctx.moveTo(0, 0); ctx.lineTo(width, 0); ctx.lineTo(width, height)
            ctx.lineTo(width / 2, height * 0.72); ctx.lineTo(0, height)
            ctx.closePath(); ctx.fill()
        }
    }
    component SectionLabel: Label {
        color: desktop.muted
        font.pixelSize: 11 * app.scale
        font.weight: Font.DemiBold
        font.letterSpacing: 1.5
    }
    component ModalCard: Dialog {
        modal: true
        anchors.centerIn: parent
        padding: 24
        width: Math.min(600 * app.scale, app.width - 48)
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: desktop.background
            radius: 0
            border.color: desktop.border
        }
        Overlay.modal: Rectangle { color: app.tint(desktop.background, 0.72) }
    }

    header: Rectangle {
        color: desktop.background
        height: 72 * app.scale
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 24
            anchors.rightMargin: 20
            spacing: 8
            QuietButton {
                text: bible.ready ? bible.passage + "  ⌄" : "Open a passage  ⌄"
                enabled: bible.ready && !bible.busy
                emphasized: true
                tip: "Go to a passage · Ctrl+K"
                onClicked: app.openPalette(false)
                Layout.maximumWidth: app.width * 0.4
            }
            Label {
                text: "NET"
                color: desktop.muted
                font.pixelSize: 10 * app.scale
                font.letterSpacing: 1
                Layout.leftMargin: 3
            }
            Item { Layout.fillWidth: true }
            QuietButton { text: "Saved"; tip: "Bookmarks, highlights & notes · Ctrl+L"; enabled: bible.ready && !bible.busy; onClicked: app.runCommand("saved") }
            QuietButton {
                text: "Notes"
                emphasized: app.notesOpen
                tip: "Toggle notes · Ctrl+Shift+N"
                enabled: bible.ready && !bible.busy
                onClicked: { app.flushNote(); app.notesOpen = !app.notesOpen }
            }
            QuietButton { text: "···"; tip: "Commands & settings · Ctrl+Shift+P"; onClicked: app.openPalette(true); font.pixelSize: 21 * app.scale; Accessible.name: "Open commands" }
        }
        FineRule { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: errorRow.implicitHeight + 20
            visible: bible.error.length > 0
            color: app.tint(desktop.accent, 0.08)
            RowLayout {
                id: errorRow
                anchors.fill: parent
                anchors.margins: 10
                anchors.leftMargin: 24
                spacing: 16
                Label { objectName: "errorMessage"; text: bible.error; color: desktop.foreground; wrapMode: Text.Wrap; Layout.fillWidth: true; font.pixelSize: 13 * app.scale; Accessible.role: Accessible.AlertMessage }
                QuietButton { text: "Dismiss"; compact: true; onClicked: bible.clearError() }
            }
        }
        Item {
            id: workspace
            Layout.fillWidth: true
            Layout.fillHeight: true

            Item {
                id: readerSurface
                objectName: "readerSurface"
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.rightMargin: app.notesOpen && app.wideLayout && bible.ready ? app.notesWidth : 0
                focus: true
                visible: bible.ready

                Flickable {
                    id: reading
                    objectName: "reading"
                    anchors.fill: parent
                    clip: true
                    contentWidth: width
                    contentHeight: readerColumn.height
                    boundsBehavior: Flickable.StopAtBounds
                    flickableDirection: Flickable.VerticalFlick
                    onWidthChanged: {
                        if (app.hasSelection)
                            Qt.callLater(function() { app.scrollToVerse(bible.cursorVerse, true) })
                    }
                    onContentYChanged: positionSaveTimer.restart()
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                    Column {
                        id: readerColumn
                        width: Math.min(760 * app.scale, reading.width - (reading.width < 700 ? 48 : 96))
                        anchors.horizontalCenter: parent.horizontalCenter
                        topPadding: 48 * app.scale
                        bottomPadding: 48 * app.scale
                        spacing: 0

                        SectionLabel { text: "NEW ENGLISH TRANSLATION"; leftPadding: 36; bottomPadding: 12 }
                        Label {
                            text: bible.passage
                            color: desktop.foreground
                            font.pixelSize: 34 * app.scale
                            font.weight: Font.Medium
                            font.letterSpacing: -0.9
                            leftPadding: 36
                            bottomPadding: 12
                            width: parent.width
                            wrapMode: Text.Wrap
                        }
                        Label {
                            text: app.verseMode ? "VISUAL  ·  j / k extends  ·  v collapses to cursor" : "j / k moves  ·  v selects a range  ·  click a verse number to annotate"
                            color: desktop.muted
                            font.pixelSize: 12 * app.scale
                            leftPadding: 36
                            bottomPadding: 32 * app.scale
                            width: parent.width
                            wrapMode: Text.Wrap
                        }
                        Repeater {
                            id: verseRepeater
                            model: bible.verses
                            delegate: Item {
                                id: verseRow
                                required property var modelData
                                required property int index
                                readonly property bool selected: modelData.number >= bible.selectedStart && modelData.number <= bible.selectedEnd && bible.selectedStart > 0
                                readonly property real gutter: 40 * app.scale
                                width: readerColumn.width
                                height: verseHeading.height + verseBody.height + 7 * app.scale
                                Item {
                                    id: bookmarkMarker
                                    objectName: "bookmarkMarker" + verseRow.modelData.number
                                    x: 0
                                    width: 14 * app.scale
                                    height: parent.height
                                    visible: verseRow.modelData.bookmarked
                                    readonly property bool starts: !!verseRow.modelData.bookmarkStart
                                    readonly property bool ends: !!verseRow.modelData.bookmarkEnd
                                    readonly property bool continuesBefore: !!verseRow.modelData.bookmarkContinuesBefore
                                    readonly property bool continuesAfter: !!verseRow.modelData.bookmarkContinuesAfter
                                    readonly property bool chapterContinuation: verseRow.index === 0 && continuesBefore
                                    readonly property var ranges: Array.from(verseRow.modelData.bookmarkRanges || []).sort(function(a, b) {
                                        return a.reference.localeCompare(b.reference)
                                    })
                                    readonly property string openReference: {
                                        let starting = ranges.filter(function(range) { return range.bookmarkStart })
                                        return (starting.length ? starting : ranges)[0]?.reference || ""
                                    }
                                    readonly property string hint: ranges.map(function(range) { return range.reference }).join("\n")
                                        + "\nOpen " + openReference + " · m · Ctrl+D toggles bookmark"
                                    // Adjacent row segments meet exactly, including headings and
                                    // chapter continuation. Caps retain logical overlapping endpoints.
                                    Rectangle {
                                        objectName: "bookmarkLine" + verseRow.modelData.number
                                        x: 5 * app.scale
                                        y: bookmarkMarker.continuesBefore ? 0 : verseHeading.height + 8 * app.scale
                                        width: app.scale
                                        height: (bookmarkMarker.continuesAfter ? parent.height : verseHeading.height + verseBody.height) - y
                                        color: desktop.accent
                                        opacity: 0.65
                                    }
                                    Rectangle {
                                        x: 5 * app.scale; y: verseHeading.height + verseBody.height - app.scale
                                        width: 7 * app.scale; height: app.scale
                                        visible: bookmarkMarker.ends
                                        color: desktop.accent
                                    }
                                    BookmarkGlyph {
                                        objectName: "bookmarkGlyph" + verseRow.modelData.number
                                        y: verseHeading.height + 5 * app.scale
                                        visible: bookmarkMarker.starts
                                    }
                                    MouseArea {
                                        id: bookmarkMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: app.openBookmark(bookmarkMarker.openReference)
                                    }
                                    ToolTip.visible: bookmarkMouse.containsMouse
                                    ToolTip.text: hint
                                    ToolTip.delay: 500
                                    Accessible.role: Accessible.Button
                                    Accessible.name: "Bookmarked passages: " + ranges.map(function(range) { return range.reference }).join(", ")
                                    Accessible.onPressAction: app.openBookmark(openReference)
                                }
                                Item {
                                    id: verseHeading
                                    objectName: "verseHeading" + verseRow.modelData.number
                                    readonly property bool hasHeading: (verseRow.modelData.heading || "").length > 0
                                    width: parent.width - verseRow.gutter - 6
                                    x: verseRow.gutter
                                    // Measure text at a fixed width, independent of effective
                                    // visibility. First import creates rows inside a hidden
                                    // reader; Label height/implicitHeight/padding feedback
                                    // otherwise re-enters when that ancestor becomes visible.
                                    height: hasHeading ? headingText.implicitHeight + 36 * app.scale : 0
                                    visible: hasHeading
                                    Text {
                                        id: headingText
                                        width: parent.width
                                        y: 22 * app.scale
                                        text: verseRow.modelData.heading || ""
                                        textFormat: Text.PlainText
                                        font.family: desktop.fontFamily
                                        font.pixelSize: 15 * app.scale
                                        font.weight: Font.DemiBold
                                        wrapMode: Text.Wrap
                                        color: desktop.foreground
                                    }
                                }
                                Rectangle {
                                    x: verseRow.gutter - 9
                                    y: verseHeading.height
                                    width: parent.width - x
                                    height: verseBody.height + 5
                                    radius: 0
                                    color: verseRow.selected ? app.tint(desktop.accent, 0.13) : verseRow.modelData.highlighted ? app.tint(desktop.accent, 0.085) : "transparent"
                                    border.width: verseRow.selected ? 1 : 0
                                    border.color: app.tint(desktop.accent, 0.3)
                                }
                                Rectangle {
                                    x: verseRow.gutter - 9
                                    y: verseHeading.height + 4
                                    height: Math.max(12, verseBody.height - 4)
                                    width: 2
                                    radius: 0
                                    visible: verseRow.modelData.highlighted
                                    color: desktop.accent
                                    opacity: 0.7
                                }
                                Item {
                                    id: verseNumber
                                    objectName: "verseNumber" + verseRow.modelData.number
                                    x: 14 * app.scale
                                    width: 17 * app.scale
                                    y: verseHeading.height
                                    height: Math.max(32, verseBody.height)
                                    Accessible.role: Accessible.Button
                                    Accessible.name: "Verse " + verseRow.modelData.number + (verseRow.modelData.hasNote ? ", has note" : "") + (verseRow.modelData.bookmarked ? ", bookmarked" : "")
                                    Accessible.onPressAction: app.selectVerse(verseRow.modelData.number, false)
                                    Label {
                                        anchors.top: parent.top
                                        anchors.topMargin: 7 * app.scale
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        text: verseRow.modelData.number
                                        font.pixelSize: 10 * app.scale
                                        font.weight: verseRow.selected ? Font.Bold : Font.Normal
                                        color: verseRow.selected ? desktop.accent : desktop.muted
                                    }
                                    Canvas {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        y: 24 * app.scale
                                        width: 8 * app.scale; height: 10 * app.scale
                                        visible: verseRow.modelData.hasNote
                                        property color ink: desktop.muted
                                        onInkChanged: requestPaint()
                                        onPaint: {
                                            let ctx = getContext("2d")
                                            ctx.clearRect(0, 0, width, height)
                                            ctx.strokeStyle = ink; ctx.lineWidth = app.scale
                                            ctx.strokeRect(app.scale / 2, app.scale / 2, width - app.scale, height - app.scale)
                                            ctx.beginPath()
                                            ctx.moveTo(width * 0.25, height * 0.35); ctx.lineTo(width * 0.75, height * 0.35)
                                            ctx.moveTo(width * 0.25, height * 0.65); ctx.lineTo(width * 0.75, height * 0.65)
                                            ctx.stroke()
                                        }
                                    }
                                    MouseArea {
                                        id: numberMouse
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        hoverEnabled: true
                                        onClicked: function(mouse) { app.selectVerse(verseRow.modelData.number, !!(mouse.modifiers & Qt.ShiftModifier)) }
                                        onDoubleClicked: { app.selectVerse(verseRow.modelData.number, false); app.openNotes(true) }
                                    }
                                    ToolTip.visible: numberMouse.containsMouse
                                    ToolTip.delay: 700
                                    ToolTip.text: "Select verse " + verseRow.modelData.number + " · Shift-click for a range" + (verseRow.modelData.hasNote ? "\nHas a note" : "") + (verseRow.modelData.bookmarked ? "\nBookmarked" : "")
                                }
                                TextEdit {
                                    id: verseBody
                                    x: verseRow.gutter + (verseRow.modelData.poetry ? 13 : 0)
                                    y: verseHeading.height
                                    width: parent.width - x - 9
                                    height: contentHeight
                                    text: verseRow.modelData.html ? '<div style="line-height:145%">' + verseRow.modelData.html + '</div>' : verseRow.modelData.text
                                    textFormat: verseRow.modelData.html ? Text.RichText : Text.PlainText
                                    font.family: desktop.fontFamily
                                    font.pixelSize: app.readerSize * app.scale
                                    color: desktop.foreground
                                    selectionColor: app.tint(desktop.accent, 0.35)
                                    selectedTextColor: desktop.foreground
                                    readOnly: true
                                    selectByMouse: true
                                    wrapMode: TextEdit.Wrap
                                    persistentSelection: false
                                    activeFocusOnTab: false
                                    padding: 0
                                    onLinkActivated: function(link) { desktop.openExternal(link) }
                                    Accessible.name: "Verse " + verseRow.modelData.number + ": " + verseRow.modelData.text
                                }
                            }
                        }
                        Label {
                            visible: bible.verses.length === 0 && !bible.busy
                            text: "This chapter has no text. Open another passage or import your NET EPUB again."
                            color: desktop.muted
                            width: parent.width
                            wrapMode: Text.Wrap
                            padding: 24
                        }
                        FineRule { width: parent.width - 36; x: 36; height: 1; visible: bible.verses.length > 0 }
                        RowLayout {
                            width: parent.width - 36
                            x: 36
                            height: 76 * app.scale
                            QuietButton { text: "←  Previous"; enabled: !(bible.book === 1 && bible.chapter === 1); onClicked: app.chapterStep(-1) }
                            Item { Layout.fillWidth: true }
                            QuietButton { text: "Next chapter  →"; enabled: !(bible.book === 66 && bible.chapter === bible.chapterCount); onClicked: app.chapterStep(1) }
                        }
                    }
                }
            }

            ScrollView {
                id: setupScroll
                objectName: "setupScroll"
                anchors.fill: parent
                visible: !bible.ready && !bible.busy
                contentWidth: availableWidth
                contentHeight: setupContent.implicitHeight + 64
                clip: true
            ColumnLayout {
                id: setupContent
                x: (setupScroll.availableWidth - width) / 2
                y: Math.max(32, (setupScroll.availableHeight - implicitHeight) / 2)
                width: Math.min(470 * app.scale, setupScroll.availableWidth - 80)
                spacing: 20
                Rectangle {
                    Layout.alignment: Qt.AlignHCenter
                    implicitWidth: 64
                    implicitHeight: 76
                    radius: 0
                    color: desktop.surface
                    border.color: desktop.border
                    Rectangle { x: 10; y: 0; width: 1; height: parent.height; color: desktop.border }
                    Label { anchors.centerIn: parent; anchors.horizontalCenterOffset: 4; text: "N"; color: desktop.accent; font.pixelSize: 32; font.weight: Font.Medium }
                }
                SectionLabel { text: "A QUIET PLACE TO READ"; Layout.alignment: Qt.AlignHCenter; Layout.topMargin: 10 }
                Label { text: "Make room for the Word."; font.pixelSize: 30 * app.scale; font.weight: Font.Medium; font.letterSpacing: -0.8; color: desktop.foreground; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap; Layout.fillWidth: true }
                Label {
                    text: "Bring your NET Bible 2.1 EPUB to begin. Read offline, mark passages, and keep your thoughts alongside the text."
                    color: desktop.muted
                    font.pixelSize: 15 * app.scale
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    lineHeight: 1.4
                    Layout.fillWidth: true
                }
                QuietButton { text: "Choose NET EPUB…"; emphasized: true; Layout.alignment: Qt.AlignHCenter; Layout.topMargin: 6; onClicked: importDialog.open() }
                Label { text: "Your Bible and annotations stay on this device."; color: desktop.muted; font.pixelSize: 12 * app.scale; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap; Layout.fillWidth: true }
                QuietButton { text: "About the NET Bible ↗"; compact: true; Layout.alignment: Qt.AlignHCenter; onClicked: aboutDialog.open() }
            }
            }

            Rectangle {
                anchors.fill: parent
                visible: app.notesOpen && !app.wideLayout && bible.ready
                color: app.tint(desktop.background, 0.55)
                MouseArea { anchors.fill: parent; onClicked: { app.flushNote(); app.notesOpen = false; app.readerFocus() } }
            }
            Rectangle {
                id: notesPanel
                objectName: "notesPanel"
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: app.notesWidth
                visible: app.notesOpen && bible.ready
                color: desktop.surface
                Rectangle { anchors.left: parent.left; width: 1; height: parent.height; color: desktop.border }
                ScrollView {
                    id: notesScroll
                    objectName: "notesScroll"
                    anchors.fill: parent
                    contentWidth: availableWidth
                    contentHeight: notesContent.height + 44
                    clip: true
                ColumnLayout {
                    id: notesContent
                    x: 22
                    y: 22
                    width: notesScroll.availableWidth - 44
                    height: Math.max(notesPanel.height - 44, 440 * app.scale)
                    spacing: 16
                    RowLayout {
                        Layout.fillWidth: true
                        SectionLabel { text: "PASSAGE NOTES"; Layout.fillWidth: true }
                        QuietButton { text: "×"; compact: true; tip: "Close notes · Escape"; Accessible.name: "Close notes"; onClicked: { app.flushNote(); app.notesOpen = false; app.readerFocus() } }
                    }
                    Label {
                        text: app.hasSelection ? bible.selectionReference : "A thought worth keeping"
                        color: desktop.foreground
                        font.pixelSize: 21 * app.scale
                        font.weight: Font.Medium
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                    RowLayout {
                        visible: app.hasSelection
                        spacing: 4
                        QuietButton { text: bible.selectionBookmarked ? "Bookmarked" : "Bookmark"; emphasized: bible.selectionBookmarked; compact: true; onClicked: bible.toggleBookmark(); tip: "m · Ctrl+D" }
                        QuietButton { text: bible.selectionHighlighted ? "Highlighted" : "Highlight"; emphasized: bible.selectionHighlighted; compact: true; onClicked: bible.toggleHighlight(); tip: "x · Ctrl+H" }
                        Item { Layout.fillWidth: true }
                        QuietButton { text: "Copy"; compact: true; onClicked: app.copyPassage(); tip: "Copy selected passage · y · Ctrl+Shift+C" }
                    }
                    FineRule { Layout.fillWidth: true }
                    RowLayout {
                        visible: app.hasSelection
                        spacing: 4
                        QuietButton { text: "Write"; compact: true; emphasized: !app.notePreview; onClicked: { app.notePreview = false; noteEditor.forceActiveFocus() } }
                        QuietButton { objectName: "previewButton"; text: "Preview"; compact: true; emphasized: app.notePreview; onClicked: { app.flushNote(); app.notePreview = true } }
                        Item { Layout.fillWidth: true }
                        Label { text: "Markdown"; color: desktop.muted; font.pixelSize: 11 * app.scale }
                    }
                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        ScrollView {
                            anchors.fill: parent
                            visible: app.hasSelection && !app.notePreview
                            clip: true
                            contentWidth: availableWidth
                            TextArea {
                                id: noteEditor
                                objectName: "noteEditor"
                                width: parent.width
                                placeholderText: "What stands out to you?\n\nWrite a note in Markdown…"
                                placeholderTextColor: desktop.muted
                                color: desktop.foreground
                                selectionColor: app.tint(desktop.accent, 0.35)
                                selectedTextColor: desktop.foreground
                                font.family: desktop.fontFamily
                                font.pixelSize: 15 * app.scale
                                wrapMode: TextEdit.Wrap
                                selectByMouse: true
                                enabled: app.hasSelection && !bible.busy
                                padding: 2
                                background: Item {}
                                onActiveFocusChanged: {
                                    if (activeFocus) Qt.callLater(function() {
                                        let top = noteEditor.mapToItem(notesContent, 0, 0).y + 22
                                        let viewport = notesScroll.contentItem
                                        if (top + 120 * app.scale > viewport.contentY + notesScroll.availableHeight)
                                            viewport.contentY = Math.max(0, Math.min(top - 12, notesScroll.contentHeight - notesScroll.availableHeight))
                                    })
                                }
                                onTextChanged: {
                                    if (!app.syncingNote && app.hasSelection && app.loadedNoteReference === bible.selectionReference && text !== bible.noteText)
                                        bible.setNoteText(text)
                                }
                                Keys.onEscapePressed: function(event) { app.flushNote(); app.readerFocus(); event.accepted = true }
                                Accessible.name: "Markdown note for " + bible.selectionReference
                            }
                        }
                        ScrollView {
                            anchors.fill: parent
                            visible: app.hasSelection && app.notePreview
                            clip: true
                            contentWidth: availableWidth
                            TextArea {
                                objectName: "notePreview"
                                width: parent.width
                                text: noteEditor.text.length ? noteEditor.text : "*Your note preview will appear here.*"
                                textFormat: TextEdit.MarkdownText
                                readOnly: true
                                selectByMouse: true
                                wrapMode: TextEdit.Wrap
                                color: noteEditor.text.length ? desktop.foreground : desktop.muted
                                font.pixelSize: 15 * app.scale
                                padding: 2
                                background: Item {}
                                onLinkActivated: function(link) { desktop.openExternal(link) }
                                Accessible.name: "Note preview"
                            }
                        }
                        Label {
                            anchors.top: parent.top
                            width: parent.width
                            visible: !app.hasSelection
                            text: "Select a verse number to start a note.\n\nShift-click another number to gather a range. Your notes are saved automatically as you write."
                            color: desktop.muted
                            font.pixelSize: 14 * app.scale
                            lineHeight: 1.5
                            wrapMode: Text.Wrap
                        }
                    }
                    Label { text: app.hasSelection ? (noteEditor.text === bible.noteText ? "Saved on this device · changes save as you type" : "Not saved · your draft is still in the editor") : "A page icon beside a verse marks a note."; color: desktop.muted; font.pixelSize: 11 * app.scale; wrapMode: Text.Wrap; Layout.fillWidth: true }
                }
                }
            }
        }
    }

    footer: Rectangle {
        height: 46 * app.scale
        color: desktop.background
        FineRule { anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top }
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 20
            anchors.rightMargin: 20
            spacing: 8
            QuietButton { text: "←"; compact: true; enabled: bible.ready && !bible.busy && !(bible.book === 1 && bible.chapter === 1); tip: "Previous chapter · h · [ · Alt+Left"; Accessible.name: "Previous chapter"; onClicked: app.chapterStep(-1) }
            QuietButton { text: "→"; compact: true; enabled: bible.ready && !bible.busy && !(bible.book === 66 && bible.chapter === bible.chapterCount); tip: "Next chapter · l · ] · Alt+Right"; Accessible.name: "Next chapter"; onClicked: app.chapterStep(1) }
            Label { text: app.hasSelection ? bible.selectionReference : bible.ready ? bible.passage : "NET 2.1 · Offline reader"; color: desktop.muted; font.pixelSize: 12 * app.scale; elide: Text.ElideRight; Layout.fillWidth: true }
            QuietButton { text: bible.selectionBookmarked ? "★" : "☆"; visible: app.hasSelection; emphasized: bible.selectionBookmarked; compact: true; tip: "Bookmark · m · Ctrl+D"; Accessible.name: "Toggle bookmark"; onClicked: bible.toggleBookmark() }
            QuietButton { text: "Mark"; visible: app.hasSelection; emphasized: bible.selectionHighlighted; compact: true; tip: "Highlight · x · Ctrl+H"; onClicked: bible.toggleHighlight() }
            QuietButton { text: app.verseMode ? "Visual" : "Normal"; visible: bible.ready; emphasized: app.verseMode; compact: true; tip: "Toggle visual selection · v"; onClicked: { app.setVisualMode(!app.verseMode); app.readerFocus() } }
            QuietButton { text: "?"; compact: true; tip: "Keybindings · ? · F1"; Accessible.name: "Keybindings"; onClicked: helpDialog.open() }
        }
    }

    ModalCard {
        id: passagePalette
        objectName: "passagePalette"
        width: Math.min(650 * app.scale, app.width - 48)
        padding: 16
        onOpened: referenceInput.forceActiveFocus()
        onClosed: app.readerFocus()
        contentItem: ColumnLayout {
            spacing: 12
            RowLayout {
                SectionLabel { text: app.paletteBook ? "CHOOSE A CHAPTER" : "GO TO A PASSAGE"; Layout.fillWidth: true }
                QuietButton { text: "Books"; compact: true; emphasized: !referenceInput.text.startsWith(">"); enabled: bible.ready; onClicked: { app.paletteBook = 0; referenceInput.text = ""; referenceInput.forceActiveFocus() } }
                QuietButton { text: "Commands"; compact: true; emphasized: referenceInput.text.startsWith(">"); onClicked: { app.paletteBook = 0; referenceInput.text = "> "; referenceInput.forceActiveFocus() } }
            }
            TextField {
                id: referenceInput
                objectName: "referenceInput"
                Layout.fillWidth: true
                implicitHeight: 52 * app.scale
                font.pixelSize: 19 * app.scale
                color: desktop.foreground
                placeholderText: app.paletteBook ? app.paletteBookName + " — chapter number" : "John 3:16–18, a book, or > command"
                placeholderTextColor: desktop.muted
                selectByMouse: true
                leftPadding: 14
                rightPadding: 14
                background: Rectangle { color: desktop.surface; radius: 0; border.color: referenceInput.activeFocus ? app.tint(desktop.accent, 0.65) : desktop.border }
                onTextChanged: { app.paletteArrowUsed = false; paletteResults.currentIndex = 0 }
                onAccepted: app.acceptPalette()
                Keys.onDownPressed: { app.paletteArrowUsed = true; paletteResults.currentIndex = Math.min(paletteResults.count - 1, paletteResults.currentIndex + 1); paletteResults.positionViewAtIndex(paletteResults.currentIndex, ListView.Contain) }
                Keys.onUpPressed: { app.paletteArrowUsed = true; paletteResults.currentIndex = Math.max(0, paletteResults.currentIndex - 1); paletteResults.positionViewAtIndex(paletteResults.currentIndex, ListView.Contain) }
                Keys.onRightPressed: function(event) {
                    let entry = paletteResults.model[paletteResults.currentIndex]
                    let browsing = (!text.trim() || /^[1-3]?\s*[A-Za-z ]+$/.test(text.trim()))
                        && (app.paletteArrowUsed || (cursorPosition === text.length && selectedText.length === 0))
                    event.accepted = event.modifiers === Qt.NoModifier && browsing && !!entry && !!entry.book && !entry.chapter
                    if (event.accepted) app.activatePaletteEntry(entry)
                }
                Keys.onEscapePressed: passagePalette.close()
                Accessible.name: "Passage reference or command"
            }
            Label {
                objectName: "referencePreview"
                visible: !referenceInput.text.trim().startsWith(">") && (referenceInput.text.trim().length > 0 || app.paletteBook > 0)
                text: app.paletteResolution.valid ? app.paletteResolution.reference : "Invalid or incomplete reference · " + app.paletteResolution.error
                color: app.paletteResolution.valid ? desktop.accent : desktop.muted
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                font.pixelSize: 12 * app.scale
            }
            QuietButton { text: "← All books"; compact: true; visible: app.paletteBook > 0; onClicked: { app.paletteBook = 0; referenceInput.text = ""; referenceInput.forceActiveFocus() } }
            ListView {
                id: paletteResults
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(340 * app.scale, app.height * 0.43)
                clip: true
                model: app.paletteEntries()
                currentIndex: 0
                spacing: 3
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                delegate: ItemDelegate {
                    id: paletteEntry
                    required property var modelData
                    required property int index
                    width: paletteResults.width
                    height: 44 * app.scale
                    highlighted: ListView.isCurrentItem
                    onClicked: app.activatePaletteEntry(modelData)
                    contentItem: RowLayout {
                        Label { text: paletteEntry.modelData.title; color: desktop.foreground; Layout.fillWidth: true; elide: Text.ElideRight; font.pixelSize: 14 * app.scale }
                        Label { text: paletteEntry.modelData.hint; color: desktop.muted; font.pixelSize: 11 * app.scale }
                        Label { text: paletteEntry.modelData.action || paletteEntry.modelData.chapter ? "↵" : "›"; color: desktop.muted; Layout.leftMargin: 8 }
                    }
                    background: Rectangle { radius: 0; color: paletteEntry.highlighted ? app.tint(desktop.accent, 0.11) : paletteEntry.hovered ? desktop.surface : "transparent" }
                }
                Label {
                    anchors.centerIn: parent
                    width: parent.width - 32
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: referenceInput.text.startsWith(">") ? "No matching commands" : "Import your NET EPUB to browse passages."
                    visible: paletteResults.count === 0 && (referenceInput.text.startsWith(">") || !bible.ready)
                    color: desktop.muted
                    font.pixelSize: 13 * app.scale
                }
            }
            FineRule { Layout.fillWidth: true }
            RowLayout {
                Layout.fillWidth: true
                Label { text: "↑ ↓ choose    → enter book    ↵ open    Esc close"; color: desktop.muted; font.pixelSize: 11 * app.scale; Layout.fillWidth: true }
                Label { text: "Reference navigation · offline"; visible: app.width >= 760; color: desktop.muted; font.pixelSize: 11 * app.scale }
            }
        }
    }

    Drawer {
        id: savedDrawer
        objectName: "savedDrawer"
        width: Math.min(430 * app.scale, app.width - 48)
        height: app.height
        edge: Qt.LeftEdge
        modal: true
        padding: 24 * app.scale
        topPadding: padding
        bottomPadding: padding
        leftPadding: padding
        rightPadding: padding
        background: Rectangle { color: desktop.background; border.color: desktop.border }
        Overlay.modal: Rectangle { color: app.tint(desktop.background, 0.65) }
        onClosed: app.readerFocus()
        contentItem: ColumnLayout {
            objectName: "savedContent"
            spacing: (savedDrawer.height < 350 * app.scale ? 6 : 12) * app.scale
            RowLayout {
                Layout.fillWidth: true
                Label { text: "Saved passages"; font.pixelSize: 24 * app.scale; font.weight: Font.Medium; color: desktop.foreground; Layout.fillWidth: true; wrapMode: Text.Wrap }
                QuietButton { text: "×"; compact: true; Accessible.name: "Close saved passages"; onClicked: savedDrawer.close() }
            }
            Label { text: "Return to what stayed with you."; visible: savedDrawer.height > 400 * app.scale; color: desktop.muted; font.pixelSize: 13 * app.scale; Layout.fillWidth: true; wrapMode: Text.Wrap }
            GridLayout {
                id: savedTabs
                objectName: "savedTabs"
                Layout.fillWidth: true
                columns: width < 350 * app.scale ? 2 : 4
                columnSpacing: 4 * app.scale
                rowSpacing: 2 * app.scale
                Repeater {
                    model: ["All", "Bookmarks", "Highlights", "Notes"]
                    delegate: QuietButton {
                        required property string modelData
                        required property int index
                        objectName: "savedTab" + index
                        text: modelData
                        emphasized: app.collectionTab === index
                        compact: true
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.preferredWidth: 1
                        padding: 6 * app.scale
                        font.pixelSize: 11 * app.scale
                        onClicked: app.collectionTab = index
                    }
                }
            }
            FineRule { Layout.fillWidth: true }
            ListView {
                id: collectionList
                objectName: "collectionList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 6 * app.scale
                model: app.filteredCollection()
                ScrollBar.vertical: ScrollBar { id: savedScrollbar; objectName: "savedScrollbar" }
                delegate: ItemDelegate {
                    id: savedEntry
                    objectName: "savedEntry" + modelData.reference
                    required property var modelData
                    width: collectionList.width - savedScrollbar.width - 8 * app.scale
                    padding: 12 * app.scale
                    height: savedContent.implicitHeight + topPadding + bottomPadding
                    clip: true
                    onClicked: {
                        if (!app.flushNote()) return
                        app.savePosition()
                        if (!bible.navigate(modelData.reference)) return
                        app.syncNote()
                        savedDrawer.close()
                        if (modelData.note.length > 0) app.openNotes(false)
                        Qt.callLater(function() { app.scrollToVerse(bible.cursorVerse, true) })
                    }
                    contentItem: ColumnLayout {
                        id: savedContent
                        objectName: "savedEntryContent" + savedEntry.modelData.reference
                        spacing: 7 * app.scale
                        Label { text: savedEntry.modelData.reference; color: desktop.foreground; font.pixelSize: 16 * app.scale; font.weight: Font.Medium; Layout.fillWidth: true; wrapMode: Text.Wrap }
                        Label {
                            text: [savedEntry.modelData.bookmark ? "Bookmark" : "", savedEntry.modelData.highlight ? "Highlight" : "", savedEntry.modelData.note.length ? "Note" : ""].filter(function(s) { return s.length > 0 }).join("  ·  ")
                            color: desktop.accent
                            font.pixelSize: 10 * app.scale
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                        }
                        Label { text: savedEntry.modelData.note.replace(/\s+/g, " "); textFormat: Text.PlainText; visible: text.length > 0; color: desktop.muted; font.pixelSize: 12 * app.scale; maximumLineCount: 3; elide: Text.ElideRight; wrapMode: Text.Wrap; Layout.fillWidth: true }
                    }
                    background: Rectangle { color: savedEntry.hovered || savedEntry.activeFocus ? desktop.surface : "transparent"; radius: 0; border.width: savedEntry.activeFocus ? 1 : 0; border.color: desktop.accent }
                }
                Label {
                    anchors.centerIn: parent
                    width: parent.width - 24
                    text: "Nothing here yet.\n\nSelect a verse, then add a bookmark, highlight, or note. Your saved passages will appear here."
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    color: desktop.muted
                    font.pixelSize: 14 * app.scale
                    lineHeight: 1.4
                    visible: collectionList.count === 0
                }
            }
            FineRule { Layout.fillWidth: true }
            RowLayout {
                Layout.fillWidth: true
                QuietButton { text: "Export notes…"; compact: true; Layout.fillWidth: true; Layout.minimumWidth: 0; onClicked: { savedDrawer.close(); app.runCommand("export") } }
                Item { Layout.fillWidth: true }
                QuietButton { text: "Backup…"; compact: true; Layout.fillWidth: true; Layout.minimumWidth: 0; onClicked: { savedDrawer.close(); app.runCommand("backup") } }
            }
        }
    }

    ModalCard {
        id: helpDialog
        objectName: "helpDialog"
        height: Math.min(660 * app.scale, app.height - 48)
        onOpened: { keybindingSearch.text = ""; keybindingSearch.forceActiveFocus() }
        onClosed: app.readerFocus()
        contentItem: ColumnLayout {
            spacing: 16 * app.scale
            RowLayout {
                Label { text: "Keybindings"; color: desktop.foreground; font.pixelSize: 23 * app.scale; font.weight: Font.Medium; Layout.fillWidth: true; wrapMode: Text.Wrap }
                QuietButton { text: "×"; compact: true; Accessible.name: "Close keybindings"; onClicked: helpDialog.close() }
            }
            TextField {
                id: keybindingSearch
                objectName: "keybindingSearch"
                Layout.fillWidth: true
                implicitHeight: 42 * app.scale
                placeholderText: "Keybindings..."
                font.pixelSize: 15 * app.scale
                selectByMouse: true
                padding: 10 * app.scale
                background: Rectangle { color: desktop.surface; border.color: keybindingSearch.activeFocus ? desktop.accent : desktop.border }
                onTextChanged: keybindingResults.positionViewAtBeginning()
                Keys.onEscapePressed: helpDialog.close()
                Accessible.name: "Search keybindings by action or key"
            }
            ListView {
                id: keybindingResults
                objectName: "keybindingResults"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 12 * app.scale
                model: app.filteredKeybindings()
                ScrollBar.vertical: ScrollBar { id: keybindingScrollbar }
                delegate: RowLayout {
                    id: keybindingRow
                    required property var modelData
                    objectName: "keybinding" + modelData.action
                    width: keybindingResults.width - keybindingScrollbar.width - 8 * app.scale
                    spacing: 16 * app.scale
                    Label {
                        text: keybindingRow.modelData.action; color: desktop.foreground
                        font.pixelSize: 13 * app.scale; Layout.fillWidth: true
                        Layout.preferredWidth: keybindingRow.width * 0.48; wrapMode: Text.Wrap
                    }
                    Label {
                        text: keybindingRow.modelData.keys; color: desktop.muted
                        font.pixelSize: 12 * app.scale; horizontalAlignment: Text.AlignRight
                        Layout.fillWidth: true; Layout.preferredWidth: keybindingRow.width * 0.48; wrapMode: Text.Wrap
                    }
                }
                Label {
                    objectName: "keybindingEmpty"
                    anchors.centerIn: parent
                    width: parent.width
                    text: "No matching keybindings"
                    visible: keybindingResults.count === 0
                    horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap
                    color: desktop.muted; font.pixelSize: 14 * app.scale
                }
            }
            Label { text: "Reading keys pause while you type. Bookmark ribbons mark range starts; margin lines continue through the range. A page icon marks a note."; visible: helpDialog.height > 500 * app.scale; color: desktop.muted; font.pixelSize: 12 * app.scale; Layout.fillWidth: true; wrapMode: Text.Wrap }
        }
    }

    ModalCard {
        id: aboutDialog
        objectName: "aboutDialog"
        contentItem: ScrollView {
            id: aboutScroll
            implicitHeight: Math.min(aboutContent.implicitHeight, app.height - 110)
            contentWidth: availableWidth
            clip: true
        ColumnLayout {
            id: aboutContent
            width: aboutScroll.availableWidth
            spacing: 16
            RowLayout {
                Label { text: "Omabible"; color: desktop.foreground; font.pixelSize: 30 * app.scale; font.weight: Font.Medium; font.letterSpacing: -1; Layout.fillWidth: true }
                QuietButton { text: "×"; compact: true; Accessible.name: "Close about"; onClicked: aboutDialog.close() }
            }
            SectionLabel { text: "READ SLOWLY. KEEP WHAT MATTERS." }
            Label { text: "An offline NET Bible reader with local bookmarks, highlights, and Markdown notes. Built with Qt, at home on your desktop."; color: desktop.foreground; wrapMode: Text.Wrap; Layout.fillWidth: true; font.pixelSize: 14 * app.scale; lineHeight: 1.4 }
            FineRule { Layout.fillWidth: true }
            Label { text: "New English Translation (NET Bible)\nNET Bible® copyright © 1996–2019 by Biblical Studies Press, L.L.C. All rights reserved. The Bible text is separately copyrighted and is imported from your own NET 2.1 EPUB; it is not distributed with Omabible."; color: desktop.muted; wrapMode: Text.Wrap; Layout.fillWidth: true; font.pixelSize: 12 * app.scale; lineHeight: 1.4 }
            QuietButton { text: "NET Bible & permissions ↗"; compact: true; onClicked: desktop.openExternal("https://netbible.com/copyright/") }
            Label { text: "Omabible " + desktop.version + " · Application code licensed under MIT\n" + (bible.edition || "NET 2.1") + " · No account required"; color: desktop.muted; font.pixelSize: 11 * app.scale; wrapMode: Text.Wrap; Layout.fillWidth: true }
            QuietButton { text: "Open local data folder ↗"; compact: true; onClicked: desktop.openExternal("file://" + bible.dataDirectory()) }
        }
        }
    }

    FileDialog {
        id: importDialog
        title: "Import NET Bible 2.1 EPUB"
        fileMode: FileDialog.OpenFile
        nameFilters: ["EPUB books (*.epub)"]
        onAccepted: { if (!app.flushNote()) return; bible.clearError(); bible.importEpub(selectedFile) }
    }
    FolderDialog {
        id: exportDialog
        title: "Choose a folder for Markdown notes"
        onAccepted: { if (!app.flushNote()) return; if (bible.exportNotes(selectedFolder)) app.notify("Notes exported as Markdown") }
    }
    FileDialog {
        id: backupDialog
        title: "Save annotation backup"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "json"
        nameFilters: ["JSON backup (*.json)"]
        onAccepted: { if (!app.flushNote()) return; if (bible.exportBackup(selectedFile)) app.notify("Annotation backup saved") }
    }
    FileDialog {
        id: restoreDialog
        title: "Restore annotation backup"
        fileMode: FileDialog.OpenFile
        nameFilters: ["JSON backup (*.json)"]
        onAccepted: app.requestRestore(selectedFile)
    }

    ModalCard {
        id: restoreConfirmation
        objectName: "restoreConfirmation"
        property url backupFile
        closePolicy: Popup.CloseOnEscape
        onOpened: restoreCancel.forceActiveFocus()
        onClosed: { backupFile = ""; app.readerFocus() }
        contentItem: ColumnLayout {
            spacing: 18
            Label {
                text: "Restore this backup?"
                color: desktop.foreground
                font.pixelSize: 23 * app.scale
                font.weight: Font.Medium
                Layout.fillWidth: true
                wrapMode: Text.Wrap
            }
            Label {
                objectName: "restoreBackupPath"
                text: restoreConfirmation.backupFile.toString()
                textFormat: Text.PlainText
                color: desktop.muted
                font.pixelSize: 12 * app.scale
                Layout.fillWidth: true
                wrapMode: Text.WrapAnywhere
            }
            Label {
                text: "This replaces all current bookmarks, highlights, notes, and your reading location with the contents of this backup. Your imported Bible stays in place."
                color: desktop.foreground
                font.pixelSize: 14 * app.scale
                Layout.fillWidth: true
                wrapMode: Text.Wrap
            }
            Label {
                visible: bible.error.length > 0
                text: bible.error
                color: desktop.accent
                Layout.fillWidth: true
                wrapMode: Text.Wrap
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                QuietButton {
                    id: restoreCancel
                    objectName: "restoreCancel"
                    text: "Cancel"
                    onClicked: restoreConfirmation.close()
                    Keys.onReturnPressed: restoreConfirmation.close()
                    Keys.onEnterPressed: restoreConfirmation.close()
                }
                QuietButton {
                    objectName: "restoreConfirm"
                    text: "Restore"
                    emphasized: true
                    onClicked: app.confirmRestore()
                    Keys.onReturnPressed: app.confirmRestore()
                    Keys.onEnterPressed: app.confirmRestore()
                }
            }
        }
    }

    Popup {
        id: busyPopup
        objectName: "busyPopup"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(410 * app.scale, app.width - 64)
        padding: 28
        modal: true
        closePolicy: Popup.NoAutoClose
        visible: bible.busy
        background: Rectangle { radius: 0; color: desktop.background; border.color: desktop.border }
        Overlay.modal: Rectangle { color: app.tint(desktop.background, 0.78) }
        contentItem: ColumnLayout {
            spacing: 20
            BusyIndicator { running: bible.busy; Layout.alignment: Qt.AlignHCenter; implicitWidth: 36; implicitHeight: 36 }
            Label { text: "Preparing your library"; color: desktop.foreground; font.pixelSize: 21 * app.scale; font.weight: Font.Medium; Layout.alignment: Qt.AlignHCenter }
            Label { text: bible.status || "Reading and validating your NET EPUB…"; color: desktop.muted; font.pixelSize: 13 * app.scale; wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter; Layout.fillWidth: true }
            ProgressBar {
                id: importProgressBar
                Layout.fillWidth: true
                from: 0
                to: 1
                value: Math.max(0, Math.min(1, bible.importProgress))
                indeterminate: bible.importProgress <= 0
                background: Rectangle { implicitHeight: 3; radius: 0; color: desktop.surface }
                contentItem: Item { implicitHeight: 3; Rectangle { width: importProgressBar.indeterminate ? parent.width * 0.15 : parent.width * importProgressBar.visualPosition; height: parent.height; radius: 0; color: desktop.accent } }
            }
            Label { text: "Everything stays on this device."; color: desktop.muted; font.pixelSize: 11 * app.scale; Layout.alignment: Qt.AlignHCenter }
        }
    }

    Rectangle {
        parent: Overlay.overlay
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 68
        width: Math.min(toast.implicitWidth + 36, app.width - 48)
        height: toast.implicitHeight + 24
        radius: 0
        color: desktop.surface
        border.color: desktop.border
        visible: toastTimer.running
        Label { id: toast; objectName: "toast"; anchors.centerIn: parent; width: parent.width - 36; wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter; color: desktop.foreground; font.pixelSize: 13 * app.scale; Accessible.role: Accessible.AlertMessage }
    }
    Timer { id: toastTimer; interval: 3000 }
    Timer { id: positionSaveTimer; interval: 350; onTriggered: app.savePosition() }
    Timer {
        id: positionTimer
        interval: 80
        onTriggered: {
            if (app.positionJumpToSelection && bible.selectedStart > 0) app.scrollToVerse(bible.cursorVerse, true)
            else reading.contentY = Math.max(0, reading.contentHeight - reading.height) * Math.max(0, Math.min(1, bible.readingPosition))
            app.restoringPosition = false
        }
    }
    Connections {
        target: bible
        function onNavigationChanged() {
            if (!app.movingSelection) {
                app.verseMode = false
                app.selectionAnchor = bible.crossChapterSelection ? 0 : bible.selectedStart
                app.restorePosition(true)
            }
            app.syncNote()
        }
    }
    Component.onCompleted: { syncNote(); restorePosition() }
    onClosing: function(close) { close.accepted = flushNote(); savePosition() }

    Shortcut { sequence: "Ctrl+K"; enabled: !bible.busy && (!app.modalOpen || passagePalette.opened); onActivated: app.openPalette(false) }
    Shortcut { sequence: "Ctrl+Shift+P"; enabled: !bible.busy && (!app.modalOpen || passagePalette.opened); onActivated: app.openPalette(true) }
    Shortcut { sequence: "Ctrl+L"; enabled: app.readerKeys; onActivated: app.runCommand("saved") }
    Shortcut { sequence: "Ctrl+N"; enabled: bible.ready && !bible.busy && !app.modalOpen; onActivated: app.openNotes(true) }
    Shortcut { sequence: "N"; enabled: app.readerKeys; onActivated: app.openNotes(true) }
    Shortcut { sequence: "Ctrl+Shift+N"; enabled: bible.ready && !bible.busy && !app.modalOpen; onActivated: { app.flushNote(); app.notesOpen = !app.notesOpen; app.readerFocus() } }
    Shortcut { sequence: "Ctrl+D"; enabled: app.readerKeys && app.hasSelection; onActivated: bible.toggleBookmark() }
    Shortcut { sequence: "Ctrl+H"; enabled: app.readerKeys && app.hasSelection; onActivated: bible.toggleHighlight() }
    Shortcut { sequence: "Ctrl+Shift+C"; enabled: app.readerKeys; onActivated: app.copyPassage() }
    Shortcut { sequence: "Alt+Left"; enabled: bible.ready && !bible.busy && !app.modalOpen; onActivated: app.chapterStep(-1) }
    Shortcut { sequence: "Alt+Right"; enabled: bible.ready && !bible.busy && !app.modalOpen; onActivated: app.chapterStep(1) }
    Shortcut { sequence: "F1"; enabled: !bible.busy && !app.modalOpen; onActivated: helpDialog.open() }
    Shortcut { sequence: "?"; enabled: app.readerKeys; onActivated: helpDialog.open() }
    Shortcut { sequence: "V"; enabled: app.readerKeys; onActivated: app.setVisualMode(!app.verseMode) }
    Shortcut { sequences: ["J", "Down"]; enabled: app.readerKeys; onActivated: app.moveVerse(1, app.verseMode) }
    Shortcut { sequences: ["K", "Up"]; enabled: app.readerKeys; onActivated: app.moveVerse(-1, app.verseMode) }
    Shortcut { sequences: ["Shift+J", "Shift+Down"]; enabled: app.readerKeys; onActivated: app.extendVerse(1) }
    Shortcut { sequences: ["Shift+K", "Shift+Up"]; enabled: app.readerKeys; onActivated: app.extendVerse(-1) }
    Shortcut { sequences: ["Space", "PgDown", "Ctrl+F"]; enabled: app.readerKeys; onActivated: app.scrollBy(reading.height * 0.85) }
    Shortcut { sequences: ["Shift+Space", "PgUp", "Ctrl+B"]; enabled: app.readerKeys; onActivated: app.scrollBy(-reading.height * 0.85) }
    Shortcut { sequences: ["G", "Home"]; enabled: app.readerKeys; onActivated: app.chapterEdge(false) }
    Shortcut { sequences: ["Shift+G", "End"]; enabled: app.readerKeys; onActivated: app.chapterEdge(true) }
    Shortcut { sequences: ["H", "["]; enabled: app.readerKeys; onActivated: app.chapterStep(-1) }
    Shortcut { sequences: ["L", "]"]; enabled: app.readerKeys; onActivated: app.chapterStep(1) }
    Shortcut { sequence: "M"; enabled: app.readerKeys && app.hasSelection; onActivated: bible.toggleBookmark() }
    Shortcut { sequence: "X"; enabled: app.readerKeys && app.hasSelection; onActivated: bible.toggleHighlight() }
    Shortcut { sequence: "Y"; enabled: app.readerKeys; onActivated: app.copyPassage() }
    Shortcut { sequences: ["Ctrl++", "Ctrl+="]; enabled: app.readerKeys; onActivated: app.readerSize = Math.min(28, app.readerSize + 1) }
    Shortcut { sequence: "Ctrl+-"; enabled: app.readerKeys; onActivated: app.readerSize = Math.max(15, app.readerSize - 1) }
    Shortcut { sequence: "Ctrl+0"; enabled: app.readerKeys; onActivated: app.readerSize = 19 }
    Shortcut {
        sequence: "Escape"
        enabled: app.readerKeys
        onActivated: {
            if (!app.flushNote()) return
            if (app.verseMode) app.setVisualMode(false)
            else if (app.notesOpen) app.notesOpen = false
            else if (app.hasSelection) app.setVisualMode(false)
            app.readerFocus()
        }
    }
}
