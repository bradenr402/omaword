pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import QtQml
import QtCore
import "HandDrawn.js" as HandDrawn
import "KeyNotation.js" as KeyNotation
import "Margins.js" as Margins
import "RowMenu.js" as RowMenu
import "TypeScale.js" as TypeScale
// The controls in their own files, which Main binds to its window below.
import "." as Shared
// qmllint disable import
import OmaWord.Reader 1.0
// qmllint enable import

ApplicationWindow {
    id: app
    objectName: "readerWindow"
    signal newWindowRequested()
    // Follow ("Open beside", WindowManager): a window reading another
    // version beside this one, the two keeping the same passage and the
    // same verse at the top (followMoved here reaches followTo there).
    signal besideRequested(string versionId)
    signal unfollowRequested()
    signal followMoved(var location)
    // The version the window beside reads; empty when there is none.
    property string followPartner: ""
    readonly property bool following: followPartner.length > 0
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

    // The side panels share one drawer beside the text, the Margin
    // (notesDrawer), showing one at a time (marginMode): Notes ("notes"), the
    // Desk ("desk"), Marks ("marks"), and See also ("related").
    property string marginMode: "notes"
    readonly property bool marginOpen: notesDrawer.shown
    readonly property bool notesOpen: marginOpen && marginMode === "notes"
    readonly property bool deskOpen: marginOpen && marginMode === "desk"
    readonly property bool marksOpen: marginOpen && marginMode === "marks"
    readonly property bool relatedOpen: marginOpen && marginMode === "related"
    // Marks' own keys: the keyboard in Marks, no menu, picker, or field in the way.
    readonly property bool marksKeys: marksOpen && marginKeys && !contextMenu.visible && !tagsUi.visibleModal && !textInputActive()
    function focusedSavedEntry() {
        let focused = activeFocusItem
        while (focused && focused !== collectionList) {
            const row = focused as SavedEntry
            if (row) return row.modelData
            focused = focused.parent
        }
        return collectionList.model[collectionList.currentIndex] || null
    }
    onNotesOpenChanged: clearCount()
    property bool notePreview: false
    property bool syncingNote: true
    // Typing saves after this pause (ms), so a keystroke never waits on the
    // disk; every flushNote() saves a pending draft at once. 0 saves on every
    // change (the test harnesses' setting).
    property int noteSaveDelay: 400
    Timer { id: noteSaveTimer; objectName: "noteSaveTimer"; interval: Math.max(1, app.noteSaveDelay); onTriggered: app.flushNote() }
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
    // Re-import an installed version whose text predates the current schema:
    // catalog versions download again; local imports ask for their file. Both
    // reuse the install/replace flow, so annotations (keyed by version) stay.
    property string reimportId: ""
    function catalogEntry(id) { return Array.from(catalog.entries).find(function(entry) { return entry.id === id }) || null }
    function reimportVersion(entry) {
        if (!entry || closingWindow || bible.busy || catalog.busy || !flushNote()) return
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
        if (closingWindow || bible.busy || catalog.busy || !flushNote()) return
        bible.clearError(); bible.importUsfm(file, expected)
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
    onObservedReadyChanged: { if (observedReady) { restorePosition(); introduceKeys() } }
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
    function openManageTags() {
        if (bible.busy || !flushNote()) return
        clearCount()
        tagsUi.openManage()
    }
    readonly property alias tagsUi: tagsUi
    readonly property var tagBackend: bible
    readonly property var tagDesktop: desktop
    TagsPanel {
        id: tagsUi
        objectName: "tagsUi"
        app: app; bible: app.tagBackend; desktop: app.tagDesktop
        relatedHost: relatedPanel
    }
    onVerseModeChanged: clearCount()
    property string verseCount: ""
    // Pending z prefix for zz/zt/zb. Implemented as single-key shortcuts,
    // since Qt's multi-key sequences stay in a partial-match state that
    // swallows unrelated keys (digits, Ctrl+K) across focus changes.
    property bool zPending: false
    // Which key: a key that waits for another (z, a count, character
    // selection's g, i, a) shows what can follow after a moment.
    readonly property bool keyPending: zPending || verseCount.length > 0 || (charMode && charPending.length > 0)
    property bool whichKeyShown: false
    onKeyPendingChanged: { whichKeyShown = false; if (keyPending) whichKeyTimer.restart(); else whichKeyTimer.stop() }
    Timer { id: whichKeyTimer; objectName: "whichKeyTimer"; interval: 400; onTriggered: app.whichKeyShown = app.keyPending }
    function whichKeyParts() {
        if (charMode && charPending === "g") return [["G", "chapter start"], ["E", "previous word end"]]
        if (charMode && (charPending === "i" || charPending === "a")) return [["W", charPending === "i" ? "word" : "word and space"], ["S", "sentence"]]
        if (zPending) return [["Z", "center"], ["T", "to top"], ["B", "to bottom"]]
        if (verseCount.length) return [[["J", "K"], verseCount + " verses"], ["G", "verse " + verseCount], [["H", "L"], verseCount + " chapters"],
            ["Escape", "cancel", function() { app.clearCount() }]]
        return []
    }
    // Shown once, after the first version is installed: the keys to start with.
    Settings {
        id: discovery
        location: StandardPaths.writableLocation(StandardPaths.AppConfigLocation) + "/discovery.ini"
        category: "keys"
        property bool introduced: false
    }
    property bool keyStripShown: false
    function introduceKeys() { if (readerInitialized && !discovery.introduced) { keyStripShown = true; discovery.introduced = true } }
    function dismissKeyStrip() { keyStripShown = false }
    onActiveFocusItemChanged: {
        clearCount()
        if (activeFocusItem && focusWithin(notesDrawer.contentItem.parent)) marginKeys = true
    }
    // Notes sits beside the text: the keyboard belongs either to it
    // (marginKeys) or to the reader. Focus entering Notes gives it to Notes;
    // only the reader taking it (readerFocus, a text drag) gives it back.
    // Menus, dialogs, and pickers, and Qt's own hand-back as a popup closes,
    // leave it as it was, so restorePanelFocus returns the keyboard to where
    // it came from.
    property bool marginKeys: false
    // Notes in Write mode: while the keyboard is in Notes, the editor keeps
    // it. A press anywhere on the panel's chrome (its buttons, labels,
    // background; notesPanel's passive PointHandler) hands it straight back,
    // as do closing menus and dialogs (restorePanelFocus, the menu's own
    // restore) and Write. Only Linked from (ctrl ↓) takes it, until esc.
    readonly property bool notesWriting: notesOpen && notesDrawer.shown && hasSelection && !notePreview && !dialogOpen && !modalOpen
    onNotesWritingChanged: if (notesWriting) noteRefocusTimer.restart()
    Timer { id: noteRefocusTimer; interval: 0; onTriggered: app.keepNoteEditorFocus() }
    function keepNoteEditorFocus() {
        if (!notesWriting || !marginKeys || bible.busy || noteEditor.activeFocus || linkedList.activeFocus) return
        const item = activeFocusItem
        const chrome = !item || item === notesDrawer.contentItem.parent || focusWithin(notesDrawer.contentItem)
        if (chrome && noteEditor.enabled) noteEditor.forceActiveFocus()
    }
    property bool restoringPosition: true
    property bool positionJumpToSelection: false
    readonly property int readerSize: bible.fontSize
    // ctrl + wheel: partial wheel steps (touchpads) add up to whole sizes.
    property real zoomSteps: 0
    // The page's body text, in pixels: every step of TypeScale.js is relative to it.
    readonly property real readerBodySize: readerSize * readerScale
    property int selectionAnchor: 0
    property bool movingSelection: false
    property int paletteBook: 0
    property string paletteBookName: ""
    // The palette's History: every passage kept in History, newest first
    // (ctrl h, or "All History" below the recent passages).
    property bool paletteHistory: false
    // A book's chapters are a grid of numbers (paletteChapters); everything
    // else is a list (paletteResults). paletteList is the one showing.
    readonly property int paletteColumns: 5
    readonly property bool paletteChapterGrid: !paletteTextMode && paletteBook > 0 && !referenceInput.text.trim().startsWith(">")
    readonly property Item paletteList: paletteChapterGrid ? paletteChapters : paletteResults
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
    // Bible search results reapply their own syntax; chapter find (`/`) takes
    // a smart-case regular expression (\c ignores case, \C matches it).
    function localSearch(query, bibleSyntax) {
        const result = bibleSyntax ? bible.findInChapter(query, true) : bible.findPatternInChapter(query)
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
        if (!bible.ready || bible.busy || !flushNote()) return
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
        if (searchError.length) { rejected(searchError); return }
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
        if (!acceptedSearchQuery.length) return
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
        }
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
        if (!rect || rect.height <= 0 || focusLayout) { scrollToVerse(segment.verse, false); return }
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
        if (!bible.ready || !flushNote()) return
        openPalette(false)
        referenceInput.text = "/ "
        referenceInput.cursorPosition = 2
    }
    function cyclePaletteMode(direction) {
        if (!bible.ready) return
        const current = paletteTextMode ? 1 : referenceInput.text.trim().startsWith(">") ? 2 : 0
        const next = (current + direction + 3) % 3
        if (next === 1) openTextSearch()
        else openPalette(next === 2)
    }
    // How many versions the shown results come from.
    readonly property int searchedVersions: {
        const versions = {}
        for (const entry of Array.from(bible.searchBooks)) versions[entry.versionId] = true
        return Object.keys(versions).length
    }
    // alt a: Bible search in every installed version, or the active one only.
    function toggleSearchScope() {
        if (bible.versions.length < 2) { rejected("Add another version to search them all"); return }
        bible.setSearchAllVersions(!bible.searchAllVersions)
        notify(bible.searchAllVersions ? "Searching every version" : "Searching " + bible.versionAbbreviation + " only", true)
        if (paletteTextMode && paletteTextQuery.length) submitBibleSearch()
    }
    function toggleSearchRegex() {
        bible.setSearchRegex(!bible.searchRegex)
        notify(bible.searchRegex ? "Searching with a regular expression" : "Searching for words", true)
        if (paletteTextMode && paletteTextQuery.length) submitBibleSearch()
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
        let entry = paletteList.model[paletteList.currentIndex]
        if (entry && entry.candidate) return { entry: entry, reference: entry.reference }
        if (entry && (paletteArrowUsed || !query || query.startsWith(">") || paletteHistory
                || (paletteBook > 0 && /^\d+$/.test(query))
                || (paletteBook === 0 && !/\d/.test(query)))) {
            return { entry: entry, reference: entry.chapter ? entry.title : "" }
        }
        // A book's grid takes "3:16" for that book's chapter and verse.
        return { entry: null, reference: paletteBook > 0 && /^[\d:\s–-]+$/.test(query) ? paletteBookName + " " + query : query }
    }
    readonly property var paletteResolution: {
        if (paletteTextMode) return { valid: false, reference: "", error: "" }
        // Relative references depend on the current book, even though the resolver is pure.
        let currentBook = bible.book
        let ready = bible.ready
        let destination = paletteDestination
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
    readonly property bool modalOpen: dialogOpen || tagsUi.visibleModal
    // Everything modal: menus, cards, palettes, pickers, and dialogs.
    readonly property bool dialogOpen: contextMenu.visible || peekCard.visible || tagsUi.pickerVisible || verseTagMenu.visible || backupScope.visible || versionDialog.visible || besideDialog.visible || passagePalette.visible || cleanDeskConfirmation.visible || clearHistoryConfirmation.visible || helpDialog.visible || settingsDialog.visible || aboutDialog.visible || importGuide.visible || catalogDetails.visible || errorDialog.visible || restoreConfirmation.visible || importDialog.visible || exportDialog.visible || backupDialog.visible || restoreDialog.visible
    // One side panel at a time, in the Margin. Its opening key brings the
    // keyboard to it; another panel's key turns the Margin to that panel.
    readonly property string sidePanel: notesOpen ? "notes" : deskOpen ? "desk" : marksOpen ? "annotations"
        : relatedOpen ? "related" : ""
    readonly property var panelActions: ["notes", "annotations", "desk", "related"]
    // With the keyboard in a side panel, these keys still act, over it:
    // palettes, dialogs, and reading toggles.
    readonly property var panelLeavingActions: ["books", "history", "bibleSearch", "commands", "help", "settings", "switchVersion",
        "about", "import", "removeVersion", "focus", "numbers", "headings", "wordsOfJesus", "chapterOutline",
        "marginNotes", "readingMode", "soundEffects", "reduceMotion"]
    readonly property bool panelSwitchKeys: sidePanel.length > 0 && bible.ready && !bible.busy && !dialogOpen
        && !findPromptOpen && !textInputActive()
    // Leaves the open side panel for another (the Margin stays open and
    // turns to it) or for none (target "": the Margin closes). Its drafts
    // save first (a note, the Desk page, a mark's note written in place);
    // when one cannot, the panel stays with its error.
    function leaveSidePanel(target) {
        const current = sidePanel
        if (current === target) return false
        if (!flushNote()) return false
        if (current === "annotations" && markEditKey.length && !finishMarkNote(false)) return false
        return target.length > 0 || closeNotes()
    }
    // The chapter outline, reached by tab, keeps its scroll keys.
    readonly property bool readerBase: bible.ready && !bible.busy && !modalOpen && !(marginOpen && marginKeys) && !findPromptOpen && !textInputActive()
        && !(chapterOutline && chapterOutline.activeFocus)
    // Character selection takes the reader's keys itself (charKey).
    readonly property bool readerKeys: readerBase && !charMode
    onReaderKeysChanged: { if (!readerKeys) clearCount() }
    // Notes docks beside the text where the window keeps a comfortable column
    // (and the reader's widest side insets) beside it: the reader and footer
    // make room. Narrower, it is a sheet over the reader's right side.
    // Neither dims or blocks the text.
    readonly property real readerComfortWidth: readerMeasure + verseGutter + 9 + 2 * 48
    readonly property real notesMinWidth: Math.min(390 * scale, width - 40)
    readonly property real notesMaxWidth: 620 * scale
    readonly property bool notesDocked: width >= readerComfortWidth + notesMinWidth
    // Docked, it grows into the room the text does not need, up to
    // notesMaxWidth; margin notes step aside while it is open (marginSpace).
    readonly property real notesWidth: notesDocked
        ? Math.max(notesMinWidth, Math.min(notesMaxWidth, width - readerComfortWidth)) : notesMinWidth
    // The room docked Notes takes from the reader (dockMargin): the reader
    // lays out once at its new width and glides there from where it was
    // (glideFromDock, as soon as the column's new place is known); the footer
    // and the other chrome at the bottom follow chromeDock, which glides
    // alongside. Resizes jump.
    property real notesDock: 0
    property real chromeDock: 0
    // The column's x before a dock change whose new width is still to land.
    property real dockSlideFrom: NaN
    NumberAnimation { id: chromeGlide; target: app; property: "chromeDock"; duration: app.panelEnterMs; easing.type: Easing.OutCubic }
    function dockMargin() {
        const target = notesDocked && marginOpen ? notesWidth : 0
        if (target === notesDock) return
        const glide = (target > 0) !== (notesDock > 0) && !reduceMotion && readerInitialized && bible.ready
        dockSlideFrom = glide ? readerColumn.x : NaN
        notesDock = target
        chromeGlide.stop()
        if (glide) { chromeGlide.from = chromeDock; chromeGlide.to = target; chromeGlide.start() }
        else chromeDock = target
    }
    onMarginOpenChanged: dockMargin()
    onNotesDockedChanged: dockMargin()
    onNotesWidthChanged: dockMargin()
    onColumnTargetXChanged: if (!isNaN(dockSlideFrom)) glideFromDock()
    function glideFromDock() {
        marginSlide.stop()
        marginSlideOffset = dockSlideFrom - columnTargetX
        marginsAttached = marginSlideOffset !== 0
        if (marginsAttached) marginSlide.start()
    }
    function endDockGlide() { dockSlideFrom = NaN }
    // The window's width beside docked Notes: the footer's.
    readonly property real stageWidth: width - notesDock
    readonly property real readerLineHeight: readerMetrics.height * bible.lineHeight
    readonly property real observedLineHeight: bible.lineHeight
    // Passage text in panels follows the reader's line height proportionally:
    // 1.25 at the default 1.5, so the panels stay compact but track the setting.
    readonly property real panelLineHeight: bible.lineHeight > 0 ? bible.lineHeight / 1.5 * 1.25 : 1.25
    onObservedLineHeightChanged: {
        // Replacing the rich document invalidates Qt's selection offsets.
        // Clear the shared clipboard source in the same update as the paint;
        // a character selection is mapped onto the new layout instead.
        relayoutTextSelection()
        draggingText = false
        dragAnchorRow = -1
    }
    readonly property real floatingSafety: focusLayout ? 0 : Math.max(72 * systemScale, footerHeight + 2 * footerMargin)
    readonly property real verseGutter: 76 * readerScale
    // Source rows, rather than numeric verse labels, define chapter intervals.
    // Union each kind's continuous coverage: overlapping logical ranges already
    // share one rail. Allocate once per interval so later starts/ends never bend it.
    readonly property var annotationLanes: packAnnotationLanes(bible.verses)
    function packAnnotationLanes(sourceRows) {
        const rows = Array.from(sourceRows), kinds = ["tag", "note"]
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
        const occupiedUntil = [-1, -1]
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
    onObservedFocusModeChanged: {
        toastTimer.stop(); footerHoverRevealed = false; footerHideTimer.stop()
        applyFocusLayout()
        Qt.callLater(app.applyFocusMode)
    }
    function applyFocusMode() {
        if (observedFocusMode) {
            if (!closeNotes()) { bible.setFocusMode(false); return }
            tagsUi.closeForFocusMode()
            verseTagMenu.close()
            passagePalette.close(); helpDialog.close(); aboutDialog.close()
            if (!settingsDialog.visible) readerFocus()
        }
    }
    // Focus mode's layout (no headings, verse numbers, decoration cells,
    // margins, or room for the footer) follows bible.focusMode through
    // focusLayout, flipped in one step that settles the new layout before its
    // first frame: one rebuild; the reading position keeps its place on
    // screen, then (entering, with a selection) slides to the typewriter line;
    // the column glides between its margin and centred places while the
    // margins fade out with it, or fade in where they settle. Paint-only
    // effects (dimming, the quiet footer) follow bible.focusMode itself.
    property bool focusLayout: false // bible.focusMode on completion, then applyFocusLayout
    function applyFocusLayout() {
        const entering = bible.focusMode
        if (focusLayout === entering) return
        marginExit.stop(); marginEnter.stop()
        if (!readerInitialized || !bible.ready) {
            focusLayout = entering; marginsLeaving = false; marginFocusLevel = entering ? 0 : 1
            return
        }
        const motion = !reduceMotion
        const anchor = readingAnchor()
        const atTop = reading.contentY <= 1
        const fromX = readerColumn.x
        // The chapter buttons' centre, less the part the layout moves.
        const navFrom = chapterNavShift - floatingSafety / 2
        const marginsShown = marginFocusLevel > 0 && (marginSpace.notes.visible || outlineShown)
        // Entering, shown margins stay laid out (beside the reflowed text) while they fade.
        if (entering) { leavingSpace = marginSpace; marginsLeaving = motion && marginsShown; focusLayout = true }
        else { focusLayout = false; marginsLeaving = false }
        settleReaderLayout()
        if (anchor) holdReadingAnchor(anchor)
        // Shown margins move with the column; hidden ones appear where they settle.
        marginsAttached = motion && marginsShown
        marginSlide.stop()
        marginSlideOffset = motion ? fromX - columnTargetX : 0
        if (marginSlideOffset !== 0) marginSlide.start()
        chapterNavGlide.stop()
        chapterNavShift = motion ? navFrom + floatingSafety / 2 : 0
        if (chapterNavShift !== 0) chapterNavGlide.start()
        if (entering && marginsLeaving) marginExit.start()
        else if (!entering && motion) marginEnter.start()
        else marginFocusLevel = entering ? 0 : 1
        // Then the typewriter line (entering), the chapter top for a reader
        // who was there, or back inside the content's ends.
        if (entering && hasSelection) placeVerse(bible.cursorVerse, false, !motion)
        else slideReader(atTop ? 0 : reading.contentY)
    }
    // A layout change applied now rather than on the timers' next turn: the
    // Paragraph/Block rebuild, the column's positions, the margins placed
    // (which the margin timer, restarted by the change, need not redo).
    function settleReaderLayout() {
        const block = blockLoader.item as BlockReader
        if (block) block.settle()
        readerColumn.forceLayout()
        updateMargins()
        marginTimer.stop()
    }
    // The verse holding the reading position, with its top's place in the
    // viewport: the cursor verse while it is on screen, else the first one
    // showing.
    function readingAnchor() {
        const top = reading.contentY, bottom = top + Math.max(1, reading.height - floatingSafety)
        let anchor = hasSelection && !bible.crossChapterSelection ? measureVerse(bible.cursorVerse) : null
        if (!anchor || anchor.bottom <= top || anchor.top >= bottom) {
            const extents = measureVerses()
            anchor = extents.find(function(e) { return e.bottom > top }) || extents[extents.length - 1] || null
        }
        return anchor ? { verse: anchor.number, offset: anchor.top - top } : null
    }
    // After a relayout, the anchor verse back at its place on screen, for a
    // moment even past the content's ends (the slide that follows clamps).
    function holdReadingAnchor(anchor) {
        const extent = measureVerse(anchor.verse)
        if (!extent) return
        readerSlide.stop()
        reading.contentY = extent.top - anchor.offset
    }
    readonly property real controlScale: Math.min(scale, 1.4)
    // Motion feedback is paint-only: opacity and translucent layers never change
    // layout, text, selection, copy output, or hit-testing. Reduce motion (shared
    // app-wide on `sounds`) collapses every duration to an instant change.
    // Motion needs both the app's Animations setting and Hyprland's.
    readonly property bool reduceMotion: sounds.reduceMotion || !desktop.animations
    readonly property int fadeMs: reduceMotion ? 0 : 100
    // Side panels: enter 125 ms over the full slide; exit 85 ms over a short one.
    readonly property int panelEnterMs: reduceMotion ? 0 : 125
    readonly property int panelExitMs: reduceMotion ? 0 : 85
    readonly property real panelExitPosition: reduceMotion ? 0 : 0.85
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
    onReduceMotionChanged: if (reduceMotion) { pulseAnimation.stop(); flashAnimation.stop(); pulseLevel = 0; flashLevel = 0; settleMargins() }
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
    // Jump pulse and copy flash: a quick rise, a longer settle.
    readonly property int pulseInMs: 125
    readonly property int pulseOutMs: 295
    readonly property int flashInMs: 65
    readonly property int flashOutMs: 180
    SequentialAnimation {
        id: pulseAnimation
        NumberAnimation { target: app; property: "pulseLevel"; from: 0; to: 1; duration: app.pulseInMs; easing.type: Easing.OutQuad }
        NumberAnimation { target: app; property: "pulseLevel"; to: 0; duration: app.pulseOutMs; easing.type: Easing.InOutQuad }
    }
    SequentialAnimation {
        id: flashAnimation
        NumberAnimation { target: app; property: "flashLevel"; from: 0; to: 1; duration: app.flashInMs; easing.type: Easing.OutQuad }
        NumberAnimation { target: app; property: "flashLevel"; to: 0; duration: app.flashOutMs; easing.type: Easing.InQuad }
    }
    FontMetrics { id: readerMetrics; font.family: desktop.fontFamily; font.pixelSize: app.readerSize * app.readerScale }
    // Highlights: a highlighter pen in the theme's yellow (Omarchy color3,
    // else the accent), one stroke per line about the x-height: from a
    // little above it to a little below the baseline, never the full line,
    // offset from the line's top (text sits at the top of its line box).
    readonly property color highlightInk: desktop.color3 !== undefined && desktop.color3.valid ? desktop.color3 : desktop.accent
    readonly property real highlightAlpha: desktop.dark ? 0.34 : 0.36
    readonly property var highlightBand: highlightBandFor(readerMetrics)
    function highlightBandFor(metrics) { return HandDrawn.highlightBand(metrics.ascent, metrics.descent, metrics.xHeight) }
    // The Verse layout's rows, one per verse: made again only when the
    // chapter or its text changes, so marks (which change bible.verses)
    // update rows in place (each reads bible.verses[index]).
    property var verseRows: []
    property string verseRowsKey: ""
    readonly property var noVerse: ({ number: 0, text: "", html: "", heading: "", footnotes: [], indents: [],
        highlightRanges: [], tagRanges: [], noteRanges: [], hasNote: false, hasTags: false })
    function syncVerseRows() {
        const verses = bible.verses
        const key = [bible.textGeneration, bible.book, bible.chapter, verses.length,
            verses.length ? verses[0].number + "-" + verses[verses.length - 1].number : ""].join(":")
        if (key === verseRowsKey) return
        verseRowsKey = key
        verseRows = Array.from(verses).map(function(verse) { return verse.number })
    }
    Connections { target: bible; function onVersesChanged() { app.syncVerseRows() } }
    // Repaints every highlight (tests: paint is deterministic).
    signal highlightsRepaint()
    function refreshHighlights() { highlightsRepaint(); const block = blockLoader.item as BlockReader; if (block) block.repaintAnnotations() }

    // Positions are TextEdit document offsets (UTF-16), never verse numbers or
    // HTML offsets. Each delegate keeps its own rich layout and selection paint.
    property int dragAnchorRow: -1
    property int dragAnchorPosition: 0
    property bool draggingText: false
    property bool gutterDrag: false
    property point dragOrigin
    property point dragPoint
    property string freeSelectedText: ""
    // A relayout (text size, line height) replaces the documents: a
    // character selection is kept and reselected from its source positions
    // once the new layout exists; anything else is cleared.
    function relayoutTextSelection() {
        if (charMode) Qt.callLater(app.reapplyCharSelection)
        else clearTextSelection()
    }
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
            if (!gutterDrag) textDragFocus((verseRepeater.itemAt(dragAnchorRow) as VerseItem).body)
        }
        applyTextDrag()
    }
    function applyTextDrag() {
        let hit = textHit(dragPoint)
        if (hit.row < 0 || dragAnchorRow < 0) return
        // The anchor is a real row, so omitted verse numbers are harmless.
        const moved = dragSelectVerses(bible.verses[dragAnchorRow].number, bible.verses[hit.row].number)
        if (gutterDrag) {
            if (moved) readerFocus()
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
    // A drag, over the numbers or the text, selects the verses it runs over
    // as it goes: from the one it began in to the one under the pointer. The
    // selection bar waits for the release (mouseSelected). Whether it moved.
    function dragSelectVerses(from, to) {
        if (from <= 0 || to <= 0) return false
        if (bible.selectedStart === Math.min(from, to) && bible.selectedEnd === Math.max(from, to) && bible.cursorVerse === to) return false
        if (!flushNote()) return false
        selectionBarWanted = false
        selectionAnchor = from
        bible.selectVerses(from, to)
        syncNote()
        return true
    }
    // Double-clicking verse text selects the word under the pointer (the verse
    // number opens Notes). The release that ends the double click keeps it.
    property bool wordClick: false
    function selectWordAt(index, source, mouse) {
        const row = verseRepeater.itemAt(index) as VerseItem
        if (!row || !row.body) return
        clearTextSelection()
        const p = row.body.mapFromItem(source, mouse.x, mouse.y)
        row.body.cursorPosition = row.body.positionAt(p.x, p.y)
        row.body.selectWord()
        freeSelectedText = row.body.selectedText.replace(/[\uFFFC\u2060]/g, "")
        wordClick = freeSelectedText.length > 0
        publishPrimary()
    }
    // A finished mouse selection (drag or double-clicked word) becomes the
    // desktop's PRIMARY selection, as plain selected text.
    function publishPrimary() { if (freeSelectedText.length) bible.setPrimarySelection(freeSelectedText) }
    function finishTextDrag(number, mouse) {
        if (wordClick) { wordClick = false; draggingText = false; dragAnchorRow = -1; return }
        if (draggingText) publishPrimary()
        if (!draggingText) { clearTextSelection(); selectVerse(number, !!(mouse.modifiers & Qt.ShiftModifier)) }
        draggingText = false
        dragAnchorRow = -1
        mouseSelected()
    }
    // A selection made with the mouse offers its common actions in a bar
    // above it (Highlight · Note · Tag · Copy · more); any key, another
    // selection by key, or a panel puts the bar away.
    property bool selectionBarWanted: false
    // Its own ⋯ menu keeps it up.
    readonly property bool selectionBarShown: selectionBarWanted && hasSelection
        && (readerKeys || (contextMenu.visible && contextMenu.target === "selectionBar"))
    function mouseSelected() { selectionBarWanted = hasSelection; Qt.callLater(app.placeSelectionBar) }
    property real selectionBarY: 0
    function placeSelectionBar() {
        if (!selectionBarWanted || !hasSelection) return
        // Rows rebuilt for new marks are placed at the next polish: place them now.
        readerColumn.forceLayout()
        const number = bible.selectedStart
        const block = blockLoader.item as BlockReader
        let at = Qt.point(verseGutter, 0)
        if (block) { const r = block.verseRect(number); at = block.body.mapToItem(readerColumn, r.x, r.y) }
        else {
            const index = Array.from(bible.verses).findIndex(function(v) { return v.number === number })
            const row = index >= 0 ? verseRepeater.itemAt(index) as VerseItem : null
            if (row && row.body) at = row.body.mapToItem(readerColumn, 0, 0)
        }
        selectionBarX = readerColumn.x + at.x
        selectionBarY = readerColumn.y + at.y
    }
    property real selectionBarX: 0
    // Actions that open something else (Notes, the tag picker, Versions,
    // Study) put the bar away; the rest keep it up for the next.
    function selectionBarRun(action, leaves) {
        if (leaves) selectionBarWanted = false
        runCommand(action)
    }
    function selectionBarParts() {
        const ready = selectionBarShown
        return [["X", bible.selectionHighlighted ? "unhighlight" : "highlight", function() { app.selectionBarRun("highlight") }, ready],
            ["W", "note", function() { app.selectionBarRun("notes", true) }, ready],
            ["T", "tag", function() { app.selectionBarRun("tags", true) }, ready],
            ["Y", "copy", function() { app.selectionBarRun("copyText") }, ready],
            [[], "⋯", app.selectionBarMore, ready, "More: Desk, Compare versions, Copy plain text, See also"]]
    }
    function selectionBarMore() {
        const more = selectionBarHints.partItems()[4]
        const p = more ? more.mapToItem(null, 0, more.height) : selectionBar.mapToItem(null, selectionBar.width, selectionBar.height)
        contextMenu.show("selectionBar", [
            { id: "desk", text: "Add to Desk", shortcut: menuKey("toggleDesk"),
                enabled: actionAvailable("toggleDesk"), run: function() { app.selectionBarRun("toggleDesk") } },
            { id: "compare", text: "Compare versions", shortcut: menuKey("compare"),
                enabled: actionAvailable("compare"), run: function() { app.selectionBarRun("compare", true) } },
            { id: "copyPlain", text: "Copy plain text", shortcut: menuKey("copyPlain"),
                enabled: actionAvailable("copyPlain"), run: function() { app.selectionBarRun("copyPlain") } },
            { id: "related", text: "See also", shortcut: menuKey("related"),
                enabled: actionAvailable("related"), run: function() { app.selectionBarRun("related", true) } }
        ], p.x, p.y)
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
            else openNoteScope(ranges[0])
            return
        }
        verseTagMenu.annotationKind = kind
        verseTagMenu.entries = ranges
        verseTagMenu.origin = Qt.point(sceneX, sceneY)
        verseTagMenu.popup(sceneX, sceneY)
    }
    function openRangeEntries(sceneX, sceneY) {
        const ready = !bible.busy
        const entries = [["note", "openNote", "Open note"], ["tag", "openTags", "Open tags…"]]
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
            run: function() { app.copyMenuSelection(false) } },
            { id: "copySelectedPlain", text: "Copy selected plain text", shortcut: menuKey("copyTextPlain"), enabled: !bible.busy,
            run: function() { app.copyMenuSelection(true) } }, menuDivider]
    }
    // Copying from the menu ends a character selection, as y does.
    function copyMenuSelection(plainText) {
        if (charMode) copyCharSelection(plainText)
        else copySelectedText(plainText)
    }
    // Selected text (mouse-dragged or character selection): ctrl c copies it
    // as a quotation (ellipses at cut verses, numbers for verses whose start is
    // selected), ctrl shift C as plain text, exactly as selected. A selection
    // that maps to no Scripture text (a heading alone) copies its plain text.
    function copySelectedText(plainText) {
        if (!freeSelectedText.length || bible.busy) { rejected(bible.busy ? unavailableReason("copy") : "Nothing selected to copy"); return false }
        // Plain text is the selection as shown (character selection: its source text).
        const range = plainText ? null : selectedSourceRange()
        const text = range ? bible.copyCharacters(range.start.verse, range.start.offset, range.end.verse, range.end.offset, false) : ""
        if (!text.length) copyFreeText()
        if (range && text.length) flashVerses(range.start.verse, range.end.verse)
        notify(plainText || !text.length ? "Selected text copied as plain text" : "Selected text copied as quotation", true)
        sounds.play("chime")
        return true
    }

    // ── Character selection (v): Vim's charwise visual over the source
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
    // The passage menu opened from character selection keeps it (its copy
    // entries copy it); anything else that takes the reader ends it.
    onReaderBaseChanged: if (!readerBase && charMode && !(contextMenu.visible && contextMenu.target === "passage")) endCharMode()
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
        if (!bible.ready || bible.busy || !bible.verses.length) { rejected(unavailableReason("characterVisual")); return false }
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
    function reapplyCharSelection() { if (charMode && charAnchor && charCursor) applyCharSelection() }
    // Text size and scale reflow the Verse rows too: reselect once laid out.
    readonly property var charLayoutKey: [readerSize, readerScale, bible.lineHeight]
    onCharLayoutKeyChanged: if (charMode) charReapplyTimer.restart()
    Timer { id: charReapplyTimer; interval: 0; onTriggered: app.reapplyCharSelection() }
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
        // The passage menu at the cursor: its copy entries copy this selection.
        if (event.key === Qt.Key_Menu || (event.key === Qt.Key_F10 && (mods & Qt.ShiftModifier))) {
            clearCount(); charPending = ""; openPassageMenuAtCursor(); return true
        }
        // Scroll keys scroll the reader; the cursor stays where it is.
        const page = Math.max(readerLineHeight, reading.height - floatingSafety - 2 * readerLineHeight)
        const scroll = mods === Qt.ControlModifier
            ? { [Qt.Key_D]: page / 2, [Qt.Key_U]: -page / 2, [Qt.Key_F]: page, [Qt.Key_B]: -page }[event.key]
            : mods === Qt.NoModifier ? { [Qt.Key_PageDown]: page, [Qt.Key_PageUp]: -page }[event.key] : undefined
        if (scroll !== undefined) {
            clearCount(); charPending = ""
            const target = Math.max(0, Math.min(reading.contentHeight - reading.height, readerTargetY() + scroll))
            if (Math.abs(target - readerTargetY()) < 1) sounds.play("boundary")
            else slideReader(target)
            return true
        }
        if (mods & Qt.ControlModifier) {
            if (event.key === Qt.Key_C && !(mods & Qt.AltModifier)) { copyCharSelection(!!(mods & Qt.ShiftModifier)); return true }
            // ctrl z undoes, ctrl shift Z and ctrl r redo, as in the reader.
            const shifted = !!(mods & Qt.ShiftModifier)
            const undoKey = event.key === Qt.Key_Z && !shifted
            if (!(mods & Qt.AltModifier) && (undoKey || (event.key === Qt.Key_Z && shifted) || (event.key === Qt.Key_R && !shifted))) {
                clearCount(); charPending = ""; runCommand(undoKey ? "undo" : "redo"); return true
            }
            clearCount(); rejected(charKeyReason); return true
        }
        if (mods & Qt.AltModifier) { clearCount(); rejected(charKeyReason); return true }
        // Named keys are motions, never count digits (home is 0's motion, not a 0).
        const named = { [Qt.Key_Left]: "h", [Qt.Key_Right]: "l", [Qt.Key_Up]: "k", [Qt.Key_Down]: "j", [Qt.Key_Home]: "0", [Qt.Key_End]: "$" }
        const isNamed = named[event.key] !== undefined
        let text = named[event.key] || event.text
        // A shifted letter is its capital even where the event's text is not.
        if (!isNamed && (mods & Qt.ShiftModifier) && event.key >= Qt.Key_A && event.key <= Qt.Key_Z && text.length === 1)
            text = text.toUpperCase()
        if (!text.length) { clearCount(); rejected(charKeyReason); return true }
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
        if (!isNamed && (/^[1-9]$/.test(text) || (text === "0" && verseCount.length))) { countDigit(text); return true }
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
        case "u": clearCount(); runCommand("undo"); break
        // v (its own key) leaves; shift V switches to verse (Visual) selection.
        case "v": endCharMode(); readerFocus(); break
        case "V": endCharMode(); setVisualMode(true); readerFocus(); break
        default: clearCount(); rejected(charKeyReason)
        }
        return true
    }
    readonly property string charKeyReason: "Not a key in character selection · " + KeyNotation.format("Escape") + " leaves it"
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
            menuAction("highlight", bible.selectionHighlighted ? "Remove highlight" : "Highlight", "highlight"),
            menuAction("note", bible.noteText.length ? "Edit note" : "Write note", "notes"),
            menuAction("tags", "Tags…", "tags")
        ].concat(undoMenuEntries()).concat([
            menuDivider,
            menuAction("desk", "Add to Desk", "toggleDesk"),
            menuDivider,
            menuAction("related", "See also", "related"),
            menuAction("chapterFind", "Find in chapter", "chapterFind")
        ]).concat(openRangeEntries(sceneX, sceneY))
            .concat(bible.focusMode ? [menuDivider, menuAction("leaveFocus", "Leave focus mode", "focus")] : [])
    }
    // Undo and Redo, named, while there is something to undo or redo.
    function undoMenuEntries() {
        return (bible.canUndo ? [menuAction("undo", "Undo " + bible.undoLabel, "undo")] : [])
            .concat(bible.canRedo ? [menuAction("redo", "Redo " + bible.redoLabel, "redo")] : [])
    }
    // Right-click outside the selection selects that verse; inside keeps the range.
    function openPassageMenu(number, sceneX, sceneY) {
        if (!bible.ready || bible.busy) return
        clearCount()
        // Selected text (mouse-dragged or character selection) stays selected
        // when the click is on it: the menu leads with copying it. A click
        // elsewhere drops it and takes the verse path below.
        if (freeSelectedText.length) {
            if (pointInSelectedText(sceneX, sceneY)) {
                contextMenu.show("passage", selectedTextEntries().concat(passageMenuEntries(sceneX, sceneY)), sceneX, sceneY)
                return
            }
            clearTextSelection()
        }
        if (number > 0) {
            const inside = hasSelection && number >= bible.selectedStart && number <= bible.selectedEnd
            if (!inside) {
                selectVerse(number, false)
                if (bible.selectedStart !== number) return
            }
        }
        contextMenu.show("passage", passageMenuEntries(sceneX, sceneY), sceneX, sceneY)
    }
    // Whether a scene point is on the selected text (its line, within a line
    // height of a selected character).
    function pointInSelectedText(sceneX, sceneY) {
        const range = selectedSourceRange()
        if (!range) return false
        const p = readerColumn.mapFromItem(null, sceneX, sceneY)
        const hit = charAtPoint(p.x, p.y)
        if (!hit || Math.abs(hit.rect.x - p.x) > readerLineHeight) return false
        return charCompare(hit.pos, range.start) >= 0 && charCompare(hit.pos, range.end) < 0
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
    // The menu key (shift F10) in a text field opens its text menu at the caret,
    // except in the palette while its current row has a menu: the key opens
    // that row's menu there (a right click on the field still gives the text menu).
    Shortcut {
        objectName: "textMenuKey"
        // Application context: modal popups (palette, dialogs) hold text fields too.
        context: Qt.ApplicationShortcut
        sequences: ["Menu", "Shift+F10"]
        enabled: app.active && app.textInputActive() && !app.contextMenuOpen
            && !(passagePalette.visible && paletteResults.currentIndex >= 0 && !!app.paletteRowMenu(paletteResults.model[paletteResults.currentIndex]))
        onActivated: {
            const field = app.activeFocusItem
            if (!field || typeof field.positionToRectangle !== "function") { app.rejected(); return }
            const r = field.positionToRectangle(field.cursorPosition)
            app.openTextMenu(field, Qt.point(r.x, r.y + r.height))
        }
    }
    // Text fields use the app's menu, never Qt's built-in editing menu
    // (ContextMenu.menu: null): Cut · Copy · Paste · Select all, and in the
    // note editor Open link and Preview. `position` is in the field's coordinates.
    function openTextMenu(field, position) {
        if (!field) return
        const point = field.mapToItem(null, position.x, position.y)
        const editable = !field.readOnly && field.enabled
        const selected = field.selectedText.length > 0
        let entries = [
            { id: "cut", text: "Cut", shortcut: KeyNotation.format("Ctrl+X"), enabled: editable && selected, run: function() { field.cut() } },
            { id: "copyField", text: "Copy", shortcut: KeyNotation.format("Ctrl+C"), enabled: selected, run: function() { field.copy() } },
            { id: "paste", text: "Paste", shortcut: KeyNotation.format("Ctrl+V"), enabled: editable && field.canPaste, run: function() { field.paste() } },
            { id: "selectAll", text: "Select all", shortcut: KeyNotation.format("Ctrl+A"), enabled: field.length > 0, run: function() { field.selectAll() } }
        ]
        if (field === noteEditor) {
            // The link under the pointer (else at the caret) opens as alt enter does.
            const at = field.positionAt(position.x, position.y)
            if (!selected) field.cursorPosition = at
            entries = entries.concat([menuDivider,
                { id: "openLink", text: "Open link", shortcut: menuKey("openNoteLink"), enabled: !!noteLinkAtCursor || noteUrlAtCursor.length > 0,
                    run: function() { app.openNoteLinkAtCursor() } },
                { id: "preview", text: "Preview", shortcut: menuKey("notePreview"), enabled: hasSelection, run: function() { app.toggleNotePreview() } }])
        } else if (field === notePreviewText) {
            entries = entries.concat([menuDivider,
                { id: "write", text: "Write", shortcut: menuKey("notePreview"), enabled: hasSelection, run: function() { app.toggleNotePreview() } }])
        }
        field.forceActiveFocus()
        contextMenu.show("text", entries, point.x, point.y)
    }
    function openPassageMenuAt(sceneX, sceneY) {
        if (!bible.ready || bible.busy) return
        clearCount()
        if (freeSelectedText.length) {
            if (pointInSelectedText(sceneX, sceneY)) {
                contextMenu.show("passage", selectedTextEntries().concat(passageMenuEntries(sceneX, sceneY)), sceneX, sceneY)
                return
            }
            clearTextSelection()
        }
        if (!hasSelection) {
            const rows = Array.from(bible.verses)
            const number = bible.cursorVerse > 0 ? bible.cursorVerse : rows.length ? rows[0].number : 0
            if (number > 0) selectVerse(number, false)
        }
        contextMenu.show("passage", passageMenuEntries(sceneX, sceneY), sceneX, sceneY)
    }
    function openPassageMenuAtCursor() {
        if (!bible.ready || bible.busy) return
        if (!hasSelection && !freeSelectedText.length) {
            const rows = Array.from(bible.verses)
            const number = bible.cursorVerse > 0 ? bible.cursorVerse : rows.length ? rows[0].number : 0
            if (number > 0) selectVerse(number, false)
        }
        let point = verseMenuPoint(bible.cursorVerse)
        // Character selection: at its cursor.
        const rect = charMode ? charRect(charCursor) : null
        if (rect) point = readerColumn.mapToItem(null, rect.x, rect.y + rect.height)
        contextMenu.show("passage", (freeSelectedText.length ? selectedTextEntries() : []).concat(passageMenuEntries(point.x, point.y)), point.x, point.y)
    }
    function sceneRowPoint(item) {
        return item ? item.mapToItem(null, 24 * scale, Math.min(item.height, 28 * scale)) : Qt.point(width / 3, height / 3)
    }
    function copyScope(scope, plainText) {
        if (!scope || bible.busy) { rejected(unavailableReason("copy")); return }
        const copied = bible.copyScope(scope, !!plainText)
        if (copied.length) {
            flashScope(scope)
            notify((scope.reference || "Passage") + (plainText ? " copied as plain text" : " copied"), true)
            sounds.play("chime")
        } else sounds.play("error")
    }
    function removeMarkerAnnotation(scope, kind) {
        if (bible.busy || !scope || !flushNote()) return
        if (kind === "tag") {
            const tags = Array.from(bible.tagsForScope(scope))
            if (tags.length && bible.setTags(scope, tags.map(function(tag) { return tag.id }), false)) {
                notify((scope.reference || "Passage") + " tags removed", true); sounds.play("remove")
            } else sounds.play("error")
        } else deleteScope(scope, true)
    }
    // noteOnly deletes just the note on this exact range (marker menu);
    // otherwise every mark, the note, and tags go. Either is one undo step.
    function deleteScope(entry, noteOnly) {
        if (bible.busy || !entry) return false
        // Freeze identity before flushing drafts can publish a replacement model.
        const scope = Object.freeze({ versionId: entry.versionId, reference: entry.reference,
            book: entry.book, chapter: entry.chapter, start: entry.start,
            endBook: entry.endBook, endChapter: entry.endChapter, end: entry.end })
        if (!flushNote() || bible.busy) return false
        if (!savedScopeExists(scope)) { sounds.play("error"); return false }
        if (!(noteOnly ? bible.deleteNoteScope(scope) : bible.deleteAnnotationScope(scope))) return false
        syncNote()
        notify((scope.reference || "Passage") + (noteOnly ? " note deleted" : " deleted"), true)
        sounds.play("remove")
        return true
    }
    // Note and tag markers in every layout. One choice targets
    // that range; several overlapping ranges keep Open's chooser and Move here.
    function sameScope(a, b) {
        return !!a && !!b && ["versionId", "book", "chapter", "start", "endBook", "endChapter", "end"].every(function(key) { return a[key] === b[key] })
    }
    // Move here: a mark's note and tags move from their range to the verses
    // selected now (in the same version); a note already there comes first,
    // the moved one after a blank line. Highlights are paint: x paints and
    // erases them. u undoes the move.
    function moveHereEntry(choices, sceneX, sceneY) {
        const here = hasSelection ? bible.tagScope() : null
        const movable = here ? choices.filter(function(scope) { return scope.versionId === here.versionId && !sameScope(scope, here) }) : []
        return { id: "moveHere", text: here ? "Move here (" + here.reference + ")" + (movable.length > 1 ? "…" : "") : "Move here",
            enabled: !bible.busy && movable.length > 0,
            run: function() { app.onMarkerScope(movable, function(scope) { app.moveMark(scope) }, sceneX, sceneY) } }
    }
    function moveMark(scope) {
        if (bible.busy || !hasSelection || !flushNote()) return
        const snapshot = bible.annotationSnapshot(scope)
        if (!snapshot.ok) { rejected(snapshot.error); return }
        const components = { note: snapshot.note.length > 0, tags: Array.from(snapshot.tags).map(function(tag) { return tag.id }) }
        if (!components.note && !components.tags.length) { rejected("Only a highlight is there · x paints and erases highlights"); return }
        const destination = bible.tagScope()
        const preview = bible.previewRangeChange(snapshot, destination, components)
        if (!preview.ok) { rejected(preview.error); return }
        if (!bible.changeAnnotationRange(snapshot, destination, components, preview.needsNoteMerge, preview.destinationToken || "")) {
            rejected(bible.error.replace(/\.$/, "")); bible.clearError(); return
        }
        syncNote()
        notify((snapshot.scope.reference || "Mark") + " moved to " + destination.reference + (preview.needsNoteMerge ? " · notes combined" : ""), true)
        sounds.play("chime")
    }
    // An action on one of overlapping ranges: run at once for one range, else
    // a second menu at the same point asks which range.
    function onMarkerScope(choices, run, sceneX, sceneY) {
        if (choices.length === 1) { run(choices[0]); return }
        contextMenu.show("markerChoice", choices.map(function(scope, index) {
            return { id: "range:" + index, text: scope.reference || "Range " + (index + 1), enabled: !bible.busy, run: function() { run(scope) } }
        }), sceneX, sceneY)
    }
    function openMarkerMenu(choices, kind, openMarker, sceneX, sceneY) {
        choices = Array.from(choices || []).filter(function(scope) { return !!scope })
        const several = choices.length > 1
        const ready = !bible.busy
        const which = several ? "…" : ""
        const removeText = kind === "tag" ? "Remove tags" : "Delete note"
        contextMenu.show("marker", [
            { id: "open", text: "Open", enabled: ready, run: openMarker },
            menuDivider,
            moveHereEntry(choices, sceneX, sceneY),
            { id: "remove", text: removeText + which, enabled: ready && choices.length > 0,
                run: function() { app.onMarkerScope(choices, function(scope) { app.removeMarkerAnnotation(scope, kind) }, sceneX, sceneY) } },
            menuDivider,
            { id: "copy", text: "Copy passage" + which, enabled: !bible.busy && choices.length > 0,
                run: function() { app.onMarkerScope(choices, function(scope) { app.copyScope(scope) }, sceneX, sceneY) } },
            { id: "copyPlain", text: "Copy plain text" + which, enabled: !bible.busy && choices.length > 0,
                run: function() { app.onMarkerScope(choices, function(scope) { app.copyScope(scope, true) }, sceneX, sceneY) } },
            menuDivider,
            { id: "desk", text: several ? "Add to Desk…" : "Add to Desk",
                enabled: ready && choices.length > 0,
                run: function() { app.onMarkerScope(choices, function(scope) { app.addToDesk(scope) }, sceneX, sceneY) } }
        ], sceneX, sceneY)
    }
    function openSavedMenu(row, sceneX, sceneY) {
        const entry = collectionList.model[row]
        if (!entry) return
        collectionList.currentIndex = row
        collectionList.forceActiveFocus()
        const ready = !bible.busy
        const extras = [
            { id: "note", text: entry.note.length ? "Edit note" : "Write note", shortcut: KeyNotation.format("W"), enabled: ready, run: function() { app.editMarkNote(entry) } },
            { id: "tags", text: "Edit tags…", shortcut: KeyNotation.format("T"), enabled: ready, run: function() { app.openPassageTags(tagsUi.snapshot(entry)) } },
            moveHereEntry([entry]),
            { id: "delete", text: "Delete", shortcut: KeyNotation.format("Delete"), enabled: ready, run: function() {
                collectionList.currentIndex = row
                collectionList.forceActiveFocus()
                app.deleteSaved()
            } }].concat(undoMenuEntries())
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
        if (entry.recent) return historyRowMenu(entry.recent)
        if (entry.ribbon) return ribbonRowMenu()
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
            if (!forward && (paletteBook > 0 || paletteHistory)) backToBooks()
            else rejected()
            return
        }
        // A modal: back closes the top one (a step back where it has steps).
        if (dialogOpen) {
            if (forward) { rejected(); return }
            if (errorDialog.visible) errorDialog.close()
            else if (peekCard.visible) peekCard.close()
            else if (catalogDetails.visible) catalogDetails.close()
            else if (cleanDeskConfirmation.visible) cleanDeskConfirmation.close()
            else if (clearHistoryConfirmation.visible) clearHistoryConfirmation.close()
            else if (restoreConfirmation.visible) restoreConfirmation.close()
            else if (verseTagMenu.visible) verseTagMenu.close()
            else if (tagsUi.pickerVisible) tagsUi.handleEscape()
            else if (backupScope.visible) backupScope.close()
            else if (importGuide.visible) importGuide.close()
            else if (versionDialog.visible) { if (versionDialog.step > 0) backVersion(); else versionDialog.close() }
            else if (besideDialog.visible) besideDialog.close()
            else if (helpDialog.visible) helpDialog.close()
            else if (settingsDialog.visible) settingsDialog.close()
            else if (aboutDialog.visible) aboutDialog.close()
            else rejected()
            return
        }
        if (sidePanel.length) {
            if (forward) rejected()
            else if (leaveSidePanel("")) readerFocus()
            return
        }
        if (!readerKeys) { rejected(unavailableReason("historyBack")); return }
        clearCount()
        historyStep(forward ? 1 : -1)
    }
    AppContextMenu {
        id: contextMenu
        uiScale: app.scale
        objectName: "contextMenu"
        theme: desktop
        onRejected: sounds.play("error")
        onAboutToHide: menuGhost.release(contextMenu, contextMenu.origin)
    }
    PeekCard {
        id: peekCard
        objectName: "peekCard"
        theme: desktop
        uiScale: app.scale
        parts: app.peekParts()
        onKeyPressed: function(event) { app.peekKey(event) }
        onRowClicked: function(index) { app.compareRow = index }
        onAboutToHide: menuGhost.release(peekCard, peekCard.origin)
        onClosed: app.restorePanelFocus()
    }
    // Exit motion (interfaces.dev: exits subtler than entrances, from their
    // trigger; frequent popups open instantly). Menus and modals open at once
    // and close at once; a ghost of what closed fades and shrinks slightly
    // toward its trigger (a menu's pointer or anchor point) or centre (a
    // modal), never taking input. Disable animations: no ghost.
    ExitGhost {
        id: menuGhost
        objectName: "menuGhost"
        duration: app.reduceMotion ? 0 : 75
        endScale: 0.95
    }
    ExitGhost {
        id: modalGhost
        objectName: "modalGhost"
        duration: app.reduceMotion ? 0 : 85
        // Modals are large: a slight scale reads as the same motion as a menu's.
        endScale: 0.985
        dimmed: true
        dimColor: app.tint(desktop.background, 0.72)
        suppressDim: app.dialogOpen
    }
    function modalExitRelease(popup) { modalGhost.release(popup, null) }
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
    // The bar beside a block quote in Markdown previews (QuoteBars).
    readonly property color quoteBarColor: tint(desktop.muted, 0.55)
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
        // Opens at once; its exit is menuGhost's, toward where it opened.
        property point origin: Qt.point(0, 0)
        enter: null
        exit: null
        onAboutToHide: menuGhost.release(verseTagMenu, verseTagMenu.origin)
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
                text: modelData.reference + (verseTagMenu.annotationKind === "tag" ? " · " + app.tagsPlainText(modelData.tags) : " · Note")
                onTriggered: {
                    if (verseTagMenu.annotationKind === "note") app.openNoteScope(modelData)
                    else app.openPassageTags(modelData)
                }
                Keys.onPressed: function(event) { if (event.modifiers !== Qt.NoModifier) event.accepted = true }
                Keys.onReleased: function(event) { if (event.modifiers !== Qt.NoModifier) event.accepted = true }
                HoverHandler { cursorShape: Qt.PointingHandCursor }
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
    readonly property string noteUrlAtCursor: urlAt(noteEditor.text, noteEditor.cursorPosition)
    function urlAt(text, at) {
        const pattern = /https?:\/\/[^\s<>"'\]\[()]+/g
        let match
        while ((match = pattern.exec(text)) !== null) {
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
    // A Markdown preview's text: passage links, and a single Enter as a line
    // break (as GitHub comments and Obsidian take it). Qt keeps no trace of
    // where a quote starts or which blocks a hard break split, so zero-width
    // spaces mark them for MarkdownQuotes (QuoteBars), which reads and
    // removes them: one opening each quote's first line, one closing each
    // line a paragraph goes on from. Lines in lists, tables, and code keep
    // Markdown's rule (Qt would turn a break in a list item into an item).
    function previewMarkdown(text) {
        const lines = linkedMarkdown(text).split("\n")
        const fence = /^ {0,3}(```|~~~)/, listMarker = /^ {0,3}([-*+]|\d{1,9}[.)])( +|$)/
        const delimiterRow = /^\s*\|?\s*:?-+:?\s*(\|\s*:?-+:?\s*)*\|?\s*$/
        const content = function(line) { return line.replace(/^ {0,3}(?:> ?)*/, "") }
        const blank = function(line) { return !content(line).trim().length }
        let quoting = false, opening = false, fenced = false, listing = false
        for (let i = 0; i < lines.length; ++i) {
            const line = lines[i]
            if (fence.test(line)) { fenced = !fenced; continue }
            if (fenced) continue
            const marks = /^( {0,3}(?:> ?)+)/.exec(line), body = content(line)
            if (marks && !quoting) opening = true
            if (listMarker.test(body)) listing = true
            else if (!blank(line) && i > 0 && blank(lines[i - 1]) && !/^ {2,}/.test(body)) listing = false
            let out = line
            if (marks && opening && line.length > marks[1].length) {
                out = marks[1] + "\u200b" + line.slice(marks[1].length)
                opening = false
            }
            // A blank line ends a quote; other lines continue it (lazily).
            quoting = !!marks || (quoting && line.trim().length > 0)
            const next = lines[i + 1]
            if (next !== undefined && !listing && !blank(line) && !blank(next) && !fence.test(next) && !/^(\t| {4})/.test(body)
                    && body.indexOf("|") < 0 && !delimiterRow.test(content(next)) && !listMarker.test(content(next)))
                out = out.replace(/\s+$/, "") + "\u200b  "
            lines[i] = out
        }
        return lines.join("\n")
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
    // Keybindings → [[: types [[ at the editor's cursor, which opens the suggestions.
    function startNoteLink() {
        if (!notesOpen || !hasSelection) { rejected(); return }
        if (notePreview) notePreview = false
        noteEditor.forceActiveFocus()
        noteEditor.insert(noteEditor.cursorPosition, "[[")
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
        if (!flushNote()) return false
        const from = beginJump()
        if (!bible.navigate(reference)) return false
        endJump(from)
        syncNote()
        pulseDestination()
        return true
    }
    function openLinkedNote(row) {
        if (!row || !flushNote()) return false
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
        // The menu names the list's keys (y, shift Y, d): the list holds the keyboard.
        linkedList.forceActiveFocus()
        contextMenu.show("linked", RowMenu.entries(app, { busy: bible.busy, scope: row,
            open: function() { app.openLinkedNote(row) } }), sceneX, sceneY)
    }
    // Tab from the note editor: the Linked from list takes the keyboard.
    function focusLinkedFrom() {
        if (!linkedFrom.length) { rejected("No other notes link here"); return }
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
        else if (plain && (event.key === Qt.Key_G || event.key === Qt.Key_Home)) action = function() { app.moveLinked(-linkedList.currentIndex) }
        else if ((mods === Qt.ShiftModifier && event.key === Qt.Key_G) || (plain && event.key === Qt.Key_End))
            action = function() { app.moveLinked(app.linkedFrom.length - 1 - linkedList.currentIndex) }
        else if (plain && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) action = function() { if (row) app.openLinkedNote(row); else app.rejected() }
        else if (plain && event.key === Qt.Key_Y) action = function() { app.copyScope(row) }
        else if (mods === Qt.ShiftModifier && event.key === Qt.Key_Y) action = function() { app.copyScope(row, true) }
        else if (plain && event.key === Qt.Key_D) action = function() { app.addToDesk(row) }
        else if ((plain && (event.key === Qt.Key_Escape || event.key === Qt.Key_Tab)) || event.key === Qt.Key_Backtab
                || (mods === Qt.ShiftModifier && event.key === Qt.Key_Tab) || (mods === Qt.ControlModifier && event.key === Qt.Key_Up))
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
        if (delta === 0 || next < 0 || next >= linkedFrom.length) { sounds.play("boundary"); return }
        linkedList.currentIndex = next
        // Keep the current row in view in the Notes pane.
        const row = linkedRepeater.itemAt(next)
        const view = notesScroll.contentItem as Flickable
        if (!row || !view) return
        const top = row.mapToItem(view.contentItem, 0, 0).y
        if (top < view.contentY) view.contentY = Math.max(0, top - 8)
        else if (top + row.height > view.contentY + view.height) view.contentY = Math.min(view.contentHeight - view.height, top + row.height - view.height + 8)
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
    // A key or row that cannot act: the error sound, and a short toast saying
    // why when there is a reason a reader can act on.
    // "2.9 MB"; the bytes are a catalog snapshot, so approximate.
    function downloadSize(bytes) { return "≈" + (bytes / 1000000).toFixed(1) + " MB" }
    // What re-importing a version of an older text schema adds.
    // A version's text predates what it would show now: footnotes and poetry
    // indents (every version), or the parallel passages its source has (the
    // catalog says which, schema 6).
    function hasParallels(version) {
        const details = catalogEntry(version.id)
        return !!details && Array.from(details.features).indexOf("parallels") >= 0
    }
    function wantsReimport(version) { return !!version && (version.needsReimport || (version.textSchema > 0 && version.textSchema < 6 && hasParallels(version))) }
    function reimportGains(version) {
        const gains = version.textSchema < 4 ? ["footnotes", "cross references", "words of Jesus", "poetry indents"]
            : version.textSchema < 5 ? ["poetry indents"] : []
        if (version.textSchema < 6 && hasParallels(version)) gains.push("parallel passages")
        return gains.length > 1 ? gains.slice(0, -1).join(", ") + " and " + gains[gains.length - 1] : gains.join("")
    }
    function featureName(feature) {
        return ({ footnotes: "Footnotes", headings: "Section headings", wordsOfJesus: "Words of Jesus", apocrypha: "Apocrypha", parallels: "Parallel passages" })[feature] || feature
    }
    function rejected(reason) {
        if (reason) notify(reason, true)
        sounds.play("error")
    }
    // Why a catalog action cannot act right now ("" when it can, or when the
    // reason is only that the key belongs elsewhere).
    function unavailableReason(action) {
        if (typeof action === "string") action = actions.find(function(entry) { return entry.id === action })
        if (!action) return ""
        if (bible.busy) return "Wait for the import to finish"
        if (["highlight", "tags", "toggleDesk", "notes", "copy", "copyPlain", "compare", "peek"].includes(action.id) && !hasSelection)
            return "Select a verse first"
        if (["noteLinkSuggest", "openNoteLink", "notePreview", "noteKeepDraft", "noteUseSaved", "noteLinkedFrom"].includes(action.id) && !notesOpen && !deskOpen)
            return "Open a note first · " + keyText("notes")
        if (action.id === "noteLinkedFrom" && !linkedFrom.length) return "No other notes link here"
        if (action.id === "compare" && hasSelection && bible.versions.length < 2) return "Add another version to compare"
        if (action.id === "peek" && hasSelection) return "No footnotes on " + bible.passage + ":" + bible.cursorVerse
        if (action.id === "undo" && !bible.canUndo) return "Nothing to undo"
        if (action.id === "redo" && !bible.canRedo) return "Nothing to redo"
        if (action.id === "openBeside" && bible.ready && bible.versions.length < 2) return "Add another version to read beside"
        if (action.id === "switchPane" && bible.ready && !sidePanel.length) return "No side panel is open · " + keyText("annotations") + ", " + keyText("related") + ", " + keyText("desk") + ", or " + keyText("notes") + " opens one"
        if (action.id === "unfollow" && !following) return "No window follows this one · " + keyText("openBeside") + " opens a version beside it"
        if (action.id === "ribbon" && bible.ready && !bible.ribbon.versionId) return "No ribbon yet · " + keyText("placeRibbon") + " places it"
        if (action.id === "removeRibbon" && bible.ready && !bible.ribbon.versionId) return "No ribbon to remove"
        if (!bible.ready) return "Add a Bible version first"
        return ""
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
    // Saves every draft in this window: Marks' note in place, the Desk, and Notes'.
    function flushNote() {
        noteSaveTimer.stop()
        if (!flushMarkNote() || !flushDesk()) return false
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
    function readerFocus() { readerSurface.forceActiveFocus(); marginKeys = false }
    // A text drag puts the keyboard on the verse text it selects: the reader's.
    function textDragFocus(body) { body.forceActiveFocus(); marginKeys = false }
    // The next (or previous) item on the tab chain from `item`, or the error.
    function focusNext(item, forward) {
        const next = item.nextItemInFocusChain(forward)
        if (next && next !== item) next.forceActiveFocus(forward ? Qt.TabFocusReason : Qt.BacktabFocusReason)
        else rejected()
    }
    function focusSetup() { if (setupScroll.visible && !dialogOpen) setupImport.forceActiveFocus() }
    // A conflict appearing in Notes puts the keyboard on its choices, unless
    // the note editor has it: typing is never interrupted. There ctrl s (save
    // my draft) and ctrl shift S (use saved text) resolve it, as the hint
    // row and the buttons' tooltips say.
    onNoteConflictChanged: if (noteConflict) Qt.callLater(function() {
        if (!app.noteConflict || !app.notesOpen || app.dialogOpen || noteEditor.activeFocus) return
        if (noteRetry.enabled) noteRetry.forceActiveFocus(); else noteUseSaved.forceActiveFocus()
    })
    readonly property bool canKeepDraft: noteConflict && noteScopeMatches && hasSelection && !bible.busy
    function keepNoteDraft() { if (canKeepDraft) retryNote(); else rejected() }
    function useSavedNote() { if (noteConflict) loadSavedNote(); else rejected() }
    function focusWithin(container) {
        for (let item = activeFocusItem; item; item = item.parent) if (item === container) return true
        return false
    }
    function restorePanelFocus() {
        // Recheck after popup teardown: a dispatched action may have opened a
        // new modal already. Never steal its focus from a delayed close signal.
        Qt.callLater(function() {
            if (app.dialogOpen || bible.busy) return
            if (app.modalOpen || bible.busy) return
            if (notesDrawer.shown && app.marginKeys) {
                if (app.deskOpen) { if (app.deskPreview) deskPreviewText.forceActiveFocus(); else deskEditor.forceActiveFocus() }
                else if (app.marksOpen) { if (!app.focusWithin(marksPanel)) app.focusSavedList() }
                else if (app.relatedOpen) { if (!tagsUi.focusInBody()) tagsUi.focusResults() }
                else if (app.hasSelection && !app.notePreview) noteEditor.forceActiveFocus()
                else if (app.hasSelection) notePreviewText.forceActiveFocus()
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
    // Margins (Margins.js): chapter outline on the left, margin notes on the
    // right. One space computation; notes win and may shift the column left,
    // the outline only takes what is left and never moves it. Both are off in
    // focus mode; entering it, they stay laid out while they fade out
    // (marginsLeaving), as the column glides to its centred place.
    readonly property real readerSideInset: reading.width < 700 ? 24 : 48
    readonly property real readerRightInset: Math.max(readerSideInset, footerRightInset)
    // A comfortable measure: 66 zeros of the reader's text (CSS 66ch), so the
    // column follows the Text size; plus the verse-number gutter.
    readonly property real readerMeasure: {
        readerMetrics.font // A function call alone would not follow the font.
        return Math.round(TypeScale.measure * readerMetrics.advanceWidth("0"))
    }
    readonly property real readerColumnWidth: Math.min(readerMeasure + verseGutter + 9, reading.width - readerSideInset - readerRightInset)
    readonly property real readerNaturalX: Math.min((reading.width - readerColumnWidth) / 2, reading.width - readerRightInset - readerColumnWidth)
    property bool marginsLeaving: false
    // The split the leaving margins keep (focus mode's layout has no
    // scrollbar or footer room to reserve).
    property var leavingSpace: null
    readonly property bool marginsAllowed: bible.ready && (!focusLayout || marginsLeaving)
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
    // bible.chapterOutline as the margin shows it: toggling off lags by the
    // outline's exit (applyOutline), so it leaves before its room goes.
    property bool outlineOn: false // set from bible.chapterOutline on completion, then by applyOutline
    readonly property var outlineHeadings: !marginsAllowed || !outlineOn ? []
        : Array.from(bible.verses).filter(function(row) { return (row.heading || "").length > 0 })
            .map(function(row) { return { verse: row.number, text: row.heading } })
    // Connector: "}" (braceWidth) after the text's right edge, an arrow, and a
    // "{" before the note text.
    readonly property real marginBraceWidth: 9 * readerScale
    readonly property real marginTextRight: marginSpace.columnX + readerColumnWidth - 9
    // Previous/next chapter buttons at the reader's outer edges, vertically
    // centred in the viewport. The outline and margin notes keep clear of
    // them; they hide where they would cover the text (narrow windows), and in
    // focus mode until the pointer is over them (then, like the footer, their
    // clicks wait 300 ms). Mouse only: h and l are the keys.
    readonly property real chapterNavSize: 40 * controlScale
    readonly property real chapterNavEdge: 6 * controlScale
    readonly property real chapterNavReserve: chapterNavEdge + chapterNavSize + 6 * controlScale
    readonly property real chapterNavScrollbar: readerScrollbar.visible ? readerScrollbar.width : 0
    readonly property bool chapterNavFits: bible.ready && marginSpace.columnX >= chapterNavReserve
        && marginSpace.columnX + readerColumnWidth <= reading.width - chapterNavReserve - chapterNavScrollbar
    readonly property var chapterBounds: {
        const books = Array.from(bible.books)
        if (!books.length) return { first: true, last: true }
        const head = books[0], tail = books[books.length - 1]
        const firstChapters = Array.from(head.chapterNumbers || []), lastChapters = Array.from(tail.chapterNumbers || [])
        return { first: bible.book === head.id && bible.chapter === firstChapters[0],
                 last: bible.book === tail.id && bible.chapter === lastChapters[lastChapters.length - 1] }
    }
    // The chapters h / l open (across book boundaries), or null at the Bible's bounds.
    readonly property var chapterNeighbors: {
        const books = Array.from(bible.books)
        const at = books.findIndex(function(book) { return book.id === bible.book })
        if (at < 0) return { previous: null, next: null }
        function entry(book, chapter) { return { book: book.id, chapter: chapter, title: book.name + " " + chapter } }
        const chapters = Array.from(books[at].chapterNumbers || [])
        const index = chapters.indexOf(bible.chapter)
        let previous = null, next = null
        if (index > 0) previous = entry(books[at], chapters[index - 1])
        else if (at > 0) { const before = Array.from(books[at - 1].chapterNumbers || []); if (before.length) previous = entry(books[at - 1], before[before.length - 1]) }
        if (index >= 0 && index < chapters.length - 1) next = entry(books[at], chapters[index + 1])
        else if (at < books.length - 1) { const after = Array.from(books[at + 1].chapterNumbers || []); if (after.length) next = entry(books[at + 1], after[0]) }
        return { previous: previous, next: next }
    }
    // While margin notes are on, their room is kept in every chapter, notes
    // or not, and the scrollbar's width is reserved whether it shows or not:
    // the column stays put from chapter to chapter.
    readonly property var marginSpace: marginsLeaving && leavingSpace ? leavingSpace : Margins.space({
        width: reading.width, columnX: readerNaturalX, columnWidth: readerColumnWidth,
        minLeft: readerSideInset, leftInset: Math.max(24 * readerScale, chapterNavReserve),
        rightInset: Math.max(28 * readerScale, footerRightInset, marginRightInset),
        wantNotes: marginsAllowed && marginNotesOn && !(marginOpen && notesDocked), connectorWidth: marginConnectorWidth,
        notesMin: 170 * readerScale, notesPreferred: marginNotesPreferred, notesMax: 330 * readerScale,
        wantOutline: marginsAllowed && outlineOn, outlineGap: 12 * readerScale,
        outlineMin: 120 * readerScale, outlineMax: 220 * readerScale
    })
    readonly property real marginConnectorWidth: 3 * readerScale + 2 * marginBraceWidth + 34 * readerScale
    readonly property real marginNotesPreferred: 250 * readerScale
    readonly property real marginRightInset: chapterNavReserve + readerScrollbar.width
    // The outline's room is kept while it is on, headings or not (as margin
    // notes' is): it draws only in chapters with headings.
    readonly property bool outlineShown: marginSpace.outline.visible && outlineHeadings.length > 0
    // While margin notes show for this chapter, the reader drops its inline
    // note marks (Verse rails, Paragraph/Block brackets); other kinds stay.
    readonly property bool inlineNoteMarks: !marginSpace.notes.visible
    readonly property real marginNotePixelSize: TypeScale.size(readerBodySize, TypeScale.note)
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
    }
    // Another chapter (or version) ends margin motion at once.
    readonly property string marginChapterKey: bible.versionId + ":" + bible.book + ":" + bible.chapter
    onMarginChapterKeyChanged: settleMargins()
    // An actual toggle only: every preference shares one notify signal.
    readonly property bool observedMarginNotes: bible.marginNotes
    onObservedMarginNotesChanged: applyMarginNotes(observedMarginNotes)
    readonly property bool observedOutline: bible.chapterOutline
    onObservedOutlineChanged: applyOutline(observedOutline)
    // Toggling margin notes slides the reader to make or return room, and fades
    // the notes in/out; resizes, chapter changes, range editing, and Disable
    // animations jump (settleMargins).
    readonly property int marginSlideMs: reduceMotion ? 0 : 100
    property real marginSlideOffset: 0
    // Where the column rests: the margins' split, or centred while margins
    // that focus mode turned off still fade out.
    readonly property real columnTargetX: marginsLeaving ? readerNaturalX : marginSpace.columnX
    // Margins shown when a motion starts move with the column, never apart
    // from their text; hidden ones fade in where they settle.
    property bool marginsAttached: false
    readonly property real marginDrift: marginsAttached ? columnTargetX + marginSlideOffset - marginSpace.columnX : 0
    // How far the column's text reaches past the margins' edge for it: a
    // column still gliding into place beside margins that wait there.
    readonly property real marginCover: Math.max(0, columnTargetX + marginSlideOffset - marginDrift - marginSpace.columnX)
    // Margin opacity: the notes' own toggle times focus mode's fade of both margins.
    property real marginNotesLevel: 1
    property real marginFocusLevel: 1
    // The outline's own toggle: it slides in from the left as it fades in,
    // and leaves the same way, quicker (as a side panel does).
    property real outlineLevel: 1
    NumberAnimation { id: outlineFade; objectName: "outlineFade"; target: app; property: "outlineLevel"
        onFinished: if (app.outlineLevel === 0) app.slideMarginsTo(app.marginNotesOn, false) }
    NumberAnimation { id: marginSlide; objectName: "marginSlide"; target: app; property: "marginSlideOffset"; to: 0; duration: app.marginSlideMs; easing.type: Easing.OutCubic
        onFinished: if (!app.marginsLeaving) app.marginsAttached = false }
    NumberAnimation { id: marginFade; target: app; property: "marginNotesLevel"; duration: app.marginSlideMs; easing.type: Easing.OutCubic
        onFinished: if (app.marginNotesLevel === 0) app.slideMarginsTo(false) }
    // Focus mode: the margins' exit (quick and subtle, like a panel's) and entrance.
    NumberAnimation { id: marginExit; objectName: "marginExit"; target: app; property: "marginFocusLevel"; to: 0; duration: app.panelExitMs; easing.type: Easing.InQuad
        onFinished: { app.marginsLeaving = false; app.marginsAttached = false } }
    NumberAnimation { id: marginEnter; objectName: "marginEnter"; target: app; property: "marginFocusLevel"; to: 1; duration: app.marginSlideMs; easing.type: Easing.OutCubic }
    // Focus mode drops the footer's room, re-centring the chapter buttons:
    // they glide there with the column.
    property real chapterNavShift: 0
    NumberAnimation { id: chapterNavGlide; objectName: "chapterNavGlide"; target: app; property: "chapterNavShift"; to: 0; duration: app.marginSlideMs; easing.type: Easing.OutCubic }
    // Margin notes (and the outline, when given) on or off, the column gliding
    // from where it was to its new place.
    function slideMarginsTo(on, outline) {
        const before = readerColumn.x
        marginNotesOn = on
        if (outline !== undefined) outlineOn = outline
        marginSlide.stop()
        marginSlideOffset = reduceMotion ? 0 : before - columnTargetX
        if (marginSlideOffset !== 0) marginSlide.start()
    }
    function applyMarginNotes(on) {
        marginFade.stop()
        if (on === marginNotesOn && !(on && marginNotesLevel < 1)) return
        if (reduceMotion || !marginSpace.notes.visible && !on) {
            marginNotesLevel = 1; slideMarginsTo(on); return
        }
        if (on) {
            if (!marginNotesOn) marginNotesLevel = 0
            slideMarginsTo(true)
            marginFade.to = 1; marginFade.start()
        } else { marginFade.to = 0; marginFade.start() }
    }
    function applyOutline(on) {
        outlineFade.stop()
        if (on === outlineOn && !(on && outlineLevel < 1)) return
        if (reduceMotion || !marginSpace.outline.visible && !on) {
            outlineLevel = 1; slideMarginsTo(marginNotesOn, on); return
        }
        if (on && !outlineOn) { outlineLevel = 0; slideMarginsTo(marginNotesOn, true) }
        outlineFade.to = on ? 1 : 0
        outlineFade.duration = on ? panelEnterMs : panelExitMs
        outlineFade.easing.type = on ? Easing.OutCubic : Easing.InCubic
        outlineFade.start()
    }
    // Margin motion ends at once: resizes, chapter changes, range editing.
    function settleMargins() {
        if (marginFade.running) { marginFade.stop(); marginNotesLevel = marginFade.to; if (!marginNotesLevel) marginNotesOn = false }
        if (outlineFade.running) { outlineFade.stop(); outlineLevel = outlineFade.to; if (!outlineLevel) outlineOn = false }
        if (marginExit.running) { marginExit.stop(); marginFocusLevel = 0; marginsLeaving = false }
        if (marginEnter.running) { marginEnter.stop(); marginFocusLevel = 1 }
        marginSlide.stop(); marginSlideOffset = 0; marginsAttached = false
        chapterNavGlide.stop(); chapterNavShift = 0
    }
    Connections {
        target: reading
        function onHeightChanged() { app.scheduleMargins() }
    }
    function scheduleMargins() {
        if (readerInitialized) marginTimer.restart()
        if (charMode) charReapplyTimer.restart()
    }
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
    // measureVerses for one verse (null when it is not laid out).
    function measureVerse(number) {
        const block = blockLoader.item as BlockReader
        if (block) {
            const extent = block.verseExtent(number)
            const offset = extent ? block.body.mapToItem(readerColumn, 0, 0).y : 0
            return extent ? { number: number, top: offset + extent.top, bottom: offset + extent.bottom } : null
        }
        const item = verseRepeater.itemAt(verseOrder(number)) as VerseItem
        return item ? { number: number, top: item.y + item.textOffset, bottom: item.y + item.textOffset + item.textHeight } : null
    }
    // Margins measure only while one is shown; `{`/`}` measure on demand.
    readonly property bool marginsMeasured: (marginSpace.notes.visible && marginNoteEntries.length > 0) || outlineShown
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
        const layout = placed.map(function(place, i) {
            const note = { top: place.y, bottom: place.y + place.height }
            const passage = braces[i]
            return Object.assign({ key: keys[i].key, reference: keys[i].reference, passageTop: passage.top, passageBottom: passage.bottom,
                naturalLines: items[i].lines, tips: Margins.tips(note, passage, { inset: 3 * readerScale, noteLines: place.lines, passageLine: readerLineHeight }) }, place)
        })
        // Unchanged placements keep their connectors' paint.
        if (JSON.stringify(layout) !== JSON.stringify(marginLayout)) marginLayout = layout
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
    function openMarginNoteMenu(entry, sceneX, sceneY) {
        if (!entry || modalOpen || bible.busy) return
        openMarkerMenu([entry.scope], "note", function() { app.openMarginNote(entry) }, sceneX, sceneY)
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
        if (!bible.ready || !flushNote()) return
        const jump = beginJump()
        selectVerse(verse, false)
        if (bible.cursorVerse !== verse) return
        endJump(jump)
        Qt.callLater(function() { app.scrollToVerse(verse, true) })
        pulseDestination()
    }
    property bool toastInFocus: false
    function notify(message, inFocus) { toastInFocus = !!inFocus; toast.text = message; toastTimer.restart() }
    // The first sound effect anyone hears says how to turn them off, after the
    // message of the action that made it (most notify just before they play).
    Connections {
        target: sounds
        function onIntroduced() {
            if (!app.active) return
            const introduction = "Sound effects · " + app.keyText("soundEffects") + " turns them off"
            app.notify(toastTimer.running && toast.text.length ? toast.text + "\n" + introduction : introduction, true)
        }
    }
    // See also for one verse (a footnote's, from Peek), selecting it first.
    function seeAlsoForVerse(verse) {
        if (!bible.ready || bible.busy || modalOpen) return
        if (bible.selectedStart !== verse || bible.selectedEnd !== verse || bible.crossChapterSelection) selectVerse(verse, false)
        if (bible.cursorVerse !== verse) return
        if (sidePanel === "related" || leaveSidePanel("related")) tagsUi.openRelated()
    }
    // Peek (PeekCard): a footnote read where it is (a marker's click, or i on
    // the cursor verse; ↑ ↓ step through the chapter's notes in the card),
    // or the selection in every version (c, Compare).
    property string peekKind: "footnote"
    property int peekIndex: -1
    readonly property var peekNote: peekIndex >= 0 ? bible.footnotes[peekIndex] || null : null
    function peekFootnote(verse, seq, sceneX, sceneY, lineHeight) {
        if (!bible.ready || bible.busy || modalOpen) return
        const index = Array.from(bible.footnotes).findIndex(function(note) { return note.verse === verse && note.seq === seq })
        if (index < 0) { rejected(); return }
        const point = sceneX === undefined ? verseMenuPoint(verse) : Qt.point(sceneX, sceneY)
        peekKind = "footnote"
        peekCard.rows = []
        showPeekNote(index)
        peekCard.show(point.x, point.y, lineHeight || 0)
    }
    function peekVerseFootnotes() {
        const note = Array.from(bible.footnotes).find(function(note) { return note.verse === bible.cursorVerse })
        if (!note) { rejected(unavailableReason("peek")); return }
        peekFootnote(note.verse, note.seq)
    }
    function showPeekNote(index) {
        const notes = Array.from(bible.footnotes)
        peekIndex = index
        const note = notes[index]
        peekCard.title = (note.kind === "fe" ? "Endnote " : "Footnote ") + footnoteMark(note.caller) + " · " + bible.passage + ":" + note.verse
            + (notes.length > 1 ? " · " + (index + 1) + " of " + notes.length : "")
        peekCard.body = note.text
    }
    function peekParts() {
        const close = ["Escape", "close", function() { peekCard.close() }]
        if (peekKind === "compare") {
            const row = compareRows[compareRow]
            const ready = peekCard.opened && !!row && !bible.busy
            return [[["J", "K"], "versions"], ["Return", "switch", app.switchToCompared, ready && row.available && !row.active],
                ["Y", "copy", function() { app.copyCompared(false) }, ready && row.available],
                ["O", "beside", app.compareBeside, ready && !row.active], close]
        }
        const ready = peekCard.opened && !!peekNote && !bible.busy
        return [["Return", "see also", app.seeAlsoFromPeek, ready], ["Y", "copy", app.copyPeek, ready],
            bible.footnotes.length > 1 ? [["Up", "Down"], "notes"] : null, close]
    }
    // Compare (c): the selection's text in every installed version, in a peek
    // under it; j k choose a version, y copies it (shift Y plain), enter
    // switches to it at the same passage.
    property var compareRows: []
    property int compareRow: 0
    onCompareRowChanged: peekCard.currentRow = compareRow
    function openCompareCard() {
        if (!actionAvailable("compare")) { rejected(unavailableReason("compare")); return }
        compareRows = Array.from(bible.compareVersions(bible.tagScope()))
        if (compareRows.length < 2) { rejected(unavailableReason("compare")); return }
        const first = compareRows.findIndex(function(row) { return !row.active && row.available })
        compareRow = first >= 0 ? first : 0
        peekKind = "compare"
        peekCard.title = "Compare · " + bible.selectionReference + " · " + compareRows.length + " versions"
        peekCard.body = ""
        peekCard.rows = compareRows.map(function(row) {
            return { title: row.abbreviation + (row.active ? " · reading" : "") + (row.available && row.reference !== bible.selectionReference ? " · " + row.reference : ""),
                text: row.available ? row.text : "Not in this version", muted: !row.available }
        })
        peekCard.currentRow = compareRow
        const point = verseMenuPoint(bible.selectedEnd || bible.cursorVerse)
        peekCard.show(point.x, point.y, 0)
    }
    function stepCompared(delta) {
        const next = compareRow + delta
        if (next < 0 || next >= compareRows.length) { sounds.play("boundary"); return }
        compareRow = next
    }
    function copyCompared(plainText) {
        const row = compareRows[compareRow]
        if (!row || !row.available) { rejected(row ? "Not in " + row.abbreviation : ""); return }
        const scope = Object.assign({}, bible.tagScope(), { versionId: row.versionId })
        if (!bible.copyScope(scope, !!plainText).length) { rejected(); return }
        peekCard.close()
        notify(row.reference + " · " + row.abbreviation + (plainText ? " copied as plain text" : " copied"), true)
        sounds.play("chime")
    }
    // o: the highlighted version in a window beside this one, following it.
    function compareBeside() {
        const row = compareRows[compareRow]
        if (!row || row.active) { rejected(row ? "Already reading " + row.abbreviation + " here" : ""); return }
        peekCard.close()
        openBeside(row.versionId)
    }
    function switchToCompared() {
        const row = compareRows[compareRow]
        if (!row || !row.available) { rejected(row ? "Not in " + row.abbreviation : ""); return }
        if (row.active) { rejected("Already reading " + row.abbreviation); return }
        peekCard.close()
        switchVersion(row.versionId)
    }
    function stepPeek(delta) {
        const next = peekIndex + delta
        if (next < 0 || next >= bible.footnotes.length) { sounds.play("boundary"); return }
        showPeekNote(next)
    }
    function seeAlsoFromPeek() {
        const note = peekNote
        if (!note) return
        peekCard.close()
        seeAlsoForVerse(note.verse)
    }
    function copyPeek() {
        const note = peekNote
        if (!note) return
        peekCard.close()
        copyText(note.text, "Footnote " + footnoteMark(note.caller))
    }
    function peekKey(event) {
        if ((event.modifiers & ~Qt.KeypadModifier) & ~Qt.ShiftModifier) return
        const plain = (event.modifiers & ~Qt.KeypadModifier) === Qt.NoModifier
        if (peekKind === "compare") {
            if (plain && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) switchToCompared()
            else if (event.key === Qt.Key_Y) copyCompared(!!(event.modifiers & Qt.ShiftModifier))
            else if (plain && event.key === Qt.Key_O) compareBeside()
            else if (plain && (event.key === Qt.Key_Down || event.key === Qt.Key_J)) stepCompared(1)
            else if (plain && (event.key === Qt.Key_Up || event.key === Qt.Key_K)) stepCompared(-1)
            else if (event.key !== Qt.Key_Escape) { rejected("Not a key in Compare · " + KeyNotation.format("Escape") + " closes it") }
            else return
            event.accepted = true
            return
        }
        if (plain && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) seeAlsoFromPeek()
        else if (event.key === Qt.Key_Y) copyPeek()
        else if (plain && (event.key === Qt.Key_Down || event.key === Qt.Key_J)) stepPeek(1)
        else if (plain && (event.key === Qt.Key_Up || event.key === Qt.Key_K)) stepPeek(-1)
        else if (event.key !== Qt.Key_Escape) { rejected("Not a key in the peek · " + KeyNotation.format("Escape") + " closes it") }
        else return
        event.accepted = true
    }
    // Footnote and inline annotation markers take clicks a little beyond their
    // drawn cell: a larger, invisible target that never moves the text.
    readonly property real markerHitMargin: 5 * readerScale
    // Footnote markers stay out of the tab order; their tooltips name the key.
    readonly property string footnoteHint: "Click or " + keyText("peek") + ": peek"
    function footnoteMark(caller) { return /^[A-Za-z0-9]{1,3}$/.test(caller || "") ? caller : "\u2020" }
    // Verse rows reserve footnote cells like BlockDocument: a word joiner and a
    // transparent image at each note's plain-text offset. Paint-only markers
    // sit over them; copy strips the cells, search maps around them.
    readonly property real footnoteCellWidth: Math.max(1, Math.round(readerSize * readerScale * 0.5))
    function footnoteHtml(html, notes) {
        if (!notes || !notes.length || focusLayout) return html
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
        holdRepeated = false
        holdTimer.stop()
        // The hold window opens once the step's own deferred work (a chapter's
        // layout) has run, so that stall never counts as holding the key.
        Qt.callLater(app.openHoldWindow)
    }
    function openHoldWindow() {
        if (!holdAction || keyHold.press(holdKey) !== holdPress) return
        holdStartedAt = holdTickAt = Date.now()
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
        const from = beginJump()
        selectVerse(number, false)
        endJump(from)
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
        if (current >= 0 && target !== current) bible.openChapter(chapters[target].book, chapters[target].chapter)
        readerFocus()
    }
    function jump(reference) { goToReference(reference, false) }
    // firstVerse: a chapter (no verse given) opens on its first verse, as the
    // palette does: "Galatians 2" selects Galatians 2:1.
    function goToReference(reference, firstVerse) {
        if (!flushNote()) return
        const from = beginJump()
        if (bible.navigate(reference)) {
            if (firstVerse) selectFirstVerse()
            endJump(from)
            passagePalette.navigated = true
            passagePalette.close()
            readerFocus()
            pulseDestination()
        }
    }
    // The palette's chapters (its grid) open on their first verse too.
    function openChapter(book, chapter) {
        if (!flushNote()) return
        const from = beginJump()
        bible.openChapter(book, chapter)
        positionTimer.stop()
        setReaderY(0)
        restoringPosition = false
        savePosition()
        selectFirstVerse()
        endJump(from)
        passagePalette.navigated = true
        passagePalette.close()
        readerFocus()
    }
    function selectFirstVerse() {
        if (hasSelection || !bible.ready || !bible.verses.length) return
        const first = bible.verses[0].number
        selectionAnchor = first
        bible.selectVerses(first, first)
        syncNote()
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
            const saved = paletteSnapshot
            paletteBookName = paletteSnapshot.name
            paletteBook = paletteSnapshot.book
            paletteHistory = !!paletteSnapshot.history && bible.historyEnabled
            referenceInput.text = paletteSnapshot.query
            paletteArrowUsed = paletteSnapshot.arrow
            paletteList.currentIndex = paletteSnapshot.row
            paletteList.positionViewAtIndex(paletteSnapshot.row, ListView.Contain)
            if (paletteTextMode) {
                pendingSearchResume = saved
                Qt.callLater(app.restoreSearchPalette)
            }
        }
        paletteCleared = false
        passagePalette.open()
        referenceInput.forceActiveFocus()
    }
    // The Passage palette at the reader's book, its chapter grid with the
    // current one chosen (clicking the chapter title).
    function openChapterList() {
        if (!bible.ready || bible.busy) { rejected(unavailableReason("books")); return }
        const book = Array.from(bible.books).find(function(b) { return b.id === bible.book })
        if (!book) { rejected(); return }
        openPalette(false)
        if (!passagePalette.visible) return
        paletteBookName = book.name
        paletteBook = book.id
        referenceInput.text = ""
        paletteArrowUsed = true
        const row = Math.max(0, Array.from(book.chapterNumbers).indexOf(bible.chapter))
        paletteChapters.currentIndex = row
        paletteChapters.positionViewAtIndex(row, GridView.Contain)
        referenceInput.forceActiveFocus()
    }
    function rememberPalette() {
        paletteSnapshot = { query: referenceInput.text, book: paletteBook, history: paletteHistory,
            name: paletteBookName, row: paletteList.currentIndex, arrow: paletteArrowUsed,
            position: paletteList.contentY, generation: bible.textGeneration }
    }
    // One esc closes the palette in any mode; space resumes it, query and all.
    function escapePalette() { passagePalette.close() }
    function paletteMode() { return referenceInput.text.trim().startsWith(">") ? "commands" : paletteTextMode ? "search" : paletteHistory ? "history" : "passage" }
    // A different mode's key switches the open palette; its own key does nothing.
    function showPaletteMode(mode) {
        if (passagePalette.visible && paletteMode() === mode) return
        if (mode === "search") openTextSearch()
        else openPalette(mode === "commands")
    }
    function openNotes(focusEditor) {
        if (!bible.ready || bible.busy) return
        clearCount()
        if (!notesOpen) openedNoteActionScope = actionScope()
        // From another panel, the Margin stays open and turns to the note.
        const turning = marginOpen && !notesOpen
        marginMode = "notes"
        notesDrawer.focusEditor = focusEditor
        notesDrawer.open()
        if (loadedNoteReference !== bible.selectionReference) syncNote()
        if (focusEditor && hasSelection) {
            notePreview = false
            if (notesDrawer.shown) focusNoteEnd()
        } else if (focusEditor && turning) notesPanel.forceActiveFocus()
    }
    function focusNoteEnd() { noteEditor.forceActiveFocus(); noteEditor.cursorPosition = noteEditor.length }
    // Closes the Notes drawer with the keyboard already outside it: a drawer
    // that closes with focus hands it back when its exit ends, over whatever
    // opened meanwhile (a palette typed into during the exit).
    function dismissNotesDrawer() {
        if (notesDrawer.visible && focusWithin(notesDrawer.contentItem.parent)) readerFocus()
        notesDrawer.close()
    }
    function closeNotes() {
        if (!flushNote()) return false
        dismissNotesDrawer()
        editingPanelClosed()
        return true
    }
    // esc in a side panel closes it, its drafts saved (leaveSidePanel), and
    // the keyboard returns to the reader.
    function closePanel() { if (leaveSidePanel("")) readerFocus() }
    // ctrl w (F6): the keyboard to the open side panel, or back to the
    // reader, the panel staying open (Notes follows the selection there).
    function switchPane() {
        if (!sidePanel.length) { rejected(unavailableReason("switchPane")); return }
        if (marginKeys) {
            if (!flushNote() || (markEditKey.length && !finishMarkNote(false))) return
            readerFocus()
        } else {
            marginKeys = true
            restorePanelFocus()
        }
    }
    // While the reader has the keyboard, a side panel's hints are this one;
    // while the panel has it, its hints end with the way back and esc.
    readonly property var panelAwayHints: [[actionKeys("switchPane")[0], "keys here", switchPane]]
    readonly property var panelReturnHint: [actionKeys("switchPane")[0], "to the text", switchPane]
    // The current row of a side panel's list while the reader has the
    // keyboard: grey, as an inactive selection (the accent tint while it has it).
    readonly property color inactiveRowColor: tint(desktop.muted, 0.16)
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
            notify("Backup restored", true)
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
    function highlightPassage() {
        if (!hasSelection || !flushNote()) return
        const scope = actionScope()
        if (bible.toggleHighlight()) {
            const added = bible.selectionHighlighted
            completeRangeAction(scope, false)
            sounds.play(added ? "chime" : "remove")
        } else sounds.play("error")
    }
    // This window's last change to marks, notes, or tags, wherever it is; the
    // toast names it ("Highlight on John 3:16 undone"). A change made since in
    // another window stops it, with a toast saying so.
    function undoChange(undo) {
        const label = undo ? bible.undoLabel : bible.redoLabel
        if (!(undo ? bible.undo() : bible.redo())) {
            notify(bible.error.replace(/\.$/, ""), true)
            bible.clearError()
            return
        }
        syncNote()
        notify(label.charAt(0).toUpperCase() + label.slice(1) + (undo ? " undone" : " redone"), true)
        sounds.play("tick")
    }
    // The Desk: one Markdown page shared by every window (bible.deskPage),
    // shown in the Margin in place of the note. d adds a passage's
    // quotation at its end (u takes it off again); the page is written like
    // a note: it saves after a pause and before anything else happens
    // (flushNote), only over the text it started from, and a change from
    // another window under a draft keeps the draft and offers both.
    property bool deskPreview: false
    property string loadedDeskText: ""
    property bool deskConflict: false
    property bool syncingDesk: false
    readonly property var deskLinks: deskOpen ? Array.from(bible.passageLinks(deskEditor.text)) : []
    readonly property var deskLinkAtCursor: {
        const at = deskEditor.cursorPosition
        return deskLinks.find(function(link) { return at >= link.start && at <= link.start + link.length }) || null
    }
    readonly property string deskUrlAtCursor: deskOpen ? urlAt(deskEditor.text, deskEditor.cursorPosition) : ""
    Timer { id: deskSaveTimer; interval: app.noteSaveDelay; onTriggered: app.flushDesk() }
    function openDesk() {
        if (!bible.ready || bible.busy) return
        clearCount()
        if (deskEditor.text === loadedDeskText) loadSavedDesk()
        const shown = notesDrawer.shown
        marginMode = "desk"
        notesDrawer.focusEditor = true
        notesDrawer.open()
        if (shown) focusDesk()
    }
    // Marks in the Margin: opened afresh (not turned to from itself), it
    // starts unfiltered at the first row here.
    function openMarks() {
        if (!bible.ready) return
        clearCount()
        const shown = notesDrawer.shown, fresh = !marksOpen
        marginMode = "marks"
        notesDrawer.focusEditor = true
        if (fresh) {
            clearSavedQuery()
            refreshMarks()
            collectionList.currentIndex = initialSavedRow()
        }
        notesDrawer.open()
        if (shown) focusSavedList()
    }
    // See also in the Margin (TagsPanel roots it first).
    function openRelatedPanel() {
        const shown = notesDrawer.shown
        marginMode = "related"
        notesDrawer.focusEditor = true
        notesDrawer.open()
        if (shown) tagsUi.focusResults()
    }
    function focusDesk() {
        if (deskPreview) deskPreviewText.forceActiveFocus()
        else { deskEditor.forceActiveFocus(); deskEditor.cursorPosition = deskEditor.length }
    }
    function loadSavedDesk() {
        syncingDesk = true
        loadedDeskText = bible.deskPage
        if (deskEditor.text !== bible.deskPage) deskEditor.text = bible.deskPage
        deskConflict = false
        syncingDesk = false
    }
    // A new page (another window's, or this one's own save) replaces the
    // editor's text unless it holds a draft; a draft the page changed under
    // is a conflict.
    function syncDesk() {
        if (deskEditor.text === loadedDeskText) loadSavedDesk()
        else if (deskEditor.text === bible.deskPage) { loadedDeskText = bible.deskPage; deskConflict = false }
        else if (bible.deskPage !== loadedDeskText) deskConflict = true
    }
    function flushDesk() {
        deskSaveTimer.stop()
        if (deskEditor.text === loadedDeskText) return true
        if (deskConflict) return false
        if (bible.setDeskPage(deskEditor.text, loadedDeskText)) { loadedDeskText = deskEditor.text; return true }
        syncDesk()
        return false
    }
    readonly property bool canKeepDeskDraft: deskConflict && !bible.busy
    function keepDeskDraft() {
        if (!canKeepDeskDraft) { rejected(); return }
        loadedDeskText = bible.deskPage
        deskConflict = false
        flushDesk()
    }
    function useSavedDesk() { if (deskConflict) loadSavedDesk(); else rejected() }
    function toggleDeskPreview() {
        if (!deskOpen) { rejected(); return }
        if (deskPreview) {
            deskPreview = false
            deskEditor.forceActiveFocus()
        } else if (flushDesk()) {
            deskPreview = true
            deskPreviewText.forceActiveFocus()
        }
    }
    // alt enter in the Desk: a passage link opens in the reader, a web address in the browser.
    function openDeskLinkAtCursor() {
        if (deskLinkAtCursor) return followPassageLink(deskLinkAtCursor.reference)
        if (deskUrlAtCursor.length) { desktop.openExternal(deskUrlAtCursor); return true }
        sounds.play("boundary")
        return false
    }
    // The Desk editor's own keys: tab types a tab and shift tab takes one
    // back (the editor keeps the keyboard), alt enter follows the link at the
    // cursor, ctrl s and ctrl shift S settle another window's change.
    function deskKey(event) {
        const enter = event.key === Qt.Key_Return || event.key === Qt.Key_Enter
        if (event.key === Qt.Key_Tab && event.modifiers === Qt.NoModifier) {
            deskEditor.remove(deskEditor.selectionStart, deskEditor.selectionEnd)
            deskEditor.insert(deskEditor.cursorPosition, "\t")
        } else if (event.key === Qt.Key_Backtab || (event.key === Qt.Key_Tab && event.modifiers === Qt.ShiftModifier)) {
            const at = deskEditor.cursorPosition
            if (at > 0 && deskEditor.getText(at - 1, at) === "\t") deskEditor.remove(at - 1, at)
        } else if (event.key === Qt.Key_S && (event.modifiers === Qt.ControlModifier || event.modifiers === (Qt.ControlModifier | Qt.ShiftModifier))) {
            if (event.modifiers & Qt.ShiftModifier) useSavedDesk(); else keepDeskDraft()
        } else if (event.key === Qt.Key_D && event.modifiers === (Qt.ControlModifier | Qt.ShiftModifier)) {
            requestCleanDesk()
        } else if (enter && event.modifiers === Qt.AltModifier) openDeskLinkAtCursor()
        else return
        event.accepted = true
    }
    // In Write mode, while the keyboard is in the Margin, the page keeps it.
    function keepDeskEditorFocus() {
        if (!deskOpen || deskPreview || !marginKeys || dialogOpen || modalOpen || bible.busy || deskEditor.activeFocus) return
        const item = activeFocusItem
        if (!item || item === notesDrawer.contentItem.parent || focusWithin(notesDrawer.contentItem)) deskEditor.forceActiveFocus()
    }
    // Adds a passage's quotation to the Desk, in every window; an open Desk
    // shows its end.
    function addToDesk(scope) {
        if (bible.busy || !scope) { rejected(unavailableReason("toggleDesk")); return false }
        if (!flushNote()) return false
        const reference = bible.appendToDesk(scope)
        if (!reference.length) { sounds.play("error"); return false }
        notify(reference + " added to the Desk", true)
        sounds.play("chime")
        if (deskOpen) Qt.callLater(function() {
            if (!deskEditor.activeFocus) deskEditor.cursorPosition = deskEditor.length
            const view = (app.deskPreview ? deskPreviewScroll : deskEditorScroll).contentItem as Flickable
            view.contentY = Math.max(0, view.contentHeight - view.height)
        })
        return true
    }
    // Clean Desk empties the page in every window, after confirming (u undoes it).
    function requestCleanDesk() {
        if (bible.busy || !flushNote()) return
        if (!bible.deskPage.length) { notify("The Desk is already clean", true); return }
        cleanDeskConfirmation.open()
    }
    function confirmCleanDesk() {
        if (bible.busy || !flushNote()) return
        if (!bible.cleanDesk()) return
        cleanDeskConfirmation.close()
        sounds.play("remove")
        notify("Cleaned the Desk · " + keyText("undo") + " brings it back", true)
    }
    function addPassageToDesk() {
        if (!hasSelection) { rejected(unavailableReason("toggleDesk")); return }
        const scope = actionScope()
        if (addToDesk(bible.tagScope())) completeRangeAction(scope, false)
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
        if (target < 0 || target >= jumpStack.length || target === jumpIndex) return false
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
    function historyEntry(id) { return Array.from(bible.history).find(function(entry) { return entry.id === id }) }
    function openHistoryItem(id) {
        if (bible.busy) return
        const entry = historyEntry(id)
        if (!entry || !flushNote()) return
        if (!entry.available) { rejected(entry.versionAbbreviation + " is not installed"); return }
        const from = beginJump()
        if (!openLocation(entry)) return
        endJump(from)
        passagePalette.navigated = true
        passagePalette.close()
        readerFocus()
    }
    function copyHistoryItem(id, plainText) {
        if (bible.busy) return
        const entry = historyEntry(id)
        if (!entry) return
        const scope = historyScope(entry)
        if (!scope) { rejected(entry.versionAbbreviation + " is not installed"); return }
        if (bible.copyScope(scope, !!plainText).length > 0) {
            notify(entry.reference + " · " + entry.versionAbbreviation + (plainText ? " copied as plain text" : " copied"), true)
            sounds.play("chime")
        } else sounds.play("error")
    }
    function historyScope(entry) {
        if (!entry || entry.start) return entry
        return entry.available && entry.firstVerse ? Object.assign({}, entry, { start: entry.firstVerse, end: entry.lastVerse }) : null
    }
    function addHistoryToDesk(id) {
        if (bible.busy) return
        const entry = historyEntry(id)
        const scope = historyScope(entry)
        if (!scope || !entry.available) { rejected(entry.versionAbbreviation + " is not installed"); return }
        addToDesk(scope)
    }
    function removeHistoryItem(id) {
        if (bible.busy) return
        if (!historyEntry(id)) return
        // The palette's History keeps its place: the row below moves up.
        const row = paletteResults.currentIndex
        if (bible.removeHistoryItem(id)) {
            sounds.play("remove")
            if (passagePalette.visible) Qt.callLater(function() {
                paletteResults.currentIndex = Math.max(0, Math.min(paletteResults.count - 1, row))
                referenceInput.forceActiveFocus()
            })
        }
    }
    function requestClearHistory() {
        if (bible.busy) return
        if (!bible.history.length && !bible.recentQueries.length) { notify("History is already empty", true); return }
        clearHistoryConfirmation.open()
    }
    function confirmClearHistory() {
        if (bible.busy) return
        const count = bible.history.length
        if (bible.clearHistory()) {
            clearHistoryConfirmation.close()
            sounds.play("remove")
            notify("Cleared History · " + count + (count === 1 ? " passage removed" : " passages removed"), true)
        }
    }
    function toggleHistoryEnabled() {
        const enable = !bible.historyEnabled
        if (!enable && paletteHistory) resetBooks()
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
    // A History row's menu (recent passages and the palette's History).
    function historyRowMenu(entry) {
        const ready = !bible.busy
        const id = entry.id
        const scope = historyScope(entry)
        return RowMenu.entries(app, { busy: bible.busy, keys: false, scope: scope,
            open: function() { app.openHistoryItem(id) }, canOpen: entry.available, canCopy: entry.available && !!scope,
            copy: function() { app.copyHistoryItem(id) }, copyPlain: function() { app.copyHistoryItem(id, true) },
            desk: function() { app.addHistoryToDesk(id) }, canDesk: entry.available && !!scope,
            extras: [{ id: "remove", text: "Remove from History", shortcut: KeyNotation.format("Delete"), enabled: ready, run: function() { app.removeHistoryItem(id) } }] })
    }
    function savePosition() {
        if (!restoringPosition && bible.ready) {
            let travel = Math.max(0, reading.contentHeight - reading.height)
            bible.readingPosition = travel > 0 ? Math.max(0, Math.min(1, readerTargetY() / travel)) : 0
        }
    }
    // Programmatic scrolls to a verse slide (ease-out, ~75 ms) and retarget
    // mid-flight, so held keys never lag; instant with animations disabled.
    readonly property int slideMs: reduceMotion ? 0 : 75
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
        if (focusLayout) target = y - 0.4 * reading.height
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
    // Marks: every mark (highlight, note, tags) on an exact range, in every
    // version, one row each in passage order (bible.marks()). Read when the
    // panel opens and again after changes, but never while a row's note is
    // being written in place: rebuilding the rows would drop the editor.
    property var marksRows: []
    function refreshMarks() {
        if (markEditKey.length) return
        // The current mark stays current; when it went, the row that took its place.
        const current = collectionList.model[collectionList.currentIndex]
        const key = current ? rangeKey(current) : "", index = collectionList.currentIndex
        marksRows = Array.from(bible.marks())
        const kept = key.length ? collectionList.model.findIndex(function(entry) { return app.rangeKey(entry) === key }) : -1
        collectionList.currentIndex = kept >= 0 ? kept : collectionList.count ? Math.max(0, Math.min(index, collectionList.count - 1)) : -1
    }
    function rangeKey(scope) { return [scope.versionId, scope.book, scope.chapter, scope.start, scope.endBook, scope.endChapter, scope.end].join(":") }
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
    // Opening Marks starts at the first row here, else the first row.
    function initialSavedRow() {
        const rows = collectionList.model || []
        const here = rows.findIndex(function(entry) { return app.savedHere(entry) })
        return here >= 0 ? here : rows.length ? 0 : -1
    }
    // The filter: every word must match. "#gr" matches a row with a tag
    // starting "gr"; other words match the reference, version, note, tags,
    // or verse text, ignoring case.
    property string savedQuery: ""
    // Typing replaces the field's text binding, so clear both.
    function clearSavedQuery() { savedQuery = ""; savedSearch.text = "" }
    function savedMatches(entry, query) {
        const text = [entry.reference, entry.versionAbbreviation || "", entry.note || "", tagsPlainText(entry.tags), entry.excerpt || ""]
            .join("\n").toLowerCase()
        const tags = Array.from(entry.tags || []).map(function(tag) { return tag.name.toLowerCase() })
        return query.toLowerCase().split(/\s+/).filter(function(word) { return word.length > 0 }).every(function(word) {
            if (word.length > 1 && word[0] === "#") return tags.some(function(name) { return name.startsWith(word.slice(1)) })
            return text.indexOf(word) >= 0
        })
    }
    function filteredMarks() {
        const query = savedQuery.trim()
        return marksRows.filter(function(entry) { return app.savedMatches(entry, query) })
    }
    // A row's tag, clicked: show only that tag's marks.
    function filterMarksByTag(name) {
        savedQuery = "#" + name.replace(/\s+/g, " ").split(" ")[0]
        savedSearch.text = savedQuery
        collectionList.currentIndex = collectionList.count ? 0 : -1
        focusSavedList()
    }
    // The filter field appears once Marks holds more than ten rows, and stays
    // while it has a query.
    readonly property bool savedSearchVisible: marksRows.length > 10 || savedQuery.length > 0
    function focusSavedSearch() { if (savedSearchVisible) savedSearch.forceActiveFocus() }
    function focusSavedList() {
        moveSaved(0)
    }
    // esc in Marks: a note written in place is done, the filter gives the
    // keyboard to the list, and the list closes Marks.
    function escapeSaved() {
        if (markEditKey.length) finishMarkNote(true)
        else if (savedSearch.activeFocus) focusSavedList()
        else closePanel()
    }
    // Leaving Marks for a dialog finishes a note written in place; a draft
    // that cannot be saved keeps it.
    function dismissMarks() { return !markEditKey.length || finishMarkNote(false) }
    function moveSaved(delta) {
        if (collectionList.count > 0) {
            collectionList.currentIndex = Math.max(0, Math.min(collectionList.count - 1, collectionList.currentIndex + delta))
            collectionList.positionViewAtIndex(collectionList.currentIndex, ListView.Contain)
        }
        if (collectionList.currentItem) collectionList.currentItem.focus = false
        collectionList.forceActiveFocus()
    }
    // A row opens its passage in the reader; Marks stays open beside it.
    function openSaved(entry) {
        if (!entry) { sounds.play("error"); return }
        if (!flushNote()) return
        const from = beginJump()
        if (!bible.openTaggedPassage(entry)) return
        endJump(from)
        syncNote()
        Qt.callLater(function() { app.scrollToVerse(bible.cursorVerse, true) })
        pulseDestination()
    }
    // delete / backspace: the row's mark goes at once (highlight, note, and
    // tags); u undoes it.
    function deleteSaved() {
        const row = collectionList.currentIndex
        const entry = focusedSavedEntry()
        if (!entry || bible.busy) return
        if (!deleteScope(entry, false)) return
        collectionList.currentIndex = Math.min(row, collectionList.count - 1)
        deleteFocusTimer.restart()
    }
    // Writing a row's note in place (w): the row opens an editor that saves
    // as you type (like Notes), compared with the text it started from so a
    // change from another window is never overwritten; esc closes it.
    // The draft lives here, not in the row: a row scrolled out of view goes.
    property string markEditKey: ""
    property var markEditScope: null
    property string markEditText: ""
    property string markEditBaseline: ""
    property bool markEditConflict: false
    Timer { id: markSaveTimer; interval: Math.max(1, app.noteSaveDelay); onTriggered: app.flushMarkNote() }
    function editMarkNote(entry) {
        if (!entry || bible.busy) { rejected(); return }
        if (markEditKey === rangeKey(entry)) { focusMarkEditor(); return }
        if (!flushNote()) return
        markEditScope = tagsUi.snapshot(entry)
        markEditText = markEditBaseline = entry.note || ""
        markEditConflict = false
        markEditKey = rangeKey(entry)
        Qt.callLater(focusMarkEditor)
    }
    function markEditor() {
        const rows = collectionList.contentItem.children
        for (let i = 0; i < rows.length; ++i) if (rows[i].editing) return rows[i].editor
        return null
    }
    function focusMarkEditor() {
        const editor = markEditor()
        if (!editor) return
        editor.forceActiveFocus()
        editor.cursorPosition = editor.length
    }
    // Saves a pending in-place draft; false keeps it (and whatever asked to
    // move on) where it is.
    function flushMarkNote() {
        markSaveTimer.stop()
        if (!markEditKey.length || markEditConflict) return !markEditConflict
        if (markEditText === markEditBaseline) return true
        const text = markEditText
        if (!bible.setScopeNote(markEditScope, text, markEditBaseline)) {
            if (bible.error.indexOf("another window") >= 0) { markEditConflict = true; bible.clearError() }
            return false
        }
        markEditBaseline = text
        return true
    }
    function finishMarkNote(refocus) {
        if (!flushMarkNote()) { rejected("Not saved · " + KeyNotation.format("Ctrl+S") + " keeps your draft, " + KeyNotation.format("Ctrl+Shift+S") + " the saved text"); return false }
        const key = markEditKey
        markEditKey = ""
        markEditScope = null
        bible.closeUndoGroup()
        refreshMarks()
        if (refocus) {
            const row = collectionList.model.findIndex(function(entry) { return app.rangeKey(entry) === key })
            if (row >= 0) collectionList.currentIndex = row
            focusSavedList()
        }
        return true
    }
    function keepMarkDraft() {
        if (!markEditConflict) { rejected(); return }
        markEditBaseline = bible.savedNote(markEditScope)
        markEditConflict = false
        flushMarkNote()
    }
    function useSavedMarkNote() {
        if (!markEditConflict) { rejected(); return }
        markEditText = markEditBaseline = bible.savedNote(markEditScope)
        const editor = markEditor()
        if (editor) editor.text = markEditText
        markEditConflict = false
    }
    function savedScopeExists(scope) {
        return !!scope && !!scope.versionId
            && (scope.versionId === bible.versionId ? Array.from(bible.collection) : marksRows).some(function(entry) {
                return ["versionId", "book", "chapter", "start", "endBook", "endChapter", "end"].every(function(key) {
                    return entry[key] === scope[key]
                })
            })
    }
    function openNoteScope(scope) {
        if (!scope || !flushNote()) return
        const from = beginJump()
        if (!bible.openTaggedPassage(scope)) return
        endJump(from)
        syncNote()
        openNotes(true)
        Qt.callLater(function() { app.scrollToVerse(bible.cursorVerse, true) })
        pulseDestination()
    }
    // With a selection, Versions shows its text in each version too.
    function openVersions(removing) {
        if (bible.busy || !flushNote()) return false
        clearCount()
        versionDialog.removing = !!removing
        versionDialog.step = 0
        versionDialog.chosen = null
        versionDialog.removeAnnotations = false
        const rows = !removing && hasSelection && bible.versions.length > 1 ? Array.from(bible.compareVersions(bible.tagScope())) : []
        const compared = {}
        for (const row of rows) compared[row.versionId] = row
        versionDialog.compared = compared
        versionDialog.compareReference = rows.length ? bible.selectionReference : ""
        versionSearch.text = ""
        versionResults.currentIndex = 0
        versionDialog.open()
        return true
    }
    // The installed versions this window does not read.
    function besideCandidates() { return Array.from(bible.versions).filter(function(entry) { return !entry.active }) }
    // shift C: the one other version opens beside this window at once; with
    // more, besideDialog asks which (showing the selection's text in each).
    function openBesideVersions() {
        const others = besideCandidates()
        if (others.length === 1) { openBeside(others[0].id); return }
        if (bible.busy || !flushNote()) return
        clearCount()
        const rows = hasSelection ? Array.from(bible.compareVersions(bible.tagScope())) : []
        const compared = {}
        for (const row of rows) compared[row.versionId] = row
        besideDialog.compared = compared
        besideDialog.compareReference = rows.length ? bible.selectionReference : ""
        besideSearch.text = ""
        besideResults.currentIndex = 0
        besideDialog.open()
    }
    function besideEntries() {
        const query = besideSearch.text.trim().toLowerCase()
        return besideCandidates().filter(function(entry) {
            return !query || (entry.abbreviation + " " + entry.name).toLowerCase().includes(query)
        })
    }
    function acceptBeside() {
        const entry = besideResults.model[besideResults.currentIndex]
        if (!entry || !bible.ready || bible.busy) { rejected(); return }
        besideDialog.close()
        openBeside(entry.id)
    }
    function besideKey(event) {
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            event.accepted = true
            if (event.modifiers === Qt.NoModifier) acceptBeside()
            return
        }
        if (event.modifiers !== Qt.NoModifier) return
        // In the search field, home and end move the caret, not the list.
        if (besideSearch.activeFocus && (event.key === Qt.Key_Home || event.key === Qt.Key_End)) return
        if (![Qt.Key_Up, Qt.Key_Down, Qt.Key_Home, Qt.Key_End, Qt.Key_PageUp, Qt.Key_PageDown].includes(event.key)) return
        event.accepted = true
        const count = besideResults.count
        const page = Math.max(1, Math.floor(besideResults.height / (64 * scale)))
        const delta = event.key === Qt.Key_Home ? -count : event.key === Qt.Key_End ? count
            : event.key === Qt.Key_PageUp ? -page : event.key === Qt.Key_PageDown ? page : event.key === Qt.Key_Up ? -1 : 1
        besideResults.currentIndex = count ? Math.max(0, Math.min(count - 1, besideResults.currentIndex + delta)) : -1
        if (count) besideResults.positionViewAtIndex(besideResults.currentIndex, ListView.Contain)
    }
    // The version in a window beside this one, the two following each other.
    function openVersionBeside(entry) {
        if (!entry || entry.active) { rejected(entry ? "Already reading " + (entry.abbreviation || entry.name) + " here" : ""); return }
        versionDialog.close()
        openBeside(entry.id)
    }
    function copyComparedVersion(plainText) {
        const entry = versionResults.model[versionResults.currentIndex]
        const row = entry ? versionDialog.compared[entry.id] : null
        if (!row || !row.available) { rejected(row ? "Not in " + row.abbreviation : ""); return }
        const scope = Object.assign({}, bible.tagScope(), { versionId: row.versionId })
        if (!bible.copyScope(scope, !!plainText).length) { rejected(); return }
        notify(row.reference + " · " + row.abbreviation + (plainText ? " copied as plain text" : " copied"), true)
        sounds.play("chime")
    }
    // Versions rows (choosing a version): Switch · Re-import · Details & Attribution · Remove…
    function openVersionMenu(index, sceneX, sceneY) {
        const entry = versionResults.model[index]
        if (!entry || versionDialog.removing || versionDialog.step !== 0) return
        versionResults.currentIndex = index
        versionResults.forceActiveFocus()
        const ready = !bible.busy
        const details = catalogEntry(entry.id)
        contextMenu.show("version", [
            { id: "switch", text: entry.active ? "Current version" : "Switch to " + (entry.abbreviation || entry.name), shortcut: KeyNotation.format("Return"),
                enabled: ready && !entry.active, run: function() { app.switchVersion(entry.id) } },
            entry.active ? null : { id: "beside", text: "Open beside", shortcut: menuKey("versionBeside"), enabled: ready && bible.ready,
                run: function() { app.openVersionBeside(entry) } },
            wantsReimport(entry) ? { id: "reimport", text: "Re-import", shortcut: menuKey("reimportVersion"), enabled: ready && !catalog.busy,
                run: function() { app.reimportVersion(entry) } } : null,
            { id: "details", text: "Details & Attribution", enabled: !!details,
                run: function() { catalogDetails.entry = details; catalogDetails.open() } },
            menuDivider,
            { id: "removeVersion", text: "Remove…", enabled: ready, run: function() {
                versionDialog.removing = true; versionDialog.chosen = entry; versionDialog.removeAnnotations = false
                versionDialog.step = 1; versionResults.currentIndex = 0; versionResults.forceActiveFocus() } }
        ].filter(function(item) { return !!item }), sceneX, sceneY)
    }
    // The footer's version button: Switch version… · Open beside (the one
    // other version by name, or … to choose) · Stop following.
    function openVersionButtonMenu(sceneX, sceneY) {
        const others = besideCandidates()
        contextMenu.show("versionButton", [menuAction("switch", "Switch version…", "switchVersion"),
            menuAction("beside", others.length === 1 ? "Open " + (others[0].abbreviation || others[0].name) + " beside" : "Open beside…", "openBeside"),
            following ? menuAction("unfollow", "Stop following " + followPartner, "unfollow") : null
        ].filter(function(item) { return !!item }), sceneX, sceneY)
    }
    function versionEntries() {
        if (versionDialog.step === 1) return [
            { name: "Keep annotations", hint: "Reconnect when this version is reimported", remove: false },
            { name: "Remove annotations too", hint: "Delete this version's notes, highlights, and tags", remove: true }
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
        return !versionDialog.removing && versionDialog.step === 0 && wantsReimport(entry) ? entry : null
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
            else if (event.modifiers === Qt.ControlModifier && !versionDialog.removing && versionDialog.step === 0)
                openVersionBeside(versionResults.model[versionResults.currentIndex])
            return
        }
        if (event.key === Qt.Key_Y && versionDialog.comparing && !textInputActive()
                && (event.modifiers === Qt.NoModifier || event.modifiers === Qt.ShiftModifier)) {
            event.accepted = true
            copyComparedVersion(event.modifiers === Qt.ShiftModifier)
            return
        }
        // Backspace (never while typing in the search) steps back a page.
        if (event.key === Qt.Key_Backspace && event.modifiers === Qt.NoModifier && !textInputActive()) {
            event.accepted = true
            if (versionDialog.step > 0 && !bible.busy) backVersion()
            else rejected()
            return
        }
        // The removal summary scrolls by key; arrows still move between Cancel and Confirm.
        if (versionDialog.step === 2 && ![Qt.Key_Up, Qt.Key_Down, Qt.Key_Left, Qt.Key_Right].includes(event.key)
                && scrollKey(versionSummaryScroll.contentItem as Flickable, event)) return
        if (event.modifiers !== Qt.NoModifier) return
        // In the search field, home and end move the caret, not the list.
        if (versionSearch.activeFocus && (event.key === Qt.Key_Home || event.key === Qt.Key_End)) return
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
        if (!versionDialog.removing || versionDialog.step !== 2 || !versionDialog.chosen || bible.busy || !flushNote()) return
        if (!bible.removeVersion(versionDialog.chosen.id, versionDialog.removeAnnotations)) return
        clearTextSelection()
        selectionAnchor = 0
        syncNote()
        if (bible.ready) restorePosition(hasSelection)
        else dismissNotesDrawer()
        versionDialog.close()
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
        // One ribbon for every window and version; it stays until placed again,
        // or removed (then nothing places it until shift R).
        { id: "placeRibbon", group: "History", title: "Place the ribbon here", sequences: ["Shift+R"], command: true },
        { id: "ribbon", group: "History", title: "Back to the ribbon", sequences: ["R"], command: true },
        { id: "removeRibbon", group: "History", title: "Remove the ribbon", sequences: ["Ctrl+Shift+R"], command: true },
        { id: "mouseBack", group: "History", title: "Back a jump, back to the palette's books, or close the open panel", sequences: ["BackClick"], explanation: true },
        { id: "mouseForward", group: "History", title: "Forward to the next jump", sequences: ["ForwardClick"], explanation: true },
        { id: "selectVerse", group: "Select", title: "Select verse", sequences: ["Click"], explanation: true },
        { id: "selectRange", group: "Select", title: "Select a range", sequences: ["Shift+Click"], explanation: true },
        { id: "visual", group: "Select", title: "Toggle visual selection", sequences: ["Shift+V"] },
        { id: "characterVisual", group: "Select", title: "Select characters (Vim charwise visual)", sequences: ["V"] },
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
        { id: "searchScope", group: "Search", title: "Bible search: every version or the one you read", sequences: ["Alt+A"], scope: "palette", command: true },
        // A query in slashes, /…/, is a regular expression either way.
        { id: "searchRegex", group: "Search", title: "Bible search: words or a regular expression", sequences: ["Alt+R"], scope: "palette", command: true },
        // Bound by the palette's input itself.
        { id: "searchBookJump", group: "Search", title: "Bible search: next book, previous book", sequences: ["Ctrl+Down", "Ctrl+Up"], scope: "search", explanation: true },
        { id: "highlight", group: "Annotate", title: "Highlight selection", sequences: ["X"], command: true },
        { id: "notes", group: "Annotate", title: "Write a note", sequences: ["W"], scope: "notes", command: true },
        { id: "tags", group: "Annotate", title: "Tag passage", sequences: ["T"], command: true },
        { id: "toggleDesk", group: "Annotate", title: "Add to Desk", sequences: ["D"], command: true },
        // Also on the Desk page while typing (deskKey); it asks first, and u undoes it.
        { id: "cleanDesk", group: "Annotate", title: "Clean Desk", sequences: ["Ctrl+Shift+D"], scope: "global", command: true },
        // This window's own changes to marks, notes, and tags (bible.undo()).
        { id: "undo", group: "Annotate", title: "Undo a mark, note, or tag change", sequences: ["U", "Ctrl+Z"], command: true },
        { id: "redo", group: "Annotate", title: "Redo the change undone", sequences: ["Ctrl+Shift+Z", "Ctrl+R"], command: true },
        { id: "manageTags", group: "Annotate", title: "Manage tags", sequences: ["Ctrl+T"], command: true },
        // Bound by the tag picker's Manage tags page itself.
        { id: "manageRename", group: "Annotate", title: "Manage tags: rename the chosen tag", sequences: ["Return"], scope: "tags", explanation: true },
        { id: "manageDelete", group: "Annotate", title: "Manage tags: delete a tag", sequences: ["Delete"], scope: "tags", explanation: true },
        { id: "manageMerge", group: "Annotate", title: "Manage tags: choose a tag to merge into", sequences: ["Ctrl+M"], scope: "tags", explanation: true },
        { id: "manageBack", group: "Annotate", title: "Manage tags: back to the picker (empty filter)", sequences: ["Backspace"], scope: "tags", explanation: true },
        // Bound by the note editor itself.
        { id: "noteLinkSuggest", group: "Annotate", title: "Suggest a passage to link (in a note)", sequences: ["[, ["], scope: "noteEditor" },
        { id: "openNoteLink", group: "Annotate", title: "Open the passage or web link at the cursor (in a note or the Desk)", sequences: ["Alt+Return"], scope: "noteEditor" },
        { id: "noteLinkClick", group: "Annotate", title: "Open the passage or web link clicked (in a note or the Desk)", sequences: ["Ctrl+Click"], scope: "noteEditor", explanation: true },
        { id: "notePreview", group: "Annotate", title: "Switch a note or the Desk between Write and Preview", sequences: ["Alt+V"], scope: "noteEditor" },
        { id: "noteKeepDraft", group: "Annotate", title: "Note or Desk changed in another window: save my draft", sequences: ["Ctrl+S"], scope: "noteEditor" },
        { id: "noteUseSaved", group: "Annotate", title: "Note or Desk changed in another window: use the saved text", sequences: ["Ctrl+Shift+S"], scope: "noteEditor" },
        { id: "noteLinkedFrom", group: "Annotate", title: "From a note to its Linked from list (esc returns)", sequences: ["Ctrl+Down"], scope: "noteEditor" },
        { id: "noteLeave", group: "Annotate", title: "Close a note or the Desk, the draft saved (its link suggestions first)", sequences: ["Escape"], scope: "noteEditor", explanation: true },
        { id: "copy", group: "Copy", title: "Copy passage as quotation", sequences: ["Y"], command: true },
        { id: "copyText", group: "Copy", title: "Copy selected text as quotation (else the passage)", sequences: ["Ctrl+C"] },
        { id: "middleCopy", group: "Copy", title: "Copy passage as quotation", sequences: ["MiddleClick"], explanation: true },
        { id: "copyPlain", group: "Copy", title: "Copy passage as plain text", sequences: ["Shift+Y"], command: true },
        { id: "copyTextPlain", group: "Copy", title: "Copy selected text as plain text (else the passage)", sequences: ["Ctrl+Shift+C"] },
        { id: "contextMenu", group: "Copy", title: "Open context menu", sequences: ["Menu", "Shift+F10"] },
        { id: "clickMenu", group: "Copy", title: "Open context menu", sequences: ["RightClick"], explanation: true },
        { id: "annotations", group: "Panels", title: "Marks panel", sequences: ["Shift+M"], command: true },
        // The Desk shows in the Margin, in place of the note (w turns back).
        { id: "desk", group: "Panels", title: "Open Desk", sequences: ["Shift+D"], command: true },
        // The See also panel (once Study); id kept for compatibility.
        { id: "related", group: "Panels", title: "See also panel", sequences: ["Shift+S"], command: true },
        // The keyboard between the reader and the open side panel, which stays
        // open (esc in a panel closes it). Works while typing in the panel too.
        { id: "switchPane", group: "Panels", title: "Keyboard to the open side panel, or back to the text", sequences: ["Ctrl+W"], aliases: ["F6"], aliasReason: "the desktop's key for moving between a window's panes.", command: true },
        // A popover (PeekCard), read in place; its own keys step and act.
        { id: "peek", group: "Panels", title: "Peek at the verse's footnotes", sequences: ["I"], command: true },
        { id: "books", group: "Palettes and windows", title: "Navigate to a passage", sequences: ["Ctrl+K"], scope: "palette", command: true },
        { id: "history", group: "Palettes and windows", title: "History", sequences: ["Ctrl+H"], scope: "palette", command: true },
        { id: "commands", group: "Palettes and windows", title: "Open commands", sequences: ["Ctrl+Space"], aliases: ["Ctrl+Shift+P"], aliasReason: "the command palette key in VS Code and its kin.", scope: "palette" },
        { id: "resume", group: "Palettes and windows", title: "Resume last palette", sequences: ["Space"] },
        { id: "settings", group: "Palettes and windows", title: "Reading settings", sequences: ["Ctrl+,"], scope: "global", command: true },
        { id: "switchVersion", group: "Palettes and windows", title: "Switch version", sequences: ["Ctrl+V"], scope: "global", command: true },
        // A window beside this one reading another version (WindowManager links
        // the two): the one other version at once, else a card to choose (besideDialog);
        // o in Compare and ctrl enter in Versions open the highlighted one.
        { id: "openBeside", group: "Palettes and windows", title: "Open a version beside this window", sequences: ["Shift+C"], command: true },
        // Versions, opened on the selection's text in each version.
        // A peek under the selection (PeekCard); Versions (ctrl v) shows the same rows.
        { id: "compare", group: "Panels", title: "Compare the selection across versions", sequences: ["C"], command: true },
        // Bound by the Compare card itself (peekKey).
        { id: "compareBeside", group: "Panels", title: "Compare: open the highlighted version in a window beside, following this one", sequences: ["O"], scope: "compare", explanation: true },
        // Bound by the Versions dialog itself, while it compares.
        { id: "compareCopy", group: "Palettes and windows", title: "Copy the highlighted version's text (Versions, comparing)", sequences: ["Y", "Shift+Y"], scope: "versions", explanation: true },
        { id: "help", group: "Palettes and windows", title: "Keybindings", sequences: ["?"], aliases: ["F1"], aliasReason: "the help key in most desktop apps.", scope: "global", command: true },
        { id: "newWindow", group: "Palettes and windows", title: "New window", sequences: ["Ctrl+N"], scope: "global", command: true },
        // Bound by the Versions dialog itself.
        { id: "addVersion", group: "Palettes and windows", title: "Add version from Versions", sequences: ["Ctrl+I"], scope: "versions", explanation: true },
        { id: "versionBeside", group: "Palettes and windows", title: "Open the highlighted version beside this window (Versions)", sequences: ["Ctrl+Return"], scope: "versions", explanation: true },
        // Bound by the Add version guide itself.
        { id: "guideInstall", group: "Palettes and windows", title: "Add version: install the highlighted version", sequences: ["Return"], scope: "importGuide", explanation: true },
        { id: "guideDetails", group: "Palettes and windows", title: "Add version: its Details & Attribution", sequences: ["Shift+Return"], scope: "importGuide", explanation: true },
        { id: "guideImport", group: "Palettes and windows", title: "Add version: import a USFM ZIP", sequences: ["Ctrl+I"], scope: "importGuide", explanation: true },
        { id: "versionBackKey", group: "Palettes and windows", title: "Back a step in Remove version (Versions)", sequences: ["Backspace"], scope: "versions", explanation: true },
        { id: "reimportVersion", group: "Palettes and windows", title: "Re-import the highlighted version (Versions)", sequences: ["Ctrl+Shift+I"], scope: "versions", explanation: true },
        { id: "focus", group: "Reading", title: "Focus mode", sequences: ["F"], setting: "focusMode", command: true },
        { id: "numbers", group: "Reading", title: "Show verse numbers", sequences: ["Alt+N"], setting: "showVerseNumbers", command: true },
        { id: "headings", group: "Reading", title: "Show headings", sequences: ["Alt+H"], setting: "showHeadings", command: true },
        { id: "chapterOutline", group: "Reading", title: "Chapter outline", sequences: ["Alt+O"], setting: "chapterOutline", command: true },
        // alt w: w writes a note; alt w shows notes in the margin.
        { id: "marginNotes", group: "Reading", title: "Show notes in margin", sequences: ["Alt+W"], setting: "marginNotes", command: true },
        // Last of the switches: right above Reading settings' "not marked" hint.
        { id: "wordsOfJesus", group: "Reading", title: "Highlight words of Jesus", sequences: ["Alt+J"], setting: "wordsOfJesus", command: true },
        { id: "readingMode", group: "Reading", title: "Layout", sequences: ["Alt+L"], command: true },
        // Paragraph, where a version marks no paragraphs read as Verse.
        { id: "defaultLayout", group: "Reading", title: "Default layout (Paragraph)", sequences: ["Alt+Shift+L"], command: true },
        { id: "soundEffects", group: "Reading", title: "Sound effects", sequences: ["Alt+S"], scope: "global", command: true },
        { id: "reduceMotion", group: "Reading", title: "Animations", sequences: ["Alt+M"], scope: "global", command: true },
        { id: "larger", group: "Reading", title: "Larger text", sequences: ["Ctrl+="], aliases: ["Ctrl++"], aliasReason: "the zoom-in key in most apps (shift = on many layouts)." },
        { id: "smaller", group: "Reading", title: "Smaller text", sequences: ["Ctrl+-"] },
        { id: "resetText", group: "Reading", title: "Reset text size", sequences: ["Ctrl+0"] },
        { id: "lineMore", group: "Reading", title: "Increase line height", sequences: ["Ctrl+Alt+="] },
        { id: "lineLess", group: "Reading", title: "Decrease line height", sequences: ["Ctrl+Alt+-"] },
        { id: "lineReset", group: "Reading", title: "Reset line height", sequences: ["Ctrl+Alt+0"], command: true },
        { id: "dismiss", group: "Reading", title: "Dismiss current panel or selection", sequences: ["Escape"], explanation: true },
        // List panels (Annotations, Desk, History, Study) bind these themselves.
        { id: "listNext", group: "Lists", title: "Next item", sequences: ["J", "Down"], scope: "list", explanation: true },
        { id: "listPrevious", group: "Lists", title: "Previous item", sequences: ["K", "Up"], scope: "list", explanation: true },
        { id: "listFirst", group: "Lists", title: "First item", sequences: ["G", "Home"], scope: "list", explanation: true },
        { id: "listLast", group: "Lists", title: "Last item", sequences: ["Shift+G", "End"], scope: "list", explanation: true },
        { id: "listHalfDown", group: "Lists", title: "Down 5 items", sequences: ["Ctrl+D"], scope: "list", explanation: true },
        { id: "listHalfUp", group: "Lists", title: "Up 5 items", sequences: ["Ctrl+U"], scope: "list", explanation: true },
        { id: "listPageDown", group: "Lists", title: "Down 10 items", sequences: ["Ctrl+F", "PgDown"], scope: "list", explanation: true },
        { id: "listPageUp", group: "Lists", title: "Up 10 items", sequences: ["Ctrl+B", "PgUp"], scope: "list", explanation: true },
        { id: "listOpen", group: "Lists", title: "Open item", sequences: ["Return"], scope: "list", explanation: true },
        { id: "listCopy", group: "Lists", title: "Copy item as quotation", sequences: ["Y"], scope: "list", explanation: true },
        { id: "listCopyPlain", group: "Lists", title: "Copy item as plain text", sequences: ["Shift+Y"], scope: "list", explanation: true },
        { id: "listDesk", group: "Lists", title: "Add item to Desk", sequences: ["D"], scope: "list", explanation: true },
        { id: "listRemove", group: "Lists", title: "Remove item", sequences: ["Delete", "Backspace"], scope: "list", explanation: true },
        { id: "listTag", group: "Lists", title: "Edit the item's tags (Marks, See also)", sequences: ["T"], scope: "list", explanation: true },
        { id: "listNote", group: "Lists", title: "Write the item's note in place (Marks)", sequences: ["W"], scope: "list", explanation: true },
        { id: "listSearch", group: "Lists", title: "Filter the list, #name for a tag (Marks with more than 10 items, See also)", sequences: ["/"], scope: "list", explanation: true },
        // Bound by the History panel; the command itself is Commands-only.
        { id: "historyRemoveKeys", group: "Palettes and windows", title: "History: remove a passage", sequences: ["Del"], scope: "history", explanation: true },
        { id: "historyClearKeys", group: "Palettes and windows", title: "History: clear it", sequences: ["Shift+Del"], scope: "history", explanation: true },
        { id: "import", group: "Commands only", title: "Add version", scope: "global", command: true },
        { id: "removeVersion", group: "Commands only", title: "Remove version", scope: "global", command: true },
        { id: "export", group: "Commands only", title: "Export your marks as Markdown", command: true },
        { id: "backup", group: "Commands only", title: "Back up your marks", command: true },
        { id: "restore", group: "Commands only", title: "Restore a backup of your marks", command: true },
        { id: "clearHistory", group: "Commands only", title: "Clear History", command: true },
        { id: "unfollow", group: "Commands only", title: "Stop following the window beside", command: true },
        { id: "toggleHistory", group: "Commands only", title: "Disable / Enable History", command: true },
        { id: "about", group: "Commands only", title: "About & Attribution", scope: "global", command: true }
    ]
    // Explanations are gestures; desk/list rows are bound by their panels.
    function globallyBound(action) { return !!action.sequences && !action.explanation && !["list", "versions", "noteEditor", "characters", "tags", "importGuide"].includes(action.scope) }
    function actionById(id) { return actions.find(function(entry) { return entry.id === id }) }
    function actionKeys(action) {
        if (typeof action === "string") action = actionById(action)
        return action && action.sequences ? Array.from(action.sequences) : []
    }
    // Generated key notation (KeyNotation.js): plain for tooltips and menus, rich for lists.
    function keyText(action) { return KeyNotation.formatList(actionKeys(action)) }
    readonly property string escapeText: KeyNotation.format("Escape")
    // The Desk header's quiet line: how many passages its page quotes (the
    // lines d writes after each quotation, "> — John 3:16 (NET)") and how many words it has.
    function deskSummary(text) {
        if (!text.trim().length) return "Empty · " + keyText("toggleDesk") + " adds the passage you read"
        const passages = (text.match(/^> \u2014 .+ \(.+\)[ \t]*$/gm) || []).length
        const words = text.split(/\s+/).filter(function(word) { return /[0-9A-Za-z\u00C0-\u1FFF\u2070-\uFFFF]/.test(word) }).length
        const parts = passages ? [passages + (passages === 1 ? " passage" : " passages")] : []
        parts.push(words + (words === 1 ? " word" : " words"))
        return parts.join(" · ")
    }
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
        // Pressed by key, a button that hides itself (Dismiss, Save my draft,
        // Retry…) would strand the keyboard on the scope above it: hand it on.
        if (byKey) Qt.callLater(function() {
            if (button.visible) return
            const focus = app.activeFocusItem
            let lost = !focus || !focus.visible
            for (let at = button; at && !lost; at = at.parent) if (at === focus) lost = true
            if (!lost) return
            const target = button.focusTarget
            if (target && target.visible && target.enabled) target.forceActiveFocus()
            else app.restorePanelFocus()
        })
    }
    // Clickable hint rows (HintRow): [keys, label, run, enabled] per part. A
    // part runs what its key would, and is clickable only while that works
    // with the current focus. Navigation parts have no run.
    // ← steps back to the book list only where the palette's keys take it
    // (in the input: nothing selected and the caret at the start, or browsing).
    readonly property bool paletteLeftGoesBack: !paletteTextMode && (paletteBook > 0 || paletteHistory)
        && (!referenceInput.activeFocus || (referenceInput.selectedText.length === 0
            && (!referenceInput.text.length || referenceInput.cursorPosition === 0 || paletteArrowUsed)))
    function paletteHintParts() {
        if (paletteHistory && !paletteTextMode && !referenceInput.text.trim().startsWith(">")) {
            const entry = paletteResults.model[paletteResults.currentIndex]
            const ready = !!entry && !bible.busy
            return [[["Up", "Down"], "choose"], ["Return", "open", app.acceptPalette, ready],
                ["Delete", "remove", function() { app.removeHistoryItem(entry.recent.id) }, ready],
                [actionKeys("historyClearKeys")[0], "clear", app.requestClearHistory, bible.history.length > 0 && !bible.busy],
                ["Backspace", "back", app.backToBooks], ["Escape", "close", app.escapePalette]]
        }
        const current = paletteResults.model[paletteResults.currentIndex]
        return [[paletteChapterGrid ? ["Left", "Right", "Up", "Down"] : ["Up", "Down"], "choose"],
            paletteMoreResults ? ["Down", "more", function() { app.loadMorePalette() }] : null,
            paletteTextMode && paletteSearchCurrent && bible.searchBooks.length > 1 ? [["Ctrl+Up", "Ctrl+Down"], "books", function() { app.jumpSearchBook(1) }] : null,
            paletteTextMode && bible.versions.length > 1 ? [actionKeys("searchScope")[0], bible.searchAllVersions ? bible.versionAbbreviation + " only" : "all versions", app.toggleSearchScope] : null,
            paletteTextMode ? [actionKeys("searchRegex")[0], bible.searchRegex ? "words" : "regex", app.toggleSearchRegex] : null,
            ["Return", "select", app.acceptPalette, !!paletteDestination.entry || paletteResolution.valid],
            !paletteTextMode && !referenceInput.text.length && paletteRemovable(current)
                ? ["Delete", "remove", function() { app.removePaletteEntry(current) }, !bible.busy] : null,
            paletteChapterGrid ? ["Backspace", "books", app.backToBooks] : paletteLeftGoesBack ? ["Left", "back", app.backToBooks] : null,
            // tab / shift tab cycle Passage → Search → Command.
            bible.ready ? ["Tab", "mode", function() { app.cyclePaletteMode(1) }] : null, ["Escape", "close", app.escapePalette]]
    }
    function versionHintParts() {
        const ready = !bible.busy
        const back = ["Backspace", "back", app.backVersion, ready]
        if (versionDialog.step === 2) return [[["Left", "Right"], "choose"], ["Return", "select", app.acceptVersion, ready], back, ["Escape", "cancel", function() { versionDialog.close() }]]
        const select = ["Return", "select", app.acceptVersion, ready && versionResults.currentIndex >= 0 && versionResults.count > 0]
        const reimport = reimportableVersion()
        const entry = versionResults.model[versionResults.currentIndex]
        const besideReady = ready && bible.ready && !!entry && !entry.active
        if (!versionDialog.removing) return [[["Up", "Down"], "choose"],
            versionDialog.comparing ? ["Y", "copy", function() { app.copyComparedVersion(false) }, ready] : null,
            [actionKeys("addVersion")[0], "add", app.addVersionFromPicker, ready],
            reimport ? [actionKeys("reimportVersion")[0], "re-import", function() { app.reimportVersion(app.reimportableVersion()) }, ready && !catalog.busy] : null,
            select,
            bible.versions.length > 1 ? [actionKeys("versionBeside")[0], "beside",
                function() { app.openVersionBeside(versionResults.model[versionResults.currentIndex]) }, besideReady] : null,
            ["Escape", "cancel", function() { versionDialog.close() }]]
        return [[["Up", "Down"], "choose"], versionSearch.activeFocus ? null : [["Home", "End"], "jump"], select, versionDialog.step > 0 ? back : null, ["Escape", "cancel", function() { versionDialog.close() }]]
    }
    function savedHintParts() {
        if (!marginKeys) return panelAwayHints
        if (markEditKey.length) return markEditConflict
            ? [[actionKeys("noteKeepDraft")[0], "save my draft", app.keepMarkDraft], [actionKeys("noteUseSaved")[0], "use saved text", app.useSavedMarkNote]]
            : [["Escape", "done", function() { app.finishMarkNote(true) }]]
        const entry = focusedSavedEntry()
        const ready = !!entry && !bible.busy
        const search = savedSearchVisible ? ["/", "filter", app.focusSavedSearch] : null
        return [["Return", "open", function() { app.openSaved(app.focusedSavedEntry()) }, ready],
            ["W", "note", function() { app.editMarkNote(app.focusedSavedEntry()) }, ready],
            ["T", "tags", function() { app.openPassageTags(tagsUi.snapshot(app.focusedSavedEntry())) }, ready],
            ["Y", "copy", function() { app.copyScope(app.focusedSavedEntry()) }, ready],
            ["D", "Desk", function() { app.addToDesk(app.focusedSavedEntry()) }, ready],
            ["Delete", "delete", app.deleteSaved, ready], search, panelReturnHint, ["Escape", "close", app.escapeSaved]]
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
        if (action.id === "soundEffects") return sounds.enabled ? "Disable sound effects" : "Enable sound effects"
        if (action.id === "searchScope") return bible.searchAllVersions ? "Search " + bible.versionAbbreviation + " only" : "Search every version"
        if (action.id === "searchRegex") return bible.searchRegex ? "Search for words" : "Search with a regular expression"
        if (action.id === "reduceMotion") return sounds.reduceMotion ? "Enable animations" : "Disable animations"
        if (action.id === "toggleHistory") return bible.historyEnabled ? "Disable History" : "Enable History"
        if (action.id === "undo" && bible.canUndo) return "Undo " + bible.undoLabel
        if (action.id === "redo" && bible.canRedo) return "Redo " + bible.redoLabel
        if (action.id === "placeRibbon" && bible.ribbon.versionId) return "Move the ribbon here"
        if (action.id === "ribbon" && bible.ribbon.versionId) return "Back to the ribbon · " + bible.ribbon.reference
        if (action.id === "removeRibbon" && bible.ribbon.versionId) return "Remove the ribbon from " + bible.ribbon.reference
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
        // ctrl w (F6) moves the keyboard from wherever it is in the reader or
        // the side panel, while typing too; never over a modal.
        if (action.id === "switchPane") return !modalOpen && bible.ready && (sidePanel.length > 0 || readerKeys)
        // Inside the tag picker ctrl t switches between the picker and Manage.
        // (During a confirmation it sounds the error: togglePage rejects it.)
        if (action.id === "manageTags" && tagsUi.pickerVisible) return !errorDialog.visible
        // Character selection owns the reader's keys (charKey); window keys
        // (palettes, dialogs) still work and end it as they take the keyboard.
        if (charMode && readerBase && action.scope !== "global" && action.scope !== "palette") return false
        if (zPending && action.sequences.some(function(sequence) { return ["Z", "T", "B"].includes(sequence) })) return false
        // In the Notes editor, the palette and dialog launchers (Ctrl, F-keys)
        // still work, saving the note first (runKey); its plain keys type.
        if (noteEditorTyping() && noteEditorLaunchActions.includes(action.id))
            return bible.ready && !modalOpen && !linkSuggesting
        // Typing owns every key, Ctrl and Alt included (ctrl v pastes); only the
        // palette's own mode keys still switch modes from its input.
        if (textInputActive() && !(action.scope === "palette" && passagePalette.visible)) return false
        // With the keyboard in Marks, w writes the current row's note in place (Marks binds it).
        if (marksOpen && marginKeys && action.id === "notes") return false
        if (sidePanel.length && panelActions.includes(action.id)) return panelSwitchKeys
        if (sidePanel.length && panelLeavingActions.includes(action.id)) return panelSwitchKeys
        // Undo works where changes are made: the reader and its side panels.
        if (sidePanel.length && undoActions.includes(action.id)) return panelSwitchKeys
        // ctrl t opens Manage tags over a side panel (Marks binds it itself).
        if (sidePanel.length && action.id === "manageTags") return panelSwitchKeys && !(marksOpen && marginKeys)
        if (settingsDialog.opened && !errorDialog.visible
                && (action.setting || ["readingMode", "defaultLayout", "larger", "smaller", "resetText", "lineLess", "lineMore", "lineReset"].includes(action.id))) return true
        // visible, not opened: the palette accepts keys during its fade-in.
        if (action.scope === "palette") return !modalOpen || passagePalette.visible
        if (action.scope === "global") return !modalOpen && (bible.ready || ["newWindow", "help", "about", "soundEffects", "reduceMotion", "import", "switchVersion", "removeVersion", "settings"].includes(action.id))
        if (action.scope === "notes") return bible.ready && !modalOpen && !textInputActive()
        return readerKeys
    }
    readonly property var textSizeActions: ["larger", "smaller", "resetText", "lineLess", "lineMore", "lineReset"]
    readonly property var undoActions: ["undo", "redo"]
    // Launchers that work while typing a note: the palettes and dialogs on Ctrl
    // or F-keys (help's ? types; its F1 alias launches). Ctrl v stays paste, so
    // Switch version is not here; ctrl n and the Alt toggles stay the editor's.
    readonly property var noteEditorLaunchActions: ["books", "history", "bibleSearch", "commands", "settings", "help", "manageTags"]
    // Typing in the Margin: a note, or the Desk's page.
    function noteEditorTyping() { return (notesOpen && !!noteEditor && noteEditor.activeFocus) || (deskOpen && !!deskEditor && deskEditor.activeFocus) }
    function actionAvailable(action) {
        if (typeof action === "string") action = actions.find(function(entry) { return entry.id === action })
        if (!action || action.explanation || bible.busy) return false
        if (action.id === "copyText" || action.id === "copyTextPlain") return bible.ready
        if (action.id === "related") return bible.ready && bible.verses.length > 0
        if (["highlight", "tags", "toggleDesk"].includes(action.id)) return hasSelection
        if (action.id === "compare") return hasSelection && bible.versions.length > 1
        if (action.id === "openBeside") return bible.ready && bible.versions.length > 1
        if (action.id === "switchPane") return bible.ready && sidePanel.length > 0
        if (action.id === "unfollow") return following
        if (action.id === "peek") return hasSelection && Array.from(bible.footnotes).some(function(note) { return note.verse === bible.cursorVerse })
        if (action.id === "undo") return bible.canUndo
        if (action.id === "redo") return bible.canRedo
        if (action.id === "ribbon" || action.id === "removeRibbon") return bible.ready && !!bible.ribbon.versionId
        // Notes editor keys, run from Keybindings: they act in an open Notes panel.
        if (["openNoteLink", "notePreview"].includes(action.id) && deskOpen) return true
        if (["noteLinkSuggest", "openNoteLink", "notePreview"].includes(action.id)) return notesOpen && hasSelection
        if (action.id === "noteKeepDraft") return deskOpen ? canKeepDeskDraft : notesOpen && canKeepDraft
        if (action.id === "noteUseSaved") return deskOpen ? deskConflict : notesOpen && noteConflict
        if (action.id === "noteLinkedFrom") return notesOpen && hasSelection && linkedFrom.length > 0
        return bible.ready || !!action.setting
            || ["books", "commands", "newWindow", "help", "about", "soundEffects", "reduceMotion", "import", "switchVersion", "removeVersion", "settings", "readingMode", "defaultLayout", "larger", "smaller", "resetText", "lineLess", "lineMore", "lineReset"].includes(action.id)
    }
    function scopeTitle(action) {
        if (action.scope === "list") return "List panels · Marks, Desk, History, See also"
        if (action.scope === "history") return "History in the palette only"
        if (action.scope === "search") return "Bible search results (" + keyText("bibleSearch") + ")"
        if (action.scope === "versions") return "Versions dialog"
        if (action.scope === "compare") return "Compare · " + keyText("compare")
        if (action.scope === "notes") return "Reader or notes preview"
        if (action.scope === "noteEditor") return "Writing a note or on the Desk"
        if (action.scope === "characters") return "Character selection · " + keyText("characterVisual")
        if (action.scope === "importGuide") return "Add version guide"
        if (action.scope === "tags") return "Tag picker · Manage tags (" + keyText("manageTags") + ")"
        if (action.explanation) return "Reader gesture"
        if (textSizeActions.includes(action.id)) return "Anywhere in this window · panels, dialogs, and while typing"
        if (noteEditorLaunchActions.includes(action.id)) return (action.scope === "palette" ? "This window · passage and command search" : "This window")
            + " · also while writing a note or on the Desk" + (action.id === "help" ? " (" + KeyNotation.format("F1") + ")" : "")
        if (action.scope === "global") return "This window · also with a side panel open, but not while typing"
        if (action.scope === "palette") return "This window · passage and command search"
        if (undoActions.includes(action.id)) return "Reader and side panels · also selecting characters, not while typing"
        return "Reader · outside text editing and dialogs"
    }
    // Keybindings come group by group (each under its sticky header), in the
    // order the groups first appear in the catalog.
    readonly property var keybindingGroups: {
        const groups = []
        for (const action of actions) if (!groups.includes(action.group)) groups.push(action.group)
        return groups
    }
    function filteredKeybindings() {
        let query = keybindingSearch.text.trim().toLowerCase()
        // The engine's sort is not stable: keep the catalog's order within a group.
        return actions.slice().sort(function(a, b) { return keybindingGroups.indexOf(a.group) - keybindingGroups.indexOf(b.group) || actions.indexOf(a) - actions.indexOf(b) })
            .filter(function(binding) {
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
            if (!paletteSearchCurrent) return []
            // Bible order, version by version; each book's first result
            // carries its heading and count (and, searching every version,
            // its version's abbreviation).
            const books = {}
            for (const entry of Array.from(bible.searchBooks)) books[entry.versionId + ":" + entry.book] = entry
            const rows = Array.from(bible.searchResults)
            return rows.map(function(row, index) {
                if (index > 0 && rows[index - 1].book === row.book && rows[index - 1].versionId === row.versionId) return row
                const book = books[row.versionId + ":" + row.book]
                return Object.assign({ firstOfGroup: true, group: book ? (bible.searchAllVersions && bible.versions.length > 1 ? book.versionAbbreviation + " · " : "")
                    + book.name + " · " + book.count : "" }, row)
            })
        }
        let query = referenceInput.text.trim().toLowerCase()
        if (query.startsWith(">")) {
            query = query.substring(1).trim()
            return actions.filter(function(c) { return c.command && actionTitle(c).toLowerCase().indexOf(query) >= 0 })
                .map(function(c) { return { title: actionTitle(c), hint: actionHint(c), action: c.id } })
        }
        if (paletteHistory) return historyPaletteEntries(query)
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
    // "Continue" at the ribbon, up to five History passages, never the one
    // being read, then "All History". The last row carries a rule that
    // separates the group from the books.
    readonly property int paletteRecentLimit: 5
    function recentPassages() {
        // Read notifying properties so the palette model follows the reader.
        const here = bible.ready && (bible.passage + bible.selectionReference + bible.versionId).length ? bible.currentLocation() : null
        const recents = Array.from(bible.history).filter(function(entry) {
            if (!entry.available || !here) return !!entry.available
            const sameChapter = entry.versionId === here.versionId && entry.book === here.book && entry.chapter === here.chapter
            return !app.sameLocation(entry, here) && !(sameChapter && (!entry.start || !here.start))
        }).slice(0, paletteRecentLimit).map(function(entry) {
            return { title: entry.reference, hint: "Recent · " + entry.versionAbbreviation, recent: entry }
        })
        // Then all of History, a step away; the rule follows the group.
        const all = bible.historyEnabled && bible.history.length
            ? [{ title: "All History", hint: bible.history.length + (bible.history.length === 1 ? " passage" : " passages"), allHistory: true }] : []
        // The ribbon comes first, away from it.
        const ribbon = bible.ribbon.versionId && !ribbonHere
            ? [{ title: "Continue · " + bible.ribbon.reference, hint: "Ribbon · " + keyText("ribbon"), ribbon: true }] : []
        const group = ribbon.concat(recents).concat(all)
        if (group.length) group[group.length - 1].lastRecent = true
        return group
    }
    // Every History passage, newest first, under Today / Yesterday / Earlier;
    // a typed query narrows them by reference or version.
    function historyPaletteEntries(query) {
        const rows = historyRows.filter(function(entry) {
            return !query || (entry.reference + " " + entry.versionAbbreviation).toLowerCase().indexOf(query) >= 0
        })
        return rows.map(function(entry, index) {
            return { title: entry.reference, recent: entry, history: true, group: entry.group,
                firstOfGroup: index === 0 || rows[index - 1].group !== entry.group,
                hint: historyTime(entry.visitedAt) + " · " + entry.versionAbbreviation + (entry.id === app.historyCurrentId ? " · here" : "") }
        })
    }
    function showPaletteHistory() {
        if (!bible.historyEnabled) { rejected("History is disabled · enable it in Commands"); return }
        paletteBook = 0
        paletteBookName = ""
        paletteHistory = true
        referenceInput.text = ""
        paletteArrowUsed = false
        paletteResults.currentIndex = 0
        paletteResults.positionViewAtBeginning()
        referenceInput.forceActiveFocus()
    }
    // ctrl h: the palette's History.
    function openHistoryPalette() {
        if (!bible.historyEnabled) { rejected("History is disabled · enable it in Commands"); return }
        if (!passagePalette.visible || paletteTextMode || referenceInput.text.trim().startsWith(">")) openPalette(false)
        if (passagePalette.visible) showPaletteHistory()
    }
    function openRecentPassage(entry) {
        if (!flushNote()) return
        const from = beginJump()
        if (!openLocation(entry)) return
        endJump(from)
        passagePalette.navigated = true
        passagePalette.close()
        readerFocus()
    }
    // The ribbon (shift R places it, r returns): one reading place for every
    // window, opened in this version where it has the passage.
    readonly property bool ribbonHere: bible.ready && !!bible.ribbon.versionId
        && bible.ribbon.book === bible.book && bible.ribbon.chapter === bible.chapter
    function placeRibbon() {
        savePosition()
        const moved = !!bible.ribbon.versionId
        const shown = ribbonHere
        if (!bible.placeRibbon()) { sounds.play("error"); return }
        notify("Ribbon " + (moved ? "moved to " : "placed at ") + bible.ribbon.reference, true)
        sounds.play("chime")
        ribbonDrop.stop()
        if (reduceMotion) { ribbonTab.drop = 0; return }
        // Shown already, it lifts out first; otherwise it drops in from above.
        ribbonLift.duration = shown ? 90 : 0
        ribbonDrop.start()
    }
    SequentialAnimation {
        id: ribbonDrop
        objectName: "ribbonDrop"
        NumberAnimation { id: ribbonLift; target: ribbonTab; property: "drop"; to: -ribbonTab.height - 2; easing.type: Easing.InQuad }
        NumberAnimation { target: ribbonTab; property: "drop"; to: 0; duration: 260; easing.type: Easing.OutBack; easing.overshoot: 2 }
    }
    // Every window loses it at once; shift R places it again.
    function removeRibbon() {
        const reference = bible.ribbon.reference
        if (!bible.removeRibbon()) { sounds.play("error"); return }
        notify("Ribbon removed from " + reference + " · " + keyText("placeRibbon") + " places it again", true)
        sounds.play("remove")
    }
    // The footer's way back to the ribbon: back, move it here, or remove it.
    // On the ribbon itself (its tab, in its chapter) only removing it applies.
    function openRibbonMenu(sceneX, sceneY, onRibbon) {
        const remove = menuAction("removeRibbon", "Remove the ribbon", "removeRibbon")
        contextMenu.show("ribbon", onRibbon ? [remove] : [menuAction("ribbon", "Back to the ribbon", "ribbon"),
            menuAction("placeRibbon", "Move the ribbon here", "placeRibbon"), menuDivider, remove], sceneX, sceneY)
    }
    function openRibbon() {
        if (!flushNote()) return false
        const location = bible.ribbonLocation()
        if (!location.versionId) { rejected("The ribbon's passage is in no installed version"); return false }
        const from = beginJump()
        if (!openLocation(location)) return false
        endJump(from)
        return true
    }
    function ribbonRowMenu() {
        const location = bible.ribbonLocation()
        const scope = location.start ? location : null
        return RowMenu.entries(app, { busy: bible.busy, keys: false, scope: scope, canCopy: !!scope, canDesk: !!scope,
            open: function() { app.activatePaletteEntry({ ribbon: true }) },
            extras: [{ id: "remove", text: "Remove the ribbon", shortcut: KeyNotation.format("Delete"), enabled: !bible.busy, run: app.removeRibbon }] })
    }
    // delete in the palette: Continue takes the ribbon away, a recent or
    // History passage leaves History.
    function paletteRemovable(entry) { return !!entry && (!!entry.ribbon || !!entry.recent) }
    function removePaletteEntry(entry) {
        if (entry && entry.ribbon) runCommand("removeRibbon")
        else if (entry && entry.recent) removeHistoryItem(entry.recent.id)
        else rejected()
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
        if (entry.allHistory) { showPaletteHistory(); return }
        if (entry.recent) { openRecentPassage(entry.recent); return }
        if (entry.ribbon) {
            if (openRibbon()) { passagePalette.navigated = true; passagePalette.close(); readerFocus() }
            return
        }
        if (entry.candidate) { goToReference(entry.reference, true); return }
        if (entry.action) {
            if (!actionAvailable(entry.action)) { rejected(unavailableReason(entry.action)); return }
            if (!flushNote()) return
            if (["books", "history", "bibleSearch", "commands", "resume"].includes(entry.action)) {
                runCommand(entry.action)
                return
            }
            passagePalette.pendingAction = entry.action
            passagePalette.close()
        } else if (entry.chapter) openChapter(entry.book, entry.chapter)
        else {
            // Changing the book rebuilds the results model; snapshot the entry first.
            let book = entry.book
            let name = entry.title
            paletteBookName = name
            paletteBook = book
            referenceInput.text = ""
            paletteArrowUsed = false
            paletteChapters.currentIndex = 0
            paletteChapters.positionViewAtBeginning()
            referenceInput.forceActiveFocus()
        }
    }
    function acceptPalette() {
        if (paletteBack.activeFocus) { backToBooks(); return }
        let destination = paletteDestination
        if (destination.entry) activatePaletteEntry(destination.entry)
        else if (paletteResolution.valid) goToReference(destination.reference, true)
        else rejected(paletteResolution.error || "Type a passage, like John 3:16")
    }
    // Back from a book's chapters to the books, that book chosen (from
    // History, its row).
    function backToBooks() {
        const book = paletteBook, history = paletteHistory
        resetBooks()
        const row = Array.from(paletteResults.model).findIndex(function(entry) {
            return history ? !!entry.allHistory : entry.book === book && !entry.chapter && !entry.recent })
        if (row > 0) {
            paletteArrowUsed = true
            paletteResults.currentIndex = row
            paletteResults.positionViewAtIndex(row, ListView.Contain)
        }
        referenceInput.forceActiveFocus()
    }
    function resetBooks() {
        paletteHistory = false
        paletteBook = 0
        paletteBookName = ""
        referenceInput.text = ""
        paletteArrowUsed = false
        paletteResults.currentIndex = 0
        referenceInput.forceActiveFocus()
    }
    // ctrl ↓ / ctrl ↑: the next book's first result, or this book's first
    // (then the previous book's), loading pages as needed.
    function jumpSearchBook(direction) {
        const books = Array.from(bible.searchBooks)
        if (!paletteTextMode || !paletteSearchCurrent || bible.searchBusy || !books.length) { rejected(); return }
        const current = paletteResults.model[paletteResults.currentIndex]
        const at = current ? books.findIndex(function(entry) { return entry.book === current.book && entry.versionId === current.versionId }) : -1
        const target = direction > 0 ? at + 1
            : at >= 0 && paletteResults.currentIndex > books[at].index ? at : at - 1
        if (target < 0 || target >= books.length) { sounds.play("boundary"); return }
        const index = books[target].index
        while (bible.searchResults.length <= index && bible.searchResults.length < bible.searchResultCount) bible.loadMoreSearchResults()
        paletteArrowUsed = true
        paletteResults.currentIndex = index
        paletteResults.positionViewAtIndex(index, ListView.Beginning)
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
        const rows = Math.max(1, Math.floor(paletteList.height / (44 * scale)))
        const page = paletteChapterGrid ? rows * paletteColumns : rows
        if (event.key === Qt.Key_PageDown) movePalette(page)
        else if (event.key === Qt.Key_PageUp) movePalette(-page)
        else if (event.key === Qt.Key_End && paletteArrowUsed) {
            if (paletteList.currentIndex >= paletteList.count - 1) loadMorePalette()
            else movePalette(paletteList.count - 1 - paletteList.currentIndex)
        } else if (event.key === Qt.Key_Home && paletteArrowUsed) movePalette(-paletteList.currentIndex)
        else return false
        event.accepted = true
        return true
    }
    // ↑ ↓ move a row: in the chapter grid, a row of paletteColumns.
    function paletteRow(direction) { movePalette(direction * (paletteChapterGrid ? paletteColumns : 1)) }
    function movePalette(delta) {
        paletteArrowUsed = true
        const view = paletteList
        // Above the first row (the grid's first row too) is Back.
        if (delta < 0 && paletteBack.visible && (view.currentIndex <= 0
                || (paletteChapterGrid && delta === -paletteColumns && view.currentIndex < paletteColumns))) {
            view.currentIndex = -1
            paletteBack.forceActiveFocus()
            return
        }
        if (delta > 0 && view.currentIndex + delta > view.count - 1 && paletteMoreResults) {
            loadMorePalette(view.currentIndex + delta)
            referenceInput.forceActiveFocus()
            return
        }
        // A grid row past the last stops at the last chapter.
        view.currentIndex = Math.max(0, Math.min(view.count - 1, view.currentIndex + delta))
        view.positionViewAtIndex(view.currentIndex, ListView.Contain)
        referenceInput.forceActiveFocus()
    }
    // A catalog key. The Margin is not modal: palettes and dialogs open over
    // its panels and return to them, and the reading toggles act with it open.
    function runKey(action) {
        selectionBarWanted = false
        // From a note or the Desk page a launcher saves the draft first and
        // opens beside the Margin, which stays; closing it returns to the
        // page. A draft that cannot be saved keeps its error, and nothing opens.
        if (noteEditorTyping() && noteEditorLaunchActions.includes(action)) {
            if (!actionAvailable(action)) { rejected(unavailableReason(action)); return }
            if (!flushNote()) { rejected("The note could not be saved"); return }
        }
        runCommand(action)
    }
    function runCommand(action) {
        if (!actionAvailable(action)) { rejected(unavailableReason(action)); return }
        if (action === "first" || action === "last") { countedVerse(action === "last"); return }
        const count = Math.max(1, Math.min(999999, Number(verseCount) || 1))
        clearCount()
        // Text size never touches a Notes draft, so typing in Notes keeps going.
        if (!textSizeActions.includes(action) && !flushNote()) return
        // A palette, dialog, or reading toggle first closes the open side panel.
        const catalogAction = actions.find(function(entry) { return entry.id === action })
        const previousSetting = catalogAction.setting ? bible[catalogAction.setting] : null
        const previousLineHeight = bible.lineHeight
        const previousReaderSize = readerSize
        switch (action) {
        case "contextMenu": openPassageMenuAtCursor(); break
        case "newWindow": if (!modalOpen) newWindowRequested(); break
        case "books": showPaletteMode("passage"); break
        case "bibleSearch": showPaletteMode("search"); break
        case "searchScope": toggleSearchScope(); break
        case "searchRegex": toggleSearchRegex(); break
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
        case "noteLinkSuggest": startNoteLink(); break
        case "openNoteLink": if (deskOpen) openDeskLinkAtCursor(); else openNoteLinkAtCursor(); break
        case "notePreview": if (deskOpen) toggleDeskPreview(); else toggleNotePreview(); break
        case "noteKeepDraft": if (deskOpen) { keepDeskDraft(); deskEditor.forceActiveFocus() } else { keepNoteDraft(); noteEditor.forceActiveFocus() } break
        case "noteUseSaved": if (deskOpen) { useSavedDesk(); deskEditor.forceActiveFocus() } else { useSavedNote(); noteEditor.forceActiveFocus() } break
        case "noteLinkedFrom": if (linkedList.activeFocus) noteEditor.forceActiveFocus(); else focusLinkedFrom(); break
        case "larger": bible.setFontSize(readerSize + 1); break
        case "smaller": bible.setFontSize(readerSize - 1); break
        case "resetText": bible.setFontSize(18); break
        case "numbers": bible.setShowVerseNumbers(!bible.showVerseNumbers); break
        case "headings": bible.setShowHeadings(!bible.showHeadings); break
        case "wordsOfJesus":
            // A version without \wj marks: say so; the setting stays as it is.
            if (!bible.hasWordsOfJesus) {
                notify("Words of Jesus are not marked in " + (bible.versionAbbreviation || "this version"), true)
                sounds.play("error")
                break
            }
            bible.setWordsOfJesus(!bible.wordsOfJesus)
            break
        case "chapterOutline": bible.setChapterOutline(!bible.chapterOutline); break
        case "marginNotes": bible.setMarginNotes(!bible.marginNotes); break
        case "previousHeading": headingStep(-1, count); break
        case "nextHeading": headingStep(1, count); break
        case "readingMode": {
            const modes = ["paragraph", "verse", "block"]
            const previousMode = bible.readingMode
            bible.setReadingMode(modes[(modes.indexOf(bible.readingMode) + 1) % modes.length])
            if (bible.readingMode !== previousMode) notify("Layout " + readingModeName(bible.readingMode), true)
            break
        }
        case "defaultLayout":
            bible.setReadingMode("paragraph")
            notify("Layout Paragraph, the default", true)
            break
        case "focus": bible.setFocusMode(!bible.focusMode); break
        case "lineLess": bible.setLineHeight(Math.round((bible.lineHeight - 0.1) * 10) / 10); break
        case "lineMore": bible.setLineHeight(Math.round((bible.lineHeight + 0.1) * 10) / 10); break
        case "lineReset": bible.setLineHeight(1.5); break
        case "settings": settingsDialog.open(); break
        case "annotations": if (sidePanel === "annotations" || leaveSidePanel("annotations")) openMarks(); break
        case "notes": if (sidePanel === "notes" || leaveSidePanel("notes")) openNotes(true); break
        case "desk": if (sidePanel === "desk" || leaveSidePanel("desk")) openDesk(); break
        case "toggleDesk": addPassageToDesk(); break
        case "cleanDesk": requestCleanDesk(); break
        case "highlight": highlightPassage(); break
        case "undo": undoChange(true); break
        case "redo": undoChange(false); break
        case "tags": openPassageTags(); break
        case "manageTags": if (tagsUi.pickerVisible) tagsUi.togglePage(); else openManageTags(); break
        case "related":
            // Study needs a passage; after opening a chapter nothing is selected yet,
            // so study the first verse, as entering Visual mode does.
            if (!hasSelection && bible.ready && bible.verses.length) {
                bible.selectVerses(bible.verses[0].number, bible.verses[0].number)
                selectionAnchor = bible.cursorVerse
            }
            if (sidePanel === "related" || leaveSidePanel("related")) tagsUi.openRelated()
            break
        case "history": openHistoryPalette(); break
        case "historyBack": historyStep(-count); break
        case "historyForward": historyStep(count); break
        case "historySwap": historySwap(); break
        case "switchPane": switchPane(); break
        case "placeRibbon": placeRibbon(); break
        case "ribbon": openRibbon(); break
        case "removeRibbon": removeRibbon(); break
        case "clearHistory": requestClearHistory(); break
        case "unfollow": unfollowRequested(); notify("Stopped following the window beside", true); break
        case "toggleHistory": toggleHistoryEnabled(); break
        case "copy": if (bible.ready) copyPassage(); break
        case "copyPlain": if (bible.ready) copyPassage(true); break
        case "export": exportDialog.open(); break
        case "backup": backupScope.open(); break
        case "restore": restoreDialog.open(); break
        case "import": importGuide.open(); break
        case "switchVersion": openVersions(false); break
        case "openBeside": openBesideVersions(); break
        case "compare": openCompareCard(); break
        case "peek": peekVerseFootnotes(); break
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
            notify(sounds.reduceMotion ? "Animations off" : desktop.animations ? "Animations on"
                : "Animations on · they follow Hyprland's, which are off", true)
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
        clearKeybindingHeader()
    }
    function keybindingEdge(last) {
        let row = last ? keybindingResults.count - 1 : 0
        keybindingResults.currentIndex = keybindingResults.count ? row : -1
        if (keybindingResults.count) keybindingResults.positionViewAtIndex(row, ListView.Contain)
        clearKeybindingHeader()
    }
    // The sticky group header covers the list's top: a current row under it
    // scrolls down clear of it.
    function clearKeybindingHeader() {
        const view = keybindingResults, item = view.currentItem
        if (!item) return
        const covered = view.contentY + keybindingHeaderHeight - item.y
        if (covered > 0 && view.currentIndex > 0) view.contentY = Math.max(view.originY, view.contentY - covered)
    }
    readonly property real keybindingHeaderHeight: 26 * scale
    // The Keybindings card's essentials: the keys to start with, each
    // clickable where it acts (moves are shown, not clickable).
    function essentialParts() {
        const label = { nextVerse: "verses", nextChapter: "chapters", books: "go to", bibleSearch: "search", notes: "note",
            highlight: "highlight", tags: "tag", toggleDesk: "Desk", copy: "copy", visual: "select verses", focus: "focus", commands: "commands" }
        const keys = function(id) { return actionKeys(id)[0] }
        return [[[keys("nextVerse"), keys("previousVerse")], label.nextVerse], [[keys("previousChapter"), keys("nextChapter")], label.nextChapter]]
            .concat(["books", "bibleSearch", "notes", "highlight", "tags", "toggleDesk", "copy", "visual", "focus", "commands"].map(function(id) {
                return [keys(id), label[id], function() { app.activateKeybinding(app.actionById(id)) }, actionAvailable(id)]
            }))
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
        // A page showing the whole first (last) row takes it, so pgdn then
        // pgup returns to the top whatever the row heights.
        const edge = view.itemAtIndex(direction < 0 ? 0 : view.count - 1)
        if (edge && edge.y >= top - 1 && edge.y + edge.height <= top + view.height + 1)
            row = direction < 0 ? 0 : view.count - 1
        view.currentIndex = row
        view.contentY = top
    }
    function activateKeybinding(action) {
        if (!action || !actionAvailable(action)) { rejected(unavailableReason(action)); return }
        if (!flushNote()) return
        helpDialog.pendingAction = action.id
        helpDialog.close()
    }

    // The layout icons (footer layout button, Reading settings' Layout
    // choices), drawn on a 24×24 canvas path already begun.
    function focusFirstSetting() {
        for (let i = 0; i < settingSwitches.count; ++i) {
            const control = settingSwitches.itemAt(i)
            if (control && control.enabled && control.visible) { control.forceActiveFocus(); return }
        }
        lineHeightSlider.forceActiveFocus()
    }
    function paintLayoutGlyph(ctx, mode) {
        if (mode === "verse") {
            // Verse: separate short rows, each with a small number tick.
            for (const y of [5, 12, 19]) {
                ctx.moveTo(3, y - 1.5); ctx.lineTo(3, y + 1.5)
                ctx.moveTo(8, y); ctx.lineTo(y === 12 ? 17 : 21, y)
            }
        } else if (mode === "paragraph") {
            // Paragraph: two text blocks, each first line indented.
            ctx.moveTo(8, 3); ctx.lineTo(21, 3)
            ctx.moveTo(3, 7); ctx.lineTo(21, 7)
            ctx.moveTo(3, 11); ctx.lineTo(14, 11)
            ctx.moveTo(8, 17); ctx.lineTo(21, 17)
            ctx.moveTo(3, 21); ctx.lineTo(17, 21)
        } else if (mode === "block") {
            // Block: one solid run of lines.
            for (const y of [4, 8.5, 13, 17.5]) { ctx.moveTo(3, y); ctx.lineTo(21, y) }
            ctx.moveTo(3, 22); ctx.lineTo(12, 22)
        }
    }
    // Controls in their own files, bound to this window and the live theme
    // under the same names, so a use here needs neither.
    component QuietButton: Shared.QuietButton { controller: app; theme: desktop }
    component FineRule: Rule { theme: desktop }
    // Lists reserve the scrollbar's width while it shows, so rows (and their
    // right-hand labels, such as "here") never sit under it.
    function scrollGutter(list) {
        const bar = list ? list.ScrollBar.vertical : null
        return bar && bar.visible ? bar.width : 0
    }
    component OverlayScrollbar: Shared.OverlayScrollbar { controller: app; theme: desktop }
    component OverlayScrollView: Shared.OverlayScrollView { controller: app; theme: desktop }
    component KeyScrollView: Shared.KeyScrollView { controller: app; theme: desktop }
    component SettingSwitch: Shared.SettingSwitch { controller: app; theme: desktop; backend: bible }
    component SquareSlider: Shared.SquareSlider { controller: app; theme: desktop }
    component FloatingButton: Shared.FloatingButton { controller: app; theme: desktop }
    component NoteGlyph: AnnotationGlyph {
        kind: "note"
        implicitWidth: 14 * app.scale
        implicitHeight: 16 * app.scale
        width: implicitWidth; height: implicitHeight
        ink: desktop.accent
    }
    component RangeMarker: Shared.RangeMarker { controller: app; theme: desktop; backend: bible; rangeMenu: verseTagMenu }
    component SectionLabel: SectionTitle { theme: desktop; uiScale: app.scale }
    component SidePanelHeader: Shared.SidePanelHeader { controller: app; theme: desktop }
    component SidePanel: Shared.SidePanel { controller: app; theme: desktop }
    component ModalCard: Shared.ModalCard { controller: app; theme: desktop }

    ColumnLayout {
        anchors.fill: parent
        anchors.rightMargin: app.notesDock
        spacing: 0
        // Character selection (v): the mode, its keys, and what is selected.
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
            color: app.tint(desktop.error, 0.12)
            RowLayout {
                id: errorRow
                anchors.fill: parent
                anchors.margins: 10
                anchors.leftMargin: 24
                spacing: 16
                Label { objectName: "errorMessage"; text: bible.error; color: desktop.foreground; wrapMode: Text.Wrap; Layout.fillWidth: true; font.pixelSize: 13 * app.scale; Accessible.role: Accessible.AlertMessage }
                QuietButton { objectName: "errorDismiss"; text: "Dismiss"; compact: true; focusTarget: readerSurface; keys: KeyNotation.format("Escape"); tip: "Dismiss · " + keys; onClicked: bible.clearError() }
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

                // Previous / next chapter at the outer edges (chapterNavFits).
                Repeater {
                    model: [{ id: "previousChapter", glyph: "chevron-left", name: "chapterPreviousButton", delta: -1 },
                            { id: "nextChapter", glyph: "chevron-right", name: "chapterNextButton", delta: 1 }]
                    delegate: Item {
                        id: chapterNav
                        required property var modelData
                        readonly property bool before: modelData.delta < 0
                        objectName: modelData.name + "Zone"
                        z: 5
                        width: app.chapterNavReserve
                        height: Math.max(app.chapterNavSize * 3, 120 * app.controlScale)
                        x: before ? 0 : readerSurface.width - width - app.chapterNavScrollbar
                        y: (readerSurface.height - app.floatingSafety - height) / 2 + app.chapterNavShift
                        visible: app.chapterNavFits && !app.findPromptOpen
                        // Focus mode: hidden until the pointer is here.
                        readonly property bool revealed: !bible.focusMode || zone.containsMouse || chapterButton.hovered
                        onRevealedChanged: if (revealed && bible.focusMode) guard.restart()
                        Timer { id: guard; interval: 300 }
                        MouseArea {
                            id: zone
                            anchors.fill: parent
                            acceptedButtons: Qt.NoButton
                            hoverEnabled: true
                            onWheel: function(wheel) { wheel.accepted = false }
                        }
                        FloatingButton {
                            id: chapterButton
                            objectName: chapterNav.modelData.name
                            glyph: chapterNav.modelData.glyph
                            persistent: true
                            focusPolicy: Qt.NoFocus
                            x: chapterNav.before ? app.chapterNavEdge : chapterNav.width - width - app.chapterNavEdge
                            anchors.verticalCenter: parent.verticalCenter
                            width: app.chapterNavSize
                            height: app.chapterNavSize
                            readonly property bool atEnd: chapterNav.before ? app.chapterBounds.first : app.chapterBounds.last
                            available: chapterNav.revealed && !atEnd && !bible.busy && !app.modalOpen
                            opacity: chapterNav.revealed ? 1 : 0
                            // The destination passage and its key; at the Bible's bounds, where it is.
                            readonly property var destination: chapterNav.before ? app.chapterNeighbors.previous : app.chapterNeighbors.next
                            label: destination ? destination.title : chapterNav.before ? "First chapter" : "Last chapter"
                            keys: destination ? app.keyText(chapterNav.modelData.id) : ""
                            onClicked: { app.chapterStep(chapterNav.modelData.delta); app.readerFocus() }
                            MouseArea {
                                anchors.fill: parent
                                z: 10
                                visible: guard.running
                                acceptedButtons: Qt.AllButtons
                            }
                        }
                    }
                }
                Flickable {
                    id: reading
                    objectName: "reading"
                    anchors.fill: parent
                    anchors.bottomMargin: app.textPopoverReserve
                    clip: true
                    contentWidth: width
                    contentHeight: readerColumn.height
                    boundsBehavior: Flickable.StopAtBounds
                    flickableDirection: Flickable.VerticalFlick
                    onWidthChanged: {
                        // A dock change glides (glideFromDock); any other resize jumps.
                        if (isNaN(app.dockSlideFrom)) app.settleMargins()
                        else Qt.callLater(app.endDockGlide)
                        if (app.hasSelection)
                            Qt.callLater(function() { app.scrollToVerse(bible.cursorVerse, true) })
                    }
                    onContentYChanged: { positionSaveTimer.restart(); if (app.following && !app.followApplying) followTimer.restart() }
                    onMovementStarted: readerSlide.stop()
                    // ctrl + wheel and a pinch change the Text size, as ctrl = and ctrl - do.
                    WheelHandler {
                        objectName: "readerZoomWheel"
                        acceptedModifiers: Qt.ControlModifier
                        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                        onWheel: function(event) {
                            app.zoomSteps += event.angleDelta.y / 120
                            const steps = app.zoomSteps > 0 ? Math.floor(app.zoomSteps) : Math.ceil(app.zoomSteps)
                            if (!steps) return
                            app.zoomSteps -= steps
                            for (let i = 0; i < Math.abs(steps); ++i) app.runCommand(steps > 0 ? "larger" : "smaller")
                        }
                    }
                    PinchHandler {
                        objectName: "readerPinch"
                        target: null
                        property real startScale: 1
                        property int startSize: 18
                        onActiveChanged: if (active) { startScale = activeScale; startSize = app.readerSize }
                        onActiveScaleChanged: if (active) bible.setFontSize(Math.max(8, Math.min(40, Math.round(startSize * activeScale / startScale))))
                    }
                    ScrollBar.vertical: OverlayScrollbar {
                        id: readerScrollbar
                        objectName: "readerScrollbar"
                        parent: readerSurface
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        anchors.topMargin: 12 * app.readerScale
                        anchors.bottomMargin: app.floatingSafety + 12 * app.readerScale
                        visible: !app.focusLayout && size < 1
                        enabled: visible
                    }

                    // Right click on the reader's empty places (gutters, headings,
                    // the chapter title, below the text, through the outline): the
                    // passage menu at the pointer, for the current selection (else
                    // the cursor verse). Under the text, whose rows take their own.
                    MouseArea {
                        objectName: "readerBackgroundMenu"
                        width: reading.width
                        height: Math.max(reading.contentHeight, reading.height)
                        acceptedButtons: Qt.RightButton
                        onClicked: function(mouse) {
                            const p = mapToItem(null, mouse.x, mouse.y)
                            app.openPassageMenuAt(p.x, p.y)
                        }
                    }
                    // The mouse selection's bar, scrolling with the text above
                    // the selection's first line (below it at the top).
                    // Its actions are buttons the bar's full height, edge to edge.
                    Rectangle {
                        id: selectionBar
                        objectName: "selectionBar"
                        visible: app.selectionBarShown
                        readonly property real gap: 6 * app.readerScale
                        width: selectionBarHints.implicitWidth + 2 * border.width
                        height: selectionBarHints.implicitHeight + 2 * border.width
                        x: Math.max(0, Math.min(reading.width - width, app.selectionBarX))
                        y: app.selectionBarY - height - gap < reading.contentY ? app.selectionBarY + app.readerLineHeight + gap : app.selectionBarY - height - gap
                        z: 5
                        color: desktop.background
                        border.color: desktop.border
                        border.width: 1
                        radius: Math.min(desktop.cornerRadius, width / 2, height / 2)
                        HintRow {
                            id: selectionBarHints
                            objectName: "selectionBarHints"
                            x: parent.border.width; y: parent.border.width
                            theme: desktop
                            parts: app.selectionBarParts()
                            pixelSize: 12 * app.scale
                            partHeight: 30 * app.scale
                            partPadding: 10 * app.scale
                        }
                    }
                    // In the ribbon's chapter an accent ribbon hangs from the
                    // top of the view, just right of the column, wherever the
                    // chapter is scrolled; a click returns to its place (r).
                    Canvas {
                        id: ribbonTab
                        objectName: "ribbonTab"
                        visible: app.ribbonHere
                        opacity: app.focusDimmedOpacity
                        width: 12 * app.readerScale
                        height: 34 * app.readerScale
                        x: readerColumn.x + readerColumn.width + 8 * app.readerScale
                        // Placing it (shift R) lifts it out of the top and drops it
                        // back in with a little settle (ribbonDrop).
                        property real drop: 0
                        y: reading.contentY + drop
                        readonly property color fill: desktop.accent
                        onFillChanged: requestPaint()
                        onWidthChanged: requestPaint()
                        onHeightChanged: requestPaint()
                        onPaint: {
                            const ctx = getContext("2d")
                            ctx.reset()
                            ctx.fillStyle = fill
                            ctx.beginPath()
                            ctx.moveTo(0, 0); ctx.lineTo(width, 0); ctx.lineTo(width, height)
                            ctx.lineTo(width / 2, height - width / 2); ctx.lineTo(0, height)
                            ctx.closePath()
                            ctx.fill()
                        }
                        MouseArea {
                            id: ribbonTabMouse
                            objectName: "ribbonTabMouse"
                            anchors.fill: parent
                            anchors.margins: -6 * app.readerScale
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            onClicked: function(mouse) {
                                if (mouse.button === Qt.RightButton) {
                                    const p = ribbonTabMouse.mapToItem(null, mouse.x, mouse.y)
                                    app.openRibbonMenu(p.x, p.y, true)
                                } else app.runCommand("ribbon")
                            }
                        }
                        AppToolTip {
                            theme: desktop
                            uiScale: app.scale
                            visible: ribbonTabMouse.containsMouse
                            text: "Ribbon · " + (bible.ribbon.reference || "") + " · " + app.keyText("ribbon") + " returns here, "
                                + app.keyText("placeRibbon") + " moves it, " + app.keyText("removeRibbon") + " removes it"
                        }
                    }
                    Column {
                        id: readerColumn
                        objectName: "readerColumn"
                        // Centered, but clear of the footer's stacked right column;
                        // margin notes may shift it left (app.marginSpace).
                        width: app.readerColumnWidth
                        x: app.columnTargetX + app.marginSlideOffset
                        onHeightChanged: { app.scheduleMargins(); if (app.selectionBarWanted) Qt.callLater(app.placeSelectionBar) }
                        onWidthChanged: { app.scheduleMargins(); if (app.selectionBarWanted) Qt.callLater(app.placeSelectionBar) }
                        topPadding: 48 * app.readerScale
                        bottomPadding: app.floatingSafety + 3 * app.readerLineHeight
                        spacing: 0

                        Label {
                            objectName: "chapterTitle"
                            opacity: app.focusDimmedOpacity
                            text: bible.passage
                            // Hover: the accent, as a link (a click opens the chapter list).
                            color: chapterTitleMouse.containsMouse ? desktop.accent : desktop.foreground
                            // A step of the reader's type scale.
                            font.pixelSize: TypeScale.size(app.readerBodySize, TypeScale.title)
                            font.weight: Font.Medium
                            font.letterSpacing: -0.9 * app.fontScale
                            leftPadding: app.verseGutter
                            bottomPadding: 24 * app.readerScale * app.fontScale
                            width: parent.width
                            wrapMode: Text.Wrap
                            // Click: the Passage palette at this book's chapter list.
                            MouseArea {
                                id: chapterTitleMouse
                                objectName: "chapterTitleMouse"
                                x: parent.leftPadding
                                width: Math.min(parent.width - x, parent.contentWidth)
                                height: parent.height - parent.bottomPadding
                                enabled: bible.ready
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: app.openChapterList()
                            }
                            AppToolTip {
                                theme: desktop
                                uiScale: app.scale
                                visible: chapterTitleMouse.containsMouse && !bible.focusMode
                                text: "Choose another chapter · " + app.menuKey("books") + " for any passage"
                            }
                        }
                        Repeater {
                            id: verseRepeater
                            model: app.continuousReader ? [] : app.verseRows
                            delegate: VerseItem {
                                id: verseRow
                                required property int index
                                // Read in place: a change to marks updates the row rather than making it again.
                                // (A row of a longer chapter reads nothing for the moment before it goes.)
                                readonly property var modelData: bible.verses[index] || app.noVerse
                                readonly property bool selected: modelData.number >= bible.selectedStart && modelData.number <= bible.selectedEnd && bible.selectedStart > 0
                                readonly property real gutter: app.verseGutter
                                // What a change of marks leaves alone, as values (a string or a
                                // number changes only when its value does): the text, the number,
                                // and the lists rows make from.
                                readonly property int verseNumber: modelData.number || 0
                                readonly property string bodyHtml: '<div style="line-height:' + (bible.lineHeight * 100) + '%">'
                                    + (app.footnoteHtml(app.verseHtml(modelData.html), modelData.footnotes) || String(modelData.text || "").replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/\n/g, "<br>")) + '</div>'
                                readonly property string highlightKey: JSON.stringify(modelData.highlightRanges || [])
                                readonly property string footnoteKey: JSON.stringify(modelData.footnotes || [])
                                body: verseBody
                                searchOverlay: verseSearch
                                textOffset: verseHeading.height
                                textHeight: verseBody.height
                                width: readerColumn.width
                                height: verseHeading.height + verseBody.height + 7 * app.readerScale
                                // Focus-mode dimming: paint-only, so layout and hit-testing are unchanged.
                                readonly property bool dimmed: app.verseDimmed(modelData.number)
                                opacity: modelData.number < app.focusFirst || modelData.number > app.focusLast ? app.focusDimmedOpacity : 1
                                Item {
                                    id: verseHeading
                                    objectName: "verseHeading" + verseRow.modelData.number
                                    readonly property bool hasHeading: bible.showHeadings && !app.focusLayout && (verseRow.modelData.heading || "").length > 0
                                    width: parent.width - verseRow.gutter - 6
                                    x: verseRow.gutter
                                    // Measure text at a fixed width, independent of effective
                                    // visibility. First import creates rows inside a hidden
                                    // reader; Label height/implicitHeight/padding feedback
                                    // otherwise re-enters when that ancestor becomes visible.
                                    height: hasHeading ? headingText.implicitHeight + headingText.y + TypeScale.size(app.readerBodySize, TypeScale.headingBelow) : 0
                                    visible: hasHeading
                                    Text {
                                        id: headingText
                                        objectName: "headingText" + verseRow.modelData.number
                                        width: parent.width
                                        // Under the chapter title, its own space suffices,
                                        // as in the other layouts (a document's first block
                                        // takes no top margin).
                                        y: verseRow.index === 0 ? 0 : TypeScale.size(app.readerBodySize, TypeScale.headingAbove)
                                        // An LRM keeps a Hebrew-first heading ("א Aleph")
                                        // in an LTR paragraph; display only.
                                        text: app.leftToRightText(verseRow.modelData.heading)
                                        horizontalAlignment: Text.AlignLeft
                                        textFormat: Text.PlainText
                                        font.family: desktop.fontFamily
                                        font.pixelSize: TypeScale.size(app.readerBodySize, TypeScale.heading)
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
                                    model: JSON.parse(verseRow.highlightKey)
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
                                        Canvas {
                                            id: highlightPaint
                                            objectName: "highlightPaint" + verseRow.modelData.number + ":" + parent.modelData.reference
                                            anchors.fill: parent
                                            readonly property var range: parent.modelData
                                            property color ink: app.highlightInk
                                            readonly property var paintKey: [app.readerScale, width, height, verseBody.width, verseBody.height, verseBody.y,
                                                app.highlightBand, app.highlightAlpha, range.reference, bible.versionId]
                                            onPaintKeyChanged: requestPaint()
                                            onInkChanged: requestPaint()
                                            Connections { target: app; function onHighlightsRepaint() { highlightPaint.requestPaint() } }
                                            onPaint: {
                                                const ctx = getContext("2d")
                                                ctx.reset()
                                                const k = app.readerScale
                                                const s = HandDrawn.seed("highlight", bible.versionId, range.reference, verseRow.modelData.number)
                                                const band = app.highlightBand
                                                const lines = verseRow.searchOverlay ? verseRow.searchOverlay.lineRects(0, String(verseRow.modelData.text || "").length) : []
                                                for (let i = 0; i < lines.length; ++i) {
                                                    const r = lines[i]
                                                    const p = verseBody.mapToItem(highlightPaint, r.x, r.y)
                                                    HandDrawn.highlighterStroke(ctx, Qt.rect(p.x, p.y + band.top, r.width, band.height),
                                                        ink, app.highlightAlpha, HandDrawn.seed(s, i), k)
                                                }
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
                                    Accessible.name: "Verse " + verseRow.modelData.number + (verseRow.modelData.hasNote ? ", has note" : "") + (verseRow.modelData.hasTags ? ", tagged" : "")
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
                                        font.pixelSize: TypeScale.size(app.readerBodySize, TypeScale.number)
                                        font.weight: verseRow.selected ? Font.Bold : Font.Normal
                                        color: verseRow.selected ? desktop.accent : desktop.muted
                                        FontMetrics { id: numberMetrics; font: numberLabel.font }
                                    }
                                    MouseArea {
                                        id: numberMouse
                                        anchors.fill: parent
                                        // Hidden numbers leave a blank gutter: no hand, no tooltip there.
                                        cursorShape: numberLabel.visible ? Qt.PointingHandCursor : Qt.ArrowCursor
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
                                        visible: numberMouse.containsMouse && numberLabel.visible
                                        text: "Select verse " + verseRow.modelData.number + " · " + KeyNotation.format("Shift+Click") + " extends · " + KeyNotation.format("MiddleClick") + " copies" + (verseRow.modelData.hasNote ? "\nHas a note" : "") + (verseRow.modelData.hasTags ? "\nTagged" : "")
                                    }
                                }
                                Repeater {
                                    model: app.focusLayout ? [] : JSON.parse(verseRow.footnoteKey)
                                    delegate: Item {
                                        id: footnoteMarker
                                        required property var modelData
                                        objectName: "verseFootnoteMark:" + verseRow.modelData.number + "." + modelData.seq
                                        // When the chapter or version changes, a row reads its new
                                        // verse a moment before its old markers go: their cells
                                        // may lie past the new text, so they take no place.
                                        readonly property int at: app.footnoteCellPosition(verseRow.modelData.footnotes, modelData)
                                        readonly property rect cell: verseBody.width > 0 && verseBody.contentHeight > 0 && at <= verseBody.length
                                            ? verseBody.positionToRectangle(at) : Qt.rect(0, 0, 0, 0)
                                        x: verseBody.x + cell.x
                                        y: verseBody.y + cell.y
                                        width: app.footnoteCellWidth
                                        height: cell.height
                                        z: 3
                                        // Out of the tab order: i peeks at the cursor verse's notes.
                                        activeFocusOnTab: false
                                        Text {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            text: app.footnoteMark(footnoteMarker.modelData.caller)
                                            color: desktop.accent
                                            font.family: desktop.fontFamily
                                            font.pixelSize: Math.max(6, TypeScale.size(app.readerBodySize, TypeScale.mark))
                                            textFormat: Text.PlainText
                                        }
                                        MouseArea {
                                            id: footnoteMouse
                                            objectName: "verseFootnoteHit:" + verseRow.modelData.number + "." + footnoteMarker.modelData.seq
                                            // A taller, invisible target than the small mark (layout unchanged):
                                            // above and below only, never over the neighboring words.
                                            anchors.fill: parent
                                            anchors.topMargin: -app.markerHitMargin
                                            anchors.bottomMargin: -app.markerHitMargin
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: {
                                                const p = footnoteMarker.mapToItem(null, 0, 0)
                                                app.peekFootnote(verseRow.modelData.number, footnoteMarker.modelData.seq, p.x, p.y, footnoteMarker.height)
                                            }
                                        }
                                        Accessible.role: Accessible.Button
                                        Accessible.name: "Footnote: " + footnoteMarker.modelData.text
                                        AppToolTip {
                                            theme: desktop
                                            uiScale: app.scale
                                            visible: footnoteMouse.containsMouse
                                            text: "Footnote · " + footnoteMarker.modelData.text + "\n" + app.footnoteHint
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
                                    lineIndents: verseRow.modelData.indents || []
                                    indentUnit: app.readerBodySize * TypeScale.poetryIndent
                                    theme: desktop
                                    segments: app.searchSegments.filter(function(segment) { return segment.verse === verseRow.verseNumber })
                                }
                                TextEdit {
                                    id: verseBody
                                    property SearchHighlights searchOverlay: verseSearch
                                    objectName: "verseBody" + verseRow.modelData.number
                                    x: verseRow.gutter
                                    y: verseHeading.height
                                    width: parent.width - x - 9
                                    height: contentHeight
                                    text: verseRow.bodyHtml
                                    textFormat: Text.RichText
                                    horizontalAlignment: TextEdit.AlignLeft
                                    font.family: desktop.fontFamily
                                    font.pixelSize: app.readerSize * app.readerScale
                                    color: desktop.foreground
                                    selectionColor: desktop.selection
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
                                        onDoubleClicked: function(mouse) { if (mouse.button === Qt.LeftButton) app.selectWordAt(verseRow.index, bodyMouse, mouse) }
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
                                onLayoutRefreshed: { app.scheduleMargins(); app.placeSelectionBar() }
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
                        x: app.marginDrift
                        width: reading.width
                        height: readerColumn.height
                        visible: app.marginSpace.notes.visible
                        opacity: app.marginNotesLevel * app.marginFocusLevel
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
                                    // Right click: the same menu as a note marker (Open, Move here, Delete note, Copy, Desk).
                                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                                    onClicked: function(mouse) {
                                        if (mouse.button === Qt.RightButton) {
                                            const p = mapToItem(null, mouse.x, mouse.y)
                                            app.openMarginNoteMenu(marginNote.modelData, p.x, p.y)
                                        } else app.openMarginNote(marginNote.modelData)
                                    }
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
                                    text: "Note on " + marginNote.modelData.reference + "\nClick: open it in Notes · " + app.menuKey("notes") + " on the selected passage"
                                        + (noteText.text.indexOf("<a ") >= 0 ? "\nLinks: " + app.menuKey("openNoteLink") + " opens the one at the cursor in Notes" : "")
                                }
                                // "{" before the note, "}" after the passage text, and an
                                // arrow between their tips (HandDrawn.js, seeded per note).
                                // The column's text never runs under it: gliding into place
                                // beside margins that wait there, the column uncovers it.
                                Item {
                                    id: connectorClip
                                    readonly property real cover: Math.min(app.marginCover, connector.width)
                                    clip: cover > 0
                                    x: connector.spanLeft - marginNote.x + cover
                                    y: connector.spanTop - marginNote.y
                                    width: connector.width - cover
                                    height: connector.height
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
                                        x: -connectorClip.cover
                                        width: Math.max(1, spanRight - spanLeft)
                                        height: Math.max(1, spanBottom - spanTop)
                                        readonly property var paintKey: [marginNote.place, width, height, spanLeft, spanTop, bible.versionId]
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
                }
                ChapterOutline {
                    id: chapterOutline
                    objectName: "chapterOutline"
                    visible: app.outlineShown
                    x: app.marginSpace.outline.x + app.marginDrift - (1 - app.outlineLevel) * 24 * app.readerScale
                    opacity: app.marginFocusLevel * app.outlineLevel
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
                    onBoundary: sounds.play("boundary")
                    onStepHeading: function(direction) { app.runCommand(direction > 0 ? "nextHeading" : "previousHeading") }
                    titleTip: "Jump to the chapter top · " + app.menuKey("previousHeading") + " steps back to it"
                    entryTip: "Jump to this section · " + app.menuKey("previousHeading") + " " + app.menuKey("nextHeading") + " step between sections"
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
                // While a side panel has the keyboard the text recedes to 90%,
                // as the panel does while the text has it (SidePanel): a veil
                // of the background rather than the surface's own opacity,
                // which would let the text show through the bottom fade.
                Rectangle {
                    objectName: "readerDim"
                    anchors.fill: parent
                    z: 6 // Over the chapter arrows.
                    color: desktop.background
                    opacity: app.marginOpen && app.marginKeys ? 0.1 : 0
                    visible: opacity > 0
                    Behavior on opacity { NumberAnimation { duration: app.fadeMs } }
                }
            }

            OverlayScrollView {
                id: setupScroll
                objectName: "setupScroll"
                anchors.fill: parent
                visible: !bible.ready && !bible.busy
                // The setup screen starts on Add version (unless a dialog has the keyboard).
                onVisibleChanged: if (visible) Qt.callLater(app.focusSetup)
                Component.onCompleted: if (visible) Qt.callLater(app.focusSetup)
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
                    text: "Add a version to begin. Install from the catalog or import a USFM ZIP.\nRead offline, highlight and tag passages, and keep your notes alongside the text."
                    color: desktop.muted
                    font.pixelSize: 15 * app.scale
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    lineHeight: 1.4
                    Layout.fillWidth: true
                }
                QuietButton { id: setupImport; objectName: "setupImport"; text: "Add version"; emphasized: true; Layout.alignment: Qt.AlignHCenter; Layout.topMargin: 6; onClicked: app.runCommand("import") }
                Label { objectName: "setupPrivacy"; text: "All your Bible versions, highlights, tags, and notes stay on this device."; color: desktop.muted; font.pixelSize: 12 * app.scale; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap; Layout.fillWidth: true }
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
        // Beside the text, never over it: the reader stays clickable, and its
        // keys work whenever the keyboard is outside Notes (marginKeys).
        modal: false
        dim: false
        // Qt hands the keyboard back to a popup that wants focus as another
        // closes: Notes wants it only while it has it.
        focus: app.marginKeys
        interactive: false
        padding: 0
        // Auto-close cannot veto a failed save. All dismissals use closeNotes().
        closePolicy: Popup.NoAutoClose
        // Slide plus a short fade. A closing drawer no longer counts as open
        // (`shown`), so reader keys and a reopen work during its exit animation.
        property bool closing: false
        readonly property bool shown: visible && !closing
        // Enter: the full slide and a fade, ease-out; exit, subtler: a short
        // slide and a quicker fade, ease-in (app.panelEnterMs / panelExitMs).
        enter: Transition {
            NumberAnimation { property: "position"; to: 1; duration: app.panelEnterMs; easing.type: Easing.OutCubic }
            NumberAnimation { property: "opacity"; to: 1; duration: app.panelEnterMs; easing.type: Easing.OutQuad }
        }
        exit: Transition {
            NumberAnimation { property: "position"; to: app.panelExitPosition; duration: app.panelExitMs; easing.type: Easing.InCubic }
            NumberAnimation { property: "opacity"; to: 0; duration: app.panelExitMs; easing.type: Easing.InQuad }
        }
        // While closing, a drawer must not keep its Escape grab: the next Escape
        // belongs to the reader (or range editor) immediately.
        onAboutToShow: { const reopening = visible; closing = false; if (reopening) focusContent() }
        // Its note's saves since opening are one undo step; the next session
        // starts another. A mark's note written in place is done.
        onAboutToHide: { closing = true; bible.closeUndoGroup(); if (app.markEditKey.length) app.finishMarkNote(false); app.restorePanelFocus() }
        // Keyboard focus is available as soon as the drawer starts fading in.
        // Opened only to show a note (focusEditor false), it leaves the
        // keyboard where it is.
        function focusContent() {
            if (app.modalOpen || !focusEditor) return
            if (app.marginMode === "desk") app.focusDesk()
            else if (app.marginMode === "marks") app.focusSavedList()
            else if (app.marginMode === "related") tagsUi.focusResults()
            else if (app.hasSelection) app.focusNoteEnd()
            else notesPanel.forceActiveFocus()
        }
        onVisibleChanged: if (visible && !closing) focusContent()
        background: Rectangle { radius: Math.min(desktop.cornerRadius, width / 2, height / 2); color: desktop.background; border.color: desktop.border }
        onClosed: { closing = false; position = 0; app.marginKeys = false; app.editingPanelClosed(); app.restorePanelFocus() }
        contentItem: Item {
            objectName: "marginContent"
            SidePanel {
                id: notesPanel
                objectName: "notesPanel"
                mode: "notes"
                Keys.onEscapePressed: function(event) { app.closePanel(); event.accepted = true }
                // A press on the panel's chrome brings the keyboard to the editor (Write mode).
                PointHandler {
                    objectName: "notesChromePress"
                    onActiveChanged: if (!active && app.notesWriting) { app.marginKeys = true; noteRefocusTimer.restart() }
                }
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
                    contentHeight: notesContent.height + 2 * notesPanel.inset
                    clip: true
                ColumnLayout {
                    id: notesContent
                    x: notesPanel.inset
                    y: notesPanel.inset
                    width: notesScroll.availableWidth - 2 * notesPanel.inset
                    // Grows past the panel (and scrolls) when a long Linked from list needs it.
                    height: Math.max(notesPanel.height - 2 * notesPanel.inset, (app.noteConflict ? 700 : 440) * app.scale, implicitHeight)
                    spacing: notesPanel.gap
                    SidePanelHeader {
                        objectName: "notesHeader"
                        Layout.fillWidth: true
                        title: "Notes"
                        subtitle: tagsUi.passageSubtitle
                        titleName: "notesTitle"; subtitleName: "notesPassage"; closeName: "notesClose"
                        closeTip: "Close Notes · " + app.escapeText
                        onCloseRequested: app.closeNotes()
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
                                id: noteRetry
                                objectName: "noteRetry"
                                text: "Save my draft"; tip: "Replace the saved note with your draft · " + app.keyText("noteKeepDraft") + " in the note"
                                focusTarget: noteEditor
                                enabled: app.noteScopeMatches && app.hasSelection && !bible.busy
                                onClicked: app.retryNote()
                            }
                            QuietButton {
                                id: noteUseSaved
                                objectName: "noteUseSaved"
                                text: "Use saved text"
                                tip: "Replace your draft with the saved note · " + app.keyText("noteUseSaved") + " in the note"
                                focusTarget: noteEditor
                                onClicked: app.loadSavedNote()
                            }
                        }
                    }
                    RowLayout {
                        visible: app.hasSelection
                        spacing: 4
                        QuietButton { objectName: "writeButton"; text: "Write"; compact: true; emphasized: !app.notePreview; keys: app.keyText("notePreview"); tip: "Write · " + keys; onClicked: { app.notePreview = false; noteEditor.forceActiveFocus() } }
                        QuietButton { objectName: "previewButton"; text: "Preview"; compact: true; emphasized: app.notePreview; keys: app.keyText("notePreview"); tip: "Preview · " + keys; onClicked: { if (app.flushNote()) { app.notePreview = true; notePreviewText.forceActiveFocus() } } }
                        Item { Layout.fillWidth: true }
                        Label { text: "Markdown"; color: desktop.muted; font.pixelSize: 11 * app.scale }
                    }
                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        // The editor keeps a usable height however long Linked from grows.
                        Layout.minimumHeight: 160 * app.scale
                        Layout.preferredHeight: 160 * app.scale
                        OverlayScrollView {
                            id: noteEditorScroll
                            objectName: "noteEditorScroll"
                            anchors.fill: parent
                            visible: app.hasSelection && !app.notePreview
                            clip: true
                            contentWidth: availableWidth
                            TextArea {
                                id: noteEditor
                                ContextMenu.menu: null
                                ContextMenu.onRequested: function(position) { app.openTextMenu(noteEditor, position) }
                                objectName: "noteEditor"
                                width: parent.width
                                color: desktop.foreground
                                selectionColor: desktop.selection
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
                                    if (!app.syncingNote) {
                                        if (app.noteSaveDelay > 0) noteSaveTimer.restart()
                                        else app.flushNote()
                                    }
                                    app.updateLinkSuggestions()
                                }
                                onCursorPositionChanged: app.updateLinkSuggestions()
                                Keys.onPressed: function(event) {
                                    const enter = event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                                    // Tab types a tab (it accepts a [[ suggestion instead, below);
                                    // shift tab takes one back. The editor keeps the keyboard.
                                    if (event.key === Qt.Key_Tab && event.modifiers === Qt.NoModifier && !(app.linkSuggesting && app.linkSuggestions.length)) {
                                        event.accepted = true
                                        noteEditor.remove(noteEditor.selectionStart, noteEditor.selectionEnd)
                                        noteEditor.insert(noteEditor.cursorPosition, "\t")
                                        return
                                    }
                                    if (event.key === Qt.Key_Backtab || (event.key === Qt.Key_Tab && event.modifiers === Qt.ShiftModifier)) {
                                        event.accepted = true
                                        const at = noteEditor.cursorPosition
                                        if (at > 0 && noteEditor.getText(at - 1, at) === "\t") noteEditor.remove(at - 1, at)
                                        return
                                    }
                                    // ctrl ↓: the Linked from list (esc comes back).
                                    if (event.key === Qt.Key_Down && event.modifiers === Qt.ControlModifier && app.linkedFrom.length) {
                                        event.accepted = true
                                        app.focusLinkedFrom()
                                        return
                                    }
                                    // A conflict with another window: ctrl s keeps this draft, ctrl shift S takes the saved text.
                                    if (event.key === Qt.Key_S && (event.modifiers === Qt.ControlModifier || event.modifiers === (Qt.ControlModifier | Qt.ShiftModifier))) {
                                        event.accepted = true
                                        if (event.modifiers & Qt.ShiftModifier) app.useSavedNote(); else app.keepNoteDraft()
                                        return
                                    }
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
                                    else app.closePanel()
                                    event.accepted = true
                                }
                                Accessible.name: "Markdown note for " + bible.selectionReference
                                // ctrl click: open the passage (or web) link clicked, as alt enter does at the cursor.
                                TapHandler {
                                    objectName: "noteEditorLinkClick"
                                    acceptedButtons: Qt.LeftButton
                                    acceptedModifiers: Qt.ControlModifier
                                    onTapped: function(point) {
                                        noteEditor.cursorPosition = noteEditor.positionAt(point.position.x, point.position.y)
                                        if (app.noteLinkAtCursor || app.noteUrlAtCursor.length) app.openNoteLinkAtCursor()
                                    }
                                }
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
                                ContextMenu.menu: null
                                ContextMenu.onRequested: function(position) { app.openTextMenu(notePreviewText, position) }
                                objectName: "notePreview"
                                width: parent.width
                                // Focusable for reading by key: the scroll keys move the preview.
                                activeFocusOnTab: true
                                Keys.onPressed: function(event) { app.scrollKey(notePreviewScroll.contentItem as Flickable, event) }
                                text: noteEditor.text.length ? app.previewMarkdown(noteEditor.text) : "*Your note preview will appear here.*"
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
                                QuoteBars { objectName: "noteQuoteBars"; target: notePreviewText; color: app.quoteBarColor; uiScale: app.scale }
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
                        visible: !app.marginKeys || noteEditor.activeFocus || notePreviewText.activeFocus
                        theme: desktop
                        parts: !app.marginKeys ? app.panelAwayHints
                            : app.linkSuggesting ? [[["Up", "Down"], "choose"], ["Return", "insert", function() { app.acceptLinkSuggestion() }, app.linkSuggestions.length > 0],
                                ["Escape", "dismiss", app.dismissLinkSuggestions]]
                            : notePreviewText.activeFocus ? [[["J", "K"], "scroll"], [app.actionKeys("notePreview")[0], "write", app.toggleNotePreview],
                                app.panelReturnHint, ["Escape", "close", app.closePanel]]
                            : app.noteConflict ? [[app.actionKeys("noteKeepDraft")[0], "save my draft", app.keepNoteDraft, app.canKeepDraft],
                                [app.actionKeys("noteUseSaved")[0], "use saved text", app.useSavedNote]]
                            : [app.noteLinkAtCursor || app.noteUrlAtCursor.length ? ["Alt+Return", "open link", app.openNoteLinkAtCursor] : null,
                                [app.actionKeys("notePreview")[0], "preview", app.toggleNotePreview],
                                app.linkedFrom.length ? [app.actionKeys("noteLinkedFrom")[0], "linked from", app.focusLinkedFrom] : null,
                                app.panelReturnHint, ["Escape", "close", app.closePanel]]
                        pixelSize: 11 * app.scale
                        Layout.fillWidth: true
                    }
                    ColumnLayout {
                        id: linkedList
                        objectName: "linkedFrom"
                        // Reached by ctrl ↓ from the note: j k move, enter opens,
                        // y / shift Y copy, d Desk, esc (or ctrl ↑, tab) returns to the note.
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
                                ["D", "Desk", function() { app.addToDesk(app.linkedFrom[linkedList.currentIndex]) }, !!row],
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
                                // Hover chooses the row (its tint), like ↑ ↓; a click inserts it.
                                MouseArea {
                                    objectName: "noteLinkSuggestionMouse" + suggestion.index
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onEntered: app.linkSuggestionIndex = suggestion.index
                                    onPositionChanged: app.linkSuggestionIndex = suggestion.index
                                    onClicked: app.acceptLinkSuggestion(suggestion.index)
                                }
                            }
                        }
                    }
                }
            }
            // The Desk in the Margin: its page, written and previewed like a note.
            SidePanel {
                id: deskPanel
                objectName: "deskPanel"
                mode: "desk"
                Keys.onEscapePressed: function(event) { app.closePanel(); event.accepted = true }
                // A press on the panel's chrome brings the keyboard to the page (Write mode).
                PointHandler {
                    objectName: "deskChromePress"
                    onActiveChanged: if (!active && app.deskOpen && !app.deskPreview) { app.marginKeys = true; Qt.callLater(app.keepDeskEditorFocus) }
                }
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: deskPanel.inset
                    spacing: deskPanel.gap
                    SidePanelHeader {
                        objectName: "deskHeader"
                        Layout.fillWidth: true
                        title: "Desk"
                        subtitle: app.deskSummary(deskEditor.text)
                        titleName: "deskTitle"; subtitleName: "deskSummary"; closeName: "deskClose"
                        closeTip: "Close the Desk · " + app.escapeText
                        onCloseRequested: app.closeNotes()
                        QuietButton {
                            objectName: "deskClean"
                            text: "Clean…"; compact: true
                            enabled: bible.deskPage.length > 0 && !bible.busy
                            keys: app.keyText("cleanDesk")
                            tip: "Clean Desk · " + keys + ": empty the page in every window (asks first; " + app.keyText("undo") + " brings it back)"
                            onClicked: app.requestCleanDesk()
                        }
                    }
                    FineRule { Layout.fillWidth: true }
                    ColumnLayout {
                        visible: app.deskConflict
                        Layout.fillWidth: true
                        Label {
                            objectName: "deskConflictMessage"
                            text: "The Desk changed in another window. Your unsaved draft is preserved. Review the saved page before replacing it."
                            color: desktop.accent; font.pixelSize: 12 * app.scale
                            Layout.fillWidth: true; wrapMode: Text.Wrap
                            Accessible.role: Accessible.AlertMessage
                        }
                        RowLayout {
                            QuietButton {
                                objectName: "deskKeepDraft"
                                text: "Save my draft"; tip: "Replace the saved page with your draft · " + app.keyText("noteKeepDraft") + " on the page"
                                focusTarget: deskEditor
                                enabled: app.canKeepDeskDraft
                                onClicked: app.keepDeskDraft()
                            }
                            QuietButton {
                                objectName: "deskUseSaved"
                                text: "Use saved text"; tip: "Replace your draft with the saved page · " + app.keyText("noteUseSaved") + " on the page"
                                focusTarget: deskEditor
                                onClicked: app.useSavedDesk()
                            }
                        }
                    }
                    RowLayout {
                        spacing: 4
                        QuietButton { objectName: "deskWriteButton"; text: "Write"; compact: true; emphasized: !app.deskPreview; keys: app.keyText("notePreview"); tip: "Write · " + keys; onClicked: { app.deskPreview = false; deskEditor.forceActiveFocus() } }
                        QuietButton { objectName: "deskPreviewButton"; text: "Preview"; compact: true; emphasized: app.deskPreview; keys: app.keyText("notePreview"); tip: "Preview · " + keys; onClicked: { if (app.flushDesk()) { app.deskPreview = true; deskPreviewText.forceActiveFocus() } } }
                        Item { Layout.fillWidth: true }
                        Label { text: "Markdown"; color: desktop.muted; font.pixelSize: 11 * app.scale }
                    }
                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        OverlayScrollView {
                            id: deskEditorScroll
                            objectName: "deskEditorScroll"
                            anchors.fill: parent
                            visible: !app.deskPreview
                            clip: true
                            contentWidth: availableWidth
                            TextArea {
                                id: deskEditor
                                ContextMenu.menu: null
                                ContextMenu.onRequested: function(position) { app.openTextMenu(deskEditor, position) }
                                objectName: "deskEditor"
                                width: parent.width
                                color: desktop.foreground
                                selectionColor: desktop.selection
                                selectedTextColor: desktop.foreground
                                font.family: desktop.fontFamily
                                font.pixelSize: 15 * app.scale
                                wrapMode: TextEdit.Wrap
                                selectByMouse: true
                                enabled: !bible.busy
                                padding: 2
                                background: Item {}
                                placeholderText: "Press " + app.keyText("toggleDesk") + " on a passage to quote it here, or write anything."
                                placeholderTextColor: desktop.muted
                                onTextChanged: if (!app.syncingDesk) { if (app.noteSaveDelay > 0) deskSaveTimer.restart(); else app.flushDesk() }
                                Keys.onPressed: function(event) { app.deskKey(event) }
                                Keys.onEscapePressed: function(event) { app.closePanel(); event.accepted = true }
                                Accessible.name: "The Desk, in Markdown"
                                // ctrl click: open the passage (or web) link clicked, as alt enter does at the cursor.
                                TapHandler {
                                    objectName: "deskEditorLinkClick"
                                    acceptedButtons: Qt.LeftButton
                                    acceptedModifiers: Qt.ControlModifier
                                    onTapped: function(point) {
                                        deskEditor.cursorPosition = deskEditor.positionAt(point.position.x, point.position.y)
                                        if (app.deskLinkAtCursor || app.deskUrlAtCursor.length) app.openDeskLinkAtCursor()
                                    }
                                }
                            }
                        }
                        OverlayScrollView {
                            id: deskPreviewScroll
                            objectName: "deskPreviewScroll"
                            anchors.fill: parent
                            visible: app.deskPreview
                            clip: true
                            contentWidth: availableWidth
                            TextArea {
                                id: deskPreviewText
                                ContextMenu.menu: null
                                ContextMenu.onRequested: function(position) { app.openTextMenu(deskPreviewText, position) }
                                objectName: "deskPreview"
                                width: parent.width
                                // Focusable for reading by key: the scroll keys move the preview.
                                activeFocusOnTab: true
                                Keys.onPressed: function(event) { app.scrollKey(deskPreviewScroll.contentItem as Flickable, event) }
                                text: deskEditor.text.length ? app.previewMarkdown(deskEditor.text) : "*Press " + app.keyText("toggleDesk") + " on a passage to quote it here.*"
                                textFormat: TextEdit.MarkdownText
                                readOnly: true
                                selectByMouse: true
                                wrapMode: TextEdit.Wrap
                                color: deskEditor.text.length ? desktop.foreground : desktop.muted
                                font.pixelSize: 15 * app.scale
                                padding: 2
                                background: Item {}
                                onLinkActivated: function(link) { app.activateNoteLink(link) }
                                Accessible.name: "Desk preview"
                                QuoteBars { objectName: "deskQuoteBars"; target: deskPreviewText; color: app.quoteBarColor; uiScale: app.scale }
                            }
                        }
                    }
                    HintRow {
                        objectName: "deskHints"
                        visible: !app.marginKeys || deskEditor.activeFocus || deskPreviewText.activeFocus
                        theme: desktop
                        parts: !app.marginKeys ? app.panelAwayHints
                            : deskPreviewText.activeFocus ? [[["J", "K"], "scroll"], [app.actionKeys("notePreview")[0], "write", app.toggleDeskPreview],
                                app.panelReturnHint, ["Escape", "close", app.closePanel]]
                            : app.deskConflict ? [[app.actionKeys("noteKeepDraft")[0], "save my draft", app.keepDeskDraft, app.canKeepDeskDraft],
                                [app.actionKeys("noteUseSaved")[0], "use saved text", app.useSavedDesk]]
                            : [app.deskLinkAtCursor || app.deskUrlAtCursor.length ? ["Alt+Return", "open link", app.openDeskLinkAtCursor] : null,
                                [app.actionKeys("notePreview")[0], "preview", app.toggleDeskPreview],
                                app.panelReturnHint, ["Escape", "close", app.closePanel]]
                        pixelSize: 11 * app.scale
                        Layout.fillWidth: true
                    }
                    Label {
                        objectName: "deskStatus"
                        text: app.deskConflict ? "Not saved · resolve the other window's change above"
                            : deskEditor.text === bible.deskPage ? "Saved on this device · the same page in every window" : "Not saved · your draft is still on the page"
                        color: desktop.muted; font.pixelSize: 11 * app.scale; wrapMode: Text.Wrap; Layout.fillWidth: true
                    }
                }
            }
            // Marks in the Margin: every mark, one row per passage, filtered by words or #tag.
            SidePanel {
                id: marksPanel
                objectName: "marksPanel"
                mode: "marks"
                // A press on the panel brings the keyboard to its list (or its field).
                PointHandler {
                    objectName: "marksChromePress"
                    onActiveChanged: if (!active && app.marksOpen && !app.marginKeys) { app.marginKeys = true; Qt.callLater(app.restorePanelFocus) }
                }
                // Changes anywhere (this window, another, any version) reread the marks once.
                Connections {
                    target: bible
                    enabled: app.marksOpen
                    function onMarksChanged() { Qt.callLater(app.refreshMarks) }
                    function onCollectionChanged() { Qt.callLater(app.refreshMarks) }
                    function onTagsChanged() { Qt.callLater(app.refreshMarks) }
                    function onVersionsChanged() { Qt.callLater(app.refreshMarks) }
                    function onVersionChanged() { Qt.callLater(app.refreshMarks) }
                }
                ColumnLayout {
                    objectName: "savedContent"
                    anchors.fill: parent
                    anchors.margins: marksPanel.inset
                    Shortcut {
                        sequence: "Escape"
                        enabled: app.marksOpen && app.marginKeys && !contextMenu.visible && !tagsUi.visibleModal
                        onActivated: app.escapeSaved()
                    }
                    Shortcut { sequence: "/"; enabled: app.marksKeys && app.savedSearchVisible; onActivated: app.focusSavedSearch() }
                    Shortcut { sequences: ["J", "Down"]; enabled: app.marksKeys; onActivated: app.moveSaved(1) }
                    Shortcut { sequences: ["K", "Up"]; enabled: app.marksKeys; onActivated: app.moveSaved(-1) }
                    ListMotionKeys { objectName: "savedMotionKeys"; active: app.marksKeys; count: collectionList.count; onMove: function(delta) { app.moveSaved(delta) } }
                    // enter opens the current row from anywhere in the drawer (a focused button takes its own enter).
                    Shortcut { sequences: ["Return", "Enter"]; enabled: app.marksKeys && collectionList.currentIndex >= 0; onActivated: app.openSaved(collectionList.model[collectionList.currentIndex]) }
                    Shortcut { sequences: ["Delete", "Backspace"]; enabled: app.marksKeys && !bible.busy; onActivated: app.deleteSaved() }
                    // Shared list keys: y quotation, shift Y plain text, d Desk.
                    Shortcut { sequence: "Y"; enabled: app.marksKeys; onActivated: app.copyScope(app.focusedSavedEntry()) }
                    Shortcut { sequence: "Shift+Y"; enabled: app.marksKeys; onActivated: app.copyScope(app.focusedSavedEntry(), true) }
                    Shortcut { sequence: "D"; enabled: app.marksKeys; onActivated: app.addToDesk(app.focusedSavedEntry()) }
                    // In place: w writes the row's note in the row; t edits its tags over the drawer; ctrl t manages tags.
                    Shortcut { objectName: "savedNoteKey"; sequence: "W"; enabled: app.marksKeys && !bible.busy && !!app.focusedSavedEntry(); onActivated: app.editMarkNote(app.focusedSavedEntry()) }
                    Shortcut {
                        objectName: "savedTagKey"
                        sequence: "T"
                        enabled: app.marksKeys && !bible.busy && !!app.focusedSavedEntry()
                        onActivated: app.openPassageTags(tagsUi.snapshot(app.focusedSavedEntry()))
                    }
                    Shortcut { sequences: app.actionKeys("manageTags"); enabled: app.marksKeys && !bible.busy; onActivated: tagsUi.openManage() }
                    // A note changed in another window while written here: keep the draft or take the saved text.
                    Shortcut { sequences: app.actionKeys("noteKeepDraft"); enabled: app.marksOpen && app.markEditConflict; onActivated: app.keepMarkDraft() }
                    Shortcut { sequences: app.actionKeys("noteUseSaved"); enabled: app.marksOpen && app.markEditConflict; onActivated: app.useSavedMarkNote() }
                    Shortcut {
                        sequences: ["Menu", "Shift+F10"]
                        enabled: app.marksKeys && collectionList.currentIndex >= 0
                        onActivated: { const p = app.sceneRowPoint(collectionList.currentItem); app.openSavedMenu(collectionList.currentIndex, p.x, p.y) }
                    }
                    spacing: marksPanel.height < 350 * app.scale ? Math.round(6 * app.scale) : marksPanel.gap
                    SidePanelHeader {
                        objectName: "savedHeader"
                        Layout.fillWidth: true
                        title: "Marks"
                        subtitle: tagsUi.passageSubtitle
                        titleName: "savedTitle"; subtitleName: "savedPassage"; closeName: "savedClose"
                        closeTip: "Close Marks · " + app.escapeText
                        onCloseRequested: app.closeNotes()
                    }
                    FineRule { Layout.fillWidth: true }
                    TextField {
                        id: savedSearch
                        ContextMenu.menu: null
                        ContextMenu.onRequested: function(position) { app.openTextMenu(savedSearch, position) }
                        objectName: "savedSearch"
                        Layout.fillWidth: true
                        visible: app.savedSearchVisible
                        implicitHeight: 36 * app.controlScale
                        font.pixelSize: 14 * app.controlScale
                        color: desktop.foreground
                        placeholderText: "Filter marks… #name for a tag"
                        placeholderTextColor: desktop.muted
                        selectByMouse: true
                        leftPadding: 10 * app.controlScale; rightPadding: 10 * app.controlScale
                        text: app.savedQuery
                        onTextEdited: {
                            app.savedQuery = text
                            collectionList.currentIndex = collectionList.count ? 0 : -1
                            // Clearing a short list's query hides the field: keep the keyboard in the list.
                            if (!app.savedSearchVisible) Qt.callLater(app.focusSavedList)
                        }
                        background: Rectangle { radius: Math.min(desktop.cornerRadius, width / 2, height / 2); color: desktop.surface; border.color: savedSearch.activeFocus ? desktop.accent : desktop.border }
                        Keys.onDownPressed: app.focusSavedList()
                        Keys.onReturnPressed: app.focusSavedList()
                        Keys.onEnterPressed: app.focusSavedList()
                        Accessible.name: "Filter marks"
                    }
                    ListView {
                        id: collectionList
                        objectName: "collectionList"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: 6 * app.scale
                        model: app.filteredMarks()
                        onModelChanged: currentIndex = count > 0 ? Math.max(0, Math.min(currentIndex, count - 1)) : -1
                        ScrollBar.vertical: OverlayScrollbar { id: savedScrollbar; objectName: "savedScrollbar" }
                        // The row sits in a plain item: the list's current item takes the
                        // list's focus, and rows themselves never do (like Desk and History).
                        delegate: Item {
                            id: savedBlock
                            required property var modelData
                            required property int index
                            readonly property bool editing: savedEntry.editing
                            readonly property var editor: savedEntry.editor
                            width: collectionList.width - app.scrollGutter(collectionList)
                            height: savedEntry.height
                            SavedEntry {
                                id: savedEntry
                                modelData: savedBlock.modelData
                                index: savedBlock.index
                                objectName: "savedEntry" + (modelData.active ? "" : modelData.versionAbbreviation + ":") + modelData.reference
                                // Like Desk and History rows: the list keeps the keyboard (tab
                                // never stops on a row); the current row is the list's.
                                focusPolicy: Qt.NoFocus
                                width: collectionList.width - app.scrollGutter(collectionList)
                                padding: 12 * app.scale
                                height: savedContent.implicitHeight + topPadding + bottomPadding
                                clip: true
                                highlighted: collectionList.currentIndex === index
                                onClicked: { collectionList.currentIndex = index; app.openSaved(modelData) }
                                TapHandler {
                                    acceptedButtons: Qt.RightButton
                                    onTapped: function(eventPoint) { app.openSavedMenu(savedEntry.index, eventPoint.scenePosition.x, eventPoint.scenePosition.y) }
                                }
                                readonly property bool editing: app.markEditKey.length > 0 && app.markEditKey === app.rangeKey(modelData)
                                property alias editor: markEditor
                                // The reader's range (equal or overlapping) is "here".
                                readonly property bool here: { app.savedHereKey; return app.savedHere(modelData) }
                                contentItem: ColumnLayout {
                                    id: savedContent
                                    objectName: "savedEntryContent" + savedEntry.modelData.reference
                                    spacing: 7 * app.scale
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 8 * app.scale
                                        Label {
                                            id: savedReference
                                            objectName: "savedReference" + savedEntry.modelData.reference
                                            text: savedEntry.modelData.reference; textFormat: Text.PlainText; color: desktop.foreground
                                            font.pixelSize: 16 * app.scale; font.weight: Font.Medium; Layout.fillWidth: true; wrapMode: Text.Wrap
                                            FontMetrics { id: savedReferenceMetrics; font: savedReference.font }
                                            // A highlighted mark: the reader's highlighter stroke (same ink and seed) under the reference.
                                            Canvas {
                                                id: savedHighlight
                                                objectName: "savedHighlight" + savedEntry.modelData.reference
                                                z: -1
                                                visible: !!savedEntry.modelData.highlight
                                                x: -4 * app.scale
                                                width: Math.min(savedReference.width, savedReference.contentWidth) + 8 * app.scale
                                                height: savedReference.height
                                                property color ink: app.highlightInk
                                                readonly property var paintKey: [visible, app.scale, width, height, app.highlightAlpha,
                                                    savedEntry.modelData.reference, savedEntry.modelData.versionId, bible.versionId]
                                                onPaintKeyChanged: requestPaint()
                                                onInkChanged: requestPaint()
                                                Connections { target: app; function onHighlightsRepaint() { savedHighlight.requestPaint() } }
                                                onPaint: {
                                                    const ctx = getContext("2d")
                                                    ctx.reset()
                                                    if (!visible) return
                                                    const s = HandDrawn.seed("highlight", savedEntry.modelData.versionId || bible.versionId, savedEntry.modelData.reference)
                                                    const band = app.highlightBandFor(savedReferenceMetrics)
                                                    const lineHeight = savedReferenceMetrics.height
                                                    const lines = Math.max(1, savedReference.lineCount)
                                                    for (let i = 0; i < lines; ++i)
                                                        HandDrawn.highlighterStroke(ctx, Qt.rect(0, i * lineHeight + band.top, width, band.height),
                                                            ink, app.highlightAlpha, HandDrawn.seed(s, "row", i), app.scale)
                                                }
                                            }
                                        }
                                        // Marks kept in another version say which.
                                        Label {
                                            objectName: "savedVersion" + savedEntry.modelData.reference
                                            visible: !savedEntry.modelData.active
                                            text: "from " + savedEntry.modelData.versionAbbreviation
                                            textFormat: Text.PlainText
                                            color: desktop.muted
                                            font.pixelSize: 11 * app.scale
                                            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                                        }
                                        Label {
                                            objectName: "savedHere" + savedEntry.modelData.reference
                                            visible: savedEntry.here
                                            text: "here"
                                            color: desktop.accent
                                            font.pixelSize: 11 * app.scale
                                            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                                        }
                                    }
                                    // Tags: click one to show only its marks.
                                    Flow {
                                        objectName: "savedTags" + savedEntry.modelData.reference
                                        Layout.fillWidth: true
                                        visible: Array.from(savedEntry.modelData.tags || []).length > 0
                                        spacing: 8 * app.scale
                                        Repeater {
                                            model: Array.from(savedEntry.modelData.tags || [])
                                            delegate: Label {
                                                required property var modelData
                                                objectName: "savedTag:" + modelData.name
                                                text: "#" + modelData.name
                                                textFormat: Text.PlainText
                                                color: desktop.accent
                                                font.pixelSize: 12 * app.scale
                                                MouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: app.filterMarksByTag(parent.modelData.name)
                                                }
                                            }
                                        }
                                    }
                                    Label { objectName: "savedNoteText" + savedEntry.modelData.reference; text: savedEntry.editing ? "" : savedEntry.modelData.note.replace(/\s+/g, " "); textFormat: Text.PlainText; visible: text.length > 0; color: desktop.muted; font.pixelSize: 12 * app.scale; maximumLineCount: 3; elide: Text.ElideRight; wrapMode: Text.Wrap; Layout.fillWidth: true }
                                    Label { objectName: "savedExcerpt" + savedEntry.modelData.reference; text: savedEntry.editing || savedEntry.modelData.note.length ? "" : app.leftToRightText(savedEntry.modelData.excerpt || ""); textFormat: Text.PlainText; visible: text.length > 0; color: desktop.muted; font.pixelSize: 13 * app.scale; maximumLineCount: 1; elide: Text.ElideRight; wrapMode: Text.Wrap; Layout.fillWidth: true }
                                    // The note, written in place (w); saves as you type, esc is done.
                                    TextArea {
                                        id: markEditor
                                        objectName: savedEntry.editing ? "markEditor" : ""
                                        visible: savedEntry.editing
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: visible ? Math.max(96 * app.scale, contentHeight + topPadding + bottomPadding) : 0
                                        wrapMode: TextEdit.Wrap
                                        textFormat: TextEdit.PlainText
                                        font.pixelSize: 14 * app.scale
                                        color: desktop.foreground
                                        placeholderText: "Write a note…"
                                        placeholderTextColor: desktop.muted
                                        selectByMouse: true
                                        ContextMenu.menu: null
                                        ContextMenu.onRequested: function(position) { app.openTextMenu(markEditor, position) }
                                        onVisibleChanged: if (visible) text = app.markEditText
                                        onTextChanged: if (savedEntry.editing && text !== app.markEditText) {
                                            app.markEditText = text
                                            if (app.noteSaveDelay > 0) markSaveTimer.restart(); else app.flushMarkNote()
                                        }
                                        background: Rectangle {
                                            radius: Math.min(desktop.cornerRadius, width / 2, height / 2)
                                            color: desktop.surface
                                            border.color: app.markEditConflict ? desktop.error : markEditor.activeFocus ? desktop.accent : desktop.border
                                        }
                                        Accessible.name: "Note on " + savedEntry.modelData.reference
                                    }
                                    Label {
                                        objectName: savedEntry.editing ? "markEditConflict" : ""
                                        visible: savedEntry.editing && app.markEditConflict
                                        text: "This note changed in another window. " + KeyNotation.format("Ctrl+S") + " saves your draft over it, " + KeyNotation.format("Ctrl+Shift+S") + " takes the saved text."
                                        color: desktop.error
                                        font.pixelSize: 12 * app.scale
                                        wrapMode: Text.Wrap
                                        Layout.fillWidth: true
                                    }
                                }
                                background: Rectangle {
                                    color: savedEntry.highlighted ? (app.marginKeys ? app.tint(desktop.accent, 0.11) : app.inactiveRowColor)
                                        : savedEntry.hovered ? desktop.surface : "transparent"
                                    radius: Math.min(desktop.cornerRadius, width / 2, height / 2)
                                    // The current row is shown by its tinted fill alone, never an accent border.
                                    border.width: 0
                                }
                            }
                        }
                        Label {
                            objectName: "savedEmpty"
                            anchors.centerIn: parent
                            width: parent.width - 24
                            text: app.marksRows.length ? "No matching marks."
                                : "No marks yet. Highlight with " + app.keyText("highlight") + ", write a note with " + app.keyText("notes")
                                    + ", or tag with " + app.keyText("tags") + "."
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.Wrap
                            color: desktop.muted
                            font.pixelSize: 13 * app.scale
                            lineHeight: 1.2
                            visible: collectionList.count === 0
                        }
                    }
                    // Marks' keys, above the footer (dropped when the drawer is too short for its list).
                    HintRow {
                        objectName: "savedHints"
                        visible: marksPanel.height >= 420 * app.scale
                        theme: desktop
                        parts: app.savedHintParts()
                        pixelSize: 11 * app.scale
                        Layout.fillWidth: true
                    }
                    FineRule { Layout.fillWidth: true }
                    RowLayout {
                        Layout.fillWidth: true
                        QuietButton { objectName: "savedExport"; text: "Export notes…"; compact: true; Layout.fillWidth: true; Layout.minimumWidth: 0; onClicked: if (app.dismissMarks()) app.runCommand("export") }
                        Item { Layout.fillWidth: true }
                        QuietButton { objectName: "savedBackup"; text: "Backup…"; compact: true; Layout.fillWidth: true; Layout.minimumWidth: 0; onClicked: if (app.dismissMarks()) app.runCommand("backup") }
                    }
                }
            }
            // See also in the Margin: TagsPanel lays its list out here (relatedHost).
            SidePanel {
                id: relatedPanel
                objectName: "relatedPanel"
                mode: "related"
                PointHandler {
                    objectName: "relatedChromePress"
                    onActiveChanged: if (!active && app.relatedOpen && !app.marginKeys) { app.marginKeys = true; Qt.callLater(app.restorePanelFocus) }
                }
            }
        }
    }

    // Footer: ? and mode on the left; search, passage, version, and layout in
    // the center; Annotations, Study, History (while enabled), and Desk on the right
    // (Notes is `w`). Buttons in a group sit edge to edge (no gaps). The center
    // stays centered while it fits. When cramped, the right group stacks into a
    // column at the bottom-right corner and the reader text keeps clear of it;
    // the footer stays one row tall. More cramped hides the layout button;
    // really cramped folds the right group into "…" and hides ?. The reader
    // keeps the footer row inside floatingSafety.
    // Focus mode keeps the footer but shows only the focus button. The pointer
    // on the footer reveals the rest at once, where they always sit (their
    // clicks wait 300 ms, so a click aimed at the focus button or the text is
    // not caught by a button that just appeared); keyboard focus entering the
    // footer reveals it too.
    property bool footerHoverRevealed: false
    readonly property bool footerFocused: !!activeFocusItem && activeFocusItem.footerButton === true
    readonly property bool footerQuiet: bible.focusMode && !footerHoverRevealed && !footerFocused
    readonly property bool footerGuard: footerGuardTimer.running
    Timer { id: footerGuardTimer; interval: 300 }
    // Hiding waits briefly: moving between the zone and a button (or two
    // buttons) passes through a frame where neither reports the pointer.
    Timer { id: footerHideTimer; objectName: "footerHideTimer"; interval: 200; onTriggered: app.footerHoverRevealed = false }
    // The pointer is on the footer: its zone, or a (revealed) button over it.
    function footerHovered() {
        let hovered = footerZone.containsMouse
        for (const group of [leftActions, locationControls, floatingActions])
            for (const child of group.children) if (child.footerButton === true && child.hovered) hovered = true
        return hovered
    }
    function updateFooterHover() {
        const hovered = footerHovered()
        if (hovered) footerHideTimer.stop()
        if (hovered && bible.focusMode && !footerHoverRevealed) { footerHoverRevealed = true; footerGuardTimer.restart() }
        else if (!hovered) { if (footerHoverRevealed && !footerHideTimer.running) footerHideTimer.restart() }
    }
    function footerFocusChanged() { if (footerFocused) footerGuardTimer.stop() }
    readonly property real footerMargin: 16 * controlScale
    // Clearance between reader text and the stacked right column.
    readonly property real footerGap: 4 * controlScale
    readonly property real footerButton: 40 * controlScale
    readonly property real footerGroupGap: 24 * controlScale
    readonly property int footerRightCount: bible.ready ? 3 : 0
    // The left group always has Aa and the mode (Keybindings before a version
    // is installed). Keybindings, then focus, join it where the footer has room
    // for them without changing any other fold, so a cramped footer sheds
    // focus, then Keybindings (both have keys), before the right group stacks
    // and folds. Focus mode always shows its button.
    readonly property real footerLeftBase: bible.ready ? 2 * footerButton : footerButton
    function footerLeftFits(extra) {
        return bible.ready
            && (footerCenterFull + 2 * Math.max(footerLeftBase + extra * footerButton, footerRightFull) + 2 * footerGroupGap <= footerAvailable
                || footerCenterMin + 2 * (Math.max(footerLeftBase + extra * footerButton, footerRightCount ? footerButton : 0) + footerGroupGap) <= footerAvailable)
    }
    readonly property bool footerShowsHelp: !bible.ready || footerLeftFits(1)
    readonly property bool footerFocusFits: footerLeftFits(2)
    readonly property bool footerShowsFocus: bible.ready && (footerFocusFits || focusLayout)
    readonly property real footerLeftFull: footerLeftBase + (bible.ready && footerShowsHelp ? footerButton : 0) + (footerShowsFocus ? footerButton : 0)
    readonly property real footerRightFull: footerRightCount * footerButton
    // The folds read a long reference's width, not the passage shown, so
    // nothing in the footer or the reader moves as the passage's name
    // changes length (a longer one elides).
    TextMetrics { id: footerPassageMeasure; font: passageControl.font; text: "2 Thessalonians 3:16–18" }
    readonly property real footerCenterFull: searchControl.implicitWidth
        + Math.ceil(footerPassageMeasure.advanceWidth + passageControl.leftPadding + passageControl.rightPadding) + 1
        + (bible.ready ? versionControl.implicitWidth : 0)
    readonly property real footerAvailable: stageWidth - 2 * footerMargin
    readonly property bool footerStacked: footerCenterFull + 2 * Math.max(footerLeftFull, footerRightFull) + 2 * footerGroupGap > footerAvailable
    // With the right group stacked, the center keeps clear of the wider corner.
    readonly property real footerSideReserve: Math.max(footerLeftFull, footerRightCount ? footerButton : 0) + footerGroupGap
    readonly property real footerPassageMin: 96 * controlScale
    // The passage elides first; the right group folds into "…" only when it
    // would drop below footerPassageMin.
    readonly property real footerCenterMin: searchControl.implicitWidth + footerPassageMin + (bible.ready ? versionControl.implicitWidth : 0)
    readonly property bool footerCollapsed: footerStacked && bible.ready && footerCenterMin + 2 * footerSideReserve > footerAvailable
    // Narrower still, Aa goes into the "…" menu too, so the passage keeps its room.
    readonly property bool footerShowsText: bible.ready && !(footerCollapsed && footerCenterMin + 2 * footerSideReserve > footerAvailable)
    readonly property real footerHeight: footerButton
    // Room the reader text leaves at the right edge for the stacked column.
    readonly property real footerRightInset: footerStacked && !footerCollapsed && footerRightCount && !focusLayout
        ? footerButton + footerMargin + 2 * footerGap : 0
    // The ribbon and its chapter ("Mark 5") after the passage, away from it, only where it fits
    // without folding anything else (r and the palette still reach it).
    readonly property bool footerShowsRibbon: bible.ready && !!bible.ribbon.versionId && !ribbonHere && !footerStacked
        && footerCenterFull + ribbonControl.implicitWidth + 2 * Math.max(footerLeftFull, footerRightFull) + 2 * footerGroupGap <= footerAvailable
    function footerPanelEntries() {
        const ready = bible.ready && !bible.busy
        return [
            footerShowsText ? null : { id: "text", text: "Text and layout (Aa)", enabled: ready, run: function() { app.toggleTextPopover() } },
            { id: "annotations", text: "Marks", shortcut: menuKey("annotations"), enabled: ready, run: function() { app.runCommand("annotations") } },
            { id: "notes", text: "Notes", shortcut: menuKey("notes"), enabled: ready, run: function() { app.toggleNotes() } },
            { id: "related", text: "See also", shortcut: menuKey("related"), enabled: ready && hasSelection, run: function() { app.runCommand("related") } },
            { id: "desk", text: "Desk", shortcut: menuKey("desk"), enabled: ready, run: function() { app.runCommand("desk") } }
        ].filter(function(entry) { return !!entry })
    }
    // Hover only (no buttons): clicks fall through to the text beneath.
    MouseArea {
        id: footerZone
        objectName: "footerZone"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.rightMargin: app.chromeDock
        anchors.bottom: parent.bottom
        height: app.footerHeight + 2 * app.footerMargin
        visible: bible.ready && bible.focusMode && !app.findPromptOpen
        acceptedButtons: Qt.NoButton
        hoverEnabled: true
        onContainsMouseChanged: app.updateFooterHover()
        // The wheel scrolls the reader beneath.
        onWheel: function(wheel) { wheel.accepted = false }
    }
    Row {
        id: leftActions
        objectName: "leftActions"
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: app.footerMargin
        spacing: 0
        visible: !app.findPromptOpen
        // Focus, text and layout (Aa), the mode, then Keybindings (which hides when cramped).
        FloatingButton {
            id: focusControl
            objectName: "focusControl"
            glyph: "focus"
            // The one footer button focus mode always shows.
            persistent: true
            selected: bible.focusMode
            visible: app.footerShowsFocus
            available: !bible.busy && !app.modalOpen && app.actionAvailable("focus")
            label: bible.focusMode ? "Focus mode · leave" : "Focus mode"
            keys: app.keyText("focus")
            onClicked: { app.runCommand("focus"); app.readerFocus() }
        }
        FloatingButton {
            id: textControl
            objectName: "textControl"
            glyph: "text"
            selected: textPopover.visible
            visible: app.footerShowsText
            available: !bible.busy && !app.modalOpen
            label: "Text size, line height, and layout"
            tip: label + "\n" + app.keyText("smaller") + " " + app.keyText("larger") + " text · " + app.keyText("readingMode") + " layout · "
                + app.keyText("settings") + " all reading settings"
            onClicked: app.toggleTextPopover()
        }
        FloatingButton {
            id: modeControl
            objectName: "modeControl"
            glyph: app.charMode ? "characters" : app.verseMode ? "visual" : "normal"
            selected: app.verseMode || app.charMode
            visible: bible.ready
            available: !bible.busy && !app.modalOpen
            label: app.charMode ? "Character selection · switch to normal"
                : app.verseMode ? "Visual mode · switch to normal" : "Normal mode · switch to visual"
            keys: app.charMode ? KeyNotation.format("Escape") : app.keyText("visual")
            tip: label + " · " + keys + (app.charMode ? "" : "\nCharacters · " + app.keyText("characterVisual"))
            onClicked: {
                if (app.charMode) app.endCharMode()
                else app.setVisualMode(!app.verseMode)
                app.readerFocus()
            }
        }
        FloatingButton {
            objectName: "helpControl"
            glyph: "help"
            label: "Keybindings"
            keys: app.keyText("help")
            visible: app.footerShowsHelp
            available: !bible.busy && !app.modalOpen
            onClicked: helpDialog.open()
        }
    }
    Row {
        id: locationControls
        objectName: "locationControls"
        anchors.bottom: parent.bottom
        anchors.bottomMargin: app.footerMargin
        x: (app.width - app.chromeDock) / 2 - width / 2
        readonly property real availableWidth: Math.max(0, app.stageWidth - 2 * (Math.max(leftActions.width, floatingActions.width) + app.footerMargin + app.footerGroupGap))
        readonly property real fixedWidth: searchControl.width + (ribbonControl.visible ? ribbonControl.width : 0)
            + (versionControl.visible ? versionControl.width : 0)
        width: Math.min(fixedWidth + passageControl.implicitWidth, availableWidth)
        visible: !app.findPromptOpen
        FloatingButton {
            id: searchControl
            objectName: "searchControl"
            glyph: "search"
            label: "Search Bible"
            keys: app.keyText("bibleSearch")
            available: bible.ready && !bible.busy && !app.modalOpen
            onClicked: app.showPaletteMode("search")
        }
        FloatingButton {
            id: passageControl
            objectName: "passageControl"
            width: Math.max(0, locationControls.width - locationControls.fixedWidth)
            text: bible.ready ? (bible.selectionReference || bible.passage) : "Open a passage"
            label: (app.hasSelection ? bible.selectionReference : bible.passage) + " · Open passage"
            keys: app.keyText("books")
            available: !bible.busy && !app.modalOpen
            onClicked: app.openPalette(false)
        }
        FloatingButton {
            id: ribbonControl
            objectName: "ribbonControl"
            text: bible.ribbon.chapterReference || ""
            leading: "ribbon"
            label: "Back to the ribbon · " + (bible.ribbon.reference || "")
            keys: app.keyText("ribbon")
            visible: app.footerShowsRibbon
            available: !bible.busy && !app.modalOpen
            onClicked: app.runCommand("ribbon")
            // A right click: back, move it here, or remove it.
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: function(eventPoint) { app.openRibbonMenu(eventPoint.scenePosition.x, eventPoint.scenePosition.y) }
            }
        }
        FloatingButton {
            id: versionControl
            objectName: "versionControl"
            width: Math.min(implicitWidth, locationControls.availableWidth * 0.4)
            text: bible.versionAbbreviation + (app.following ? " ⇄ " + app.followPartner : "")
            label: bible.edition + " · Switch version" + (app.following ? " · following the window beside, reading " + app.followPartner : "")
            keys: app.keyText("switchVersion")
            tip: label + " · " + keys + "\nOpen a version beside · " + app.keyText("openBeside") + ", or right-click"
            visible: bible.ready
            available: !bible.busy && !app.modalOpen
            onClicked: app.openVersions(false)
            // A right click: the version, a version beside, or no longer following.
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: function(eventPoint) { app.openVersionButtonMenu(eventPoint.scenePosition.x, eventPoint.scenePosition.y) }
            }
        }
    }
    Grid {
        id: floatingActions
        objectName: "floatingActions"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: app.footerMargin
        anchors.rightMargin: app.footerMargin + app.chromeDock
        spacing: 0
        // One row, or a column bottom-aligned in the corner when cramped.
        columns: app.footerStacked ? 1 : 5
        visible: !app.findPromptOpen
        readonly property bool expanded: bible.ready && !app.footerCollapsed
        FloatingButton {
            objectName: "savedControl"
            visible: floatingActions.expanded
            glyph: "marks"
            selected: app.marksOpen
            label: "Marks"
            keys: app.keyText("annotations")
            available: bible.ready && !bible.busy && !app.modalOpen
            onClicked: app.runCommand("annotations")
        }
        FloatingButton {
            objectName: "relatedControl"
            glyph: "study"
            selected: app.relatedOpen
            visible: floatingActions.expanded
            available: bible.ready && !bible.busy && !app.modalOpen
            label: "See also"
            keys: app.keyText("related")
            onClicked: app.runCommand("related")
        }
        FloatingButton {
            objectName: "deskControl"
            visible: floatingActions.expanded
            glyph: "desk"
            selected: app.deskOpen
            label: "Desk"
            keys: app.keyText("desk")
            available: bible.ready && !bible.busy && !app.modalOpen
            onClicked: app.runCommand("desk")
        }
        FloatingButton {
            id: footerMore
            objectName: "footerMore"
            visible: bible.ready && app.footerCollapsed
            glyph: "more"
            label: "Panels"
            available: bible.ready && !bible.busy && !app.modalOpen
            onClicked: {
                const p = footerMore.mapToItem(null, 0, 0)
                const entries = app.footerPanelEntries()
                contextMenu.show("footer", entries, p.x, p.y - entries.length * 30 * app.scale - 8 * app.scale)
            }
        }
    }

    // Aa: text size, line height, and layout at hand, docked above the left
    // footer group. The keyboard stays in the reader (these all have keys),
    // and it never covers the text: while it is open the reader's view ends
    // above it (textPopoverReserve). Aa, esc, focus mode, or any panel or
    // dialog closes it.
    function toggleTextPopover() {
        if (textPopover.visible) { textPopover.close(); return }
        if (modalOpen || !bible.ready) { rejected(); return }
        textPopover.open()
    }
    readonly property real textPopoverReserve: textPopover.visible ? Math.max(0, height - textPopover.y + 6 * scale) : 0
    onModalOpenChanged: if (modalOpen && textPopover.visible) textPopover.close()
    onFocusLayoutChanged: if (focusLayout && textPopover.visible) textPopover.close()
    Popup {
        id: textPopover
        objectName: "textPopover"
        parent: Overlay.overlay
        modal: false
        dim: false
        focus: false
        closePolicy: Popup.NoAutoClose
        enter: null
        exit: null
        padding: 12 * app.scale
        x: app.footerMargin
        y: app.height - app.footerMargin - app.footerButton - 6 * app.scale - height
        onAboutToHide: menuGhost.release(textPopover, textControl.mapToItem(null, textControl.width / 2, 0))
        background: Rectangle {
            color: desktop.background
            border.color: desktop.border
            radius: Math.min(desktop.cornerRadius, 12 * app.scale)
        }
        contentItem: ColumnLayout {
            spacing: 8 * app.scale
            Accessible.role: Accessible.Pane
            Accessible.name: "Text size, line height, and layout"
            Repeater {
                // [title, smaller, larger, value]
                model: [["Text size", "smaller", "larger"], ["Line height", "lineLess", "lineMore"]]
                delegate: RowLayout {
                    id: stepRow
                    required property var modelData
                    required property int index
                    spacing: 2 * app.scale
                    Label { text: stepRow.modelData[0]; color: desktop.foreground; font.pixelSize: 13 * app.scale; Layout.fillWidth: true; Layout.minimumWidth: 96 * app.scale }
                    QuietButton {
                        objectName: "textPopover:" + stepRow.modelData[1]
                        glyph: "minus"; compact: true
                        keys: app.keyText(stepRow.modelData[1]); tip: "Decrease · " + keys
                        Accessible.name: "Decrease " + stepRow.modelData[0].toLowerCase()
                        enabled: app.actionAvailable(stepRow.modelData[1])
                        onClicked: app.runCommand(stepRow.modelData[1])
                    }
                    Label {
                        objectName: "textPopover:value" + stepRow.index
                        text: stepRow.index === 0 ? app.readerSize : bible.lineHeight.toFixed(1)
                        color: desktop.accent; font.pixelSize: 14 * app.scale
                        horizontalAlignment: Text.AlignHCenter
                        Layout.preferredWidth: 36 * app.scale
                    }
                    QuietButton {
                        objectName: "textPopover:" + stepRow.modelData[2]
                        glyph: "plus"; compact: true
                        keys: app.keyText(stepRow.modelData[2]); tip: "Increase · " + keys
                        Accessible.name: "Increase " + stepRow.modelData[0].toLowerCase()
                        enabled: app.actionAvailable(stepRow.modelData[2])
                        onClicked: app.runCommand(stepRow.modelData[2])
                    }
                }
            }
            RowLayout {
                spacing: 4 * app.scale
                Repeater {
                    model: ["paragraph", "verse", "block"]
                    delegate: QuietButton {
                        id: popoverLayout
                        required property string modelData
                        objectName: "textPopover:" + modelData
                        text: app.readingModeName(modelData)
                        checkable: true
                        autoExclusive: true
                        checked: bible.readingMode === modelData
                        emphasized: checked
                        compact: true
                        Layout.fillWidth: true
                        Layout.minimumWidth: implicitWidth
                        keys: app.keyText("readingMode")
                        tip: app.readingModeName(modelData) + " layout · " + keys + " cycles"
                        Accessible.name: text + " layout"
                        leftPadding: 10 + 16 * app.scale + 6 * app.scale
                        implicitWidth: contentItem.implicitWidth + leftPadding + rightPadding
                        onClicked: bible.setReadingMode(modelData)
                        Canvas {
                            objectName: "textPopoverIcon:" + popoverLayout.modelData
                            width: 16 * app.scale
                            height: width
                            x: 10
                            y: (popoverLayout.height - height) / 2
                            property color ink: popoverLayout.emphasized ? desktop.accent : desktop.foreground
                            onInkChanged: requestPaint()
                            onPaint: {
                                const ctx = getContext("2d")
                                ctx.reset()
                                ctx.scale(width / 24, height / 24)
                                ctx.strokeStyle = ink
                                ctx.lineWidth = 1.8
                                ctx.lineCap = "square"
                                ctx.beginPath()
                                app.paintLayoutGlyph(ctx, popoverLayout.modelData)
                                ctx.stroke()
                            }
                        }
                    }
                }
            }
            FineRule { Layout.fillWidth: true }
            QuietButton {
                objectName: "textPopover:settings"
                text: "Reading settings…"; compact: true
                keys: app.keyText("settings"); tip: "All reading settings · " + keys
                Layout.alignment: Qt.AlignRight
                onClicked: { textPopover.close(); app.runCommand("settings") }
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
        fadeDuration: app.reduceMotion ? 0 : 85
        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
        anchors.margins: 16 * app.controlScale
        anchors.rightMargin: 16 * app.controlScale + app.chromeDock
        theme: desktop
        uiScale: app.scale
        matchIndex: app.searchMatchIndex
        matchCount: app.searchMatches.length
        error: app.searchError
        onPreview: function(query) { app.previewChapterFind(query) }
        onAccepted: function(query) { app.acceptChapterFind(query) }
        onCanceled: app.cancelChapterFind()
        onRejected: sounds.play("error")
        onTextMenu: function(field, position) { app.openTextMenu(field, position) }
    }
    // The match counter acts like n / shift N: click the next match, shift
    // click the previous; right click for Next / Previous / Clear search.
    function searchCounterMenu(sceneX, sceneY) {
        const ready = acceptedSearchQuery.length > 0
        contextMenu.show("searchCount", [
            menuAction("nextMatch", "Next match", "nextMatch"),
            menuAction("previousMatch", "Previous match", "previousMatch"),
            menuDivider,
            { id: "clearSearch", text: "Clear search", shortcut: KeyNotation.format("Escape"), enabled: ready,
                run: function() { app.clearSearch(); app.readerFocus() } }
        ], sceneX, sceneY)
    }
    Label {
        id: searchMatchCount
        objectName: "searchMatchCount"
        anchors.right: parent.right; anchors.bottom: parent.bottom
        anchors.rightMargin: 20 * app.controlScale + app.footerRightInset + app.chromeDock
        // Above the footer row, which focus mode keeps (quiet, revealed on hover).
        anchors.bottomMargin: app.footerMargin + app.footerHeight + 6 * app.controlScale
        visible: !app.findPromptOpen && app.acceptedSearchQuery.length > 0
        text: app.searchError || (app.searchMatches.length ? (app.searchMatchIndex < 0
            ? app.searchMatches.length + " matches" : (app.searchMatchIndex + 1) + " / " + app.searchMatches.length) : "No matches")
        color: searchCountMouse.containsMouse ? desktop.foreground : desktop.muted; font.pixelSize: 12 * app.scale
        MouseArea {
            id: searchCountMouse
            objectName: "searchMatchCountMouse"
            anchors.fill: parent
            anchors.margins: -6 * app.controlScale
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: function(mouse) {
                if (mouse.button === Qt.RightButton) {
                    const p = mapToItem(null, mouse.x, mouse.y)
                    app.searchCounterMenu(p.x, p.y)
                } else app.runCommand((mouse.modifiers & Qt.ShiftModifier) ? "previousMatch" : "nextMatch")
            }
        }
        AppToolTip {
            theme: desktop
            uiScale: app.scale
            visible: searchCountMouse.containsMouse
            text: "Click: next match · " + app.keyText("nextMatch") + "\n" + KeyNotation.format("Shift+Click") + ": previous · "
                + app.keyText("previousMatch") + "\n" + KeyNotation.format("RightClick") + ": more"
        }
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
        // The selection's text in each version (versionId → compareVersions row).
        property var compared: ({})
        property string compareReference: ""
        readonly property bool comparing: !removing && step === 0 && compareReference.length > 0
        property bool removeAnnotations: false
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
                text: !versionDialog.removing ? "Versions" + (versionDialog.comparing ? " · " + versionDialog.compareReference : "")
                    : versionDialog.step === 0 ? "Remove version"
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
                    ContextMenu.menu: null
                    ContextMenu.onRequested: function(position) { app.openTextMenu(versionSearch, position) }
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
                    glyph: "plus"
                    glyphSize: 18 * versionDialog.uiScale
                    keys: app.keyText("addVersion")
                    tip: "Add version · " + keys
                    Accessible.name: "Add version"
                    visible: !versionDialog.removing
                    enabled: !bible.busy
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
                    width: versionResults.width - app.scrollGutter(versionResults)
                    height: versionEntryContent.implicitHeight + 20 * versionDialog.uiScale
                    highlighted: ListView.isCurrentItem
                    onClicked: { versionResults.currentIndex = index; versionResults.forceActiveFocus(); app.acceptVersion() }
                    Keys.onPressed: function(event) { app.versionKey(event) }
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: function(eventPoint) { app.openVersionMenu(versionEntry.index, eventPoint.scenePosition.x, eventPoint.scenePosition.y) }
                    }
                    background: Rectangle { radius: Math.min(desktop.cornerRadius, width / 2, height / 2); color: versionEntry.highlighted ? app.tint(desktop.accent, 0.11) : versionEntry.hovered ? desktop.surface : "transparent" }
                    contentItem: ColumnLayout {
                        id: versionEntryContent
                        spacing: 4 * versionDialog.uiScale
                        Label {
                            text: (versionEntry.modelData.abbreviation ? versionEntry.modelData.abbreviation + " · " : "") + versionEntry.modelData.name
                            color: desktop.foreground; font.pixelSize: 14 * versionDialog.uiScale
                            Layout.fillWidth: true; wrapMode: Text.Wrap
                        }
                        // Comparing: this version's text for the selection.
                        Label {
                            readonly property var compared: versionDialog.comparing ? versionDialog.compared[versionEntry.modelData.id] : null
                            objectName: "versionText:" + (versionEntry.modelData.id || "")
                            visible: !!compared
                            text: !compared ? "" : compared.available ? compared.text : "Not in this version"
                            textFormat: Text.PlainText
                            color: compared && compared.available ? desktop.foreground : desktop.muted
                            font.pixelSize: 13 * versionDialog.uiScale
                            maximumLineCount: 4; elide: Text.ElideRight
                            Layout.fillWidth: true; wrapMode: Text.Wrap
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                text: versionEntry.modelData.hint || ((versionEntry.modelData.active ? "Current version" : "Installed locally")
                                    + (app.wantsReimport(versionEntry.modelData) ? " · older text format: re-import for " + app.reimportGains(versionEntry.modelData) : ""))
                                color: desktop.muted; font.pixelSize: 11 * versionDialog.uiScale
                                Layout.fillWidth: true; wrapMode: Text.Wrap
                            }
                            QuietButton {
                                objectName: "versionReimport:" + (versionEntry.modelData.id || "")
                                visible: !versionDialog.removing && versionDialog.step === 0 && app.wantsReimport(versionEntry.modelData)
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
                        + (versionDialog.removeAnnotations ? "Delete its notes, highlights, and tags. This cannot be undone."
                            : "Keep its notes, highlights, and tags. They will reconnect when the same version is reimported.")
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
                color: desktop.error; font.pixelSize: 12 * versionDialog.uiScale
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

    // Open beside (shift C) with more than one other version: which one to
    // read in a window beside this one, with the selection's text in each.
    ModalCard {
        id: besideDialog
        objectName: "besideDialog"
        // The selection's text in each version (versionId → compareVersions row).
        property var compared: ({})
        property string compareReference: ""
        readonly property real uiScale: Math.min(app.scale, Math.max(1, (app.height - 48) / 240))
        // As tall as all its versions need, at most as tall as Versions; a
        // search keeps that height, so the field never moves as you type.
        property real fitHeight: 0
        onImplicitHeightChanged: if (!besideSearch.text.length) fitHeight = implicitHeight
        height: Math.min(fitHeight, 540 * app.scale, app.height - 48)
        // Focus at once, while the modal is still fading in.
        onVisibleChanged: if (visible) besideSearch.forceActiveFocus()
        onClosed: app.restorePanelFocus()
        contentItem: ColumnLayout {
            spacing: 12 * besideDialog.uiScale
            Keys.onPressed: function(event) { app.besideKey(event) }
            Label {
                objectName: "besideTitle"
                text: "Open beside" + (besideDialog.compareReference.length ? " · " + besideDialog.compareReference : "")
                color: desktop.foreground; font.pixelSize: 22 * besideDialog.uiScale
                Layout.fillWidth: true; wrapMode: Text.Wrap
            }
            TextField {
                id: besideSearch
                ContextMenu.menu: null
                ContextMenu.onRequested: function(position) { app.openTextMenu(besideSearch, position) }
                objectName: "besideSearch"
                Layout.fillWidth: true
                implicitHeight: Math.round(42 * besideDialog.uiScale)
                placeholderText: "Search versions…"
                font.pixelSize: 15 * besideDialog.uiScale
                selectByMouse: true
                padding: 10 * besideDialog.uiScale
                background: Rectangle { radius: Math.min(desktop.cornerRadius, width / 2, height / 2); color: desktop.surface; border.color: besideSearch.activeFocus ? desktop.accent : desktop.border }
                onTextChanged: { besideResults.currentIndex = 0; besideResults.positionViewAtBeginning() }
                Keys.onPressed: function(event) { app.besideKey(event) }
                KeyNavigation.tab: besideResults
                Accessible.name: "Search versions to open beside"
            }
            ListView {
                id: besideResults
                objectName: "besideResults"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredHeight: Math.max(contentHeight, 64 * besideDialog.uiScale)
                clip: true
                spacing: 3 * besideDialog.uiScale
                model: app.besideEntries()
                onModelChanged: currentIndex = count ? Math.max(0, Math.min(currentIndex, count - 1)) : -1
                Keys.onPressed: function(event) { app.besideKey(event) }
                ScrollBar.vertical: OverlayScrollbar { objectName: "besideScrollbar" }
                delegate: ItemDelegate {
                    id: besideEntry
                    required property var modelData
                    required property int index
                    objectName: "besideEntry" + index
                    width: besideResults.width - app.scrollGutter(besideResults)
                    height: besideEntryContent.implicitHeight + 20 * besideDialog.uiScale
                    highlighted: ListView.isCurrentItem
                    onClicked: { besideResults.currentIndex = index; app.acceptBeside() }
                    Keys.onPressed: function(event) { app.besideKey(event) }
                    background: Rectangle { radius: Math.min(desktop.cornerRadius, width / 2, height / 2); color: besideEntry.highlighted ? app.tint(desktop.accent, 0.11) : besideEntry.hovered ? desktop.surface : "transparent" }
                    contentItem: ColumnLayout {
                        id: besideEntryContent
                        spacing: 4 * besideDialog.uiScale
                        Label {
                            text: (besideEntry.modelData.abbreviation ? besideEntry.modelData.abbreviation + " · " : "") + besideEntry.modelData.name
                            color: desktop.foreground; font.pixelSize: 14 * besideDialog.uiScale
                            Layout.fillWidth: true; wrapMode: Text.Wrap
                        }
                        // With a selection: this version's text for it.
                        Label {
                            readonly property var compared: besideDialog.compared[besideEntry.modelData.id] || null
                            objectName: "besideText:" + besideEntry.modelData.id
                            visible: !!compared
                            text: !compared ? "" : compared.available ? compared.text : "Not in this version"
                            textFormat: Text.PlainText
                            color: compared && compared.available ? desktop.foreground : desktop.muted
                            font.pixelSize: 13 * besideDialog.uiScale
                            maximumLineCount: 4; elide: Text.ElideRight
                            Layout.fillWidth: true; wrapMode: Text.Wrap
                        }
                    }
                }
                Label {
                    objectName: "besideEmpty"
                    anchors.centerIn: parent
                    width: parent.width
                    text: "No matching versions"
                    visible: besideResults.count === 0
                    color: desktop.muted; font.pixelSize: 13 * besideDialog.uiScale
                    horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap
                }
            }
            FineRule { Layout.fillWidth: true }
            HintRow {
                objectName: "besideHints"
                theme: desktop
                parts: [[["Up", "Down"], "choose"], ["Return", "open beside", app.acceptBeside, bible.ready && !bible.busy && besideResults.count > 0],
                    ["Escape", "cancel", function() { besideDialog.close() }]]
                pixelSize: 11 * besideDialog.uiScale
                Layout.fillWidth: true
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
                    ContextMenu.menu: null
                    ContextMenu.onRequested: function(position) { app.openTextMenu(referenceInput, position) }
                    objectName: "referenceInput"
                    anchors.fill: parent
                    implicitHeight: 38 * app.scale
                    font.pixelSize: 15 * app.scale
                    color: desktop.foreground
                    placeholderText: app.paletteBook ? app.paletteBookName + " — chapter, or chapter:verse" : "John 3:16–18, / text, or > command"
                    placeholderTextColor: desktop.muted
                    selectByMouse: true
                    leftPadding: 14
                    rightPadding: 14
                    background: Rectangle { color: desktop.surface; radius: Math.min(desktop.cornerRadius, width / 2, height / 2); border.color: referenceInput.activeFocus ? app.tint(desktop.accent, 0.65) : desktop.border }
                    onTextChanged: {
                        app.paletteCleared = false; app.paletteArrowUsed = false
                        app.scheduleBibleSearch(); paletteResults.currentIndex = 0; paletteChapters.currentIndex = 0
                    }
                    Keys.onPressed: function(event) {
                        if (app.paletteListKey(event)) return
                        const mode = event.modifiers === Qt.ControlModifier
                            ? ({ [Qt.Key_K]: "books", [Qt.Key_H]: "history", [Qt.Key_Slash]: "bibleSearch", [Qt.Key_Space]: "commands" })[event.key] : undefined
                        if (mode) { event.accepted = true; app.runCommand(mode); return }
                        if ((event.text === "/" || event.text === ">")
                                && (!text.length || selectedText === text)) {
                            text = event.text + " "; cursorPosition = text.length
                            event.accepted = true
                        } else if (event.key === Qt.Key_Backspace && event.modifiers === Qt.NoModifier
                                && (text === "/ " || text === "> ") && cursorPosition === 2 && !selectedText.length) {
                            text = ""; event.accepted = true
                        } else if (event.key === Qt.Key_Backspace && event.modifiers === Qt.NoModifier
                                && !text.length && (app.paletteBook > 0 || app.paletteHistory)) {
                            // Backspace with nothing typed steps back from a book's chapters or History.
                            app.backToBooks(); event.accepted = true
                        } else if (event.key === Qt.Key_Delete && event.modifiers === Qt.NoModifier && !app.paletteHistory
                                && !app.paletteTextMode && !text.length && app.paletteRemovable(paletteResults.model[paletteResults.currentIndex])) {
                            // On Continue or a recent passage, with nothing typed.
                            event.accepted = true
                            app.removePaletteEntry(paletteResults.model[paletteResults.currentIndex])
                        } else if (event.key === Qt.Key_Delete && app.paletteHistory && !app.paletteTextMode
                                && (event.modifiers === Qt.ShiftModifier || (event.modifiers === Qt.NoModifier
                                    && cursorPosition === text.length && !selectedText.length))) {
                            // In History, delete (where it deletes no text) removes the
                            // chosen passage; shift delete clears History.
                            event.accepted = true
                            const entry = paletteResults.model[paletteResults.currentIndex]
                            if (event.modifiers === Qt.ShiftModifier) app.requestClearHistory()
                            else app.removePaletteEntry(entry)
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
                    // ctrl ↓ / ctrl ↑ jump between the books of Bible search results.
                    Keys.onDownPressed: function(event) { if (app.paletteTextMode && event.modifiers === Qt.ControlModifier) app.jumpSearchBook(1); else app.paletteRow(1) }
                    Keys.onUpPressed: function(event) { if (app.paletteTextMode && event.modifiers === Qt.ControlModifier) app.jumpSearchBook(-1); else app.paletteRow(-1) }

                    // ← and → move through the chapter grid (← from the first
                    // chapter back to the books); in the book list → opens a
                    // book's chapters and ← steps back. A typed query keeps them
                    // for its caret until the arrows take over.
                    Keys.onLeftPressed: function(event) {
                        event.accepted = !app.paletteTextMode && event.modifiers === Qt.NoModifier && app.paletteBook > 0
                            && selectedText.length === 0 && (!text.length || cursorPosition === 0 || app.paletteArrowUsed)
                        if (!event.accepted) return
                        if (app.paletteChapterGrid && paletteChapters.currentIndex > 0) app.movePalette(-1)
                        else app.backToBooks()
                    }
                    Keys.onRightPressed: function(event) {
                        let entry = app.paletteList.model[app.paletteList.currentIndex]
                        let browsing = (!text.trim() || /^[1-3]?\s*[A-Za-z ]+$/.test(text.trim()) || (app.paletteBook > 0 && /^\d+$/.test(text.trim())) || text.trim().startsWith(">"))
                            && (app.paletteArrowUsed || (cursorPosition === text.length && selectedText.length === 0))
                        event.accepted = !app.paletteTextMode && event.modifiers === Qt.NoModifier && browsing && !!entry
                        if (!event.accepted) return
                        if (app.paletteChapterGrid) app.movePalette(1)
                        else app.activatePaletteEntry(entry)
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
                visible: !app.paletteTextMode && !referenceInput.text.trim().startsWith(">") && (referenceInput.text.trim().length > 0 || app.paletteBook > 0 || app.paletteHistory)
                text: paletteBack.activeFocus ? "Browse all books" : app.paletteResolution.valid ? app.paletteResolution.reference : app.paletteResolution.error
                color: app.paletteResolution.valid ? desktop.accent : desktop.muted
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                font.pixelSize: 12 * app.scale
            }
            QuietButton {
                id: paletteBack
                objectName: "paletteBack"
                text: "← All books"
                compact: true
                visible: !app.paletteTextMode && (app.paletteBook > 0 || app.paletteHistory)
                emphasized: activeFocus
                onClicked: app.backToBooks()
                Keys.onDownPressed: { app.paletteList.currentIndex = -1; app.movePalette(1) }
                // Nothing above Back.
                Keys.onUpPressed: app.rejected()
            }
            // A book's chapters: a grid of numbers, the reader's chapter in the accent.
            GridView {
                id: paletteChapters
                objectName: "paletteChapters"
                visible: app.paletteChapterGrid
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 40
                Layout.preferredHeight: Math.min(340 * app.scale, app.height * 0.43)
                clip: true
                model: app.paletteChapterGrid ? app.paletteEntries() : []
                currentIndex: 0
                // Always clear of the overlay scrollbar (its width): whether it
                // shows depends on the rows, which depend on this width.
                cellWidth: Math.floor((width - 18 * app.scale) / app.paletteColumns)
                cellHeight: 44 * app.scale
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: OverlayScrollbar { objectName: "paletteChaptersScrollbar" }
                Keys.onReturnPressed: app.acceptPalette()
                Keys.onEnterPressed: app.acceptPalette()
                delegate: ItemDelegate {
                    id: chapterCell
                    required property var modelData
                    required property int index
                    objectName: "paletteChapter" + modelData.chapter
                    width: paletteChapters.cellWidth - 3 * app.scale
                    height: paletteChapters.cellHeight - 3 * app.scale
                    highlighted: GridView.isCurrentItem
                    focusPolicy: Qt.NoFocus
                    readonly property bool here: modelData.book === bible.book && modelData.chapter === bible.chapter
                    onClicked: app.activatePaletteEntry(modelData)
                    contentItem: Label {
                        text: chapterCell.modelData.chapter
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        color: chapterCell.here ? desktop.accent : desktop.foreground
                        font.pixelSize: 14 * app.scale
                    }
                    background: Rectangle {
                        radius: Math.min(desktop.cornerRadius, width / 2, height / 2)
                        color: chapterCell.highlighted ? app.tint(desktop.accent, 0.11) : chapterCell.hovered ? desktop.surface : "transparent"
                    }
                    Accessible.name: chapterCell.modelData.title
                }
            }
            ListView {
                id: paletteResults
                objectName: "paletteResults"
                visible: !app.paletteChapterGrid
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 40
                Layout.preferredHeight: Math.min(340 * app.scale, app.height * 0.43)
                clip: true
                model: app.paletteChapterGrid ? [] : app.paletteEntries()
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
                    // Clear of the overlay scrollbar while it shows.
                    width: paletteResults.width - app.scrollGutter(paletteResults)
                    // History's day (Today, Yesterday, Earlier) heads its first row.
                    readonly property real groupHeight: paletteEntry.modelData.firstOfGroup ? 24 * app.scale : 0
                    height: (app.paletteTextMode ? searchResultContent.implicitHeight + 20 * app.scale : 44 * app.scale) + groupHeight
                    topInset: groupHeight
                    topPadding: groupHeight + verticalPadding
                    highlighted: ListView.isCurrentItem
                    Label {
                        objectName: "paletteGroup" + paletteEntry.index
                        visible: paletteEntry.groupHeight > 0
                        text: paletteEntry.modelData.group || ""
                        x: 12 * app.scale
                        y: paletteEntry.groupHeight - height - 3 * app.scale
                        color: desktop.muted
                        font.pixelSize: 12 * app.scale; font.weight: Font.Medium
                    }
                    // Unavailable rows read muted and stay clickable: a click sounds the error.
                    readonly property bool available: !modelData.action || app.actionAvailable(modelData.action)
                    onClicked: { if (available) app.activatePaletteEntry(modelData); else app.rejected(app.unavailableReason(modelData.action)) }
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        enabled: !!paletteEntry.modelData.recentQuery || !!paletteEntry.modelData.recent || !!paletteEntry.modelData.ribbon || app.paletteTextMode
                        onTapped: function(eventPoint) { app.openSearchMenu(paletteEntry.index, eventPoint.scenePosition.x, eventPoint.scenePosition.y) }
                    }
                    contentItem: ColumnLayout {
                        id: searchResultContent
                        spacing: 6 * app.scale
                        RowLayout {
                            Layout.fillWidth: true
                            Label { text: paletteEntry.modelData.title || ((paletteEntry.modelData.reference || "") + " · " + (paletteEntry.modelData.versionAbbreviation || "")); textFormat: Text.PlainText; color: paletteEntry.available ? desktop.foreground : desktop.muted; Layout.fillWidth: true; elide: Text.ElideRight; font.pixelSize: 14 * app.scale }
                            Label { objectName: "paletteHint" + (paletteEntry.modelData.action || ""); text: paletteEntry.modelData.hint || ""; color: desktop.muted; font.pixelSize: 11 * app.scale }
                            Label {
                                objectName: "paletteChevron" + paletteEntry.index
                                text: "›"
                                visible: !app.paletteTextMode && (!!paletteEntry.modelData.allHistory
                                    || (!!paletteEntry.modelData.book && !paletteEntry.modelData.chapter))
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
                    text: app.paletteTextMode ? (!app.paletteTextQuery.length ? (bible.searchRegex ? "Search with a regular expression, as in chapter find"
                            : 'Search words, "an exact phrase", or /a regular expression/')
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
                Label {
                    objectName: "searchResultCount"
                    text: paletteResults.count + " of " + bible.searchResultCount + " results · "
                        + bible.searchBooks.length + (bible.searchBooks.length === 1 ? " book" : " books")
                        + (app.searchedVersions > 1 ? " in " + app.searchedVersions + " versions" : "")
                    color: desktop.muted; font.pixelSize: 12 * app.scale; Layout.fillWidth: true
                }
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

    ModalCard {
        id: cleanDeskConfirmation
        objectName: "cleanDeskConfirmation"
        // Outside clicks cancel, like esc; they never reach what is underneath.
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        // Focus at once, while the modal is still fading in.
        onVisibleChanged: if (visible) { cleanDeskCancel.forceActiveFocus() }
        onClosed: app.restorePanelFocus()
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
                text: "Everything on the Desk goes, in every window: its quotations and what you wrote around them. Your notes, highlights, and tags are kept, and "
                    + app.keyText("undo") + " brings the page back."
                color: desktop.foreground; font.pixelSize: 14 * app.scale; Layout.fillWidth: true; wrapMode: Text.Wrap
            }
            Label { text: bible.error; visible: text.length > 0; color: desktop.error; Layout.fillWidth: true; wrapMode: Text.Wrap }
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

    ModalCard {
        id: clearHistoryConfirmation
        objectName: "clearHistoryConfirmation"
        // Outside clicks cancel, like esc; they never reach what is underneath.
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        // Focus at once, while the modal is still fading in.
        onVisibleChanged: if (visible) { clearHistoryCancel.forceActiveFocus() }
        onClosed: {
            // From the palette's History: back to its input.
            if (passagePalette.visible) referenceInput.forceActiveFocus()
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
            Label { text: bible.error; visible: text.length > 0; color: desktop.error; Layout.fillWidth: true; wrapMode: Text.Wrap }
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
        id: helpDialog
        objectName: "helpDialog"
        property string pendingAction: ""
        height: Math.min(660 * app.scale, app.height - 48)
        // Focus at once, while the modal is still fading in.
        onVisibleChanged: if (visible) { keybindingSearch.text = ""; keybindingSearch.forceActiveFocus(); app.dismissKeyStrip() }
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
                ContextMenu.menu: null
                ContextMenu.onRequested: function(position) { app.openTextMenu(keybindingSearch, position) }
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
                // home / end move the caret in a typed search (with nothing typed,
                // where there is no caret to move, the list's first / last row);
                // pgup / pgdn page the list.
                Keys.onPressed: function(event) {
                    if (event.modifiers !== Qt.NoModifier) return
                    if (event.key === Qt.Key_PageUp || event.key === Qt.Key_PageDown) {
                        app.pageKeybindings(event.key === Qt.Key_PageDown ? 1 : -1)
                        event.accepted = true
                    } else if (!text.length && (event.key === Qt.Key_Home || event.key === Qt.Key_End)) {
                        app.keybindingEdge(event.key === Qt.Key_End)
                        event.accepted = true
                    }
                }
                onAccepted: app.activateKeybinding(keybindingResults.model[keybindingResults.currentIndex])
                Keys.onEscapePressed: helpDialog.close()
                Accessible.name: "Search keybindings by action or key"
            }
            }
            ColumnLayout {
                objectName: "keybindingEssentials"
                // Where the list keeps room to scroll: short windows show the list alone.
                visible: !keybindingSearch.text.length && helpDialog.height >= 420 * app.scale
                Layout.fillWidth: true
                spacing: 6 * app.scale
                SectionLabel { text: "ESSENTIALS"; Layout.fillWidth: true }
                HintRow {
                    objectName: "essentialHints"
                    theme: desktop
                    parts: app.essentialParts()
                    pixelSize: 13 * app.scale
                    Layout.fillWidth: true
                }
                FineRule { Layout.fillWidth: true }
            }
            ListView {
                id: keybindingResults
                objectName: "keybindingResults"
                section.property: "group"
                section.labelPositioning: ViewSection.InlineLabels | ViewSection.CurrentLabelAtStart
                section.delegate: Rectangle {
                    required property string section
                    objectName: "keybindingGroup:" + section
                    width: keybindingResults.width - app.scrollGutter(keybindingResults)
                    height: app.keybindingHeaderHeight
                    // Opaque, so rows pass under it while it sticks.
                    color: desktop.background
                    z: 2
                    SectionLabel { anchors.verticalCenter: parent.verticalCenter; x: 8 * app.scale; text: parent.section.toUpperCase() }
                }
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
                    // Clear of the scrollbar while it shows.
                    width: keybindingResults.width - app.scrollGutter(keybindingResults)
                    height: keybindingContent.implicitHeight + 20 * app.scale
                    // Unavailable rows read muted; a click sounds the error (activateKeybinding).
                    readonly property bool available: app.actionAvailable(modelData)
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
                        text: keybindingRow.modelData.title; color: keybindingRow.available ? desktop.foreground : desktop.muted
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
        // Focus starts at the top: the first switch, scrolled into view.
        onVisibleChanged: if (visible) { settingsScroll.contentItem.contentY = 0; app.focusFirstSetting() }
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
                id: settingSwitches
                model: app.actions.filter(function(action) { return !!action.setting })
                delegate: SettingSwitch {
                    required property var modelData
                    catalogAction: modelData
                    Layout.fillWidth: true
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
            Repeater {
                model: Array.from(bible.versions).filter(function(version) { return app.wantsReimport(version) })
                delegate: RowLayout {
                    id: reimportRow
                    required property var modelData
                    Layout.fillWidth: true
                    Label {
                        text: reimportRow.modelData.abbreviation + " uses an older text format: re-import for " + app.reimportGains(reimportRow.modelData) + "."
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
            // The on/off switches above; the layout and sliders below.
            FineRule { objectName: "settingsSwitchesRule"; Layout.fillWidth: true }
            RowLayout {
                Layout.fillWidth: true
                Label { objectName: "readingModeTitle"; text: "Layout"; color: desktop.foreground; font.pixelSize: 13 * app.scale; Layout.fillWidth: true }
                Label { objectName: "readingModeKey"; text: app.keyRich("readingMode"); textFormat: Text.StyledText; color: desktop.muted; font.pixelSize: 12 * app.scale }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 4 * app.scale
                uniformCellSizes: true
                Repeater {
                    model: ["paragraph", "verse", "block"]
                    delegate: QuietButton {
                        id: layoutChoice
                        required property string modelData
                        readonly property real iconSize: 16 * app.scale
                        readonly property real iconGap: 6 * app.scale
                        objectName: "readingMode:" + modelData
                        text: app.readingModeName(modelData)
                        checkable: true
                        autoExclusive: true
                        checked: bible.readingMode === modelData
                        emphasized: checked
                        compact: true
                        Layout.fillWidth: true
                        // Room for the icon, keeping icon and name centred together.
                        leftPadding: 10 + iconSize + iconGap
                        rightPadding: 10
                        onClicked: bible.setReadingMode(modelData)
                        Accessible.name: text + " layout"
                        // The footer layout button's icon for this layout, left of its name.
                        Canvas {
                            objectName: "readingModeIcon:" + layoutChoice.modelData
                            width: layoutChoice.iconSize
                            height: width
                            x: layoutChoice.leftPadding + (layoutChoice.availableWidth - layoutChoice.contentItem.implicitWidth) / 2 - width - layoutChoice.iconGap
                            y: (layoutChoice.height - height) / 2
                            property color ink: !layoutChoice.enabled ? desktop.muted : layoutChoice.emphasized ? desktop.accent : desktop.foreground
                            onInkChanged: requestPaint()
                            onPaint: {
                                const ctx = getContext("2d")
                                ctx.reset()
                                ctx.scale(width / 24, height / 24)
                                ctx.strokeStyle = ink
                                ctx.lineWidth = 1.8
                                ctx.lineCap = "square"
                                ctx.beginPath()
                                app.paintLayoutGlyph(ctx, layoutChoice.modelData)
                                ctx.stroke()
                            }
                        }
                    }
                }
            }
            Label {
                objectName: "readingModeDefault"
                text: "Paragraph is the default · " + app.keyRich("defaultLayout") + " returns to it"
                textFormat: Text.StyledText
                color: desktop.muted; font.pixelSize: 12 * app.scale
                Layout.fillWidth: true; wrapMode: Text.Wrap
            }
            FineRule { objectName: "settingsTextSizeRule"; Layout.fillWidth: true }
            RowLayout {
                Layout.fillWidth: true
                Label { objectName: "textSizeTitle"; text: "Text size"; color: desktop.foreground; font.pixelSize: 13 * app.scale }
                Item { Layout.fillWidth: true }
                Label { objectName: "fontSizeValue"; text: bible.fontSize; color: desktop.accent; font.pixelSize: 14 * app.scale }
            }
            SquareSlider {
                id: fontSizeSliderItem
                objectName: "fontSizeSlider"
                Layout.fillWidth: true
                from: 8; to: 40; stepSize: 1
                snapMode: Slider.SnapAlways
                value: bible.fontSize
                onMoved: bible.setFontSize(Math.round(value))
                Accessible.name: "Text size"
            }
            HintRow {
                objectName: "fontSizeHints"
                theme: desktop
                // With the slider focused, its arrows adjust it too.
                parts: (fontSizeSliderItem.activeFocus ? [[["Left", "Right"], "adjust"]] : []).concat(["smaller", "larger", "resetText"].map(function(id, index) {
                    return [app.actionKeys(id)[0], ["decrease", "increase", "reset"][index], () => app.runCommand(id), app.actionAvailable(id)]
                }))
                pixelSize: 11 * app.scale
                Layout.fillWidth: true
            }
            FineRule { objectName: "settingsLineHeightRule"; Layout.fillWidth: true }
            RowLayout {
                Layout.fillWidth: true
                Label { objectName: "lineHeightTitle"; text: "Line height"; color: desktop.foreground; font.pixelSize: 13 * app.scale }
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
                // With the slider focused, its arrows adjust it too.
                parts: (lineHeightSlider.activeFocus ? [[["Left", "Right"], "adjust"]] : []).concat(["lineLess", "lineMore", "lineReset"].map(function(id, index) {
                    return [app.actionKeys(id)[0], ["decrease", "increase", "reset"][index], () => app.runCommand(id), app.actionAvailable(id)]
                }))
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
        contentItem: ColumnLayout {
            spacing: 12 * app.scale
            KeyScrollView {
                id: aboutScroll
                objectName: "aboutScroll"
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(aboutContent.implicitHeight, app.height - 150)
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
                    Label { text: "An offline Bible reader with local highlights, tags, and Markdown notes."; color: desktop.foreground; wrapMode: Text.Wrap; Layout.fillWidth: true; font.pixelSize: 14 * app.scale; lineHeight: 1.4 }
                    FineRule { Layout.fillWidth: true }
                    Label { objectName: "aboutEdition"; text: bible.ready ? bible.edition : "No version installed"; textFormat: Text.PlainText; color: desktop.foreground; wrapMode: Text.Wrap; Layout.fillWidth: true; font.pixelSize: 14 * app.scale }
                    Label { objectName: "aboutCopyright"; text: bible.versionCopyright; textFormat: Text.PlainText; visible: text.length > 0; color: desktop.muted; wrapMode: Text.Wrap; Layout.fillWidth: true; font.pixelSize: 12 * app.scale; lineHeight: 1.4 }
                    Label { objectName: "aboutSources"; text: "Install versions directly from eBible.org or import your own local USFM ZIP. Bible text is stored in this app's local library. No connection is needed to read, highlight, tag, or write notes after import."; color: desktop.muted; wrapMode: Text.Wrap; Layout.fillWidth: true; font.pixelSize: 12 * app.scale }
                    QuietButton {
                        readonly property var entry: Array.from(catalog.entries).find(function(version) { return version.id === bible.versionId })
                        text: "Version details & permissions ↗"; visible: !!entry; compact: true
                        onClicked: desktop.openExternal(entry.detailsUrl)
                    }
                    Label { objectName: "aboutCrossReferences"; text: "Cross-references beyond a version's own: OpenBible.info (openbible.info/labs/cross-references), licensed CC BY. Community-ranked; they follow the version's own in See also."; color: desktop.muted; wrapMode: Text.Wrap; Layout.fillWidth: true; font.pixelSize: 12 * app.scale }
                    Label { text: desktop.appName + " " + desktop.version + " · Application code licensed under MIT\nNo account required"; color: desktop.muted; font.pixelSize: 11 * app.scale; wrapMode: Text.Wrap; Layout.fillWidth: true }
                    QuietButton { text: "Open local data folder ↗"; compact: true; onClicked: desktop.openExternal("file://" + bible.dataDirectory()) }
                }
            }
            HintRow {
                objectName: "aboutHints"
                Layout.fillWidth: true
                theme: desktop
                pixelSize: 11 * app.scale
                parts: [[["J", "K"], "scroll"], ["Escape", "close", function() { aboutDialog.close() }]]
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
            // The guide's keys work from anywhere in it (buttons, rows), not
            // just the search field: ↑ ↓ choose, enter installs, shift enter
            // details, ctrl i imports a ZIP (keys a focused control leaves alone).
            Keys.onShortcutOverride: function(event) {
                if ((event.modifiers & ~Qt.KeypadModifier) === Qt.ControlModifier && event.key === Qt.Key_I) event.accepted = true
            }
            Keys.onPressed: function(event) { importGuide.guideKey(event) }
            Label { text: "Add version"; color: desktop.foreground; font.pixelSize: 22 * importGuide.uiScale; Layout.fillWidth: true; wrapMode: Text.Wrap }
            TextField {
                id: catalogSearch
                ContextMenu.menu: null
                ContextMenu.onRequested: function(position) { app.openTextMenu(catalogSearch, position) }
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
                                // Current: the accent tint; hovered: the surface tint.
                                visible: catalogRow.current || catalogRowMouse.containsMouse
                                color: catalogRow.current ? app.tint(desktop.accent, 0.11) : desktop.surface
                                radius: Math.min(desktop.cornerRadius, width / 2, height / 2)
                            }
                            MouseArea {
                                id: catalogRowMouse
                                objectName: "catalogRowMouse:" + catalogRow.modelData.id
                                anchors.fill: parent
                                acceptedButtons: Qt.LeftButton
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: importGuide.catalogIndex = catalogRow.index
                            }
                        ColumnLayout {
                            id: catalogRowContent
                            width: parent.width
                            spacing: 6 * app.scale
                            readonly property bool downloading: catalog.busy && app.downloadId === catalogRow.modelData.id
                            Label { text: catalogRow.modelData.abbreviation + " · " + catalogRow.modelData.name; textFormat: Text.PlainText; color: desktop.foreground; font.pixelSize: 14 * app.scale; wrapMode: Text.Wrap; Layout.fillWidth: true }
                            // What it is, when, how big, under what terms, and what it carries.
                            Label { objectName: "catalogDescription:" + catalogRow.modelData.id; text: catalogRow.modelData.description; textFormat: Text.PlainText; color: desktop.foreground; font.pixelSize: 13 * app.scale; wrapMode: Text.Wrap; Layout.fillWidth: true }
                            Label {
                                objectName: "catalogFacts:" + catalogRow.modelData.id
                                text: [catalogRow.modelData.year, app.downloadSize(catalogRow.modelData.size), catalogRow.modelData.license]
                                    .concat(Array.from(catalogRow.modelData.features).map(app.featureName)).join(" · ")
                                textFormat: Text.PlainText; color: desktop.muted; font.pixelSize: 12 * app.scale; wrapMode: Text.Wrap; Layout.fillWidth: true
                            }
                            // The download's progress, in its own card.
                            ProgressBar { objectName: "catalogProgress:" + catalogRow.modelData.id; visible: catalogRowContent.downloading; Layout.fillWidth: true; from: 0; to: 1; value: Math.max(0, Math.min(1, catalog.progress)); indeterminate: catalog.progress <= 0 }
                            Label { objectName: "catalogStatus:" + catalogRow.modelData.id; visible: catalogRowContent.downloading; text: "Downloading… " + (catalog.progress > 0 ? Math.round(catalog.progress * 100) + "%" : ""); color: desktop.foreground; font.pixelSize: 12 * app.scale; Layout.fillWidth: true; wrapMode: Text.Wrap }
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
                        text: "Importing the same edition replaces its Bible data and preserves your notes, highlights, tags, and settings. Adding another supported version keeps the installed editions. Marks belong to their own version."
                        color: desktop.accent; font.pixelSize: 14 * app.scale; wrapMode: Text.Wrap; Layout.fillWidth: true
                    }
                    Label { objectName: "importDataPath"; text: bible.dataDirectory(); textFormat: Text.PlainText; color: desktop.muted; font.pixelSize: 12 * app.scale; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
                    Label { objectName: "importOffline"; text: "After a successful import, you can move or delete the source USFM ZIP. No connection is needed to read or use your annotations."; color: desktop.muted; font.pixelSize: 13 * app.scale; wrapMode: Text.Wrap; Layout.fillWidth: true }
                }
            }
            Label { objectName: "catalogError"; text: catalog.error || app.downloadMessage || bible.error; visible: text.length > 0; textFormat: Text.PlainText; color: desktop.error; Layout.fillWidth: true; wrapMode: Text.Wrap; Accessible.role: Accessible.AlertMessage }
            RowLayout {
                visible: catalog.busy || ((catalog.error.length > 0 || app.downloadMessage.length > 0) && app.downloadId.length > 0)
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                QuietButton { objectName: "catalogCancel"; text: "Cancel"; focusTarget: catalogSearch; onClicked: app.cancelDownload() }
                QuietButton { objectName: "catalogRetry"; text: "Retry"; focusTarget: catalogSearch; visible: !catalog.busy; enabled: !bible.busy; onClicked: { if (app.pendingDownload.toString().length) app.importDownloaded(); else app.installVersion(app.downloadId) } }
            }
            FineRule { Layout.fillWidth: true }
            RowLayout {
                Layout.fillWidth: true
                // A local USFM ZIP: a quiet link beside the catalog, not its main action.
                QuietButton {
                    id: importChoose
                    objectName: "importChoose"
                    text: "Import USFM ZIP…"
                    font.pixelSize: 13 * importGuide.uiScale
                    compact: true
                    enabled: !bible.busy && !catalog.busy
                    tip: "Import a local USFM ZIP · " + KeyNotation.format("Ctrl+I")
                    onClicked: { if (bible.busy || catalog.busy || !app.flushNote()) return; importGuide.choosePending = true; importGuide.close() }
                }
                Item { Layout.fillWidth: true }
                QuietButton { objectName: "importCancel"; text: "Close"; font.pixelSize: 14 * importGuide.uiScale; onClicked: importGuide.close() }
            }
            HintRow {
                objectName: "importGuideHints"
                // Short windows keep the room for the catalog (the keys still work).
                visible: importGuide.height >= 520 * importGuide.uiScale
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
        onClosed: { if (importGuide.visible) catalogSearch.forceActiveFocus(); else if (versionDialog.visible) versionResults.forceActiveFocus(); else app.restorePanelFocus() }
        onVisibleChanged: if (visible) { detailsScroll.contentItem.contentY = 0; detailsScroll.forceActiveFocus() }
        contentItem: ColumnLayout {
            spacing: 12 * app.scale
            KeyScrollView {
                id: detailsScroll
                objectName: "detailsScroll"
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(detailsContent.implicitHeight, app.height - 150)
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
            HintRow {
                objectName: "detailsHints"
                Layout.fillWidth: true
                theme: desktop
                pixelSize: 11 * app.scale
                parts: [[["J", "K"], "scroll"], ["Escape", "close", function() { catalogDetails.close() }]]
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
        title: "Export your marks as Markdown — choose a folder"
        onAccepted: { if (!app.flushNote()) return; if (bible.exportNotes(selectedFolder)) app.notify("Exported your marks as Markdown", true) }
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
            // ← → move between Cancel and Choose file…; the scroll keys scroll.
            Keys.onPressed: function(event) {
                if ((event.key === Qt.Key_Left || event.key === Qt.Key_Right) && event.modifiers === Qt.NoModifier) {
                    if (backupCancel.activeFocus && backupChoose.enabled) backupChoose.forceActiveFocus(); else backupCancel.forceActiveFocus()
                    event.accepted = true
                    return
                }
                app.scrollKey(backupScopeScroll.contentItem as Flickable, event)
            }
            Label {
                text: "Back up your marks"
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
                    text: "This backup holds the highlights, notes, and tags of every installed version, with the Desk and each version's reading place.\n\n"
                        + "Marks kept for a removed version are not included: reimport it first.\n\n"
                        + "Bible text, source USFM files, History, the ribbon, and preferences are not included."
                    textFormat: Text.PlainText; color: desktop.foreground
                    font.pixelSize: 14 * app.scale; wrapMode: Text.Wrap
                }
            }
            FineRule { Layout.fillWidth: true }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                QuietButton {
                    id: backupCancel
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
            HintRow {
                objectName: "backupScopeHints"
                theme: desktop
                parts: [app.confirmationHintParts(backupCancel, backupChoose, backupScope)[0], [["J", "K"], "scroll"]]
                    .concat(app.confirmationHintParts(backupCancel, backupChoose, backupScope).slice(1))
                pixelSize: 11 * app.scale
                Layout.fillWidth: true
            }
        }
    }
    FileDialog {
        id: backupDialog
        objectName: "backupDialog"
        title: "Back up your marks"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "json"
        nameFilters: ["JSON backup (*.json)"]
        onAccepted: { if (!app.flushNote()) return; if (bible.exportBackup(selectedFile)) app.notify("Backup saved", true) }
        onVisibleChanged: { if (!visible) app.restorePanelFocus() }
    }
    FileDialog {
        id: restoreDialog
        objectName: "restoreDialog"
        title: "Restore a backup of your marks"
        fileMode: FileDialog.OpenFile
        nameFilters: ["JSON backup (*.json)"]
        onAccepted: app.requestRestore(selectedFile)
        // Cancelled: the keyboard returns to the panel or reader it came from.
        onRejected: app.restorePanelFocus()
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
                text: "This replaces the highlights, notes, tags, Desk passages, and reading place of each version in the backup; other versions are untouched. A backup from an earlier " + desktop.appName + " holds one version, and restores into it while you read it (" + bible.versionAbbreviation + " now). Your Bible text and preferences stay as they are."
                color: desktop.foreground
                font.pixelSize: 14 * app.scale
                Layout.fillWidth: true
                wrapMode: Text.Wrap
            }
            Label {
                visible: bible.error.length > 0
                text: bible.error
                color: desktop.error
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

    // Which key (what can follow a waiting key) and, once, the keys to start
    // with: one strip above the footer, centered beside docked Notes.
    function introParts() {
        return [[["J", "K"], "verses"], [["H", "L"], "chapters"], [actionKeys("books")[0], "go to", function() { app.runCommand("books") }],
            [actionKeys("bibleSearch")[0], "search", function() { app.runCommand("bibleSearch") }], [actionKeys("notes")[0], "note", function() { app.runCommand("notes") }],
            [actionKeys("help")[0], "all keys", function() { app.dismissKeyStrip(); app.runCommand("help") }]]
    }
    Rectangle {
        id: keyStrip
        objectName: "keyStrip"
        parent: Overlay.overlay
        readonly property bool whichKey: app.whichKeyShown && app.whichKeyParts().length > 0
        visible: (whichKey || app.keyStripShown) && bible.ready && !app.modalOpen && !app.findPromptOpen
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.horizontalCenterOffset: -app.chromeDock / 2
        anchors.bottom: parent.bottom
        anchors.bottomMargin: app.footerMargin + app.footerHeight + 12 * app.controlScale
        width: Math.min(560 * app.scale, app.stageWidth - 48)
        height: keyStripRow.implicitHeight + 16 * app.scale
        radius: Math.min(desktop.cornerRadius, height / 2)
        color: desktop.surface
        border.color: desktop.border
        RowLayout {
            id: keyStripRow
            anchors.verticalCenter: parent.verticalCenter
            x: 16 * app.scale
            width: parent.width - 2 * x
            spacing: 12 * app.scale
            Label {
                objectName: "keyStripPending"
                visible: keyStrip.whichKey
                text: app.zPending ? "z" : app.charMode && app.charPending.length ? app.charPending : app.verseCount
                color: desktop.accent; font.pixelSize: 13 * app.scale; font.weight: Font.DemiBold
            }
            HintRow {
                objectName: "keyStripHints"
                theme: desktop
                parts: keyStrip.whichKey ? app.whichKeyParts() : app.introParts()
                pixelSize: 12 * app.scale
                Layout.fillWidth: true
            }
            QuietButton {
                objectName: "keyStripClose"
                visible: !keyStrip.whichKey
                glyph: "close-down"; compact: true
                keys: KeyNotation.format("Escape"); tip: "Close · " + keys
                Accessible.name: "Close the key strip"
                onClicked: app.dismissKeyStrip()
            }
        }
    }
    // The selection mode, named above the footer's mode button.
    Rectangle {
        objectName: "modePill"
        visible: bible.ready && (app.verseMode || app.charMode) && !app.findPromptOpen && modeControl.visible
        x: leftActions.x + modeControl.x + modeControl.width / 2 - width / 2
        y: leftActions.y - height - 6 * app.controlScale
        width: modePillLabel.implicitWidth + 16 * app.scale
        height: modePillLabel.implicitHeight + 6 * app.scale
        radius: height / 2
        color: app.tint(desktop.accent, 0.14)
        border.color: app.tint(desktop.accent, 0.4)
        Label {
            id: modePillLabel
            objectName: "modePillLabel"
            anchors.centerIn: parent
            text: app.charMode ? "CHARACTERS" : "VISUAL"
            color: desktop.accent
            font.pixelSize: 10.5 * app.scale; font.weight: Font.DemiBold; font.letterSpacing: 1
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
            QuietButton { id: errorDismiss; objectName: "errorDialogDismiss"; text: "Dismiss"; Layout.alignment: Qt.AlignRight; onClicked: errorDialog.close() }
            HintRow {
                objectName: "errorDialogHints"
                theme: desktop
                parts: [["Return", "dismiss", function() { errorDialog.close() }], ["Escape", "close", function() { errorDialog.close() }]]
                pixelSize: 11 * app.scale
                Layout.fillWidth: true
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
        anchors.horizontalCenterOffset: -app.chromeDock / 2
        anchors.bottom: parent.bottom
        anchors.bottomMargin: bible.focusMode ? 68 : Math.max(68, app.footerMargin + app.footerHeight + 12 * app.controlScale)
        width: Math.min(toast.implicitWidth + 36, app.stageWidth - 2 * Math.max(24, app.footerRightInset))
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
            if (app.marksOpen && app.marginKeys) app.moveSaved(0)
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
            app.followRestored()
        }
    }
    // Follow: what this window reads (the passage, and the verse at the top
    // with how far into it), sent to the window beside a moment after it
    // changes; and the window beside's, taken without sending it back.
    Timer { id: followTimer; interval: 30; onTriggered: app.emitFollow() }
    Connections {
        target: bible
        enabled: app.following
        function onSelectionChanged() { if (!app.followApplying) followTimer.restart() }
    }
    property bool followApplying: false
    property var pendingFollow: null
    function verseStartY(number) {
        const block = blockLoader.item as BlockReader
        if (block) return blockLoader.mapToItem(readerColumn, 0, block.verseRect(number).y).y
        const index = Array.from(bible.verses).findIndex(function(verse) { return verse.number === number })
        const row = index >= 0 ? verseRepeater.itemAt(index) as VerseItem : null
        return row ? row.mapToItem(readerColumn, 0, 0).y + row.textOffset : null
    }
    // The verse at the top of the view and how far into it (0 to 1) the view begins.
    function followTop() {
        const top = readerTargetY(), verses = Array.from(bible.verses)
        let previous = null
        for (const verse of verses) {
            const y = verseStartY(verse.number)
            if (y === null) continue
            if (y > top) {
                if (!previous) return { verse: verse.number, fraction: 0 }
                return { verse: previous.verse, fraction: Math.max(0, Math.min(1, (top - previous.y) / Math.max(1, y - previous.y))) }
            }
            previous = { verse: verse.number, y: y }
        }
        if (!previous) return { verse: 0, fraction: 0 }
        return { verse: previous.verse, fraction: Math.max(0, Math.min(1, (top - previous.y) / Math.max(1, readerColumn.height - previous.y))) }
    }
    function followLocation() {
        const top = followTop()
        return { book: bible.book, chapter: bible.chapter, scope: hasSelection ? bible.tagScope() : null,
                 topVerse: top.verse, topFraction: top.fraction }
    }
    function emitFollow() {
        if (!following || followApplying || restoringPosition || !bible.ready || bible.busy) return
        followMoved(followLocation())
    }
    function followTo(location) {
        if (!location || !bible.ready || bible.busy) return
        const scope = location.scope ? Object.assign({}, location.scope, { versionId: bible.versionId })
            : { versionId: bible.versionId, book: location.book, chapter: location.chapter, start: 0, endBook: location.book, endChapter: location.chapter, end: 0 }
        const here = hasSelection ? bible.tagScope() : null
        const same = location.book === bible.book && location.chapter === bible.chapter
            && (scope.start === 0 ? !here : !!here && ["book", "chapter", "start", "endBook", "endChapter", "end"].every(function(key) { return here[key] === scope[key] }))
        followApplying = true
        if (same) {
            // Mid-restore (a window just opened, or a version just loaded): its restore applies it.
            if (restoringPosition) { pendingFollow = location; return }
            applyFollowScroll(location)
            Qt.callLater(function() { app.followApplying = false })
            return
        }
        if (!flushNote()) { followApplying = false; return }
        pendingFollow = location
        // Where this version lacks the verses: their chapter; lacking that, nothing moves.
        if (!bible.restoreSelection(scope, false, -1)
                && !bible.restoreSelection({ versionId: bible.versionId, book: location.book, chapter: location.chapter, start: 0, end: 0 }, false, -1)) {
            pendingFollow = null
            followApplying = false
            bible.clearError()
            notify("Not in " + bible.versionAbbreviation, true)
            return
        }
        syncNote()
    }
    // After a followed move's chapter is laid out: its scroll, then the window listens again.
    function followRestored() {
        if (!pendingFollow) { if (following && !followApplying) followTimer.restart(); return }
        applyFollowScroll(pendingFollow)
        pendingFollow = null
        Qt.callLater(function() { app.followApplying = false })
    }
    function applyFollowScroll(location) {
        if (!location.topVerse) return
        const y = verseStartY(location.topVerse)
        if (y === null) return
        let next = null
        for (const verse of Array.from(bible.verses)) if (verse.number > location.topVerse) { next = verseStartY(verse.number); break }
        const span = next !== null ? next - y : Math.max(0, readerColumn.height - y)
        const target = clampReaderY(y + location.topFraction * span)
        if (Math.abs(target - reading.contentY) >= 1) setReaderY(target)
    }
    function openBeside(versionId) {
        if (!bible.ready || bible.busy || !flushNote()) { rejected(); return }
        besideRequested(versionId)
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
            if (app.paletteTextMode) app.scheduleBibleSearch()
        }
        function onSearchChanged() { Qt.callLater(app.restoreSearchPalette) }
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
            // New marks (an undo while selecting characters) keep a character
            // selection; another chapter ends it (onNavigationChanged).
            app.relayoutTextSelection()
            if (app.selectionBarWanted) Qt.callLater(app.placeSelectionBar)
            app.draggingText = false
            app.dragAnchorRow = -1
        }
        // Open Notes follows the selection, however it moves.
        function onSelectionChanged() { if (app.notesOpen && !noteEditor.activeFocus) Qt.callLater(app.syncNote) }
        function onDeskChanged() { app.syncDesk() }
        function onNavigationChanged() {
            app.selectionBarWanted = false
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
            if (!bible.historyEnabled) { historyDwellTimer.stop(); if (paletteHistory) resetBooks() }
            else if (app.dwellPending && !historyDwellTimer.running) historyDwellTimer.restart()
        }
    }
    Component.onCompleted: {
        syncVerseRows()
        focusLayout = bible.focusMode; marginFocusLevel = focusLayout ? 0 : 1; marginNotesOn = bible.marginNotes; outlineOn = bible.chapterOutline
        readerInitialized = true; syncNote(); loadSavedDesk(); restorePosition(); restartDwell(); scheduleMargins()
    }
    onClosing: function(close) {
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
            // Checked again on activation: the enabled binding can lag a focus
            // change inside a popup (Notes), and typing must keep its keys.
            onActivated: { if (!app.shortcutEnabled(modelData)) return; app.runKey(modelData.id); app.beginHold(modelData.id) }
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
    // alt v in the Margin (a note's editor, preview, Linked from; the Desk's);
    // the drawer is a popup, so the application context reaches it in the
    // active window only.
    Shortcut {
        objectName: "notePreviewShortcut"
        context: Qt.ApplicationShortcut
        sequences: app.actionKeys("notePreview")
        // Without a selection there is no note: the key sounds the error.
        enabled: app.active && app.marginOpen && app.focusWithin(notesDrawer.contentItem) && !app.modalOpen
        onActivated: if (app.deskOpen) app.toggleDeskPreview(); else app.toggleNotePreview()
    }
    Shortcut { sequence: "Home"; enabled: app.readerKeys; onActivated: app.chapterEdge(false) }
    Shortcut { sequence: "End"; enabled: app.readerKeys; onActivated: app.chapterEdge(true) }
    Shortcut {
        sequence: "Escape"
        enabled: app.readerKeys
        onActivated: {
            if (textPopover.visible) { textPopover.close(); return }
            if (app.keyStripShown) { app.dismissKeyStrip(); return }
            if (app.selectionBarShown) { app.selectionBarWanted = false; return }
            if (app.marginOpen) {
                // Open Notes closes before the reading state it follows (search,
                // selection) changes; a text selection or a count goes first.
                // (The editor's esc only brought the keyboard here.)
                if (app.freeSelectedText.length) { app.clearTextSelection(); app.readerFocus() }
                else if (app.verseCount.length) app.clearCount()
                else if (app.closeNotes()) app.readerFocus()
                return
            }
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
