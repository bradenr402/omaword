pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import QtQml
import "HandDrawn.js" as HandDrawn
import "KeyNotation.js" as KeyNotation
import "Margins.js" as Margins
import "RowMenu.js" as RowMenu
// qmllint disable import
import OmaWord.Reader 1.0
// qmllint enable import

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
    font.pixelSize: Math.round(14 * app.scale)
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

    readonly property bool notesOpen: notesDrawer.shown
    readonly property alias rangeEditor: rangeEditor
    readonly property bool rangeEditing: rangeEditor.sessionOpen
    RangeEditor {
        id: rangeEditor
        objectName: "rangeEditor"
        app: app; bible: app.tagBackend; desktop: app.tagDesktop
        onFinished: function(saved) { if (saved) app.notify("Annotation range saved", true) }
    }
    // Public marker API: canonical scope (or exact overlapping scopes), optional kind.
    function openRangeEditor(scope = null, kind = "") { return rangeEditor.open(scope, kind) }
    function prepareRangeEditor() {
        pendingActionCollapse = null
        clearCount()
        clearTextSelection()
        notesDrawer.close()
        tagsUi.close()
        savedDrawer.close()
        verseTagMenu.close()
        contextMenu.close()
    }
    function focusedSavedEntry() {
        let focused = activeFocusItem
        while (focused && focused !== collectionList) {
            const row = focused as SavedEntry
            if (row) return row.modelData
            focused = focused.parent
        }
        return collectionList.model[collectionList.currentIndex] || null
    }
    function editSavedRange() {
        let focused = activeFocusItem
        while (focused && focused !== collectionList) {
            const row = focused as SavedEntry
            if (row) { openRangeEditor(row.modelData, ["bookmark", "highlight", "note", "tag"][collectionTab]); return }
            focused = focused.parent
        }
        const entry = collectionList.model[collectionList.currentIndex]
        if (entry) openRangeEditor(entry, ["bookmark", "highlight", "note", "tag"][collectionTab])
    }
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
        if (rangeEditing) return
        if (closingWindow || bible.busy || catalog.busy || !flushNote()) return
        pendingDownload = ""
        downloadMessage = ""
        downloadId = id
        catalog.clearError()
        catalog.download(id)
    }
    // Re-import an installed version whose text predates the current schema:
    // catalog versions download again; local imports ask for their file. Both
    // reuse the install/replace flow, so annotations (keyed by version) stay.
    property string reimportId: ""
    function catalogEntry(id) { return Array.from(catalog.entries).find(function(entry) { return entry.id === id }) || null }
    function reimportVersion(entry) {
        if (rangeEditing || !entry || closingWindow || bible.busy || catalog.busy || !flushNote()) return
        versionDialog.close()
        settingsDialog.close()
        if (catalogEntry(entry.id)) {
            importGuide.open()
            installVersion(entry.id)
        } else {
            reimportId = entry.id
            importDialog.open()
        }
    }
    // The file picker's choice; a pending Re-import pins the expected edition.
    function importChosen(file) {
        const expected = reimportId
        reimportId = ""
        if (rangeEditing || closingWindow || bible.busy || catalog.busy || !flushNote()) return
        bible.clearError(); bible.importUsfm(file, expected)
    }
    function cancelDownload() {
        downloadId = ""
        pendingDownload = ""
        downloadMessage = ""
        catalog.cancel()
    }
    function importDownloaded() {
        if (rangeEditing) return
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
        if (rangeEditing) return
        if (!scope || !scope.visual || !verseMode || scope.identity !== selectionIdentity) return
        verseMode = false
        clearCount()
        pendingActionCollapse = scope
        if (!pinned) editingPanelClosed()
    }
    function editingPanelClosed() {
        if (rangeEditing) return
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
        if (rangeEditing) return
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
        if (rangeEditing) return
        if (bible.busy || !flushNote()) return
        clearCount()
        if (manage) tagsUi.openManage()
        else { collectionTab = 3; savedDrawer.open() }
    }
    readonly property alias tagsUi: tagsUi
    readonly property var tagBackend: bible
    readonly property var tagDesktop: desktop
    TagsPanel {
        id: tagsUi
        objectName: "tagsUi"
        app: app; bible: app.tagBackend; desktop: app.tagDesktop
    }
    onVerseModeChanged: clearCount()
    property string verseCount: ""
    // Pending z prefix for zz/zt/zb. Implemented as single-key shortcuts,
    // since Qt's multi-key sequences stay in a partial-match state that
    // swallows unrelated keys (digits, Ctrl+K) across focus changes.
    property bool zPending: false
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
    readonly property bool paletteTextMode: referenceInput.text.trim().startsWith("/")
    readonly property string paletteTextQuery: paletteTextMode ? referenceInput.text.trim().substring(1).trim() : ""
    property string submittedSearchQuery: ""
    property string submittedSearchGeneration: ""
    property var pendingSearchResume: null
    readonly property bool paletteSearchCurrent: paletteTextMode && !bibleSearchTimer.running
        && submittedSearchQuery === paletteTextQuery && submittedSearchGeneration === bible.textGeneration
    property string acceptedSearchQuery: ""
    property bool acceptedSearchBibleSyntax: false
    property string chapterQueryHistory: ""
    property var searchMatches: []
    property int searchMatchIndex: -1
    property string searchError: ""
    property string searchMatchIdentity: ""
    property var findSnapshot: null
    readonly property bool findPromptOpen: chapterFind.opened
    readonly property var searchActiveMatch: searchMatches[searchMatchIndex] || null
    readonly property var searchSegments: {
        const segments = []
        searchMatches.forEach(function(match, index) {
            Array.from(match.segments || []).forEach(function(segment) {
                segments.push({ verse: segment.verse, start: segment.start, length: segment.length, active: index === searchMatchIndex })
            })
        })
        return segments
    }
    readonly property string searchScope: bible.textGeneration + ":" + bible.book + ":" + bible.chapter
    onSearchScopeChanged: if (readerInitialized) searchRefreshTimer.restart()
    function localSearch(query, bibleSyntax) {
        const result = bible.findInChapter(query, bibleSyntax)
        searchError = result.error || ""
        searchMatches = Array.from(result.matches || []).sort(function(a, b) {
            return a.start - b.start || a.segments[0].start - b.segments[0].start || a.end - b.end
        })
        searchMatchIndex = -1
        searchMatchIdentity = ""
    }
    function nearestSearchMatch() {
        return searchMatches.findIndex(function(match) { return match.start >= bible.cursorVerse })
    }
    function refreshAcceptedSearch() {
        if (findPromptOpen) { chapterFind.cancelPending(); chapterFind.opened = false; findSnapshot = null; readerFocus() }
        localSearch(acceptedSearchQuery, acceptedSearchBibleSyntax)
        // Reapply ink only: chapter/version navigation owns its own viewport.
    }
    function clearSearch() {
        acceptedSearchQuery = ""; searchMatches = []; searchMatchIndex = -1
        searchMatchIdentity = ""; searchError = ""
    }
    function beginChapterFind() {
        if (rangeEditing || !bible.ready || bible.busy || !flushNote()) return
        if (verseMode && !setVisualMode(false)) return
        clearCount(); clearTextSelection(); positionTimer.stop(); positionSaveTimer.stop()
        findSnapshot = { query: acceptedSearchQuery, bibleSyntax: acceptedSearchBibleSyntax,
            matches: searchMatches, index: searchMatchIndex, identity: searchMatchIdentity,
            error: searchError, scope: bible.tagScope(), cursor: bible.cursorVerse,
            anchor: selectionAnchor, position: readerTargetY(), generation: searchScope }
        chapterFind.begin(chapterQueryHistory)
        previewChapterFind(chapterQueryHistory)
    }
    function previewChapterFind(query) {
        if (!findPromptOpen) return
        localSearch(query, false)
        searchMatchIndex = nearestSearchMatch()
        const match = searchActiveMatch
        if (match) scrollSearchMatch(match)
        else if (searchMatches.length && !searchError.length) searchError = "No matches after cursor"
    }
    function acceptChapterFind(query) {
        if (!findPromptOpen || !flushNote()) return
        previewChapterFind(query)
        if (searchMatchIndex < 0 && searchMatches.length) {
            searchMatchIndex = 0
            searchError = ""
        }
        if (searchError.length) return
        const match = searchActiveMatch
        if (!query.trim().length || !match || !focusSearchMatch(match)) return
        chapterQueryHistory = query
        acceptedSearchQuery = query
        acceptedSearchBibleSyntax = false
        chapterFind.opened = false; findSnapshot = null
        readerFocus()
    }
    function acceptFindByClick() {
        if (!findPromptOpen) return
        chapterFind.cancelPending()
        if (chapterFind.query.trim().length) acceptChapterFind(chapterFind.query)
        if (findPromptOpen) cancelChapterFind()
    }
    function cancelChapterFind() {
        chapterFind.cancelPending()
        const saved = findSnapshot
        chapterFind.opened = false; findSnapshot = null
        if (saved && saved.generation === searchScope) {
            acceptedSearchQuery = saved.query; acceptedSearchBibleSyntax = saved.bibleSyntax
            searchMatches = saved.matches; searchMatchIndex = saved.index
            searchMatchIdentity = saved.identity; searchError = saved.error
            // Preview never changes the canonical scope or its direction.
            selectionAnchor = saved.anchor; verseMode = false
            positionTimer.stop(); positionSaveTimer.stop(); restoringPosition = false
            setReaderY(saved.position); savePosition()
        } else refreshAcceptedSearch()
        readerFocus()
    }
    function focusSearchMatch(match) {
        if (!match || !flushNote()) return false
        pendingActionCollapse = null; clearTextSelection(); verseMode = false
        bible.selectVerses(match.start, match.end)
        selectionAnchor = match.start; syncNote()
        searchMatchIdentity = selectionIdentity
        positionTimer.stop(); restoringPosition = false
        scrollSearchMatch(match); savePosition()
        return true
    }
    function navigateSearch(direction, count) {
        if (rangeEditing || !acceptedSearchQuery.length) return
        if (searchRefreshTimer.running) { searchRefreshTimer.stop(); refreshAcceptedSearch() }
        if (!searchMatches.length) { notify("No matches in this chapter", true); sounds.play("boundary"); return }
        let current = searchMatchIdentity === selectionIdentity ? searchMatchIndex : -1
        let target
        if (current >= 0) target = current + direction * count
        else if (direction > 0) {
            current = searchMatches.findIndex(function(match) { return match.start >= bible.cursorVerse })
            target = (current < 0 ? searchMatches.length : current) + count - 1
        } else {
            current = searchMatches.length - 1
            while (current >= 0 && searchMatches[current].start > bible.cursorVerse) --current
            target = current - count + 1
        }
        const clamped = Math.max(0, Math.min(searchMatches.length - 1, target))
        if (!focusSearchMatch(searchMatches[clamped])) return
        searchMatchIndex = clamped
        if (target !== clamped) {
            notify(direction > 0 ? "No matches after cursor" : "No matches before cursor", true)
            sounds.play("boundary")
        } else sounds.play("tick")
        readerFocus()
        pulseDestination()
    }
    function scrollSearchMatch(match) {
        const segment = match && match.segments[0]
        if (!segment) return
        readerColumn.forceLayout()
        const block = blockLoader.item as BlockReader
        const index = Array.from(bible.verses).findIndex(function(row) { return row.number === segment.verse })
        const row = verseRepeater.itemAt(index) as VerseItem
        const rect = block ? block.searchRect(segment) : row ? row.searchOverlay.rectFor(segment.start, segment.length) : null
        if (!rect || rect.height <= 0 || bible.focusMode) { scrollToVerse(segment.verse, false); return }
        const y = block ? blockLoader.mapToItem(readerColumn, rect.x, rect.y).y
            : row.body.mapToItem(readerColumn, rect.x, rect.y).y
        const available = Math.max(1, reading.height - Math.max(floatingSafety, findPromptOpen ? chapterFind.height : 0))
        const margin = Math.min(readerLineHeight * 2, available * 0.2)
        let target = readerTargetY()
        if (y < target + margin) target = y - margin
        else if (y + rect.height > target + available - margin) target = y + rect.height - available + margin
        slideReader(target)
    }
    function openTextSearch() {
        if (rangeEditing || !bible.ready || !flushNote()) return
        openPalette(false)
        referenceInput.text = "/ "
        referenceInput.cursorPosition = 2
    }
    function cyclePaletteMode(direction) {
        if (!bible.ready) return
        const current = paletteTextMode ? 1 : referenceInput.text.trim().startsWith(">") ? 2 : 0
        const next = (current + direction + 3) % 3
        sounds.play("tick")
        if (next === 1) openTextSearch()
        else openPalette(next === 2)
    }
    function scheduleBibleSearch() {
        pendingSearchResume = null
        bibleSearchTimer.stop()
        submittedSearchQuery = ""; submittedSearchGeneration = ""
        bible.cancelScriptureSearch()
        // onTextChanged can run before the derived mode/query bindings update.
        const text = referenceInput.text.trim()
        if (text.startsWith("/") && text.substring(1).trim().length) bibleSearchTimer.restart()
    }
    function submitBibleSearch() {
        submittedSearchQuery = paletteTextQuery
        submittedSearchGeneration = bible.textGeneration
        bible.searchScripture(submittedSearchQuery)
    }
    function restoreSearchPalette() {
        const saved = pendingSearchResume
        if (!saved || !paletteSearchCurrent || bible.searchBusy) return
        if (saved.query !== referenceInput.text || saved.generation !== bible.textGeneration) { pendingSearchResume = null; return }
        if (bible.searchResults.length <= saved.row && bible.searchResults.length < bible.searchResultCount) {
            bible.loadMoreSearchResults(); return
        }
        pendingSearchResume = null
        paletteResults.currentIndex = Math.max(0, Math.min(saved.row, paletteResults.count - 1))
        paletteArrowUsed = saved.arrow
        Qt.callLater(function() {
            if (referenceInput.text !== saved.query) return
            paletteResults.positionViewAtIndex(paletteResults.currentIndex, ListView.Contain)
            paletteResults.contentY = saved.position
        })
    }
    function acceptBibleResult(entry) {
        if (!paletteSearchCurrent || bible.searchBusy || !entry || entry.recentQuery || !flushNote()) return
        const query = submittedSearchQuery
        const from = beginJump()
        if (!bible.openTaggedPassage(entry)) return
        endJump(from)
        bible.recordQuery(query)
        acceptedSearchQuery = query; acceptedSearchBibleSyntax = true
        searchRefreshTimer.stop(); localSearch(query, true)
        searchMatchIndex = searchMatches.findIndex(function(match) {
            return match.start === entry.start && match.end === entry.end
                && JSON.stringify(match.segments) === JSON.stringify(entry.segments)
        })
        if (searchMatchIndex < 0) searchMatchIndex = searchMatches.findIndex(function(match) { return match.start === entry.start && match.end === entry.end })
        searchMatchIdentity = selectionIdentity; verseMode = false; syncNote()
        passagePalette.navigated = true; passagePalette.close(); readerFocus()
        positionTimer.stop(); restoringPosition = false
        pulseDestination()
        Qt.callLater(function() { app.scrollSearchMatch(entry); app.savePosition() })
    }
    Timer { id: bibleSearchTimer; interval: 150; onTriggered: app.submitBibleSearch() }
    Timer { id: searchRefreshTimer; interval: 0; onTriggered: app.refreshAcceptedSearch() }
    readonly property var paletteDestination: {
        if (paletteTextMode) return { entry: paletteSearchCurrent || !paletteTextQuery.length ? paletteResults.model[paletteResults.currentIndex] : null, reference: "" }
        let query = referenceInput.text.trim()
        let entry = paletteResults.model[paletteResults.currentIndex]
        if (entry && entry.candidate) return { entry: entry, reference: entry.reference }
        if (entry && (paletteArrowUsed || !query || query.startsWith(">")
                || (paletteBook > 0 && /^\d+$/.test(query))
                || (paletteBook === 0 && !/\d/.test(query)))) {
            return { entry: entry, reference: entry.chapter ? entry.title : "" }
        }
        return { entry: null, reference: paletteBook > 0 && /^[\d:\s–-]+$/.test(query)
            ? paletteBookName + " " + (paletteChapter > 0 && !query.includes(":") ? paletteChapter + ":" : "") + query : query }
    }
    readonly property var paletteResolution: {
        if (paletteTextMode) return { valid: false, reference: "", error: "" }
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
    // The desktop text scale sizes the reader text. The Text size preference
    // (bible.fontSize, default 18) also scales every other piece of UI text
    // and its controls proportionally, so `scale` is the product of both.
    readonly property real systemScale: desktop.textScale > 0 ? desktop.textScale : 1
    readonly property real readerScale: systemScale
    readonly property real fontScale: bible.fontSize > 0 ? bible.fontSize / 18 : 1
    readonly property real scale: systemScale * fontScale
    readonly property bool hasSelection: bible.ready && bible.selectedStart > 0
    readonly property bool contextMenuOpen: contextMenu.visible
    readonly property bool modalOpen: dialogOpen || savedDrawer.shown || deskDrawer.shown || historyDrawer.shown || tagsUi.visibleModal
    // Everything modal except the side panels (Annotations, Desk, Study).
    readonly property bool dialogOpen: rangeEditor.visibleModal || contextMenu.visible || tagsUi.pickerVisible || verseTagMenu.visible || backupScope.visible || versionDialog.visible || passagePalette.visible || cleanDeskConfirmation.visible || clearHistoryConfirmation.visible || deleteConfirmation.visible || helpDialog.visible || settingsDialog.visible || aboutDialog.visible || importGuide.visible || catalogDetails.visible || errorDialog.visible || restoreConfirmation.visible || importDialog.visible || exportDialog.visible || backupDialog.visible || restoreDialog.visible
    readonly property bool notesClosableFromReader: notesOpen && bible.ready && !bible.busy && !modalOpen && !findPromptOpen && !textInputActive()
    // One side panel at a time. Its opening key does nothing while it is open;
    // another panel's key switches to that panel.
    readonly property string sidePanel: notesOpen ? "notes" : savedDrawer.shown ? "annotations"
        : deskDrawer.shown ? "desk" : historyDrawer.shown ? "history" : tagsUi.relatedShown ? "related" : ""
    readonly property var panelActions: ["notes", "annotations", "desk", "related", "history"]
    readonly property bool panelSwitchKeys: sidePanel.length > 0 && bible.ready && !bible.busy && !dialogOpen
        && !findPromptOpen && !rangeEditing && !textInputActive()
    // Closes the open side panel before another opens. A Notes draft must
    // save first; when it cannot, Notes stays open with its error.
    function leaveSidePanel(target) {
        const current = sidePanel
        if (current === target) return false
        if (!flushNote()) return false
        if (current === "notes") return closeNotes()
        if (current === "annotations") savedDrawer.close()
        else if (current === "desk") deskDrawer.close()
        else if (current === "history") historyDrawer.close()
        else if (current === "related") tagsUi.close()
        return true
    }
    // The chapter outline, reached by tab, keeps its scroll keys.
    readonly property bool readerBase: bible.ready && !bible.busy && !modalOpen && !notesOpen && !findPromptOpen && !textInputActive()
        && !(chapterOutline && chapterOutline.activeFocus)
    // Character selection takes the reader's keys itself (charKey).
    readonly property bool readerKeys: readerBase && !charMode
    onReaderKeysChanged: { if (!readerKeys) clearCount() }
    readonly property real notesWidth: Math.min(390 * scale, width - 40)
    readonly property real readerLineHeight: readerMetrics.height * bible.lineHeight
    readonly property real observedLineHeight: bible.lineHeight
    // Passage text in panels follows the reader's line height proportionally:
    // 1.25 at the default 1.5, so the panels stay compact but track the setting.
    readonly property real panelLineHeight: bible.lineHeight > 0 ? bible.lineHeight / 1.5 * 1.25 : 1.25
    onObservedLineHeightChanged: {
        // Replacing the rich document invalidates Qt's selection offsets.
        // Clear the shared clipboard source in the same update as the paint.
        clearTextSelection()
        draggingText = false
        dragAnchorRow = -1
    }
    readonly property real floatingSafety: bible.focusMode ? 0 : Math.max(72 * systemScale, footerHeight + 2 * footerMargin)
    readonly property real verseGutter: 76 * readerScale
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
    onObservedFocusModeChanged: { toastTimer.stop(); footerHoverRevealed = false; footerRevealTimer.stop(); Qt.callLater(app.applyFocusMode) }
    function applyFocusMode() {
        if (observedFocusMode) {
            if (!closeNotes()) { bible.setFocusMode(false); return }
            tagsUi.closeForFocusMode()
            verseTagMenu.close()
            passagePalette.close(); savedDrawer.close(); helpDialog.close(); aboutDialog.close()
            if (!settingsDialog.visible) readerFocus()
            if (hasSelection) Qt.callLater(function() { readerColumn.forceLayout(); app.scrollToVerse(bible.cursorVerse, false) })
        }
    }
    readonly property real controlScale: Math.min(scale, 1.4)
    // Motion feedback is paint-only: opacity and translucent layers never change
    // layout, text, selection, copy output, or hit-testing. Reduce motion (shared
    // app-wide on `sounds`) collapses every duration to an instant change.
    readonly property bool reduceMotion: sounds.reduceMotion
    readonly property int fadeMs: reduceMotion ? 0 : 140
    readonly property real focusDimOpacity: 0.32
    // Focus mode keeps the selection, else the cursor verse, at full strength.
    readonly property int focusFirst: bible.selectedStart > 0 ? bible.selectedStart : bible.cursorVerse
    readonly property int focusLast: bible.selectedStart > 0 ? bible.selectedEnd : bible.cursorVerse
    // Display-only LRM at each line start: plain Text has no base-direction
    // setting, and a Hebrew-first line must stay an LTR paragraph.
    function leftToRightText(text) {
        return text ? String(text).split("\n").map(function(line) { return "\u200E" + line }).join("\n") : ""
    }
    function verseDimmed(number) { return bible.focusMode && (number < focusFirst || number > focusLast) }
    // 0 → 1 as focus mode is entered: the only dimming that fades. Which verses
    // are dimmed follows the cursor instantly.
    property real focusDimLevel: bible.focusMode ? 1 : 0
    Behavior on focusDimLevel { NumberAnimation { duration: app.fadeMs } }
    readonly property real focusDimmedOpacity: 1 - (1 - focusDimOpacity) * focusDimLevel
    property int pulseFirst: 0
    property int pulseLast: 0
    property real pulseLevel: 0
    property int flashFirst: 0
    property int flashLast: 0
    property real flashLevel: 0
    readonly property bool pulseActive: pulseAnimation.running
    readonly property bool flashActive: flashAnimation.running
    onReduceMotionChanged: if (reduceMotion) { pulseAnimation.stop(); flashAnimation.stop(); pulseLevel = 0; flashLevel = 0 }
    // Deferred so the pulse marks the final selection after navigation settles.
    function pulseDestination() { Qt.callLater(app.startPulse) }
    function startPulse() {
        if (reduceMotion || !bible.ready || focusFirst <= 0) return
        pulseFirst = focusFirst
        pulseLast = focusLast
        pulseAnimation.restart()
    }
    function flashVerses(first, last) {
        if (reduceMotion || first <= 0) return
        flashFirst = first
        flashLast = Math.max(first, last)
        flashAnimation.restart()
    }
    function flashChapter() {
        const rows = Array.from(bible.verses)
        if (rows.length) flashVerses(rows[0].number, rows[rows.length - 1].number)
    }
    function flashScope(scope) {
        if (scope && scope.book === bible.book && scope.chapter === bible.chapter && (scope.endBook || scope.book) === bible.book
                && (scope.endChapter || scope.chapter) === bible.chapter) flashVerses(scope.start, scope.end)
    }
    SequentialAnimation {
        id: pulseAnimation
        NumberAnimation { target: app; property: "pulseLevel"; from: 0; to: 1; duration: 180; easing.type: Easing.OutQuad }
        NumberAnimation { target: app; property: "pulseLevel"; to: 0; duration: 420; easing.type: Easing.InOutQuad }
    }
    SequentialAnimation {
        id: flashAnimation
        NumberAnimation { target: app; property: "flashLevel"; from: 0; to: 1; duration: 90; easing.type: Easing.OutQuad }
        NumberAnimation { target: app; property: "flashLevel"; to: 0; duration: 260; easing.type: Easing.InQuad }
    }
    FontMetrics { id: readerMetrics; font.family: desktop.fontFamily; font.pixelSize: app.readerSize * app.readerScale }
    component VerseItem: Item {
        property real textOffset: 0
        property real textHeight: 0
        property TextEdit body
        property SearchHighlights searchOverlay
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
        if (charMode) { charMode = false; charPending = ""; charAnchor = null; charCursor = null }
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
        if (rangeEditing && gutterDrag) return
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
                pieces.push(body.selectedText.replace(/[\uFFFC\u2060]/g, ""))
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
    // Context menu: one themed menu for passages, markers, Desk items,
    // annotation rows, and search results. Items reuse the catalog actions.
    readonly property var menuDivider: ({ separator: true })
    // Menus show an action's primary key only.
    function menuKey(action) { const keys = actionKeys(action); return keys.length ? KeyNotation.format(keys[0]) : "" }
    function menuAction(id, text, action) {
        return { id: id, text: text, shortcut: menuKey(action), enabled: actionAvailable(action),
            run: function() { app.runCommand(action) } }
    }
    // The cursor verse's annotation ranges of one kind (every layout reads bible.verses).
    function cursorRanges(kind) {
        const row = Array.from(bible.verses).find(function(verse) { return verse.number === bible.cursorVerse })
        return row ? Array.from(row[kind + "Ranges"] || []) : []
    }
    // Opens a kind's range at the cursor; overlapping ranges get the chooser.
    function openCursorRanges(kind, sceneX, sceneY) {
        const ranges = cursorRanges(kind)
        if (!ranges.length) { rejected(); return }
        if (ranges.length === 1) {
            if (kind === "tag") openPassageTags(ranges[0])
            else if (kind === "bookmark") openBookmark(ranges[0])
            else openNoteScope(ranges[0])
            return
        }
        verseTagMenu.annotationKind = kind
        verseTagMenu.entries = ranges
        verseTagMenu.popup(sceneX, sceneY)
    }
    function openRangeEntries(sceneX, sceneY) {
        const ready = !rangeEditing && !bible.busy
        const entries = [["bookmark", "openBookmark", "Open bookmark"], ["note", "openNote", "Open note"], ["tag", "openTags", "Open tags…"]]
            .filter(function(kind) { return app.cursorRanges(kind[0]).length > 0 })
            .map(function(kind) {
                const many = app.cursorRanges(kind[0]).length > 1
                return { id: kind[1], text: kind[2] + (many ? " (choose)" : ""), enabled: ready,
                    run: function() { app.openCursorRanges(kind[0], sceneX, sceneY) } }
            })
        return entries.length ? [menuDivider].concat(entries) : []
    }
    function selectedTextEntries() {
        return [{ id: "copySelected", text: "Copy selected text", shortcut: menuKey("copyText"), enabled: !bible.busy,
            run: function() { app.copySelectedText(false) } },
            { id: "copySelectedPlain", text: "Copy selected plain text", shortcut: menuKey("copyTextPlain"), enabled: !bible.busy,
            run: function() { app.copySelectedText(true) } }, menuDivider]
    }
    // Selected text (mouse-dragged or character selection): ctrl c copies it
    // as a quotation (ellipses at cut verses, numbers for verses whose start is
    // selected), ctrl shift C as plain text, exactly as selected. A selection
    // that maps to no Scripture text (a heading alone) copies its plain text.
    function copySelectedText(plainText) {
        if (!freeSelectedText.length || bible.busy) { rejected(); return false }
        // Plain text is the selection as shown (character selection: its source text).
        const range = plainText ? null : selectedSourceRange()
        const text = range ? bible.copyCharacters(range.start.verse, range.start.offset, range.end.verse, range.end.offset, false) : ""
        if (!text.length) copyFreeText()
        if (range && text.length) flashVerses(range.start.verse, range.end.verse)
        notify(plainText || !text.length ? "Selected text copied as plain text" : "Selected text copied as quotation", true)
        sounds.play("chime")
        return true
    }

    // ── Character selection (shift V): Vim's charwise visual over the source
    // text of the chapter. Positions are {verse, offset}: a verse number and a
    // UTF-16 offset into that verse's plain text, shared by all three layouts;
    // each layout maps them into its own document (SearchDocument per Verse
    // row, BlockDocument in Paragraph/Block) and paints the native selection.
    // Motions and copy text come from the backend (textselection.cpp); lines
    // (j k 0 ^ $) come from the layout's geometry.
    property bool charMode: false
    property var charAnchor: null
    property var charCursor: null
    property real charColumn: -1
    property string charPending: ""
    readonly property string charChapterKey: bible.versionId + ":" + bible.book + ":" + bible.chapter + ":" + bible.readingMode
    onCharChapterKeyChanged: if (charMode) endCharMode()
    onReaderBaseChanged: if (!readerBase && charMode) endCharMode()
    function verseText(number) {
        const row = Array.from(bible.verses).find(function(v) { return v.number === number })
        return row ? String(row.text || "") : ""
    }
    function verseRowFor(number) {
        const index = Array.from(bible.verses).findIndex(function(v) { return v.number === number })
        return index >= 0 ? verseRepeater.itemAt(index) as VerseItem : null
    }
    // A source range of one verse in its layout: {body, start, end} document positions.
    function charDocRange(verse, offset, length) {
        const block = blockLoader.item as BlockReader
        let range = []
        let body = null
        if (block) { range = Array.from(block.mapping.documentRange(verse, offset, length)); body = block.body }
        else {
            const row = verseRowFor(verse)
            if (!row || !row.searchOverlay) return null
            range = Array.from(row.searchOverlay.documentRange(offset, length)); body = row.body
        }
        return range.length === 2 ? { body: body, start: range[0], end: range[1] } : null
    }
    // A document position of `body` (verse row `verse`, or the block body) back
    // to source: forward → the first character at/after it, else the exclusive end before it.
    function charSourceAt(body, verse, position, forward) {
        const block = blockLoader.item as BlockReader
        if (block) {
            const at = block.mapping.sourceAt(position, forward)
            return at && at.verse ? { verse: at.verse, offset: at.offset } : null
        }
        const row = verseRowFor(verse)
        if (!row || !row.searchOverlay) return null
        const offset = row.searchOverlay.sourceAt(position, forward)
        return forward ? (offset >= 0 ? { verse: verse, offset: offset } : null) : { verse: verse, offset: offset }
    }
    function charLength(pos) { return Math.max(1, bible.characterEnd(pos.verse, pos.offset) - pos.offset) }
    // The character's box in readerColumn coordinates (x, y, height), with its body.
    function charRect(pos) {
        if (!pos) return null
        const range = charDocRange(pos.verse, pos.offset, charLength(pos))
        if (!range) return null
        const r = range.body.positionToRectangle(range.start)
        const p = range.body.mapToItem(readerColumn, r.x, r.y)
        return { x: p.x, y: p.y, height: r.height, body: range.body, verse: pos.verse, position: range.start }
    }
    function charCompare(a, b) { return a.verse !== b.verse ? verseOrder(a.verse) - verseOrder(b.verse) : a.offset - b.offset }
    function verseOrder(number) { return Array.from(bible.verses).findIndex(function(v) { return v.number === number }) }
    // [start, end) of the character selection, end exclusive.
    function charRange() {
        if (!charMode || !charAnchor || !charCursor) return null
        const forward = charCompare(charAnchor, charCursor) <= 0
        const first = forward ? charAnchor : charCursor, last = forward ? charCursor : charAnchor
        return { start: { verse: first.verse, offset: first.offset },
                 end: { verse: last.verse, offset: bible.characterEnd(last.verse, last.offset) } }
    }
    // The mouse-dragged selection as source [start, end), or the character selection.
    function selectedSourceRange() {
        if (charMode) return charRange()
        const block = blockLoader.item as BlockReader
        if (block) {
            const body = block.body
            if (body.selectionStart === body.selectionEnd) return null
            const start = charSourceAt(body, 0, body.selectionStart, true)
            const end = charSourceAt(body, 0, body.selectionEnd, false)
            return start && end && charCompare(start, end) < 0 ? { start: start, end: end } : null
        }
        let start = null, end = null
        for (let i = 0; i < verseRepeater.count; ++i) {
            const row = verseRepeater.itemAt(i) as VerseItem
            if (!row || !row.body || row.body.selectionStart === row.body.selectionEnd) continue
            const number = bible.verses[i].number
            if (!start) start = charSourceAt(row.body, number, row.body.selectionStart, true) || { verse: number, offset: verseText(number).length }
            end = charSourceAt(row.body, number, row.body.selectionEnd, false)
        }
        return start && end && charCompare(start, end) < 0 ? { start: start, end: end } : null
    }
    // Paints the character selection with the layouts' native text selection.
    function applyCharSelection(reveal) {
        const range = charRange()
        if (!range) return
        const last = { verse: range.end.verse, offset: charCompare(charAnchor, charCursor) <= 0 ? charCursor.offset : charAnchor.offset }
        const first = charDocRange(range.start.verse, range.start.offset, charLength(range.start))
        const end = charDocRange(last.verse, last.offset, range.end.offset - last.offset)
        const block = blockLoader.item as BlockReader
        if (block) {
            if (first && end) block.body.select(first.start, end.end)
        } else {
            const a = verseOrder(range.start.verse), z = verseOrder(range.end.verse)
            for (let i = 0; i < verseRepeater.count; ++i) {
                const row = verseRepeater.itemAt(i) as VerseItem
                if (!row || !row.body) continue
                if (i < a || i > z) { row.body.deselect(); continue }
                row.body.select(i === a && first ? first.start : 0, i === z && end ? end.end : row.body.length)
            }
        }
        freeSelectedText = bible.characterText(range.start.verse, range.start.offset, range.end.verse, range.end.offset, false)
        if (reveal !== false) revealChar()
    }
    function revealChar() {
        const rect = charRect(charCursor)
        if (!rect) return
        const available = Math.max(1, reading.height - floatingSafety)
        const margin = Math.min(2 * readerLineHeight, available / 4)
        const top = readerTargetY()
        if (rect.y < top + margin) slideReader(rect.y - margin)
        else if (rect.y + rect.height > top + available - margin) slideReader(rect.y + rect.height - available + margin)
    }
    function firstInk(verse) {
        const text = verseText(verse)
        let i = 0
        while (i < text.length - 1 && /\s/.test(text.charAt(i))) ++i
        return i
    }
    function beginCharMode() {
        if (!bible.ready || bible.busy || rangeEditing || !bible.verses.length) { rejected(); return false }
        clearCount()
        // A mouse-dragged selection becomes the character selection.
        const dragged = freeSelectedText.length ? selectedSourceRange() : null
        let anchor, cursor
        if (dragged) {
            anchor = dragged.start
            cursor = bible.textMotion("", dragged.end.verse, Math.max(0, dragged.end.offset - 1))
        } else {
            // The cursor verse when one is selected, else the first text on screen.
            const number = hasSelection && bible.cursorVerse > 0 ? bible.cursorVerse : 0
            anchor = number ? bible.textMotion("", number, firstInk(number)) : firstVisibleChar()
            cursor = anchor
        }
        if (!anchor || !anchor.verse || !cursor || !cursor.verse) { rejected(); return false }
        if (verseMode) { verseMode = false }
        charAnchor = { verse: anchor.verse, offset: anchor.offset }
        charCursor = { verse: cursor.verse, offset: cursor.offset }
        charColumn = -1
        charPending = ""
        charMode = true
        readerFocus()
        applyCharSelection()
        notify("Character selection", true)
        return true
    }
    function firstVisibleChar() {
        const top = readerTargetY()
        const bottom = top + Math.max(1, reading.height - floatingSafety)
        for (let y = Math.max(top, 0) + 4; y < bottom; y += Math.max(2, readerLineHeight / 3)) {
            // The start of the first line whose text is on screen.
            const hit = charAtPoint(0, y)
            if (hit && hit.rect.y >= top) return hit.pos
        }
        const first = bible.verses[0].number
        return bible.textMotion("", first, firstInk(first))
    }
    function endCharMode() {
        if (!charMode) return
        charMode = false
        charPending = ""
        charAnchor = null
        charCursor = null
        clearCount()
        clearTextSelection()
    }
    function setCharCursor(pos, keepColumn) {
        if (!pos || !pos.verse) { rejected(); return }
        charCursor = { verse: pos.verse, offset: pos.offset }
        if (!keepColumn) charColumn = -1
        applyCharSelection()
    }
    function charCount() {
        const count = Math.max(1, Math.min(999999, Number(verseCount) || 1))
        clearCount()
        return count
    }
    function charMotion(motion, counted) {
        let count = 1
        if (counted === false) clearCount()
        else count = charCount()
        const next = bible.textMotion(motion, charCursor.verse, charCursor.offset, count)
        if (next && next.verse === charCursor.verse && next.offset === charCursor.offset) sounds.play("boundary")
        setCharCursor(next)
    }
    function charObject(object) {
        const count = charCount()
        const range = bible.textObject(object, charAnchor.verse, charAnchor.offset, charCursor.verse, charCursor.offset, count)
        if (!range || !range.anchor || !range.anchor.verse) { rejected(); return }
        charAnchor = { verse: range.anchor.verse, offset: range.anchor.offset }
        setCharCursor(range.cursor)
    }
    function charSwap() {
        clearCount()
        const anchor = charAnchor
        charAnchor = charCursor
        setCharCursor(anchor)
    }
    // The character under a readerColumn point, on that point's line.
    function charAtPoint(x, y) {
        const block = blockLoader.item as BlockReader
        let body = null, verse = 0, position = 0
        if (block) {
            body = block.body
            const p = body.mapFromItem(readerColumn, x, y)
            position = body.positionAt(p.x, p.y)
        } else {
            // Only a point on a row's text: headings and row gaps hold none.
            for (let i = 0; i < verseRepeater.count && !body; ++i) {
                const row = verseRepeater.itemAt(i) as VerseItem
                const p = row && row.body ? row.body.mapFromItem(readerColumn, x, y) : null
                if (!p || p.y < 0 || p.y > row.body.height) continue
                body = row.body
                verse = bible.verses[i].number
                position = body.positionAt(p.x, p.y)
            }
            if (!body) return null
        }
        // Below a line's glyphs (line spacing) Qt answers the document end:
        // ask again at the middle of the line that position is on.
        const line = body.positionToRectangle(position)
        const local = body.mapFromItem(readerColumn, x, y)
        position = body.positionAt(local.x, line.y + line.height / 2)
        const lineY = body.positionToRectangle(position).y
        let at = charSourceAt(body, verse, position, true)
        let rect = at ? charRect(at) : null
        // Past the line's last character: take that character instead.
        if (!rect || body.mapFromItem(readerColumn, rect.x, rect.y).y > lineY + 1) {
            const before = charSourceAt(body, verse, position, false)
            if (!before || before.offset <= 0) return rect ? { pos: at, rect: rect } : null
            at = bible.textMotion("", before.verse, before.offset - 1)
            rect = charRect(at)
        }
        return rect ? { pos: at, rect: rect } : null
    }
    function charLineStep(direction) {
        const count = charCount()
        let pos = charCursor
        let rect = charRect(pos)
        if (!rect) { rejected(); return }
        if (charColumn < 0) charColumn = rect.x
        let moved = false
        for (let i = 0; i < count; ++i) {
            let next = null
            let y = direction > 0 ? rect.y + rect.height + 1 : rect.y - 1
            const step = Math.max(2, rect.height / 3)
            for (let n = 0; n < 600 && y >= 0 && y <= readerColumn.height; ++n, y += direction * step) {
                const hit = charAtPoint(charColumn, y)
                if (hit && (direction > 0 ? hit.rect.y > rect.y + rect.height / 2 : hit.rect.y + hit.rect.height / 2 < rect.y)) { next = hit; break }
            }
            if (!next) break
            pos = next.pos; rect = next.rect; moved = true
        }
        if (!moved) { sounds.play("boundary"); return }
        setCharCursor(pos, true)
    }
    // 0 ^ $: the cursor's visual line in the current layout.
    function charLineEdge(edge) {
        clearCount()
        const rect = charRect(charCursor)
        if (!rect) { rejected(); return }
        const body = rect.body
        const local = body.mapFromItem(readerColumn, rect.x, rect.y + rect.height / 2)
        const lineStart = charSourceAt(body, rect.verse, body.positionAt(0, local.y), true)
        const lineEndExclusive = charSourceAt(body, rect.verse, body.positionAt(body.width + 1000, local.y), false)
        if (!lineStart || !lineEndExclusive) { rejected(); return }
        let pos = lineStart
        if (edge === "end") {
            pos = bible.textMotion("", lineEndExclusive.verse, Math.max(0, lineEndExclusive.offset - 1))
            // A wrapped line's trailing space is invisible: stop on the last ink.
            while (pos.offset > 0 && /\s/.test(verseText(pos.verse).charAt(pos.offset)) && charCompare(pos, lineStart) > 0)
                pos = bible.textMotion("h", pos.verse, pos.offset, 1)
        } else if (edge === "first") {
            let guard = 0
            while (/\s/.test(verseText(pos.verse).charAt(pos.offset)) && charCompare(pos, lineEndExclusive) < 0 && guard++ < 500) {
                const next = bible.textMotion("l", pos.verse, pos.offset, 1)
                if (next.verse === pos.verse && next.offset === pos.offset) break
                pos = next
            }
        }
        setCharCursor(pos)
    }
    function copyCharSelection(plainText) {
        clearCount()
        if (copySelectedText(plainText)) endCharMode()
        readerFocus()
    }
    // Keys while selecting characters; returns whether the key was used.
    function charKey(event) {
        const mods = event.modifiers & ~Qt.KeypadModifier
        if ([Qt.Key_Shift, Qt.Key_Control, Qt.Key_Alt, Qt.Key_Meta].includes(event.key)) return false
        if (event.key === Qt.Key_Escape) {
            if (charPending.length || verseCount.length) { charPending = ""; clearCount() }
            else { endCharMode(); readerFocus() }
            return true
        }
        if (mods & Qt.ControlModifier) {
            if (event.key === Qt.Key_C && !(mods & Qt.AltModifier)) { copyCharSelection(!!(mods & Qt.ShiftModifier)); return true }
            return false
        }
        if (mods & Qt.AltModifier) return false
        const named = { [Qt.Key_Left]: "h", [Qt.Key_Right]: "l", [Qt.Key_Up]: "k", [Qt.Key_Down]: "j", [Qt.Key_Home]: "0", [Qt.Key_End]: "$" }
        const text = named[event.key] || event.text
        if (!text.length) return false
        if (charPending.length) {
            const pending = charPending
            charPending = ""
            if (pending === "g") {
                if (text === "g") charMotion("gg", false)
                else if (text === "e" || text === "E") charMotion("g" + text)
                else { clearCount(); rejected() }
            } else if (text === "w" || text === "s") charObject(pending + text)
            else { clearCount(); rejected() }
            return true
        }
        if (/^[1-9]$/.test(text) || (text === "0" && verseCount.length)) { countDigit(text); return true }
        switch (text) {
        case "h": case "l": case "w": case "b": case "e": case "W": case "B": case "E": case "(": case ")": charMotion(text); break
        case "j": charLineStep(1); break
        case "k": charLineStep(-1); break
        case "0": charLineEdge("start"); break
        case "^": charLineEdge("first"); break
        case "$": charLineEdge("end"); break
        case "G": charMotion("G", false); break
        case "g": case "i": case "a": charPending = text; break
        case "o": charSwap(); break
        case "y": copyCharSelection(false); break
        case "Y": copyCharSelection(true); break
        case "V": endCharMode(); readerFocus(); break
        case "v": endCharMode(); setVisualMode(true); readerFocus(); break
        default: clearCount(); rejected()
        }
        return true
    }
    function characterHintParts() {
        const ready = charMode && !bible.busy
        return [[["H", "L", "W", "B", "E", "J", "K"], "move"], ["O", "swap", app.charSwap, ready],
            ["Y", "quotation", function() { app.copyCharSelection(false) }, ready],
            ["Shift+Y", "plain text", function() { app.copyCharSelection(true) }, ready],
            ["Escape", "exit", function() { app.endCharMode(); app.readerFocus() }, charMode]]
    }
    function passageMenuEntries(sceneX, sceneY) {
        return [
            menuAction("copy", "Copy quotation", "copy"),
            menuAction("copyPlain", "Copy plain text", "copyPlain"),
            menuDivider,
            menuAction("bookmark", bible.selectionBookmarked ? "Remove bookmark" : "Bookmark", "bookmark"),
            menuAction("highlight", bible.selectionHighlighted ? "Remove highlight" : "Highlight", "highlight"),
            menuAction("note", bible.noteText.length ? "Edit note" : "Write note", "notes"),
            menuAction("tags", "Tags…", "tags"),
            menuDivider,
            menuAction("desk", bible.currentInDesk ? "Remove from Desk" : "Add to Desk", "toggleDesk"),
            menuDivider,
            menuAction("related", "Study", "related"),
            menuAction("chapterFind", "Find in chapter", "chapterFind")
        ].concat(openRangeEntries(sceneX, sceneY))
            .concat(bible.focusMode ? [menuDivider, menuAction("leaveFocus", "Leave focus mode", "focus")] : [])
    }
    // While an annotation range is being moved, the menu offers the session's
    // own actions (the same keys as the status bar), then the copies.
    function rangeEditMenuEntries() {
        const ready = rangeEditor.active && !rangeEditor.visibleModal && !bible.busy
        return [
            { id: "saveRange", text: "Save range", shortcut: KeyNotation.format("Return"), enabled: ready, run: function() { rangeEditor.accept() } },
            { id: "swapEnds", text: "Swap ends", shortcut: KeyNotation.format("O"), enabled: ready, run: function() { rangeEditor.swap() } },
            { id: "cancelRange", text: "Cancel", shortcut: KeyNotation.format("Escape"), enabled: rangeEditor.sessionOpen, run: function() { rangeEditor.cancel() } },
            menuDivider,
            menuAction("copy", "Copy quotation", "copy"),
            menuAction("copyPlain", "Copy plain text", "copyPlain")
        ]
    }
    // Right-click outside the selection selects that verse; inside keeps the range.
    function openPassageMenu(number, sceneX, sceneY) {
        if (!bible.ready || bible.busy) return
        clearCount()
        if (rangeEditor.active) { contextMenu.show("rangeEdit", rangeEditMenuEntries(), sceneX, sceneY); return }
        // Text dragged with the mouse stays selected: the menu leads with copying it.
        if (freeSelectedText.length) {
            contextMenu.show("passage", selectedTextEntries().concat(passageMenuEntries(sceneX, sceneY)), sceneX, sceneY)
            return
        }
        if (!rangeEditing && number > 0) {
            const inside = hasSelection && number >= bible.selectedStart && number <= bible.selectedEnd
            if (!inside) {
                selectVerse(number, false)
                if (bible.selectedStart !== number) return
            }
        }
        contextMenu.show("passage", passageMenuEntries(sceneX, sceneY), sceneX, sceneY)
    }
    function verseMenuPoint(number) {
        const view = reading.mapToItem(null, 0, 0)
        let point = Qt.point(view.x + reading.width / 3, view.y + reading.height / 3)
        const block = blockLoader.item as BlockReader
        if (block) {
            const r = block.verseRect(number)
            point = block.body.mapToItem(null, r.x, r.y + r.height)
        } else {
            const index = Array.from(bible.verses).findIndex(function(v) { return v.number === number })
            const row = index >= 0 ? verseRepeater.itemAt(index) as VerseItem : null
            if (row && row.body) point = row.body.mapToItem(null, 24 * readerScale, Math.min(row.body.height, readerLineHeight))
        }
        return Qt.point(point.x, Math.max(view.y, Math.min(view.y + reading.height - 24 * readerScale, point.y)))
    }
    function openPassageMenuAtCursor() {
        if (!bible.ready || bible.busy) return
        if (rangeEditor.active) {
            const at = verseMenuPoint(bible.cursorVerse)
            contextMenu.show("rangeEdit", rangeEditMenuEntries(), at.x, at.y)
            return
        }
        if (!hasSelection && !freeSelectedText.length) {
            const rows = Array.from(bible.verses)
            const number = bible.cursorVerse > 0 ? bible.cursorVerse : rows.length ? rows[0].number : 0
            if (number > 0) selectVerse(number, false)
        }
        const point = verseMenuPoint(bible.cursorVerse)
        contextMenu.show("passage", (freeSelectedText.length ? selectedTextEntries() : []).concat(passageMenuEntries(point.x, point.y)), point.x, point.y)
    }
    function sceneRowPoint(item) {
        return item ? item.mapToItem(null, 24 * scale, Math.min(item.height, 28 * scale)) : Qt.point(width / 3, height / 3)
    }
    function deskRowFor(scope) {
        return Array.from(bible.desk).find(function(entry) {
            return entry.versionId === scope.versionId && ["book", "chapter", "start", "endBook", "endChapter", "end"].every(function(key) {
                return entry[key] === scope[key]
            })
        })
    }
    function toggleScopeDesk(scope) {
        if (rangeEditing || bible.busy || !scope) { sounds.play("error"); return }
        const existing = deskRowFor(scope)
        if (existing ? bible.removeFromDesk(existing.id) : bible.addToDesk(scope)) {
            const reference = (existing || {}).reference || scope.reference || "Passage"
            notify(reference + (existing ? " removed from Desk" : " added to Desk"), true)
            sounds.play(existing ? "remove" : "chime")
        } else sounds.play("error")
    }
    function copyScope(scope, plainText) {
        if (!scope || bible.busy) { sounds.play("error"); return }
        const copied = bible.copyScope(scope, !!plainText)
        if (copied.length) {
            flashScope(scope)
            notify((scope.reference || "Passage") + (plainText ? " copied as plain text" : " copied"), true)
            sounds.play("chime")
        } else sounds.play("error")
    }
    function removeMarkerAnnotation(scope, kind) {
        if (rangeEditing || bible.busy || !scope || !flushNote()) return
        if (kind === "bookmark") {
            if (!bible.openTaggedPassage(scope)) { sounds.play("error"); return }
            syncNote()
            if (bible.selectionBookmarked) markPassage(false)
        } else if (kind === "tag") {
            const tags = Array.from(bible.tagsForScope(scope))
            let removed = tags.length > 0
            for (const tag of tags) removed = bible.setTag(scope, tag.id, false) && removed
            if (removed) { notify((scope.reference || "Passage") + " tags removed", true); sounds.play("remove") }
            else sounds.play("error")
        } else requestDeleteScope(scope, true)
    }
    // noteOnly deletes just the note on this exact range (marker menu);
    // otherwise every mark, the note, and tags go (Annotations panel).
    function requestDeleteScope(entry, noteOnly) {
        if (rangeEditing || bible.busy || !entry || !flushNote()) return
        const scope = Object.freeze({ versionId: entry.versionId, reference: entry.reference,
            book: entry.book, chapter: entry.chapter, start: entry.start,
            endBook: entry.endBook, endChapter: entry.endChapter, end: entry.end })
        if (!savedScopeExists(scope)) { sounds.play("error"); return }
        deleteConfirmation.scope = scope
        deleteConfirmation.row = -1
        deleteConfirmation.noteOnly = noteOnly === true
        deleteConfirmation.open()
    }
    // Bookmark, note, and tag markers in every layout. One choice targets
    // that range; several overlapping ranges keep Open's chooser and Edit range.
    function openMarkerMenu(choices, kind, openMarker, editScope, sceneX, sceneY) {
        choices = Array.from(choices || [])
        const scope = choices.length === 1 ? choices[0] : null
        const ready = !rangeEditing && !bible.busy
        contextMenu.show("marker", [
            { id: "open", text: "Open", enabled: ready, run: openMarker },
            menuDivider,
            { id: "editRange", text: "Edit range", shortcut: menuKey("editRange"), enabled: ready,
                run: function() { app.openRangeEditor(editScope, kind) } },
            { id: "remove", text: kind === "bookmark" ? "Remove bookmark" : kind === "tag" ? "Remove tags" : "Delete note…",
                enabled: ready && !!scope, run: function() { app.removeMarkerAnnotation(scope, kind) } },
            menuDivider,
            { id: "copy", text: "Copy passage", enabled: !bible.busy && !!scope, run: function() { app.copyScope(scope) } },
            menuDivider,
            { id: "desk", text: scope && deskRowFor(scope) ? "Remove from Desk" : "Add to Desk", enabled: ready && !!scope,
                run: function() { app.toggleScopeDesk(scope) } }
        ], sceneX, sceneY)
    }
    function openDeskMenu(row, sceneX, sceneY) {
        const entry = deskList.model[row]
        if (!entry) return
        deskList.currentIndex = row
        moveDesk(0)
        const id = entry.id
        contextMenu.show("desk", RowMenu.entries(app, { busy: bible.busy, scope: entry,
            open: function() { app.openDeskItem(id) }, canOpen: entry.available, canCopy: entry.available,
            copy: function() { app.copyDeskItem(id) }, copyPlain: function() { app.copyDeskItem(id, true) },
            inDesk: true, desk: function() { app.removeDeskItem(id) } }), sceneX, sceneY)
    }
    function openSavedMenu(row, sceneX, sceneY) {
        const entry = collectionList.model[row]
        if (!entry) return
        collectionList.currentIndex = row
        collectionList.forceActiveFocus()
        const ready = !rangeEditing && !bible.busy
        const kind = ["bookmark", "highlight", "note", "tag"][collectionTab]
        const editRange = { id: "editRange", text: "Edit range", shortcut: menuKey("editRange"), enabled: ready, run: function() { app.openRangeEditor(entry, kind) } }
        const extras = collectionTab === 3 ? [
            { id: "tags", text: "Edit tags…", shortcut: KeyNotation.format("T"), enabled: ready, run: function() { app.openPassageTags(tagsUi.snapshot(entry)) } },
            editRange,
            { id: "removeTag", text: "Remove " + entry.group + "…", shortcut: KeyNotation.format("Delete"), enabled: ready, run: function() { app.requestRemoveTag(entry, row) } }
        ] : [editRange,
            { id: "delete", text: "Delete…", shortcut: KeyNotation.format("Delete"), enabled: ready, run: function() {
                collectionList.currentIndex = row
                collectionList.forceActiveFocus()
                app.requestDeleteSaved()
            } }]
        contextMenu.show("annotation", RowMenu.entries(app, { busy: bible.busy, scope: entry,
            open: function() { app.openSaved(entry) }, extras: extras }), sceneX, sceneY)
    }
    // Palette rows with a passage (search results, recent passages) or a
    // recent search share the row menu; y, shift Y, d type in the input, so
    // the menu shows only enter.
    function paletteRowMenu(entry) {
        if (!entry) return null
        if (entry.recentQuery) {
            const query = entry.recentQuery
            const copy = function() { app.copyText(query, "Search “" + query + "”") }
            return RowMenu.entries(app, { busy: bible.busy, keys: false, desk: false, openText: "Search again",
                open: function() { app.activatePaletteEntry(entry) }, copy: copy, copyPlain: copy })
        }
        if (entry.recent) {
            const scope = historyScope(entry.recent)
            return RowMenu.entries(app, { busy: bible.busy, keys: false, scope: scope,
                open: function() { app.openRecentPassage(entry.recent) } })
        }
        if (paletteTextMode && entry.reference && entry.start)
            return RowMenu.entries(app, { busy: bible.busy, keys: false, scope: entry,
                open: function() { app.acceptBibleResult(entry) } })
        return null
    }
    function openSearchMenu(row, sceneX, sceneY) {
        const entry = paletteResults.model[row]
        const entries = paletteRowMenu(entry)
        if (!entries) return
        paletteResults.currentIndex = row
        paletteArrowUsed = true
        contextMenu.show("search", entries, sceneX, sceneY)
    }
    // Copies plain text that is not a passage (a recent search).
    function copyText(text, label) {
        if (!text.length || bible.busy) { sounds.play("error"); return }
        bible.copyText(text)
        notify(label + " copied", true)
        sounds.play("chime")
    }
    // Panels outside Main (Study) show their rows' menus through here.
    function showContextMenu(kind, entries, sceneX, sceneY) { contextMenu.show(kind, entries, sceneX, sceneY) }
    function mouseNavigate(forward) {
        if (contextMenu.visible) { contextMenu.close(); return }
        if (passagePalette.visible) {
            if (!forward && (paletteBook > 0 || paletteChapter > 0)) backToBooks()
            else rejected()
            return
        }
        if (sidePanel.length && !dialogOpen && !rangeEditing) {
            if (forward) rejected()
            else if (leaveSidePanel("")) readerFocus()
            return
        }
        if (!readerKeys || rangeEditing) { rejected(); return }
        clearCount()
        historyStep(forward ? 1 : -1)
    }
    AppContextMenu {
        id: contextMenu
        uiScale: app.scale
        objectName: "contextMenu"
        theme: desktop
    }
    // The mouse's back and forward buttons, anywhere in the window: a menu
    // closes; the palette steps back toward its book list; an open side panel
    // closes; the reader travels its jumps like [ and ].
    MouseArea {
        objectName: "mouseNavigation"
        parent: Overlay.overlay
        anchors.fill: parent
        z: 2000
        acceptedButtons: Qt.BackButton | Qt.ForwardButton
        onPressed: function(mouse) { app.mouseNavigate(mouse.button === Qt.ForwardButton) }
    }
    MouseArea {
        objectName: "contextMenuOutside"
        parent: Overlay.overlay
        anchors.fill: parent
        z: contextMenu.z - 0.5
        visible: contextMenu.visible
        acceptedButtons: Qt.AllButtons
        onPressed: function(mouse) { mouse.accepted = contextMenu.pressedOutside(mouse.button) }
        onWheel: function(wheel) { wheel.accepted = true }
    }
    function copyClickedVerse(number, plainText) {
        if (rangeEditing) { bible.copySelection(plainText); return }
        if (!flushNote()) return
        const scope = actionScope()
        const inside = hasSelection && number >= bible.selectedStart && number <= bible.selectedEnd
        const first = inside ? bible.selectedStart : number, last = inside ? bible.selectedEnd : number
        const reference = inside ? bible.selectionReference : bible.passage + ":" + number
        // The selection API preserves an inside-hit range. copyVerse focuses an
        // outside hit only after the backend has successfully read/copied it.
        const copied = inside ? bible.copySelection(plainText) : bible.copyVerse(number, plainText)
        if (copied.length) {
            flashVerses(first, last)
            if (inside) completeRangeAction(scope, false)
            if (!inside) selectionAnchor = bible.cursorVerse
            syncNote()
            notify(reference + (plainText ? " copied as plain text" : " copied"))
            sounds.play("chime")
        }
    }
    Timer {
        interval: 16
        repeat: true
        running: app.draggingText
        onTriggered: {
            let edge = 48 * app.readerScale
            let bottom = reading.height - app.floatingSafety
            let speed = app.dragPoint.y < edge ? -Math.min(edge, edge - app.dragPoint.y)
                : app.dragPoint.y > bottom - edge ? Math.min(edge, app.dragPoint.y - bottom + edge) : 0
            if (speed) { app.scrollBy(speed * 0.35); app.applyTextDrag() }
        }
    }

    function clearCount() { verseCount = ""; zPending = false; if (countTimer) countTimer.stop() }
    function countDigit(digit) {
        zPending = false
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
        const from = beginJump()
        selectVerse(number, false)
        endJump(from)
        Qt.callLater(function() { app.scrollToVerse(bible.cursorVerse, false) })
        pulseDestination()
    }

    function tint(color, alpha) { return Qt.rgba(color.r, color.g, color.b, alpha) }
    // Words of Jesus: the importer marks \wj as <span class="wj">. Coloring only
    // swaps that span's style, so glyphs, layout, offsets, and copy never change.
    readonly property string wordsOfJesusColor: bible.wordsOfJesus ? String(desktop.color1) : ""
    function verseHtml(html) {
        if (!wordsOfJesusColor || !html || html.indexOf('<span class="wj">') < 0) return html
        return html.split('<span class="wj">').join('<span class="wj" style="color:' + wordsOfJesusColor + '">')
    }
    function tagsPlainText(tags) { return Array.from(tags || []).map(function(t) { return t.name }).join(", ") }
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
    // Passage links in notes: exact plain-text references (bible.passageLinks)
    // open as History jumps; typing [[ suggests references from the shared
    // fuzzy parser and inserts the full reference, without brackets.
    readonly property var noteLinks: bible.ready ? Array.from(bible.passageLinks(noteEditor.text)) : []
    readonly property var noteLinkAtCursor: {
        const at = noteEditor.cursorPosition
        return noteLinks.find(function(link) { return at >= link.start && at <= link.start + link.length }) || null
    }
    // A web address (http/https) at the note cursor, without trailing punctuation.
    readonly property string noteUrlAtCursor: {
        const at = noteEditor.cursorPosition
        const pattern = /https?:\/\/[^\s<>"'\]\[()]+/g
        let match
        while ((match = pattern.exec(noteEditor.text)) !== null) {
            const url = match[0].replace(/[.,;:!?]+$/, "")
            if (at >= match.index && at <= match.index + url.length) return url
        }
        return ""
    }
    readonly property var linkedFrom: {
        const watched = [bible.collection, bible.selectionReference, bible.passage, bible.noteText]
        return bible.ready && notesOpen && watched ? Array.from(bible.linkedNotes()) : []
    }
    property int linkQueryStart: -1
    property int linkDismissedAt: -1
    property string linkQuery: ""
    property var linkSuggestions: []
    property int linkSuggestionIndex: 0
    property bool acceptingLink: false
    readonly property bool linkSuggesting: linkQueryStart >= 0 && noteEditor.activeFocus && !notePreview
    function updateLinkSuggestions() {
        if (acceptingLink || syncingNote) return
        const text = noteEditor.text, at = noteEditor.cursorPosition
        if (linkDismissedAt >= 0 && text.substr(linkDismissedAt, 2) !== "[[") linkDismissedAt = -1
        const open = at >= 2 ? text.lastIndexOf("[[", at - 2) : -1
        const query = open >= 0 ? text.substring(open + 2, at) : ""
        if (open < 0 || open === linkDismissedAt || /[\[\]\n]/.test(query) || query.length > 40) {
            linkQueryStart = -1; linkQuery = ""; linkSuggestions = []
            return
        }
        linkQueryStart = open
        if (query === linkQuery && linkSuggestions.length) return
        linkQuery = query
        linkSuggestions = query.trim().length ? Array.from(bible.referenceCandidates(query.trim(), 6)) : []
        linkSuggestionIndex = 0
    }
    function moveLinkSuggestion(delta) {
        linkSuggestionIndex = Math.max(0, Math.min(linkSuggestions.length - 1, linkSuggestionIndex + delta))
    }
    function acceptLinkSuggestion(index) {
        const candidate = linkSuggestions[index === undefined ? linkSuggestionIndex : index]
        if (!candidate || linkQueryStart < 0) return false
        const start = linkQueryStart, end = noteEditor.cursorPosition
        acceptingLink = true
        noteEditor.remove(start, end)
        noteEditor.insert(start, candidate.reference)
        noteEditor.cursorPosition = start + candidate.reference.length
        acceptingLink = false
        linkQueryStart = -1; linkQuery = ""; linkSuggestions = []
        flushNote()
        return true
    }
    function dismissLinkSuggestions() {
        linkDismissedAt = linkQueryStart
        linkQueryStart = -1; linkQuery = ""; linkSuggestions = []
    }
    // Markdown preview with each passage link made clickable.
    function linkedMarkdown(text) {
        const links = Array.from(bible.passageLinks(text))
        for (let i = links.length - 1; i >= 0; --i) {
            const link = links[i]
            text = text.slice(0, link.start) + "[" + link.text + "](omaword-passage:" + encodeURIComponent(link.reference) + ")"
                + text.slice(link.start + link.length)
        }
        return text
    }
    function activateNoteLink(link) {
        if (link.startsWith("omaword-passage:")) followPassageLink(decodeURIComponent(link.substring(16)))
        else desktop.openExternal(link)
    }
    // alt enter: a passage link opens in the reader, a web address in the browser.
    function openNoteLinkAtCursor() {
        if (noteLinkAtCursor) return followPassageLink(noteLinkAtCursor.reference)
        if (noteUrlAtCursor.length) { desktop.openExternal(noteUrlAtCursor); return true }
        sounds.play("boundary")
        return false
    }
    // alt v: Write ↔ Preview, from the editor or anywhere in the Notes panel.
    function toggleNotePreview() {
        if (!notesOpen || !hasSelection) { rejected(); return }
        if (notePreview) {
            notePreview = false
            noteEditor.forceActiveFocus()
        } else if (flushNote()) {
            notePreview = true
            notePreviewText.forceActiveFocus()
        }
    }
    // Opens a note's passage link as a History jump; Notes stays open on it.
    function followPassageLink(reference) {
        if (rangeEditing || !flushNote()) return false
        const from = beginJump()
        if (!bible.navigate(reference)) return false
        endJump(from)
        syncNote()
        pulseDestination()
        return true
    }
    function openLinkedNote(row) {
        if (rangeEditing || !row || !flushNote()) return false
        const from = beginJump()
        if (!bible.openTaggedPassage(row)) return false
        endJump(from)
        syncNote()
        pulseDestination()
        return true
    }
    // Notes → Linked from rows: the shared row menu on the linking note's passage.
    function openLinkedMenu(index, sceneX, sceneY) {
        const row = linkedFrom[index]
        if (!row) return
        linkedList.currentIndex = index
        contextMenu.show("linked", RowMenu.entries(app, { busy: bible.busy, scope: row,
            open: function() { app.openLinkedNote(row) } }), sceneX, sceneY)
    }
    // Tab from the note editor: the Linked from list takes the keyboard.
    function focusLinkedFrom() {
        if (!linkedFrom.length) { rejected(); return }
        linkedList.currentIndex = Math.max(0, Math.min(linkedFrom.length - 1, linkedList.currentIndex))
        linkedList.forceActiveFocus()
    }
    // Linked from list keys; `run` false only claims them (ShortcutOverride).
    function linkedKey(event, run) {
        const mods = event.modifiers & ~Qt.KeypadModifier
        const row = linkedFrom[linkedList.currentIndex]
        const plain = mods === Qt.NoModifier
        let action = null
        if (plain && (event.key === Qt.Key_J || event.key === Qt.Key_Down)) action = function() { app.moveLinked(1) }
        else if (plain && (event.key === Qt.Key_K || event.key === Qt.Key_Up)) action = function() { app.moveLinked(-1) }
        else if (plain && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) action = function() { if (row) app.openLinkedNote(row); else app.rejected() }
        else if (plain && event.key === Qt.Key_Y) action = function() { app.copyScope(row) }
        else if (mods === Qt.ShiftModifier && event.key === Qt.Key_Y) action = function() { app.copyScope(row, true) }
        else if (plain && event.key === Qt.Key_D) action = function() { app.toggleScopeDesk(row) }
        else if ((plain && (event.key === Qt.Key_Escape || event.key === Qt.Key_Tab)) || event.key === Qt.Key_Backtab
                || (mods === Qt.ShiftModifier && event.key === Qt.Key_Tab))
            action = function() { noteEditor.forceActiveFocus() }
        else if ((plain && event.key === Qt.Key_Menu) || (mods === Qt.ShiftModifier && event.key === Qt.Key_F10))
            action = function() {
                const item = linkedRepeater.itemAt(linkedList.currentIndex)
                const at = app.sceneRowPoint(item)
                app.openLinkedMenu(linkedList.currentIndex, at.x, at.y)
            }
        if (!action) return false
        if (run) { event.accepted = true; action() }
        return true
    }
    function moveLinked(delta) {
        const next = linkedList.currentIndex + delta
        if (next < 0 || next >= linkedFrom.length) { sounds.play("boundary"); return }
        linkedList.currentIndex = next
    }
    // Scroll keys for scrollable dialogs and panes: j k ↑ ↓ a line, pgup pgdn
    // a page, ctrl d/u half a page, home end the ends. Returns whether it acted.
    function scrollKey(flick, event) {
        if (!flick) return false
        const mods = event.modifiers & ~Qt.KeypadModifier
        const line = 40 * scale, page = Math.max(line, flick.height - line)
        let delta = null
        if (mods === Qt.NoModifier) {
            if (event.key === Qt.Key_J || event.key === Qt.Key_Down) delta = line
            else if (event.key === Qt.Key_K || event.key === Qt.Key_Up) delta = -line
            else if (event.key === Qt.Key_PageDown) delta = page
            else if (event.key === Qt.Key_PageUp) delta = -page
            else if (event.key === Qt.Key_Home) delta = -Infinity
            else if (event.key === Qt.Key_End) delta = Infinity
        } else if (mods === Qt.ControlModifier) {
            if (event.key === Qt.Key_D) delta = page / 2
            else if (event.key === Qt.Key_U) delta = -page / 2
        }
        if (delta === null) return false
        event.accepted = true
        const top = flick.originY, bottom = flick.originY + Math.max(0, flick.contentHeight - flick.height)
        const target = delta === Infinity ? bottom : delta === -Infinity ? top : flick.contentY + delta
        flick.contentY = Math.max(top, Math.min(bottom, target))
        return true
    }
    // Keeps the focused item inside a scrolled pane visible as focus moves (tab).
    function revealFocus(flick) {
        const focused = activeFocusItem
        if (!flick || !focused || !flick.contentItem || flick.height <= 0) return
        let inside = false
        for (let at = focused.parent; at; at = at.parent) if (at === flick.contentItem) { inside = true; break }
        if (!inside) return
        const margin = 12 * scale
        const y = focused.mapToItem(flick.contentItem, 0, 0).y
        const top = flick.originY, bottom = flick.originY + Math.max(0, flick.contentHeight - flick.height)
        let target = flick.contentY
        if (y + focused.height + margin > flick.contentY + flick.height) target = y + focused.height + margin - flick.height
        if (y - margin < target) target = y - margin
        flick.contentY = Math.max(top, Math.min(bottom, target))
    }
    // A key or row that cannot act sounds the error rather than doing nothing.
    function rejected() { sounds.play("error") }
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
        if (rangeEditing) return true
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
    function focusWithin(container) {
        for (let item = activeFocusItem; item; item = item.parent) if (item === container) return true
        return false
    }
    function restorePanelFocus() {
        // Recheck after popup teardown: a dispatched action may have opened a
        // new modal already. Never steal its focus from a delayed close signal.
        Qt.callLater(function() {
            if (app.dialogOpen || bible.busy) return
            // A side panel that replaced another keeps the keyboard.
            if (savedDrawer.shown) { if (!app.focusWithin(savedDrawer.contentItem)) savedDrawer.focusContent(); return }
            if (deskDrawer.shown) { if (!app.focusWithin(deskDrawer.contentItem)) deskDrawer.focusContent(); return }
            if (historyDrawer.shown) { if (!app.focusWithin(historyDrawer.contentItem)) historyDrawer.focusContent(); return }
            if (tagsUi.relatedShown) { if (!tagsUi.focusInBody()) tagsUi.focusResults(); return }
            if (app.modalOpen || bible.busy) return
            if (notesDrawer.shown) {
                if (app.hasSelection && !app.notePreview) noteEditor.forceActiveFocus()
                else if (app.hasSelection) notePreviewText.forceActiveFocus()
                else notesPanel.forceActiveFocus()
            } else app.readerFocus()
        })
    }
    function showFocusedError() {
        if (rangeEditing) return
        if (bible.focusMode && bible.error.length && !bible.busy) {
            errorDialog.message = bible.error
            errorDialog.open()
        }
    }
    // Margins (Margins.js): chapter outline on the left, margin notes on the
    // right. One space computation; notes win and may shift the column left,
    // the outline only takes what is left and never moves it. Both are off in
    // focus mode.
    readonly property real readerSideInset: reading.width < 700 ? 24 : 48
    readonly property real readerRightInset: Math.max(readerSideInset, footerRightInset)
    readonly property real readerColumnWidth: Math.min(760 * readerScale, reading.width - readerSideInset - readerRightInset)
    readonly property real readerNaturalX: Math.min((reading.width - readerColumnWidth) / 2, reading.width - readerRightInset - readerColumnWidth)
    readonly property bool marginsAllowed: bible.ready && !bible.focusMode && !rangeEditing
    // bible.marginNotes as the margins show it: toggling off lags by the notes'
    // fade-out (applyMarginNotes) so they leave before the reader slides back.
    property bool marginNotesOn: false // set from bible.marginNotes on completion, then by applyMarginNotes
    // Chapter notes, one per annotation, with the chapter rows they cover.
    readonly property var marginNoteEntries: {
        if (!marginsAllowed || !marginNotesOn) return []
        const byKey = {}, list = []
        for (const row of Array.from(bible.verses)) {
            for (const range of Array.from(row.noteRanges || [])) {
                const key = [range.book, range.chapter, range.start, range.endBook, range.endChapter, range.end].join(":")
                let entry = byKey[key]
                if (!entry) {
                    entry = byKey[key] = { key: key, reference: range.reference, note: range.note || "", scope: range,
                        first: row.number, last: row.number, continuesBefore: !!range.continuesBefore }
                    list.push(entry)
                }
                entry.last = row.number
                entry.continuesAfter = !!range.continuesAfter
            }
        }
        return list
    }
    readonly property var outlineHeadings: !marginsAllowed || !bible.chapterOutline ? []
        : Array.from(bible.verses).filter(function(row) { return (row.heading || "").length > 0 })
            .map(function(row) { return { verse: row.number, text: row.heading } })
    // Connector: "}" (braceWidth) after the text's right edge, an arrow, and a
    // "{" before the note text.
    readonly property real marginBraceWidth: 9 * readerScale
    readonly property real marginTextRight: marginSpace.columnX + readerColumnWidth - 9
    readonly property var marginSpace: Margins.space({
        width: reading.width, columnX: readerNaturalX, columnWidth: readerColumnWidth,
        minLeft: readerSideInset, leftInset: 24 * readerScale, rightInset: Math.max(28 * readerScale, footerRightInset),
        wantNotes: marginNoteEntries.length > 0, connectorWidth: 3 * readerScale + 2 * marginBraceWidth + 34 * readerScale,
        notesMin: 170 * readerScale, notesPreferred: 250 * readerScale, notesMax: 330 * readerScale,
        wantOutline: outlineHeadings.length > 0, outlineGap: 12 * readerScale,
        outlineMin: 120 * readerScale, outlineMax: 220 * readerScale
    })
    // While margin notes show for this chapter, the reader drops its inline
    // note marks (Verse rails, Paragraph/Block brackets); other kinds stay.
    readonly property bool inlineNoteMarks: !marginSpace.notes.visible
    readonly property real marginNotePixelSize: Math.round(readerSize * readerScale * 0.8)
    readonly property real marginNoteLineHeight: Math.round(marginNotePixelSize * 1.45)
    // Content-space verse spans {number, top, bottom} (reader column coordinates)
    // and the placed notes, refreshed after layout changes.
    property var verseExtents: []
    property var marginLayout: []
    Timer { id: marginTimer; interval: 0; onTriggered: app.updateMargins() }
    onMarginSpaceChanged: scheduleMargins()
    onMarginNoteEntriesChanged: scheduleMargins()
    onMarginNoteLineHeightChanged: scheduleMargins()
    Connections {
        target: bible
        function onVersesChanged() { app.scheduleMargins() }
        function onMarginNotesChanged() { app.applyMarginNotes(bible.marginNotes) }
    }
    // Toggling margin notes slides the reader to make or return room, and fades
    // the notes in/out; resizes, chapter changes, and Disable animations jump.
    readonly property int marginSlideMs: reduceMotion ? 0 : 140
    property real marginSlideOffset: 0
    NumberAnimation { id: marginSlide; objectName: "marginSlide"; target: app; property: "marginSlideOffset"; to: 0; duration: app.marginSlideMs; easing.type: Easing.OutCubic }
    NumberAnimation { id: marginFade; target: marginLayer; property: "opacity"; duration: app.marginSlideMs; easing.type: Easing.OutCubic
        onFinished: if (marginLayer.opacity === 0) app.slideMarginsTo(false) }
    function slideMarginsTo(on) {
        const before = marginSpace.columnX + marginSlideOffset
        marginNotesOn = on
        marginSlide.stop()
        marginSlideOffset = reduceMotion ? 0 : before - marginSpace.columnX
        if (marginSlideOffset !== 0) marginSlide.start()
    }
    function applyMarginNotes(on) {
        marginFade.stop()
        if (on === marginNotesOn && !(on && marginLayer.opacity < 1)) return
        if (reduceMotion || !marginSpace.notes.visible && !on) {
            marginLayer.opacity = 1; slideMarginsTo(on); return
        }
        if (on) {
            if (!marginNotesOn) marginLayer.opacity = 0
            slideMarginsTo(true)
            marginFade.to = 1; marginFade.start()
        } else { marginFade.to = 0; marginFade.start() }
    }
    Connections {
        target: reading
        function onHeightChanged() { app.scheduleMargins() }
    }
    function scheduleMargins() { if (readerInitialized) marginTimer.restart() }
    function measureVerses() {
        const rows = Array.from(bible.verses)
        const block = blockLoader.item as BlockReader
        const out = []
        if (block) {
            const offset = block.body.mapToItem(readerColumn, 0, 0).y
            for (const extent of block.verseExtents())
                out.push({ number: extent.number, top: offset + extent.top, bottom: offset + extent.bottom })
        } else {
            for (let i = 0; i < rows.length; ++i) {
                const item = verseRepeater.itemAt(i) as VerseItem
                if (!item) continue
                out.push({ number: rows[i].number, top: item.y + item.textOffset, bottom: item.y + item.textOffset + item.textHeight })
            }
        }
        return out
    }
    // Margins measure only while one is shown; `{`/`}` measure on demand.
    readonly property bool marginsMeasured: marginSpace.notes.visible || marginSpace.outline.visible
    onMarginsMeasuredChanged: scheduleMargins()
    function updateMargins() {
        if (!bible.ready || !marginsMeasured) {
            if (verseExtents.length) verseExtents = []
            if (marginLayout.length) marginLayout = []
            return
        }
        readerColumn.forceLayout()
        const extents = measureVerses()
        verseExtents = extents
        if (!marginSpace.notes.visible) { if (marginLayout.length) marginLayout = []; return }
        const entries = marginNoteEntries
        const span = function(first, last) {
            const a = extents.find(function(e) { return e.number === first }), z = extents.find(function(e) { return e.number === last })
            return a && z ? { top: a.top, bottom: z.bottom } : null
        }
        const items = [], keys = []
        for (let i = 0; i < entries.length; ++i) {
            const passage = span(entries[i].first, entries[i].last)
            const delegate = marginRepeater.itemAt(i)
            if (!passage || !delegate) continue
            items.push({ top: passage.top, bottom: passage.bottom, lines: Math.max(1, delegate.naturalLines), lineHeight: marginNoteLineHeight, pad: 0 })
            keys.push(entries[i])
        }
        const placed = Margins.place(items, {
            viewport: Math.max(1, reading.height - floatingSafety), gap: Math.round(marginNoteLineHeight * 0.6),
            upMax: marginNoteLineHeight, bounds: { top: 0, bottom: readerColumn.height }
        })
        // Paragraph layouts share lines between neighbouring passages; their
        // "}" braces split such a shared line instead of overlapping.
        const braces = items.map(function(item) { return { top: item.top, bottom: item.bottom } })
        const byTop = braces.map(function(brace, i) { return i }).sort(function(a, b) { return braces[a].top - braces[b].top || a - b })
        for (let n = 1; n < byTop.length; ++n) {
            const previous = braces[byTop[n - 1]], current = braces[byTop[n]]
            if (current.top < previous.bottom && current.top > previous.top && current.bottom > previous.bottom) {
                const split = (current.top + previous.bottom) / 2
                previous.bottom = split; current.top = split
            }
        }
        marginLayout = placed.map(function(place, i) {
            const note = { top: place.y, bottom: place.y + place.height }
            const passage = braces[i]
            return Object.assign({ key: keys[i].key, reference: keys[i].reference, passageTop: passage.top, passageBottom: passage.bottom,
                naturalLines: items[i].lines, tips: Margins.tips(note, passage, { inset: 3 * readerScale, noteLines: place.lines, passageLine: readerLineHeight }) }, place)
        })
    }
    function marginPlacement(key) { return marginLayout.find(function(place) { return place.key === key }) || null }
    // Plain note text with exact passage links (as in the Notes preview).
    function marginNoteHtml(text) {
        const escape = function(part) { return part.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/\n/g, "<br>") }
        const links = Array.from(bible.passageLinks(text)).sort(function(a, b) { return a.start - b.start })
        let out = "", at = 0
        for (const link of links) {
            if (link.start < at) continue
            out += escape(text.slice(at, link.start)) + '<a href="omaword-passage:' + encodeURIComponent(link.reference) + '">' + escape(link.text) + "</a>"
            at = link.start + link.length
        }
        return out + escape(text.slice(at))
    }
    function openMarginNote(entry) {
        if (!entry || modalOpen || bible.busy) return
        openNoteScope(entry.scope)
    }
    function followMarginLink(link) {
        if (modalOpen || bible.busy) return
        if (link.startsWith("omaword-passage:")) jump(decodeURIComponent(link.substring(16)))
        else desktop.openExternal(link)
    }
    // The section being read: the cursor's while it is on screen, otherwise
    // the one at a reading line a quarter down the viewport.
    function readingVerse() {
        const available = Math.max(1, reading.height - floatingSafety)
        const top = reading.contentY, bottom = top + available
        const extents = verseExtents
        if (hasSelection && !bible.crossChapterSelection) {
            const cursor = extents.find(function(e) { return e.number === bible.cursorVerse })
            if (cursor && cursor.bottom > top && cursor.top < bottom) return bible.cursorVerse
        }
        const line = top + 0.25 * available
        let verse = bible.verses.length ? bible.verses[0].number : 0
        for (const extent of extents) if (extent.top <= line) verse = extent.number
        return verse
    }
    readonly property int outlineCurrent: {
        const watch = [reading.contentY, verseExtents, bible.cursorVerse, bible.selectedStart]
        return watch && outlineHeadings.length ? Margins.sectionOf(outlineHeadings, readingVerse()) : 0
    }
    // `{` / `}`: previous/next section heading (counts), a History jump. `{`
    // ends at the chapter top (its first verse, scrolled to the top), also in
    // chapters and versions without headings.
    function headingStep(direction, count) {
        const headings = !bible.hasHeadings ? [] : Array.from(bible.verses)
            .filter(function(row) { return (row.heading || "").length > 0 })
            .map(function(row) { return { verse: row.number } })
        if (direction > 0 && !headings.length) {
            sounds.play("boundary")
            notify(bible.hasHeadings ? "No headings in " + bible.passage
                : bible.versionAbbreviation + " has no section headings", true)
            return
        }
        if (!marginsMeasured) { readerColumn.forceLayout(); verseExtents = measureVerses() }
        const from = hasSelection && !bible.crossChapterSelection ? bible.cursorVerse : readingVerse()
        const top = bible.verses.length ? bible.verses[0].number : 0
        const step = Margins.headingStep(headings, from, direction, count, top)
        if (step.moved) jumpToHeading(step.verse)
        // Scrolled away from the top while the first verse is current.
        else if (direction < 0 && from === top && reading.contentY > 1) jumpToHeading(top)
        else if (direction < 0) {
            sounds.play("boundary")
            notify("Already at the top of " + bible.passage, true)
        }
        if (direction > 0 && step.short) {
            sounds.play("boundary")
            notify("No more headings below", true)
        }
    }
    function jumpToHeading(verse) {
        if (rangeEditing || !bible.ready || !flushNote()) return
        const jump = beginJump()
        selectVerse(verse, false)
        if (bible.cursorVerse !== verse) return
        endJump(jump)
        Qt.callLater(function() { app.scrollToVerse(verse, true) })
        pulseDestination()
    }
    property bool toastInFocus: false
    function notify(message, inFocus) { toastInFocus = !!inFocus; toast.text = message; toastTimer.restart() }
    // Footnote markers (all layouts) open Study → Footnotes at that note,
    // selecting its verse first (Study is a passage panel).
    function openFootnote(verse, seq) {
        if (!bible.ready || bible.busy || rangeEditing || modalOpen) return
        if (bible.selectedStart !== verse || bible.selectedEnd !== verse || bible.crossChapterSelection) selectVerse(verse, false)
        if (bible.cursorVerse !== verse) return
        if (sidePanel === "related") { tagsUi.setStudyTab("footnotes", { verse: verse, seq: seq }); return }
        if (leaveSidePanel("related")) tagsUi.openStudy("footnotes", { verse: verse, seq: seq })
    }
    function footnoteMark(caller) { return /^[A-Za-z0-9]{1,3}$/.test(caller || "") ? caller : "\u2020" }
    // Verse rows reserve footnote cells like BlockDocument: a word joiner and a
    // transparent image at each note's plain-text offset. Paint-only markers
    // sit over them; copy strips the cells, search maps around them.
    readonly property real footnoteCellWidth: Math.max(1, Math.round(readerSize * readerScale * 0.5))
    function footnoteHtml(html, notes) {
        if (!notes || !notes.length || bible.focusMode) return html
        const offsets = Array.from(notes).map(function(note) { return note.offset }).sort(function(a, b) { return a - b })
        const cell = '&#8288;<img src="qrc:/resources/cell.png" width="' + footnoteCellWidth + '" height="1">'
        let out = "", count = 0, next = 0, i = 0
        while (i < html.length) {
            while (next < offsets.length && offsets[next] <= count) { out += cell; ++next }
            const c = html[i]
            if (c === "<") {
                const end = html.indexOf(">", i)
                const tag = end < 0 ? html.substring(i) : html.substring(i, end + 1)
                out += tag; i += tag.length
                if (/^<br\s*\/?>$/i.test(tag)) ++count
                continue
            }
            if (c === "&") {
                const end = html.indexOf(";", i)
                const entity = end < 0 || end - i > 10 ? c : html.substring(i, end + 1)
                out += entity; i += entity.length; ++count
                continue
            }
            out += c; ++i; ++count
        }
        while (next++ < offsets.length) out += cell
        return out
    }
    // Document position of the k-th (offset-sorted) footnote cell's image.
    function footnoteCellPosition(notes, note) {
        const sorted = Array.from(notes).slice().sort(function(a, b) { return a.offset - b.offset || a.seq - b.seq })
        const k = sorted.findIndex(function(entry) { return entry.seq === note.seq })
        return note.offset + 2 * k + 1
    }
    function selectVerse(number, extend) {
        if (rangeEditor.active) {
            const rows = Array.from(bible.verses)
            const from = rows.findIndex(function(row) { return row.number === bible.cursorVerse })
            const to = rows.findIndex(function(row) { return row.number === number })
            if (from >= 0 && to >= 0) moveVerse(to - from, true)
            return
        }
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
    // Holding a verse motion repeats it immediately after a brief tap window,
    // instead of waiting for the platform's (often ~600 ms) repeat delay.
    readonly property var heldKeys: ({ nextVerse: [Qt.Key_J, Qt.Key_Down], previousVerse: [Qt.Key_K, Qt.Key_Up],
        extendNext: [Qt.Key_J, Qt.Key_Down], extendPrevious: [Qt.Key_K, Qt.Key_Up],
        previousChapter: [Qt.Key_H, Qt.Key_Left], nextChapter: [Qt.Key_L, Qt.Key_Right] })
    readonly property var chapterActions: ["previousChapter", "nextChapter"]
    property string holdAction: ""
    property int holdKey: 0
    property int holdPress: 0
    property real holdTickAt: 0
    property bool holdRepeated: false
    property real holdStartedAt: 0
    function beginHold(action) {
        const keys = heldKeys[action]
        if (!keys) return
        const key = keys.find(function(candidate) { return keyHold.isDown(candidate) })
        if (key === undefined) return
        holdAction = action; holdKey = key; holdPress = keyHold.press(key)
        holdStartedAt = holdTickAt = Date.now()
        holdRepeated = false
        holdTimer.interval = 150
        holdTimer.restart()
    }
    function endHold() {
        holdTimer.stop()
        holdAction = ""; holdKey = 0; holdPress = 0; holdRepeated = false
    }
    // qmllint disable import
    KeyHold { id: keyHold; onReleased: function(key) { if (key === app.holdKey) app.endHold() } }
    // qmllint enable import
    Timer {
        id: holdTimer
        repeat: true
        onTriggered: {
            if (!app.active || !app.readerKeys || keyHold.press(app.holdKey) !== app.holdPress) { app.endHold(); return }
            // "Down" only means no release has been processed yet. After the
            // GUI thread stalls (a heavy chapter render), this overdue tick can
            // run before a release still queued behind it, so a late tick
            // never repeats: it restarts the hold window, letting the loop
            // drain input first. A tap must never repeat. Once repeating, the
            // repeats themselves stall (chapter steps), so a late tick only
            // yields one short pass to pending input before continuing.
            const now = Date.now(), late = now - app.holdTickAt > interval + 50
            app.holdTickAt = now
            if (late && !app.holdRepeated) { app.holdStartedAt = now; return }
            if (late) { interval = 10; return }
            // The animation-driven Timer can also fire early after a long frame.
            if (!app.holdRepeated && now - app.holdStartedAt < 140) return
            interval = app.chapterActions.includes(app.holdAction) ? 80 : 33
            app.holdRepeated = true
            app.runCommand(app.holdAction)
            // The repeat's own work is not a stall.
            app.holdTickAt = Date.now()
        }
    }
    onActiveChanged: if (!active) endHold()
    function moveVerse(delta, extend, bulk) {
        if (rangeEditor.active) extend = true
        clearCount()
        if (!flushNote()) return
        clearTextSelection()
        positionTimer.stop()
        restoringPosition = true
        movingSelection = true
        const fromChapter = bible.book + ":" + bible.chapter
        if (bulk) bible.moveByVerses(delta, extend)
        else bible.moveSelection(delta, extend)
        movingSelection = false
        syncNote()
        selectionAnchor = bible.crossChapterSelection ? 0
            : bible.cursorVerse === bible.selectedStart ? bible.selectedEnd : bible.selectedStart
        const movedIdentity = selectionIdentity
        Qt.callLater(function() {
            if (app.selectionIdentity !== movedIdentity) return
            readerColumn.forceLayout()
            // Crossing into another chapter has nothing to slide from.
            app.placeVerse(bible.cursorVerse, false, bible.book + ":" + bible.chapter !== fromChapter)
            app.restoringPosition = false
            app.savePosition()
        })
    }
    function setVisualMode(enabled) {
        if (rangeEditing) return false
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
        if (rangeEditor.active) { moveVerse(delta, true); return }
        if (!verseMode && !setVisualMode(true)) return
        moveVerse(delta, true)
    }
    function chapterEdge(last) {
        clearCount()
        if (!flushNote() || bible.verses.length === 0) return
        let number = bible.verses[last ? bible.verses.length - 1 : 0].number
        const from = beginJump()
        selectVerse(number, false)
        endJump(from)
        Qt.callLater(function() { app.scrollToVerse(bible.cursorVerse, false) })
    }
    function chapterStep(delta) {
        if (rangeEditing) return
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
        if (current >= 0 && target !== current) bible.openChapter(chapters[target].book, chapters[target].chapter)
        readerFocus()
    }
    function jump(reference) {
        if (rangeEditing) return
        if (!flushNote()) return
        const from = beginJump()
        if (bible.navigate(reference)) {
            endJump(from)
            passagePalette.navigated = true
            passagePalette.close()
            readerFocus()
            pulseDestination()
        }
    }
    function openChapter(book, chapter) {
        if (rangeEditing) return
        if (!flushNote()) return
        const from = beginJump()
        bible.openChapter(book, chapter)
        positionTimer.stop()
        setReaderY(0)
        restoringPosition = false
        savePosition()
        endJump(from)
        passagePalette.navigated = true
        passagePalette.close()
        readerFocus()
    }
    function openPalette(commands, resume) {
        if (rangeEditing) return
        clearCount()
        if (!flushNote()) return
        if (!resume) {
            resetBooks()
            referenceInput.text = commands || !bible.ready ? "> " : ""
            paletteCleared = false
            rememberPalette()
        } else if (paletteSnapshot) {
            const saved = paletteSnapshot
            paletteBookName = paletteSnapshot.name
            paletteBook = paletteSnapshot.book
            paletteChapter = paletteSnapshot.chapter
            referenceInput.text = paletteSnapshot.query
            paletteArrowUsed = paletteSnapshot.arrow
            paletteResults.currentIndex = paletteSnapshot.row
            paletteResults.positionViewAtIndex(paletteSnapshot.row, ListView.Contain)
            if (paletteTextMode) {
                pendingSearchResume = saved
                Qt.callLater(app.restoreSearchPalette)
            }
        }
        paletteCleared = false
        passagePalette.open()
        referenceInput.forceActiveFocus()
    }
    function rememberPalette() {
        paletteSnapshot = { query: referenceInput.text, book: paletteBook, chapter: paletteChapter,
            name: paletteBookName, row: paletteResults.currentIndex, arrow: paletteArrowUsed,
            position: paletteResults.contentY, generation: bible.textGeneration }
    }
    // One esc closes the palette in any mode; space resumes it, query and all.
    function escapePalette() { passagePalette.close() }
    function paletteMode() { return referenceInput.text.trim().startsWith(">") ? "commands" : paletteTextMode ? "search" : "passage" }
    // A different mode's key switches the open palette; its own key does nothing.
    function showPaletteMode(mode) {
        if (passagePalette.visible && paletteMode() === mode) return
        sounds.play("tick")
        if (mode === "search") openTextSearch()
        else openPalette(mode === "commands")
    }
    function openNotes(focusEditor) {
        if (rangeEditing) return
        if (!bible.ready || bible.busy) return
        clearCount()
        if (!notesOpen) openedNoteActionScope = actionScope()
        notesDrawer.focusEditor = focusEditor
        notesDrawer.open()
        if (loadedNoteReference !== bible.selectionReference) syncNote()
        if (focusEditor && hasSelection) {
            notePreview = false
            if (notesDrawer.shown) focusNoteEnd()
        }
    }
    function focusNoteEnd() { noteEditor.forceActiveFocus(); noteEditor.cursorPosition = noteEditor.length }
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
        if (rangeEditing) return
        if (!flushNote()) return
        restoreConfirmation.backupFile = file
        restoreConfirmation.open()
    }
    function confirmRestore() {
        if (rangeEditing) return
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
        const first = hasSelection ? bible.selectedStart : 0, last = bible.selectedEnd
        if (bible.copySelection(!!plainText).length > 0) {
            if (first > 0) flashVerses(first, last); else flashChapter()
            completeRangeAction(scope, false)
            notify(reference + (plainText ? " copied as plain text" : " copied"))
            sounds.play("chime")
        }
    }
    function markPassage(highlight) {
        if (rangeEditing) return
        if (!hasSelection || !flushNote()) return
        const scope = actionScope()
        const success = highlight ? bible.toggleHighlight() : bible.toggleBookmark()
        if (success) {
            const added = highlight ? bible.selectionHighlighted : bible.selectionBookmarked
            completeRangeAction(scope, false)
            sounds.play(added ? "chime" : "remove")
        } else sounds.play("error")
    }
    // Desk: one global study list shared by every window and version.
    property int deskSelectedId: -1
    property int deskRow: 0
    function toggleDeskPassage() {
        if (rangeEditing) return
        if (!hasSelection || !flushNote()) return
        const scope = actionScope()
        const reference = bible.selectionReference
        if (bible.toggleDesk()) {
            const added = bible.currentInDesk
            completeRangeAction(scope, false)
            notify(reference + (added ? " added to Desk" : " removed from Desk"))
            sounds.play(added ? "chime" : "remove")
        } else sounds.play("error")
    }
    function openDesk() {
        if (rangeEditing || deskDrawer.shown) return
        deskDrawer.open()
    }
    function moveDesk(delta) {
        if (deskList.count > 0) {
            deskList.currentIndex = Math.max(0, Math.min(deskList.count - 1, deskList.currentIndex + delta))
            deskList.positionViewAtIndex(deskList.currentIndex, ListView.Contain)
            deskRow = deskList.currentIndex
            deskSelectedId = deskList.model[deskRow].id
        } else { deskRow = 0; deskSelectedId = -1 }
        deskList.forceActiveFocus()
    }
    function restoreDeskRow() {
        const rows = Array.from(bible.desk)
        let row = rows.findIndex(function(entry) { return entry.id === deskSelectedId })
        if (row < 0) row = Math.min(deskRow, rows.length - 1)
        deskList.currentIndex = row
        deskRow = Math.max(0, row)
        deskSelectedId = row >= 0 ? rows[row].id : -1
    }
    function deskEntry(id) { return Array.from(bible.desk).find(function(entry) { return entry.id === id }) }
    function openDeskItem(id) {
        if (rangeEditing || bible.busy) return
        const entry = deskEntry(id)
        if (!entry || !flushNote()) return
        if (!entry.available) { sounds.play("error"); notify(entry.versionAbbreviation + " is not installed", true); return }
        const from = beginJump()
        if (!bible.openDeskItem(id)) return
        endJump(from)
        syncNote()
        deskDrawer.close()
        readerFocus()
        Qt.callLater(function() { app.scrollToVerse(bible.cursorVerse, true) })
        pulseDestination()
    }
    function copyDeskItem(id, plainText) {
        if (rangeEditing || bible.busy) return
        const entry = deskEntry(id)
        if (!entry) return
        if ((plainText ? bible.copyScope(entry, true) : bible.copyDeskItem(id)).length > 0) {
            notify(entry.reference + " · " + entry.versionAbbreviation + (plainText ? " copied as plain text" : " copied"), true)
            sounds.play("chime")
        } else sounds.play("error")
    }
    function removeDeskItem(id) {
        if (rangeEditing || bible.busy) return
        const row = Array.from(bible.desk).findIndex(function(entry) { return entry.id === id })
        if (row < 0) return
        if (bible.removeFromDesk(id)) {
            deskSelectedId = -1
            deskRow = row
            restoreDeskRow()
            sounds.play("remove")
        }
        if (deskDrawer.shown) deskList.forceActiveFocus()
    }
    function removeCurrentDeskItem() {
        const entry = deskList.model[deskList.currentIndex]
        if (entry) removeDeskItem(entry.id)
    }
    function requestCleanDesk() {
        if (rangeEditing || bible.busy) return
        if (!bible.desk.length) { notify("The Desk is already clean", true); return }
        cleanDeskConfirmation.open()
    }
    function confirmCleanDesk() {
        if (rangeEditing || bible.busy || !flushNote()) return
        const count = bible.desk.length
        if (bible.cleanDesk()) {
            cleanDeskConfirmation.close()
            sounds.play("remove")
            notify("Cleaned the Desk · " + count + (count === 1 ? " passage removed" : " passages removed"), true)
        }
    }
    // History. Each window keeps an in-memory jump stack (never saved): every
    // jump records instantly; verse and chapter steps do not. [ and ] travel it
    // like a browser; - swaps with the previous jump. The shared, persistent
    // History list only records a location once it is kept for historyDwellMs.
    property var jumpStack: []
    property int jumpIndex: -1
    // Where this window was before its latest jump or travel: - returns there.
    property int jumpPrevious: -1
    property int historyDwellMs: 5000
    property string dwellChapterKey: ""
    property bool dwellPending: false
    property int historySelectedId: -1
    property int historyRow: 0
    readonly property var locationKeys: ["versionId", "book", "chapter", "start", "endBook", "endChapter", "end"]
    function sameLocation(a, b) {
        return !!a && !!b && locationKeys.every(function(key) { return a[key] === b[key] })
    }
    function locationNow() {
        if (!bible.ready) return null
        savePosition()
        const location = bible.currentLocation()
        return location && location.versionId ? location : null
    }
    function beginJump() { return locationNow() }
    function endJump(from) {
        const to = bible.ready ? bible.currentLocation() : null
        if (!from || !to || !to.versionId || sameLocation(from, to)) return
        let stack = jumpStack.slice(0, jumpIndex + 1)
        if (stack.length) stack[stack.length - 1] = from
        else stack.push(from)
        stack.push(to)
        if (stack.length > 200) stack = stack.slice(stack.length - 200)
        jumpStack = stack
        jumpIndex = stack.length - 1
        jumpPrevious = jumpIndex - 1
        restartDwell()
    }
    function restartDwell() {
        dwellChapterKey = bible.ready ? bible.versionId + ":" + bible.book + ":" + bible.chapter : ""
        dwellPending = bible.ready
        if (bible.ready && bible.historyEnabled) historyDwellTimer.restart()
        else historyDwellTimer.stop()
    }
    function recordVisit() {
        if (!bible.ready || !bible.historyEnabled || bible.busy) return
        dwellPending = false
        savePosition()
        bible.recordHistory()
    }
    // Opens a saved location (jump stack or History row) with its scroll position.
    function openLocation(location) {
        if (!location || !flushNote()) return false
        if (!bible.openLocation(location)) {
            sounds.play("error")
            notify(bible.error || "That passage is no longer available", true)
            return false
        }
        clearTextSelection()
        selectionAnchor = bible.crossChapterSelection ? 0 : bible.selectedStart
        syncNote()
        restorePosition(false)
        readerFocus()
        pulseDestination()
        restartDwell()
        return true
    }
    function travelJump(target) {
        if (rangeEditing || target < 0 || target >= jumpStack.length || target === jumpIndex) return false
        if (!flushNote()) return false
        const stack = jumpStack.slice()
        const here = locationNow()
        if (here) stack[jumpIndex] = here
        if (!openLocation(stack[target])) return false
        jumpStack = stack
        jumpPrevious = jumpIndex
        jumpIndex = target
        return true
    }
    function historyStep(delta) {
        const target = Math.max(0, Math.min(jumpStack.length - 1, jumpIndex + delta))
        if (!jumpStack.length || target === jumpIndex) {
            notify(delta < 0 ? "No earlier jump" : "No later jump", true)
            return
        }
        travelJump(target)
    }
    // Swaps with the location before the latest jump or [ ] step; repeating flips back.
    function historySwap() {
        if (jumpPrevious < 0 || jumpPrevious >= jumpStack.length || jumpPrevious === jumpIndex) { notify("No previous jump", true); return }
        travelJump(jumpPrevious)
    }
    // The jump stack position as a History row: the newest saved visit of the
    // current stack location.
    readonly property int historyCurrentId: {
        const here = jumpIndex >= 0 ? jumpStack[jumpIndex] : null
        const rows = Array.from(bible.history)
        const current = here ? rows.find(function(row) { return app.sameLocation(row, here) }) : null
        return current ? current.id : -1
    }
    function openHistory() {
        if (rangeEditing || historyDrawer.shown) return
        if (!bible.historyEnabled) { notify("History is disabled · enable it in Commands", true); sounds.play("error"); return }
        historyDrawer.open()
    }
    function historyEntry(id) { return Array.from(bible.history).find(function(entry) { return entry.id === id }) }
    function moveHistory(delta) {
        if (historyList.count > 0) {
            historyList.currentIndex = Math.max(0, Math.min(historyList.count - 1, historyList.currentIndex + delta))
            historyList.positionViewAtIndex(historyList.currentIndex, ListView.Contain)
            historyRow = historyList.currentIndex
            historySelectedId = historyList.model[historyRow].id
        } else { historyRow = 0; historySelectedId = -1 }
        historyList.forceActiveFocus()
    }
    function restoreHistoryRow() {
        const rows = Array.from(bible.history)
        let row = rows.findIndex(function(entry) { return entry.id === historySelectedId })
        if (row < 0) row = Math.min(historyRow, rows.length - 1)
        historyList.currentIndex = row
        historyRow = Math.max(0, row)
        historySelectedId = row >= 0 ? rows[row].id : -1
    }
    function openHistoryItem(id) {
        if (rangeEditing || bible.busy) return
        const entry = historyEntry(id)
        if (!entry || !flushNote()) return
        if (!entry.available) { sounds.play("error"); notify(entry.versionAbbreviation + " is not installed", true); return }
        const from = beginJump()
        if (!openLocation(entry)) return
        endJump(from)
        historyDrawer.close()
        readerFocus()
    }
    function copyHistoryItem(id, plainText) {
        if (rangeEditing || bible.busy) return
        const entry = historyEntry(id)
        if (!entry) return
        const scope = historyScope(entry)
        if (!scope) { sounds.play("error"); return }
        if (bible.copyScope(scope, !!plainText).length > 0) {
            notify(entry.reference + " · " + entry.versionAbbreviation + (plainText ? " copied as plain text" : " copied"), true)
            sounds.play("chime")
        } else sounds.play("error")
    }
    function historyScope(entry) {
        if (!entry || entry.start) return entry
        return entry.available && entry.firstVerse ? Object.assign({}, entry, { start: entry.firstVerse, end: entry.lastVerse }) : null
    }
    function toggleHistoryDesk(id) {
        if (rangeEditing || bible.busy) return
        const entry = historyEntry(id)
        const scope = historyScope(entry)
        if (!scope || !entry.available) { sounds.play("error"); return }
        toggleScopeDesk(scope)
        if (historyDrawer.shown) historyList.forceActiveFocus()
    }
    function removeHistoryItem(id) {
        if (rangeEditing || bible.busy) return
        const row = Array.from(bible.history).findIndex(function(entry) { return entry.id === id })
        if (row < 0) return
        if (bible.removeHistoryItem(id)) {
            historySelectedId = -1
            historyRow = row
            restoreHistoryRow()
            sounds.play("remove")
        }
        if (historyDrawer.shown) historyList.forceActiveFocus()
    }
    function currentHistoryEntry() { return historyList.model[historyList.currentIndex] }
    function requestClearHistory() {
        if (rangeEditing || bible.busy) return
        if (!bible.history.length && !bible.recentQueries.length) { notify("History is already empty", true); return }
        clearHistoryConfirmation.open()
    }
    function confirmClearHistory() {
        if (rangeEditing || bible.busy) return
        const count = bible.history.length
        if (bible.clearHistory()) {
            clearHistoryConfirmation.close()
            sounds.play("remove")
            notify("Cleared History · " + count + (count === 1 ? " passage removed" : " passages removed"), true)
        }
    }
    function toggleHistoryEnabled() {
        const enable = !bible.historyEnabled
        if (!enable && historyDrawer.shown) historyDrawer.close()
        if (!bible.setHistoryEnabled(enable)) { sounds.play("error"); return }
        restartDwell()
        notify(enable ? "History enabled" : "History disabled · cleared", true)
        sounds.play("tick")
    }
    // Today, Yesterday, Earlier by local calendar day.
    function historyGroup(visitedAt) {
        const visited = new Date(visitedAt)
        const today = new Date(); today.setHours(0, 0, 0, 0)
        const day = new Date(visited); day.setHours(0, 0, 0, 0)
        const days = Math.round((today - day) / 86400000)
        return days <= 0 ? "Today" : days === 1 ? "Yesterday" : "Earlier"
    }
    function historyTime(visitedAt) {
        const visited = new Date(visitedAt)
        return historyGroup(visitedAt) === "Earlier"
            ? visited.toLocaleDateString(Qt.locale(), "MMM d") + " · " + visited.toLocaleTimeString(Qt.locale(), Locale.ShortFormat)
            : visited.toLocaleTimeString(Qt.locale(), Locale.ShortFormat)
    }
    readonly property var historyRows: Array.from(bible.history).map(function(entry) {
        return Object.assign({ group: app.historyGroup(entry.visitedAt) }, entry)
    })
    function openHistoryMenu(row, sceneX, sceneY) {
        const entry = historyList.model[row]
        if (!entry) return
        historyList.currentIndex = row
        moveHistory(0)
        const ready = !rangeEditing && !bible.busy
        const id = entry.id
        const scope = historyScope(entry)
        contextMenu.show("history", RowMenu.entries(app, { busy: bible.busy, scope: scope,
            open: function() { app.openHistoryItem(id) }, canOpen: entry.available, canCopy: entry.available && !!scope,
            copy: function() { app.copyHistoryItem(id) }, copyPlain: function() { app.copyHistoryItem(id, true) },
            desk: function() { app.toggleHistoryDesk(id) }, canDesk: entry.available && !!scope,
            extras: [{ id: "remove", text: "Remove from History", shortcut: KeyNotation.format("Delete"), enabled: ready, run: function() { app.removeHistoryItem(id) } }] }),
            sceneX, sceneY)
    }
    function savePosition() {
        if (!restoringPosition && bible.ready) {
            let travel = Math.max(0, reading.contentHeight - reading.height)
            bible.readingPosition = travel > 0 ? Math.max(0, Math.min(1, readerTargetY() / travel)) : 0
        }
    }
    // Programmatic scrolls to a verse slide (ease-out, ~110 ms) and retarget
    // mid-flight, so held keys never lag; instant with animations disabled.
    readonly property int slideMs: reduceMotion ? 0 : 110
    NumberAnimation { id: readerSlide; objectName: "readerSlide"; target: reading; property: "contentY"; duration: app.slideMs; easing.type: Easing.OutCubic }
    function clampReaderY(y) { return Math.max(0, Math.min(Math.max(0, reading.contentHeight - reading.height), y)) }
    function readerTargetY() { return readerSlide.running ? readerSlide.to : reading.contentY }
    function setReaderY(y) { readerSlide.stop(); reading.contentY = clampReaderY(y) }
    function slideReader(y) {
        const target = clampReaderY(y)
        if (readerSlide.running) {
            if (Math.abs(readerSlide.to - target) < 0.5) return
            readerSlide.stop()
        }
        if (!slideMs || Math.abs(target - reading.contentY) < 1) { reading.contentY = target; return }
        readerSlide.from = reading.contentY
        readerSlide.to = target
        readerSlide.start()
    }
    function scrollBy(amount) {
        clearCount()
        setReaderY(readerTargetY() + amount)
    }
    function scrollToVerse(number, alignTop) { placeVerse(number, alignTop, false) }
    // instant: restoring a freshly opened chapter, where there is nothing to slide from.
    function placeVerse(number, alignTop, instant) {
        let index = Array.from(bible.verses).findIndex(function(verse) { return verse.number === number })
        let row = verseRepeater.itemAt(index) as VerseItem
        let block = blockLoader.item as BlockReader
        if (!row && !block) return
        if (index === 0) { if (instant) setReaderY(0); else slideReader(0); return }
        let rect = block ? block.verseRect(number) : null
        let y = rect ? blockLoader.mapToItem(readerColumn, 0, rect.y).y : row.mapToItem(readerColumn, 0, 0).y + row.textOffset
        let textHeight = rect ? rect.height : row.textHeight
        let available = Math.max(1, reading.height - floatingSafety)
        let margin = Math.min(3 * readerLineHeight, Math.max(12 * readerScale, (available - textHeight) / 2))
        // A very long verse cannot fit. Keep its beginning and a useful context
        // margin visible rather than jumping to its end on every movement.
        if (textHeight > available - 24 * readerScale)
            margin = Math.min(3 * readerLineHeight, available * 0.2)
        let extent = Math.min(textHeight, Math.max(1, available - 2 * margin))
        let target = readerTargetY()
        // Focus mode is a typewriter: the cursor verse stays 40% down.
        if (bible.focusMode) target = y - 0.4 * reading.height
        else if (alignTop || y < target + margin) target = y - margin
        else if (y + extent > target + available - margin) target = y + extent - available + margin
        if (instant) setReaderY(target)
        else slideReader(target)
    }
    function alignCurrentVerse(alignment) {
        positionTimer.stop()
        restoringPosition = false
        readerColumn.forceLayout()
        const index = Array.from(bible.verses).findIndex(function(verse) { return verse.number === bible.cursorVerse })
        const row = verseRepeater.itemAt(index) as VerseItem
        const block = blockLoader.item as BlockReader
        if (!row && !block) return
        const rect = block ? block.verseRect(bible.cursorVerse) : null
        const y = block ? blockLoader.mapToItem(readerColumn, 0, rect.y).y
            : row.mapToItem(readerColumn, 0, row.textOffset).y
        const height = block ? rect.height : row.textHeight
        const available = Math.max(1, reading.height - floatingSafety)
        const offset = alignment === "top" ? 0 : alignment === "bottom" ? available - height : (available - height) / 2
        slideReader(y - Math.max(0, offset))
        savePosition()
    }
    // Navigation slides only within the chapter already on screen; other
    // restores (new chapter, layout, version) are instant.
    property string shownChapterKey: ""
    property bool restoreSlides: false
    function restorePosition(jumpToSelection) { beginRestore(jumpToSelection, false) }
    function beginRestore(jumpToSelection, slide) {
        const key = bible.versionId + ":" + bible.book + ":" + bible.chapter
        restoreSlides = !!slide && key === shownChapterKey
        shownChapterKey = key
        restoringPosition = true
        positionJumpToSelection = !!jumpToSelection
        positionTimer.restart()
    }
    function collectionForTab() {
        if (collectionTab === 3) return taggedRows()
        return Array.from(bible.collection).filter(function(entry) {
            return (collectionTab === 0 && entry.bookmark)
                || (collectionTab === 1 && entry.highlight) || (collectionTab === 2 && entry.note.length > 0)
        })
    }
    // Annotations → Tags: every tagged passage in this version, grouped by tag
    // (tag name order), each group in canonical order. A passage with several
    // tags appears once per tag.
    property var savedTagExcerpts: ({})
    function rangeKey(scope) { return [scope.versionId, scope.book, scope.chapter, scope.start, scope.endBook, scope.endChapter, scope.end].join(":") }
    function refreshTagExcerpts() {
        const ids = Array.from(bible.tags).map(function(tag) { return tag.id })
        const excerpts = {}
        if (ids.length && bible.versionId) Array.from(bible.taggedPassages(ids, false, bible.versionId)).forEach(function(row) {
            excerpts[app.rangeKey(row)] = row.excerpt || ""
        })
        savedTagExcerpts = excerpts
    }
    function taggedRows() {
        const ranges = Array.from(bible.collection).filter(function(entry) { return entry.tags && entry.tags.length > 0 })
        const rows = []
        Array.from(bible.tags).forEach(function(tag) {
            ranges.forEach(function(range) {
                if (!Array.from(range.tags).some(function(other) { return other.id === tag.id })) return
                rows.push(Object.assign({}, range, { group: tag.name, groupId: tag.id,
                    excerpt: app.savedTagExcerpts[app.rangeKey(range)] || "" }))
            })
        })
        return rows
    }
    // The reader's range ("here" rows): the selection, else nothing.
    function verseKey(book, chapter, verse) { return (book * 1000 + chapter) * 1000 + verse }
    function savedHere(entry) {
        if (!entry || !hasSelection || entry.versionId !== bible.versionId) return false
        const scope = bible.tagScope()
        if (!scope || !scope.start) return false
        return verseKey(entry.book, entry.chapter, entry.start) <= verseKey(scope.endBook, scope.endChapter, scope.end)
            && verseKey(scope.book, scope.chapter, scope.start) <= verseKey(entry.endBook, entry.endChapter, entry.end)
    }
    readonly property var savedHereKey: [bible.versionId, bible.selectionReference, bible.selectedStart, bible.selectedEnd, bible.passage]
    // Opening the Tags tab starts at the first row here, else the first row.
    function initialSavedRow() {
        if (collectionTab !== 3) return collectionList.count ? 0 : -1
        const rows = collectionList.model || []
        const here = rows.findIndex(function(entry) { return app.savedHere(entry) })
        return here >= 0 ? here : rows.length ? 0 : -1
    }
    // Annotations search: reference, note, and tag names, case-insensitive.
    property string savedQuery: ""
    // Typing replaces the field's text binding, so clear both.
    function clearSavedQuery() { savedQuery = ""; savedSearch.text = "" }
    function savedMatches(entry, query) {
        if (!query.length) return true
        // Tags: a match on the tag's name keeps its whole group.
        const fields = collectionTab === 3 ? [entry.reference, entry.note || "", entry.group, entry.excerpt || ""]
            : [entry.reference, entry.note || "", tagsPlainText(entry.tags), entry.excerpt || ""]
        return fields.join("\n").toLowerCase().indexOf(query.toLowerCase()) >= 0
    }
    function filteredCollection() {
        const query = savedQuery.trim()
        const rows = collectionForTab().filter(function(entry) { return app.savedMatches(entry, query) })
        if (collectionTab !== 3) return rows
        // Section headings: "name · count" over the rows shown.
        const counts = {}
        rows.forEach(function(row) { counts[row.groupId] = (counts[row.groupId] || 0) + 1 })
        return rows.map(function(row, index) {
            return Object.assign(row, { groupCount: counts[row.groupId], firstOfGroup: index === 0 || rows[index - 1].groupId !== row.groupId })
        })
    }
    // The search field appears once a tab holds more than ten items, and stays
    // while it has a query.
    readonly property int savedTabTotal: collectionForTab().length
    readonly property bool savedSearchVisible: savedTabTotal > 10 || savedQuery.length > 0
    function focusSavedSearch() { if (savedSearchVisible) savedSearch.forceActiveFocus() }
    function focusSavedList() {
        moveSaved(0)
    }
    function escapeSaved() {
        if (savedSearch.activeFocus) focusSavedList()
        else savedDrawer.close()
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
        const tab = (collectionTab + delta + 4) % 4
        clearSavedQuery()
        collectionTab = tab
        if (tab === 3) refreshTagExcerpts()
        collectionList.currentIndex = initialSavedRow()
        moveSaved(0)
    }
    function openSaved(entry) {
        if (rangeEditing || !entry) { sounds.play("error"); return }
        if (!flushNote()) return
        const from = beginJump()
        if (!bible.openTaggedPassage(entry)) return
        endJump(from)
        syncNote()
        savedDrawer.close()
        if (entry.note.length > 0 && collectionTab !== 3) openNotes(false)
        Qt.callLater(function() { app.scrollToVerse(bible.cursorVerse, true) })
        pulseDestination()
    }
    function requestDeleteSaved() {
        if (rangeEditing) return
        let row = collectionList.currentIndex
        let focused = activeFocusItem
        while (focused && focused !== collectionList) {
            let delegate = focused as SavedEntry
            if (delegate) { row = delegate.index; break }
            focused = focused.parent
        }
        let entry = focused instanceof SavedEntry ? (focused as SavedEntry).modelData : collectionList.model[row]
        if (!entry || bible.busy) return
        // Tags: removes only this row's tag from its range.
        if (collectionTab === 3) { requestRemoveTag(entry, row); return }
        // Freeze identity before flushing drafts can publish a replacement model.
        const scope = Object.freeze({ versionId: entry.versionId, reference: entry.reference,
            book: entry.book, chapter: entry.chapter, start: entry.start,
            endBook: entry.endBook, endChapter: entry.endChapter, end: entry.end })
        if (!flushNote() || bible.busy || !savedScopeExists(scope)) return
        deleteConfirmation.scope = scope
        deleteConfirmation.row = row
        deleteConfirmation.open()
    }
    function requestRemoveTag(entry, row) {
        if (rangeEditing || !entry || !entry.groupId || bible.busy) return
        const scope = Object.freeze({ versionId: entry.versionId, reference: entry.reference,
            book: entry.book, chapter: entry.chapter, start: entry.start,
            endBook: entry.endBook, endChapter: entry.endChapter, end: entry.end })
        if (!flushNote() || bible.busy) return
        deleteConfirmation.tag = { id: entry.groupId, name: entry.group }
        deleteConfirmation.scope = scope
        deleteConfirmation.row = row
        if (!deleteScopeValid()) { deleteConfirmation.tag = null; deleteConfirmation.scope = null; return }
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
        const tag = deleteConfirmation.tag
        if (tag) return savedScopeExists(deleteConfirmation.scope)
            && Array.from(bible.tagsForScope(deleteConfirmation.scope, false)).some(function(other) { return other.id === tag.id })
        return savedScopeExists(deleteConfirmation.scope)
    }
    function invalidateDeleteSaved() {
        deleteConfirmation.scope = null
        deleteConfirmation.row = -1
        deleteConfirmation.close()
    }
    function confirmDeleteSaved() {
        if (rangeEditing) return
        if (!deleteConfirmation.opened || !deleteScopeValid()) { invalidateDeleteSaved(); return }
        const scope = deleteConfirmation.scope
        const row = deleteConfirmation.row
        if (!flushNote() || bible.busy) return
        if (!deleteScopeValid() || deleteConfirmation.scope !== scope) {
            invalidateDeleteSaved()
            return
        }
        if (deleteConfirmation.tag) {
            const tag = deleteConfirmation.tag
            if (!bible.setTag(scope, tag.id, false)) { sounds.play("error"); return }
            notify(tag.name + " removed from " + (scope.reference || "passage"), true)
            sounds.play("remove")
            deleteConfirmation.close()
            Qt.callLater(function() { collectionList.currentIndex = Math.min(row, collectionList.count - 1) })
            return
        }
        if (deleteConfirmation.noteOnly) {
            if (!bible.deleteNoteScope(scope)) { sounds.play("error"); return }
            syncNote()
            notify((scope.reference || "Passage") + " note deleted", true)
            sounds.play("remove")
            deleteConfirmation.close()
            return
        }
        if (!bible.deleteAnnotationScope(scope)) return
        syncNote()
        collectionList.currentIndex = Math.min(row, collectionList.count - 1)
        deleteConfirmation.close()
    }
    function openBookmark(scope) {
        if (rangeEditing) return
        if (!flushNote()) return
        const from = beginJump()
        if (typeof scope === "string" ? bible.navigate(scope) : bible.openTaggedPassage(scope)) { endJump(from); syncNote(); readerFocus(); pulseDestination() }
    }
    function openNoteScope(scope) {
        if (rangeEditing) return
        if (!scope || !flushNote()) return
        const from = beginJump()
        if (!bible.openTaggedPassage(scope)) return
        endJump(from)
        syncNote()
        openNotes(true)
        Qt.callLater(function() { app.scrollToVerse(bible.cursorVerse, true) })
        pulseDestination()
    }
    function openVersions(removing) {
        if (rangeEditing) return
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
    // The highlighted Versions row, when its text predates the current schema.
    function reimportableVersion() {
        const entry = versionResults.model[versionResults.currentIndex]
        return !versionDialog.removing && versionDialog.step === 0 && entry && entry.needsReimport ? entry : null
    }
    function versionKey(event) {
        if (event.key === Qt.Key_I && event.modifiers === (Qt.ControlModifier | Qt.ShiftModifier)) {
            event.accepted = true
            const entry = reimportableVersion()
            if (entry && !bible.busy && !catalog.busy) reimportVersion(entry)
            else sounds.play("error")
            return
        }
        if (event.key === Qt.Key_I && event.modifiers === Qt.ControlModifier) {
            event.accepted = true
            if (!versionDialog.removing && versionDialog.step === 0) addVersionFromPicker()
            return
        }
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            event.accepted = true
            if (event.modifiers === Qt.NoModifier) acceptVersion()
            return
        }
        // The removal summary scrolls by key; arrows still move between Cancel and Confirm.
        if (versionDialog.step === 2 && ![Qt.Key_Up, Qt.Key_Down, Qt.Key_Left, Qt.Key_Right].includes(event.key)
                && scrollKey(versionSummaryScroll.contentItem as Flickable, event)) return
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
        if (bible.busy) { sounds.play("error"); return }
        if (versionAdd.activeFocus) { addVersionFromPicker(); return }
        if (versionCancel.activeFocus) { versionDialog.close(); return }
        if (versionBack.activeFocus) { backVersion(); return }
        if (versionDialog.step === 2) { confirmRemoveVersion(); return }
        const entry = versionResults.model[versionResults.currentIndex]
        if (!entry) { sounds.play("error"); return }
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
        if (rangeEditing) return
        if (bible.busy || !flushNote()) return
        const selected = hasSelection
        const from = beginJump()
        if (!bible.switchVersion(id)) return
        endJump(from)
        clearTextSelection()
        selectionAnchor = 0
        syncNote()
        restorePosition(hasSelection)
        versionDialog.close()
        notify(selected && !hasSelection ? bible.status : "Switched to " + bible.versionAbbreviation, true)
    }
    function confirmRemoveVersion() {
        if (rangeEditing) return
        if (!versionDialog.removing || versionDialog.step !== 2 || !versionDialog.chosen || bible.busy || !flushNote()) return
        const deskRemoved = versionDialog.removeAnnotations ? bible.deskCountForVersion(versionDialog.chosen.id) : 0
        const abbreviation = versionDialog.chosen.abbreviation
        if (!bible.removeVersion(versionDialog.chosen.id, versionDialog.removeAnnotations)) return
        clearTextSelection()
        selectionAnchor = 0
        syncNote()
        if (bible.ready) restorePosition(hasSelection)
        else notesDrawer.close()
        versionDialog.close()
        if (deskRemoved > 0)
            notify("Removed " + abbreviation + " · " + deskRemoved + (deskRemoved === 1 ? " Desk item" : " Desk items") + " removed too", true)
    }
    // One action catalog supplies shortcuts, searchable keybindings, and commands.
    // Context-only gestures are explanatory rows, never executable guesses.
    // Plain data literals only: bin/shortcuts parses this array for the README.
    // `sequences` are Qt key sequences (display text is generated by KeyNotation.js);
    // `aliases` also work but stay out of the in-app list; `group` orders docs.
    readonly property var actions: [
        { id: "previousVerse", group: "Navigate", title: "Previous verse", sequences: ["K", "Up"] },
        { id: "nextVerse", group: "Navigate", title: "Next verse", sequences: ["J", "Down"] },
        { id: "countMotion", group: "Navigate", title: "Repeat a verse motion by a count", sequences: ["4, J"], explanation: true },
        { id: "goToVerse", group: "Navigate", title: "Go to verse number", sequences: ["2, 0, G"], aliases: ["2, 0, Shift+G"], aliasReason: "Vim's go-to-line.", explanation: true },
        { id: "first", group: "Navigate", title: "Select chapter beginning", sequences: ["G", "Home"] },
        { id: "last", group: "Navigate", title: "Select chapter end", sequences: ["Shift+G", "End"] },
        { id: "previousFive", group: "Navigate", title: "Previous 5 verses", sequences: ["Ctrl+U"] },
        { id: "nextFive", group: "Navigate", title: "Next 5 verses", sequences: ["Ctrl+D"] },
        { id: "previousTen", group: "Navigate", title: "Previous 10 verses", sequences: ["Ctrl+B", "PgUp"] },
        { id: "nextTen", group: "Navigate", title: "Next 10 verses", sequences: ["Ctrl+F", "PgDown"] },
        { id: "centerVerse", group: "Navigate", title: "Center current verse", sequences: ["Z, Z"], command: true },
        { id: "topVerse", group: "Navigate", title: "Scroll current verse to top", sequences: ["Z, T"], command: true },
        { id: "bottomVerse", group: "Navigate", title: "Scroll current verse to bottom", sequences: ["Z, B"], command: true },
        { id: "previousChapter", group: "Navigate", title: "Previous chapter", sequences: ["H", "Left"] },
        { id: "nextChapter", group: "Navigate", title: "Next chapter", sequences: ["L", "Right"] },
        { id: "previousHeading", group: "Navigate", title: "Previous heading or chapter top", sequences: ["{"] },
        { id: "nextHeading", group: "Navigate", title: "Next section heading", sequences: ["}"] },
        { id: "historyBack", group: "History", title: "Back to the previous jump", sequences: ["["] },
        { id: "historyForward", group: "History", title: "Forward to the next jump", sequences: ["]"] },
        { id: "historySwap", group: "History", title: "Swap with the previous jump", sequences: ["-"] },
        { id: "mouseBack", group: "History", title: "Back a jump, back to the palette's books, or close the open panel", sequences: ["BackClick"], explanation: true },
        { id: "mouseForward", group: "History", title: "Forward to the next jump", sequences: ["ForwardClick"], explanation: true },
        { id: "selectVerse", group: "Select", title: "Select verse", sequences: ["Click"], explanation: true },
        { id: "selectRange", group: "Select", title: "Select a range", sequences: ["Shift+Click"], explanation: true },
        { id: "visual", group: "Select", title: "Toggle visual selection", sequences: ["V"] },
        { id: "characterVisual", group: "Select", title: "Select characters (Vim charwise visual)", sequences: ["Shift+V"] },
        // Bound by character selection itself (charKey).
        { id: "charMove", group: "Select", title: "Characters: left, right (across verses)", sequences: ["H", "L"], scope: "characters", explanation: true },
        { id: "charWords", group: "Select", title: "Characters: by word", sequences: ["W", "B", "E", "G, E"], scope: "characters", explanation: true },
        { id: "charBigWords", group: "Select", title: "Characters: by WORD (no spaces)", sequences: ["Shift+W", "Shift+B", "Shift+E"], scope: "characters", explanation: true },
        { id: "charSentences", group: "Select", title: "Characters: by sentence", sequences: ["(", ")"], scope: "characters", explanation: true },
        { id: "charLines", group: "Select", title: "Characters: by line", sequences: ["J", "K"], scope: "characters", explanation: true },
        { id: "charLineEdges", group: "Select", title: "Characters: line start, first ink, end", sequences: ["0", "^", "$"], scope: "characters", explanation: true },
        { id: "charEdges", group: "Select", title: "Characters: chapter start, end", sequences: ["G, G", "Shift+G"], scope: "characters", explanation: true },
        { id: "charObjects", group: "Select", title: "Characters: word, sentence objects", sequences: ["I, W", "A, W", "I, S", "A, S"], scope: "characters", explanation: true },
        { id: "charSwap", group: "Select", title: "Characters: swap ends", sequences: ["O"], scope: "characters", explanation: true },
        { id: "charCopy", group: "Select", title: "Characters: copy quotation", sequences: ["Y", "Ctrl+C"], scope: "characters", explanation: true },
        { id: "charCopyPlain", group: "Select", title: "Characters: copy plain text", sequences: ["Shift+Y", "Ctrl+Shift+C"], scope: "characters", explanation: true },
        { id: "charExit", group: "Select", title: "Characters: leave", sequences: ["Escape"], scope: "characters", explanation: true },
        { id: "extendNext", group: "Select", title: "Extend to next verse", sequences: ["Shift+J", "Shift+Down"] },
        { id: "extendPrevious", group: "Select", title: "Extend to previous verse", sequences: ["Shift+K", "Shift+Up"] },
        { id: "chapterFind", group: "Search", title: "Find in chapter", sequences: ["/"], command: true },
        { id: "nextMatch", group: "Search", title: "Next search match", sequences: ["N"] },
        { id: "previousMatch", group: "Search", title: "Previous search match", sequences: ["Shift+N"] },
        { id: "bibleSearch", group: "Search", title: "Search Bible", sequences: ["Ctrl+/"], scope: "palette", command: true },
        { id: "bookmark", group: "Annotate", title: "Bookmark selection", sequences: ["M"], command: true },
        { id: "highlight", group: "Annotate", title: "Highlight selection", sequences: ["X"], command: true },
        { id: "notes", group: "Annotate", title: "Write a note", sequences: ["W"], scope: "notes", command: true },
        { id: "tags", group: "Annotate", title: "Tag passage", sequences: ["T"], command: true },
        { id: "toggleDesk", group: "Annotate", title: "Add to Desk", sequences: ["D"], command: true },
        { id: "editRange", group: "Annotate", title: "Edit annotation range", sequences: ["Shift+E"], command: true },
        { id: "rangeChooseAnother", group: "Annotate", title: "Edit range: choose another overlapping range", sequences: ["Backspace"], scope: "rangeEditor", explanation: true },
        { id: "rangeMergeBack", group: "Annotate", title: "Edit range: back from Combine notes to moving the range", sequences: ["Escape"], scope: "rangeEditor", explanation: true },
        { id: "manageTags", group: "Annotate", title: "Manage tags", sequences: ["Ctrl+T"], command: true },
        // Bound by the tag picker's Manage tags page itself.
        { id: "manageRename", group: "Annotate", title: "Manage tags: rename the chosen tag", sequences: ["Return"], scope: "tags", explanation: true },
        { id: "manageDelete", group: "Annotate", title: "Manage tags: delete a tag", sequences: ["Delete"], scope: "tags", explanation: true },
        { id: "manageMerge", group: "Annotate", title: "Manage tags: choose a tag to merge into", sequences: ["Ctrl+M"], scope: "tags", explanation: true },
        { id: "manageBack", group: "Annotate", title: "Manage tags: back to the picker (empty filter)", sequences: ["Backspace"], scope: "tags", explanation: true },
        // Bound by the note editor itself.
        { id: "noteLinkSuggest", group: "Annotate", title: "Suggest a passage to link (in a note)", sequences: ["[, ["], scope: "noteEditor" },
        { id: "openNoteLink", group: "Annotate", title: "Open the passage or web link at the cursor (in a note)", sequences: ["Alt+Return"], scope: "noteEditor" },
        { id: "notePreview", group: "Annotate", title: "Switch a note between Write and Preview (Notes panel)", sequences: ["Alt+V"], scope: "noteEditor" },
        { id: "noteLinkedFrom", group: "Annotate", title: "Between a note and its Linked from list (in a note)", sequences: ["Tab", "Shift+Tab"], scope: "noteEditor" },
        { id: "copy", group: "Copy", title: "Copy passage as quotation", sequences: ["Y"], command: true },
        { id: "copyText", group: "Copy", title: "Copy selected text as quotation (else the passage)", sequences: ["Ctrl+C"] },
        { id: "middleCopy", group: "Copy", title: "Copy passage as quotation", sequences: ["MiddleClick"], explanation: true },
        { id: "copyPlain", group: "Copy", title: "Copy passage as plain text", sequences: ["Shift+Y"], command: true },
        { id: "copyTextPlain", group: "Copy", title: "Copy selected text as plain text (else the passage)", sequences: ["Ctrl+Shift+C"] },
        { id: "contextMenu", group: "Copy", title: "Open context menu", sequences: ["Menu", "Shift+F10"] },
        { id: "clickMenu", group: "Copy", title: "Open context menu", sequences: ["RightClick"], explanation: true },
        { id: "annotations", group: "Panels", title: "Annotations panel", sequences: ["Shift+A"], command: true },
        { id: "desk", group: "Panels", title: "Open Desk", sequences: ["Shift+D"], command: true },
        // The Study panel (Cross-references, Footnotes, Tags); id kept for compatibility.
        { id: "related", group: "Panels", title: "Study panel", sequences: ["Shift+S"], command: true },
        { id: "history", group: "Panels", title: "History", sequences: ["Shift+H"], command: true },
        { id: "books", group: "Palettes and windows", title: "Navigate to a passage", sequences: ["Ctrl+K"], scope: "palette", command: true },
        { id: "commands", group: "Palettes and windows", title: "Open commands", sequences: ["Ctrl+Space"], aliases: ["Ctrl+Shift+P"], aliasReason: "the command palette key in VS Code and its kin.", scope: "palette" },
        { id: "resume", group: "Palettes and windows", title: "Resume last palette", sequences: ["Space"] },
        { id: "settings", group: "Palettes and windows", title: "Reading settings", sequences: ["Ctrl+R"], scope: "global", command: true },
        { id: "switchVersion", group: "Palettes and windows", title: "Switch version", sequences: ["Ctrl+V"], scope: "global", command: true },
        { id: "help", group: "Palettes and windows", title: "Keybindings", sequences: ["?"], aliases: ["F1"], aliasReason: "the help key in most desktop apps.", scope: "global", command: true },
        { id: "newWindow", group: "Palettes and windows", title: "New window", sequences: ["Ctrl+N"], scope: "global", command: true },
        // Bound by the Versions dialog itself.
        { id: "addVersion", group: "Palettes and windows", title: "Add version from Versions", sequences: ["Ctrl+I"], scope: "versions", explanation: true },
        // Bound by the Add version guide itself.
        { id: "guideInstall", group: "Palettes and windows", title: "Add version: install the highlighted version", sequences: ["Return"], scope: "importGuide", explanation: true },
        { id: "guideDetails", group: "Palettes and windows", title: "Add version: its Details & Attribution", sequences: ["Shift+Return"], scope: "importGuide", explanation: true },
        { id: "guideImport", group: "Palettes and windows", title: "Add version: import a USFM ZIP", sequences: ["Ctrl+I"], scope: "importGuide", explanation: true },
        { id: "reimportVersion", group: "Palettes and windows", title: "Re-import the highlighted version (Versions)", sequences: ["Ctrl+Shift+I"], scope: "versions", explanation: true },
        { id: "focus", group: "Reading", title: "Focus mode", sequences: ["F"], setting: "focusMode", command: true },
        { id: "numbers", group: "Reading", title: "Show verse numbers", sequences: ["Alt+N"], setting: "showVerseNumbers", command: true },
        { id: "headings", group: "Reading", title: "Show headings", sequences: ["Alt+H"], setting: "showHeadings", command: true },
        { id: "versePicker", group: "Reading", title: "Verse picker", sequences: ["Alt+P"], setting: "versePicker", command: true },
        { id: "wordsOfJesus", group: "Reading", title: "Highlight words of Jesus", sequences: ["Alt+J"], setting: "wordsOfJesus", command: true },
        { id: "chapterOutline", group: "Reading", title: "Chapter outline", sequences: ["Alt+O"], setting: "chapterOutline", command: true },
        // alt w: w writes a note; alt w shows notes in the margin.
        { id: "marginNotes", group: "Reading", title: "Margin notes", sequences: ["Alt+W"], setting: "marginNotes", command: true },
        { id: "readingMode", group: "Reading", title: "Reading mode", sequences: ["Alt+L"], command: true },
        { id: "soundEffects", group: "Reading", title: "Sound effects", sequences: ["Alt+S"], scope: "global", command: true },
        { id: "reduceMotion", group: "Reading", title: "Animations", sequences: ["Alt+M"], scope: "global", command: true },
        { id: "larger", group: "Reading", title: "Larger text", sequences: ["Ctrl++", "Ctrl+="] },
        { id: "smaller", group: "Reading", title: "Smaller text", sequences: ["Ctrl+-"] },
        { id: "resetText", group: "Reading", title: "Reset text size", sequences: ["Ctrl+0"] },
        { id: "lineMore", group: "Reading", title: "Increase line height", sequences: ["Ctrl+Alt+="] },
        { id: "lineLess", group: "Reading", title: "Decrease line height", sequences: ["Ctrl+Alt+-"] },
        { id: "lineReset", group: "Reading", title: "Reset line height", sequences: ["Ctrl+Alt+0"], command: true },
        { id: "dismiss", group: "Reading", title: "Dismiss current panel or selection", sequences: ["Escape"], explanation: true },
        // List panels (Annotations, Desk, History, Study) bind these themselves.
        { id: "listNext", group: "Lists", title: "Next item", sequences: ["J", "Down"], scope: "list", explanation: true },
        { id: "listPrevious", group: "Lists", title: "Previous item", sequences: ["K", "Up"], scope: "list", explanation: true },
        { id: "listFirst", group: "Lists", title: "First item", sequences: ["G"], scope: "list", explanation: true },
        { id: "listLast", group: "Lists", title: "Last item", sequences: ["Shift+G"], scope: "list", explanation: true },
        { id: "listHalfDown", group: "Lists", title: "Down 5 items", sequences: ["Ctrl+D"], scope: "list", explanation: true },
        { id: "listHalfUp", group: "Lists", title: "Up 5 items", sequences: ["Ctrl+U"], scope: "list", explanation: true },
        { id: "listPageDown", group: "Lists", title: "Down 10 items", sequences: ["Ctrl+F", "PgDown"], scope: "list", explanation: true },
        { id: "listPageUp", group: "Lists", title: "Up 10 items", sequences: ["Ctrl+B", "PgUp"], scope: "list", explanation: true },
        { id: "listOpen", group: "Lists", title: "Open item", sequences: ["Return"], scope: "list", explanation: true },
        { id: "listCopy", group: "Lists", title: "Copy item as quotation", sequences: ["Y"], scope: "list", explanation: true },
        { id: "listCopyPlain", group: "Lists", title: "Copy item as plain text", sequences: ["Shift+Y"], scope: "list", explanation: true },
        { id: "listDesk", group: "Lists", title: "Add item to Desk, or remove it", sequences: ["D"], scope: "list", explanation: true },
        { id: "listEditRange", group: "Lists", title: "Edit item's annotation range", sequences: ["Shift+E"], scope: "list", explanation: true },
        { id: "listRemove", group: "Lists", title: "Remove item", sequences: ["Delete", "Backspace"], scope: "list", explanation: true },
        { id: "listTag", group: "Lists", title: "Edit the item's tags (Annotations and Study Tags)", sequences: ["T"], scope: "list", explanation: true },
        { id: "studyChips", group: "Lists", title: "Between Study Tags results and tag chips", sequences: ["Tab", "Shift+Tab"], scope: "list", explanation: true },
        { id: "studyChipToggle", group: "Lists", title: "Turn a Study tag chip on or off", sequences: ["Space"], scope: "list", explanation: true },
        { id: "studyChipSolo", group: "Lists", title: "Show only this chip's tag, or every tag again", sequences: ["Shift+Space"], scope: "list", explanation: true },
        { id: "studyChipsAll", group: "Lists", title: "Turn every Study tag chip back on", sequences: ["0"], scope: "list", explanation: true },
        { id: "studyAllVersions", group: "Lists", title: "Study Tags in all installed versions, or this one", sequences: ["A"], scope: "list", explanation: true },
        { id: "listSearch", group: "Lists", title: "Search the list (Annotations, Study's Tags)", sequences: ["/"], scope: "list", explanation: true },
        { id: "listPreviousTab", group: "Lists", title: "Previous tab (Annotations, Study)", sequences: ["H", "Left"], scope: "list", explanation: true },
        { id: "listNextTab", group: "Lists", title: "Next tab (Annotations, Study)", sequences: ["L", "Right"], scope: "list", explanation: true },
        // Desk-only: bound by deskDrawer, never globally.
        { id: "cleanDesk", group: "Lists", title: "Clean Desk", sequences: ["Shift+Del", "Shift+Backspace"], scope: "desk", command: true },
        // Bound by the History panel; the command itself is Commands-only.
        { id: "historyClearKeys", group: "Lists", title: "Clear History", sequences: ["Shift+Del", "Shift+Backspace"], scope: "history", explanation: true },
        { id: "import", group: "Commands only", title: "Add version", scope: "global", command: true },
        { id: "removeVersion", group: "Commands only", title: "Remove version", scope: "global", command: true },
        { id: "export", group: "Commands only", title: "Export notes as Markdown", command: true },
        { id: "backup", group: "Commands only", title: "Save annotation backup", command: true },
        { id: "restore", group: "Commands only", title: "Restore annotation backup", command: true },
        { id: "clearHistory", group: "Commands only", title: "Clear History", command: true },
        { id: "toggleHistory", group: "Commands only", title: "Disable / Enable History", command: true },
        { id: "about", group: "Commands only", title: "About & Attribution", scope: "global", command: true }
    ]
    // Explanations are gestures; desk/list rows are bound by their panels.
    function globallyBound(action) { return !!action.sequences && !action.explanation && !["desk", "list", "versions", "noteEditor", "characters", "tags", "rangeEditor", "importGuide"].includes(action.scope) }
    function actionById(id) { return actions.find(function(entry) { return entry.id === id }) }
    function actionKeys(action) {
        if (typeof action === "string") action = actionById(action)
        return action && action.sequences ? Array.from(action.sequences) : []
    }
    // Generated key notation (KeyNotation.js): plain for tooltips and menus, rich for lists.
    function keyText(action) { return KeyNotation.formatList(actionKeys(action)) }
    function keyRich(action) { return KeyNotation.richList(actionKeys(action), desktop.foreground, desktop.muted) }
    function formatKeys(sequences) { return KeyNotation.formatList(sequences) }
    // A focused button owns enter and space: they activate it, never a window
    // shortcut (space would otherwise resume the last palette).
    function buttonShortcutOverride(event) {
        const plain = (event.modifiers & ~Qt.KeypadModifier) === Qt.NoModifier
        if (plain && [Qt.Key_Space, Qt.Key_Return, Qt.Key_Enter].includes(event.key)) event.accepted = true
    }
    // Plain enter presses a focused button (modified enter passes through).
    function buttonEnter(button, event) {
        if ((event.modifiers & ~Qt.KeypadModifier) !== Qt.NoModifier) { event.accepted = false; return }
        button.keyActivated = true
        button.clicked()
    }
    // After a mouse click on a button that held the keyboard (reached by tab),
    // the keyboard returns to the reader, the open panel, or the note editor.
    function buttonClicked(button) {
        const byKey = button.keyActivated
        button.keyActivated = false
        if (!byKey && button.activeFocus) restorePanelFocus()
    }
    // Clickable hint rows (HintRow): [keys, label, run, enabled] per part. A
    // part runs what its key would, and is clickable only while that works
    // with the current focus. Navigation parts have no run.
    function rangeEditHintParts() {
        const ready = rangeEditor.active && !rangeEditor.visibleModal && !bible.busy
        return [["O", "swap", function() { rangeEditor.swap() }, ready], ["Return", "save", function() { rangeEditor.accept() }, ready],
            ["Escape", "cancel", function() { rangeEditor.cancel() }, rangeEditor.sessionOpen]]
    }
    function paletteHintParts() {
        return [[["Up", "Down"], "choose"], paletteMoreResults ? ["Down", "more", function() { app.loadMorePalette() }] : null,
            ["Return", "select", app.acceptPalette, !!paletteDestination.entry || paletteResolution.valid],
            paletteBook > 0 ? ["Left", "back", app.backToBooks] : null, ["Escape", "close", app.escapePalette]]
    }
    function versionHintParts() {
        const ready = !bible.busy
        if (versionDialog.step === 2) return [[["Left", "Right"], "choose"], ["Return", "select", app.acceptVersion, ready], ["Escape", "cancel", function() { versionDialog.close() }]]
        const select = ["Return", "select", app.acceptVersion, ready && versionResults.currentIndex >= 0 && versionResults.count > 0]
        const reimport = reimportableVersion()
        if (!versionDialog.removing) return [[["Up", "Down"], "choose"], [actionKeys("addVersion")[0], "add", app.addVersionFromPicker, ready],
            reimport ? [actionKeys("reimportVersion")[0], "re-import", function() { app.reimportVersion(app.reimportableVersion()) }, ready && !catalog.busy] : null,
            select, ["Escape", "cancel", function() { versionDialog.close() }]]
        return [[["Up", "Down"], "choose"], [["Home", "End"], "jump"], select, ["Escape", "cancel", function() { versionDialog.close() }]]
    }
    function savedHintParts() {
        const entry = focusedSavedEntry()
        const ready = !!entry && !bible.busy && !rangeEditing
        const search = savedSearchVisible ? ["/", "search", app.focusSavedSearch] : null
        const close = ["Escape", "close", app.escapeSaved]
        const open = ["Return", "open", function() { app.openSaved(app.focusedSavedEntry()) }, ready]
        const copy = ["Y", "copy", function() { app.copyScope(app.focusedSavedEntry()) }, !!entry && !bible.busy]
        const desk = ["D", "Desk", function() { app.toggleScopeDesk(app.focusedSavedEntry()) }, ready]
        const range = ["Shift+E", "range", app.editSavedRange, ready]
        if (collectionTab === 3) return [[["Left", "Right"], "tabs"], open,
            ["T", "tags", function() { app.openPassageTags(tagsUi.snapshot(app.focusedSavedEntry())) }, ready],
            ["Delete", "remove tag", app.requestDeleteSaved, ready], copy, desk, range,
            [actionKeys("manageTags")[0], "manage", function() { tagsUi.openManage() }, !bible.busy && !rangeEditing], search, close]
        return [[["Left", "Right"], "tabs"], open, copy, desk, range, ["Delete", "delete", app.requestDeleteSaved, ready], search, close]
    }
    function deskHintParts() {
        const entry = deskList.model[deskList.currentIndex]
        const ready = !!entry && !bible.busy && !rangeEditing
        return [[["Up", "Down"], "choose"], ["Return", "open", function() { app.openDeskItem(entry.id) }, ready && entry.available],
            ["Y", "copy", function() { app.copyDeskItem(entry.id) }, ready && entry.available],
            ["Delete", "remove", function() { app.removeDeskItem(entry.id) }, ready],
            [actionKeys("cleanDesk")[0], "clean", app.requestCleanDesk, deskList.count > 0 && !bible.busy && !rangeEditing],
            ["Escape", "close", function() { deskDrawer.close() }]]
    }
    function historyHintParts() {
        const entry = historyList.model[historyList.currentIndex]
        const ready = !!entry && !bible.busy && !rangeEditing
        return [[["Up", "Down"], "choose"], ["Return", "open", function() { app.openHistoryItem(entry.id) }, ready && entry.available],
            ["Y", "copy", function() { app.copyHistoryItem(entry.id) }, ready && entry.available],
            ["Delete", "remove", function() { app.removeHistoryItem(entry.id) }, ready],
            [actionKeys("historyClearKeys")[0], "clear", app.requestClearHistory, historyList.count > 0 && !bible.busy && !rangeEditing],
            ["Escape", "close", function() { historyDrawer.close() }]]
    }
    // Confirmations: arrows move between Cancel and Confirm; enter presses the focused one.
    function confirmationHintParts(cancel, confirm, dialog) {
        return [[["Left", "Right"], "choose"], ["Return", "select", function() { (confirm.activeFocus ? confirm : cancel).clicked() },
            !bible.busy && (cancel.activeFocus || (confirm.activeFocus && confirm.enabled))], ["Escape", "cancel", function() { dialog.close() }]]
    }
    function hintRow(parts) { return KeyNotation.hint(parts, desktop.foreground, desktop.muted) }
    function hintPlain(parts) { return KeyNotation.hintPlain(parts) }
    function actionHint(action) {
        const keys = keyText(action)
        if (action.id === "readingMode") return readingModeName(bible.readingMode) + (keys ? " · " + keys : "")
        if (action.setting) return (bible[action.setting] ? "On" : "Off") + (keys ? " · " + keys : "")
        return keys
    }
    // Commands whose palette title describes the action it would take now.
    function actionTitle(action) {
        if (action.id === "toggleDesk") return bible.currentInDesk ? "Remove from Desk" : "Add to Desk"
        if (action.id === "soundEffects") return sounds.enabled ? "Disable sound effects" : "Enable sound effects"
        if (action.id === "reduceMotion") return sounds.reduceMotion ? "Enable animations" : "Disable animations"
        if (action.id === "toggleHistory") return bible.historyEnabled ? "Disable History" : "Enable History"
        return action.title
    }
    function readingModeName(mode) { return mode.charAt(0).toUpperCase() + mode.slice(1) }
    function shortcutEnabled(action) {
        if (action.explanation || bible.busy) return false
        // A reader key whose action cannot run now still fires, so runCommand
        // sounds the error instead of swallowing the key silently.
        return shortcutContext(action) && (actionAvailable(action) || readerKeys)
    }
    function shortcutContext(action) {
        // Text size and line height type nothing, so they work from anywhere:
        // panels, the palette, modals, find, and text fields included.
        if (textSizeActions.includes(action.id)) return !errorDialog.visible
        if (findPromptOpen) return false
        // Inside the tag picker ctrl t switches between the picker and Manage.
        if (action.id === "manageTags" && tagsUi.pickerVisible) return !tagsUi.confirmation && !errorDialog.visible
        // Character selection owns the reader's keys (charKey); window keys
        // (palettes, dialogs) still work and end it as they take the keyboard.
        if (charMode && readerBase && action.scope !== "global" && action.scope !== "palette") return false
        if (zPending && action.sequences.some(function(sequence) { return ["Z", "T", "B"].includes(sequence) })) return false
        // Typing owns every key, Ctrl and Alt included (ctrl v pastes); only the
        // palette's own mode keys still switch modes from its input.
        if (textInputActive() && !(action.scope === "palette" && passagePalette.visible)) return false
        if (sidePanel.length && panelActions.includes(action.id)) return panelSwitchKeys
        if (settingsDialog.opened && !deleteConfirmation.visible && !errorDialog.visible
                && (action.setting || ["readingMode", "larger", "smaller", "resetText", "lineLess", "lineMore", "lineReset"].includes(action.id))) return true
        // visible, not opened: the palette accepts keys during its fade-in.
        if (action.scope === "palette") return !modalOpen || passagePalette.visible
        if (action.scope === "global") return !modalOpen && (bible.ready || ["newWindow", "help", "about", "soundEffects", "reduceMotion", "import", "switchVersion", "removeVersion", "settings"].includes(action.id))
        if (action.scope === "notes") return bible.ready && !modalOpen && !textInputActive()
        return readerKeys
    }
    readonly property var textSizeActions: ["larger", "smaller", "resetText", "lineLess", "lineMore", "lineReset"]
    function actionAvailable(action) {
        if (typeof action === "string") action = actions.find(function(entry) { return entry.id === action })
        if (!action || action.explanation || bible.busy) return false
        if (rangeEditing && !["newWindow", "contextMenu", "previousVerse", "nextVerse", "previousFive", "nextFive", "previousTen", "nextTen", "extendPrevious", "extendNext", "first", "last", "copy", "copyPlain", "copyText", "copyTextPlain", "larger", "smaller", "resetText", "lineLess", "lineMore", "lineReset"].includes(action.id)) return false
        if (action.id === "copyText" || action.id === "copyTextPlain") return bible.ready
        if (action.id === "related") return bible.ready && bible.verses.length > 0
        if (["bookmark", "highlight", "tags", "toggleDesk"].includes(action.id)) return hasSelection
        return bible.ready || !!action.setting
            || ["books", "commands", "newWindow", "help", "about", "soundEffects", "reduceMotion", "import", "switchVersion", "removeVersion", "settings", "readingMode", "larger", "smaller", "resetText", "lineLess", "lineMore", "lineReset"].includes(action.id)
    }
    function scopeTitle(action) {
        if (action.scope === "list") return "List panels · Annotations, Desk, History, Study"
        if (action.scope === "history") return "History panel only"
        if (action.scope === "versions") return "Versions dialog"
        if (action.explanation) return "Reader gesture"
        if (textSizeActions.includes(action.id)) return "Anywhere in this window · panels, dialogs, and while typing"
        if (action.scope === "global") return "This window · also with the Notes panel open, but not while typing"
        if (action.scope === "palette") return "This window · passage and command search"
        if (action.scope === "notes") return "Reader or notes preview"
        if (action.scope === "desk") return "Desk panel only"
        if (action.scope === "noteEditor") return "Notes editor"
        if (action.scope === "characters") return "Character selection · " + keyText("characterVisual")
        if (action.scope === "importGuide") return "Add version guide"
        if (action.scope === "rangeEditor") return "Range editing (" + keyText("editRange") + ")"
        if (action.scope === "tags") return "Tag picker · Manage tags (" + keyText("manageTags") + ")"
        return "Reader · outside text editing and dialogs"
    }
    function filteredKeybindings() {
        let query = keybindingSearch.text.trim().toLowerCase()
        return actions.filter(function(binding) {
            if (!binding.sequences) return false
            // Single-letter searches match a key, rather than every action containing it.
            const keys = binding.sequences.map(function(sequence) { return KeyNotation.format(sequence) })
            return !query || (query.length === 1
                ? keys.some(function(key) { return key.toLowerCase() === query })
                : (binding.title + " " + keys.join(" · ") + " " + binding.sequences.join(" ").replace(/\+/g, " ")).toLowerCase().indexOf(query) >= 0)
        })
    }
    function paletteEntries() {
        if (paletteTextMode) {
            // An empty search lists recent queries (History) to search again.
            if (!paletteTextQuery.length) return Array.from(bible.recentQueries).slice(0, 5).map(function(query) {
                return { title: query, hint: "Recent search", recentQuery: query }
            })
            return paletteSearchCurrent ? Array.from(bible.searchResults) : []
        }
        let query = referenceInput.text.trim().toLowerCase()
        if (query.startsWith(">")) {
            query = query.substring(1).trim()
            return actions.filter(function(c) { return c.command && actionTitle(c).toLowerCase().indexOf(query) >= 0 })
                .map(function(c) { return { title: actionTitle(c), hint: actionHint(c), action: c.id } })
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
        // Typed numbers: the shared fuzzy parser's ranked candidates (jn316, ps23, …).
        if (/\d/.test(query.replace(/^[1-3]\s*/, ""))) return Array.from(bible.referenceCandidates(referenceInput.text.trim())).map(function(candidate) {
            return { title: candidate.reference, hint: candidate.start ? "Passage" : "Chapter", candidate: true, reference: candidate.reference }
        })
        // Resolve exact canonical/source aliases without changing the displayed source name.
        const resolved = query && !/\d\s*$/.test(query) ? bible.resolveReference(query) : null
        return (query ? [] : recentPassages()).concat(Array.from(bible.books).filter(function(b) {
            return !query || b.name.toLowerCase().indexOf(query) >= 0
                || b.code.toLowerCase().indexOf(query) >= 0 || b.abbreviation.toLowerCase().indexOf(query) >= 0
                || (resolved && resolved.valid && resolved.reference === b.name + " " + b.chapterNumbers[0])
        })
            .map(function(b) { return { title: b.name, hint: b.chapters + " chapters", book: b.id, chapter: 0 } }))
    }
    // Up to five History passages, never the one being read. The last one
    // carries a rule that separates the group from the rest of the list.
    readonly property int paletteRecentLimit: 5
    function recentPassages() {
        // Read notifying properties so the palette model follows the reader.
        const here = bible.ready && (bible.passage + bible.selectionReference + bible.versionId).length ? bible.currentLocation() : null
        return Array.from(bible.history).filter(function(entry) {
            if (!entry.available || !here) return !!entry.available
            const sameChapter = entry.versionId === here.versionId && entry.book === here.book && entry.chapter === here.chapter
            return !app.sameLocation(entry, here) && !(sameChapter && (!entry.start || !here.start))
        }).slice(0, paletteRecentLimit).map(function(entry, index, recents) {
            return { title: entry.reference, hint: "Recent · " + entry.versionAbbreviation, recent: entry,
                lastRecent: index === recents.length - 1 }
        })
    }
    function openRecentPassage(entry) {
        if (rangeEditing || !flushNote()) return
        const from = beginJump()
        if (!openLocation(entry)) return
        endJump(from)
        passagePalette.navigated = true
        passagePalette.close()
        readerFocus()
    }
    function activatePaletteEntry(entry) {
        if (!entry) return
        if (paletteTextMode) {
            if (entry.recentQuery) {
                referenceInput.text = "/ " + entry.recentQuery
                referenceInput.cursorPosition = referenceInput.text.length
                referenceInput.forceActiveFocus()
            } else acceptBibleResult(entry)
            return
        }
        if (entry.recent) { openRecentPassage(entry.recent); return }
        if (entry.candidate) { jump(entry.reference); return }
        if (entry.action) {
            if (!actionAvailable(entry.action)) { sounds.play("error"); return }
            if (!flushNote()) return
            if (["books", "bibleSearch", "commands", "resume"].includes(entry.action)) {
                runCommand(entry.action)
                return
            }
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
        else sounds.play("error")
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
    // Bible search loads results a page at a time: moving past the last loaded
    // row (↓, pgdn, end) loads the next page and continues into it.
    readonly property bool paletteMoreResults: paletteTextMode && paletteSearchCurrent && !bible.searchBusy
        && paletteResults.count > 0 && paletteResults.count < bible.searchResultCount
    function loadMorePalette(target) {
        if (!paletteMoreResults) { sounds.play("boundary"); return false }
        const row = target === undefined ? paletteResults.count : target
        bible.loadMoreSearchResults()
        paletteResults.currentIndex = Math.max(0, Math.min(paletteResults.count - 1, row))
        paletteResults.positionViewAtIndex(paletteResults.currentIndex, ListView.Contain)
        paletteArrowUsed = true
        return true
    }
    // Pages through the results; end jumps to the last row (once browsing the
    // list), then on into the next loaded page; home to the first.
    function paletteListKey(event) {
        if (event.modifiers !== Qt.NoModifier) return false
        const page = Math.max(1, Math.floor(paletteResults.height / (44 * scale)))
        if (event.key === Qt.Key_PageDown) movePalette(page)
        else if (event.key === Qt.Key_PageUp) movePalette(-page)
        else if (event.key === Qt.Key_End && paletteArrowUsed) {
            if (paletteResults.currentIndex >= paletteResults.count - 1) loadMorePalette()
            else movePalette(paletteResults.count - 1 - paletteResults.currentIndex)
        } else if (event.key === Qt.Key_Home && paletteArrowUsed) movePalette(-paletteResults.currentIndex)
        else return false
        event.accepted = true
        return true
    }
    function movePalette(delta) {
        paletteArrowUsed = true
        if (delta < 0 && paletteResults.currentIndex <= 0 && paletteBack.visible) {
            paletteResults.currentIndex = -1
            paletteBack.forceActiveFocus()
            return
        }
        if (delta > 0 && paletteResults.currentIndex + delta > paletteResults.count - 1 && paletteMoreResults) {
            loadMorePalette(paletteResults.currentIndex + delta)
            referenceInput.forceActiveFocus()
            return
        }
        paletteResults.currentIndex = Math.max(0, Math.min(paletteResults.count - 1, paletteResults.currentIndex + delta))
        paletteResults.positionViewAtIndex(paletteResults.currentIndex, ListView.Contain)
        referenceInput.forceActiveFocus()
    }
    function runCommand(action) {
        if (!actionAvailable(action)) { sounds.play("error"); return }
        if (action === "first" || action === "last") { countedVerse(action === "last"); return }
        const count = Math.max(1, Math.min(999999, Number(verseCount) || 1))
        clearCount()
        // Text size never touches a Notes draft, so typing in Notes keeps going.
        if (!textSizeActions.includes(action) && !flushNote()) return
        const catalogAction = actions.find(function(entry) { return entry.id === action })
        const previousSetting = catalogAction.setting ? bible[catalogAction.setting] : null
        const previousLineHeight = bible.lineHeight
        const previousReaderSize = readerSize
        switch (action) {
        case "editRange": openRangeEditor(); break
        case "contextMenu": openPassageMenuAtCursor(); break
        case "newWindow": if (!modalOpen) newWindowRequested(); break
        case "books": showPaletteMode("passage"); break
        case "bibleSearch": showPaletteMode("search"); break
        case "chapterFind": beginChapterFind(); break
        case "centerVerse": alignCurrentVerse("center"); break
        case "topVerse": alignCurrentVerse("top"); break
        case "bottomVerse": alignCurrentVerse("bottom"); break
        case "nextMatch": navigateSearch(1, count); break
        case "previousMatch": navigateSearch(-1, count); break
        case "commands": showPaletteMode("commands"); break
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
        case "copyText": if (freeSelectedText.length) copySelectedText(false); else copyPassage(false); break
        case "copyTextPlain": if (freeSelectedText.length) copySelectedText(true); else copyPassage(true); break
        case "characterVisual": beginCharMode(); break
        case "larger": bible.setFontSize(readerSize + 1); break
        case "smaller": bible.setFontSize(readerSize - 1); break
        case "resetText": bible.setFontSize(18); break
        case "numbers": bible.setShowVerseNumbers(!bible.showVerseNumbers); break
        case "headings": bible.setShowHeadings(!bible.showHeadings); break
        case "versePicker": bible.setVersePicker(!bible.versePicker); break
        case "wordsOfJesus": bible.setWordsOfJesus(!bible.wordsOfJesus); break
        case "chapterOutline": bible.setChapterOutline(!bible.chapterOutline); break
        case "marginNotes": bible.setMarginNotes(!bible.marginNotes); break
        case "previousHeading": headingStep(-1, count); break
        case "nextHeading": headingStep(1, count); break
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
        case "settings": settingsDialog.open(); break
        case "annotations":
            if (!leaveSidePanel("annotations")) break
            bible.refreshCollection()
            if (collectionTab === 3) openTagsBrowser()
            else savedDrawer.open()
            break
        case "notes": if (sidePanel === "notes" || leaveSidePanel("notes")) openNotes(true); break
        case "desk": if (leaveSidePanel("desk")) openDesk(); break
        case "toggleDesk": toggleDeskPassage(); break
        case "cleanDesk": requestCleanDesk(); break
        case "bookmark": markPassage(false); break
        case "highlight": markPassage(true); break
        case "tags": openPassageTags(); break
        case "manageTags": if (tagsUi.pickerVisible) tagsUi.togglePage(); else openTagsBrowser(true); break
        case "related":
            // Study needs a passage; after opening a chapter nothing is selected yet,
            // so study the first verse, as entering Visual mode does.
            if (!hasSelection && bible.ready && bible.verses.length) {
                bible.selectVerses(bible.verses[0].number, bible.verses[0].number)
                selectionAnchor = bible.cursorVerse
            }
            if (leaveSidePanel("related")) tagsUi.openRelated()
            break
        case "history":
            if (!bible.historyEnabled) { openHistory(); break }
            if (leaveSidePanel("history")) openHistory()
            break
        case "historyBack": historyStep(-count); break
        case "historyForward": historyStep(count); break
        case "historySwap": historySwap(); break
        case "clearHistory": requestClearHistory(); break
        case "toggleHistory": toggleHistoryEnabled(); break
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
        case "soundEffects":
            sounds.enabled = !sounds.enabled
            notify("Sound effects " + (sounds.enabled ? "on" : "off"), true)
            sounds.play("tick")
            break
        case "reduceMotion":
            sounds.reduceMotion = !sounds.reduceMotion
            notify(sounds.reduceMotion ? "Animations off" : "Animations on", true)
            sounds.play("tick")
            break
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
        // Lay out the destination page first: its rows may not exist yet.
        view.contentY = top
        view.forceLayout()
        let row = -1
        let distance = Infinity
        let fallback = view.currentIndex
        let shown = 0
        for (let i = 0; i < view.count; ++i) {
            const item = view.itemAtIndex(i)
            if (!item) continue
            // Without a fully visible row, keep the one most visible on the page.
            const visible = Math.min(item.y + item.height, top + view.height) - Math.max(item.y, top)
            if (visible > shown) { shown = visible; fallback = i }
            // A row overhanging the page by a fraction of a pixel still fits.
            if (item.y < top - 1 || item.y + item.height > top + view.height + 1) continue
            const gap = Math.abs(item.y + item.height / 2 - target)
            if (gap < distance) { distance = gap; row = i }
        }
        if (row < 0) row = fallback
        view.currentIndex = row
        view.contentY = top
    }
    function activateKeybinding(action) {
        if (!action || !actionAvailable(action)) { sounds.play("error"); return }
        if (!flushNote()) return
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
        // Chrome buttons take the keyboard only by tab; a mouse click leaves it where it was.
        focusPolicy: Qt.TabFocus
        property bool keyActivated: false
        Keys.onShortcutOverride: function(event) { app.buttonShortcutOverride(event) }
        Keys.onPressed: function(event) { if (event.key === Qt.Key_Space) button.keyActivated = true }
        Keys.onReturnPressed: function(event) { app.buttonEnter(button, event) }
        Keys.onEnterPressed: function(event) { app.buttonEnter(button, event) }
        Connections { target: button; function onClicked() { app.buttonClicked(button) } }
        AppToolTip { theme: desktop; uiScale: app.scale; visible: (button.hovered || button.activeFocus) && button.tip.length > 0; text: button.tip }
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
    // A scrollable dialog pane: it takes focus, scrolls by key (scrollKey),
    // and keeps the focused control inside it visible (revealFocus).
    component KeyScrollView: OverlayScrollView {
        id: keyScroll
        activeFocusOnTab: true
        focusPolicy: Qt.StrongFocus
        Keys.onPressed: function(event) { app.scrollKey(keyScroll.contentItem as Flickable, event) }
        Connections {
            target: app
            enabled: keyScroll.visible
            function onActiveFocusItemChanged() { app.revealFocus(keyScroll.contentItem as Flickable) }
        }
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
        Keys.onShortcutOverride: function(event) { app.buttonShortcutOverride(event) }
        Keys.onReturnPressed: app.runCommand(catalogAction.id)
        Keys.onEnterPressed: app.runCommand(catalogAction.id)
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
                text: app.keyRich(settingSwitch.catalogAction)
                textFormat: Text.StyledText
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
        // Whether the button can act now; focus mode's quiet footer also hides
        // and disables every button but the persistent one, in place.
        property bool available: true
        property bool persistent: false
        readonly property bool footerButton: true
        readonly property bool quiet: app.footerQuiet && !persistent
        enabled: available && !quiet
        opacity: quiet ? 0 : 1
        Behavior on opacity { NumberAnimation { duration: app.fadeMs } }
        onActiveFocusChanged: app.footerFocusChanged()
        onHoveredChanged: app.updateFooterHover()
        implicitWidth: glyph.length ? implicitHeight : Math.ceil(floatingText.implicitWidth + leftPadding + rightPadding) + 1
        implicitHeight: 40 * app.controlScale
        padding: 8 * app.controlScale
        hoverEnabled: true
        // Footer chrome: the keyboard reaches it by tab only, and a mouse click
        // returns the keyboard to the reader (or the open panel).
        focusPolicy: Qt.TabFocus
        font.pixelSize: 13 * app.scale
        onPressed: app.clearCount()
        property bool keyActivated: false
        Keys.onShortcutOverride: function(event) { app.buttonShortcutOverride(event) }
        Keys.onPressed: function(event) { if (event.key === Qt.Key_Space) floatingButton.keyActivated = true }
        Keys.onReturnPressed: function(event) { app.buttonEnter(floatingButton, event) }
        Keys.onEnterPressed: function(event) { app.buttonEnter(floatingButton, event) }
        Connections { target: floatingButton; function onClicked() { app.buttonClicked(floatingButton) } }
        // Just revealed by hover: a click that was aimed elsewhere is ignored briefly.
        MouseArea {
            anchors.fill: parent
            z: 10
            visible: app.footerGuard && !floatingButton.persistent
            acceptedButtons: Qt.AllButtons
        }
        Accessible.name: tip
        AppToolTip { theme: desktop; uiScale: app.scale; visible: floatingButton.hovered || floatingButton.activeFocus; text: floatingButton.tip }
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
                visible: floatingButton.glyph.length > 0 && floatingButton.glyph !== "saved"
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
                    if (glyph === "search") {
                        ctx.arc(10.5, 10.5, 7, 0, 2 * Math.PI)
                        ctx.moveTo(15.45, 15.45); ctx.lineTo(20, 20)
                    } else if (glyph === "help") {
                        ctx.moveTo(8, 7); ctx.bezierCurveTo(8, 1, 19, 2, 16, 9)
                        ctx.bezierCurveTo(15, 11, 12, 11, 12, 15)
                        ctx.moveTo(12, 19); ctx.lineTo(12, 19.3)
                    } else if (glyph === "desk") {
                        // Stacked pages resting in an open tray.
                        ctx.moveTo(7, 4); ctx.lineTo(17, 4)
                        ctx.moveTo(5, 8); ctx.lineTo(19, 8)
                        ctx.moveTo(2, 13); ctx.lineTo(8, 13); ctx.lineTo(9.5, 16)
                        ctx.lineTo(14.5, 16); ctx.lineTo(16, 13); ctx.lineTo(22, 13)
                        ctx.lineTo(22, 21); ctx.lineTo(2, 21); ctx.closePath()
                    } else if (glyph === "layout-verse") {
                        // Verse: separate short rows, each with a small number tick.
                        for (const y of [5, 12, 19]) {
                            ctx.moveTo(3, y - 1.5); ctx.lineTo(3, y + 1.5)
                            ctx.moveTo(8, y); ctx.lineTo(y === 12 ? 17 : 21, y)
                        }
                    } else if (glyph === "layout-paragraph") {
                        // Paragraph: two text blocks, each first line indented.
                        ctx.moveTo(8, 3); ctx.lineTo(21, 3)
                        ctx.moveTo(3, 7); ctx.lineTo(21, 7)
                        ctx.moveTo(3, 11); ctx.lineTo(14, 11)
                        ctx.moveTo(8, 17); ctx.lineTo(21, 17)
                        ctx.moveTo(3, 21); ctx.lineTo(17, 21)
                    } else if (glyph === "layout-block") {
                        // Block: one solid run of lines.
                        for (const y of [4, 8.5, 13, 17.5]) { ctx.moveTo(3, y); ctx.lineTo(21, y) }
                        ctx.moveTo(3, 22); ctx.lineTo(12, 22)
                    } else if (glyph === "history") {
                        // A clock face with hands at ten past.
                        ctx.arc(12, 12, 9.5, 0, 2 * Math.PI)
                        ctx.moveTo(12, 6.5); ctx.lineTo(12, 12); ctx.lineTo(16, 14)
                    } else if (glyph === "study") {
                        // An open book under a magnifier: reading closely.
                        // The lens hides the book's lower-right corner.
                        ctx.moveTo(18.5, 9.5); ctx.lineTo(18.5, 3.5); ctx.lineTo(13.5, 3.5)
                        ctx.lineTo(10, 5); ctx.lineTo(6.5, 3.5); ctx.lineTo(1.5, 3.5)
                        ctx.lineTo(1.5, 16); ctx.lineTo(6.5, 16); ctx.lineTo(10, 17.5); ctx.lineTo(11, 17.1)
                        ctx.moveTo(10, 5); ctx.lineTo(10, 11.5)
                        ctx.moveTo(4, 7); ctx.lineTo(7, 7)
                        ctx.moveTo(4, 10.5); ctx.lineTo(7, 10.5)
                        ctx.moveTo(22, 22); ctx.lineTo(19.6, 19.6)
                        ctx.stroke()
                        ctx.beginPath()
                        ctx.arc(16, 16, 5, 0, 2 * Math.PI)
                    } else if (glyph === "more") {
                        ctx.fillRect(4, 11, 3, 3); ctx.fillRect(10.5, 11, 3, 3); ctx.fillRect(17, 11, 3, 3)
                    } else if (glyph === "focus") {
                        // A viewfinder: four corner brackets around a dot.
                        ctx.moveTo(3, 8); ctx.lineTo(3, 3); ctx.lineTo(8, 3)
                        ctx.moveTo(16, 3); ctx.lineTo(21, 3); ctx.lineTo(21, 8)
                        ctx.moveTo(21, 16); ctx.lineTo(21, 21); ctx.lineTo(16, 21)
                        ctx.moveTo(8, 21); ctx.lineTo(3, 21); ctx.lineTo(3, 16)
                        ctx.stroke()
                        ctx.beginPath()
                        ctx.arc(12, 12, 2.2, 0, 2 * Math.PI)
                        ctx.fill()
                        ctx.beginPath()
                    } else if (glyph === "characters") {
                        // Two text lines, the second partly selected, and a text caret.
                        ctx.moveTo(3, 6); ctx.lineTo(21, 6)
                        ctx.moveTo(3, 12); ctx.lineTo(9, 12)
                        ctx.moveTo(3, 18); ctx.lineTo(8, 18)
                        ctx.moveTo(16, 9); ctx.lineTo(16, 21)
                        ctx.moveTo(13, 9); ctx.lineTo(19, 9)
                        ctx.moveTo(13, 21); ctx.lineTo(19, 21)
                        ctx.stroke()
                        ctx.beginPath()
                        ctx.fillRect(9, 10, 6, 4)
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
                // Annotations wears the note glyph.
                visible: floatingButton.glyph === "saved"
                kind: "note"
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
        width: 16 * app.readerScale
        // Mouse only: the passage menu's Open bookmark / note / tags reach the same ranges by key.
        focusPolicy: Qt.NoFocus
        readonly property int lane: (app.annotationLanes[rowIndex] || {})[kind] || 0
        x: (36 - lane * 18) * app.readerScale
        padding: 0
        visible: ranges.length > 0 && !bible.focusMode
        Accessible.name: hint
        enabled: !app.rangeEditing
        function openMarker() {
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
        onClicked: openMarker()
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: function(mouse) {
                if (mouse.button === Qt.RightButton) {
                    const p = mapToItem(null, mouse.x, mouse.y)
                    app.openMarkerMenu(marker.choices, marker.kind, function() { marker.openMarker() }, marker.choices, p.x, p.y)
                } else if (mouse.modifiers & Qt.ShiftModifier) app.openRangeEditor(marker.choices, marker.kind)
                else marker.openMarker()
            }
        }
        readonly property int markSeed: HandDrawn.seed(kind, bible.versionId,
            ranges.map(function(r) { return r.reference }).sort().join(","))
        contentItem: Item {
            // Geometry hooks for the rail and end cap; the ink is the
            // hand-drawn pen stroke painted by railPaint inside this item.
            Item {
                id: railLine
                objectName: marker.kind + "Line" + marker.number
                x: 5 * app.readerScale
                y: marker.continuesBefore ? 0 : marker.headingHeight + 8 * app.readerScale
                width: app.readerScale
                height: Math.max(0, (marker.continuesAfter ? parent.height : marker.headingHeight + marker.bodyHeight) - y)
            }
            Item {
                id: railEndCap
                objectName: marker.kind + "EndCap" + marker.number
                x: 5 * app.readerScale; y: marker.headingHeight + marker.bodyHeight - app.readerScale
                width: 7 * app.readerScale; height: app.readerScale
                visible: marker.ends
            }
            Canvas {
                id: railPaint
                objectName: marker.kind + "Rail" + marker.number
                anchors.fill: parent
                property color ink: desktop.accent
                readonly property var paintKey: [railLine.y, railLine.height, railEndCap.y, railEndCap.visible,
                    marker.continuesBefore, marker.continuesAfter, marker.markSeed, app.readerScale, width, height]
                onPaintKeyChanged: requestPaint()
                onInkChanged: requestPaint()
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.reset()
                    const k = app.readerScale
                    const cx = railLine.x + railLine.width / 2
                    if (railLine.height > 0) {
                        ctx.fillStyle = Qt.rgba(ink.r, ink.g, ink.b, 0.65)
                        HandDrawn.wavyLine(ctx, cx, railLine.y, cx, railLine.y + railLine.height,
                            HandDrawn.seed(marker.markSeed, "rail"), k,
                            { width: k, joinStart: marker.continuesBefore, joinEnd: marker.continuesAfter })
                    }
                    if (railEndCap.visible) {
                        const cy = railEndCap.y + railEndCap.height / 2
                        ctx.fillStyle = ink
                        HandDrawn.wavyLine(ctx, railEndCap.x, cy, railEndCap.x + railEndCap.width, cy,
                            HandDrawn.seed(marker.markSeed, "cap"), k, { width: k, amp: 0.6 * k, roughStart: false })
                    }
                }
            }
            AnnotationGlyph {
                objectName: marker.kind + "Glyph" + marker.number
                kind: marker.kind; ink: desktop.accent
                filled: true
                y: marker.headingHeight + 5 * app.readerScale
                width: 14 * app.readerScale; height: 16 * app.readerScale
                visible: marker.starts || (marker.rowIndex === 0 && marker.continuesBefore)
            }
        }
        background: Rectangle {
            color: marker.hovered || marker.activeFocus ? app.tint(desktop.accent, 0.12) : "transparent"
            radius: Math.min(desktop.cornerRadius, width / 2, height / 2)
        }
        AppToolTip { theme: desktop; uiScale: app.scale; visible: marker.visible && (marker.hovered || marker.activeFocus); text: marker.hint + "\n" + KeyNotation.format("Shift+Click") + " · Edit range · " + app.menuKey("editRange") + " in reader" }
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
        // Every modal fades in and closes at once: no exit motion.
        enter: Transition {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: app.fadeMs; easing.type: Easing.OutQuad }
        }
        exit: null
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        Rectangle {
            objectName: "rangeEditStatus"
            Layout.fillWidth: true
            implicitHeight: rangeStatusContent.implicitHeight + 16
            visible: rangeEditor.active
            color: desktop.surface
            ColumnLayout {
                id: rangeStatusContent
                anchors.fill: parent
                anchors.margins: 8
                spacing: 2
                RowLayout {
                    Layout.fillWidth: true
                    spacing: rangeEditKeys.spacing
                    Label {
                        objectName: "rangeEditTitle"
                        Layout.alignment: Qt.AlignTop
                        text: "Edit range · " + rangeEditor.endpoint
                        textFormat: Text.PlainText
                        color: desktop.accent
                        font.pixelSize: 13 * app.scale
                    }
                    HintRow {
                        id: rangeEditKeys
                        objectName: "rangeEditHints"
                        // Fill what the title leaves; the parts wrap there.
                        Layout.fillWidth: true
                        Layout.preferredWidth: 0
                        Layout.alignment: Qt.AlignTop
                        theme: desktop
                        pixelSize: 13 * app.scale
                        parts: app.rangeEditHintParts()
                    }
                }
                Label {
                    objectName: "rangeEditScope"
                    Layout.fillWidth: true
                    text: rangeEditor.source ? rangeEditor.source.scope.reference + " → " + bible.selectionReference + " · " + bible.versionAbbreviation : ""
                    textFormat: Text.PlainText
                    color: desktop.foreground
                    wrapMode: Text.Wrap
                    font.pixelSize: 12 * app.scale
                }
                Label { Layout.fillWidth: true; text: rangeEditor.message; visible: text.length > 0; color: desktop.foreground; wrapMode: Text.Wrap; textFormat: Text.PlainText }
            }
        }
        // Character selection (shift V): the mode, its keys, and what is selected.
        Rectangle {
            objectName: "characterStatus"
            Layout.fillWidth: true
            implicitHeight: characterStatusContent.implicitHeight + 16
            visible: app.charMode
            color: desktop.surface
            RowLayout {
                id: characterStatusContent
                anchors.fill: parent
                anchors.margins: 8
                anchors.leftMargin: 24
                spacing: 16
                Label {
                    objectName: "characterTitle"
                    Layout.alignment: Qt.AlignTop
                    text: "Characters" + (app.charPending.length ? " · " + app.charPending : "")
                        + (app.verseCount.length ? " · " + app.verseCount : "")
                    textFormat: Text.PlainText
                    color: desktop.accent
                    font.pixelSize: 13 * app.scale
                }
                HintRow {
                    objectName: "characterHints"
                    Layout.fillWidth: true
                    Layout.preferredWidth: 0
                    Layout.alignment: Qt.AlignTop
                    theme: desktop
                    pixelSize: 13 * app.scale
                    parts: app.characterHintParts()
                }
            }
        }
        Rectangle {
            objectName: "errorBanner"
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
                QuietButton { objectName: "errorDismiss"; text: "Dismiss"; compact: true; tip: "Dismiss · " + KeyNotation.format("Escape"); onClicked: bible.clearError() }
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
                    if (app.charMode && app.readerBase) { event.accepted = app.charKey(event); return }
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
                    onMovementStarted: readerSlide.stop()
                    ScrollBar.vertical: OverlayScrollbar {
                        id: readerScrollbar
                        objectName: "readerScrollbar"
                        parent: readerSurface
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        anchors.topMargin: 12 * app.readerScale
                        anchors.bottomMargin: app.floatingSafety + 12 * app.readerScale
                        visible: !bible.focusMode && size < 1
                        enabled: visible
                    }

                    Column {
                        id: readerColumn
                        objectName: "readerColumn"
                        // Centered, but clear of the footer's stacked right column;
                        // margin notes may shift it left (app.marginSpace).
                        width: app.readerColumnWidth
                        x: app.marginSpace.columnX + app.marginSlideOffset
                        onHeightChanged: app.scheduleMargins()
                        onWidthChanged: app.scheduleMargins()
                        topPadding: 48 * app.readerScale
                        bottomPadding: app.floatingSafety + 3 * app.readerLineHeight
                        spacing: 0

                        Label {
                            objectName: "chapterTitle"
                            opacity: app.focusDimmedOpacity
                            text: bible.passage
                            color: desktop.foreground
                            font.pixelSize: 34 * app.readerScale
                            font.weight: Font.Medium
                            font.letterSpacing: -0.9
                            leftPadding: app.verseGutter
                            bottomPadding: 24 * app.readerScale
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
                                searchOverlay: verseSearch
                                textOffset: verseHeading.height
                                textHeight: verseBody.height
                                width: readerColumn.width
                                height: verseHeading.height + verseBody.height + 7 * app.readerScale
                                // Focus-mode dimming: paint-only, so layout and hit-testing are unchanged.
                                readonly property bool dimmed: app.verseDimmed(modelData.number)
                                opacity: modelData.number < app.focusFirst || modelData.number > app.focusLast ? app.focusDimmedOpacity : 1
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
                                    height: hasHeading ? headingText.implicitHeight + 36 * app.readerScale : 0
                                    visible: hasHeading
                                    Text {
                                        objectName: "headingPrefix" + verseRow.modelData.number
                                        x: -26 * app.readerScale
                                        y: headingText.y
                                        text: "#"
                                        font: headingText.font
                                        color: app.tint(desktop.accent, 0.6)
                                    }
                                    Text {
                                        id: headingText
                                        objectName: "headingText" + verseRow.modelData.number
                                        width: parent.width
                                        y: 22 * app.readerScale
                                        // An LRM keeps a Hebrew-first heading ("א Aleph")
                                        // in an LTR paragraph; display only.
                                        text: app.leftToRightText(verseRow.modelData.heading)
                                        horizontalAlignment: Text.AlignLeft
                                        textFormat: Text.PlainText
                                        font.family: desktop.fontFamily
                                        font.pixelSize: 15 * app.readerScale
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
                                    visible: ranges.length > 0 && !bible.focusMode && app.inlineNoteMarks
                                }
                                // Annotation tint: one translucent layer per covering range
                                // (HandDrawn.tintCount, capped), joined with neighbouring rows
                                // that keep the layer; highlights paint on top.
                                Canvas {
                                    id: annotationTint
                                    objectName: "annotationTint" + verseRow.modelData.number
                                    readonly property int layers: bible.focusMode ? 0 : HandDrawn.tintCount(verseRow.modelData)
                                    readonly property int layersBefore: verseRow.index > 0 ? HandDrawn.tintCount(bible.verses[verseRow.index - 1]) : 0
                                    readonly property int layersAfter: HandDrawn.tintCount(bible.verses[verseRow.index + 1])
                                    x: verseRow.gutter - 9 * app.readerScale
                                    width: verseRow.width - x
                                    height: verseRow.height
                                    visible: layers > 0
                                    property color ink: desktop.accent
                                    readonly property var paintKey: [layers, layersBefore, layersAfter, width, height, verseHeading.height,
                                        verseBody.height, app.readerScale, bible.versionId]
                                    onPaintKeyChanged: requestPaint()
                                    onInkChanged: requestPaint()
                                    onPaint: {
                                        const ctx = getContext("2d")
                                        ctx.reset()
                                        const k = app.readerScale
                                        ctx.fillStyle = Qt.rgba(ink.r, ink.g, ink.b, HandDrawn.tintAlpha())
                                        for (let layer = 1; layer <= layers; ++layer) {
                                            const before = layersBefore >= layer, after = layersAfter >= layer
                                            const top = before ? 0 : verseHeading.height
                                            const bottom = after ? height : verseHeading.height + verseBody.height + 3
                                            HandDrawn.highlighterBand(ctx, Qt.rect(0, top, width, bottom - top),
                                                HandDrawn.seed("tint", bible.versionId, layer, verseRow.modelData.number), k,
                                                { top: !before, bottom: !after })
                                        }
                                    }
                                }
                                Rectangle {
                                    objectName: "jumpPulse" + verseRow.modelData.number
                                    readonly property bool target: verseRow.modelData.number >= app.pulseFirst && verseRow.modelData.number <= app.pulseLast
                                    x: verseRow.gutter - 9 * app.readerScale
                                    y: verseHeading.height
                                    width: verseRow.width - x
                                    height: verseBody.height + 3
                                    color: desktop.accent
                                    opacity: target && app.pulseActive ? 0.24 * app.pulseLevel : 0
                                    visible: opacity > 0
                                }
                                Rectangle {
                                    objectName: "copyFlash" + verseRow.modelData.number
                                    readonly property bool target: verseRow.modelData.number >= app.flashFirst && verseRow.modelData.number <= app.flashLast
                                    x: verseRow.gutter - 9 * app.readerScale
                                    y: verseHeading.height
                                    width: verseRow.width - x
                                    height: verseBody.height + 3
                                    color: desktop.accent
                                    opacity: target && app.flashActive ? 0.3 * app.flashLevel : 0
                                    visible: opacity > 0
                                }
                                Repeater {
                                    model: verseRow.modelData.highlightRanges || []
                                    delegate: Rectangle {
                                        required property var modelData
                                        objectName: "highlightSegment" + verseRow.modelData.number + ":" + modelData.reference
                                        x: verseRow.gutter - 9 * app.readerScale
                                        y: modelData.highlightContinuesBefore ? 0 : verseHeading.height
                                        width: verseRow.width - x
                                        height: (modelData.highlightContinuesAfter ? verseRow.height : verseHeading.height + verseBody.height + 3) - y
                                        radius: 0
                                        // Painted as a highlighter band by highlightPaint.
                                        color: "transparent"
                                        Item {
                                            id: highlightAccent
                                            objectName: "highlightAccent" + verseRow.modelData.number + ":" + parent.modelData.reference
                                            width: 2; height: parent.height
                                            visible: !bible.focusMode
                                        }
                                        Canvas {
                                            id: highlightPaint
                                            objectName: "highlightPaint" + verseRow.modelData.number + ":" + parent.modelData.reference
                                            anchors.fill: parent
                                            readonly property var range: parent.modelData
                                            property color ink: desktop.accent
                                            readonly property var paintKey: [highlightAccent.visible, app.readerScale, width, height,
                                                range.reference, range.highlightContinuesBefore, range.highlightContinuesAfter, bible.versionId]
                                            onPaintKeyChanged: requestPaint()
                                            onInkChanged: requestPaint()
                                            onPaint: {
                                                const ctx = getContext("2d")
                                                ctx.reset()
                                                const k = app.readerScale
                                                const s = HandDrawn.seed("highlight", bible.versionId, range.reference)
                                                const before = !!range.highlightContinuesBefore, after = !!range.highlightContinuesAfter
                                                ctx.fillStyle = Qt.rgba(ink.r, ink.g, ink.b, 0.16)
                                                HandDrawn.highlighterBand(ctx, Qt.rect(0, 0, width, height),
                                                    HandDrawn.seed(s, verseRow.modelData.number), k,
                                                    { top: !before, bottom: !after })
                                                if (!highlightAccent.visible) return
                                                // Wobble stays right of the accent's left edge,
                                                // inside its reserved three pixels before text.
                                                const amp = HandDrawn.accentAmp(k)
                                                ctx.fillStyle = Qt.rgba(ink.r, ink.g, ink.b, 0.7)
                                                HandDrawn.wavyLine(ctx, 1 + amp, 0, 1 + amp, height,
                                                    HandDrawn.seed(s, "accent", verseRow.modelData.number), k,
                                                    { width: 2, amp: amp, joinStart: before, joinEnd: after })
                                            }
                                        }
                                    }
                                }
                                Rectangle {
                                    objectName: "selectionSegment" + verseRow.modelData.number
                                    x: verseRow.gutter - 9 * app.readerScale
                                    y: verseRow.index > 0 && (bible.verses[verseRow.index - 1]?.number || 0) >= bible.selectedStart ? 0 : verseHeading.height
                                    width: parent.width - x
                                    height: (verseRow.modelData.number < bible.selectedEnd ? verseRow.height : verseHeading.height + verseBody.height + 3) - y
                                    visible: verseRow.selected && !app.charMode
                                    radius: 0
                                    color: app.tint(desktop.accent, 0.22)
                                }
                                Item {
                                    id: verseNumber
                                    objectName: "verseNumber" + verseRow.modelData.number
                                    x: 54 * app.readerScale
                                    width: 17 * app.readerScale
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
                                        anchors.right: parent.right
                                        anchors.rightMargin: 8 * app.readerScale
                                        // Three-digit labels must also fit between the
                                        // packed annotation lane (glyph ends at 50) and
                                        // the 8px clearance before the highlight accent.
                                        width: 12 * app.readerScale
                                        horizontalAlignment: Text.AlignRight
                                        fontSizeMode: Text.HorizontalFit
                                        minimumPixelSize: 1
                                        text: verseRow.modelData.number
                                        font.pixelSize: 10 * app.readerScale
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
                                            else if (mouse.button === Qt.MiddleButton) app.copyClickedVerse(verseRow.modelData.number, false)
                                            else { const p = numberMouse.mapToItem(null, mouse.x, mouse.y); app.openPassageMenu(verseRow.modelData.number, p.x, p.y) }
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
                                        uiScale: app.scale
                                        visible: numberMouse.containsMouse && !bible.focusMode
                                        text: "Select verse " + verseRow.modelData.number + " · " + KeyNotation.format("Shift+Click") + " for a range" + (verseRow.modelData.hasNote ? "\nHas a note" : "") + (verseRow.modelData.bookmarked ? "\nBookmarked" : "") + (verseRow.modelData.hasTags ? "\nTagged" : "")
                                    }
                                }
                                Repeater {
                                    model: bible.focusMode ? [] : (verseRow.modelData.footnotes || [])
                                    delegate: Item {
                                        id: footnoteMarker
                                        required property var modelData
                                        objectName: "verseFootnoteMark:" + verseRow.modelData.number + "." + modelData.seq
                                        readonly property rect cell: verseBody.width > 0 && verseBody.contentHeight > 0
                                            ? verseBody.positionToRectangle(app.footnoteCellPosition(verseRow.modelData.footnotes, modelData)) : Qt.rect(0, 0, 0, 0)
                                        x: verseBody.x + cell.x
                                        y: verseBody.y + cell.y
                                        width: app.footnoteCellWidth
                                        height: cell.height
                                        z: 3
                                        Text {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            text: app.footnoteMark(footnoteMarker.modelData.caller)
                                            color: desktop.accent
                                            font.family: desktop.fontFamily
                                            font.pixelSize: Math.max(6, app.readerSize * app.readerScale * 0.55)
                                            textFormat: Text.PlainText
                                        }
                                        MouseArea {
                                            id: footnoteMouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: app.openFootnote(verseRow.modelData.number, footnoteMarker.modelData.seq)
                                        }
                                        Accessible.role: Accessible.Button
                                        Accessible.name: "Footnote: " + footnoteMarker.modelData.text
                                        AppToolTip {
                                            theme: desktop
                                            uiScale: app.scale
                                            visible: footnoteMouse.containsMouse
                                            text: "Footnote · " + footnoteMarker.modelData.text + "\nClick to read it in Study"
                                        }
                                    }
                                }
                                SearchHighlights {
                                    id: verseSearch
                                    objectName: "verseSearch" + verseRow.modelData.number
                                    markScale: app.readerScale
                                    x: verseBody.x; y: verseBody.y
                                    width: verseBody.width; height: verseBody.height
                                    body: verseBody
                                    sourceText: verseRow.modelData.text
                                    theme: desktop
                                    segments: app.searchSegments.filter(function(segment) { return segment.verse === verseRow.modelData.number })
                                }
                                TextEdit {
                                    id: verseBody
                                    property SearchHighlights searchOverlay: verseSearch
                                    objectName: "verseBody" + verseRow.modelData.number
                                    x: verseRow.gutter + (verseRow.modelData.poetry ? 13 : 0)
                                    y: verseHeading.height
                                    width: parent.width - x - 9
                                    height: contentHeight
                                    text: '<div style="line-height:' + (bible.lineHeight * 100) + '%">'
                                        + (app.footnoteHtml(app.verseHtml(verseRow.modelData.html), verseRow.modelData.footnotes) || verseRow.modelData.text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/\n/g, "<br>")) + '</div>'
                                    textFormat: Text.RichText
                                    horizontalAlignment: TextEdit.AlignLeft
                                    font.family: desktop.fontFamily
                                    font.pixelSize: app.readerSize * app.readerScale
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
                                        // Ctrl a never selects one verse's text: reader selection spans verses.
                                        if (event.key === Qt.Key_A && event.modifiers === Qt.ControlModifier && app.readerKeys) event.accepted = true
                                        if (event.key === Qt.Key_C && event.modifiers === Qt.ControlModifier && app.freeSelectedText.length) { app.copySelectedText(false); event.accepted = true }
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
                                            else if (mouse.button === Qt.MiddleButton) app.copyClickedVerse(verseRow.modelData.number, false)
                                            else { const p = bodyMouse.mapToItem(null, mouse.x, mouse.y); app.openPassageMenu(verseRow.modelData.number, p.x, p.y) }
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
                                onLayoutRefreshed: app.scheduleMargins()
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
                    // Margin notes scroll with the text, beside their passages.
                    Item {
                        id: marginLayer
                        objectName: "marginLayer"
                        width: reading.width
                        height: readerColumn.height
                        visible: app.marginSpace.notes.visible
                        Repeater {
                            id: marginRepeater
                            model: app.marginNoteEntries
                            onCountChanged: app.scheduleMargins()
                            delegate: Item {
                                id: marginNote
                                required property var modelData
                                required property int index
                                readonly property var place: app.marginPlacement(modelData.key)
                                readonly property int naturalLines: measure.lineCount
                                onNaturalLinesChanged: app.scheduleMargins()
                                objectName: "marginNote:" + modelData.reference
                                visible: !!place && !place.hidden
                                x: app.marginSpace.notes.x
                                y: place ? place.y : 0
                                width: app.marginSpace.notes.width
                                height: place ? place.height : 0
                                Accessible.role: Accessible.Button
                                Accessible.name: "Note on " + modelData.reference + ": " + modelData.note
                                Accessible.onPressAction: app.openMarginNote(modelData)
                                Text {
                                    id: measure
                                    visible: false
                                    width: parent.width
                                    text: noteText.text
                                    textFormat: Text.StyledText
                                    wrapMode: Text.Wrap
                                    font: noteText.font
                                    lineHeightMode: Text.FixedHeight
                                    lineHeight: app.marginNoteLineHeight
                                }
                                MouseArea {
                                    id: marginNoteMouse
                                    objectName: "marginNoteMouse:" + marginNote.modelData.reference
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: app.openMarginNote(marginNote.modelData)
                                }
                                Text {
                                    id: noteText
                                    objectName: "marginNoteText:" + marginNote.modelData.reference
                                    width: parent.width
                                    text: app.marginNoteHtml(marginNote.modelData.note)
                                    textFormat: Text.StyledText
                                    wrapMode: Text.Wrap
                                    maximumLineCount: marginNote.place ? marginNote.place.lines : 1
                                    elide: Text.ElideRight
                                    lineHeightMode: Text.FixedHeight
                                    lineHeight: app.marginNoteLineHeight
                                    font.family: desktop.fontFamily
                                    font.italic: true
                                    font.pixelSize: app.marginNotePixelSize
                                    color: desktop.foreground
                                    linkColor: desktop.accent
                                    onLinkActivated: function(link) { app.followMarginLink(link) }
                                }
                                AppToolTip {
                                    theme: desktop
                                    uiScale: app.scale
                                    visible: marginNoteMouse.containsMouse && !noteText.hoveredLink
                                    text: "Note on " + marginNote.modelData.reference + "\nClick to open it in Notes"
                                }
                                // "{" before the note, "}" after the passage text, and an
                                // arrow between their tips (HandDrawn.js, seeded per note).
                                Canvas {
                                    id: connector
                                    objectName: "marginConnector:" + marginNote.modelData.reference
                                    readonly property real k: app.readerScale
                                    readonly property real spanLeft: app.marginTextRight + 3 * k
                                    readonly property real spanRight: marginNote.x - 5 * k
                                    readonly property real spanTop: marginNote.place ? Math.min(marginNote.place.y, marginNote.place.passageTop) - 4 * k : 0
                                    readonly property real spanBottom: marginNote.place ? Math.max(marginNote.place.y + marginNote.place.height, marginNote.place.passageBottom) + 4 * k : 0
                                    property color ink: desktop.accent
                                    // Scene geometry for tests: tips in marginLayer coordinates.
                                    property var geometry: null
                                    x: spanLeft - marginNote.x
                                    y: spanTop - marginNote.y
                                    width: Math.max(1, spanRight - spanLeft)
                                    height: Math.max(1, spanBottom - spanTop)
                                    readonly property var paintKey: [marginNote.place, width, height, x, y, bible.versionId]
                                    onPaintKeyChanged: requestPaint()
                                    onInkChanged: requestPaint()
                                    onPaint: {
                                        const ctx = getContext("2d")
                                        ctx.reset()
                                        const place = marginNote.place
                                        if (!place) return
                                        const s = HandDrawn.seed("marginNote", bible.versionId, marginNote.modelData.key)
                                        const brace = app.marginBraceWidth
                                        const noteMid = (place.y + place.y + place.height) / 2 + place.tips.note - spanTop
                                        const passageMid = (place.passageTop + place.passageBottom) / 2 + place.tips.passage - spanTop
                                        ctx.fillStyle = Qt.rgba(ink.r, ink.g, ink.b, 0.85)
                                        const close = HandDrawn.wavyBrace(ctx, 0, place.passageTop - spanTop, place.passageBottom - spanTop, passageMid,
                                            brace, 1, HandDrawn.seed(s, "passage"), k)
                                        const open = HandDrawn.wavyBrace(ctx, width - brace, place.y - spanTop, place.y + place.height - spanTop, noteMid,
                                            brace, -1, HandDrawn.seed(s, "note"), k)
                                        ctx.fillStyle = Qt.rgba(ink.r, ink.g, ink.b, 0.7)
                                        HandDrawn.wavyArrow(ctx, open.tip.x - 2 * k, open.tip.y, close.tip.x + 2.5 * k, close.tip.y,
                                            HandDrawn.seed(s, "arrow"), k)
                                        geometry = { noteTip: { x: open.tip.x + spanLeft, y: open.tip.y + spanTop }, passageTip: { x: close.tip.x + spanLeft, y: close.tip.y + spanTop },
                                            arrowEnd: { x: close.tip.x + 2.5 * k + spanLeft, y: close.tip.y + spanTop } }
                                    }
                                }
                            }
                        }
                    }
                }
                ChapterOutline {
                    id: chapterOutline
                    objectName: "chapterOutline"
                    visible: app.marginSpace.outline.visible
                    x: app.marginSpace.outline.x
                    y: 48 * app.readerScale
                    width: app.marginSpace.outline.width
                    height: Math.max(0, reading.height - app.floatingSafety - y - 12 * app.readerScale)
                    headings: app.outlineHeadings
                    title: bible.passage
                    current: app.outlineCurrent
                    theme: desktop
                    uiScale: app.readerScale
                    onActivated: function(verse) { app.jumpToHeading(verse) }
                    onActivatedTop: if (bible.verses.length) app.jumpToHeading(bible.verses[0].number)
                    scrollKey: app.scrollKey
                    onLeave: app.readerFocus()
                }
                // A continuous fade keeps floating controls legible without a
                // footer surface, divider, or shortening the reading viewport.
                Rectangle {
                    id: readingBottomFade
                    objectName: "readingBottomFade"
                    // In focus mode only while the footer is revealed.
                    readonly property bool shown: !bible.focusMode || !app.footerQuiet
                    visible: opacity > 0
                    opacity: shown ? 1 : 0
                    Behavior on opacity { NumberAnimation { duration: app.fadeMs } }
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: Math.max(72 * app.systemScale, app.footerHeight + 2 * app.footerMargin) + 24 * app.scale
                    // Fully opaque from just above the footer's top row, so reader
                    // text never shows through the footer controls.
                    readonly property real opaqueFrom: Math.min(0.6, Math.max(0.05,
                        (height - app.footerHeight - app.footerMargin - 8 * app.controlScale) / Math.max(1, height)))
                    gradient: Gradient {
                        GradientStop { position: 0; color: app.tint(desktop.background, 0) }
                        GradientStop { position: readingBottomFade.opaqueFrom; color: desktop.background }
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
        // Qt blocks window shortcuts behind any visible modal popup, including
        // one in its exit transition; a closing drawer stops being modal so the
        // reader's keys (Escape, t, …) work immediately.
        modal: !closing
        dim: true
        focus: true
        interactive: false
        padding: 0
        // Auto-close cannot veto a failed save. All dismissals use closeNotes().
        closePolicy: Popup.NoAutoClose
        // Slide plus a short fade. A closing drawer no longer counts as open
        // (`shown`), so reader keys and a reopen work during its exit animation.
        property bool closing: false
        readonly property bool shown: visible && !closing
        enter: Transition {
            SmoothedAnimation { velocity: app.reduceMotion ? 100000 : 5 }
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: app.fadeMs; easing.type: Easing.OutQuad }
        }
        exit: Transition {
            SmoothedAnimation { velocity: app.reduceMotion ? 100000 : 5 }
            NumberAnimation { property: "opacity"; from: 1; to: 0; duration: app.fadeMs; easing.type: Easing.InQuad }
        }
        // While closing, a drawer must not keep its Escape grab: the next Escape
        // belongs to the reader (or range editor) immediately.
        onAboutToShow: { const reopening = visible; closing = false; if (reopening) focusContent() }
        onAboutToHide: { closing = true; app.restorePanelFocus() }
        // Keyboard focus is available as soon as the drawer starts fading in.
        function focusContent() {
            if (app.modalOpen) return
            if (focusEditor && app.hasSelection) app.focusNoteEnd()
            else if (app.hasSelection && app.notePreview) notePreviewText.forceActiveFocus()
            else notesPanel.forceActiveFocus()
        }
        onVisibleChanged: if (visible && !closing) focusContent()
        background: Rectangle { radius: Math.min(desktop.cornerRadius, width / 2, height / 2); color: desktop.background; border.color: desktop.border }
        Overlay.modal: Rectangle {
            color: app.tint(desktop.background, 0.65)
        }
        Overlay.modeless: Rectangle { color: app.tint(desktop.background, 0.65) }
        onClosed: { closing = false; app.editingPanelClosed(); app.restorePanelFocus() }
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
                    Connections {
                        target: app
                        enabled: notesDrawer.visible
                        function onActiveFocusItemChanged() { app.revealFocus(notesScroll.contentItem as Flickable) }
                    }
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
                        QuietButton { text: bible.selectionBookmarked ? "Bookmarked" : "Bookmark"; emphasized: bible.selectionBookmarked; compact: true; onClicked: app.runCommand("bookmark"); tip: app.keyText("bookmark") }
                        QuietButton { text: bible.selectionHighlighted ? "Highlighted" : "Highlight"; emphasized: bible.selectionHighlighted; compact: true; onClicked: app.runCommand("highlight"); tip: app.keyText("highlight") }
                        Item { Layout.fillWidth: true }
                        QuietButton { objectName: "notesCopy"; text: "Copy"; compact: true; onClicked: app.copyPassage(); tip: "Copy passage as quotation · " + app.keyText("copy") }
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
                            }
                            QuietButton {
                                objectName: "noteUseSaved"
                                text: "Use saved text"
                                onClicked: app.loadSavedNote()
                            }
                        }
                    }
                    RowLayout {
                        visible: app.hasSelection
                        spacing: 4
                        QuietButton { objectName: "writeButton"; text: "Write"; compact: true; emphasized: !app.notePreview; tip: "Write · " + app.keyText("notePreview"); onClicked: { app.notePreview = false; noteEditor.forceActiveFocus() } }
                        QuietButton { objectName: "previewButton"; text: "Preview"; compact: true; emphasized: app.notePreview; tip: "Preview · " + app.keyText("notePreview"); onClicked: { if (app.flushNote()) { app.notePreview = true; notePreviewText.forceActiveFocus() } } }
                        Item { Layout.fillWidth: true }
                        Label { text: "Markdown"; color: desktop.muted; font.pixelSize: 11 * app.scale }
                    }
                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        OverlayScrollView {
                            id: noteEditorScroll
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
                                    app.updateLinkSuggestions()
                                }
                                onCursorPositionChanged: app.updateLinkSuggestions()
                                Keys.onPressed: function(event) {
                                    const enter = event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                                    // Tab never types: it accepts a [[ suggestion, else moves to Linked from.
                                    if (event.key === Qt.Key_Tab && event.modifiers === Qt.NoModifier) {
                                        event.accepted = true
                                        if (app.linkSuggesting && app.linkSuggestions.length) app.acceptLinkSuggestion()
                                        else app.focusLinkedFrom()
                                        return
                                    }
                                    if (event.key === Qt.Key_Backtab || (event.key === Qt.Key_Tab && event.modifiers === Qt.ShiftModifier)) { event.accepted = true; return }
                                    if (enter && event.modifiers === Qt.AltModifier) {
                                        event.accepted = true
                                        app.openNoteLinkAtCursor()
                                        return
                                    }
                                    if (!app.linkSuggesting || event.modifiers !== Qt.NoModifier || !app.linkSuggestions.length) return
                                    if (event.key === Qt.Key_Down || event.key === Qt.Key_Up) {
                                        app.moveLinkSuggestion(event.key === Qt.Key_Down ? 1 : -1); event.accepted = true
                                    } else if (enter || event.key === Qt.Key_Tab) event.accepted = app.acceptLinkSuggestion()
                                }
                                // Specific key handlers run before onPressed.
                                Keys.onEscapePressed: function(event) {
                                    if (app.linkSuggesting) app.dismissLinkSuggestions()
                                    else app.closeNotes()
                                    event.accepted = true
                                }
                                Accessible.name: "Markdown note for " + bible.selectionReference
                            }
                        }
                        OverlayScrollView {
                            id: notePreviewScroll
                            objectName: "notePreviewScroll"
                            anchors.fill: parent
                            visible: app.hasSelection && app.notePreview
                            clip: true
                            contentWidth: availableWidth
                            TextArea {
                                id: notePreviewText
                                objectName: "notePreview"
                                width: parent.width
                                // Focusable for reading by key: the scroll keys move the preview.
                                activeFocusOnTab: true
                                Keys.onPressed: function(event) { app.scrollKey(notePreviewScroll.contentItem as Flickable, event) }
                                text: noteEditor.text.length ? app.linkedMarkdown(noteEditor.text) : "*Your note preview will appear here.*"
                                textFormat: TextEdit.MarkdownText
                                readOnly: true
                                selectByMouse: true
                                wrapMode: TextEdit.Wrap
                                color: noteEditor.text.length ? desktop.foreground : desktop.muted
                                font.pixelSize: 15 * app.scale
                                padding: 2
                                background: Item {}
                                onLinkActivated: function(link) { app.activateNoteLink(link) }
                                Accessible.name: "Note preview"
                            }
                        }
                        Label {
                            anchors.top: parent.top
                            width: parent.width
                            visible: !app.hasSelection
                            text: "Select a verse number to start a note.\n\n" + KeyNotation.format("Shift+Click") + " another number to gather a range. Your notes are saved automatically as you write."
                            color: desktop.muted
                            font.pixelSize: 14 * app.scale
                            lineHeight: 1.5
                            wrapMode: Text.Wrap
                        }
                    }
                    HintRow {
                        objectName: "notesHints"
                        visible: noteEditor.activeFocus || notePreviewText.activeFocus
                        theme: desktop
                        parts: app.linkSuggesting ? [[["Up", "Down"], "choose"], ["Return", "insert", function() { app.acceptLinkSuggestion() }, app.linkSuggestions.length > 0],
                                ["Escape", "dismiss", app.dismissLinkSuggestions]]
                            : notePreviewText.activeFocus ? [[["J", "K"], "scroll"], [app.actionKeys("notePreview")[0], "write", app.toggleNotePreview]]
                            : [app.noteLinkAtCursor || app.noteUrlAtCursor.length ? ["Alt+Return", "open link", app.openNoteLinkAtCursor] : null,
                                [app.actionKeys("notePreview")[0], "preview", app.toggleNotePreview],
                                app.linkedFrom.length ? [app.actionKeys("noteLinkedFrom")[0], "linked from", app.focusLinkedFrom] : null]
                        pixelSize: 11 * app.scale
                        Layout.fillWidth: true
                    }
                    ColumnLayout {
                        id: linkedList
                        objectName: "linkedFrom"
                        // Reached by tab from the note: j k move, enter opens,
                        // y / shift Y copy, d Desk, esc (or tab) returns to the note.
                        property int currentIndex: 0
                        visible: app.linkedFrom.length > 0
                        Layout.fillWidth: true
                        spacing: 2
                        Keys.onShortcutOverride: function(event) { if (app.linkedKey(event, false)) event.accepted = true }
                        Keys.onPressed: function(event) { app.linkedKey(event, true) }
                        SectionLabel { text: "LINKED FROM"; Layout.fillWidth: true }
                        Repeater {
                            id: linkedRepeater
                            model: app.linkedFrom
                            delegate: ItemDelegate {
                                id: linkedRow
                                required property var modelData
                                required property int index
                                objectName: "linkedFromRow" + index
                                readonly property bool current: linkedList.activeFocus && linkedList.currentIndex === index
                                Layout.fillWidth: true
                                padding: 6 * app.scale
                                focusPolicy: Qt.NoFocus
                                onClicked: app.openLinkedNote(modelData)
                                TapHandler {
                                    acceptedButtons: Qt.RightButton
                                    onTapped: function(eventPoint) { app.openLinkedMenu(linkedRow.index, eventPoint.scenePosition.x, eventPoint.scenePosition.y) }
                                }
                                contentItem: ColumnLayout {
                                    spacing: 2
                                    Label { text: linkedRow.modelData.reference; textFormat: Text.PlainText; color: desktop.foreground; font.pixelSize: 13 * app.scale; Layout.fillWidth: true; elide: Text.ElideRight }
                                    Label { text: linkedRow.modelData.excerpt; textFormat: Text.PlainText; color: desktop.muted; font.pixelSize: 12 * app.scale; Layout.fillWidth: true; elide: Text.ElideRight }
                                }
                                background: Rectangle {
                                    objectName: "linkedFromRowBackground" + linkedRow.index
                                    radius: Math.min(desktop.cornerRadius, width / 2, height / 2)
                                    color: linkedRow.current ? app.tint(desktop.accent, 0.11) : linkedRow.hovered ? desktop.surface : "transparent"
                                }
                                Accessible.name: "Linked from " + modelData.reference
                            }
                        }
                        HintRow {
                            objectName: "linkedHints"
                            visible: linkedList.activeFocus
                            theme: desktop
                            readonly property var row: app.linkedFrom[linkedList.currentIndex] || null
                            parts: [[["Up", "Down"], "choose"], ["Return", "open", function() { app.openLinkedNote(app.linkedFrom[linkedList.currentIndex]) }, !!row],
                                ["Y", "copy", function() { app.copyScope(app.linkedFrom[linkedList.currentIndex]) }, !!row],
                                ["D", "Desk", function() { app.toggleScopeDesk(app.linkedFrom[linkedList.currentIndex]) }, !!row],
                                ["Escape", "back to the note", function() { noteEditor.forceActiveFocus() }]]
                            pixelSize: 11 * app.scale
                            Layout.fillWidth: true
                        }
                    }
                    Label { text: app.noteConflict ? "Not saved · resolve the other window's change above" : app.hasSelection ? (noteEditor.text === bible.noteText ? "Saved on this device · changes save as you type" : "Not saved · your draft is still in the editor") : "A page icon beside a verse marks a note."; color: desktop.muted; font.pixelSize: 11 * app.scale; wrapMode: Text.Wrap; Layout.fillWidth: true }
                }
                }
                Rectangle {
                    id: linkPopup
                    objectName: "noteLinkPopup"
                    visible: app.linkSuggesting
                    z: 20
                    readonly property point at: {
                        const caret = noteEditor.cursorRectangle
                        // Re-evaluated as either view scrolls.
                        const scrolled = [noteEditorScroll.contentItem.contentY, notesScroll.contentItem.contentY]
                        return scrolled && noteEditor.mapToItem(notesPanel, caret.x, caret.y + caret.height)
                    }
                    width: Math.min(320 * app.scale, notesPanel.width - 16)
                    height: linkColumn.implicitHeight + 12
                    x: Math.max(8, Math.min(notesPanel.width - width - 8, at.x))
                    y: Math.min(at.y + 4, notesPanel.height - height - 8)
                    color: desktop.surface
                    border.color: desktop.border
                    radius: Math.min(desktop.cornerRadius, width / 2, height / 2)
                    Column {
                        id: linkColumn
                        x: 6; y: 6
                        width: parent.width - 12
                        Label {
                            visible: !app.linkSuggestions.length
                            width: parent.width
                            padding: 6 * app.scale
                            text: app.linkQuery.trim().length ? "No matching passage" : "Type a passage · jn316, ps23, 1co13"
                            color: desktop.muted
                            font.pixelSize: 12 * app.scale
                        }
                        Repeater {
                            model: app.linkSuggestions
                            delegate: Rectangle {
                                id: suggestion
                                required property var modelData
                                required property int index
                                objectName: "noteLinkSuggestion" + index
                                width: linkColumn.width
                                height: suggestionLabel.implicitHeight + 10 * app.scale
                                radius: Math.min(desktop.cornerRadius, width / 2, height / 2)
                                color: index === app.linkSuggestionIndex ? app.tint(desktop.accent, 0.11) : "transparent"
                                Label {
                                    id: suggestionLabel
                                    anchors.verticalCenter: parent.verticalCenter
                                    x: 6 * app.scale
                                    text: suggestion.modelData.reference
                                    textFormat: Text.PlainText
                                    color: desktop.foreground
                                    font.pixelSize: 13 * app.scale
                                }
                                MouseArea { anchors.fill: parent; onClicked: app.acceptLinkSuggestion(suggestion.index) }
                            }
                        }
                    }
                }
        }
    }

    MouseArea {
        objectName: "notesOutside"
        parent: Overlay.overlay
        // Cover only the dimmed outside region. Drawer consumes its own outside
        // events before the dimmer, so guard dismissal here without auto-close.
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: app.width - notesDrawer.width * notesDrawer.position
        z: notesDrawer.z + 0.1
        visible: app.notesOpen && !app.modalOpen
        acceptedButtons: Qt.AllButtons
        onPressed: app.closeNotes()
        onWheel: function(wheel) { wheel.accepted = true }
    }
    // Outside an open side panel, a press only closes the panel: it never
    // reaches the reader or footer beneath (a Drawer alone lets it through).
    // Hidden while a dialog, picker, or menu sits above the panel.
    component DrawerOutside: MouseArea {
        id: outside
        required property Drawer drawer
        property bool shown: drawer.visible
        signal dismiss()
        parent: Overlay.overlay
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: app.width - drawer.width * drawer.position
        z: outside.drawer.z + 0.1
        visible: shown && !app.dialogOpen
        acceptedButtons: Qt.AllButtons
        onPressed: outside.dismiss()
        onWheel: function(wheel) { wheel.accepted = true }
    }
    DrawerOutside { objectName: "savedOutside"; drawer: savedDrawer; shown: savedDrawer.shown; onDismiss: savedDrawer.close() }
    DrawerOutside { objectName: "deskOutside"; drawer: deskDrawer; shown: deskDrawer.shown && !cleanDeskConfirmation.visible; onDismiss: deskDrawer.close() }
    DrawerOutside { objectName: "historyOutside"; drawer: historyDrawer; shown: historyDrawer.shown && !clearHistoryConfirmation.visible; onDismiss: historyDrawer.close() }
    DrawerOutside { objectName: "studyOutside"; drawer: tagsUi.studyDrawer; shown: tagsUi.relatedShown && !tagsUi.pickerVisible; onDismiss: tagsUi.close() }

    // Footer: ? and mode on the left; search, passage, version, and layout in
    // the center; Annotations, Study, History (while enabled), and Desk on the right
    // (Notes is `w`). Buttons in a group sit edge to edge (no gaps). The center
    // stays centered while it fits. When cramped, the right group stacks into a
    // column at the bottom-right corner and the reader text keeps clear of it;
    // the footer stays one row tall. More cramped hides the layout button;
    // really cramped folds the right group into "…" and hides ?. The reader
    // keeps the footer row inside floatingSafety.
    // Focus mode keeps the footer but shows only the focus button. Resting the
    // pointer on the footer for 250 ms reveals the rest where they always sit
    // (their clicks wait 300 ms more, so a click aimed at the text is not
    // caught); keyboard focus entering the footer reveals it at once.
    property bool footerHoverRevealed: false
    readonly property bool footerFocused: !!activeFocusItem && activeFocusItem.footerButton === true
    readonly property bool footerQuiet: bible.focusMode && !footerHoverRevealed && !footerFocused
    readonly property bool footerGuard: footerGuardTimer.running
    Timer { id: footerRevealTimer; interval: 250; onTriggered: { app.footerHoverRevealed = true; footerGuardTimer.restart() } }
    Timer { id: footerGuardTimer; interval: 300 }
    // The pointer is on the footer: its zone, or a (revealed) button over it.
    function updateFooterHover() {
        let hovered = footerZone.containsMouse
        for (const group of [leftActions, locationControls, floatingActions])
            for (const child of group.children) if (child.footerButton === true && child.hovered) hovered = true
        if (hovered && bible.focusMode && !footerHoverRevealed) { if (!footerRevealTimer.running) footerRevealTimer.restart() }
        else if (!hovered) { footerRevealTimer.stop(); footerHoverRevealed = false }
    }
    function footerFocusChanged() { if (footerFocused) footerGuardTimer.stop() }
    readonly property real footerMargin: 16 * controlScale
    // Clearance between reader text and the stacked right column.
    readonly property real footerGap: 4 * controlScale
    readonly property real footerButton: 40 * controlScale
    readonly property real footerGroupGap: 24 * controlScale
    readonly property int footerRightCount: bible.ready ? (bible.historyEnabled ? 4 : 3) : 0
    // The focus button joins the left group where the footer has room for it
    // without changing any other fold (and always in focus mode).
    readonly property real footerLeftBase: bible.ready ? 2 * footerButton : footerButton
    readonly property bool footerFocusFits: bible.ready
        && (footerCenterFull + 2 * Math.max(footerLeftBase + footerButton, footerRightFull) + 2 * footerGroupGap <= footerAvailable
            || footerCenterMin + layoutControl.implicitWidth + 2 * (Math.max(footerLeftBase + footerButton, footerRightCount ? footerButton : 0) + footerGroupGap) <= footerAvailable)
    readonly property bool footerShowsFocus: bible.ready && (footerFocusFits || bible.focusMode)
    readonly property real footerLeftFull: footerLeftBase + (footerShowsFocus ? footerButton : 0)
    readonly property real footerRightFull: footerRightCount * footerButton
    readonly property real footerCenterFull: searchControl.implicitWidth + passageControl.implicitWidth
        + (bible.ready ? versionControl.implicitWidth + layoutControl.implicitWidth : 0)
    readonly property real footerAvailable: width - 2 * footerMargin
    readonly property bool footerStacked: footerCenterFull + 2 * Math.max(footerLeftFull, footerRightFull) + 2 * footerGroupGap > footerAvailable
    // With the right group stacked, the center keeps clear of the wider corner.
    readonly property real footerSideReserve: Math.max(footerLeftFull, footerRightCount ? footerButton : 0) + footerGroupGap
    readonly property real footerPassageMin: 96 * controlScale
    // The passage elides first; the layout button goes only when the passage
    // would drop below footerPassageMin, and the right group folds into "…"
    // only when even that is not enough.
    readonly property real footerCenterMin: searchControl.implicitWidth + footerPassageMin + (bible.ready ? versionControl.implicitWidth : 0)
    readonly property bool footerHideLayout: footerStacked && bible.ready
        && footerCenterMin + layoutControl.implicitWidth + 2 * footerSideReserve > footerAvailable
    readonly property bool footerCollapsed: footerStacked && bible.ready && footerCenterMin + 2 * footerSideReserve > footerAvailable
    readonly property real footerHeight: footerButton
    // Room the reader text leaves at the right edge for the stacked column.
    readonly property real footerRightInset: footerStacked && !footerCollapsed && footerRightCount && !bible.focusMode
        ? footerButton + footerMargin + 2 * footerGap : 0
    function footerPanelEntries() {
        const ready = !rangeEditing && bible.ready && !bible.busy
        return [
            { id: "annotations", text: "Annotations", shortcut: menuKey("annotations"), enabled: ready, run: function() { app.runCommand("annotations") } },
            { id: "notes", text: "Notes", shortcut: menuKey("notes"), enabled: ready, run: function() { app.toggleNotes() } },
            { id: "related", text: "Study", shortcut: menuKey("related"), enabled: ready && hasSelection, run: function() { app.runCommand("related") } },
            bible.historyEnabled ? { id: "history", text: "History", shortcut: menuKey("history"), enabled: ready, run: function() { app.runCommand("history") } } : null,
            { id: "desk", text: "Desk", shortcut: menuKey("desk"), enabled: ready, run: function() { app.runCommand("desk") } }
        ].filter(function(entry) { return !!entry })
    }
    // Hover only (no buttons): clicks fall through to the text beneath.
    MouseArea {
        id: footerZone
        objectName: "footerZone"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: app.footerHeight + 2 * app.footerMargin
        visible: bible.ready && bible.focusMode && !app.findPromptOpen
        acceptedButtons: Qt.NoButton
        hoverEnabled: true
        onContainsMouseChanged: app.updateFooterHover()
    }
    Row {
        id: leftActions
        objectName: "leftActions"
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: app.footerMargin
        spacing: 0
        visible: !app.findPromptOpen
        FloatingButton {
            objectName: "helpControl"
            glyph: "help"
            tip: "Keybindings · " + app.keyText("help")
            visible: !app.footerCollapsed
            available: !app.rangeEditing && !bible.busy && !app.modalOpen
            onClicked: helpDialog.open()
        }
        FloatingButton {
            id: modeControl
            objectName: "modeControl"
            glyph: app.charMode ? "characters" : app.verseMode ? "visual" : "normal"
            selected: app.verseMode || app.charMode
            visible: bible.ready
            available: !app.rangeEditing && !bible.busy && !app.modalOpen
            tip: app.charMode ? "Character selection · switch to normal · " + KeyNotation.format("Escape")
                : (app.verseMode ? "Visual mode · switch to normal" : "Normal mode · switch to visual") + " · " + app.keyText("visual")
                  + "\nCharacters · " + app.keyText("characterVisual")
            onClicked: {
                if (app.charMode) app.endCharMode()
                else app.setVisualMode(!app.verseMode)
                app.readerFocus()
            }
        }
        FloatingButton {
            id: focusControl
            objectName: "focusControl"
            glyph: "focus"
            // The one footer button focus mode always shows.
            persistent: true
            selected: bible.focusMode
            visible: app.footerShowsFocus
            available: !bible.busy && !app.modalOpen && app.actionAvailable("focus")
            tip: (bible.focusMode ? "Focus mode · leave" : "Focus mode") + " · " + app.keyText("focus")
            onClicked: { app.runCommand("focus"); app.readerFocus() }
        }
    }
    Row {
        id: locationControls
        objectName: "locationControls"
        anchors.bottom: parent.bottom
        anchors.bottomMargin: app.footerMargin
        x: app.width / 2 - width / 2
        readonly property real availableWidth: Math.max(0, app.width - 2 * (Math.max(leftActions.width, floatingActions.width) + app.footerMargin + app.footerGroupGap))
        readonly property real fixedWidth: searchControl.width + (versionControl.visible ? versionControl.width : 0) + (layoutControl.visible ? layoutControl.width : 0)
        width: Math.min(fixedWidth + passageControl.implicitWidth, availableWidth)
        visible: !app.findPromptOpen
        FloatingButton {
            id: searchControl
            objectName: "searchControl"
            glyph: "search"
            tip: "Search Bible · " + app.keyText("bibleSearch")
            available: bible.ready && !app.rangeEditing && !bible.busy && !app.modalOpen
            onClicked: app.showPaletteMode("search")
        }
        FloatingButton {
            id: passageControl
            objectName: "passageControl"
            width: Math.max(0, locationControls.width - locationControls.fixedWidth)
            text: bible.ready ? (bible.selectionReference || bible.passage) : "Open a passage"
            tip: (app.hasSelection ? bible.selectionReference : bible.passage) + " · Open passage · " + app.keyText("books")
            available: !app.rangeEditing && !bible.busy && !app.modalOpen
            onClicked: app.openPalette(false)
        }
        FloatingButton {
            id: versionControl
            objectName: "versionControl"
            width: Math.min(implicitWidth, locationControls.availableWidth * 0.4)
            text: bible.versionAbbreviation
            tip: bible.edition + " · Switch version · " + app.keyText("switchVersion")
            visible: bible.ready
            available: !app.rangeEditing && !bible.busy && !app.modalOpen
            onClicked: app.openVersions(false)
        }
        FloatingButton {
            id: layoutControl
            objectName: "layoutControl"
            glyph: "layout-" + bible.readingMode
            tip: "Layout · " + app.readingModeName(bible.readingMode) + " · " + app.keyText("readingMode")
            visible: bible.ready && !app.footerHideLayout
            available: !app.rangeEditing && !bible.busy && !app.modalOpen
            onClicked: app.runCommand("readingMode")
        }
    }
    Grid {
        id: floatingActions
        objectName: "floatingActions"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: app.footerMargin
        spacing: 0
        // One row, or a column bottom-aligned in the corner when cramped.
        columns: app.footerStacked ? 1 : 5
        visible: !app.findPromptOpen
        readonly property bool expanded: bible.ready && !app.footerCollapsed
        FloatingButton {
            objectName: "savedControl"
            visible: floatingActions.expanded
            glyph: "saved"
            selected: savedDrawer.visible
            tip: "Annotations · " + app.keyText("annotations")
            available: !app.rangeEditing && bible.ready && !bible.busy && !app.modalOpen
            onClicked: app.runCommand("annotations")
        }
        FloatingButton {
            objectName: "relatedControl"
            glyph: "study"
            visible: floatingActions.expanded
            available: !app.rangeEditing && bible.ready && !bible.busy && !app.modalOpen
            tip: "Study · " + app.keyText("related")
            onClicked: app.runCommand("related")
        }
        FloatingButton {
            objectName: "historyControl"
            glyph: "history"
            visible: floatingActions.expanded && bible.historyEnabled
            selected: historyDrawer.visible
            tip: "History · " + app.keyText("history")
            available: !app.rangeEditing && bible.ready && !bible.busy && !app.modalOpen
            onClicked: app.runCommand("history")
        }
        FloatingButton {
            objectName: "deskControl"
            visible: floatingActions.expanded
            glyph: "desk"
            selected: deskDrawer.visible
            tip: "Desk · " + app.keyText("desk")
            available: !app.rangeEditing && bible.ready && !bible.busy && !app.modalOpen
            onClicked: app.runCommand("desk")
        }
        FloatingButton {
            id: footerMore
            objectName: "footerMore"
            visible: bible.ready && app.footerCollapsed
            glyph: "more"
            tip: "Panels"
            available: !app.rangeEditing && bible.ready && !bible.busy && !app.modalOpen
            onClicked: {
                const p = footerMore.mapToItem(null, 0, 0)
                const entries = app.footerPanelEntries()
                contextMenu.show("footer", entries, p.x, p.y - entries.length * 30 * app.scale - 8 * app.scale)
            }
        }
    }

    // While the find prompt is open, a click in the reader accepts the current
    // match and closes the prompt (or cancels when nothing matches); the click
    // itself selects nothing.
    MouseArea {
        objectName: "findClickCatcher"
        anchors.fill: parent
        visible: app.findPromptOpen
        acceptedButtons: Qt.AllButtons
        onPressed: app.acceptFindByClick()
        onWheel: function(wheel) { wheel.accepted = false }
    }
    ChapterFind {
        id: chapterFind
        objectName: "chapterFind"
        fadeDuration: app.reduceMotion ? 0 : 120
        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
        anchors.margins: 16 * app.controlScale
        theme: desktop
        uiScale: app.scale
        matchIndex: app.searchMatchIndex
        matchCount: app.searchMatches.length
        error: app.searchError
        onPreview: function(query) { app.previewChapterFind(query) }
        onAccepted: function(query) { app.acceptChapterFind(query) }
        onCanceled: app.cancelChapterFind()
    }
    Label {
        id: searchMatchCount
        objectName: "searchMatchCount"
        anchors.right: parent.right; anchors.bottom: parent.bottom
        anchors.rightMargin: 20 * app.controlScale + app.footerRightInset
        // Above the footer row, which focus mode keeps (quiet, revealed on hover).
        anchors.bottomMargin: app.footerMargin + app.footerHeight + 6 * app.controlScale
        visible: !app.findPromptOpen && app.acceptedSearchQuery.length > 0
        text: app.searchError || (app.searchMatches.length ? (app.searchMatchIndex < 0
            ? app.searchMatches.length + " matches" : (app.searchMatchIndex + 1) + " / " + app.searchMatches.length) : "No matches")
        color: desktop.muted; font.pixelSize: 12 * app.scale
    }
    Label {
        objectName: "searchQueryLabel"
        anchors.left: parent.left; anchors.right: searchMatchCount.left
        anchors.leftMargin: 20 * app.controlScale; anchors.rightMargin: 16 * app.controlScale
        anchors.baseline: searchMatchCount.baseline
        visible: searchMatchCount.visible
        text: "/ " + app.acceptedSearchQuery
        textFormat: Text.PlainText; elide: Text.ElideRight
        color: desktop.muted; font.pixelSize: 12 * app.scale
    }

    ModalCard {
        id: versionDialog
        objectName: "versionDialog"
        property bool removing: false
        property int step: 0
        property var chosen: null
        property bool removeAnnotations: false
        readonly property int deskCount: chosen && removeAnnotations && bible.desk.length >= 0 ? bible.deskCountForVersion(chosen.id) : 0
        property bool addPending: false
        readonly property real uiScale: Math.min(app.scale, Math.max(1, (app.height - 48) / 240))
        height: Math.min(540 * app.scale, app.height - 48)
        // Focus at once, while the modal is still fading in.
        onVisibleChanged: if (visible) { versionSearch.forceActiveFocus() }
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
                color: desktop.foreground; font.pixelSize: 22 * versionDialog.uiScale
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
                    tip: "Add version · " + app.keyText("addVersion")
                    Accessible.name: tip
                    visible: !versionDialog.removing
                    enabled: !bible.busy
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
                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                text: versionEntry.modelData.hint || ((versionEntry.modelData.active ? "Current version" : "Installed locally")
                                    + (versionEntry.modelData.needsReimport ? " · older text format: re-import for footnotes, cross references, and words of Jesus" : ""))
                                color: desktop.muted; font.pixelSize: 11 * versionDialog.uiScale
                                Layout.fillWidth: true; wrapMode: Text.Wrap
                            }
                            QuietButton {
                                objectName: "versionReimport:" + (versionEntry.modelData.id || "")
                                visible: !versionDialog.removing && versionDialog.step === 0 && !!versionEntry.modelData.needsReimport
                                text: "Re-import"
                                compact: true
                                font.pixelSize: 12 * versionDialog.uiScale
                                enabled: !bible.busy && !catalog.busy
                                tip: (app.catalogEntry(versionEntry.modelData.id) ? "Download " + versionEntry.modelData.abbreviation + " again from eBible.org and replace its text; annotations are kept"
                                    : "Choose " + versionEntry.modelData.abbreviation + "'s USFM file again and replace its text; annotations are kept") + " · " + app.keyText("reimportVersion")
                                onClicked: app.reimportVersion(versionEntry.modelData)
                            }
                        }
                    }
                }
                Label {
                    objectName: "versionEmpty"
                    anchors.centerIn: parent
                    width: parent.width
                    text: bible.busy ? "Please wait…" : bible.versions.length ? "No matching versions" : "No versions installed. Add version to begin."
                    visible: versionResults.count === 0 || bible.busy
                    color: desktop.muted; font.pixelSize: 13 * versionDialog.uiScale
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
                        + (versionDialog.removeAnnotations ? "Delete its notes, bookmarks, and highlights"
                            + (versionDialog.deskCount > 0 ? ", and " + versionDialog.deskCount + (versionDialog.deskCount === 1 ? " Desk item" : " Desk items") : "")
                            + ". This cannot be undone."
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
            HintRow {
                objectName: "versionHints"
                theme: desktop
                parts: app.versionHintParts()
                pixelSize: 11 * versionDialog.uiScale
                Layout.fillWidth: true
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
        Shortcut { sequence: "Tab"; enabled: passagePalette.visible && !contextMenu.visible; onActivated: app.cyclePaletteMode(1) }
        Shortcut { sequence: "Shift+Tab"; enabled: passagePalette.visible && !contextMenu.visible; onActivated: app.cyclePaletteMode(-1) }
        Shortcut {
            sequences: ["Menu", "Shift+F10"]
            enabled: passagePalette.visible && !contextMenu.visible && paletteResults.currentIndex >= 0 && !!app.paletteRowMenu(paletteResults.model[paletteResults.currentIndex])
            onActivated: { const p = app.sceneRowPoint(paletteResults.currentItem); app.openSearchMenu(paletteResults.currentIndex, p.x, p.y) }
        }
        // Fades in with input focus already live (ModalCard); closes at once.
        // Focus at once, while the modal is still fading in.
        onVisibleChanged: if (visible) { referenceInput.forceActiveFocus() }
        onAboutToHide: {
            if (!app.paletteCleared) app.rememberPalette()
        }
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
                event.accepted = !app.paletteTextMode && app.paletteBook > 0 && event.modifiers === Qt.NoModifier
                if (event.accepted) app.backToBooks()
            }
            Item {
                Layout.fillWidth: true
                implicitHeight: referenceInput.implicitHeight
                TextField {
                    id: referenceInput
                    objectName: "referenceInput"
                    anchors.fill: parent
                    implicitHeight: 38 * app.scale
                    font.pixelSize: 15 * app.scale
                    color: desktop.foreground
                    placeholderText: app.paletteChapter ? app.paletteBookName + " " + app.paletteChapter + " — verse number"
                        : app.paletteBook ? app.paletteBookName + " — chapter number" : "John 3:16–18, / text, or > command"
                    placeholderTextColor: desktop.muted
                    selectByMouse: true
                    leftPadding: 14
                    rightPadding: 14
                    background: Rectangle { color: desktop.surface; radius: Math.min(desktop.cornerRadius, width / 2, height / 2); border.color: referenceInput.activeFocus ? app.tint(desktop.accent, 0.65) : desktop.border }
                    onTextChanged: {
                        app.paletteCleared = false; app.paletteArrowUsed = false
                        app.scheduleBibleSearch(); paletteResults.currentIndex = 0
                    }
                    Keys.onPressed: function(event) {
                        if (app.paletteListKey(event)) return
                        const mode = event.modifiers === Qt.ControlModifier
                            ? ({ [Qt.Key_K]: "books", [Qt.Key_Slash]: "bibleSearch", [Qt.Key_Space]: "commands" })[event.key] : undefined
                        if (mode) { event.accepted = true; app.runCommand(mode); return }
                        if ((event.text === "/" || event.text === ">")
                                && (!text.length || selectedText === text)) {
                            text = event.text + " "; cursorPosition = text.length
                            event.accepted = true
                        } else if (event.key === Qt.Key_Backspace && event.modifiers === Qt.NoModifier
                                && (text === "/ " || text === "> ") && cursorPosition === 2 && !selectedText.length) {
                            text = ""; event.accepted = true
                        }
                    }
                    // Dim only the recognized prefix, retaining native input,
                    // cursor movement, selection, and horizontal scrolling.
                    Item {
                        parent: referenceInput.parent
                        z: 10
                        // Paint above the native glyph node, clipped to the
                        // padded input viewport as the text scrolls horizontally.
                        anchors.fill: referenceInput
                        anchors.leftMargin: referenceInput.leftPadding
                        anchors.rightMargin: referenceInput.rightPadding
                        clip: true
                        Rectangle {
                            objectName: "palettePrefix"
                            readonly property rect prefixRect: {
                                const text = referenceInput.text
                                const cursor = referenceInput.cursorRectangle
                                return referenceInput.positionToRectangle(0)
                            }
                            x: prefixRect.x
                            y: prefixRect.y + referenceInput.topPadding
                            width: {
                                const text = referenceInput.text
                                const cursor = referenceInput.cursorRectangle
                                return referenceInput.positionToRectangle(1).x - referenceInput.positionToRectangle(0).x
                            }
                            height: prefixRect.height
                            color: desktop.surface
                            visible: (referenceInput.text.startsWith("/") || referenceInput.text.startsWith(">"))
                                && referenceInput.selectionStart === referenceInput.selectionEnd
                            Text {
                                text: referenceInput.text.charAt(0)
                                font: referenceInput.font
                                color: desktop.muted
                            }
                        }
                    }
                    onAccepted: app.acceptPalette()
                    Keys.onDownPressed: app.movePalette(1)
                    Keys.onUpPressed: app.movePalette(-1)

                    Keys.onLeftPressed: function(event) {
                        event.accepted = !app.paletteTextMode && event.modifiers === Qt.NoModifier && app.paletteBook > 0
                            && selectedText.length === 0 && (!text.length || cursorPosition === 0 || app.paletteArrowUsed)
                        if (event.accepted) app.backToBooks()
                    }
                    Keys.onRightPressed: function(event) {
                        let entry = paletteResults.model[paletteResults.currentIndex]
                        let browsing = (!text.trim() || /^[1-3]?\s*[A-Za-z ]+$/.test(text.trim()) || (app.paletteBook > 0 && /^\d+$/.test(text.trim())) || text.trim().startsWith(">"))
                            && (app.paletteArrowUsed || (cursorPosition === text.length && selectedText.length === 0))
                        event.accepted = !app.paletteTextMode && event.modifiers === Qt.NoModifier && browsing && !!entry
                        if (event.accepted) app.activatePaletteEntry(entry)
                    }
                    Keys.onEscapePressed: app.escapePalette()
                    Accessible.name: "Passage reference, / Bible search, or > command"
                }
            }
            PanelTabs {
                Layout.fillWidth: true
                labels: ["Passage", "Search", "Command"]
                names: ["paletteBooks", "paletteSearch", "paletteCommands"]
                theme: desktop
                uiScale: app.scale
                uniform: true
                enabledTabs: [bible.ready, bible.ready, true]
                currentIndex: referenceInput.text.startsWith(">") ? 2 : app.paletteTextMode ? 1 : 0
                onActivated: index => { if (index === 0) app.openPalette(false); else if (index === 1) app.openTextSearch(); else app.openPalette(true) }
            }
            Label {
                objectName: "referencePreview"
                visible: !app.paletteTextMode && !referenceInput.text.trim().startsWith(">") && (referenceInput.text.trim().length > 0 || app.paletteBook > 0)
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
                visible: !app.paletteTextMode && app.paletteBook > 0
                emphasized: activeFocus
                onClicked: app.backToBooks()
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
                    height: app.paletteTextMode ? searchResultContent.implicitHeight + 20 * app.scale : 44 * app.scale
                    // Keep right-aligned hints clear of the overlay scrollbar.
                    rightPadding: leftPadding + (paletteResults.ScrollBar.vertical.visible ? 12 * app.scale : 0)
                    highlighted: ListView.isCurrentItem
                    enabled: !modelData.action || app.actionAvailable(modelData.action)
                    onClicked: app.activatePaletteEntry(modelData)
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        enabled: !!paletteEntry.modelData.recentQuery || !!paletteEntry.modelData.recent || app.paletteTextMode
                        onTapped: function(eventPoint) { app.openSearchMenu(paletteEntry.index, eventPoint.scenePosition.x, eventPoint.scenePosition.y) }
                    }
                    contentItem: ColumnLayout {
                        id: searchResultContent
                        spacing: 6 * app.scale
                        RowLayout {
                            Layout.fillWidth: true
                            Label { text: paletteEntry.modelData.title || ((paletteEntry.modelData.reference || "") + " · " + (paletteEntry.modelData.versionAbbreviation || "")); textFormat: Text.PlainText; color: paletteEntry.enabled ? desktop.foreground : desktop.muted; Layout.fillWidth: true; elide: Text.ElideRight; font.pixelSize: 14 * app.scale }
                            Label { objectName: "paletteHint" + (paletteEntry.modelData.action || ""); text: paletteEntry.modelData.hint || ""; color: desktop.muted; font.pixelSize: 11 * app.scale }
                            Label {
                                objectName: "paletteChevron" + paletteEntry.index
                                text: "›"
                                visible: !app.paletteTextMode && !!paletteEntry.modelData.book && !paletteEntry.modelData.verse
                                    && (!paletteEntry.modelData.chapter || bible.versePicker)
                                color: desktop.muted
                                Layout.leftMargin: 8
                            }
                        }
                        Item {
                            visible: app.paletteTextMode
                            Layout.fillWidth: true
                            implicitHeight: resultText.contentHeight
                            SearchHighlights {
                                anchors.fill: resultText
                                body: resultText; sourceText: resultText.text; theme: desktop; markScale: app.scale
                                outlined: true
                                segments: paletteEntry.modelData.displaySegments || []
                            }
                            TextEdit {
                                id: resultText
                                objectName: "searchResultText" + paletteEntry.index
                                width: parent.width; height: contentHeight
                                text: app.paletteTextMode ? paletteEntry.modelData.text || "" : ""
                                textFormat: TextEdit.PlainText; wrapMode: TextEdit.Wrap
                                horizontalAlignment: TextEdit.AlignLeft
                                readOnly: true; selectByMouse: false; activeFocusOnPress: false
                                enabled: false
                                color: desktop.foreground; font.pixelSize: 13 * app.scale
                            }
                        }
                    }
                    background: Rectangle { radius: Math.min(desktop.cornerRadius, width / 2, height / 2); color: paletteEntry.highlighted ? app.tint(desktop.accent, 0.11) : paletteEntry.hovered ? desktop.surface : "transparent" }
                    // A thin rule below the recent passages, before the books.
                    FineRule {
                        objectName: "paletteRecentRule"
                        visible: !!paletteEntry.modelData.lastRecent && paletteEntry.index < paletteResults.count - 1
                        parent: paletteEntry
                        x: 0; width: paletteEntry.width
                        y: paletteEntry.height + Math.floor(paletteResults.spacing / 2)
                    }
                }
                Label {
                    anchors.centerIn: parent
                    width: parent.width - 32
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: app.paletteTextMode ? (!app.paletteTextQuery.length ? 'Search words or "an exact phrase"'
                        : !app.paletteSearchCurrent || bible.searchBusy ? "Searching…"
                        : bible.searchError || "No matching verses")
                        : referenceInput.text.startsWith(">") ? "No matching commands" : "Add version to browse passages."
                    visible: paletteResults.count === 0 && (app.paletteTextMode || referenceInput.text.startsWith(">") || !bible.ready)
                    color: desktop.muted
                    font.pixelSize: 13 * app.scale
                }
            }
            RowLayout {
                visible: app.paletteTextMode && app.paletteSearchCurrent && !bible.searchBusy && paletteResults.count > 0
                Layout.fillWidth: true
                Label { text: paletteResults.count + " of " + bible.searchResultCount + " results"; color: desktop.muted; font.pixelSize: 12 * app.scale; Layout.fillWidth: true }
                QuietButton { objectName: "searchLoadMore"; text: "Show more"; compact: true; visible: paletteResults.count < bible.searchResultCount; onClicked: bible.loadMoreSearchResults() }
            }
            FineRule { Layout.fillWidth: true }
            RowLayout {
                Layout.fillWidth: true
                HintRow {
                    objectName: "paletteHints"
                    theme: desktop
                    parts: app.paletteHintParts()
                    pixelSize: 11 * app.scale
                    Layout.fillWidth: true
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
        // Qt blocks window shortcuts behind any visible modal popup, including
        // one in its exit transition; a closing drawer stops being modal so the
        // reader's keys (Escape, t, …) work immediately.
        modal: !closing
        dim: true
        focus: true
        padding: 16 * app.controlScale
        topPadding: padding
        bottomPadding: padding
        leftPadding: padding
        rightPadding: padding
        // Esc is handled below: it first leaves the search field.
        closePolicy: closing || deleteConfirmation.visible || tagsUi.visibleModal ? Popup.NoAutoClose : Popup.CloseOnPressOutside
        readonly property bool tabKeys: shown && !contextMenu.visible && !deleteConfirmation.visible && !tagsUi.visibleModal && !rangeEditor.visibleModal && !app.textInputActive()
        readonly property bool collectionKeys: tabKeys
        background: Rectangle { radius: Math.min(desktop.cornerRadius, width / 2, height / 2); color: desktop.background; border.color: desktop.border }
        Overlay.modal: Rectangle { color: app.tint(desktop.background, 0.65) }
        Overlay.modeless: Rectangle { color: app.tint(desktop.background, 0.65) }
        // Slide plus a short fade. A closing drawer no longer counts as open
        // (`shown`), so reader keys and a reopen work during its exit animation.
        property bool closing: false
        readonly property bool shown: visible && !closing
        enter: Transition {
            SmoothedAnimation { velocity: app.reduceMotion ? 100000 : 5 }
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: app.fadeMs; easing.type: Easing.OutQuad }
        }
        exit: Transition {
            SmoothedAnimation { velocity: app.reduceMotion ? 100000 : 5 }
            NumberAnimation { property: "opacity"; from: 1; to: 0; duration: app.fadeMs; easing.type: Easing.InQuad }
        }
        // While closing, a drawer must not keep its Escape grab: the next Escape
        // belongs to the reader (or range editor) immediately.
        // A fresh open (not one interrupting the exit animation) starts without a query.
        onAboutToShow: { const reopening = visible; if (!closing) app.clearSavedQuery(); closing = false; if (reopening) focusContent() }
        onAboutToHide: { closing = true; app.restorePanelFocus() }
        function focusContent() { app.focusSavedList() }
        onVisibleChanged: if (visible && !closing) {
            if (app.collectionTab === 3) { app.refreshTagExcerpts(); collectionList.currentIndex = app.initialSavedRow() }
            focusContent()
        }
        onClosed: { closing = false; app.restorePanelFocus() }
        Connections {
            target: bible
            enabled: savedDrawer.visible && app.collectionTab === 3
            function onTagsChanged() { app.refreshTagExcerpts() }
        }
        contentItem: ColumnLayout {
            objectName: "savedContent"
            Shortcut {
                sequence: "Escape"
                enabled: savedDrawer.shown && !contextMenu.visible && !deleteConfirmation.visible && !tagsUi.visibleModal && !rangeEditor.visibleModal
                onActivated: app.escapeSaved()
            }
            Shortcut { sequence: "/"; enabled: savedDrawer.tabKeys && app.savedSearchVisible; onActivated: app.focusSavedSearch() }
            Shortcut { sequences: ["J", "Down"]; enabled: savedDrawer.collectionKeys; onActivated: app.moveSaved(1) }
            Shortcut { sequences: ["K", "Up"]; enabled: savedDrawer.collectionKeys; onActivated: app.moveSaved(-1) }
            ListMotionKeys { objectName: "savedMotionKeys"; active: savedDrawer.collectionKeys; count: collectionList.count; onMove: function(delta) { app.moveSaved(delta) } }
            Shortcut { sequences: ["H", "Left"]; enabled: savedDrawer.tabKeys; onActivated: app.filterSaved(-1) }
            Shortcut { sequences: ["L", "Right"]; enabled: savedDrawer.tabKeys; onActivated: app.filterSaved(1) }
            Shortcut { sequence: "Shift+E"; enabled: savedDrawer.collectionKeys && !bible.busy; onActivated: app.editSavedRange() }
            Shortcut { sequences: ["Return", "Enter"]; enabled: savedDrawer.shown && !deleteConfirmation.visible && app.activeFocusItem === collectionList; onActivated: app.openSaved(collectionList.model[collectionList.currentIndex]) }
            Shortcut { sequences: ["Delete", "Backspace"]; enabled: savedDrawer.collectionKeys && !bible.busy; onActivated: app.requestDeleteSaved() }
            // Shared list keys: y quotation, shift Y plain text, d Desk.
            Shortcut { sequence: "Y"; enabled: savedDrawer.collectionKeys; onActivated: app.copyScope(app.focusedSavedEntry()) }
            Shortcut { sequence: "Shift+Y"; enabled: savedDrawer.collectionKeys; onActivated: app.copyScope(app.focusedSavedEntry(), true) }
            Shortcut { sequence: "D"; enabled: savedDrawer.collectionKeys; onActivated: app.toggleScopeDesk(app.focusedSavedEntry()) }
            // Tags: t edits the row's tags over the drawer; ctrl t manages tags.
            Shortcut {
                objectName: "savedTagKey"
                sequence: "T"
                enabled: savedDrawer.collectionKeys && app.collectionTab === 3 && !bible.busy && !!app.focusedSavedEntry()
                onActivated: app.openPassageTags(tagsUi.snapshot(app.focusedSavedEntry()))
            }
            Shortcut { sequences: app.actionKeys("manageTags"); enabled: savedDrawer.collectionKeys && app.collectionTab === 3 && !bible.busy; onActivated: tagsUi.openManage() }
            Shortcut {
                sequences: ["Menu", "Shift+F10"]
                enabled: savedDrawer.collectionKeys && collectionList.currentIndex >= 0
                onActivated: { const p = app.sceneRowPoint(collectionList.currentItem); app.openSavedMenu(collectionList.currentIndex, p.x, p.y) }
            }
            spacing: (savedDrawer.height < 350 * app.scale ? 6 : 12) * app.scale
            PanelHeader {
                objectName: "savedHeader"
                Layout.fillWidth: true
                theme: desktop
                title: "Annotations"
                subtitle: tagsUi.passageSubtitle
                uiScale: app.scale
                titleScale: app.controlScale
                titleName: "savedTitle"
                subtitleName: "savedPassage"
            }
            PanelTabs {
                id: savedTabs
                objectName: "savedTabs"
                Layout.fillWidth: true
                labels: ["Bookmarks", "Highlights", "Notes", "Tags"]
                theme: desktop
                uiScale: app.controlScale
                namePrefix: "savedTab"
                currentIndex: app.collectionTab
                onActivated: index => app.filterSaved(index - app.collectionTab)
            }
            FineRule { Layout.fillWidth: true }
            TextField {
                id: savedSearch
                objectName: "savedSearch"
                Layout.fillWidth: true
                visible: app.savedSearchVisible
                implicitHeight: 36 * app.controlScale
                font.pixelSize: 14 * app.controlScale
                color: desktop.foreground
                placeholderText: "Search " + ["bookmarks", "highlights", "notes", "tagged passages"][app.collectionTab] + "…"
                placeholderTextColor: desktop.muted
                selectByMouse: true
                leftPadding: 10 * app.controlScale; rightPadding: 10 * app.controlScale
                text: app.savedQuery
                onTextEdited: {
                    app.savedQuery = text
                    collectionList.currentIndex = collectionList.count ? 0 : -1
                    // Clearing a short tab's query hides the field: keep the keyboard in the list.
                    if (!app.savedSearchVisible) Qt.callLater(app.focusSavedList)
                }
                background: Rectangle { radius: Math.min(desktop.cornerRadius, width / 2, height / 2); color: desktop.surface; border.color: savedSearch.activeFocus ? desktop.accent : desktop.border }
                Keys.onDownPressed: app.focusSavedList()
                Keys.onReturnPressed: app.focusSavedList()
                Keys.onEnterPressed: app.focusSavedList()
                Accessible.name: "Search annotations"
            }
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
                // Tags rows sit under "name · count" headings; headings are part
                // of the row's block, so j/k and the motions never stop on them.
                delegate: Column {
                    id: savedBlock
                    required property var modelData
                    required property int index
                    width: collectionList.width
                    Label {
                        objectName: "savedTagGroup:" + (savedBlock.modelData.group || "")
                        visible: app.collectionTab === 3 && !!savedBlock.modelData.firstOfGroup
                        width: parent.width
                        text: (savedBlock.modelData.group || "") + " · " + (savedBlock.modelData.groupCount || 0)
                        textFormat: Text.PlainText
                        color: desktop.muted
                        font.pixelSize: 12 * app.scale; font.weight: Font.Medium
                        elide: Text.ElideRight
                        leftPadding: 12 * app.scale
                        topPadding: savedBlock.index ? 10 * app.scale : 0
                        bottomPadding: 4 * app.scale
                    }
                SavedEntry {
                    id: savedEntry
                    modelData: savedBlock.modelData
                    index: savedBlock.index
                    objectName: app.collectionTab === 3 ? "savedTagEntry:" + modelData.group + ":" + modelData.reference : "savedEntry" + modelData.reference
                    // Like Desk and History rows: the list keeps the keyboard (tab
                    // never stops on a row); the current row is the list's.
                    focusPolicy: Qt.NoFocus
                    width: collectionList.width
                    padding: 12 * app.scale
                    height: savedContent.implicitHeight + topPadding + bottomPadding
                    clip: true
                    highlighted: collectionList.currentIndex === savedBlock.index
                    onClicked: { collectionList.currentIndex = savedBlock.index; app.openSaved(modelData) }
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: function(eventPoint) { app.openSavedMenu(savedEntry.index, eventPoint.scenePosition.x, eventPoint.scenePosition.y) }
                    }
                    // Each tab shows only its own annotation kind: Bookmarks the
                    // bookmark, Highlights the highlight mark, Notes the note.
                    readonly property bool showBookmark: app.collectionTab === 0 && !!modelData.bookmark
                    readonly property bool showHighlight: app.collectionTab === 1 && !!modelData.highlight
                    readonly property bool showNote: app.collectionTab === 2 && modelData.note.length > 0
                    readonly property bool showTag: app.collectionTab === 3
                    // Tags: the reader's range (equal or overlapping) is "here".
                    readonly property bool here: { app.savedHereKey; return showTag && app.savedHere(modelData) }
                    // Other tags on the range; omitted when the group's tag is its only one.
                    readonly property string otherTags: showTag && Array.from(modelData.tags || []).length > 1 ? app.tagsPlainText(modelData.tags).replace(/, /g, " · ") : ""
                    contentItem: RowLayout {
                        id: savedContent
                        objectName: "savedEntryContent" + savedEntry.modelData.reference
                        spacing: 12 * app.scale
                        ColumnLayout {
                            Layout.alignment: Qt.AlignTop
                            spacing: 7 * app.scale
                            visible: savedEntry.showBookmark || savedEntry.showNote || savedEntry.showTag
                            BookmarkGlyph { objectName: "savedBookmark" + savedEntry.modelData.reference; visible: savedEntry.showBookmark; Layout.alignment: Qt.AlignHCenter }
                            NoteGlyph { objectName: "savedNote" + savedEntry.modelData.reference; visible: savedEntry.showNote; Layout.alignment: Qt.AlignHCenter }
                            AnnotationGlyph {
                                objectName: "savedTag" + savedEntry.modelData.reference
                                visible: savedEntry.showTag
                                kind: "tag"; ink: desktop.accent
                                implicitWidth: 14 * app.scale; implicitHeight: 16 * app.scale
                                Layout.alignment: Qt.AlignHCenter
                            }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 7 * app.scale
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8 * app.scale
                                Label { text: savedEntry.modelData.reference; textFormat: Text.PlainText; color: desktop.foreground; font.pixelSize: 16 * app.scale; font.weight: Font.Medium; Layout.fillWidth: true; wrapMode: Text.Wrap }
                                Label {
                                    objectName: "savedHere" + savedEntry.modelData.reference
                                    visible: savedEntry.here
                                    text: "here"
                                    color: desktop.accent
                                    font.pixelSize: 11 * app.scale
                                    Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                                }
                            }
                            Label { objectName: "savedTagLine" + savedEntry.modelData.reference; text: savedEntry.otherTags; textFormat: Text.PlainText; visible: text.length > 0; color: desktop.accent; font.pixelSize: 12 * app.scale; elide: Text.ElideRight; Layout.fillWidth: true }
                            Label { objectName: "savedExcerpt" + savedEntry.modelData.reference; text: savedEntry.showTag ? app.leftToRightText(savedEntry.modelData.excerpt || "") : ""; textFormat: Text.PlainText; visible: text.length > 0; color: desktop.muted; font.pixelSize: 13 * app.scale; maximumLineCount: 1; elide: Text.ElideRight; wrapMode: Text.Wrap; Layout.fillWidth: true }
                            Label { objectName: "savedNoteText" + savedEntry.modelData.reference; text: savedEntry.showNote ? savedEntry.modelData.note.replace(/\s+/g, " ") : ""; textFormat: Text.PlainText; visible: text.length > 0; color: desktop.muted; font.pixelSize: 12 * app.scale; maximumLineCount: 3; elide: Text.ElideRight; wrapMode: Text.Wrap; Layout.fillWidth: true }
                        }
                    }
                    background: Rectangle {
                        color: savedEntry.highlighted ? app.tint(desktop.accent, 0.11) : savedEntry.hovered ? desktop.surface : "transparent"
                        radius: Math.min(desktop.cornerRadius, width / 2, height / 2)
                        // The current row is shown by its tinted fill alone, never an accent border.
                        border.width: 0
                        Canvas {
                            // The reader's hand-drawn highlight accent: same pen and seed.
                            objectName: "savedHighlight" + savedEntry.modelData.reference
                            visible: savedEntry.showHighlight
                            width: 3 * app.scale
                            height: parent.height
                            property color ink: desktop.accent
                            readonly property var paintKey: [visible, app.scale, width, height,
                                savedEntry.modelData.reference, savedEntry.modelData.versionId, bible.versionId]
                            onPaintKeyChanged: requestPaint()
                            onInkChanged: requestPaint()
                            onPaint: {
                                const ctx = getContext("2d")
                                ctx.reset()
                                if (!visible) return
                                const k = app.scale
                                const amp = 0.6 * k
                                const s = HandDrawn.seed("highlight", savedEntry.modelData.versionId || bible.versionId,
                                    savedEntry.modelData.reference)
                                ctx.fillStyle = Qt.rgba(ink.r, ink.g, ink.b, 0.7)
                                HandDrawn.wavyLine(ctx, 1 + amp, 0, 1 + amp, height, HandDrawn.seed(s, "accent"), k,
                                    { width: 2, amp: amp })
                            }
                        }
                    }
                }
                }
                Label {
                    objectName: "savedEmpty"
                    anchors.centerIn: parent
                    width: parent.width - 24
                    text: app.collectionTab === 3
                        ? (app.savedQuery.trim().length && app.collectionForTab().length ? "No matching tagged passages."
                            : Array.from(bible.tags).some(function(tag) { return tag.count > 0 }) ? "No tagged passages in " + bible.versionAbbreviation + ". Tags in other versions stay with them."
                            : "No tagged passages yet. Press " + app.keyText("tags") + " on a passage to tag it.")
                        : app.savedQuery.trim().length && app.collectionForTab().length ? "No matching annotations."
                        : ["No bookmarks yet.", "No highlights yet.", "No notes yet."][app.collectionTab] || ""
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    color: desktop.muted
                    font.pixelSize: 13 * app.scale
                    lineHeight: 1.2
                    visible: collectionList.count === 0
                }
            }
            // Each tab's keys, above the footer (dropped when the drawer is too short for its list).
            HintRow {
                objectName: "savedHints"
                visible: savedDrawer.height >= 420 * app.scale
                theme: desktop
                parts: app.savedHintParts()
                pixelSize: 11 * app.scale
                Layout.fillWidth: true
            }
            FineRule { Layout.fillWidth: true }
            RowLayout {
                Layout.fillWidth: true
                QuietButton { objectName: "savedExport"; text: "Export notes…"; compact: true; Layout.fillWidth: true; Layout.minimumWidth: 0; onClicked: { savedDrawer.close(); app.runCommand("export") } }
                Item { Layout.fillWidth: true }
                QuietButton { objectName: "savedBackup"; text: "Backup…"; compact: true; Layout.fillWidth: true; Layout.minimumWidth: 0; onClicked: { savedDrawer.close(); app.runCommand("backup") } }
            }
        }
    }

    Drawer {
        id: deskDrawer
        objectName: "deskDrawer"
        width: Math.min(520 * app.scale, app.width - 48)
        height: app.height
        edge: Qt.RightEdge
        // Qt blocks window shortcuts behind any visible modal popup, including
        // one in its exit transition; a closing drawer stops being modal so the
        // reader's keys (Escape, t, …) work immediately.
        modal: !closing
        dim: true
        focus: true
        padding: 16 * app.controlScale
        topPadding: padding
        bottomPadding: padding
        leftPadding: padding
        rightPadding: padding
        closePolicy: closing || cleanDeskConfirmation.visible || errorDialog.visible ? Popup.NoAutoClose : Popup.CloseOnEscape | Popup.CloseOnPressOutside
        readonly property bool listKeys: shown && !contextMenu.visible && !cleanDeskConfirmation.visible && !errorDialog.visible && !bible.busy && !app.textInputActive()
        background: Rectangle { radius: Math.min(desktop.cornerRadius, width / 2, height / 2); color: desktop.background; border.color: desktop.border }
        Overlay.modal: Rectangle { color: app.tint(desktop.background, 0.65) }
        Overlay.modeless: Rectangle { color: app.tint(desktop.background, 0.65) }
        // Slide plus a short fade. A closing drawer no longer counts as open
        // (`shown`), so reader keys and a reopen work during its exit animation.
        property bool closing: false
        readonly property bool shown: visible && !closing
        enter: Transition {
            SmoothedAnimation { velocity: app.reduceMotion ? 100000 : 5 }
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: app.fadeMs; easing.type: Easing.OutQuad }
        }
        exit: Transition {
            SmoothedAnimation { velocity: app.reduceMotion ? 100000 : 5 }
            NumberAnimation { property: "opacity"; from: 1; to: 0; duration: app.fadeMs; easing.type: Easing.InQuad }
        }
        // While closing, a drawer must not keep its Escape grab: the next Escape
        // belongs to the reader (or range editor) immediately.
        onAboutToShow: { const reopening = visible; closing = false; if (reopening) focusContent() }
        onAboutToHide: { closing = true; app.restorePanelFocus() }
        function focusContent() { app.restoreDeskRow(); app.moveDesk(0) }
        onVisibleChanged: if (visible && !closing) focusContent()
        onClosed: { closing = false; app.restorePanelFocus() }
        contentItem: ColumnLayout {
            objectName: "deskContent"
            spacing: (deskDrawer.height < 350 * app.scale ? 6 : 12) * app.scale
            Shortcut { sequences: ["J", "Down"]; enabled: deskDrawer.listKeys; onActivated: app.moveDesk(1) }
            Shortcut { sequences: ["K", "Up"]; enabled: deskDrawer.listKeys; onActivated: app.moveDesk(-1) }
            ListMotionKeys { objectName: "deskMotionKeys"; active: deskDrawer.listKeys; count: deskList.count; onMove: function(delta) { app.moveDesk(delta) } }
            Shortcut { sequence: "Home"; enabled: deskDrawer.listKeys; onActivated: app.moveDesk(-deskList.count) }
            Shortcut { sequence: "End"; enabled: deskDrawer.listKeys; onActivated: app.moveDesk(deskList.count) }
            Shortcut { sequences: ["Return", "Enter"]; enabled: deskDrawer.listKeys && app.activeFocusItem === deskList; onActivated: { const entry = deskList.model[deskList.currentIndex]; if (entry) app.openDeskItem(entry.id); else sounds.play("error") } }
            Shortcut { sequence: "Y"; enabled: deskDrawer.listKeys; onActivated: { const entry = deskList.model[deskList.currentIndex]; if (entry) app.copyDeskItem(entry.id); else sounds.play("error") } }
            Shortcut { sequence: "Shift+Y"; enabled: deskDrawer.listKeys; onActivated: { const entry = deskList.model[deskList.currentIndex]; if (entry) app.copyDeskItem(entry.id, true); else sounds.play("error") } }
            // d (the Desk toggle) and delete remove one passage; shift delete asks to clean the Desk.
            Shortcut { sequences: ["D", "Delete", "Backspace"]; enabled: deskDrawer.listKeys; onActivated: { if (deskList.count) app.removeCurrentDeskItem(); else sounds.play("error") } }
            Shortcut { sequences: app.actionKeys("cleanDesk"); enabled: deskDrawer.listKeys && deskList.count > 0; onActivated: app.requestCleanDesk() }
            Shortcut {
                sequences: ["Menu", "Shift+F10"]
                enabled: deskDrawer.listKeys && deskList.currentIndex >= 0
                onActivated: { const p = app.sceneRowPoint(deskList.currentItem); app.openDeskMenu(deskList.currentIndex, p.x, p.y) }
            }
            RowLayout {
                Layout.fillWidth: true
                PanelHeader {
                    Layout.fillWidth: true
                    theme: desktop
                    title: "Desk"
                    subtitle: deskList.count === 1 ? "1 passage" : deskList.count + " passages"
                    uiScale: app.scale
                    titleScale: app.controlScale
                    titleName: "deskTitle"
                    subtitleName: "deskCount"
                }
                QuietButton {
                    objectName: "deskClean"
                    Layout.alignment: Qt.AlignTop | Qt.AlignRight
                    text: "Clean Desk"; compact: true
                    enabled: deskList.count > 0 && !app.rangeEditing && !bible.busy
                    onClicked: app.requestCleanDesk()
                }
            }
            FineRule { Layout.fillWidth: true }
            ListView {
                id: deskList
                objectName: "deskList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 6 * app.scale
                model: bible.desk
                onModelChanged: Qt.callLater(app.restoreDeskRow)
                ScrollBar.vertical: OverlayScrollbar { objectName: "deskScrollbar" }
                delegate: ItemDelegate {
                    id: deskEntry
                    required property var modelData
                    required property int index
                    objectName: "deskEntry" + index
                    width: deskList.width
                    padding: 12 * app.scale
                    height: deskEntryContent.implicitHeight + topPadding + bottomPadding
                    focusPolicy: Qt.NoFocus
                    highlighted: ListView.isCurrentItem
                    onClicked: { deskList.currentIndex = index; app.moveDesk(0); app.openDeskItem(modelData.id) }
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: function(eventPoint) { app.openDeskMenu(deskEntry.index, eventPoint.scenePosition.x, eventPoint.scenePosition.y) }
                    }
                    contentItem: ColumnLayout {
                        id: deskEntryContent
                        spacing: 7 * app.scale
                        Label {
                            objectName: "deskHeader" + deskEntry.index
                            text: deskEntry.modelData.reference + " · " + deskEntry.modelData.versionAbbreviation
                            textFormat: Text.PlainText
                            color: deskEntry.modelData.available ? desktop.foreground : desktop.muted
                            font.pixelSize: 16 * app.scale; font.weight: Font.Medium
                            Layout.fillWidth: true; wrapMode: Text.Wrap
                        }
                        Label {
                            objectName: "deskText" + deskEntry.index
                            // LRM: display-only LTR base direction (copy uses the backend).
                            text: app.leftToRightText(deskEntry.modelData.text)
                            visible: text.length > 0
                            textFormat: Text.PlainText
                            horizontalAlignment: Text.AlignLeft
                            // Matches History excerpts: muted, the same size and line height.
                            color: desktop.muted
                            font.pixelSize: 13 * app.scale
                            lineHeight: app.panelLineHeight
                            Layout.fillWidth: true; wrapMode: Text.Wrap
                        }
                        Label {
                            objectName: "deskUnavailable" + deskEntry.index
                            visible: !deskEntry.modelData.available
                            text: deskEntry.modelData.versionAbbreviation + " is not installed. Reimport it to open or copy this passage."
                            color: desktop.muted; font.pixelSize: 12 * app.scale; font.italic: true
                            Layout.fillWidth: true; wrapMode: Text.Wrap
                        }
                    }
                    background: Rectangle {
                        color: deskEntry.highlighted ? app.tint(desktop.accent, 0.11) : deskEntry.hovered ? desktop.surface : "transparent"
                        radius: Math.min(desktop.cornerRadius, width / 2, height / 2)
                        // The current row is shown by its tinted fill alone, never an accent border.
                        border.width: 0
                    }
                }
                Label {
                    objectName: "deskEmpty"
                    anchors.centerIn: parent
                    width: parent.width - 24
                    text: "The Desk is empty. Press " + app.keyText("toggleDesk") + " on a passage to add it."
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    color: desktop.muted
                    font.pixelSize: 13 * app.scale
                    visible: deskList.count === 0
                }
            }
            Label { objectName: "deskError"; text: bible.error; visible: text.length > 0; color: desktop.accent; font.pixelSize: 12 * app.scale; Layout.fillWidth: true; wrapMode: Text.Wrap }
            FineRule { Layout.fillWidth: true }
            // No-break spaces keep each key with its verb when the row wraps.
            HintRow {
                objectName: "deskHints"
                theme: desktop
                parts: app.deskHintParts()
                pixelSize: 11 * app.scale
                Layout.fillWidth: true
            }
        }
    }

    ModalCard {
        id: cleanDeskConfirmation
        objectName: "cleanDeskConfirmation"
        // Outside clicks cancel, like esc; they never reach what is underneath.
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        // Focus at once, while the modal is still fading in.
        onVisibleChanged: if (visible) { cleanDeskCancel.forceActiveFocus() }
        onClosed: {
            if (deskDrawer.shown) Qt.callLater(function() { if (!cleanDeskConfirmation.visible) app.moveDesk(0) })
            else app.restorePanelFocus()
        }
        Shortcut {
            sequences: ["Left", "Up", "Right", "Down"]
            enabled: cleanDeskConfirmation.opened && !errorDialog.visible
            onActivated: {
                if (cleanDeskCancel.activeFocus) cleanDeskConfirm.forceActiveFocus()
                else cleanDeskCancel.forceActiveFocus()
            }
        }
        contentItem: ColumnLayout {
            spacing: 18 * app.scale
            Label { text: "Clean the Desk?"; color: desktop.foreground; font.pixelSize: 22 * app.scale; Layout.fillWidth: true; wrapMode: Text.Wrap }
            Label {
                objectName: "cleanDeskExplanation"
                text: "Remove all " + bible.desk.length + (bible.desk.length === 1 ? " passage" : " passages") + " from the Desk in every window. Your notes, bookmarks, highlights, and tags are kept."
                color: desktop.foreground; font.pixelSize: 14 * app.scale; Layout.fillWidth: true; wrapMode: Text.Wrap
            }
            Label { text: bible.error; visible: text.length > 0; color: desktop.accent; Layout.fillWidth: true; wrapMode: Text.Wrap }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                QuietButton { id: cleanDeskCancel; objectName: "cleanDeskCancel"; text: "Cancel"; onClicked: cleanDeskConfirmation.close() }
                QuietButton { id: cleanDeskConfirm; objectName: "cleanDeskConfirm"; text: "Clean Desk"; emphasized: true; onClicked: app.confirmCleanDesk() }
            }
            HintRow {
                objectName: "cleanDeskHints"
                theme: desktop
                parts: app.confirmationHintParts(cleanDeskCancel, cleanDeskConfirm, cleanDeskConfirmation)
                pixelSize: 11 * app.scale
                Layout.fillWidth: true
            }
        }
    }

    Drawer {
        id: historyDrawer
        objectName: "historyDrawer"
        width: Math.min(520 * app.scale, app.width - 48)
        height: app.height
        edge: Qt.RightEdge
        // Same modality and close rules as the Desk drawer.
        modal: !closing
        dim: true
        focus: true
        padding: 16 * app.controlScale
        topPadding: padding
        bottomPadding: padding
        leftPadding: padding
        rightPadding: padding
        closePolicy: closing || clearHistoryConfirmation.visible || errorDialog.visible ? Popup.NoAutoClose : Popup.CloseOnEscape | Popup.CloseOnPressOutside
        readonly property bool listKeys: shown && !contextMenu.visible && !clearHistoryConfirmation.visible && !errorDialog.visible && !bible.busy && !app.textInputActive()
        background: Rectangle { radius: Math.min(desktop.cornerRadius, width / 2, height / 2); color: desktop.background; border.color: desktop.border }
        Overlay.modal: Rectangle { color: app.tint(desktop.background, 0.65) }
        Overlay.modeless: Rectangle { color: app.tint(desktop.background, 0.65) }
        property bool closing: false
        readonly property bool shown: visible && !closing
        enter: Transition {
            SmoothedAnimation { velocity: app.reduceMotion ? 100000 : 5 }
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: app.fadeMs; easing.type: Easing.OutQuad }
        }
        exit: Transition {
            SmoothedAnimation { velocity: app.reduceMotion ? 100000 : 5 }
            NumberAnimation { property: "opacity"; from: 1; to: 0; duration: app.fadeMs; easing.type: Easing.InQuad }
        }
        onAboutToShow: { const reopening = visible; closing = false; app.historySelectedId = -1; app.historyRow = 0; if (reopening) focusContent() }
        onAboutToHide: { closing = true; app.restorePanelFocus() }
        function focusContent() { app.restoreHistoryRow(); app.moveHistory(0) }
        onVisibleChanged: if (visible && !closing) focusContent()
        onClosed: { closing = false; app.restorePanelFocus() }
        contentItem: ColumnLayout {
            objectName: "historyContent"
            spacing: (historyDrawer.height < 350 * app.scale ? 6 : 12) * app.scale
            Shortcut { sequences: ["J", "Down"]; enabled: historyDrawer.listKeys; onActivated: app.moveHistory(1) }
            Shortcut { sequences: ["K", "Up"]; enabled: historyDrawer.listKeys; onActivated: app.moveHistory(-1) }
            ListMotionKeys { objectName: "historyMotionKeys"; active: historyDrawer.listKeys; count: historyList.count; onMove: function(delta) { app.moveHistory(delta) } }
            Shortcut { sequence: "Home"; enabled: historyDrawer.listKeys; onActivated: app.moveHistory(-historyList.count) }
            Shortcut { sequence: "End"; enabled: historyDrawer.listKeys; onActivated: app.moveHistory(historyList.count) }
            Shortcut { sequences: ["Return", "Enter"]; enabled: historyDrawer.listKeys && historyList.activeFocus; onActivated: { const entry = app.currentHistoryEntry(); if (entry) app.openHistoryItem(entry.id); else sounds.play("error") } }
            Shortcut { sequence: "Y"; enabled: historyDrawer.listKeys; onActivated: { const entry = app.currentHistoryEntry(); if (entry) app.copyHistoryItem(entry.id); else sounds.play("error") } }
            Shortcut { sequence: "Shift+Y"; enabled: historyDrawer.listKeys; onActivated: { const entry = app.currentHistoryEntry(); if (entry) app.copyHistoryItem(entry.id, true); else sounds.play("error") } }
            Shortcut { sequence: "D"; enabled: historyDrawer.listKeys; onActivated: { const entry = app.currentHistoryEntry(); if (entry) app.toggleHistoryDesk(entry.id); else sounds.play("error") } }
            Shortcut { sequences: ["Delete", "Backspace"]; enabled: historyDrawer.listKeys; onActivated: { const entry = app.currentHistoryEntry(); if (entry) app.removeHistoryItem(entry.id); else sounds.play("error") } }
            Shortcut { sequences: app.actionKeys("historyClearKeys"); enabled: historyDrawer.listKeys && historyList.count > 0; onActivated: app.requestClearHistory() }
            Shortcut {
                sequences: ["Menu", "Shift+F10"]
                enabled: historyDrawer.listKeys && historyList.currentIndex >= 0
                onActivated: { const p = app.sceneRowPoint(historyList.currentItem); app.openHistoryMenu(historyList.currentIndex, p.x, p.y) }
            }
            RowLayout {
                Layout.fillWidth: true
                PanelHeader {
                    Layout.fillWidth: true
                    theme: desktop
                    title: "History"
                    subtitle: historyList.count === 1 ? "1 passage" : historyList.count + " passages"
                    uiScale: app.scale
                    titleScale: app.controlScale
                    titleName: "historyTitle"
                    subtitleName: "historyCount"
                }
                QuietButton {
                    objectName: "historyClear"
                    Layout.alignment: Qt.AlignTop | Qt.AlignRight
                    text: "Clear History"; compact: true
                    enabled: historyList.count > 0 && !app.rangeEditing && !bible.busy
                    onClicked: app.requestClearHistory()
                }
            }
            FineRule { Layout.fillWidth: true }
            ListView {
                id: historyList
                objectName: "historyList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 6 * app.scale
                model: app.historyRows
                onModelChanged: Qt.callLater(app.restoreHistoryRow)
                ScrollBar.vertical: OverlayScrollbar { objectName: "historyScrollbar" }
                delegate: ColumnLayout {
                    id: historyBlock
                    required property var modelData
                    required property int index
                    readonly property bool firstOfGroup: index === 0 || historyList.model[index - 1].group !== modelData.group
                    readonly property bool current: modelData.id === app.historyCurrentId
                    width: historyList.width
                    spacing: 6 * app.scale
                    Label {
                        objectName: "historyGroup" + historyBlock.index
                        visible: historyBlock.firstOfGroup
                        text: historyBlock.modelData.group
                        color: desktop.muted
                        font.pixelSize: 12 * app.scale; font.weight: Font.Medium
                        Layout.topMargin: historyBlock.index ? 8 * app.scale : 0
                        Layout.leftMargin: 12 * app.scale
                    }
                    ItemDelegate {
                        id: historyEntry
                        objectName: "historyEntry" + historyBlock.index
                        Layout.fillWidth: true
                        padding: 12 * app.scale
                        implicitHeight: historyEntryContent.implicitHeight + topPadding + bottomPadding
                        focusPolicy: Qt.NoFocus
                        highlighted: historyList.currentIndex === historyBlock.index
                        onClicked: { historyList.currentIndex = historyBlock.index; app.moveHistory(0); app.openHistoryItem(historyBlock.modelData.id) }
                        TapHandler {
                            acceptedButtons: Qt.RightButton
                            onTapped: function(eventPoint) { app.openHistoryMenu(historyBlock.index, eventPoint.scenePosition.x, eventPoint.scenePosition.y) }
                        }
                        contentItem: ColumnLayout {
                            id: historyEntryContent
                            spacing: 5 * app.scale
                            RowLayout {
                                Layout.fillWidth: true
                                Label {
                                    objectName: "historyHeader" + historyBlock.index
                                    text: historyBlock.modelData.reference + " · " + historyBlock.modelData.versionAbbreviation
                                    textFormat: Text.PlainText
                                    color: historyBlock.modelData.available ? desktop.foreground : desktop.muted
                                    font.pixelSize: 16 * app.scale; font.weight: Font.Medium
                                    Layout.fillWidth: true; elide: Text.ElideRight
                                }
                                Label {
                                    objectName: "historyTime" + historyBlock.index
                                    text: app.historyTime(historyBlock.modelData.visitedAt)
                                    color: desktop.muted; font.pixelSize: 12 * app.scale
                                }
                            }
                            Label {
                                objectName: "historyExcerpt" + historyBlock.index
                                text: app.leftToRightText(historyBlock.modelData.excerpt)
                                visible: historyBlock.modelData.excerpt.length > 0
                                textFormat: Text.PlainText
                                horizontalAlignment: Text.AlignLeft
                                color: desktop.muted
                                font.pixelSize: 13 * app.scale
                                lineHeight: app.panelLineHeight
                                maximumLineCount: 2
                                elide: Text.ElideRight
                                Layout.fillWidth: true; wrapMode: Text.Wrap
                            }
                            Label {
                                visible: !historyBlock.modelData.available
                                text: historyBlock.modelData.versionAbbreviation + " is not installed."
                                color: desktop.muted; font.pixelSize: 12 * app.scale; font.italic: true
                                Layout.fillWidth: true; wrapMode: Text.Wrap
                            }
                        }
                        background: Rectangle {
                            color: historyEntry.highlighted ? app.tint(desktop.accent, 0.11) : historyEntry.hovered ? desktop.surface : "transparent"
                            radius: Math.min(desktop.cornerRadius, width / 2, height / 2)
                            border.width: 0
                            // The jump stack's current position: a thin accent edge.
                            Rectangle {
                                objectName: "historyCurrent" + historyBlock.index
                                visible: historyBlock.current
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                anchors.margins: 6 * app.scale
                                width: 2 * app.scale
                                color: app.tint(desktop.accent, 0.7)
                            }
                        }
                    }
                }
                Label {
                    objectName: "historyEmpty"
                    anchors.centerIn: parent
                    width: parent.width - 24
                    text: "No History yet. Passages you stay on for a few seconds appear here."
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    color: desktop.muted
                    font.pixelSize: 13 * app.scale
                    visible: historyList.count === 0
                }
            }
            Label { objectName: "historyError"; text: bible.error; visible: text.length > 0; color: desktop.accent; font.pixelSize: 12 * app.scale; Layout.fillWidth: true; wrapMode: Text.Wrap }
            FineRule { Layout.fillWidth: true }
            HintRow {
                objectName: "historyHints"
                theme: desktop
                parts: app.historyHintParts()
                pixelSize: 11 * app.scale
                Layout.fillWidth: true
            }
        }
    }

    ModalCard {
        id: clearHistoryConfirmation
        objectName: "clearHistoryConfirmation"
        // Outside clicks cancel, like esc; they never reach what is underneath.
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        // Focus at once, while the modal is still fading in.
        onVisibleChanged: if (visible) { clearHistoryCancel.forceActiveFocus() }
        onClosed: {
            if (historyDrawer.shown) Qt.callLater(function() { if (!clearHistoryConfirmation.visible) app.moveHistory(0) })
            else app.restorePanelFocus()
        }
        Shortcut {
            sequences: ["Left", "Up", "Right", "Down"]
            enabled: clearHistoryConfirmation.opened && !errorDialog.visible
            onActivated: {
                if (clearHistoryCancel.activeFocus) clearHistoryConfirm.forceActiveFocus()
                else clearHistoryCancel.forceActiveFocus()
            }
        }
        contentItem: ColumnLayout {
            spacing: 18 * app.scale
            Label { text: "Clear History?"; color: desktop.foreground; font.pixelSize: 22 * app.scale; Layout.fillWidth: true; wrapMode: Text.Wrap }
            Label {
                objectName: "clearHistoryExplanation"
                text: "Remove all " + bible.history.length + (bible.history.length === 1 ? " passage" : " passages") + " and your recent searches from History in every window. Back and forward in each window still work."
                color: desktop.foreground; font.pixelSize: 14 * app.scale; Layout.fillWidth: true; wrapMode: Text.Wrap
            }
            Label { text: bible.error; visible: text.length > 0; color: desktop.accent; Layout.fillWidth: true; wrapMode: Text.Wrap }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                QuietButton { id: clearHistoryCancel; objectName: "clearHistoryCancel"; text: "Cancel"; onClicked: clearHistoryConfirmation.close() }
                QuietButton { id: clearHistoryConfirm; objectName: "clearHistoryConfirm"; text: "Clear History"; emphasized: true; onClicked: app.confirmClearHistory() }
            }
            HintRow {
                objectName: "clearHistoryHints"
                theme: desktop
                parts: app.confirmationHintParts(clearHistoryCancel, clearHistoryConfirm, clearHistoryConfirmation)
                pixelSize: 11 * app.scale
                Layout.fillWidth: true
            }
        }
    }

    ModalCard {
        id: deleteConfirmation
        objectName: "deleteConfirmation"
        property var scope: null
        // Annotations → Tags: {id, name} of the one tag to remove.
        property var tag: null
        readonly property string reference: scope ? scope.reference : ""
        readonly property string versionId: scope ? scope.versionId : ""
        property int row: -1
        property bool noteOnly: false
        // Outside clicks cancel, like esc; they never reach what is underneath.
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        // Focus at once, while the modal is still fading in.
        onVisibleChanged: if (visible) { deleteCancel.forceActiveFocus() }
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
            tag = null
            row = -1
            noteOnly = false
            deleteFocusTimer.restart()
        }
        contentItem: ColumnLayout {
            spacing: 18 * app.scale
            Label {
                objectName: "deleteTitle"
                text: deleteConfirmation.tag ? "Remove " + deleteConfirmation.tag.name + " from " + deleteConfirmation.reference + "?"
                    : deleteConfirmation.noteOnly ? "Delete note?" : "Delete annotations?"
                textFormat: Text.PlainText
                color: desktop.foreground; font.pixelSize: 22 * app.scale; Layout.fillWidth: true; wrapMode: Text.Wrap
            }
            Label { objectName: "deleteReference"; visible: !deleteConfirmation.tag; text: deleteConfirmation.reference; textFormat: Text.PlainText; color: desktop.accent; font.pixelSize: 16 * app.scale; Layout.fillWidth: true; wrapMode: Text.Wrap }
            Label { objectName: "deleteExplanation"; text: deleteConfirmation.tag ? "Only this tag is removed from this exact range. Its other tags, marks, and note stay."
                : deleteConfirmation.noteOnly ? "This deletes the note for this exact reference. Bookmarks, highlights, and tags stay."
                : "This deletes all marks (bookmark and highlight), the note, and tags for this exact reference."; color: desktop.foreground; font.pixelSize: 14 * app.scale; Layout.fillWidth: true; wrapMode: Text.Wrap }
            Label { text: bible.error; visible: text.length > 0; color: desktop.accent; Layout.fillWidth: true; wrapMode: Text.Wrap }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                QuietButton { id: deleteCancel; objectName: "deleteCancel"; text: "Cancel"; onClicked: deleteConfirmation.close() }
                QuietButton { id: deleteConfirm; objectName: "deleteConfirm"; text: deleteConfirmation.tag ? "Remove" : "Delete"; emphasized: true; onClicked: app.confirmDeleteSaved() }
            }
            HintRow {
                objectName: "deleteHints"
                theme: desktop
                parts: app.confirmationHintParts(deleteCancel, deleteConfirm, deleteConfirmation)
                pixelSize: 11 * app.scale
                Layout.fillWidth: true
            }
        }
    }

    ModalCard {
        id: helpDialog
        objectName: "helpDialog"
        property string pendingAction: ""
        height: Math.min(660 * app.scale, app.height - 48)
        // Focus at once, while the modal is still fading in.
        onVisibleChanged: if (visible) { keybindingSearch.text = ""; keybindingSearch.forceActiveFocus() }
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
                placeholderText: "Search keybindings…"
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
                    rightPadding: leftPadding + (keybindingScrollbar.visible ? 12 * app.scale : 0)
                    enabled: app.actionAvailable(modelData)
                    AppToolTip { theme: desktop; uiScale: app.scale; visible: keybindingRow.hovered; text: app.scopeTitle(keybindingRow.modelData) }
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
                        text: app.keyRich(keybindingRow.modelData); textFormat: Text.StyledText; color: desktop.muted
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
                    color: desktop.muted; font.pixelSize: 13 * app.scale
                }
            }
            FineRule { objectName: "keybindingFooterRule"; Layout.fillWidth: true }
            HintRow {
                objectName: "keybindingHints"
                theme: desktop
                readonly property var current: keybindingResults.model[keybindingResults.currentIndex]
                parts: [[["Up", "Down"], "choose"], ["Return", "select", () => app.activateKeybinding(current), !!current && app.actionAvailable(current)],
                    ["Escape", "close", () => helpDialog.close()]]
                pixelSize: 11 * app.scale
                Layout.fillWidth: true
            }
        }
    }

    ModalCard {
        id: settingsDialog
        objectName: "settingsDialog"
        width: Math.min(480 * app.scale, app.width - 48)
        height: Math.min(settingsContent.implicitHeight + 48, app.height - 48)
        // Focus at once, while the modal is still fading in.
        onVisibleChanged: if (visible) { lineHeightSlider.forceActiveFocus() }
        onClosed: app.restorePanelFocus()
        contentItem: KeyScrollView {
            id: settingsScroll
            objectName: "settingsScroll"
            contentWidth: availableWidth
            clip: true
        ColumnLayout {
            id: settingsContent
            width: settingsScroll.availableWidth
            spacing: 12 * app.scale
            RowLayout {
                Label { text: "Reading settings"; color: desktop.foreground; font.pixelSize: 22 * app.scale; Layout.fillWidth: true }
            }
            Repeater {
                model: app.actions.filter(function(action) { return !!action.setting })
                delegate: SettingSwitch {
                    required property var modelData
                    catalogAction: modelData
                    Layout.fillWidth: true
                }
            }
            Repeater {
                model: Array.from(bible.versions).filter(function(version) { return version.needsReimport })
                delegate: RowLayout {
                    id: reimportRow
                    required property var modelData
                    Layout.fillWidth: true
                    Label {
                        text: reimportRow.modelData.abbreviation + " uses an older text format (no footnotes, cross references, or words of Jesus)."
                        color: desktop.muted; font.pixelSize: 12 * app.scale; wrapMode: Text.Wrap; Layout.fillWidth: true
                    }
                    QuietButton {
                        objectName: "settingsReimport:" + reimportRow.modelData.id
                        text: "Re-import"; compact: true
                        enabled: !bible.busy && !catalog.busy
                        onClicked: app.reimportVersion(reimportRow.modelData)
                    }
                }
            }
            Label {
                objectName: "wordsOfJesusUnmarked"
                visible: bible.ready && !bible.hasWordsOfJesus
                text: "Words of Jesus are not marked in this version" + (bible.textSchema > 0 && bible.textSchema < 4 ? " (re-import it in Versions to check)." : ".")
                color: desktop.muted
                font.pixelSize: 12 * app.scale
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            RowLayout {
                Layout.fillWidth: true
                Label { text: "Reading mode"; color: desktop.foreground; font.pixelSize: 13 * app.scale; Layout.fillWidth: true }
                Label { objectName: "readingModeKey"; text: app.keyRich("readingMode"); textFormat: Text.StyledText; color: desktop.muted; font.pixelSize: 12 * app.scale }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 4 * app.scale
                uniformCellSizes: true
                Repeater {
                    model: ["paragraph", "verse", "block"]
                    delegate: QuietButton {
                        required property string modelData
                        objectName: "readingMode:" + modelData
                        text: app.readingModeName(modelData)
                        checkable: true
                        autoExclusive: true
                        checked: bible.readingMode === modelData
                        emphasized: checked
                        compact: true
                        Layout.fillWidth: true
                        onClicked: bible.setReadingMode(modelData)
                        Accessible.name: text + " reading mode"
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Label { text: "Font size"; color: desktop.foreground; font.pixelSize: 13 * app.scale }
                Item { Layout.fillWidth: true }
                Label { objectName: "fontSizeValue"; text: bible.fontSize; color: desktop.accent; font.pixelSize: 14 * app.scale }
            }
            SquareSlider {
                objectName: "fontSizeSlider"
                Layout.fillWidth: true
                from: 8; to: 40; stepSize: 1
                snapMode: Slider.SnapAlways
                value: bible.fontSize
                onMoved: bible.setFontSize(Math.round(value))
                Accessible.name: "Font size"
            }
            HintRow {
                objectName: "fontSizeHints"
                theme: desktop
                parts: ["smaller", "larger", "resetText"].map(function(id, index) {
                    return [app.actionKeys(id)[0], ["decrease", "increase", "reset"][index], () => app.runCommand(id), app.actionAvailable(id)]
                })
                pixelSize: 11 * app.scale
                Layout.fillWidth: true
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
            HintRow {
                objectName: "lineHeightHints"
                theme: desktop
                parts: ["lineLess", "lineMore", "lineReset"].map(function(id, index) {
                    return [app.actionKeys(id)[0], ["decrease", "increase", "reset"][index], () => app.runCommand(id), app.actionAvailable(id)]
                })
                pixelSize: 11 * app.scale
                Layout.fillWidth: true
            }
        }
        }
    }

    ModalCard {
        id: aboutDialog
        objectName: "aboutDialog"
        onClosed: app.restorePanelFocus()
        onVisibleChanged: if (visible) { aboutScroll.contentItem.contentY = 0; aboutScroll.forceActiveFocus() }
        contentItem: KeyScrollView {
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
        // The highlighted catalog row: ↑ ↓ from the search field choose, enter
        // installs it, shift enter shows its Details & Attribution.
        property int catalogIndex: 0
        readonly property var catalogEntry: catalogResults.model[catalogIndex] || null
        function moveCatalog(delta) {
            if (!catalogResults.count) { sounds.play("boundary"); return }
            const next = Math.max(0, Math.min(catalogResults.count - 1, catalogIndex + delta))
            if (next === catalogIndex) sounds.play("boundary")
            catalogIndex = next
            const row = catalogResults.itemAt(catalogIndex)
            const flick = importGuideScroll.contentItem as Flickable
            if (!row || !flick) return
            const y = row.mapToItem(flick.contentItem, 0, 0).y
            if (y < flick.contentY) flick.contentY = y
            else if (y + row.height > flick.contentY + flick.height) flick.contentY = y + row.height - flick.height
        }
        function installCurrent() {
            if (!catalogEntry || bible.busy || catalog.busy) { app.rejected(); return }
            app.installVersion(catalogEntry.id)
        }
        function showCurrentDetails() {
            if (!catalogEntry) { app.rejected(); return }
            catalogDetails.entry = catalogEntry
            catalogDetails.open()
        }
        function chooseZip() {
            if (bible.busy || catalog.busy || !app.flushNote()) { app.rejected(); return }
            choosePending = true
            close()
        }
        function guideKey(event) {
            const mods = event.modifiers & ~Qt.KeypadModifier
            const enter = event.key === Qt.Key_Return || event.key === Qt.Key_Enter
            if (mods === Qt.NoModifier && event.key === Qt.Key_Down) moveCatalog(1)
            else if (mods === Qt.NoModifier && event.key === Qt.Key_Up) moveCatalog(-1)
            else if (mods === Qt.NoModifier && enter) installCurrent()
            else if (mods === Qt.ShiftModifier && enter) showCurrentDetails()
            else if (mods === Qt.ControlModifier && event.key === Qt.Key_I) chooseZip()
            else return false
            event.accepted = true
            return true
        }
        readonly property real uiScale: Math.min(app.scale, Math.max(1, (app.height - 48) / 360))
        height: Math.min(650 * app.scale, app.height - 48)
        // Focus at once, while the modal is still fading in.
        onVisibleChanged: if (visible) { catalogSearch.text = ""; catalogIndex = 0; catalogSearch.forceActiveFocus() }
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
            Label { text: "Add version"; color: desktop.foreground; font.pixelSize: 22 * importGuide.uiScale; Layout.fillWidth: true; wrapMode: Text.Wrap }
            TextField {
                id: catalogSearch
                objectName: "catalogSearch"
                Layout.fillWidth: true
                implicitHeight: 42 * importGuide.uiScale
                font.pixelSize: 15 * importGuide.uiScale
                selectByMouse: true
                padding: 10 * importGuide.uiScale
                background: Rectangle { radius: Math.min(desktop.cornerRadius, width / 2, height / 2); color: desktop.surface; border.color: catalogSearch.activeFocus ? desktop.accent : desktop.border }
                placeholderText: "Search versions…"
                Accessible.name: "Search versions"
                onTextChanged: importGuide.catalogIndex = 0
                // ctrl i imports a ZIP even while the Versions dialog lies below.
                Keys.onShortcutOverride: function(event) {
                    if ((event.modifiers & ~Qt.KeypadModifier) === Qt.ControlModifier && event.key === Qt.Key_I) event.accepted = true
                }
                // ↑ ↓ choose a version, enter installs, shift enter shows details,
                // ctrl i imports a ZIP; paging keys scroll the guide.
                Keys.onPressed: function(event) {
                    if (importGuide.guideKey(event)) return
                    const paging = [Qt.Key_PageUp, Qt.Key_PageDown].includes(event.key)
                        || (event.modifiers === Qt.ControlModifier && [Qt.Key_D, Qt.Key_U].includes(event.key))
                    if (paging) app.scrollKey(importGuideScroll.contentItem as Flickable, event)
                }
            }
            KeyScrollView {
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
                        delegate: Item {
                            id: catalogRow
                            required property var modelData
                            required property int index
                            readonly property bool current: index === importGuide.catalogIndex
                            objectName: "catalogRow:" + modelData.id
                            Layout.fillWidth: true
                            implicitWidth: catalogRowContent.implicitWidth
                            implicitHeight: catalogRowContent.implicitHeight
                            Rectangle {
                                objectName: "catalogRowTint:" + catalogRow.modelData.id
                                anchors.fill: parent
                                anchors.margins: -4 * app.scale
                                visible: catalogRow.current
                                color: app.tint(desktop.accent, 0.11)
                                radius: Math.min(desktop.cornerRadius, width / 2, height / 2)
                            }
                            MouseArea { anchors.fill: parent; acceptedButtons: Qt.LeftButton; onClicked: importGuide.catalogIndex = catalogRow.index }
                        ColumnLayout {
                            id: catalogRowContent
                            width: parent.width
                            spacing: 6 * app.scale
                            Label { text: catalogRow.modelData.abbreviation + " · " + catalogRow.modelData.name; textFormat: Text.PlainText; color: desktop.foreground; font.pixelSize: 14 * app.scale; wrapMode: Text.Wrap; Layout.fillWidth: true }
                            RowLayout {
                                Layout.fillWidth: true
                                Label { objectName: "catalogInstalled:" + catalogRow.modelData.id; visible: app.installedVersion(catalogRow.modelData.id); text: "Installed"; color: desktop.accent; font.pixelSize: 12 * app.scale }
                                Item { Layout.fillWidth: true }
                                QuietButton { objectName: "catalogDetails:" + catalogRow.modelData.id; text: "Details & Attribution"; font.pixelSize: 12 * importGuide.uiScale; compact: true; onClicked: { importGuide.catalogIndex = catalogRow.index; catalogDetails.entry = catalogRow.modelData; catalogDetails.open() } }
                                QuietButton {
                                    objectName: "catalogInstall:" + catalogRow.modelData.id
                                    text: "Install"; compact: true; emphasized: true
                                    font.pixelSize: 12 * importGuide.uiScale
                                    enabled: !bible.busy && !catalog.busy
                                    onClicked: { importGuide.catalogIndex = catalogRow.index; app.installVersion(catalogRow.modelData.id) }
                                }
                            }
                            FineRule { Layout.fillWidth: true }
                        }
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
                QuietButton { objectName: "importCancel"; text: "Close"; font.pixelSize: 14 * importGuide.uiScale; onClicked: importGuide.close() }
                QuietButton {
                    id: importChoose
                    objectName: "importChoose"
                    text: "Import USFM ZIP…"
                    font.pixelSize: 14 * importGuide.uiScale
                    emphasized: true
                    enabled: !bible.busy && !catalog.busy
                    tip: "Import a local USFM ZIP · " + KeyNotation.format("Ctrl+I")
                    onClicked: { if (bible.busy || catalog.busy || !app.flushNote()) return; importGuide.choosePending = true; importGuide.close() }
                }
            }
            HintRow {
                objectName: "importGuideHints"
                Layout.fillWidth: true
                theme: desktop
                pixelSize: 11 * importGuide.uiScale
                parts: [[["Up", "Down"], "choose"],
                    ["Return", "install", function() { importGuide.installCurrent() }, !!importGuide.catalogEntry && !bible.busy && !catalog.busy],
                    ["Shift+Return", "details", function() { importGuide.showCurrentDetails() }, !!importGuide.catalogEntry],
                    ["Ctrl+I", "import ZIP…", function() { importGuide.chooseZip() }, !bible.busy && !catalog.busy],
                    ["Escape", "close", function() { importGuide.close() }]]
            }
        }
    }

    ModalCard {
        id: catalogDetails
        objectName: "catalogDetails"
        property var entry: null
        onClosed: { if (importGuide.visible) catalogSearch.forceActiveFocus(); else app.restorePanelFocus() }
        onVisibleChanged: if (visible) { detailsScroll.contentItem.contentY = 0; detailsScroll.forceActiveFocus() }
        contentItem: KeyScrollView {
            id: detailsScroll
            objectName: "detailsScroll"
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
        title: app.reimportId ? "Re-import " + (Array.from(bible.versions).find(function(v) { return v.id === app.reimportId }) || { name: app.reimportId }).name : "Add version"
        fileMode: FileDialog.OpenFile
        nameFilters: ["USFM ZIP (*.zip)", "USFM files (*.usfm *.sfm)"]
        onAccepted: app.importChosen(selectedFile)
        onRejected: app.reimportId = ""
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
        // Focus at once, while the modal is still fading in.
        onVisibleChanged: if (visible) { backupScopeScroll.contentItem.contentY = 0; backupChoose.forceActiveFocus() }
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
            Keys.onPressed: function(event) { app.scrollKey(backupScopeScroll.contentItem as Flickable, event) }
            Label {
                text: "Save " + bible.versionAbbreviation + " annotation backup"
                color: desktop.foreground; font.pixelSize: 22 * app.scale
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
                }
                QuietButton {
                    id: backupChoose
                    objectName: "backupChoose"
                    text: "Choose file…"
                    emphasized: true
                    enabled: app.actionAvailable("backup")
                    onClicked: { if (!app.flushNote()) return; backupScope.choosePending = true; backupScope.close() }
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
        // Outside clicks cancel, like esc; they never reach what is underneath.
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        // Focus at once, while the modal is still fading in.
        onVisibleChanged: if (visible) { restoreCancel.forceActiveFocus() }
        Shortcut {
            sequences: ["Left", "Up", "Right", "Down"]
            enabled: restoreConfirmation.opened && !errorDialog.visible
            onActivated: {
                if (restoreCancel.activeFocus) restoreConfirm.forceActiveFocus()
                else restoreCancel.forceActiveFocus()
            }
        }
        onClosed: { backupFile = ""; app.restorePanelFocus() }
        contentItem: ColumnLayout {
            spacing: 18
            Label {
                text: "Restore this backup?"
                color: desktop.foreground
                font.pixelSize: 22 * app.scale
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
                }
                QuietButton {
                    id: restoreConfirm
                    objectName: "restoreConfirm"
                    text: "Restore"
                    emphasized: true
                    onClicked: app.confirmRestore()
                }
            }
            HintRow {
                objectName: "restoreHints"
                theme: desktop
                parts: app.confirmationHintParts(restoreCancel, restoreConfirm, restoreConfirmation)
                pixelSize: 11 * app.scale
                Layout.fillWidth: true
            }
        }
    }

    ModalCard {
        id: errorDialog
        objectName: "errorDialog"
        property string message: ""
        // Focus at once, while the modal is still fading in.
        onVisibleChanged: if (visible) { errorDismiss.forceActiveFocus() }
        onClosed: app.restorePanelFocus()
        contentItem: ColumnLayout {
            spacing: 16 * app.scale
            Label { text: "Unable to complete action"; color: desktop.foreground; font.pixelSize: 22 * app.scale; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Label { objectName: "focusedErrorMessage"; text: errorDialog.message; textFormat: Text.PlainText; color: desktop.foreground; wrapMode: Text.Wrap; Layout.fillWidth: true; Accessible.role: Accessible.AlertMessage }
            QuietButton { id: errorDismiss; text: "Dismiss"; Layout.alignment: Qt.AlignRight; onClicked: errorDialog.close() }
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
        anchors.bottomMargin: bible.focusMode ? 68 : Math.max(68, app.footerMargin + app.footerHeight + 12 * app.controlScale)
        width: Math.min(toast.implicitWidth + 36, app.width - 2 * Math.max(24, app.footerRightInset))
        height: toast.implicitHeight + 24
        radius: Math.min(desktop.cornerRadius, width / 2, height / 2)
        color: desktop.surface
        border.color: desktop.border
        // Toasts fade and rise in, then fade and sink out (paint-only).
        id: toastFrame
        objectName: "toastFrame"
        readonly property bool shown: toastTimer.running && (!bible.focusMode || app.toastInFocus)
        opacity: shown ? 1 : 0
        visible: shown || opacity > 0
        Behavior on opacity { NumberAnimation { duration: app.fadeMs; easing.type: Easing.OutQuad } }
        transform: Translate { y: (1 - toastFrame.opacity) * 8 * app.scale }
        Label { id: toast; objectName: "toast"; anchors.centerIn: parent; width: parent.width - 36; wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter; color: desktop.foreground; font.pixelSize: 13 * app.scale; Accessible.role: Accessible.AlertMessage }
    }
    Timer { id: toastTimer; objectName: "toastTimer"; interval: 3000 }
    Timer { id: countTimer; interval: 2000; onTriggered: app.clearCount() }
    Timer { id: positionSaveTimer; interval: 350; onTriggered: app.savePosition() }
    Timer { id: historyDwellTimer; objectName: "historyDwellTimer"; interval: app.historyDwellMs; onTriggered: app.recordVisit() }
    Timer { id: readingModeRestoreTimer; interval: 0; onTriggered: app.restorePosition(app.hasSelection) }
    Timer {
        id: deleteFocusTimer
        interval: 0
        onTriggered: {
            if (deleteConfirmation.visible) return
            if (savedDrawer.shown) app.moveSaved(0)
            else app.restorePanelFocus()
        }
    }
    Timer {
        id: positionTimer
        interval: 80
        onTriggered: {
            if (app.positionJumpToSelection && bible.selectedStart > 0) app.placeVerse(bible.cursorVerse, true, !app.restoreSlides)
            else app.setReaderY(Math.max(0, reading.contentHeight - reading.height) * Math.max(0, Math.min(1, bible.readingPosition)))
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
        function onVersionChanged() {
            app.invalidateDeleteSaved()
            if (app.paletteTextMode) app.scheduleBibleSearch()
        }
        function onSearchChanged() { Qt.callLater(app.restoreSearchPalette) }
        function onCollectionChanged() {
            if (deleteConfirmation.visible && !app.deleteScopeValid()) app.invalidateDeleteSaved()
        }
        function onErrorChanged() {
            if (bible.error.length) sounds.play("error")
            Qt.callLater(app.showFocusedError)
        }
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
                app.beginRestore(true, true)
            }
            app.syncNote()
            // Reading another chapter (h, l, or any jump) restarts the History
            // dwell; verse steps inside one chapter keep it running.
            if (bible.versionId + ":" + bible.book + ":" + bible.chapter !== app.dwellChapterKey) app.restartDwell()
        }
        function onHistoryChanged() {
            if (!bible.historyEnabled) { historyDwellTimer.stop(); if (historyDrawer.shown) historyDrawer.close() }
            else if (app.dwellPending && !historyDwellTimer.running) historyDwellTimer.restart()
        }
    }
    Component.onCompleted: { marginNotesOn = bible.marginNotes; readerInitialized = true; syncNote(); restorePosition(); restartDwell(); scheduleMargins() }
    onClosing: function(close) {
        if (rangeEditing) rangeEditor.cancel()
        if (rangeEditing) { close.accepted = false; return }
        close.accepted = flushNote()
        savePosition()
        if (close.accepted) { closingWindow = true; cancelDownload() }
    }

    Instantiator {
        model: app.actions.filter(function(action) { return app.globallyBound(action) && !action.sequences.some(function(sequence) { return sequence.startsWith("Z, ") }) })
        delegate: Shortcut {
            required property var modelData
            // Application context reaches the notes drawer's popup focus scope;
            // only the active reader window may register its catalog shortcuts.
            context: Qt.ApplicationShortcut
            // Home/End stay plain chapter edges; g/G also accept a count.
            sequences: modelData.id === "first" ? ["G"] : modelData.id === "last" ? ["Shift+G"]
                : modelData.sequences.concat(modelData.aliases || [])
            enabled: app.active && app.shortcutEnabled(modelData)
            // Verse motions repeat on our own timer, without the platform delay.
            autoRepeat: !app.heldKeys[modelData.id]
            onActivated: { app.runCommand(modelData.id); app.beginHold(modelData.id) }
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
    Shortcut { sequence: "Z"; enabled: app.active && app.readerKeys && !app.zPending; onActivated: { app.clearCount(); app.zPending = true; countTimer.restart() } }
    Instantiator {
        model: app.actions.filter(function(action) { return app.globallyBound(action) && action.sequences.some(function(sequence) { return sequence.startsWith("Z, ") }) })
        delegate: Shortcut {
            required property var modelData
            sequence: modelData.sequences.find(function(sequence) { return sequence.startsWith("Z, ") }).slice(3)
            enabled: app.active && app.zPending && app.actionAvailable(modelData) && app.readerKeys
            onActivated: app.runCommand(modelData.id)
        }
    }
    // alt v in the Notes panel (editor, preview, Linked from); the drawer is a
    // popup, so the application context reaches it in the active window only.
    Shortcut {
        objectName: "notePreviewShortcut"
        context: Qt.ApplicationShortcut
        sequences: app.actionKeys("notePreview")
        enabled: app.active && app.notesOpen && app.hasSelection && app.focusWithin(notesDrawer.contentItem) && !app.modalOpen
        onActivated: app.toggleNotePreview()
    }
    Shortcut { sequence: "Home"; enabled: app.readerKeys; onActivated: app.chapterEdge(false) }
    Shortcut { sequence: "End"; enabled: app.readerKeys; onActivated: app.chapterEdge(true) }
    Shortcut { sequences: ["Return", "Enter"]; enabled: app.readerKeys && rangeEditor.active; onActivated: rangeEditor.accept() }
    Shortcut { sequence: "O"; enabled: app.readerKeys && rangeEditor.active; onActivated: rangeEditor.swap() }
    // Escape closes the Notes drawer while focus sits outside the editor (the
    // editor and preview close it themselves). Application context reaches past
    // the drawer's popup focus scope, so only the active window may enable it.
    Shortcut {
        objectName: "closeNotesShortcut"
        context: Qt.ApplicationShortcut
        sequence: "Escape"
        enabled: app.active && app.notesClosableFromReader && !rangeEditor.sessionOpen
        onActivated: if (app.closeNotes()) app.readerFocus()
    }
    Shortcut {
        sequence: "Escape"
        enabled: app.readerKeys || (rangeEditor.sessionOpen && !app.modalOpen && !app.textInputActive())
        onActivated: {
            if (rangeEditor.sessionOpen) { rangeEditor.cancel(); return }
            if (app.acceptedSearchQuery.length) { app.clearSearch(); app.readerFocus(); return }
            if (app.freeSelectedText.length) { app.clearTextSelection(); app.readerFocus(); return }
            if (app.verseCount.length) { app.clearCount(); return }
            // Visual mode or a multi-verse range collapses to the cursor before
            // Escape leaves focus mode; a single-verse selection does not block it.
            const range = app.verseMode || (app.hasSelection && bible.selectedStart !== bible.selectedEnd)
            // With nothing else to dismiss, the last esc clears the error banner.
            if (!range && !bible.focusMode && bible.error.length) { bible.clearError(); app.readerFocus(); return }
            if (range || !bible.focusMode) {
                if (!app.flushNote()) return
                if (app.verseMode || app.hasSelection) app.setVisualMode(false)
                app.readerFocus()
                return
            }
            app.runCommand("focus"); app.readerFocus()
        }
    }
}
