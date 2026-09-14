pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import QtQml

ApplicationWindow {
    id: app
    objectName: "readerWindow"
    signal newWindowRequested()
    width: 1120
    height: 820
    minimumWidth: 640
    minimumHeight: 480
    visible: true
    title: bible.ready ? bible.passage + " — " + desktop.appName : desktop.appName
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
    property string loadedVersionId: ""
    property string loadedNoteText: ""
    property bool noteConflict: false
    property bool closingWindow: false
    property string downloadId: ""
    property url pendingDownload: ""
    property string downloadMessage: ""
    property string importingCatalogId: ""
    property bool replacingCatalogVersion: false
    function installedVersion(id) {
        return Array.from(bible.versions).some(function(entry) { return entry.id === id })
    }
    function installVersion(id) {
        if (closingWindow || bible.busy || catalog.busy || !flushNote()) return
        pendingDownload = ""
        downloadMessage = ""
        downloadId = id
        catalog.clearError()
        catalog.download(id)
    }
    function cancelDownload() {
        downloadId = ""
        pendingDownload = ""
        downloadMessage = ""
        catalog.cancel()
    }
    function importDownloaded() {
        if (closingWindow || !pendingDownload.toString().length) return
        if (bible.busy || !flushNote()) {
            downloadMessage = bible.busy ? "Download ready. Wait for the current import, then retry."
                : "Download ready. Save or resolve your note draft, then retry."
            return
        }
        const file = pendingDownload
        // Set completion context before importUsfm emits busyChanged synchronously.
        importingCatalogId = downloadId
        replacingCatalogVersion = installedVersion(downloadId)
        downloadMessage = ""
        bible.clearError()
        bible.importUsfm(file)
        if (bible.busy) {
            pendingDownload = ""
            downloadId = ""
            importGuide.close()
        } else {
            importingCatalogId = ""
            replacingCatalogVersion = false
            downloadMessage = bible.error || "Download ready. Resolve any unsaved drafts, then retry."
        }
    }
    readonly property bool noteScopeMatches: loadedVersionId === bible.versionId && loadedNoteReference === bible.selectionReference
    readonly property string observedNoteText: bible.noteText
    onObservedNoteTextChanged: syncNote()
    readonly property bool observedReady: bible.ready
    onObservedReadyChanged: { if (observedReady) restorePosition() }
    property bool verseMode: false
    property var pendingActionCollapse: null
    readonly property string selectionIdentity: bible.versionId + "\n" + bible.selectionReference
        + "\n" + JSON.stringify(bible.tagScope()) + "\n" + bible.book + ":" + bible.chapter + ":" + bible.cursorVerse
    onSelectionIdentityChanged: {
        if (pendingActionCollapse && pendingActionCollapse.identity !== selectionIdentity)
            pendingActionCollapse = null
        openedTagActionScope = null
        openedNoteActionScope = null
        if (tagsUi) tagsUi.followSelection()
    }
    function actionScope() {
        return { identity: selectionIdentity, versionId: bible.versionId,
            reference: bible.selectionReference, range: bible.tagScope(), cursor: bible.cursorVerse, visual: verseMode }
    }
    function sameTagScope(a, b) {
        return !!a && !!b && ["versionId", "reference", "book", "chapter", "start", "endBook", "endChapter", "end"].every(function(key) {
            return a[key] === b[key]
        })
    }
    function completeRangeAction(scope, pinned) {
        if (!scope || !scope.visual || !verseMode || scope.identity !== selectionIdentity) return
        verseMode = false
        clearCount()
        pendingActionCollapse = scope
        if (!pinned) editingPanelClosed()
    }
    function editingPanelClosed() {
        if (notesOpen || tagsUi.scopePinned || !pendingActionCollapse) return
        const scope = pendingActionCollapse
        pendingActionCollapse = null
        if (scope.identity !== selectionIdentity) return
        bible.selectVerses(scope.cursor, scope.cursor)
        selectionAnchor = bible.cursorVerse
        syncNote()
    }
    property var openedTagActionScope: null
    property var openedNoteActionScope: null
    function tagActionCompleted(scope) {
        const captured = openedTagActionScope
        if (scope && captured && !sameTagScope(scope, captured.range)) return
        completeRangeAction(captured, true)
    }
    function openPassageTags(scope) {
        if (bible.busy || !flushNote()) return
        if (typeof scope === "string") {
            if (!bible.navigate(scope)) return
            scope = bible.tagScope()
        }
        clearCount()
        openedTagActionScope = actionScope()
        // Marker scopes already contain immutable endpoint metadata. Opening a
        // picker must not navigate or reduce that range to the clicked verse.
        if (scope && !sameTagScope(scope, openedTagActionScope.range))
            openedTagActionScope = null
        tagsUi.open(scope || bible.tagScope())
    }
    function openTagsBrowser(manage) {
        if (bible.busy || !flushNote()) return
        clearCount()
        if (manage) tagsUi.openBrowser(true)
        else tagsUi.openSummary()
    }
    readonly property alias tagsUi: tagsUi
    readonly property var tagBackend: bible
    readonly property var tagDesktop: desktop
    TagsPanel {
        id: tagsUi
        objectName: "tagsUi"
        app: app; bible: app.tagBackend; desktop: app.tagDesktop
        embeddedHost: annotationsTagsHost
        onBrowserRequested: { app.collectionTab = 3; savedDrawer.open() }
        onBrowserClosed: savedDrawer.close()
    }
    onVerseModeChanged: clearCount()
    property string verseCount: ""
    onActiveFocusItemChanged: clearCount()
    property bool restoringPosition: true
    property bool positionJumpToSelection: false
    readonly property int readerSize: bible.fontSize
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
    readonly property bool modalOpen: tagsUi.visibleModal || verseTagMenu.visible || backupScope.visible || versionDialog.visible || passagePalette.visible || savedDrawer.visible || deleteConfirmation.visible || helpDialog.visible || settingsDialog.visible || aboutDialog.visible || importGuide.visible || catalogDetails.visible || errorDialog.visible || restoreConfirmation.visible || importDialog.visible || exportDialog.visible || backupDialog.visible || restoreDialog.visible || !!(blockLoader.item as BlockReader)?.annotationMenuOpen
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
    readonly property real verseGutter: 76 * scale
    // Source rows, rather than numeric verse labels, define chapter intervals.
    // Union each kind's continuous coverage: overlapping logical ranges already
    // share one rail. Allocate once per interval so later starts/ends never bend it.
    readonly property var annotationLanes: packAnnotationLanes(bible.verses)
    function packAnnotationLanes(sourceRows) {
        const rows = Array.from(sourceRows), kinds = ["bookmark", "tag", "note"]
        const intervals = [], lanes = rows.map(function() { return {} })
        kinds.forEach(function(kind, priority) {
            let interval = null
            rows.forEach(function(row, ordinal) {
                if (Array.from(row[kind + "Ranges"] || []).length) {
                    if (!interval) {
                        interval = { kind: kind, priority: priority, start: ordinal, end: ordinal }
                        intervals.push(interval)
                    }
                    interval.end = ordinal
                } else interval = null
            })
        })
        intervals.sort(function(a, b) { return a.start - b.start || a.priority - b.priority })
        const occupiedUntil = [-1, -1, -1]
        intervals.forEach(function(interval) {
            const lane = occupiedUntil.findIndex(function(end) { return end < interval.start })
            occupiedUntil[lane] = interval.end
            for (let row = interval.start; row <= interval.end; ++row) lanes[row][interval.kind] = lane
        })
        return lanes
    }
    readonly property bool continuousReader: bible.readingMode === "block" || (bible.readingMode === "paragraph" && bible.hasParagraphs)
    property bool readerInitialized: false
    readonly property string observedReadingMode: bible.readingMode
    onObservedReadingModeChanged: {
        if (!readerInitialized) return
        clearTextSelection()
        readingModeRestoreTimer.restart()
    }
    readonly property bool observedFocusMode: bible.focusMode
    onObservedFocusModeChanged: { toastTimer.stop(); Qt.callLater(app.applyFocusMode) }
    function applyFocusMode() {
        if (observedFocusMode) {
            if (!closeNotes()) { bible.setFocusMode(false); return }
            tagsUi.closeForFocusMode()
            verseTagMenu.close()
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
        const scope = actionScope()
        const inside = hasSelection && number >= bible.selectedStart && number <= bible.selectedEnd
        const reference = inside ? bible.selectionReference : bible.passage + ":" + number
        // The selection API preserves an inside-hit range. copyVerse focuses an
        // outside hit only after the backend has successfully read/copied it.
        const copied = inside ? bible.copySelection(plainText) : bible.copyVerse(number, plainText)
        if (copied.length) {
            if (inside) completeRangeAction(scope, false)
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
    function tagsPlainText(tags) { return Array.from(tags || []).map(function(t) { return t.name }).join(", ") }
    component TagGlyph: AnnotationGlyph {
        kind: "tag"
        implicitWidth: 12 * app.scale
        implicitHeight: 14 * app.scale
        width: implicitWidth; height: implicitHeight
        ink: desktop.accent
    }
    Menu {
        id: verseTagMenu
        objectName: "verseTagMenu"
        property var entries: []
        property string annotationKind: "tag"
        modal: true
        focus: true
        popupType: Popup.Item
        onOpened: { currentIndex = 0; if (itemAt(0)) itemAt(0).forceActiveFocus() }
        background: Rectangle {
            implicitWidth: 260 * app.scale
            color: desktop.background; border.color: desktop.border
            radius: Math.min(desktop.cornerRadius, width / 2, height / 2)
        }
        Instantiator {
            model: verseTagMenu.entries
            delegate: MenuItem {
                id: verseTagAction
                required property var modelData
                objectName: "verseTagAction:" + modelData.reference
                text: modelData.reference + (verseTagMenu.annotationKind === "tag" ? " · " + app.tagsPlainText(modelData.tags)
                    : verseTagMenu.annotationKind === "bookmark" ? " · Bookmark" : " · Note")
                onTriggered: {
                    if (verseTagMenu.annotationKind === "note") app.openNoteScope(modelData)
                    else if (verseTagMenu.annotationKind === "bookmark") app.openBookmark(modelData)
                    else app.openPassageTags(modelData)
                }
                Keys.onPressed: function(event) { if (event.modifiers !== Qt.NoModifier) event.accepted = true }
                Keys.onReleased: function(event) { if (event.modifiers !== Qt.NoModifier) event.accepted = true }
                contentItem: Text { text: verseTagAction.text; textFormat: Text.PlainText; color: desktop.foreground; font: verseTagAction.font }
                background: Rectangle {
                    implicitHeight: 36 * app.scale
                    color: verseTagAction.highlighted ? app.tint(desktop.accent, 0.16) : "transparent"
                    radius: Math.min(desktop.cornerRadius, width / 2, height / 2)
                }
            }
            onObjectAdded: function(index, object) { verseTagMenu.insertItem(index, object) }
            onObjectRemoved: function(index, object) { verseTagMenu.removeItem(object) }
        }
    }
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
        if (noteConflict && (!noteScopeMatches || noteEditor.text !== bible.noteText)) return
        // A failed local save leaves the editor ahead of its last synced value.
        // Peer notifications must neither replace that draft nor authorize a save
        // over the peer's newer text. Local successful saves match the editor.
        if (!syncingNote && noteEditor.text !== loadedNoteText) {
            if (!noteScopeMatches || (bible.noteText !== loadedNoteText && bible.noteText !== noteEditor.text)) {
                noteConflict = true
                return
            }
            if (noteEditor.text !== bible.noteText) return
        }
        loadSavedNote()
    }
    function loadSavedNote() {
        syncingNote = true
        loadedNoteReference = bible.selectionReference
        loadedVersionId = bible.versionId
        loadedNoteText = bible.noteText
        if (noteEditor.text !== bible.noteText)
            noteEditor.text = bible.noteText
        noteConflict = false
        syncingNote = false
    }
    function flushNote() {
        if (syncingNote) return true
        syncNote()
        if (noteConflict) return false
        if (hasSelection && noteScopeMatches && noteEditor.text !== bible.noteText) {
            const scope = openedNoteActionScope
            bible.setNoteText(noteEditor.text)
            if (noteScopeMatches && noteEditor.text === bible.noteText) completeRangeAction(scope, true)
        }
        // Also advance the baseline when setNoteText was a no-op or emitted no signal.
        syncNote()
        return !noteConflict && noteEditor.text === loadedNoteText
    }
    function retryNote() {
        if (!hasSelection || !noteScopeMatches || bible.busy) return
        // Explicit user choice to replace the current saved text with this draft.
        loadedNoteText = bible.noteText
        noteConflict = false
        flushNote()
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
        pendingActionCollapse = null
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
        pendingActionCollapse = null
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
        let chapters = []
        for (let book of books) {
            for (let number of book.chapterNumbers) chapters.push({ book: book.id, chapter: number })
        }
        let current = chapters.findIndex(function(entry) { return entry.book === bible.book && entry.chapter === bible.chapter })
        let target = Math.max(0, Math.min(chapters.length - 1, current + delta))
        if (current >= 0 && target !== current) {
            bible.openChapter(chapters[target].book, chapters[target].chapter)
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
        if (!notesOpen) openedNoteActionScope = actionScope()
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
        editingPanelClosed()
        return true
    }
    function toggleNotes() {
        if (notesOpen) closeNotes()
        else openNotes(true)
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
            notify("Restored " + bible.versionAbbreviation + " annotation backup", true)
        }
    }
    function copyPassage(plainText) {
        clearCount()
        if (!flushNote()) return
        const scope = actionScope()
        const reference = hasSelection ? bible.selectionReference : bible.passage
        if (bible.copySelection(!!plainText).length > 0) {
            completeRangeAction(scope, false)
            notify(reference + (plainText ? " copied as plain text" : " copied"))
        }
    }
    function markPassage(highlight) {
        if (!hasSelection || !flushNote()) return
        const scope = actionScope()
        const success = highlight ? bible.toggleHighlight() : bible.toggleBookmark()
        if (success) completeRangeAction(scope, false)
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
            return (collectionTab === 0 && entry.bookmark)
                || (collectionTab === 1 && entry.highlight) || (collectionTab === 2 && entry.note.length > 0)
        })
    }
    function moveSaved(delta) {
        if (collectionTab === 3) return
        if (collectionList.count > 0) {
            collectionList.currentIndex = Math.max(0, Math.min(collectionList.count - 1, collectionList.currentIndex + delta))
            collectionList.positionViewAtIndex(collectionList.currentIndex, ListView.Contain)
        }
        if (collectionList.currentItem) collectionList.currentItem.focus = false
        collectionList.forceActiveFocus()
    }
    function filterSaved(delta) {
        const tab = (collectionTab + delta + 4) % 4
        if (tab === 3) { openTagsBrowser(); return }
        tagsUi.embeddedClosed()
        collectionTab = tab
        collectionList.currentIndex = collectionList.count > 0 ? 0 : -1
        moveSaved(0)
    }
    function openSaved(entry) {
        if (!entry || !flushNote()) return
        savePosition()
        if (!bible.openTaggedPassage(entry)) return
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
        if (!entry || bible.busy) return
        // Freeze identity before flushing drafts can publish a replacement model.
        const scope = Object.freeze({ versionId: entry.versionId, reference: entry.reference,
            book: entry.book, chapter: entry.chapter, start: entry.start,
            endBook: entry.endBook, endChapter: entry.endChapter, end: entry.end })
        if (!flushNote() || bible.busy || !savedScopeExists(scope)) return
        deleteConfirmation.scope = scope
        deleteConfirmation.row = row
        deleteConfirmation.open()
    }
    function savedScopeExists(scope) {
        return !!scope && !!scope.versionId && scope.versionId === bible.versionId
            && Array.from(bible.collection).some(function(entry) {
                return ["versionId", "book", "chapter", "start", "endBook", "endChapter", "end"].every(function(key) {
                    return entry[key] === scope[key]
                })
            })
    }
    function deleteScopeValid() {
        return savedScopeExists(deleteConfirmation.scope)
    }
    function invalidateDeleteSaved() {
        deleteConfirmation.scope = null
        deleteConfirmation.row = -1
        deleteConfirmation.close()
    }
    function confirmDeleteSaved() {
        if (!deleteConfirmation.opened || !deleteScopeValid()) { invalidateDeleteSaved(); return }
        const scope = deleteConfirmation.scope
        const row = deleteConfirmation.row
        if (!flushNote() || bible.busy) return
        if (!deleteScopeValid() || deleteConfirmation.scope !== scope) {
            invalidateDeleteSaved()
            return
        }
        if (!bible.deleteAnnotationScope(scope)) return
        syncNote()
        collectionList.currentIndex = Math.min(row, collectionList.count - 1)
        deleteConfirmation.close()
    }
    function openBookmark(scope) {
        if (!flushNote()) return
        savePosition()
        if (typeof scope === "string" ? bible.navigate(scope) : bible.openTaggedPassage(scope)) { syncNote(); readerFocus() }
    }
    function openNoteScope(scope) {
        if (!scope || !flushNote()) return
        savePosition()
        if (!bible.openTaggedPassage(scope)) return
        syncNote()
        openNotes(true)
        Qt.callLater(function() { app.scrollToVerse(bible.cursorVerse, true) })
    }
    // Row metadata already represents range membership, including chapter
    // continuations. Match canonical identities rather than source display names.
    readonly property var cursorContextScopes: {
        if (bible.readingMode !== "paragraph" || continuousReader || bible.focusMode) return []
        const row = Array.from(bible.verses).find(function(verse) { return verse.number === bible.cursorVerse })
        return row ? Array.from(row.bookmarkRanges || []).concat(Array.from(row.tagRanges || []), Array.from(row.noteRanges || [])) : []
    }
    function rowInCursorContext(row) {
        return Array.from(row.bookmarkRanges || []).concat(Array.from(row.tagRanges || []), Array.from(row.noteRanges || [])).some(function(range) {
            return cursorContextScopes.some(function(scope) { return sameTagScope(range, scope) })
        })
    }
    function openVersions(removing) {
        if (bible.busy || !flushNote()) return
        clearCount()
        versionDialog.removing = !!removing
        versionDialog.step = 0
        versionDialog.chosen = null
        versionDialog.removeAnnotations = false
        versionSearch.text = ""
        versionResults.currentIndex = 0
        versionDialog.open()
    }
    function versionEntries() {
        if (versionDialog.step === 1) return [
            { name: "Keep annotations", hint: "Reconnect when this version is reimported", remove: false },
            { name: "Remove annotations too", hint: "Delete this version's notes, bookmarks, and highlights", remove: true }
        ]
        const query = versionSearch.text.trim().toLowerCase()
        return Array.from(bible.versions).filter(function(entry) {
            return !query || (entry.abbreviation + " " + entry.name).toLowerCase().includes(query)
        })
    }
    function moveVersion(delta) {
        versionResults.currentIndex = versionResults.count ? Math.max(0, Math.min(versionResults.count - 1, versionResults.currentIndex + delta)) : -1
        if (versionResults.count) versionResults.positionViewAtIndex(versionResults.currentIndex, ListView.Contain)
    }
    function versionKey(event) {
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            event.accepted = true
            if (event.modifiers === Qt.NoModifier) acceptVersion()
            return
        }
        if (event.modifiers !== Qt.NoModifier) return
        const keys = [Qt.Key_Up, Qt.Key_Down, Qt.Key_Home, Qt.Key_End, Qt.Key_PageUp, Qt.Key_PageDown]
        if (versionDialog.step === 2) keys.push(Qt.Key_Left, Qt.Key_Right)
        if (!keys.includes(event.key)) return
        event.accepted = true
        if (versionDialog.step === 2) {
            if (versionCancel.activeFocus) versionNext.forceActiveFocus()
            else versionCancel.forceActiveFocus()
            return
        }
        const page = Math.max(1, Math.floor(versionResults.height / (64 * scale)))
        moveVersion(event.key === Qt.Key_Home ? -versionResults.count : event.key === Qt.Key_End ? versionResults.count
            : event.key === Qt.Key_PageUp ? -page : event.key === Qt.Key_PageDown ? page : event.key === Qt.Key_Up ? -1 : 1)
    }
    function acceptVersion() {
        if (bible.busy) return
        if (versionAdd.activeFocus) { addVersionFromPicker(); return }
        if (versionCancel.activeFocus) { versionDialog.close(); return }
        if (versionBack.activeFocus) { backVersion(); return }
        if (versionDialog.step === 2) { confirmRemoveVersion(); return }
        const entry = versionResults.model[versionResults.currentIndex]
        if (!entry) return
        if (!versionDialog.removing) { switchVersion(entry.id); return }
        if (versionDialog.step === 0) versionDialog.chosen = entry
        else versionDialog.removeAnnotations = !!entry.remove
        versionDialog.step++
        versionResults.currentIndex = 0
        if (versionDialog.step === 2) versionCancel.forceActiveFocus()
        else versionResults.forceActiveFocus()
    }
    function backVersion() {
        if (bible.busy || versionDialog.step === 0) return
        versionDialog.step--
        versionResults.currentIndex = versionDialog.step === 1 && versionDialog.removeAnnotations ? 1 : 0
        if (versionDialog.step === 0) versionSearch.forceActiveFocus()
        else versionResults.forceActiveFocus()
    }
    function addVersionFromPicker() {
        if (versionDialog.removing || versionDialog.step !== 0 || bible.busy || !flushNote()) return
        versionDialog.addPending = true
        versionDialog.close()
    }
    function switchVersion(id) {
        if (bible.busy || !flushNote()) return
        const selected = hasSelection
        savePosition()
        if (!bible.switchVersion(id)) return
        clearTextSelection()
        selectionAnchor = 0
        syncNote()
        restorePosition(hasSelection)
        versionDialog.close()
        notify(selected && !hasSelection ? bible.status : "Switched to " + bible.versionAbbreviation, true)
    }
    function confirmRemoveVersion() {
        if (!versionDialog.removing || versionDialog.step !== 2 || !versionDialog.chosen || bible.busy || !flushNote()) return
        if (!bible.removeVersion(versionDialog.chosen.id, versionDialog.removeAnnotations)) return
        clearTextSelection()
        selectionAnchor = 0
        syncNote()
        if (bible.ready) restorePosition(hasSelection)
        else notesDrawer.close()
        versionDialog.close()
    }
    // One action catalog supplies shortcuts, searchable keybindings, and commands.
    // Context-only gestures are explanatory rows, never executable guesses.
    readonly property var actions: [
        { id: "books", title: "Search for a passage", keys: "s · Ctrl k", sequences: ["S", "Ctrl+K"], scope: "palette", command: true },
        { id: "commands", title: "Open commands", keys: "Ctrl Space · Ctrl Shift P", sequences: ["Ctrl+Space", "Ctrl+Shift+P"], scope: "palette" },
        { id: "newWindow", title: "New window", keys: "Ctrl Shift N", sequences: ["Ctrl+Shift+N"], scope: "global", command: true },
        { id: "annotations", title: "Annotations panel", keys: "Ctrl a", sequences: ["Ctrl+A"], command: true },
        { id: "toggleNotes", title: "Notes panel", keys: "Ctrl n", sequences: ["Ctrl+N"], scope: "global", command: true },
        { id: "notes", title: "Write a note", keys: "n", sequences: ["N"], scope: "notes", command: true },
        { id: "tags", title: "Tag passage", keys: "t", sequences: ["T"], command: true },
        { id: "tagsBrowser", title: "Tags", keys: "", command: true },
        { id: "manageTags", title: "Manage tags", keys: "", command: true },
        { id: "related", title: "Related passages", keys: "Shift L", sequences: ["Shift+L"], command: true },
        { id: "copy", title: "Copy passage as quotation", keys: "y · Ctrl c", sequences: ["Y"], command: true },
        { id: "copyPlain", title: "Copy passage as plain text", keys: "Shift Y · Ctrl Shift C", sequences: ["Shift+Y", "Ctrl+Shift+C"], command: true },
        { id: "copyText", title: "Copy selected text", keys: "Ctrl c", sequences: ["Ctrl+C"] },
        { id: "clickCopy", title: "Copy passage as quotation", keys: "Right Click", explanation: true },
        { id: "middleCopy", title: "Copy passage as plain text", keys: "Middle Click", explanation: true },
        { id: "previousChapter", title: "Previous chapter", keys: "h · Left · [", sequences: ["H", "Left", "["] },
        { id: "nextChapter", title: "Next chapter", keys: "l · Right · ]", sequences: ["L", "Right", "]"] },
        { id: "previousVerse", title: "Previous verse", keys: "k · Up", sequences: ["K", "Up"] },
        { id: "nextVerse", title: "Next verse", keys: "j · Down", sequences: ["J", "Down"] },
        { id: "previousFive", title: "Previous 5 verses", keys: "Ctrl u", sequences: ["Ctrl+U"] },
        { id: "nextFive", title: "Next 5 verses", keys: "Ctrl d", sequences: ["Ctrl+D"] },
        { id: "previousTen", title: "Previous 10 verses", keys: "Ctrl b · PageUp", sequences: ["Ctrl+B", "PgUp"] },
        { id: "nextTen", title: "Next 10 verses", keys: "Ctrl f · PageDown", sequences: ["Ctrl+F", "PgDown"] },
        { id: "selectVerse", title: "Select verse", keys: "Click", explanation: true },
        { id: "selectRange", title: "Select a range", keys: "Shift Click", explanation: true },
        { id: "visual", title: "Toggle visual selection", keys: "v", sequences: ["V"] },
        { id: "extendPrevious", title: "Extend to previous verse", keys: "Shift K · Shift Up", sequences: ["Shift+K", "Shift+Up"] },
        { id: "extendNext", title: "Extend to next verse", keys: "Shift J · Shift Down", sequences: ["Shift+J", "Shift+Down"] },
        { id: "first", title: "Select chapter beginning", keys: "g · Home", sequences: ["G", "Home"] },
        { id: "last", title: "Select chapter end", keys: "Shift G · End", sequences: ["Shift+G", "End"] },
        { id: "bookmark", title: "Bookmark selection", keys: "m · Ctrl m", sequences: ["M", "Ctrl+M"], command: true },
        { id: "highlight", title: "Highlight selection", keys: "x · Ctrl h", sequences: ["X", "Ctrl+H"], command: true },
        { id: "export", title: "Export notes as Markdown", keys: "", command: true },
        { id: "backup", title: "Save annotation backup", keys: "", command: true },
        { id: "restore", title: "Restore annotation backup", keys: "", command: true },
        { id: "settings", title: "Reading settings", keys: "Shift R", sequences: ["Shift+R"], command: true },
        { id: "larger", title: "Larger text", keys: "Ctrl + · Ctrl =", sequences: ["Ctrl++", "Ctrl+="] },
        { id: "smaller", title: "Smaller text", keys: "Ctrl −", sequences: ["Ctrl+-"] },
        { id: "resetText", title: "Reset text size", keys: "Ctrl 0", sequences: ["Ctrl+0"] },
        { id: "numbers", title: "Show verse numbers", keys: "r", sequences: ["R"], setting: "showVerseNumbers", command: true },
        { id: "headings", title: "Show headings", keys: "Shift H", sequences: ["Shift+H"], setting: "showHeadings", command: true },
        { id: "versePicker", title: "Verse picker", keys: "p", sequences: ["P"], setting: "versePicker", command: true },
        { id: "readingMode", title: "Reading mode", keys: "b", sequences: ["B"], command: true },
        { id: "focus", title: "Focus mode", keys: "f", sequences: ["F"], setting: "focusMode", command: true },
        { id: "lineHeight", title: "Line height", keys: "", command: true },
        { id: "lineLess", title: "Decrease line height", keys: "Ctrl [", sequences: ["Ctrl+["] },
        { id: "lineMore", title: "Increase line height", keys: "Ctrl ]", sequences: ["Ctrl+]"] },
        { id: "lineReset", title: "Reset line height", keys: "Ctrl \\", sequences: ["Ctrl+\\"], command: true },
        { id: "switchVersion", title: "Switch version", keys: "Shift V", sequences: ["Shift+V"], scope: "global", command: true },
        { id: "removeVersion", title: "Remove version", keys: "", scope: "global", command: true },
        { id: "import", title: "Add version", keys: "", scope: "global", command: true },
        { id: "resume", title: "Resume last palette", keys: "Space", sequences: ["Space"] },
        { id: "help", title: "Keybindings", keys: "? · F1", sequences: ["?", "F1"], scope: "global", command: true },
        { id: "about", title: "About & Attribution", keys: "", scope: "global", command: true },
        { id: "goToVerse", title: "Go to verse number", keys: "number then g (e.g. 20g)", explanation: true },
        { id: "dismiss", title: "Dismiss current panel or selection", keys: "Esc", explanation: true }
    ]
    function actionHint(action) {
        if (action.id === "readingMode") return readingModeName(bible.readingMode) + " · b"
        if (action.setting) return (bible[action.setting] ? "On" : "Off") + (action.keys ? " · " + action.keys : "")
        if (action.id === "lineHeight") return bible.lineHeight.toFixed(1)
        return action.keys
    }
    function readingModeName(mode) { return mode.charAt(0).toUpperCase() + mode.slice(1) }
    function shortcutEnabled(action) {
        if (!actionAvailable(action)) return false
        // Keep the command globally available, but never consume a typed uppercase V.
        if (action.id === "switchVersion") return !modalOpen && !notesOpen && !textInputActive()
        if (settingsDialog.opened && !deleteConfirmation.visible && !errorDialog.visible
                && (action.setting || ["readingMode", "larger", "smaller", "resetText", "lineLess", "lineMore", "lineReset"].includes(action.id))) return true
        if (action.scope === "palette") return !modalOpen || passagePalette.opened
        if (action.scope === "global") return !modalOpen && (bible.ready || ["newWindow", "help", "about", "import", "switchVersion", "removeVersion"].includes(action.id))
        if (action.scope === "notes") return bible.ready && !modalOpen && !textInputActive()
        return readerKeys
    }
    function actionAvailable(action) {
        if (typeof action === "string") action = actions.find(function(entry) { return entry.id === action })
        if (!action || action.explanation || bible.busy) return false
        if (action.id === "copyText") return bible.ready
        if (["bookmark", "highlight", "tags", "related"].includes(action.id)) return hasSelection
        return bible.ready || !!action.setting
            || ["books", "commands", "newWindow", "help", "about", "import", "switchVersion", "removeVersion", "settings", "readingMode", "larger", "smaller", "resetText", "lineHeight", "lineLess", "lineMore", "lineReset"].includes(action.id)
    }
    function scopeTitle(action) {
        if (action.explanation) return "Reader gesture"
        if (action.id === "switchVersion") return "Reader shortcut · also available in commands while writing notes"
        if (action.scope === "global") return "This window · also available while writing notes"
        if (action.scope === "palette") return "This window · passage and command search"
        if (action.scope === "notes") return "Reader or notes preview"
        return "Reader · outside text editing and dialogs"
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
            if (book) for (let i of book.chapterNumbers) {
                if (!query || String(i).indexOf(query) === 0)
                    result.push({ title: paletteBookName + " " + i, hint: "Chapter " + i, book: paletteBook, chapter: i })
            }
            return result
        }
        // Resolve exact canonical/source aliases without changing the displayed source name.
        const resolved = query && !/\d\s*$/.test(query) ? bible.resolveReference(query) : null
        return Array.from(bible.books).filter(function(b) {
            return !query || b.name.toLowerCase().indexOf(query) >= 0
                || b.code.toLowerCase().indexOf(query) >= 0 || b.abbreviation.toLowerCase().indexOf(query) >= 0
                || (resolved && resolved.valid && resolved.reference === b.name + " " + b.chapterNumbers[0])
        })
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
            const row = Array.from(bible.chaptersForBook(paletteBook)).indexOf(chapter)
            paletteResults.currentIndex = row
            paletteResults.positionViewAtIndex(row, ListView.Contain)
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
        const previousReaderSize = readerSize
        switch (action) {
        case "newWindow": if (!modalOpen) newWindowRequested(); break
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
        case "larger": bible.setFontSize(readerSize + 1); break
        case "smaller": bible.setFontSize(readerSize - 1); break
        case "resetText": bible.setFontSize(18); break
        case "numbers": bible.setShowVerseNumbers(!bible.showVerseNumbers); break
        case "headings": bible.setShowHeadings(!bible.showHeadings); break
        case "versePicker": bible.setVersePicker(!bible.versePicker); break
        case "readingMode": {
            const modes = ["paragraph", "verse", "block"]
            const previousMode = bible.readingMode
            bible.setReadingMode(modes[(modes.indexOf(bible.readingMode) + 1) % modes.length])
            if (bible.readingMode !== previousMode) notify("Reading mode " + readingModeName(bible.readingMode), true)
            break
        }
        case "focus": bible.setFocusMode(!bible.focusMode); break
        case "lineLess": bible.setLineHeight(Math.round((bible.lineHeight - 0.1) * 10) / 10); break
        case "lineMore": bible.setLineHeight(Math.round((bible.lineHeight + 0.1) * 10) / 10); break
        case "lineReset": bible.setLineHeight(1.5); break
        case "lineHeight": settingsDialog.lineOnly = true; settingsDialog.open(); break
        case "settings": settingsDialog.lineOnly = false; settingsDialog.open(); break
        case "annotations":
            bible.refreshCollection()
            if (collectionTab === 3) openTagsBrowser()
            else savedDrawer.open()
            break
        case "notes": openNotes(true); break
        case "bookmark": markPassage(false); break
        case "highlight": markPassage(true); break
        case "tags": openPassageTags(); break
        case "tagsBrowser": openTagsBrowser(); break
        case "manageTags": openTagsBrowser(true); break
        case "related": if (flushNote()) tagsUi.openRelated(); break
        case "copy": if (bible.ready) copyPassage(); break
        case "copyPlain": if (bible.ready) copyPassage(true); break
        case "export": exportDialog.open(); break
        case "backup": backupScope.open(); break
        case "restore": restoreDialog.open(); break
        case "import": importGuide.open(); break
        case "switchVersion": openVersions(false); break
        case "removeVersion": openVersions(true); break
        case "help": helpDialog.open(); break
        case "about": aboutDialog.open(); break
        }
        if (catalogAction.setting && bible[catalogAction.setting] !== previousSetting)
            notify(catalogAction.title + " " + (bible[catalogAction.setting] ? "on" : "off"), true)
        if (["lineLess", "lineMore", "lineReset"].includes(action) && bible.lineHeight !== previousLineHeight)
            notify("Line height " + bible.lineHeight.toFixed(1), true)
        if (["larger", "smaller", "resetText"].includes(action) && readerSize !== previousReaderSize)
            notify("Text size " + readerSize, true)
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
            radius: Math.min(desktop.cornerRadius, width / 2, height / 2)
            color: button.down ? app.tint(desktop.accent, 0.16) : button.emphasized ? app.tint(desktop.accent, 0.10) : button.hovered ? desktop.surface : "transparent"
            border.width: button.activeFocus ? 1 : 0
            border.color: desktop.accent
        }
        AppToolTip { theme: desktop; visible: (button.hovered || button.activeFocus) && button.tip.length > 0; text: button.tip; delay: 650 }
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
                radius: Math.min(desktop.cornerRadius, width / 2, height / 2)
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
            radius: Math.min(desktop.cornerRadius, width / 2, height / 2)
            color: settingSwitch.checked ? desktop.accent : desktop.surface
            border.color: settingSwitch.activeFocus ? desktop.foreground : settingSwitch.checked ? desktop.accent : desktop.border
            Rectangle {
                objectName: settingSwitch.objectName + "Thumb"
                x: (settingSwitch.checked ? 19 : 3) * app.scale
                y: 3 * app.scale
                width: 14 * app.scale
                height: width
                radius: Math.min(desktop.cornerRadius, width / 2, height / 2)
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
        background: Rectangle { radius: Math.min(desktop.cornerRadius, width / 2, height / 2); color: settingSwitch.hovered ? desktop.surface : "transparent" }
    }
    component SquareSlider: Slider {
        id: slider
        implicitHeight: 32 * app.scale
        implicitWidth: 200 * app.scale
        padding: 9 * app.scale
        Keys.priority: Keys.BeforeItem
        Keys.onPressed: function(event) {
            if (event.modifiers === Qt.NoModifier && (event.key === Qt.Key_Left || event.key === Qt.Key_Right)) event.accepted = true
            else if (event.modifiers === Qt.NoModifier && (event.key === Qt.Key_Up || event.key === Qt.Key_Down)) {
                if (event.key === Qt.Key_Up) increase()
                else decrease()
                moved()
                event.accepted = true
            }
        }
        background: Rectangle {
            x: slider.leftPadding
            y: slider.topPadding + (slider.availableHeight - height) / 2
            width: slider.availableWidth
            height: 3 * app.scale
            radius: Math.min(desktop.cornerRadius, width / 2, height / 2)
            color: desktop.border
            Rectangle {
                objectName: slider.objectName + "Track"
                x: slider.mirrored ? parent.width - width : 0
                width: slider.position * parent.width
                height: parent.height
                radius: Math.min(desktop.cornerRadius, width / 2, height / 2)
                color: desktop.accent
            }
        }
        handle: Rectangle {
            objectName: slider.objectName + "Handle"
            x: slider.leftPadding + slider.visualPosition * (slider.availableWidth - width)
            y: slider.topPadding + (slider.availableHeight - height) / 2
            implicitWidth: 18 * app.scale
            implicitHeight: 18 * app.scale
            radius: Math.min(desktop.cornerRadius, width / 2, height / 2)
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
        AppToolTip { theme: desktop; visible: floatingButton.hovered || floatingButton.activeFocus; text: floatingButton.tip; delay: 500 }
        background: Rectangle {
            color: floatingButton.down ? app.tint(desktop.accent, 0.18)
                : floatingButton.hovered ? app.tint(desktop.accent, 0.09) : "transparent"
            radius: Math.min(desktop.cornerRadius, width / 2, height / 2)
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
                visible: floatingButton.glyph.length > 0 && !["notes", "saved", "related"].includes(floatingButton.glyph)
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
                    ctx.lineCap = "square"
                    ctx.lineJoin = "miter"
                    ctx.beginPath()
                    if (glyph === "help") {
                        ctx.moveTo(8, 7); ctx.bezierCurveTo(8, 1, 19, 2, 16, 9)
                        ctx.bezierCurveTo(15, 11, 12, 11, 12, 15)
                        ctx.moveTo(12, 19); ctx.lineTo(12, 19.3)
                    } else if (glyph === "visual") {
                        // The bracket and square handles span all three verse lines.
                        ctx.moveTo(8, 6); ctx.lineTo(21, 6)
                        ctx.moveTo(8, 12); ctx.lineTo(21, 12)
                        ctx.moveTo(8, 19); ctx.lineTo(17, 19)
                        ctx.moveTo(5, 3); ctx.lineTo(3, 3); ctx.lineTo(3, 22); ctx.lineTo(5, 22)
                        ctx.fillRect(4, 2, 3, 3)
                        ctx.fillRect(4, 21, 3, 3)
                    } else {
                        ctx.moveTo(12, 6); ctx.lineTo(8, 4); ctx.lineTo(2, 4)
                        ctx.lineTo(2, 19); ctx.lineTo(8, 19); ctx.lineTo(12, 21)
                        ctx.lineTo(16, 19); ctx.lineTo(22, 19); ctx.lineTo(22, 4)
                        ctx.lineTo(16, 4); ctx.closePath()
                        ctx.moveTo(12, 6); ctx.lineTo(12, 21)
                        ctx.moveTo(5, 8); ctx.lineTo(8, 8)
                        ctx.moveTo(16, 8); ctx.lineTo(19, 8)
                    }
                    ctx.stroke()
                }
            }
            AnnotationGlyph {
                objectName: floatingButton.objectName + "Glyph"
                anchors.centerIn: parent
                width: 22 * app.controlScale; height: width
                visible: ["notes", "saved", "related"].includes(floatingButton.glyph)
                kind: floatingButton.glyph === "notes" ? "note" : floatingButton.glyph === "related" ? "tag" : "bookmark"
                ink: floatingIcon.ink
            }
        }
    }
    component BookmarkGlyph: AnnotationGlyph {
        kind: "bookmark"
        ink: desktop.accent
        implicitWidth: 11 * app.scale
        implicitHeight: 15 * app.scale
        width: implicitWidth; height: implicitHeight
    }
    component NoteGlyph: AnnotationGlyph {
        kind: "note"
        implicitWidth: 14 * app.scale
        implicitHeight: 16 * app.scale
        width: implicitWidth; height: implicitHeight
        ink: desktop.accent
    }
    component RangeMarker: ToolButton {
        id: marker
        required property string kind
        required property int number
        required property int rowIndex
        required property var ranges
        required property real headingHeight
        required property real bodyHeight
        readonly property bool starts: ranges.some(function(r) { return r[kind + "Start"] })
        readonly property bool ends: ranges.some(function(r) { return r[kind + "End"] })
        readonly property bool continuesBefore: ranges.some(function(r) { return r.continuesBefore || r[kind + "ContinuesBefore"] })
        readonly property bool continuesAfter: ranges.some(function(r) { return r.continuesAfter || r[kind + "ContinuesAfter"] })
        readonly property var entries: ranges.filter(function(r) { return r[kind + "Start"] || (rowIndex === 0 && (r.continuesBefore || r[kind + "ContinuesBefore"])) })
        readonly property var choices: entries.length ? entries : ranges
        readonly property string hint: ranges.map(function(r) {
            return r.reference + (marker.kind === "tag" ? " · " + app.tagsPlainText(r.tags) : "")
        }).join("\n")
        width: 16 * app.scale
        readonly property int lane: (app.annotationLanes[rowIndex] || {})[kind] || 0
        x: (36 - lane * 18) * app.scale
        padding: 0
        visible: ranges.length > 0 && !bible.focusMode
        Accessible.name: hint
        onClicked: {
            if (choices.length === 1) {
                if (kind === "tag") app.openPassageTags(choices[0])
                else if (kind === "bookmark") app.openBookmark(choices[0])
                else app.openNoteScope(choices[0])
            } else {
                verseTagMenu.annotationKind = kind
                verseTagMenu.entries = choices
                const p = mapToItem(app.contentItem, width, headingHeight)
                verseTagMenu.popup(p.x, p.y)
            }
        }
        contentItem: Item {
            Rectangle {
                objectName: marker.kind + "Line" + marker.number
                x: 5 * app.scale
                y: marker.continuesBefore ? 0 : marker.headingHeight + 8 * app.scale
                width: app.scale
                height: Math.max(0, (marker.continuesAfter ? parent.height : marker.headingHeight + marker.bodyHeight) - y)
                color: desktop.accent; opacity: 0.65
            }
            Rectangle {
                objectName: marker.kind + "EndCap" + marker.number
                x: 5 * app.scale; y: marker.headingHeight + marker.bodyHeight - app.scale
                width: 7 * app.scale; height: app.scale
                visible: marker.ends
                color: desktop.accent
            }
            AnnotationGlyph {
                objectName: marker.kind + "Glyph" + marker.number
                kind: marker.kind; ink: desktop.accent
                filled: true
                y: marker.headingHeight + 5 * app.scale
                width: 14 * app.scale; height: 16 * app.scale
                visible: marker.starts || (marker.rowIndex === 0 && marker.continuesBefore)
            }
        }
        background: Rectangle {
            color: marker.hovered || marker.activeFocus ? app.tint(desktop.accent, 0.12) : "transparent"
            radius: Math.min(desktop.cornerRadius, width / 2, height / 2)
        }
        AppToolTip { theme: desktop; visible: marker.visible && (marker.hovered || marker.activeFocus); text: marker.hint }
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
            radius: Math.min(desktop.cornerRadius, width / 2, height / 2)
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
            radius: Math.min(desktop.cornerRadius, width / 2, height / 2)
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

                        SectionLabel { objectName: "editionLabel"; visible: !bible.focusMode; text: bible.edition.toUpperCase(); width: parent.width; wrapMode: Text.Wrap; leftPadding: app.verseGutter; bottomPadding: 12 }
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
                            model: app.continuousReader ? [] : bible.verses
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
                                RangeMarker {
                                    objectName: "bookmarkMarker" + verseRow.modelData.number
                                    kind: "bookmark"; number: verseRow.modelData.number; rowIndex: verseRow.index
                                    ranges: Array.from(verseRow.modelData.bookmarkRanges || [])
                                    headingHeight: verseHeading.height; bodyHeight: verseBody.height
                                    height: verseRow.height
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
                                RangeMarker {
                                    id: verseTagMarker
                                    objectName: "verseTagMarker" + verseRow.modelData.number
                                    kind: "tag"; number: verseRow.modelData.number; rowIndex: verseRow.index
                                    ranges: Array.from(verseRow.modelData.tagRanges || [])
                                    headingHeight: verseHeading.height; bodyHeight: verseBody.height
                                    height: verseRow.height
                                }
                                RangeMarker {
                                    objectName: "verseNoteMarker" + verseRow.modelData.number
                                    kind: "note"; number: verseRow.modelData.number; rowIndex: verseRow.index
                                    ranges: Array.from(verseRow.modelData.noteRanges || [])
                                    headingHeight: verseHeading.height; bodyHeight: verseBody.height
                                    height: verseRow.height
                                }
                                Rectangle {
                                    objectName: "contextSegment" + verseRow.modelData.number
                                    x: verseRow.gutter - 9
                                    y: verseHeading.height
                                    width: verseRow.width - x
                                    height: verseBody.height + 3
                                    visible: !verseRow.selected && !verseRow.modelData.highlighted && app.rowInCursorContext(verseRow.modelData)
                                    color: app.tint(desktop.accent, 0.06)
                                    radius: 0
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
                                    x: 54 * app.scale
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
                                    AppToolTip {
                                        theme: desktop
                                        visible: numberMouse.containsMouse && !bible.focusMode
                                        delay: 700
                                        text: "Select verse " + verseRow.modelData.number + " · Shift Click for a range" + (verseRow.modelData.hasNote ? "\nHas a note" : "") + (verseRow.modelData.bookmarked ? "\nBookmarked" : "") + (verseRow.modelData.hasTags ? "\nTagged" : "")
                                    }
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
                                        if (event.key === Qt.Key_A && event.modifiers === Qt.ControlModifier && app.readerKeys) event.accepted = true
                                        if (event.key === Qt.Key_C && event.modifiers === Qt.ControlModifier && app.freeSelectedText.length) event.accepted = true
                                    }
                                    Keys.onPressed: function(event) {
                                        if (event.key === Qt.Key_A && event.modifiers === Qt.ControlModifier && app.readerKeys) { app.runCommand("annotations"); event.accepted = true }
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
                            active: app.continuousReader
                            visible: active
                            x: app.verseGutter
                            width: parent.width - x - 9
                            sourceComponent: BlockReader {
                                mode: bible.readingMode
                                objectName: "blockReader"
                                controller: app
                                backend: bible
                                theme: desktop
                                viewport: reading
                            }
                        }
                        Label {
                            visible: bible.verses.length === 0 && !bible.busy
                            text: "This chapter has no text. Open another passage or add this version again."
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
                Label { objectName: "setupTitle"; text: desktop.appName; font.pixelSize: 30 * app.scale; font.weight: Font.Medium; font.letterSpacing: -0.8; color: desktop.foreground; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap; Layout.fillWidth: true }
                Label {
                    objectName: "setupDescription"
                    text: "Add a version to begin. Install from the catalog or import a USFM ZIP.\nRead offline, bookmark and highlight passages, and keep your notes alongside the text."
                    color: desktop.muted
                    font.pixelSize: 15 * app.scale
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    lineHeight: 1.4
                    Layout.fillWidth: true
                }
                QuietButton { objectName: "setupImport"; text: "Add version"; emphasized: true; Layout.alignment: Qt.AlignHCenter; Layout.topMargin: 6; onClicked: app.runCommand("import") }
                Label { objectName: "setupPrivacy"; text: "All your Bible versions, highlights, bookmarks, and notes stay on this device."; color: desktop.muted; font.pixelSize: 12 * app.scale; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap; Layout.fillWidth: true }
                QuietButton { objectName: "setupAbout"; text: "About & Attribution"; compact: true; Layout.alignment: Qt.AlignHCenter; onClicked: aboutDialog.open() }
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
        background: Rectangle { radius: Math.min(desktop.cornerRadius, width / 2, height / 2); color: desktop.background; border.color: desktop.border }
        Overlay.modal: Rectangle {
            color: app.tint(desktop.background, 0.65)
        }
        onOpened: {
            if (app.modalOpen) return
            if (focusEditor && app.hasSelection) noteEditor.forceActiveFocus()
            else notesPanel.forceActiveFocus()
        }
        onClosed: { app.editingPanelClosed(); app.restorePanelFocus() }
        contentItem: Rectangle {
                id: notesPanel
                objectName: "notesPanel"
                color: desktop.background
                radius: Math.min(desktop.cornerRadius, width / 2, height / 2)
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
                    height: Math.max(notesPanel.height - 44, (app.noteConflict ? 700 : 440) * app.scale)
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
                    ColumnLayout {
                        visible: app.noteConflict
                        Layout.fillWidth: true
                        Label {
                            objectName: "noteConflictMessage"
                            text: "This note changed in another window. Your unsaved draft is preserved. Review the saved text before replacing it."
                            color: desktop.accent; font.pixelSize: 12 * app.scale
                            Layout.fillWidth: true; wrapMode: Text.Wrap
                            Accessible.role: Accessible.AlertMessage
                        }
                        Label {
                            objectName: "noteConflictSaved"
                            text: app.noteScopeMatches ? "Saved text: " + (bible.noteText || "(empty note)")
                                : "Draft belongs to " + app.loadedVersionId + " · " + app.loadedNoteReference
                            textFormat: Text.PlainText
                            color: desktop.muted; font.pixelSize: 12 * app.scale
                            Layout.fillWidth: true; wrapMode: Text.Wrap
                            maximumLineCount: 4; elide: Text.ElideRight
                        }
                        RowLayout {
                            QuietButton {
                                objectName: "noteRetry"
                                text: "Save my draft"; tip: "Replace the saved note with your draft"
                                enabled: app.noteScopeMatches && app.hasSelection && !bible.busy
                                onClicked: app.retryNote()
                                Keys.onReturnPressed: clicked(); Keys.onEnterPressed: clicked()
                            }
                            QuietButton {
                                objectName: "noteUseSaved"
                                text: "Use saved text"
                                onClicked: app.loadSavedNote()
                                Keys.onReturnPressed: clicked(); Keys.onEnterPressed: clicked()
                            }
                        }
                    }
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
                                    if (!app.syncingNote) app.flushNote()
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
                    Label { text: app.noteConflict ? "Not saved · resolve the other window's change above" : app.hasSelection ? (noteEditor.text === bible.noteText ? "Saved on this device · changes save as you type" : "Not saved · your draft is still in the editor") : "A page icon beside a verse marks a note."; color: desktop.muted; font.pixelSize: 11 * app.scale; wrapMode: Text.Wrap; Layout.fillWidth: true }
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

    Row {
        id: leftActions
        objectName: "leftActions"
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: 16 * app.controlScale
        spacing: 4 * app.controlScale
        visible: !bible.focusMode
        FloatingButton {
            objectName: "helpControl"
            glyph: "help"
            tip: "Keybindings · ? · F1"
            enabled: !bible.busy && !app.modalOpen
            onClicked: helpDialog.open()
        }
        FloatingButton {
            id: modeControl
            objectName: "modeControl"
            glyph: app.verseMode ? "visual" : "normal"
            selected: app.verseMode
            visible: bible.ready
            enabled: !bible.busy && !app.modalOpen
            tip: (app.verseMode ? "Visual mode · switch to normal" : "Normal mode · switch to visual") + " · v"
            onClicked: { app.setVisualMode(!app.verseMode); app.readerFocus() }
        }
    }
    Row {
        id: locationControls
        objectName: "locationControls"
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 16 * app.controlScale
        x: app.width / 2 - width / 2
        readonly property real availableWidth: Math.max(0, app.width - 2 * (Math.max(leftActions.width, floatingActions.width) + 24 * app.controlScale))
        width: Math.min(passageControl.implicitWidth + (versionControl.visible ? versionControl.width : 0), availableWidth)
        visible: !bible.focusMode
        FloatingButton {
            id: passageControl
            objectName: "passageControl"
            width: Math.max(0, locationControls.width - (versionControl.visible ? versionControl.width : 0))
            text: bible.ready ? (bible.selectionReference || bible.passage) : "Open a passage"
            tip: (app.hasSelection ? bible.selectionReference : bible.passage) + " · Open passage · Ctrl k"
            enabled: !bible.busy && !app.modalOpen
            onClicked: app.openPalette(false)
        }
        FloatingButton {
            id: versionControl
            objectName: "versionControl"
            width: Math.min(implicitWidth, locationControls.availableWidth * 0.4)
            text: bible.versionAbbreviation
            tip: bible.edition + " · Switch version · Shift V"
            visible: bible.ready
            enabled: !bible.busy && !app.modalOpen
            onClicked: app.openVersions(false)
        }
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
            objectName: "relatedControl"
            glyph: "related"
            visible: bible.ready
            enabled: app.hasSelection && !bible.busy && !app.modalOpen
            tip: "Related passages · Shift L"
            onClicked: app.runCommand("related")
        }
        FloatingButton {
            objectName: "savedControl"
            visible: bible.ready
            glyph: "saved"
            tip: "Annotations · Ctrl a"
            enabled: bible.ready && !bible.busy && !app.modalOpen
            onClicked: app.runCommand("annotations")
        }
        FloatingButton {
            objectName: "notesControl"
            visible: bible.ready
            glyph: "notes"
            selected: app.notesOpen
            tip: "Notes · Ctrl n"
            enabled: bible.ready && !bible.busy && !app.modalOpen
            onClicked: app.toggleNotes()
        }
    }

    ModalCard {
        id: versionDialog
        objectName: "versionDialog"
        property bool removing: false
        property int step: 0
        property var chosen: null
        property bool removeAnnotations: false
        property bool addPending: false
        readonly property real uiScale: Math.min(app.scale, Math.max(1, (app.height - 48) / 240))
        height: Math.min(540 * app.scale, app.height - 48)
        onOpened: versionSearch.forceActiveFocus()
        onClosed: {
            const add = addPending
            addPending = false
            chosen = null
            if (add) Qt.callLater(function() { app.runCommand("import") })
            else app.restorePanelFocus()
        }
        contentItem: ColumnLayout {
            spacing: 12 * versionDialog.uiScale
            Keys.onPressed: function(event) { app.versionKey(event) }
            Label {
                objectName: "versionTitle"
                text: !versionDialog.removing ? "Switch version" : versionDialog.step === 0 ? "Remove version"
                    : versionDialog.step === 1 ? "Remove annotations too?" : "Confirm removal"
                color: desktop.foreground; font.pixelSize: 20 * versionDialog.uiScale
                Layout.fillWidth: true; wrapMode: Text.Wrap
            }
            RowLayout {
                visible: versionDialog.step === 0
                Layout.fillWidth: true
                spacing: 8 * versionDialog.uiScale
                TextField {
                    id: versionSearch
                    objectName: "versionSearch"
                    enabled: !bible.busy
                    Layout.fillWidth: true
                    implicitHeight: Math.round(42 * versionDialog.uiScale)
                    placeholderText: "Search versions…"
                    font.pixelSize: 15 * versionDialog.uiScale
                    selectByMouse: true
                    padding: 10 * versionDialog.uiScale
                    background: Rectangle { radius: Math.min(desktop.cornerRadius, width / 2, height / 2); color: desktop.surface; border.color: versionSearch.activeFocus ? desktop.accent : desktop.border }
                    onTextChanged: { versionResults.currentIndex = 0; versionResults.positionViewAtBeginning() }
                    Keys.onPressed: function(event) { app.versionKey(event) }
                    KeyNavigation.tab: versionAdd.visible ? versionAdd : versionResults
                    Accessible.name: "Search installed versions"
                }
                QuietButton {
                    id: versionAdd
                    objectName: "versionAdd"
                    text: "+"
                    tip: "Add version"
                    Accessible.name: tip
                    visible: !versionDialog.removing
                    enabled: !bible.busy
                    focusPolicy: Qt.StrongFocus
                    font.pixelSize: 24 * versionDialog.uiScale
                    Layout.preferredWidth: versionSearch.implicitHeight
                    Layout.preferredHeight: versionSearch.implicitHeight
                    onClicked: app.addVersionFromPicker()
                    Keys.onPressed: function(event) { app.versionKey(event) }
                    KeyNavigation.tab: versionResults
                }
            }
            ListView {
                id: versionResults
                objectName: "versionResults"
                visible: versionDialog.step < 2
                enabled: !bible.busy
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 3 * versionDialog.uiScale
                model: app.versionEntries()
                onModelChanged: currentIndex = count ? Math.max(0, Math.min(currentIndex, count - 1)) : -1
                Keys.onPressed: function(event) { app.versionKey(event) }
                ScrollBar.vertical: OverlayScrollbar { objectName: "versionScrollbar" }
                delegate: ItemDelegate {
                    id: versionEntry
                    required property var modelData
                    required property int index
                    objectName: "versionEntry" + index
                    width: versionResults.width
                    height: versionEntryContent.implicitHeight + 20 * versionDialog.uiScale
                    highlighted: ListView.isCurrentItem
                    onClicked: { versionResults.currentIndex = index; versionResults.forceActiveFocus(); app.acceptVersion() }
                    Keys.onPressed: function(event) { app.versionKey(event) }
                    background: Rectangle { radius: Math.min(desktop.cornerRadius, width / 2, height / 2); color: versionEntry.highlighted ? app.tint(desktop.accent, 0.11) : versionEntry.hovered ? desktop.surface : "transparent" }
                    contentItem: ColumnLayout {
                        id: versionEntryContent
                        spacing: 4 * versionDialog.uiScale
                        Label {
                            text: (versionEntry.modelData.abbreviation ? versionEntry.modelData.abbreviation + " · " : "") + versionEntry.modelData.name
                            color: desktop.foreground; font.pixelSize: 14 * versionDialog.uiScale
                            Layout.fillWidth: true; wrapMode: Text.Wrap
                        }
                        Label {
                            text: versionEntry.modelData.hint || (versionEntry.modelData.active ? "Current version" : "Installed locally")
                            color: desktop.muted; font.pixelSize: 11 * versionDialog.uiScale
                            Layout.fillWidth: true; wrapMode: Text.Wrap
                        }
                    }
                }
                Label {
                    objectName: "versionEmpty"
                    anchors.centerIn: parent
                    width: parent.width
                    text: bible.busy ? "Please wait…" : bible.versions.length ? "No matching versions" : "No versions installed. Add version to begin."
                    visible: versionResults.count === 0 || bible.busy
                    color: desktop.muted; font.pixelSize: 14 * versionDialog.uiScale
                    horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap
                }
            }
            OverlayScrollView {
                id: versionSummaryScroll
                objectName: "versionSummaryScroll"
                visible: versionDialog.step === 2
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: availableWidth
                clip: true
                Label {
                    objectName: "versionSummary"
                    width: versionSummaryScroll.availableWidth
                    text: versionDialog.chosen ? "Remove " + versionDialog.chosen.name + " (" + versionDialog.chosen.abbreviation + ") from this library?\n\n"
                        + (versionDialog.removeAnnotations ? "Delete its notes, bookmarks, and highlights. This cannot be undone."
                            : "Keep its notes, bookmarks, and highlights. They will reconnect when the same version is reimported.")
                        + "\n\n" + (bible.versions.length === 1 ? "The library will be empty. You can add a version again at any time." : "Other installed versions remain available.")
                        + " Your settings and source USFM files are kept." : ""
                    textFormat: Text.PlainText
                    color: desktop.foreground; font.pixelSize: 14 * versionDialog.uiScale; wrapMode: Text.Wrap
                }
            }
            Label {
                objectName: "versionError"
                text: bible.error
                visible: text.length > 0
                color: desktop.accent; font.pixelSize: 12 * versionDialog.uiScale
                Layout.fillWidth: true; wrapMode: Text.Wrap
            }
            FineRule { Layout.fillWidth: true }
            Label {
                text: versionDialog.step === 2 ? "← → choose    Enter select    Esc cancel"
                    : !versionDialog.removing ? "↑ ↓ choose    Tab to +    Enter select    Esc cancel" : "↑ ↓ choose    Home / End    Enter select    Esc cancel"
                color: desktop.muted; font.pixelSize: 11 * versionDialog.uiScale
                Layout.fillWidth: true; wrapMode: Text.Wrap
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 4 * versionDialog.uiScale
                QuietButton {
                    id: versionBack
                    objectName: "versionBack"
                    font.pixelSize: 13 * versionDialog.uiScale
                    implicitHeight: 38 * versionDialog.uiScale
                    text: "Back"
                    visible: versionDialog.step > 0
                    enabled: !bible.busy
                    onClicked: app.backVersion()
                    Keys.onPressed: function(event) { app.versionKey(event) }
                }
                Item { Layout.fillWidth: true }
                QuietButton {
                    id: versionCancel
                    objectName: "versionCancel"
                    font.pixelSize: 13 * versionDialog.uiScale
                    implicitHeight: 38 * versionDialog.uiScale
                    text: "Cancel"
                    onClicked: versionDialog.close()
                    Keys.onPressed: function(event) { app.versionKey(event) }
                }
                QuietButton {
                    id: versionNext
                    objectName: "versionNext"
                    font.pixelSize: 13 * versionDialog.uiScale
                    implicitHeight: 38 * versionDialog.uiScale
                    text: versionDialog.step === 2 ? "Confirm" : versionDialog.removing ? "Next" : "Switch"
                    emphasized: true
                    enabled: !bible.busy && (versionDialog.step === 2 || versionResults.count > 0)
                    onClicked: { versionNext.forceActiveFocus(); app.acceptVersion() }
                    Keys.onPressed: function(event) { app.versionKey(event) }
                }
            }
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
                    background: Rectangle { color: desktop.surface; radius: Math.min(desktop.cornerRadius, width / 2, height / 2); border.color: referenceInput.activeFocus ? app.tint(desktop.accent, 0.65) : desktop.border }
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
                    background: Rectangle { radius: Math.min(desktop.cornerRadius, width / 2, height / 2); color: paletteEntry.highlighted ? app.tint(desktop.accent, 0.11) : paletteEntry.hovered ? desktop.surface : "transparent" }
                }
                Label {
                    anchors.centerIn: parent
                    width: parent.width - 32
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: referenceInput.text.startsWith(">") ? "No matching commands" : "Add version to browse passages."
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
        width: Math.min(520 * app.scale, app.width - 48)
        height: app.height
        edge: Qt.RightEdge
        modal: true
        focus: true
        padding: 16 * app.controlScale
        topPadding: padding
        bottomPadding: padding
        leftPadding: padding
        rightPadding: padding
        closePolicy: deleteConfirmation.visible || tagsUi.visibleModal ? Popup.NoAutoClose : Popup.CloseOnEscape | Popup.CloseOnPressOutside
        readonly property bool collectionKeys: opened && app.collectionTab !== 3 && !deleteConfirmation.visible && !tagsUi.visibleModal && !app.textInputActive()
        background: Rectangle { radius: Math.min(desktop.cornerRadius, width / 2, height / 2); color: desktop.background; border.color: desktop.border }
        Overlay.modal: Rectangle { color: app.tint(desktop.background, 0.65) }
        onOpened: app.moveSaved(0)
        onClosed: { tagsUi.embeddedClosed(); app.restorePanelFocus() }
        contentItem: ColumnLayout {
            objectName: "savedContent"
            Shortcut { sequence: "Ctrl+A"; enabled: savedDrawer.opened && !deleteConfirmation.visible && !tagsUi.visibleModal && !bible.busy && !app.textInputActive(); onActivated: savedDrawer.close() }
            Shortcut { sequences: ["J", "Down"]; enabled: savedDrawer.collectionKeys; onActivated: app.moveSaved(1) }
            Shortcut { sequences: ["K", "Up"]; enabled: savedDrawer.collectionKeys; onActivated: app.moveSaved(-1) }
            Shortcut { sequences: ["H", "Left"]; enabled: savedDrawer.collectionKeys; onActivated: app.filterSaved(-1) }
            Shortcut { sequences: ["L", "Right"]; enabled: savedDrawer.collectionKeys; onActivated: app.filterSaved(1) }
            Shortcut { sequences: ["Return", "Enter"]; enabled: savedDrawer.opened && !deleteConfirmation.visible && app.activeFocusItem === collectionList; onActivated: app.openSaved(collectionList.model[collectionList.currentIndex]) }
            Shortcut { sequences: ["Delete", "Backspace"]; enabled: savedDrawer.collectionKeys && !bible.busy; onActivated: app.requestDeleteSaved() }
            spacing: (savedDrawer.height < 350 * app.scale ? 6 : 12) * app.scale
            RowLayout {
                Layout.fillWidth: true
                Label { objectName: "savedTitle"; text: "Annotations"; font.pixelSize: 24 * app.controlScale; font.weight: Font.Medium; color: desktop.foreground; Layout.fillWidth: true; wrapMode: Text.Wrap }
            }
            RowLayout {
                id: savedTabs
                objectName: "savedTabs"
                Layout.fillWidth: true
                spacing: 4 * app.controlScale
                // One row takes priority over unbounded text scaling. Allocate
                // natural label widths, with the same bounded font for all tabs.
                readonly property real labelSize: Math.max(12, Math.min(14 * app.controlScale, width / 29))
                Repeater {
                    model: ["Bookmarks", "Highlights", "Notes", "Tags"]
                    delegate: QuietButton {
                        required property string modelData
                        required property int index
                        objectName: "savedTab" + index
                        text: modelData
                        emphasized: app.collectionTab === index
                        compact: true
                        implicitHeight: 32 * app.controlScale
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.preferredWidth: implicitWidth
                        padding: 6 * app.controlScale
                        font.pixelSize: savedTabs.labelSize
                        onClicked: app.filterSaved(index - app.collectionTab)
                        Keys.onReturnPressed: clicked()
                        Keys.onEnterPressed: clicked()
                    }
                }
            }
            FineRule { Layout.fillWidth: true }
            Item {
                id: annotationsTagsHost
                objectName: "annotationsTagsHost"
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: app.collectionTab === 3
            }
            ListView {
                id: collectionList
                visible: app.collectionTab !== 3
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
                            visible: savedEntry.modelData.bookmark || savedEntry.modelData.note.length > 0 || (savedEntry.modelData.tags || []).length > 0
                            BookmarkGlyph { objectName: "savedBookmark" + savedEntry.modelData.reference; visible: savedEntry.modelData.bookmark; Layout.alignment: Qt.AlignHCenter }
                            NoteGlyph { objectName: "savedNote" + savedEntry.modelData.reference; visible: savedEntry.modelData.note.length > 0; Layout.alignment: Qt.AlignHCenter }
                            TagGlyph { objectName: "savedTag" + savedEntry.modelData.reference; visible: (savedEntry.modelData.tags || []).length > 0; Layout.alignment: Qt.AlignHCenter }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 7 * app.scale
                            Label { text: savedEntry.modelData.reference; textFormat: Text.PlainText; color: desktop.foreground; font.pixelSize: 16 * app.scale; font.weight: Font.Medium; Layout.fillWidth: true; wrapMode: Text.Wrap }
                            Label { text: savedEntry.modelData.note.replace(/\s+/g, " "); textFormat: Text.PlainText; visible: text.length > 0; color: desktop.muted; font.pixelSize: 12 * app.scale; maximumLineCount: 3; elide: Text.ElideRight; wrapMode: Text.Wrap; Layout.fillWidth: true }
                            Label { objectName: "savedTags" + savedEntry.modelData.reference; text: app.tagsPlainText(savedEntry.modelData.tags); textFormat: Text.PlainText; visible: text.length > 0; color: desktop.accent; font.pixelSize: 12 * app.scale; wrapMode: Text.Wrap; Layout.fillWidth: true }
                        }
                    }
                    background: Rectangle {
                        color: savedEntry.highlighted ? app.tint(desktop.accent, 0.11) : savedEntry.hovered ? desktop.surface : "transparent"
                        radius: Math.min(desktop.cornerRadius, width / 2, height / 2)
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
                    text: ["No bookmarks yet.", "No highlights yet.", "No notes yet."][app.collectionTab] || ""
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
        property var scope: null
        readonly property string reference: scope ? scope.reference : ""
        readonly property string versionId: scope ? scope.versionId : ""
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
            scope = null
            row = -1
            deleteFocusTimer.restart()
        }
        contentItem: ColumnLayout {
            spacing: 18 * app.scale
            Label { text: "Delete annotations?"; color: desktop.foreground; font.pixelSize: 22 * app.scale; Layout.fillWidth: true; wrapMode: Text.Wrap }
            Label { objectName: "deleteReference"; text: deleteConfirmation.reference; textFormat: Text.PlainText; color: desktop.accent; font.pixelSize: 16 * app.scale; Layout.fillWidth: true; wrapMode: Text.Wrap }
            Label { objectName: "deleteExplanation"; text: "This deletes all marks (bookmark and highlight), the note, and tags for this exact reference."; color: desktop.foreground; font.pixelSize: 14 * app.scale; Layout.fillWidth: true; wrapMode: Text.Wrap }
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
                background: Rectangle { radius: Math.min(desktop.cornerRadius, width / 2, height / 2); color: desktop.surface; border.color: keybindingSearch.activeFocus ? desktop.accent : desktop.border }
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
                    AppToolTip { theme: desktop; visible: keybindingRow.hovered; text: app.scopeTitle(keybindingRow.modelData); delay: 650 }
                    highlighted: ListView.isCurrentItem
                    onClicked: app.activateKeybinding(modelData)
                    Keys.onReturnPressed: clicked()
                    Keys.onEnterPressed: clicked()
                    background: Rectangle { radius: Math.min(desktop.cornerRadius, width / 2, height / 2); color: keybindingRow.highlighted ? app.tint(desktop.accent, 0.11) : keybindingRow.hovered ? desktop.surface : "transparent" }
                    contentItem: RowLayout {
                    id: keybindingContent
                    spacing: 12 * app.scale
                    Label {
                        text: keybindingRow.modelData.title; color: keybindingRow.enabled ? desktop.foreground : desktop.muted
                        font.pixelSize: 13 * app.scale; Layout.fillWidth: true
                        Layout.preferredWidth: keybindingRow.width * 0.48; wrapMode: Text.Wrap
                    }
                    Label {
                        objectName: "keybindingHint" + keybindingRow.modelData.id
                        text: keybindingRow.modelData.keys; color: desktop.muted
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
                visible: !settingsDialog.lineOnly
                Layout.fillWidth: true
                Label { text: "Reading mode"; color: desktop.foreground; font.pixelSize: 13 * app.scale; Layout.fillWidth: true }
                Label { text: "b"; color: desktop.muted; font.pixelSize: 12 * app.scale }
            }
            RowLayout {
                visible: !settingsDialog.lineOnly
                Layout.fillWidth: true
                spacing: 4 * app.scale
                Repeater {
                    model: ["paragraph", "verse", "block"]
                    delegate: Button {
                        required property string modelData
                        objectName: "readingMode:" + modelData
                        text: app.readingModeName(modelData)
                        checkable: true
                        autoExclusive: true
                        checked: bible.readingMode === modelData
                        Layout.fillWidth: true
                        onClicked: bible.setReadingMode(modelData)
                        Accessible.name: text + " reading mode"
                    }
                }
            }
            RowLayout {
                visible: !settingsDialog.lineOnly
                Layout.fillWidth: true
                Label { text: "Font size"; color: desktop.foreground; font.pixelSize: 13 * app.scale }
                Item { Layout.fillWidth: true }
                Label { objectName: "fontSizeValue"; text: bible.fontSize; color: desktop.accent; font.pixelSize: 14 * app.scale }
            }
            SquareSlider {
                objectName: "fontSizeSlider"
                visible: !settingsDialog.lineOnly
                Layout.fillWidth: true
                from: 8; to: 40; stepSize: 1
                snapMode: Slider.SnapAlways
                value: bible.fontSize
                onMoved: bible.setFontSize(Math.round(value))
                Accessible.name: "Font size"
            }
            Label { visible: !settingsDialog.lineOnly; text: "Ctrl − decrease    Ctrl + increase    Ctrl 0 reset"; color: desktop.muted; font.pixelSize: 11 * app.scale; wrapMode: Text.Wrap; Layout.fillWidth: true }
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
                Label { text: desktop.appName; color: desktop.foreground; font.pixelSize: 30 * app.scale; font.weight: Font.Medium; font.letterSpacing: -1; Layout.fillWidth: true }
            }
            Label { text: "About & Attribution"; color: desktop.foreground; font.pixelSize: 18 * app.scale; Layout.fillWidth: true; wrapMode: Text.Wrap }
            Label { text: "An offline Bible reader with local bookmarks, highlights, and Markdown notes."; color: desktop.foreground; wrapMode: Text.Wrap; Layout.fillWidth: true; font.pixelSize: 14 * app.scale; lineHeight: 1.4 }
            FineRule { Layout.fillWidth: true }
            Label { objectName: "aboutEdition"; text: bible.ready ? bible.edition : "No version installed"; textFormat: Text.PlainText; color: desktop.foreground; wrapMode: Text.Wrap; Layout.fillWidth: true; font.pixelSize: 14 * app.scale }
            Label { objectName: "aboutCopyright"; text: bible.versionCopyright; textFormat: Text.PlainText; visible: text.length > 0; color: desktop.muted; wrapMode: Text.Wrap; Layout.fillWidth: true; font.pixelSize: 12 * app.scale; lineHeight: 1.4 }
            Label { objectName: "aboutSources"; text: "Install versions directly from eBible.org or import your own local USFM ZIP. Bible text is stored in this app's local library. No connection is needed to read, bookmark, highlight, or write notes after import."; color: desktop.muted; wrapMode: Text.Wrap; Layout.fillWidth: true; font.pixelSize: 12 * app.scale }
            QuietButton {
                readonly property var entry: Array.from(catalog.entries).find(function(version) { return version.id === bible.versionId })
                text: "Version details & permissions ↗"; visible: !!entry; compact: true
                onClicked: desktop.openExternal(entry.detailsUrl)
            }
            Label { text: desktop.appName + " " + desktop.version + " · Application code licensed under MIT\nNo account required"; color: desktop.muted; font.pixelSize: 11 * app.scale; wrapMode: Text.Wrap; Layout.fillWidth: true }
            QuietButton { text: "Open local data folder ↗"; compact: true; onClicked: desktop.openExternal("file://" + bible.dataDirectory()) }
        }
        }
    }

    ModalCard {
        id: importGuide
        objectName: "importGuide"
        property bool choosePending: false
        readonly property real uiScale: Math.min(app.scale, Math.max(1, (app.height - 48) / 360))
        height: Math.min(650 * app.scale, app.height - 48)
        onOpened: { catalogSearch.text = ""; catalogSearch.forceActiveFocus() }
        onClosed: {
            const choose = choosePending
            choosePending = false
            if (choose) Qt.callLater(function() {
                if (!app.closingWindow && !bible.busy && !catalog.busy && app.flushNote()) importDialog.open()
                else app.restorePanelFocus()
            })
            else app.restorePanelFocus()
        }
        contentItem: ColumnLayout {
            spacing: 12 * importGuide.uiScale
            Label { text: "Add version"; color: desktop.foreground; font.pixelSize: 24 * importGuide.uiScale; Layout.fillWidth: true; wrapMode: Text.Wrap }
            TextField {
                id: catalogSearch
                objectName: "catalogSearch"
                Layout.fillWidth: true
                implicitHeight: 42 * importGuide.uiScale
                font.pixelSize: 15 * importGuide.uiScale
                selectByMouse: true
                padding: 10 * importGuide.uiScale
                background: Rectangle { radius: Math.min(desktop.cornerRadius, width / 2, height / 2); color: desktop.surface; border.color: catalogSearch.activeFocus ? desktop.accent : desktop.border }
                placeholderText: "Search versions"
                Accessible.name: "Search versions"
            }
            OverlayScrollView {
                id: importGuideScroll
                objectName: "importGuideScroll"
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: availableWidth
                clip: true
                ColumnLayout {
                    width: importGuideScroll.availableWidth
                    spacing: 16 * app.scale
                    Label { objectName: "importSupported"; text: "Install a USFM ZIP directly from eBible.org, or import a local file. The catalog is available offline; installing requires a connection."; color: desktop.muted; font.pixelSize: 13 * app.scale; wrapMode: Text.Wrap; Layout.fillWidth: true }
                    Repeater {
                        id: catalogResults
                        objectName: "catalogResults"
                        model: Array.from(catalog.entries).filter(function(entry) {
                            const query = catalogSearch.text.trim().toLowerCase()
                            return !query || (entry.name + " " + entry.abbreviation).toLowerCase().includes(query)
                        })
                        delegate: ColumnLayout {
                            id: catalogRow
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: 6 * app.scale
                            Label { text: catalogRow.modelData.abbreviation + " · " + catalogRow.modelData.name; textFormat: Text.PlainText; color: desktop.foreground; font.pixelSize: 14 * app.scale; wrapMode: Text.Wrap; Layout.fillWidth: true }
                            RowLayout {
                                Layout.fillWidth: true
                                Label { objectName: "catalogInstalled:" + catalogRow.modelData.id; visible: app.installedVersion(catalogRow.modelData.id); text: "Installed"; color: desktop.accent; font.pixelSize: 12 * app.scale }
                                Item { Layout.fillWidth: true }
                                QuietButton { objectName: "catalogDetails:" + catalogRow.modelData.id; text: "Details & Attribution"; font.pixelSize: 12 * importGuide.uiScale; compact: true; onClicked: { catalogDetails.entry = catalogRow.modelData; catalogDetails.open() } }
                                QuietButton {
                                    objectName: "catalogInstall:" + catalogRow.modelData.id
                                    text: "Install"; compact: true; emphasized: true
                                    font.pixelSize: 12 * importGuide.uiScale
                                    enabled: !bible.busy && !catalog.busy
                                    onClicked: app.installVersion(catalogRow.modelData.id)
                                }
                            }
                            FineRule { Layout.fillWidth: true }
                        }
                    }
                    Label { visible: catalogResults.count === 0; text: "No matching versions"; color: desktop.muted; Layout.fillWidth: true }
                    Label {
                        objectName: "importReplacement"
                        visible: bible.ready
                        text: "Importing the same edition replaces its Bible data and preserves your notes, bookmarks, highlights, and settings. Adding another supported version keeps the installed editions. Annotations belong to their own version."
                        color: desktop.accent; font.pixelSize: 14 * app.scale; wrapMode: Text.Wrap; Layout.fillWidth: true
                    }
                    Label { objectName: "importDataPath"; text: bible.dataDirectory(); textFormat: Text.PlainText; color: desktop.muted; font.pixelSize: 12 * app.scale; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
                    Label { objectName: "importOffline"; text: "After a successful import, you can move or delete the source USFM ZIP. No connection is needed to read or use your annotations."; color: desktop.muted; font.pixelSize: 13 * app.scale; wrapMode: Text.Wrap; Layout.fillWidth: true }
                }
            }
            ProgressBar { objectName: "catalogProgress"; visible: catalog.busy; Layout.fillWidth: true; from: 0; to: 1; value: Math.max(0, Math.min(1, catalog.progress)); indeterminate: catalog.progress <= 0 }
            Label { objectName: "catalogStatus"; visible: catalog.busy; text: "Downloading USFM ZIP… " + (catalog.progress > 0 ? Math.round(catalog.progress * 100) + "%" : ""); color: desktop.foreground; Layout.fillWidth: true; wrapMode: Text.Wrap }
            Label { objectName: "catalogError"; text: catalog.error || app.downloadMessage || bible.error; visible: text.length > 0; textFormat: Text.PlainText; color: desktop.accent; Layout.fillWidth: true; wrapMode: Text.Wrap; Accessible.role: Accessible.AlertMessage }
            RowLayout {
                visible: catalog.busy || ((catalog.error.length > 0 || app.downloadMessage.length > 0) && app.downloadId.length > 0)
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                QuietButton { objectName: "catalogCancel"; text: "Cancel"; onClicked: app.cancelDownload() }
                QuietButton { objectName: "catalogRetry"; text: "Retry"; visible: !catalog.busy; enabled: !bible.busy; onClicked: { if (app.pendingDownload.toString().length) app.importDownloaded(); else app.installVersion(app.downloadId) } }
            }
            FineRule { Layout.fillWidth: true }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                QuietButton { objectName: "importCancel"; text: "Close"; font.pixelSize: 14 * importGuide.uiScale; onClicked: importGuide.close(); Keys.onReturnPressed: clicked(); Keys.onEnterPressed: clicked() }
                QuietButton {
                    id: importChoose
                    objectName: "importChoose"
                    text: "Import USFM ZIP…"
                    font.pixelSize: 14 * importGuide.uiScale
                    emphasized: true
                    enabled: !bible.busy && !catalog.busy
                    onClicked: { if (bible.busy || catalog.busy || !app.flushNote()) return; importGuide.choosePending = true; importGuide.close() }
                    Keys.onReturnPressed: clicked()
                    Keys.onEnterPressed: clicked()
                }
            }
        }
    }

    ModalCard {
        id: catalogDetails
        objectName: "catalogDetails"
        property var entry: null
        onClosed: { if (importGuide.visible) catalogSearch.forceActiveFocus(); else app.restorePanelFocus() }
        contentItem: OverlayScrollView {
            id: detailsScroll
            implicitHeight: Math.min(detailsContent.implicitHeight, app.height - 110)
            contentWidth: availableWidth
            clip: true
            ColumnLayout {
                id: detailsContent
                width: detailsScroll.availableWidth
                spacing: 16 * app.scale
                Label { text: "Details & Attribution"; color: desktop.foreground; font.pixelSize: 22 * app.scale; wrapMode: Text.Wrap; Layout.fillWidth: true }
                Label { text: catalogDetails.entry ? catalogDetails.entry.name : ""; textFormat: Text.PlainText; color: desktop.foreground; wrapMode: Text.Wrap; Layout.fillWidth: true }
                Label { objectName: "catalogCopyright"; text: catalogDetails.entry ? catalogDetails.entry.copyright : ""; textFormat: Text.PlainText; color: desktop.muted; wrapMode: Text.Wrap; Layout.fillWidth: true }
                QuietButton { objectName: "catalogProvider"; text: "eBible.org version & permissions ↗"; compact: true; onClicked: desktop.openExternal(catalogDetails.entry.detailsUrl) }
                QuietButton { text: "Close"; Layout.alignment: Qt.AlignRight; onClicked: catalogDetails.close() }
            }
        }
    }

    FileDialog {
        id: importDialog
        objectName: "importDialog"
        title: "Add version"
        fileMode: FileDialog.OpenFile
        nameFilters: ["USFM ZIP (*.zip)", "USFM files (*.usfm *.sfm)"]
        onAccepted: { if (app.closingWindow || bible.busy || catalog.busy || !app.flushNote()) return; bible.clearError(); bible.importUsfm(selectedFile) }
        onVisibleChanged: { if (!visible) app.restorePanelFocus() }
    }
    FolderDialog {
        id: exportDialog
        objectName: "exportDialog"
        title: "Export " + bible.versionAbbreviation + " notes only — choose a folder"
        onAccepted: { if (!app.flushNote()) return; if (bible.exportNotes(selectedFolder)) app.notify("Exported " + bible.versionAbbreviation + " notes as Markdown", true) }
        onVisibleChanged: { if (!visible) app.restorePanelFocus() }
    }
    ModalCard {
        id: backupScope
        objectName: "backupScope"
        property bool choosePending: false
        height: Math.min(470 * app.scale, app.height - 48)
        onOpened: { backupScopeScroll.contentItem.contentY = 0; backupChoose.forceActiveFocus() }
        onClosed: {
            const choose = choosePending
            choosePending = false
            if (choose) Qt.callLater(function() {
                if (app.actionAvailable("backup") && app.flushNote()) backupDialog.open()
                else app.restorePanelFocus()
            })
            else app.restorePanelFocus()
        }
        contentItem: ColumnLayout {
            spacing: 12 * app.scale
            Label {
                text: "Save " + bible.versionAbbreviation + " annotation backup"
                color: desktop.foreground; font.pixelSize: 20 * app.scale
                Layout.fillWidth: true; wrapMode: Text.Wrap
            }
            OverlayScrollView {
                id: backupScopeScroll
                objectName: "backupScopeScroll"
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: availableWidth
                clip: true
                Label {
                    objectName: "backupScopeExplanation"
                    width: backupScopeScroll.availableWidth
                    text: "This backup includes only " + bible.edition + " (" + bible.versionAbbreviation + ") bookmarks, highlights, notes, and reading location.\n\n"
                        + "Other editions are excluded. Switch to each edition to back it up separately. To back up retained annotations from a removed edition, reimport that edition first.\n\n"
                        + "Bible text, source USFM files, and global preferences are not included."
                    textFormat: Text.PlainText; color: desktop.foreground
                    font.pixelSize: 14 * app.scale; wrapMode: Text.Wrap
                }
            }
            FineRule { Layout.fillWidth: true }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                QuietButton {
                    objectName: "backupCancel"
                    text: "Cancel"
                    onClicked: backupScope.close()
                    Keys.onReturnPressed: clicked()
                    Keys.onEnterPressed: clicked()
                }
                QuietButton {
                    id: backupChoose
                    objectName: "backupChoose"
                    text: "Choose file…"
                    emphasized: true
                    enabled: app.actionAvailable("backup")
                    onClicked: { if (!app.flushNote()) return; backupScope.choosePending = true; backupScope.close() }
                    Keys.onReturnPressed: clicked()
                    Keys.onEnterPressed: clicked()
                }
            }
        }
    }
    FileDialog {
        id: backupDialog
        objectName: "backupDialog"
        title: "Save " + bible.versionAbbreviation + " annotation backup — active edition only"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "json"
        nameFilters: ["JSON backup (*.json)"]
        onAccepted: { if (!app.flushNote()) return; if (bible.exportBackup(selectedFile)) app.notify("Saved " + bible.versionAbbreviation + " annotation backup", true) }
        onVisibleChanged: { if (!visible) app.restorePanelFocus() }
    }
    FileDialog {
        id: restoreDialog
        objectName: "restoreDialog"
        title: "Restore " + bible.versionAbbreviation + " annotation backup"
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
                objectName: "restoreScopeExplanation"
                text: "This replaces only the active edition's bookmarks, highlights, notes, and reading location: " + bible.edition + " (" + bible.versionAbbreviation + "). The backup must match this edition. Other editions and global preferences are untouched. Your imported Bible stays in place."
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
        background: Rectangle { radius: Math.min(desktop.cornerRadius, width / 2, height / 2); color: desktop.background; border.color: desktop.border }
        Overlay.modal: Rectangle { color: app.tint(desktop.background, 0.78) }
        contentItem: ColumnLayout {
            spacing: 20
            BusyIndicator { running: bible.busy; Layout.alignment: Qt.AlignHCenter; implicitWidth: 36; implicitHeight: 36 }
            Label { text: "Preparing your library"; color: desktop.foreground; font.pixelSize: 21 * app.scale; font.weight: Font.Medium; Layout.alignment: Qt.AlignHCenter }
            Label { text: bible.status || "Reading and validating your USFM…"; color: desktop.muted; font.pixelSize: 13 * app.scale; wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter; Layout.fillWidth: true }
            ProgressBar {
                id: importProgressBar
                Layout.fillWidth: true
                from: 0
                to: 1
                value: Math.max(0, Math.min(1, bible.importProgress))
                indeterminate: bible.importProgress <= 0
                background: Rectangle { implicitHeight: 3; radius: Math.min(desktop.cornerRadius, width / 2, height / 2); color: desktop.surface }
                contentItem: Item { implicitHeight: 3; Rectangle { width: importProgressBar.indeterminate ? parent.width * 0.15 : parent.width * importProgressBar.visualPosition; height: parent.height; radius: Math.min(desktop.cornerRadius, width / 2, height / 2); color: desktop.accent } }
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
        radius: Math.min(desktop.cornerRadius, width / 2, height / 2)
        color: desktop.surface
        border.color: desktop.border
        visible: toastTimer.running && (!bible.focusMode || app.toastInFocus)
        Label { id: toast; objectName: "toast"; anchors.centerIn: parent; width: parent.width - 36; wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter; color: desktop.foreground; font.pixelSize: 13 * app.scale; Accessible.role: Accessible.AlertMessage }
    }
    Timer { id: toastTimer; interval: 3000 }
    Timer { id: countTimer; interval: 2000; onTriggered: app.clearCount() }
    Timer { id: positionSaveTimer; interval: 350; onTriggered: app.savePosition() }
    Timer { id: readingModeRestoreTimer; interval: 0; onTriggered: app.restorePosition(app.hasSelection) }
    Timer {
        id: deleteFocusTimer
        interval: 0
        onTriggered: {
            if (deleteConfirmation.visible) return
            if (savedDrawer.opened) app.moveSaved(0)
            else app.restorePanelFocus()
        }
    }
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
        target: catalog
        function onDownloadFinished(file) {
            if (app.closingWindow || !app.downloadId.length) return
            app.pendingDownload = file
            app.importDownloaded()
        }
    }
    Connections {
        target: bible
        function onVersionChanged() { app.invalidateDeleteSaved() }
        function onCollectionChanged() {
            if (deleteConfirmation.visible && !app.deleteScopeValid()) app.invalidateDeleteSaved()
        }
        function onErrorChanged() { Qt.callLater(app.showFocusedError) }
        function onBusyChanged() {
            if (!bible.busy) {
                if (app.importingCatalogId.length && bible.ready && !bible.error.length && bible.versionId === app.importingCatalogId)
                    app.notify((app.replacingCatalogVersion ? "Reimported " : "Installed ") + bible.versionAbbreviation, true)
                app.importingCatalogId = ""
                Qt.callLater(app.showFocusedError)
            }
        }
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
    Component.onCompleted: { readerInitialized = true; syncNote(); restorePosition() }
    onClosing: function(close) {
        close.accepted = flushNote()
        savePosition()
        if (close.accepted) { closingWindow = true; cancelDownload() }
    }

    Instantiator {
        model: app.actions.filter(function(action) { return !!action.sequences })
        delegate: Shortcut {
            required property var modelData
            // Application context reaches the notes drawer's popup focus scope;
            // only the active reader window may register its catalog shortcuts.
            context: Qt.ApplicationShortcut
            sequences: modelData.id === "books" && !app.readerKeys ? ["Ctrl+K"]
                : modelData.id === "first" ? ["G"] : modelData.id === "last" ? ["Shift+G"] : modelData.sequences
            enabled: app.active && app.shortcutEnabled(modelData)
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
