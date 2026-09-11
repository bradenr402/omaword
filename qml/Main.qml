pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import QtQml

ApplicationWindow {
    id: app
    objectName: "readerWindow"
    width: 1120
    height: 820
    minimumWidth: 640
    minimumHeight: 480
    visible: true
    title: bible.ready ? bible.passage + " — OmaBible" : "OmaBible"
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
    palette.toolTipBase: desktop.surface
    palette.toolTipText: desktop.foreground

    readonly property bool notesOpen: notesDrawer.visible
    onNotesOpenChanged: clearCount()
    property bool notePreview: false
    property bool syncingNote: true
    property string loadedNoteReference: ""
    readonly property string observedNoteText: bible.noteText
    onObservedNoteTextChanged: syncNote()
    readonly property bool observedReady: bible.ready
    onObservedReadyChanged: { if (observedReady) restorePosition() }
    property bool verseMode: false
    onVerseModeChanged: clearCount()
    property string verseCount: ""
    onActiveFocusItemChanged: clearCount()
    property bool restoringPosition: true
    property bool positionJumpToSelection: false
    property real readerSize: 19
    property int selectionAnchor: 0
    property bool movingSelection: false
    property int collectionTab: 0
    property int paletteBook: 0
    property int paletteChapter: 0
    property string paletteBookName: ""
    property bool paletteArrowUsed: false
    property var paletteSnapshot: null
    property bool paletteCleared: false
    readonly property var paletteDestination: {
        let query = referenceInput.text.trim()
        let entry = paletteResults.model[paletteResults.currentIndex]
        if (entry && (paletteArrowUsed || !query || query.startsWith(">")
                || (paletteBook > 0 && /^\d+$/.test(query))
                || (paletteBook === 0 && !/\d/.test(query)))) {
            return { entry: entry, reference: entry.chapter ? entry.title : "" }
        }
        return { entry: null, reference: paletteBook > 0 && /^[\d:\s–-]+$/.test(query)
            ? paletteBookName + " " + (paletteChapter > 0 && !query.includes(":") ? paletteChapter + ":" : "") + query : query }
    }
    readonly property var paletteResolution: {
        // Relative references depend on the current book, even though the resolver is pure.
        let currentBook = bible.book
        let ready = bible.ready
        let destination = paletteDestination
        if (destination.entry && destination.entry.chapter && !destination.entry.verse && bible.versePicker)
            return { valid: true, reference: "Browse verses in " + destination.entry.title }
        if (destination.entry && !destination.reference)
            return { valid: true, reference: destination.entry.book ? "Browse " + destination.entry.title : destination.entry.title }
        return bible.resolveReference(destination.reference)
    }
    readonly property real scale: desktop.textScale > 0 ? desktop.textScale : 1
    readonly property bool hasSelection: bible.ready && bible.selectedStart > 0
    readonly property bool modalOpen: passagePalette.visible || savedDrawer.visible || deleteConfirmation.visible || helpDialog.visible || settingsDialog.visible || aboutDialog.visible || errorDialog.visible || restoreConfirmation.visible || importDialog.visible || exportDialog.visible || backupDialog.visible || restoreDialog.visible || !!(blockLoader.item as BlockReader)?.annotationMenuOpen
    readonly property bool readerKeys: bible.ready && !bible.busy && !modalOpen && !notesOpen && !textInputActive()
    onReaderKeysChanged: { if (!readerKeys) clearCount() }
    readonly property real notesWidth: Math.min(390 * scale, width - 40)
    readonly property real readerLineHeight: readerMetrics.height * bible.lineHeight
    readonly property real observedLineHeight: bible.lineHeight
    onObservedLineHeightChanged: {
        // Replacing the rich document invalidates Qt's selection offsets.
        // Clear the shared clipboard source in the same update as the paint.
        clearTextSelection()
        draggingText = false
        dragAnchorRow = -1
    }
    readonly property real floatingSafety: bible.focusMode ? 0 : 72 * scale
    readonly property real verseGutter: 40 * scale
    readonly property bool observedBlockMode: bible.blockMode
    onObservedBlockModeChanged: {
        clearTextSelection()
        Qt.callLater(function() { app.restorePosition(app.hasSelection) })
    }
    readonly property bool observedFocusMode: bible.focusMode
    onObservedFocusModeChanged: { toastTimer.stop(); Qt.callLater(app.applyFocusMode) }
    function applyFocusMode() {
        if (observedFocusMode) {
            if (!closeNotes()) { bible.setFocusMode(false); return }
            passagePalette.close(); savedDrawer.close(); helpDialog.close(); aboutDialog.close()
            if (!settingsDialog.visible) readerFocus()
        }
    }
    readonly property real controlScale: Math.min(scale, 1.4)
    FontMetrics { id: readerMetrics; font.family: desktop.fontFamily; font.pixelSize: app.readerSize * app.scale }
    component VerseItem: Item {
        property real textOffset: 0
        property real textHeight: 0
        property TextEdit body
    }
    component SavedEntry: ItemDelegate {
        required property var modelData
        required property int index
    }

    // Positions are TextEdit document offsets (UTF-16), never verse numbers or
    // HTML offsets. Each delegate keeps its own rich layout and selection paint.
    property int dragAnchorRow: -1
    property int dragAnchorPosition: 0
    property bool draggingText: false
    property bool gutterDrag: false
    property point dragOrigin
    property point dragPoint
    property string freeSelectedText: ""
    function clearTextSelection() {
        freeSelectedText = ""
        let block = blockLoader.item as BlockReader
        if (block) block.clearSelection()
        for (let i = 0; i < verseRepeater.count; ++i) {
            let row = verseRepeater.itemAt(i) as VerseItem
            if (row && row.body) row.body.deselect()
        }
    }
    function textHit(point) {
        for (let i = 0; i < verseRepeater.count; ++i) {
            let row = verseRepeater.itemAt(i) as VerseItem
            let p = row.body.mapFromItem(reading, point.x, point.y)
            if (p.y <= row.body.height || i === verseRepeater.count - 1) {
                let position = p.y < 0 ? 0 : p.y > row.body.height ? row.body.length
                    : row.body.positionAt(p.x, p.y)
                return { row: i, position: position }
            }
        }
        return { row: -1, position: 0 }
    }
    function beginTextDrag(index, source, mouse, gutter) {
        dragOrigin = source.mapToItem(reading, mouse.x, mouse.y)
        dragPoint = dragOrigin
        dragAnchorRow = index
        let body = (verseRepeater.itemAt(index) as VerseItem).body
        let p = body.mapFromItem(reading, dragPoint.x, dragPoint.y)
        dragAnchorPosition = body.positionAt(p.x, p.y)
        gutterDrag = gutter
        draggingText = false
    }
    function updateTextDrag(source, mouse) {
        dragPoint = source.mapToItem(reading, mouse.x, mouse.y)
        // Qt exposes this runtime style hint without QML property metadata.
        // qmllint disable missing-property
        if (!draggingText && Math.hypot(dragPoint.x - dragOrigin.x, dragPoint.y - dragOrigin.y) < Qt.styleHints.startDragDistance) return
        // qmllint enable missing-property
        if (!draggingText) {
            clearTextSelection()
            draggingText = true
            if (!gutterDrag) (verseRepeater.itemAt(dragAnchorRow) as VerseItem).body.forceActiveFocus()
        }
        applyTextDrag()
    }
    function applyTextDrag() {
        let hit = textHit(dragPoint)
        if (hit.row < 0 || dragAnchorRow < 0) return
        if (gutterDrag) {
            // The anchor is a real row, so omitted verse numbers are harmless.
            let start = bible.verses[dragAnchorRow].number
            let end = bible.verses[hit.row].number
            if (bible.selectedStart !== Math.min(start, end) || bible.selectedEnd !== Math.max(start, end) || bible.cursorVerse !== end) {
                if (!flushNote()) return
                selectionAnchor = start
                bible.selectVerses(start, end)
                syncNote()
                readerFocus()
            }
            return
        }
        let forward = hit.row > dragAnchorRow || (hit.row === dragAnchorRow && hit.position >= dragAnchorPosition)
        let first = forward ? dragAnchorRow : hit.row
        let last = forward ? hit.row : dragAnchorRow
        let start = forward ? dragAnchorPosition : hit.position
        let end = forward ? hit.position : dragAnchorPosition
        let pieces = []
        for (let i = 0; i < verseRepeater.count; ++i) {
            let body = (verseRepeater.itemAt(i) as VerseItem).body
            if (i < first || i > last) body.deselect()
            else {
                body.select(i === first ? start : 0, i === last ? end : body.length)
                pieces.push(body.selectedText)
            }
        }
        freeSelectedText = pieces.join("\n")
    }
    function finishTextDrag(number, mouse) {
        if (!draggingText) { clearTextSelection(); selectVerse(number, !!(mouse.modifiers & Qt.ShiftModifier)) }
        draggingText = false
        dragAnchorRow = -1
    }
    function copyFreeText() {
        if (freeSelectedText.length) bible.copyText(freeSelectedText)
    }
    function copyClickedVerse(number, plainText) {
        if (!flushNote()) return
        const inside = hasSelection && number >= bible.selectedStart && number <= bible.selectedEnd
        const reference = inside ? bible.selectionReference : bible.passage + ":" + number
        // The selection API preserves an inside-hit range. copyVerse focuses an
        // outside hit only after the backend has successfully read/copied it.
        const copied = inside ? bible.copySelection(plainText) : bible.copyVerse(number, plainText)
        if (copied.length) {
            if (!inside) selectionAnchor = bible.cursorVerse
            syncNote()
            notify(reference + (plainText ? " copied as plain text" : " copied"))
        }
    }
    Timer {
        interval: 16
        repeat: true
        running: app.draggingText
        onTriggered: {
            let edge = 48 * app.scale
            let bottom = reading.height - app.floatingSafety
            let speed = app.dragPoint.y < edge ? -Math.min(edge, edge - app.dragPoint.y)
                : app.dragPoint.y > bottom - edge ? Math.min(edge, app.dragPoint.y - bottom + edge) : 0
            if (speed) { app.scrollBy(speed * 0.35); app.applyTextDrag() }
        }
    }

    function clearCount() { verseCount = ""; if (countTimer) countTimer.stop() }
    function countDigit(digit) {
        if (verseCount.length < 6) verseCount += digit
        countTimer.restart()
    }
    function countedVerse(last) {
        if (!verseCount.length) { chapterEdge(last); return }
        let number = Number(verseCount)
        clearCount()
        // Only existing rows are valid: omitted verses must not silently resolve nearby.
        if (!Array.from(bible.verses).some(function(verse) { return verse.number === number })) {
            notify("Verse " + number + " is not available in " + bible.passage)
            return
        }
        selectVerse(number, false)
        Qt.callLater(function() { app.scrollToVerse(bible.cursorVerse, false) })
    }

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
    function restorePanelFocus() {
        // Recheck after popup teardown: a dispatched action may have opened a
        // new modal already. Never steal its focus from a delayed close signal.
        Qt.callLater(function() {
            if (app.modalOpen || bible.busy) return
            if (notesDrawer.opened) {
                if (app.hasSelection && !app.notePreview) noteEditor.forceActiveFocus()
                else notesPanel.forceActiveFocus()
            } else app.readerFocus()
        })
    }
    function showFocusedError() {
        if (bible.focusMode && bible.error.length && !bible.busy) {
            errorDialog.message = bible.error
            errorDialog.open()
        }
    }
    property bool toastInFocus: false
    function notify(message, inFocus) { toastInFocus = !!inFocus; toast.text = message; toastTimer.restart() }
    function selectVerse(number, extend) {
        clearCount()
        if (!flushNote()) return
        clearTextSelection()
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
    function moveVerse(delta, extend, bulk) {
        clearCount()
        if (!flushNote()) return
        clearTextSelection()
        positionTimer.stop()
        restoringPosition = true
        movingSelection = true
        if (bulk) bible.moveByVerses(delta, extend)
        else bible.moveSelection(delta, extend)
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
        clearCount()
        if (!flushNote() || !bible.ready || bible.verses.length === 0) return false
        if (hasSelection) bible.selectVerses(bible.cursorVerse, bible.cursorVerse)
        else if (enabled) bible.selectVerses(bible.verses[0].number, bible.verses[0].number)
        if (bible.error.length) return false
        selectionAnchor = bible.cursorVerse
        let changed = verseMode !== enabled
        verseMode = enabled
        syncNote()
        if (changed) notify("Visual selection " + (verseMode ? "on" : "off"), true)
        return true
    }
    function extendVerse(delta) {
        if (!verseMode && !setVisualMode(true)) return
        moveVerse(delta, true)
    }
    function chapterEdge(last) {
        clearCount()
        if (!flushNote() || bible.verses.length === 0) return
        let number = bible.verses[last ? bible.verses.length - 1 : 0].number
        selectVerse(number, false)
        Qt.callLater(function() { app.scrollToVerse(bible.cursorVerse, false) })
    }
    function chapterStep(delta) {
        clearCount()
        if (!flushNote()) return
        savePosition()
        // Map once over the books, rather than opening every intervening chapter.
        let books = Array.from(bible.books)
        let current = bible.chapter - 1
        let total = 0
        for (let book of books) {
            if (book.id < bible.book) current += book.chapters
            total += book.chapters
        }
        let target = Math.max(0, Math.min(total - 1, current + delta))
        if (target !== current) for (let book of books) {
            if (target < book.chapters) { bible.openChapter(book.id, target + 1); break }
            target -= book.chapters
        }
        readerFocus()
    }
    function jump(reference) {
        if (!flushNote()) return
        savePosition()
        if (bible.navigate(reference)) {
            passagePalette.navigated = true
            passagePalette.close()
            readerFocus()
        }
    }
    function openChapter(book, chapter) {
        if (!flushNote()) return
        savePosition()
        bible.openChapter(book, chapter)
        positionTimer.stop()
        reading.contentY = 0
        restoringPosition = false
        savePosition()
        passagePalette.navigated = true
        passagePalette.close()
        readerFocus()
    }
    function openPalette(commands, resume) {
        clearCount()
        if (!flushNote()) return
        if (!resume) {
            resetBooks()
            referenceInput.text = commands || !bible.ready ? "> " : ""
            paletteCleared = false
            rememberPalette()
        } else if (paletteSnapshot) {
            paletteBookName = paletteSnapshot.name
            paletteBook = paletteSnapshot.book
            paletteChapter = paletteSnapshot.chapter
            referenceInput.text = paletteSnapshot.query
            paletteArrowUsed = paletteSnapshot.arrow
            paletteResults.currentIndex = paletteSnapshot.row
            paletteResults.positionViewAtIndex(paletteSnapshot.row, ListView.Contain)
        }
        paletteCleared = false
        passagePalette.open()
        referenceInput.forceActiveFocus()
    }
    function rememberPalette() {
        paletteSnapshot = { query: referenceInput.text, book: paletteBook, chapter: paletteChapter,
            name: paletteBookName, row: paletteResults.currentIndex, arrow: paletteArrowUsed }
    }
    function escapePalette() {
        let commands = referenceInput.text.trim().startsWith(">")
        let query = commands ? referenceInput.text.trim().substring(1).trim() : referenceInput.text.trim()
        if (query.length) {
            rememberPalette()
            referenceInput.text = commands ? "> " : ""
            paletteCleared = true
        } else passagePalette.close()
    }
    function openNotes(focusEditor) {
        if (!bible.ready || bible.busy) return
        clearCount()
        notesDrawer.focusEditor = focusEditor
        notesDrawer.open()
        if (loadedNoteReference !== bible.selectionReference) syncNote()
        if (focusEditor && hasSelection) {
            notePreview = false
            if (notesDrawer.opened) noteEditor.forceActiveFocus()
        }
    }
    function closeNotes() {
        if (!flushNote()) return false
        notesDrawer.close()
        return true
    }
    function toggleNotes() {
        if (notesOpen) { if (closeNotes()) notify("Notes off", true) }
        else { openNotes(true); if (notesOpen) notify("Notes on", true) }
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
    function copyPassage(plainText) {
        clearCount()
        const reference = hasSelection ? bible.selectionReference : bible.passage
        if (bible.copySelection(!!plainText).length > 0)
            notify(reference + (plainText ? " copied as plain text" : " copied"))
    }
    function savePosition() {
        if (!restoringPosition && bible.ready) {
            let travel = Math.max(0, reading.contentHeight - reading.height)
            bible.readingPosition = travel > 0 ? Math.max(0, Math.min(1, reading.contentY / travel)) : 0
        }
    }
    function scrollBy(amount) {
        clearCount()
        reading.contentY = Math.max(0, Math.min(Math.max(0, reading.contentHeight - reading.height), reading.contentY + amount))
    }
    function scrollToVerse(number, alignTop) {
        let index = Array.from(bible.verses).findIndex(function(verse) { return verse.number === number })
        let row = verseRepeater.itemAt(index) as VerseItem
        let block = blockLoader.item as BlockReader
        if (!row && !block) return
        if (index === 0) { reading.contentY = 0; return }
        let rect = block ? block.verseRect(number) : null
        let y = rect ? blockLoader.mapToItem(readerColumn, 0, rect.y).y : row.mapToItem(readerColumn, 0, 0).y + row.textOffset
        let textHeight = rect ? rect.height : row.textHeight
        let available = Math.max(1, reading.height - floatingSafety)
        let margin = Math.min(3 * readerLineHeight, Math.max(12 * scale, (available - textHeight) / 2))
        // A very long verse cannot fit. Keep its beginning and a useful context
        // margin visible rather than jumping to its end on every movement.
        if (textHeight > available - 24 * scale)
            margin = Math.min(3 * readerLineHeight, available * 0.2)
        let extent = Math.min(textHeight, Math.max(1, available - 2 * margin))
        let target = reading.contentY
        if (alignTop || y < target + margin) target = y - margin
        else if (y + extent > target + available - margin) target = y + extent - available + margin
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
    function moveSaved(delta) {
        if (collectionList.count > 0) {
            collectionList.currentIndex = Math.max(0, Math.min(collectionList.count - 1, collectionList.currentIndex + delta))
            collectionList.positionViewAtIndex(collectionList.currentIndex, ListView.Contain)
        }
        if (collectionList.currentItem) collectionList.currentItem.focus = false
        collectionList.forceActiveFocus()
    }
    function filterSaved(delta) {
        collectionTab = (collectionTab + delta + 4) % 4
        collectionList.currentIndex = collectionList.count > 0 ? 0 : -1
        moveSaved(0)
    }
    function openSaved(entry) {
        if (!entry || !flushNote()) return
        savePosition()
        if (!bible.navigate(entry.reference)) return
        syncNote()
        savedDrawer.close()
        if (entry.note.length > 0) openNotes(false)
        Qt.callLater(function() { app.scrollToVerse(bible.cursorVerse, true) })
    }
    function requestDeleteSaved() {
        let row = collectionList.currentIndex
        let focused = activeFocusItem
        while (focused && focused !== collectionList) {
            let delegate = focused as SavedEntry
            if (delegate) { row = delegate.index; break }
            focused = focused.parent
        }
        let entry = focused instanceof SavedEntry ? (focused as SavedEntry).modelData : collectionList.model[row]
        if (!entry || !flushNote() || bible.busy) return
        deleteConfirmation.reference = entry.reference
        deleteConfirmation.row = row
        deleteConfirmation.open()
    }
    function confirmDeleteSaved() {
        if (!flushNote() || bible.busy) return
        if (!bible.deleteAnnotation(deleteConfirmation.reference)) return
        syncNote()
        collectionList.currentIndex = Math.min(deleteConfirmation.row, collectionList.count - 1)
        deleteConfirmation.close()
    }
    function openBookmark(reference) {
        if (!flushNote()) return
        savePosition()
        if (bible.navigate(reference)) { syncNote(); readerFocus() }
    }
    // One action catalog supplies shortcuts, searchable keybindings, and commands.
    // Context-only gestures are explanatory rows, never executable guesses.
    readonly property var actions: [
        { id: "books", title: "Open a passage", keys: "Ctrl k", sequences: ["Ctrl+K"], scope: "palette", command: true },
        { id: "commands", title: "Open commands", keys: "Ctrl Space", sequences: ["Ctrl+Space"], scope: "palette" },
        { id: "resume", title: "Resume last palette", keys: "Space", sequences: ["Space"] },
        { id: "saved", title: "Saved passages", keys: "Ctrl l", sequences: ["Ctrl+L"], command: true },
        { id: "previousChapter", title: "Previous chapter", keys: "h · Left · [", sequences: ["H", "Left", "["] },
        { id: "nextChapter", title: "Next chapter", keys: "l · Right · ]", sequences: ["L", "Right", "]"] },
        { id: "previousVerse", title: "Previous verse", keys: "k · Up", sequences: ["K", "Up"] },
        { id: "nextVerse", title: "Next verse", keys: "j · Down", sequences: ["J", "Down"] },
        { id: "visual", title: "Toggle visual selection", keys: "v", sequences: ["V"] },
        { id: "extendPrevious", title: "Extend to previous verse", keys: "Shift K · Shift Up", sequences: ["Shift+K", "Shift+Up"] },
        { id: "extendNext", title: "Extend to next verse", keys: "Shift J · Shift Down", sequences: ["Shift+J", "Shift+Down"] },
        { id: "first", title: "Select chapter beginning", keys: "g · Home", sequences: ["G", "Home"] },
        { id: "last", title: "Select chapter end", keys: "Shift G · End", sequences: ["Shift+G", "End"] },
        { id: "previousFive", title: "Previous 5 verses", keys: "Ctrl u", sequences: ["Ctrl+U"] },
        { id: "nextFive", title: "Next 5 verses", keys: "Ctrl d", sequences: ["Ctrl+D"] },
        { id: "previousTen", title: "Previous 10 verses", keys: "Ctrl b · PageUp", sequences: ["Ctrl+B", "PgUp"] },
        { id: "nextTen", title: "Next 10 verses", keys: "Ctrl f · PageDown", sequences: ["Ctrl+F", "PgDown"] },
        { id: "bookmark", title: "Bookmark selection", keys: "m · Ctrl m", sequences: ["M", "Ctrl+M"], command: true },
        { id: "highlight", title: "Highlight selection", keys: "x · Ctrl h", sequences: ["X", "Ctrl+H"], command: true },
        { id: "notes", title: "Write a note", keys: "n", sequences: ["N"], scope: "notes", command: true },
        { id: "toggleNotes", title: "Toggle notes", keys: "Ctrl n", sequences: ["Ctrl+N"], scope: "global", command: true },
        { id: "copy", title: "Copy passage as quotation", keys: "y · Ctrl c", sequences: ["Y"], command: true },
        { id: "copyPlain", title: "Copy passage as plain text", keys: "Shift Y · Ctrl Shift C", sequences: ["Shift+Y", "Ctrl+Shift+C"], command: true },
        { id: "copyText", title: "Copy selected text", keys: "Ctrl c", sequences: ["Ctrl+C"] },
        { id: "larger", title: "Larger text", keys: "Ctrl + · Ctrl =", sequences: ["Ctrl++", "Ctrl+="] },
        { id: "smaller", title: "Smaller text", keys: "Ctrl −", sequences: ["Ctrl+-"] },
        { id: "resetText", title: "Reset text size", keys: "Ctrl 0", sequences: ["Ctrl+0"] },
        { id: "numbers", title: "Show verse numbers", keys: "r", sequences: ["R"], setting: "showVerseNumbers", command: true },
        { id: "headings", title: "Show headings", keys: "t", sequences: ["T"], setting: "showHeadings", command: true },
        { id: "versePicker", title: "Verse picker", keys: "p", sequences: ["P"], setting: "versePicker", command: true },
        { id: "block", title: "Block mode", keys: "b", sequences: ["B"], setting: "blockMode", command: true },
        { id: "focus", title: "Focus mode", keys: "f", sequences: ["F"], setting: "focusMode", command: true },
        { id: "lineHeight", title: "Line height", keys: "", command: true },
        { id: "lineLess", title: "Decrease line height", keys: "Ctrl [", sequences: ["Ctrl+["] },
        { id: "lineMore", title: "Increase line height", keys: "Ctrl ]", sequences: ["Ctrl+]"] },
        { id: "lineReset", title: "Reset line height", keys: "Ctrl \\", sequences: ["Ctrl+\\"], command: true },
        { id: "settings", title: "Reading settings", keys: "", command: true },
        { id: "export", title: "Export notes as Markdown", keys: "", command: true },
        { id: "backup", title: "Save annotation backup", keys: "", command: true },
        { id: "restore", title: "Restore annotation backup", keys: "", command: true },
        { id: "import", title: "Import NET 2.1 EPUB", keys: "", scope: "global", command: true },
        { id: "help", title: "Keybindings", keys: "? · F1", sequences: ["?", "F1"], scope: "global", command: true },
        { id: "about", title: "About & attribution", keys: "", scope: "global", command: true },
        { title: "Select a verse", keys: "Click number", explanation: true },
        { title: "Select a range", keys: "Shift Click", explanation: true },
        { title: "Select text across verses", keys: "Drag text", explanation: true },
        { title: "Copy clicked quotation", keys: "Right Click", explanation: true },
        { title: "Copy clicked plain text", keys: "Middle Click", explanation: true },
        { title: "Go to verse number", keys: "number then g (e.g. 20g)", explanation: true },
        { title: "Repeat a motion", keys: "Count without spaces: 4k, 3Down, 2Ctrl d", explanation: true },
        { title: "Dismiss current panel or selection", keys: "Esc", explanation: true }
    ]
    function actionHint(action) {
        if (action.setting) return (bible[action.setting] ? "On" : "Off") + (action.keys ? " · " + action.keys : "")
        if (action.id === "lineHeight") return bible.lineHeight.toFixed(1)
        return action.keys
    }
    function shortcutEnabled(action) {
        if (!actionAvailable(action)) return false
        if (settingsDialog.opened && !deleteConfirmation.visible && !errorDialog.visible
                && (action.setting || ["lineLess", "lineMore", "lineReset"].includes(action.id))) return true
        if (action.scope === "palette") return !modalOpen || passagePalette.opened
        if (action.scope === "global") return !modalOpen && (bible.ready || action.id === "help" || action.id === "about" || action.id === "import")
        if (action.scope === "notes") return bible.ready && !modalOpen && !textInputActive()
        return readerKeys
    }
    function actionAvailable(action) {
        if (typeof action === "string") action = actions.find(function(entry) { return entry.id === action })
        if (!action || action.explanation || bible.busy) return false
        if (action.id === "copyText") return bible.ready
        if (action.id === "bookmark" || action.id === "highlight") return hasSelection
        return bible.ready || !!action.setting
            || ["books", "commands", "help", "about", "import", "settings", "lineHeight", "lineLess", "lineMore", "lineReset"].includes(action.id)
    }
    function filteredKeybindings() {
        let query = keybindingSearch.text.trim().toLowerCase()
        return actions.filter(function(binding) {
            if (!binding.sequences && !binding.explanation) return false
            // Single-letter searches match a key, rather than every action containing it.
            return !query || (query.length === 1
                ? binding.keys.split(/\s*[·/]\s*/).some(function(key) { return key.toLowerCase() === query })
                : (binding.title + " " + binding.keys).toLowerCase().indexOf(query) >= 0)
        })
    }
    function paletteEntries() {
        let query = referenceInput.text.trim().toLowerCase()
        if (query.startsWith(">")) {
            query = query.substring(1).trim()
            return actions.filter(function(c) { return c.command && c.title.toLowerCase().indexOf(query) >= 0 })
                .map(function(c) { return { title: c.title, hint: actionHint(c), action: c.id } })
        }
        if (paletteChapter > 0) {
            return Array.from(bible.chapterVerses(paletteBook, paletteChapter))
                .filter(function(v) { return !query || String(v.number).startsWith(query) })
                .map(function(v) { return { title: paletteBookName + " " + paletteChapter + ":" + v.number,
                    hint: "Verse " + v.number, book: paletteBook, chapter: paletteChapter, verse: v.number } })
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
            if (!actionAvailable(entry.action) || !flushNote()) return
            passagePalette.pendingAction = entry.action
            passagePalette.close()
        } else if (entry.verse) jump(entry.title)
        else if (entry.chapter) {
            if (!bible.versePicker) { openChapter(entry.book, entry.chapter); return }
            paletteChapter = entry.chapter
            referenceInput.text = ""
            paletteArrowUsed = false
            paletteResults.currentIndex = 0
            referenceInput.forceActiveFocus()
        }
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
        if (paletteBack.activeFocus) { backToBooks(); return }
        let destination = paletteDestination
        if (destination.entry) activatePaletteEntry(destination.entry)
        else if (paletteResolution.valid) jump(destination.reference)
    }
    function backToBooks() {
        if (paletteChapter > 0) {
            let chapter = paletteChapter
            paletteChapter = 0
            referenceInput.text = ""
            paletteArrowUsed = true
            paletteResults.currentIndex = chapter - 1
            paletteResults.positionViewAtIndex(chapter - 1, ListView.Contain)
            referenceInput.forceActiveFocus()
            return
        }
        resetBooks()
        referenceInput.forceActiveFocus()
    }
    function resetBooks() {
        paletteChapter = 0
        paletteBook = 0
        paletteBookName = ""
        referenceInput.text = ""
        paletteArrowUsed = false
        paletteResults.currentIndex = 0
        referenceInput.forceActiveFocus()
    }
    function movePalette(delta) {
        paletteArrowUsed = true
        if (delta < 0 && paletteResults.currentIndex <= 0 && paletteBack.visible) {
            paletteResults.currentIndex = -1
            paletteBack.forceActiveFocus()
            return
        }
        paletteResults.currentIndex = Math.max(0, Math.min(paletteResults.count - 1, paletteResults.currentIndex + delta))
        paletteResults.positionViewAtIndex(paletteResults.currentIndex, ListView.Contain)
        referenceInput.forceActiveFocus()
    }
    function runCommand(action) {
        if (!actionAvailable(action)) return
        if (action === "first" || action === "last") { countedVerse(action === "last"); return }
        const count = Math.max(1, Math.min(999999, Number(verseCount) || 1))
        clearCount()
        if (!flushNote()) return
        const catalogAction = actions.find(function(entry) { return entry.id === action })
        const previousSetting = catalogAction.setting ? bible[catalogAction.setting] : null
        const previousLineHeight = bible.lineHeight
        switch (action) {
        case "books": openPalette(false); break
        case "commands": openPalette(true); break
        case "resume": openPalette(false, true); break
        case "previousChapter": chapterStep(-count); break
        case "nextChapter": chapterStep(count); break
        case "previousVerse": moveVerse(-count, verseMode); break
        case "nextVerse": moveVerse(count, verseMode); break
        case "previousFive": moveVerse(-5 * count, verseMode, true); break
        case "nextFive": moveVerse(5 * count, verseMode, true); break
        case "previousTen": moveVerse(-10 * count, verseMode, true); break
        case "nextTen": moveVerse(10 * count, verseMode, true); break
        case "visual": setVisualMode(!verseMode); break
        case "extendPrevious": extendVerse(-count); break
        case "extendNext": extendVerse(count); break
        case "toggleNotes": toggleNotes(); break
        case "copyText": if (freeSelectedText.length) copyFreeText(); else copyPassage(false); break
        case "larger": readerSize = Math.min(28, readerSize + 1); break
        case "smaller": readerSize = Math.max(15, readerSize - 1); break
        case "resetText": readerSize = 19; break
        case "numbers": bible.setShowVerseNumbers(!bible.showVerseNumbers); break
        case "headings": bible.setShowHeadings(!bible.showHeadings); break
        case "versePicker": bible.setVersePicker(!bible.versePicker); break
        case "block": bible.setBlockMode(!bible.blockMode); break
        case "focus": bible.setFocusMode(!bible.focusMode); break
        case "lineLess": bible.setLineHeight(Math.round((bible.lineHeight - 0.1) * 10) / 10); break
        case "lineMore": bible.setLineHeight(Math.round((bible.lineHeight + 0.1) * 10) / 10); break
        case "lineReset": bible.setLineHeight(1.5); break
        case "lineHeight": settingsDialog.lineOnly = true; settingsDialog.open(); break
        case "settings": settingsDialog.lineOnly = false; settingsDialog.open(); break
        case "saved": bible.refreshCollection(); savedDrawer.open(); break
        case "notes": openNotes(true); break
        case "bookmark": if (hasSelection) bible.toggleBookmark(); else notify("Select a verse number to bookmark a passage"); break
        case "highlight": if (hasSelection) bible.toggleHighlight(); else notify("Select a verse number to highlight a passage"); break
        case "copy": if (bible.ready) copyPassage(); break
        case "copyPlain": if (bible.ready) copyPassage(true); break
        case "export": exportDialog.open(); break
        case "backup": backupDialog.open(); break
        case "restore": restoreDialog.open(); break
        case "import": importDialog.open(); break
        case "help": helpDialog.open(); break
        case "about": aboutDialog.open(); break
        }
        if (catalogAction.setting && bible[catalogAction.setting] !== previousSetting)
            notify(catalogAction.title + " " + (bible[catalogAction.setting] ? "on" : "off"), true)
        if (["lineLess", "lineMore", "lineReset"].includes(action) && bible.lineHeight !== previousLineHeight)
            notify("Line height " + bible.lineHeight.toFixed(1), true)
    }
    function moveKeybinding(delta) {
        keybindingResults.currentIndex = Math.max(0, Math.min(keybindingResults.count - 1, keybindingResults.currentIndex + delta))
        keybindingResults.positionViewAtIndex(keybindingResults.currentIndex, ListView.Contain)
    }
    function keybindingEdge(last) {
        let row = last ? keybindingResults.count - 1 : 0
        keybindingResults.currentIndex = keybindingResults.count ? row : -1
        if (keybindingResults.count) keybindingResults.positionViewAtIndex(row, ListView.Contain)
    }
    function pageKeybindings(direction) {
        if (!keybindingResults.count) return
        keybindingResults.forceLayout()
        const view = keybindingResults
        const current = view.currentItem
        const offset = current ? Math.max(0, Math.min(view.height, current.y + current.height / 2 - view.contentY)) : 0
        const top = Math.max(view.originY, Math.min(view.originY + Math.max(0, view.contentHeight - view.height),
            view.contentY + direction * view.height))
        const target = top + offset
        let row = view.currentIndex
        let distance = Infinity
        for (let i = 0; i < view.count; ++i) {
            const item = view.itemAtIndex(i)
            if (!item || item.y < top || item.y + item.height > top + view.height) continue
            const gap = Math.abs(item.y + item.height / 2 - target)
            if (gap < distance) { distance = gap; row = i }
        }
        view.currentIndex = row
        view.contentY = top
    }
    function activateKeybinding(action) {
        if (!actionAvailable(action) || !flushNote()) return
        helpDialog.pendingAction = action.id
        helpDialog.close()
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
    component OverlayScrollbar: ScrollBar {
        id: bar
        implicitWidth: 18 * app.scale
        width: implicitWidth
        anchors.right: parent.right
        z: 10
        policy: ScrollBar.AlwaysOn
        visible: size < 1
        hoverEnabled: true
        padding: 0
        minimumSize: 0.035
        onPositionChanged: linger.restart()
        onHoveredChanged: linger.restart()
        Timer { id: linger; interval: 1100 }
        contentItem: Item {
            Rectangle {
                objectName: bar.objectName + "Thumb"
                anchors.right: parent.right
                anchors.rightMargin: 4 * app.scale
                height: parent.height
                width: (bar.hovered || bar.pressed ? 10 : 4) * app.scale
                color: desktop.muted
                opacity: bar.hovered || bar.pressed ? 0.8 : linger.running ? 0.5 : 0.2
                Behavior on width { NumberAnimation { duration: 120 } }
                Behavior on opacity { NumberAnimation { duration: 200 } }
            }
        }
        background: Item {}
    }
    component OverlayScrollView: ScrollView {
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ScrollBar.vertical: OverlayScrollbar { objectName: parent.objectName + "Scrollbar" }
        // Scrollbars paint over the viewport, including when a platform style
        // would otherwise reserve their implicit width as a gutter.
        leftPadding: 0
        rightPadding: 0
    }
    component SettingSwitch: Switch {
        id: settingSwitch
        required property var catalogAction
        objectName: "setting" + catalogAction.id
        text: catalogAction.title
        checked: !!bible[catalogAction.setting]
        enabled: app.actionAvailable(catalogAction)
        implicitHeight: Math.max(40 * app.scale, settingLabel.implicitHeight + 16 * app.scale)
        padding: 0
        spacing: 12 * app.scale
        onClicked: app.runCommand(catalogAction.id)
        indicator: Rectangle {
            objectName: settingSwitch.objectName + "Track"
            x: settingSwitch.width - width
            y: (settingSwitch.height - height) / 2
            width: 36 * app.scale
            height: 20 * app.scale
            radius: 0
            color: settingSwitch.checked ? desktop.accent : desktop.surface
            border.color: settingSwitch.activeFocus ? desktop.foreground : settingSwitch.checked ? desktop.accent : desktop.border
            Rectangle {
                objectName: settingSwitch.objectName + "Thumb"
                x: (settingSwitch.checked ? 19 : 3) * app.scale
                y: 3 * app.scale
                width: 14 * app.scale
                height: width
                radius: 0
                color: settingSwitch.checked ? desktop.background : desktop.muted
            }
        }
        contentItem: RowLayout {
            spacing: 12 * app.scale
            Label {
                id: settingLabel
                objectName: settingSwitch.objectName + "Label"
                text: settingSwitch.text
                color: desktop.foreground
                font.pixelSize: 13 * app.scale
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            Label {
                objectName: settingSwitch.objectName + "Key"
                text: settingSwitch.catalogAction.keys
                color: desktop.muted
                font.pixelSize: 12 * app.scale
            }
            Item { Layout.preferredWidth: 36 * app.scale; Layout.preferredHeight: 20 * app.scale }
        }
        background: Rectangle { color: settingSwitch.hovered ? desktop.surface : "transparent" }
    }
    component SquareSlider: Slider {
        id: slider
        implicitHeight: 32 * app.scale
        implicitWidth: 200 * app.scale
        padding: 9 * app.scale
        background: Rectangle {
            x: slider.leftPadding
            y: slider.topPadding + (slider.availableHeight - height) / 2
            width: slider.availableWidth
            height: 3 * app.scale
            radius: 0
            color: desktop.border
            Rectangle {
                objectName: slider.objectName + "Track"
                x: slider.mirrored ? parent.width - width : 0
                width: slider.position * parent.width
                height: parent.height
                radius: 0
                color: desktop.accent
            }
        }
        handle: Rectangle {
            objectName: slider.objectName + "Handle"
            x: slider.leftPadding + slider.visualPosition * (slider.availableWidth - width)
            y: slider.topPadding + (slider.availableHeight - height) / 2
            implicitWidth: 18 * app.scale
            implicitHeight: 18 * app.scale
            radius: 0
            color: slider.pressed ? desktop.accent : desktop.surface
            border.color: desktop.accent
            border.width: slider.activeFocus ? 2 : 1
        }
    }
    component FloatingButton: Button {
        id: floatingButton
        property string glyph: ""
        property string tip: ""
        property bool selected: false
        implicitWidth: glyph.length ? implicitHeight : Math.ceil(floatingText.implicitWidth + leftPadding + rightPadding) + 1
        implicitHeight: 40 * app.controlScale
        padding: 8 * app.controlScale
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        font.pixelSize: 13 * app.scale
        onPressed: app.clearCount()
        Keys.onReturnPressed: clicked()
        Keys.onEnterPressed: clicked()
        Accessible.name: tip
        ToolTip.visible: hovered || activeFocus
        ToolTip.text: tip
        ToolTip.delay: 500
        background: Rectangle {
            color: floatingButton.down ? app.tint(desktop.accent, 0.18)
                : floatingButton.hovered ? app.tint(desktop.accent, 0.09) : "transparent"
            radius: 0
            border.width: floatingButton.activeFocus ? 1 : 0
            border.color: desktop.accent
        }
        contentItem: Item {
            Text {
                id: floatingText
                objectName: floatingButton.objectName + "Label"
                anchors.fill: parent
                text: floatingButton.text
                visible: !floatingButton.glyph.length
                color: desktop.foreground
                font: floatingButton.font
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }
            Canvas {
                id: floatingIcon
                anchors.centerIn: parent
                width: 22 * app.controlScale
                height: width
                visible: floatingButton.glyph.length > 0
                property color ink: !floatingButton.enabled ? desktop.muted : floatingButton.selected ? desktop.accent : desktop.foreground
                property string glyph: floatingButton.glyph
                onInkChanged: requestPaint()
                onGlyphChanged: requestPaint()
                onPaint: {
                    let ctx = getContext("2d")
                    ctx.reset()
                    ctx.scale(width / 24, height / 24)
                    ctx.strokeStyle = ink
                    ctx.fillStyle = ink
                    ctx.lineWidth = 1.6
                    ctx.lineCap = "round"
                    ctx.lineJoin = "round"
                    ctx.beginPath()
                    if (glyph === "saved") {
                        ctx.moveTo(6, 3); ctx.lineTo(18, 3); ctx.lineTo(18, 21)
                        ctx.lineTo(12, 17); ctx.lineTo(6, 21); ctx.closePath()
                    } else if (glyph === "notes") {
                        ctx.moveTo(15, 3); ctx.lineTo(5, 3); ctx.lineTo(5, 21)
                        ctx.lineTo(19, 21); ctx.lineTo(19, 7); ctx.lineTo(15, 3)
                        ctx.lineTo(15, 7); ctx.lineTo(19, 7)
                        ctx.moveTo(9, 11); ctx.lineTo(15, 11)
                        ctx.moveTo(9, 15); ctx.lineTo(15, 15)
                    } else if (glyph === "help") {
                        ctx.moveTo(8, 7); ctx.bezierCurveTo(8, 1, 19, 2, 16, 9)
                        ctx.bezierCurveTo(15, 11, 12, 11, 12, 15)
                        ctx.moveTo(12, 19); ctx.lineTo(12, 19.3)
                    } else if (glyph === "visual") {
                        ctx.rect(4, 4, 16, 16)
                        ctx.moveTo(8, 9); ctx.lineTo(16, 9)
                        ctx.moveTo(8, 13); ctx.lineTo(16, 13)
                        ctx.moveTo(8, 17); ctx.lineTo(13, 17)
                    } else {
                        ctx.moveTo(9, 3); ctx.lineTo(15, 3)
                        ctx.moveTo(12, 3); ctx.lineTo(12, 21)
                        ctx.moveTo(9, 21); ctx.lineTo(15, 21)
                    }
                    ctx.stroke()
                }
            }
        }
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
    component NoteGlyph: Canvas {
        implicitWidth: 13 * app.scale
        implicitHeight: 16 * app.scale
        property color ink: desktop.accent
        onInkChanged: requestPaint()
        onPaint: {
            let ctx = getContext("2d")
            ctx.reset()
            ctx.scale(width / 16, height / 20)
            ctx.strokeStyle = ink
            ctx.lineWidth = 1.4
            ctx.beginPath()
            ctx.moveTo(10, 1); ctx.lineTo(1, 1); ctx.lineTo(1, 19)
            ctx.lineTo(15, 19); ctx.lineTo(15, 6); ctx.lineTo(10, 1)
            ctx.lineTo(10, 6); ctx.lineTo(15, 6)
            ctx.moveTo(4, 10); ctx.lineTo(12, 10)
            ctx.moveTo(4, 14); ctx.lineTo(12, 14)
            ctx.stroke()
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

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: errorRow.implicitHeight + 20
            visible: bible.error.length > 0 && !bible.focusMode
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
                focus: true
                visible: bible.ready
                Keys.onPressed: function(event) {
                    // Unbound reader keys also end a pending count. Text editors
                    // and modal content have their own focus trees.
                    if (app.readerKeys && event.key !== Qt.Key_Shift && event.key !== Qt.Key_Control && event.key !== Qt.Key_Alt) app.clearCount()
                }

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
                    ScrollBar.vertical: OverlayScrollbar {
                        id: readerScrollbar
                        objectName: "readerScrollbar"
                        parent: readerSurface
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        anchors.topMargin: 12 * app.scale
                        anchors.bottomMargin: app.floatingSafety + 12 * app.scale
                        visible: !bible.focusMode && size < 1
                        enabled: visible
                    }

                    Column {
                        id: readerColumn
                        width: Math.min(760 * app.scale, reading.width - (reading.width < 700 ? 48 : 96))
                        anchors.horizontalCenter: parent.horizontalCenter
                        topPadding: 48 * app.scale
                        bottomPadding: app.floatingSafety + 3 * app.readerLineHeight
                        spacing: 0

                        SectionLabel { objectName: "editionLabel"; visible: !bible.focusMode; text: "NEW ENGLISH TRANSLATION"; leftPadding: app.verseGutter; bottomPadding: 12 }
                        Label {
                            objectName: "chapterTitle"
                            text: bible.passage
                            color: desktop.foreground
                            font.pixelSize: 34 * app.scale
                            font.weight: Font.Medium
                            font.letterSpacing: -0.9
                            leftPadding: app.verseGutter
                            bottomPadding: 24 * app.scale
                            width: parent.width
                            wrapMode: Text.Wrap
                        }
                        Repeater {
                            id: verseRepeater
                            model: bible.blockMode ? [] : bible.verses
                            delegate: VerseItem {
                                id: verseRow
                                required property var modelData
                                required property int index
                                readonly property bool selected: modelData.number >= bible.selectedStart && modelData.number <= bible.selectedEnd && bible.selectedStart > 0
                                readonly property real gutter: app.verseGutter
                                body: verseBody
                                textOffset: verseHeading.height
                                textHeight: verseBody.height
                                width: readerColumn.width
                                height: verseHeading.height + verseBody.height + 7 * app.scale
                                Item {
                                    id: bookmarkMarker
                                    objectName: "bookmarkMarker" + verseRow.modelData.number
                                    x: 0
                                    width: 14 * app.scale
                                    height: parent.height
                                    visible: verseRow.modelData.bookmarked && !bible.focusMode
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
                                        + "\nOpen " + openReference + " · m · Ctrl m toggles bookmark"
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
                                    readonly property bool hasHeading: bible.showHeadings && !bible.focusMode && (verseRow.modelData.heading || "").length > 0
                                    width: parent.width - verseRow.gutter - 6
                                    x: verseRow.gutter
                                    // Measure text at a fixed width, independent of effective
                                    // visibility. First import creates rows inside a hidden
                                    // reader; Label height/implicitHeight/padding feedback
                                    // otherwise re-enters when that ancestor becomes visible.
                                    height: hasHeading ? headingText.implicitHeight + 36 * app.scale : 0
                                    visible: hasHeading
                                    Text {
                                        objectName: "headingPrefix" + verseRow.modelData.number
                                        x: -26 * app.scale
                                        y: headingText.y
                                        text: "#"
                                        font: headingText.font
                                        color: app.tint(desktop.accent, 0.6)
                                    }
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
                                Repeater {
                                    model: verseRow.modelData.highlightRanges || []
                                    delegate: Rectangle {
                                        required property var modelData
                                        objectName: "highlightSegment" + verseRow.modelData.number + ":" + modelData.reference
                                        x: verseRow.gutter - 9
                                        y: modelData.highlightContinuesBefore ? 0 : verseHeading.height
                                        width: verseRow.width - x
                                        height: (modelData.highlightContinuesAfter ? verseRow.height : verseHeading.height + verseBody.height + 3) - y
                                        radius: 0
                                        color: app.tint(desktop.accent, 0.16)
                                        Rectangle {
                                            objectName: "highlightAccent" + verseRow.modelData.number + ":" + parent.modelData.reference
                                            width: 2; height: parent.height
                                            color: desktop.accent; opacity: 0.7
                                            visible: !bible.focusMode
                                        }
                                    }
                                }
                                Rectangle {
                                    objectName: "selectionSegment" + verseRow.modelData.number
                                    x: verseRow.gutter - 9
                                    y: verseRow.index > 0 && (bible.verses[verseRow.index - 1]?.number || 0) >= bible.selectedStart ? 0 : verseHeading.height
                                    width: parent.width - x
                                    height: (verseRow.modelData.number < bible.selectedEnd ? verseRow.height : verseHeading.height + verseBody.height + 3) - y
                                    visible: verseRow.selected
                                    radius: 0
                                    color: app.tint(desktop.accent, 0.22)
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
                                        id: numberLabel
                                        objectName: "verseNumberLabel" + verseRow.modelData.number
                                        visible: bible.showVerseNumbers && !bible.focusMode
                                        anchors.top: parent.top
                                        anchors.topMargin: Math.max(0, readerMetrics.ascent + readerMetrics.tightBoundingRect("H").y
                                            - numberMetrics.ascent - numberMetrics.tightBoundingRect("0").y)
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        text: verseRow.modelData.number
                                        font.pixelSize: 10 * app.scale
                                        font.weight: verseRow.selected ? Font.Bold : Font.Normal
                                        color: verseRow.selected ? desktop.accent : desktop.muted
                                        FontMetrics { id: numberMetrics; font: numberLabel.font }
                                    }
                                    Canvas {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        y: 24 * app.scale
                                        width: 8 * app.scale; height: 10 * app.scale
                                        visible: verseRow.modelData.hasNote && !bible.focusMode
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
                                        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
                                        preventStealing: true
                                        onPressed: function(mouse) { if (mouse.button === Qt.LeftButton) app.beginTextDrag(verseRow.index, numberMouse, mouse, true) }
                                        onPositionChanged: function(mouse) { if (pressedButtons & Qt.LeftButton) app.updateTextDrag(numberMouse, mouse) }
                                        onReleased: function(mouse) {
                                            if (mouse.button === Qt.LeftButton) app.finishTextDrag(verseRow.modelData.number, mouse)
                                            else app.copyClickedVerse(verseRow.modelData.number, mouse.button === Qt.MiddleButton)
                                        }
                                        onCanceled: { app.draggingText = false; app.dragAnchorRow = -1 }
                                        onDoubleClicked: function(mouse) {
                                            if (mouse.button !== Qt.LeftButton) return
                                            app.selectVerse(verseRow.modelData.number, false)
                                            app.openNotes(true)
                                        }
                                    }
                                    ToolTip.visible: numberMouse.containsMouse && !bible.focusMode
                                    ToolTip.delay: 700
                                    ToolTip.text: "Select verse " + verseRow.modelData.number + " · Shift Click for a range" + (verseRow.modelData.hasNote ? "\nHas a note" : "") + (verseRow.modelData.bookmarked ? "\nBookmarked" : "")
                                }
                                TextEdit {
                                    id: verseBody
                                    objectName: "verseBody" + verseRow.modelData.number
                                    x: verseRow.gutter + (verseRow.modelData.poetry ? 13 : 0)
                                    y: verseHeading.height
                                    width: parent.width - x - 9
                                    height: contentHeight
                                    text: '<div style="line-height:' + (bible.lineHeight * 100) + '%">'
                                        + (verseRow.modelData.html || verseRow.modelData.text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/\n/g, "<br>")) + '</div>'
                                    textFormat: Text.RichText
                                    font.family: desktop.fontFamily
                                    font.pixelSize: app.readerSize * app.scale
                                    color: desktop.foreground
                                    selectionColor: app.tint(desktop.accent, 0.35)
                                    selectedTextColor: desktop.foreground
                                    readOnly: true
                                    selectByMouse: true
                                    wrapMode: TextEdit.Wrap
                                    persistentSelection: true
                                    activeFocusOnTab: false
                                    padding: 0
                                    onLinkActivated: function(link) { desktop.openExternal(link) }
                                    Accessible.name: "Verse " + verseRow.modelData.number + ": " + verseRow.modelData.text
                                    Keys.onShortcutOverride: function(event) {
                                        if (event.key === Qt.Key_C && event.modifiers === Qt.ControlModifier && app.freeSelectedText.length) event.accepted = true
                                    }
                                    Keys.onPressed: function(event) {
                                        if (event.key === Qt.Key_C && event.modifiers === Qt.ControlModifier && app.freeSelectedText.length) { app.copyFreeText(); event.accepted = true }
                                    }
                                    MouseArea {
                                        id: bodyMouse
                                        anchors.fill: parent
                                        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
                                        cursorShape: Qt.IBeamCursor
                                        preventStealing: true
                                        onPressed: function(mouse) { if (mouse.button === Qt.LeftButton) app.beginTextDrag(verseRow.index, bodyMouse, mouse, false) }
                                        onPositionChanged: function(mouse) { if (pressedButtons & Qt.LeftButton) app.updateTextDrag(bodyMouse, mouse) }
                                        onReleased: function(mouse) {
                                            if (mouse.button === Qt.LeftButton) app.finishTextDrag(verseRow.modelData.number, mouse)
                                            else app.copyClickedVerse(verseRow.modelData.number, mouse.button === Qt.MiddleButton)
                                        }
                                        onCanceled: { app.draggingText = false; app.dragAnchorRow = -1 }
                                    }
                                }
                            }
                        }
                        Loader {
                            id: blockLoader
                            objectName: "blockLoader"
                            active: bible.blockMode
                            visible: active
                            x: app.verseGutter
                            width: parent.width - x - 9
                            sourceComponent: BlockReader {
                                objectName: "blockReader"
                                controller: app
                                backend: bible
                                theme: desktop
                                viewport: reading
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
                    }
                }
                // A continuous fade keeps floating controls legible without a
                // footer surface, divider, or shortening the reading viewport.
                Rectangle {
                    objectName: "readingBottomFade"
                    visible: !bible.focusMode
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: app.floatingSafety + 24 * app.scale
                    gradient: Gradient {
                        GradientStop { position: 0; color: app.tint(desktop.background, 0) }
                        GradientStop { position: 0.6; color: desktop.background }
                        GradientStop { position: 1; color: desktop.background }
                    }
                }
            }

            OverlayScrollView {
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

        }
    }

    Drawer {
        id: notesDrawer
        objectName: "notesDrawer"
        property bool focusEditor: false
        width: app.notesWidth
        height: app.height
        edge: Qt.RightEdge
        modal: true
        focus: true
        interactive: false
        padding: 0
        // Auto-close cannot veto a failed save. All dismissals use closeNotes().
        closePolicy: Popup.NoAutoClose
        background: Rectangle { color: desktop.background; border.color: desktop.border }
        Overlay.modal: Rectangle {
            color: app.tint(desktop.background, 0.65)
        }
        onOpened: {
            if (app.modalOpen) return
            if (focusEditor && app.hasSelection) noteEditor.forceActiveFocus()
            else notesPanel.forceActiveFocus()
        }
        onClosed: app.restorePanelFocus()
        contentItem: Rectangle {
                id: notesPanel
                objectName: "notesPanel"
                color: desktop.background
                Keys.onEscapePressed: function(event) { app.closeNotes(); event.accepted = true }
                Rectangle { anchors.left: parent.left; width: 1; height: parent.height; color: desktop.border }
                OverlayScrollView {
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
                        QuietButton { text: bible.selectionBookmarked ? "Bookmarked" : "Bookmark"; emphasized: bible.selectionBookmarked; compact: true; onClicked: app.runCommand("bookmark"); tip: "m · Ctrl m" }
                        QuietButton { text: bible.selectionHighlighted ? "Highlighted" : "Highlight"; emphasized: bible.selectionHighlighted; compact: true; onClicked: app.runCommand("highlight"); tip: "x · Ctrl h" }
                        Item { Layout.fillWidth: true }
                        QuietButton { objectName: "notesCopy"; text: "Copy"; compact: true; onClicked: app.copyPassage(); tip: "Copy passage as quotation · y · Ctrl c" }
                    }
                    FineRule { Layout.fillWidth: true }
                    RowLayout {
                        visible: app.hasSelection
                        spacing: 4
                        QuietButton { text: "Write"; compact: true; emphasized: !app.notePreview; onClicked: { app.notePreview = false; noteEditor.forceActiveFocus() } }
                        QuietButton { objectName: "previewButton"; text: "Preview"; compact: true; emphasized: app.notePreview; onClicked: { if (app.flushNote()) app.notePreview = true } }
                        Item { Layout.fillWidth: true }
                        Label { text: "Markdown"; color: desktop.muted; font.pixelSize: 11 * app.scale }
                    }
                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        OverlayScrollView {
                            objectName: "noteEditorScroll"
                            anchors.fill: parent
                            visible: app.hasSelection && !app.notePreview
                            clip: true
                            contentWidth: availableWidth
                            TextArea {
                                id: noteEditor
                                objectName: "noteEditor"
                                width: parent.width
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
                                Keys.onEscapePressed: function(event) { app.closeNotes(); event.accepted = true }
                                Accessible.name: "Markdown note for " + bible.selectionReference
                            }
                        }
                        OverlayScrollView {
                            objectName: "notePreviewScroll"
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
                            text: "Select a verse number to start a note.\n\nShift Click another number to gather a range. Your notes are saved automatically as you write."
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

    MouseArea {
        parent: Overlay.overlay
        // Cover only the dimmed outside region. Drawer consumes its own outside
        // events before the dimmer, so guard dismissal here without auto-close.
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: app.width - notesDrawer.width * notesDrawer.position
        z: notesDrawer.z + 0.1
        visible: app.notesOpen && !app.modalOpen
        onClicked: app.closeNotes()
    }

    FloatingButton {
        id: modeControl
        objectName: "modeControl"
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: 16 * app.controlScale
        glyph: app.verseMode ? "visual" : "normal"
        selected: app.verseMode
        visible: bible.ready && !bible.focusMode
        enabled: !bible.busy && !app.modalOpen
        tip: (app.verseMode ? "Visual mode · switch to normal" : "Normal mode · switch to visual") + " · v"
        onClicked: { app.setVisualMode(!app.verseMode); app.readerFocus() }
    }
    FloatingButton {
        id: passageControl
        objectName: "passageControl"
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 16 * app.controlScale
        x: app.width / 2 - width / 2
        width: Math.max(0, Math.min(implicitWidth, app.width - 2 * (floatingActions.width + 24 * app.controlScale)))
        text: bible.ready ? (bible.selectionReference || bible.passage) : "Open a passage"
        tip: (app.hasSelection ? bible.selectionReference : bible.passage) + " · Open passage · Ctrl k"
        visible: !bible.focusMode
        enabled: !bible.busy && !app.modalOpen
        onClicked: app.openPalette(false)
    }
    Row {
        id: floatingActions
        objectName: "floatingActions"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 16 * app.controlScale
        spacing: 4 * app.controlScale
        visible: !bible.focusMode
        FloatingButton {
            objectName: "savedControl"
            glyph: "saved"
            tip: "Saved · Ctrl l"
            enabled: bible.ready && !bible.busy && !app.modalOpen
            onClicked: app.runCommand("saved")
        }
        FloatingButton {
            objectName: "notesControl"
            glyph: "notes"
            selected: app.notesOpen
            tip: "Notes · Ctrl n"
            enabled: bible.ready && !bible.busy && !app.modalOpen
            onClicked: app.toggleNotes()
        }
        FloatingButton {
            objectName: "helpControl"
            glyph: "help"
            tip: "Keybindings · ? · F1"
            enabled: !bible.busy && !app.modalOpen
            onClicked: helpDialog.open()
        }
    }

    ModalCard {
        id: passagePalette
        objectName: "passagePalette"
        property string pendingAction: ""
        property bool navigated: false
        closePolicy: Popup.CloseOnPressOutside
        width: Math.min(650 * app.scale, app.width - 48)
        height: Math.min(560 * app.scale, app.height - 48)
        padding: 16
        onOpened: referenceInput.forceActiveFocus()
        onAboutToHide: { if (!app.paletteCleared) app.rememberPalette() }
        onClosed: {
            let action = pendingAction
            pendingAction = ""
            if (action) Qt.callLater(function() { app.runCommand(action) })
            else if (navigated) app.readerFocus()
            else app.restorePanelFocus()
            navigated = false
        }
        contentItem: ColumnLayout {
            spacing: 12
            Keys.onEscapePressed: app.escapePalette()
            Keys.onLeftPressed: function(event) {
                event.accepted = app.paletteBook > 0 && event.modifiers === Qt.NoModifier
                if (event.accepted) app.backToBooks()
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 4 * app.scale
                TextField {
                    id: referenceInput
                    objectName: "referenceInput"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    implicitHeight: 38 * app.scale
                    Layout.preferredHeight: implicitHeight
                    font.pixelSize: 15 * app.scale
                    color: desktop.foreground
                    placeholderText: app.paletteChapter ? app.paletteBookName + " " + app.paletteChapter + " — verse number"
                        : app.paletteBook ? app.paletteBookName + " — chapter number" : "John 3:16–18, a book, or > command"
                    placeholderTextColor: desktop.muted
                    selectByMouse: true
                    leftPadding: 14
                    rightPadding: 14
                    background: Rectangle { color: desktop.surface; radius: 0; border.color: referenceInput.activeFocus ? app.tint(desktop.accent, 0.65) : desktop.border }
                    onTextChanged: { app.paletteCleared = false; app.paletteArrowUsed = false; paletteResults.currentIndex = 0 }
                    onAccepted: app.acceptPalette()
                    Keys.onDownPressed: app.movePalette(1)
                    Keys.onUpPressed: app.movePalette(-1)
                    Keys.onLeftPressed: function(event) {
                        event.accepted = event.modifiers === Qt.NoModifier && app.paletteBook > 0
                            && selectedText.length === 0 && (!text.length || cursorPosition === 0 || app.paletteArrowUsed)
                        if (event.accepted) app.backToBooks()
                    }
                    Keys.onRightPressed: function(event) {
                        let entry = paletteResults.model[paletteResults.currentIndex]
                        let browsing = (!text.trim() || /^[1-3]?\s*[A-Za-z ]+$/.test(text.trim()) || (app.paletteBook > 0 && /^\d+$/.test(text.trim())) || text.trim().startsWith(">"))
                            && (app.paletteArrowUsed || (cursorPosition === text.length && selectedText.length === 0))
                        event.accepted = event.modifiers === Qt.NoModifier && browsing && !!entry
                        if (event.accepted) app.activatePaletteEntry(entry)
                    }
                    Keys.onEscapePressed: app.escapePalette()
                    Accessible.name: "Passage reference or command"
                }
                QuietButton { objectName: "paletteBooks"; text: "Search"; Layout.preferredHeight: referenceInput.implicitHeight; emphasized: !referenceInput.text.startsWith(">"); enabled: bible.ready; onClicked: app.openPalette(false) }
                QuietButton { objectName: "paletteCommands"; text: "Commands"; Layout.preferredHeight: referenceInput.implicitHeight; emphasized: referenceInput.text.startsWith(">"); onClicked: app.openPalette(true) }
            }
            Label {
                objectName: "referencePreview"
                visible: !referenceInput.text.trim().startsWith(">") && (referenceInput.text.trim().length > 0 || app.paletteBook > 0)
                text: paletteBack.activeFocus ? (app.paletteChapter ? "Browse chapters in " + app.paletteBookName : "Browse all books") : app.paletteResolution.valid ? app.paletteResolution.reference : app.paletteResolution.error
                color: app.paletteResolution.valid ? desktop.accent : desktop.muted
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                font.pixelSize: 12 * app.scale
            }
            QuietButton {
                id: paletteBack
                objectName: "paletteBack"
                text: app.paletteChapter ? "← " + app.paletteBookName + " chapters" : "← All books"
                compact: true
                visible: app.paletteBook > 0
                emphasized: activeFocus
                onClicked: app.backToBooks()
                Keys.onReturnPressed: app.backToBooks()
                Keys.onEnterPressed: app.backToBooks()
                Keys.onDownPressed: { paletteResults.currentIndex = -1; app.movePalette(1) }
                Keys.onUpPressed: {}
            }
            ListView {
                id: paletteResults
                objectName: "paletteResults"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 40
                Layout.preferredHeight: Math.min(340 * app.scale, app.height * 0.43)
                clip: true
                model: app.paletteEntries()
                currentIndex: 0
                spacing: 3
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: OverlayScrollbar { objectName: "paletteScrollbar" }
                Keys.onUpPressed: app.movePalette(-1)
                Keys.onDownPressed: app.movePalette(1)
                Keys.onReturnPressed: app.acceptPalette()
                Keys.onEnterPressed: app.acceptPalette()
                delegate: ItemDelegate {
                    id: paletteEntry
                    required property var modelData
                    required property int index
                    width: paletteResults.width
                    height: 44 * app.scale
                    highlighted: ListView.isCurrentItem
                    enabled: !modelData.action || app.actionAvailable(modelData.action)
                    onClicked: app.activatePaletteEntry(modelData)
                    contentItem: RowLayout {
                        Label { text: paletteEntry.modelData.title; color: paletteEntry.enabled ? desktop.foreground : desktop.muted; Layout.fillWidth: true; elide: Text.ElideRight; font.pixelSize: 14 * app.scale }
                        Label { objectName: "paletteHint" + (paletteEntry.modelData.action || ""); text: paletteEntry.modelData.hint; color: desktop.muted; font.pixelSize: 11 * app.scale }
                        Label {
                            objectName: "paletteChevron" + paletteEntry.index
                            text: "›"
                            visible: !!paletteEntry.modelData.book && !paletteEntry.modelData.verse
                                && (!paletteEntry.modelData.chapter || bible.versePicker)
                            color: desktop.muted
                            Layout.leftMargin: 8
                        }
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
                Label {
                    objectName: "paletteHints"
                    text: "↑ ↓ choose    Enter select" + (app.paletteBook > 0 ? "    ← back" : "") + "    Esc clear / close"
                    color: desktop.muted
                    font.pixelSize: 11 * app.scale
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                }
            }
        }
    }

    Drawer {
        id: savedDrawer
        objectName: "savedDrawer"
        width: Math.min(430 * app.scale, app.width - 48)
        height: app.height
        edge: Qt.RightEdge
        modal: true
        focus: true
        padding: 24 * app.scale
        topPadding: padding
        bottomPadding: padding
        leftPadding: padding
        rightPadding: padding
        closePolicy: deleteConfirmation.visible ? Popup.NoAutoClose : Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle { color: desktop.background; border.color: desktop.border }
        Overlay.modal: Rectangle { color: app.tint(desktop.background, 0.65) }
        onOpened: app.moveSaved(0)
        onClosed: app.restorePanelFocus()
        contentItem: ColumnLayout {
            objectName: "savedContent"
            Shortcut { sequence: "Ctrl+L"; enabled: savedDrawer.opened && !deleteConfirmation.visible && !bible.busy; onActivated: savedDrawer.close() }
            Shortcut { sequences: ["J", "Down"]; enabled: savedDrawer.opened && !deleteConfirmation.visible; onActivated: app.moveSaved(1) }
            Shortcut { sequences: ["K", "Up"]; enabled: savedDrawer.opened && !deleteConfirmation.visible; onActivated: app.moveSaved(-1) }
            Shortcut { sequences: ["H", "Left"]; enabled: savedDrawer.opened && !deleteConfirmation.visible; onActivated: app.filterSaved(-1) }
            Shortcut { sequences: ["L", "Right"]; enabled: savedDrawer.opened && !deleteConfirmation.visible; onActivated: app.filterSaved(1) }
            Shortcut { sequences: ["Return", "Enter"]; enabled: savedDrawer.opened && !deleteConfirmation.visible && app.activeFocusItem === collectionList; onActivated: app.openSaved(collectionList.model[collectionList.currentIndex]) }
            Shortcut { sequences: ["Delete", "Backspace"]; enabled: savedDrawer.opened && !deleteConfirmation.visible && !bible.busy; onActivated: app.requestDeleteSaved() }
            spacing: (savedDrawer.height < 350 * app.scale ? 6 : 12) * app.scale
            RowLayout {
                Layout.fillWidth: true
                Label { objectName: "savedTitle"; text: "Saved"; font.pixelSize: 24 * app.scale; font.weight: Font.Medium; color: desktop.foreground; Layout.fillWidth: true; wrapMode: Text.Wrap }
            }
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
                        onClicked: app.filterSaved(index - app.collectionTab)
                        Keys.onReturnPressed: clicked()
                        Keys.onEnterPressed: clicked()
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
                onModelChanged: currentIndex = count > 0 ? Math.max(0, Math.min(currentIndex, count - 1)) : -1
                ScrollBar.vertical: OverlayScrollbar { id: savedScrollbar; objectName: "savedScrollbar" }
                delegate: SavedEntry {
                    id: savedEntry
                    objectName: "savedEntry" + modelData.reference
                    focusPolicy: Qt.StrongFocus
                    width: collectionList.width
                    padding: 12 * app.scale
                    height: savedContent.implicitHeight + topPadding + bottomPadding
                    clip: true
                    highlighted: ListView.isCurrentItem
                    onClicked: app.openSaved(modelData)
                    Keys.onReturnPressed: clicked()
                    Keys.onEnterPressed: clicked()
                    contentItem: RowLayout {
                        id: savedContent
                        objectName: "savedEntryContent" + savedEntry.modelData.reference
                        spacing: 12 * app.scale
                        ColumnLayout {
                            Layout.alignment: Qt.AlignTop
                            spacing: 7 * app.scale
                            visible: savedEntry.modelData.bookmark || savedEntry.modelData.note.length > 0
                            BookmarkGlyph { objectName: "savedBookmark" + savedEntry.modelData.reference; visible: savedEntry.modelData.bookmark; Layout.alignment: Qt.AlignHCenter }
                            NoteGlyph { objectName: "savedNote" + savedEntry.modelData.reference; visible: savedEntry.modelData.note.length > 0; Layout.alignment: Qt.AlignHCenter }
                        }
                        ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 7 * app.scale
                        Label { text: savedEntry.modelData.reference; color: desktop.foreground; font.pixelSize: 16 * app.scale; font.weight: Font.Medium; Layout.fillWidth: true; wrapMode: Text.Wrap }
                        Label { text: savedEntry.modelData.note.replace(/\s+/g, " "); textFormat: Text.PlainText; visible: text.length > 0; color: desktop.muted; font.pixelSize: 12 * app.scale; maximumLineCount: 3; elide: Text.ElideRight; wrapMode: Text.Wrap; Layout.fillWidth: true }
                        }
                    }
                    background: Rectangle {
                        color: savedEntry.highlighted ? app.tint(desktop.accent, 0.11) : savedEntry.hovered ? desktop.surface : "transparent"
                        radius: 0
                        border.width: savedEntry.highlighted || savedEntry.activeFocus ? 1 : 0
                        border.color: desktop.accent
                        Rectangle {
                            objectName: "savedHighlight" + savedEntry.modelData.reference
                            visible: !!savedEntry.modelData.highlight
                            width: 3 * app.scale
                            height: parent.height
                            color: desktop.accent
                        }
                    }
                }
                Label {
                    objectName: "savedEmpty"
                    anchors.centerIn: parent
                    width: parent.width - 24
                    text: ["No saved passages yet.", "No bookmarks yet.", "No highlights yet.", "No notes yet."][app.collectionTab]
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    color: desktop.muted
                    font.pixelSize: 12 * app.scale
                    lineHeight: 1.2
                    visible: collectionList.count === 0
                }
            }
            FineRule { Layout.fillWidth: true }
            RowLayout {
                Layout.fillWidth: true
                QuietButton { objectName: "savedExport"; text: "Export notes…"; compact: true; Layout.fillWidth: true; Layout.minimumWidth: 0; Keys.onReturnPressed: clicked(); Keys.onEnterPressed: clicked(); onClicked: { savedDrawer.close(); app.runCommand("export") } }
                Item { Layout.fillWidth: true }
                QuietButton { objectName: "savedBackup"; text: "Backup…"; compact: true; Layout.fillWidth: true; Layout.minimumWidth: 0; Keys.onReturnPressed: clicked(); Keys.onEnterPressed: clicked(); onClicked: { savedDrawer.close(); app.runCommand("backup") } }
            }
        }
    }

    ModalCard {
        id: deleteConfirmation
        objectName: "deleteConfirmation"
        property string reference: ""
        property int row: -1
        closePolicy: Popup.CloseOnEscape
        onOpened: deleteCancel.forceActiveFocus()
        Shortcut {
            sequences: ["Left", "Up", "Right", "Down"]
            enabled: deleteConfirmation.opened && !errorDialog.visible
            onActivated: {
                if (deleteCancel.activeFocus) deleteConfirm.forceActiveFocus()
                else deleteCancel.forceActiveFocus()
            }
        }
        onClosed: {
            reference = ""
            Qt.callLater(function() { if (savedDrawer.opened) app.moveSaved(0); else app.restorePanelFocus() })
        }
        contentItem: ColumnLayout {
            spacing: 18 * app.scale
            Label { text: "Delete saved passage?"; color: desktop.foreground; font.pixelSize: 22 * app.scale; Layout.fillWidth: true; wrapMode: Text.Wrap }
            Label { objectName: "deleteReference"; text: deleteConfirmation.reference; textFormat: Text.PlainText; color: desktop.accent; font.pixelSize: 16 * app.scale; Layout.fillWidth: true; wrapMode: Text.Wrap }
            Label { objectName: "deleteExplanation"; text: "This deletes all marks (bookmark and highlight) and the note for this exact reference."; color: desktop.foreground; font.pixelSize: 14 * app.scale; Layout.fillWidth: true; wrapMode: Text.Wrap }
            Label { text: bible.error; visible: text.length > 0; color: desktop.accent; Layout.fillWidth: true; wrapMode: Text.Wrap }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                QuietButton { id: deleteCancel; objectName: "deleteCancel"; text: "Cancel"; onClicked: deleteConfirmation.close(); Keys.onReturnPressed: clicked(); Keys.onEnterPressed: clicked() }
                QuietButton { id: deleteConfirm; objectName: "deleteConfirm"; text: "Delete"; emphasized: true; onClicked: app.confirmDeleteSaved(); Keys.onReturnPressed: clicked(); Keys.onEnterPressed: clicked() }
            }
        }
    }

    ModalCard {
        id: helpDialog
        objectName: "helpDialog"
        property string pendingAction: ""
        height: Math.min(660 * app.scale, app.height - 48)
        onOpened: { keybindingSearch.text = ""; keybindingSearch.forceActiveFocus() }
        onClosed: {
            let action = pendingAction
            pendingAction = ""
            if (action) Qt.callLater(function() { app.runCommand(action) })
            else app.restorePanelFocus()
        }
        contentItem: ColumnLayout {
            spacing: 12 * app.scale
            Keys.onPressed: function(event) {
                if (event.modifiers !== Qt.NoModifier) return
                if ([Qt.Key_Home, Qt.Key_End, Qt.Key_PageUp, Qt.Key_PageDown].includes(event.key)) {
                    if (event.key === Qt.Key_PageUp || event.key === Qt.Key_PageDown)
                        app.pageKeybindings(event.key === Qt.Key_PageDown ? 1 : -1)
                    else app.keybindingEdge(event.key === Qt.Key_End)
                    event.accepted = true
                }
            }
            RowLayout {
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
                onTextChanged: { keybindingResults.currentIndex = 0; keybindingResults.positionViewAtBeginning() }
                Keys.onUpPressed: app.moveKeybinding(-1)
                Keys.onDownPressed: app.moveKeybinding(1)
                Keys.onPressed: function(event) {
                    if (event.modifiers !== Qt.NoModifier) return
                    if ([Qt.Key_Home, Qt.Key_End, Qt.Key_PageUp, Qt.Key_PageDown].includes(event.key)) {
                        if (event.key === Qt.Key_PageUp || event.key === Qt.Key_PageDown)
                            app.pageKeybindings(event.key === Qt.Key_PageDown ? 1 : -1)
                        else app.keybindingEdge(event.key === Qt.Key_End)
                        event.accepted = true
                    }
                }
                onAccepted: app.activateKeybinding(keybindingResults.model[keybindingResults.currentIndex])
                Keys.onEscapePressed: helpDialog.close()
                Accessible.name: "Search keybindings by action or key"
            }
            }
            ListView {
                id: keybindingResults
                objectName: "keybindingResults"
                // This small catalog is fully laid out for geometric paging of wrapped rows.
                cacheBuffer: Math.max(0, contentHeight)
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 3 * app.scale
                model: app.filteredKeybindings()
                currentIndex: 0
                Keys.onUpPressed: app.moveKeybinding(-1)
                Keys.onDownPressed: app.moveKeybinding(1)
                Keys.onReturnPressed: app.activateKeybinding(model[currentIndex])
                Keys.onEnterPressed: app.activateKeybinding(model[currentIndex])
                ScrollBar.vertical: OverlayScrollbar { id: keybindingScrollbar; objectName: "keybindingScrollbar" }
                delegate: ItemDelegate {
                    id: keybindingRow
                    required property var modelData
                    objectName: "keybinding" + modelData.title
                    width: keybindingResults.width
                    height: keybindingContent.implicitHeight + 20 * app.scale
                    enabled: app.actionAvailable(modelData)
                    highlighted: ListView.isCurrentItem
                    onClicked: app.activateKeybinding(modelData)
                    Keys.onReturnPressed: clicked()
                    Keys.onEnterPressed: clicked()
                    background: Rectangle { color: keybindingRow.highlighted ? app.tint(desktop.accent, 0.11) : keybindingRow.hovered ? desktop.surface : "transparent" }
                    contentItem: RowLayout {
                    id: keybindingContent
                    spacing: 12 * app.scale
                    Label {
                        text: keybindingRow.modelData.title; color: keybindingRow.enabled ? desktop.foreground : desktop.muted
                        font.pixelSize: 13 * app.scale; Layout.fillWidth: true
                        Layout.preferredWidth: keybindingRow.width * 0.48; wrapMode: Text.Wrap
                    }
                    Label {
                        text: app.actionHint(keybindingRow.modelData); color: desktop.muted
                        font.pixelSize: 12 * app.scale; horizontalAlignment: Text.AlignRight
                        Layout.fillWidth: true; Layout.preferredWidth: keybindingRow.width * 0.48; wrapMode: Text.Wrap
                    }
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
            FineRule { objectName: "keybindingFooterRule"; Layout.fillWidth: true }
            Label { text: "↑ ↓ choose    Enter select    Esc close"; color: desktop.muted; font.pixelSize: 11 * app.scale; Layout.fillWidth: true; wrapMode: Text.Wrap }
        }
    }

    ModalCard {
        id: settingsDialog
        objectName: "settingsDialog"
        property bool lineOnly: false
        width: Math.min(480 * app.scale, app.width - 48)
        height: Math.min(settingsContent.implicitHeight + 48, app.height - 48)
        onOpened: lineHeightSlider.forceActiveFocus()
        onClosed: app.restorePanelFocus()
        contentItem: OverlayScrollView {
            id: settingsScroll
            objectName: "settingsScroll"
            contentWidth: availableWidth
            clip: true
        ColumnLayout {
            id: settingsContent
            width: settingsScroll.availableWidth
            spacing: 12 * app.scale
            RowLayout {
                Label { text: settingsDialog.lineOnly ? "Line height" : "Reading settings"; color: desktop.foreground; font.pixelSize: 20 * app.scale; Layout.fillWidth: true }
            }
            Repeater {
                model: app.actions.filter(function(action) { return !!action.setting })
                delegate: SettingSwitch {
                    required property var modelData
                    catalogAction: modelData
                    visible: !settingsDialog.lineOnly
                    Layout.fillWidth: true
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Label { text: "Line height"; color: desktop.foreground; font.pixelSize: 13 * app.scale }
                Item { Layout.fillWidth: true }
                Label { objectName: "lineHeightValue"; text: bible.lineHeight.toFixed(1); color: desktop.accent; font.pixelSize: 14 * app.scale }
            }
            SquareSlider {
                id: lineHeightSlider
                objectName: "lineHeightSlider"
                Layout.fillWidth: true
                from: 1; to: 3; stepSize: 0.1
                snapMode: Slider.SnapAlways
                value: bible.lineHeight
                onMoved: bible.setLineHeight(Math.round(value * 10) / 10)
                Accessible.name: "Line height"
            }
            Label { text: "Ctrl [ decrease    Ctrl ] increase    Ctrl \\ reset"; color: desktop.muted; font.pixelSize: 11 * app.scale; wrapMode: Text.Wrap; Layout.fillWidth: true }
        }
        }
    }

    ModalCard {
        id: aboutDialog
        objectName: "aboutDialog"
        onClosed: app.restorePanelFocus()
        contentItem: OverlayScrollView {
            id: aboutScroll
            objectName: "aboutScroll"
            implicitHeight: Math.min(aboutContent.implicitHeight, app.height - 110)
            contentWidth: availableWidth
            clip: true
        ColumnLayout {
            id: aboutContent
            width: aboutScroll.availableWidth
            spacing: 16
            RowLayout {
                Label { text: "OmaBible"; color: desktop.foreground; font.pixelSize: 30 * app.scale; font.weight: Font.Medium; font.letterSpacing: -1; Layout.fillWidth: true }
            }
            SectionLabel { text: "READ SLOWLY. KEEP WHAT MATTERS." }
            Label { text: "An offline NET Bible reader with local bookmarks, highlights, and Markdown notes. Built with Qt, at home on your desktop."; color: desktop.foreground; wrapMode: Text.Wrap; Layout.fillWidth: true; font.pixelSize: 14 * app.scale; lineHeight: 1.4 }
            FineRule { Layout.fillWidth: true }
            Label { text: "New English Translation (NET Bible)\nNET Bible® copyright © 1996–2019 by Biblical Studies Press, L.L.C. All rights reserved. The Bible text is separately copyrighted and is imported from your own NET 2.1 EPUB; it is not distributed with OmaBible."; color: desktop.muted; wrapMode: Text.Wrap; Layout.fillWidth: true; font.pixelSize: 12 * app.scale; lineHeight: 1.4 }
            QuietButton { text: "NET Bible & permissions ↗"; compact: true; onClicked: desktop.openExternal("https://netbible.com/copyright/") }
            Label { text: "OmaBible " + desktop.version + " · Application code licensed under MIT\n" + (bible.edition || "NET 2.1") + " · No account required"; color: desktop.muted; font.pixelSize: 11 * app.scale; wrapMode: Text.Wrap; Layout.fillWidth: true }
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
        objectName: "exportDialog"
        title: "Choose a folder for Markdown notes"
        onAccepted: { if (!app.flushNote()) return; if (bible.exportNotes(selectedFolder)) app.notify("Notes exported as Markdown") }
    }
    FileDialog {
        id: backupDialog
        objectName: "backupDialog"
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
        onClosed: { backupFile = ""; app.restorePanelFocus() }
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

    ModalCard {
        id: errorDialog
        objectName: "errorDialog"
        property string message: ""
        onOpened: errorDismiss.forceActiveFocus()
        onClosed: app.restorePanelFocus()
        contentItem: ColumnLayout {
            spacing: 16 * app.scale
            Label { text: "Unable to complete action"; color: desktop.foreground; font.pixelSize: 20 * app.scale; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Label { objectName: "focusedErrorMessage"; text: errorDialog.message; textFormat: Text.PlainText; color: desktop.foreground; wrapMode: Text.Wrap; Layout.fillWidth: true; Accessible.role: Accessible.AlertMessage }
            QuietButton { id: errorDismiss; text: "Dismiss"; Layout.alignment: Qt.AlignRight; onClicked: errorDialog.close(); Keys.onReturnPressed: clicked(); Keys.onEnterPressed: clicked() }
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
        visible: toastTimer.running && (!bible.focusMode || app.toastInFocus)
        Label { id: toast; objectName: "toast"; anchors.centerIn: parent; width: parent.width - 36; wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter; color: desktop.foreground; font.pixelSize: 13 * app.scale; Accessible.role: Accessible.AlertMessage }
    }
    Timer { id: toastTimer; interval: 3000 }
    Timer { id: countTimer; interval: 2000; onTriggered: app.clearCount() }
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
        function onErrorChanged() { Qt.callLater(app.showFocusedError) }
        function onBusyChanged() { if (!bible.busy) Qt.callLater(app.showFocusedError) }
        function onVersesChanged() {
            app.clearTextSelection()
            app.draggingText = false
            app.dragAnchorRow = -1
        }
        function onNavigationChanged() {
            app.clearTextSelection()
            app.draggingText = false
            app.dragAnchorRow = -1
            app.clearCount()
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

    Instantiator {
        model: app.actions.filter(function(action) { return !!action.sequences })
        delegate: Shortcut {
            required property var modelData
            context: Qt.ApplicationShortcut
            sequences: modelData.id === "first" ? ["G"] : modelData.id === "last" ? ["Shift+G"] : modelData.sequences
            enabled: app.shortcutEnabled(modelData)
            onActivated: app.runCommand(modelData.id)
        }
    }
    Instantiator {
        model: 10
        delegate: Shortcut {
            required property int index
            sequence: String(index)
            enabled: app.readerKeys
            onActivated: app.countDigit(String(index))
        }
    }
    Shortcut { sequence: "Home"; enabled: app.readerKeys; onActivated: app.chapterEdge(false) }
    Shortcut { sequence: "End"; enabled: app.readerKeys; onActivated: app.chapterEdge(true) }
    Shortcut {
        sequence: "Escape"
        enabled: app.readerKeys
        onActivated: {
            if (bible.focusMode) { app.runCommand("focus"); app.readerFocus(); return }
            if (app.freeSelectedText.length) { app.clearTextSelection(); app.readerFocus(); return }
            if (app.verseCount.length) { app.clearCount(); return }
            if (!app.flushNote()) return
            if (app.verseMode) app.setVisualMode(false)
            else if (app.hasSelection) app.setVisualMode(false)
            app.readerFocus()
        }
    }
}
