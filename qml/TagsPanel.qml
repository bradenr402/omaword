pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "KeyNotation.js" as KeyNotation
import "RowMenu.js" as RowMenu

// Two surfaces share the tag state: the modal tag picker (with Manage tags)
// and the Study drawer (Cross-references · Footnotes · Tags). The picker may
// open over the Study and Annotations drawers; they stay open beneath it.
Item {
    id: panel
    required property var app
    required property var bible
    required property var desktop

    readonly property string hostKind: surface.visible ? "picker" : relatedDrawer.visible ? "related" : "none"
    readonly property bool visibleModal: surface.visible || relatedDrawer.visible
    readonly property bool pickerVisible: surface.visible
    readonly property bool relatedShown: relatedDrawer.visible && !relatedDrawer.closing
    // The Study drawer itself, for the reader's outside-click catcher.
    readonly property Drawer studyDrawer: relatedDrawer
    readonly property bool contentVisible: visibleModal
    readonly property bool scopePinned: surface.visible && editingScope !== null
    property var editingScope: null
    // Study's root passage: the reader's passage when Study opened. It follows
    // the reader, except that opening a row from inside the panel keeps it
    // (while the panel keeps focus) so the list stays put.
    property var relatedSource: null
    // The shared panel header's subtitle (Annotations and Study): the reader's
    // selection, or its chapter when nothing is selected — never blank.
    readonly property string passageSubtitle: (bible.selectedStart > 0 && bible.selectionReference
        ? bible.selectionReference : bible.passage) + (bible.versionAbbreviation ? " · " + bible.versionAbbreviation : "")
    // Study panel (the Related drawer, now tabbed): the passage's cross
    // references, the current chapter's footnotes, and other passages sharing
    // its tags (internal id "related", shown as "Tags"). ←/→ (h/l) switch
    // tabs; every tab shares the list keys and motions. Opens on
    // Cross-references, then remembers the last tab for this window.
    readonly property var studyTabs: ["crossrefs", "footnotes", "related"]
    readonly property var studyTitles: ["Cross-references", "Footnotes", "Tags"]
    property string studyTab: "crossrefs"
    property var crossRows: []
    readonly property var footnoteRows: Array.from(bible.footnotes || [])
    readonly property var studyRows: studyTab === "crossrefs" ? crossRows : studyTab === "footnotes" ? footnoteRows : []
    readonly property bool studyListTab: relatedDrawer.visible && studyTab !== "related"
    readonly property bool tagsTab: relatedDrawer.visible && studyTab === "related"
    function setStudyTab(tab, note) {
        if (studyTabs.indexOf(tab) < 0) return
        studyTab = tab
        refreshStudy()
        studyList.currentIndex = studyRows.length ? 0 : -1
        if (tab === "footnotes") {
            // Start at the requested note, else the cursor verse's first note.
            const verse = note ? note.verse : bible.cursorVerse
            const index = footnoteRows.findIndex(function(row) {
                return row.verse === verse && (!note || !note.seq || row.seq === note.seq)
            })
            if (index >= 0) studyList.currentIndex = index
        }
        if (studyList.currentIndex >= 0) studyList.positionViewAtIndex(studyList.currentIndex, ListView.Contain)
        Qt.callLater(function() { if (panel.relatedShown) panel.focusResults() })
    }
    function stepStudyTab(delta) {
        const index = studyTabs.indexOf(studyTab)
        setStudyTab(studyTabs[(index + delta + studyTabs.length) % studyTabs.length])
    }
    function refreshStudy() {
        crossRows = studyTab === "crossrefs" && installed(relatedSource) ? Array.from(bible.crossReferences(relatedSource)) : []
    }
    function footnoteScope(note) {
        if (!note) return null
        return { versionId: bible.versionId, reference: bible.passage + ":" + note.verse, book: bible.book, chapter: bible.chapter,
            start: note.verse, endBook: bible.book, endChapter: bible.chapter, end: note.verse }
    }
    function studyScope(row) { return studyTab === "footnotes" ? footnoteScope(row) : row || null }
    function rejected() { if (typeof app.rejected === "function") app.rejected() }
    function openStudyRow(index) {
        const row = studyRows[index]
        if (!row) { rejected(); return }
        studyList.currentIndex = index
        openTagged(studyScope(row))
    }

    // ── Study → Tags: "Where else have I tagged what this passage is about?"
    // Chips are the root's tags: exact-range tags, then tags on overlapping
    // ranges. Results are the other passages sharing any chip that is on,
    // ranked by shared count, then canonical order (the backend's order).
    property var rootChips: []
    property var chipOff: []
    property bool allVersions: false
    property var allResults: []
    property var results: []
    property bool chipsExpanded: false
    property int chipLimit: 1000
    property int chipIndex: 0
    property string studyQuery: ""
    property bool studySearchOpen: false
    readonly property var onIds: rootChips.filter(function(chip) { return panel.chipOff.indexOf(chip.id) < 0 })
        .map(function(chip) { return chip.id })
    readonly property bool otherVersions: Array.from(bible.versions).length > 1
    readonly property var visibleResults: results.filter(function(entry) { return panel.resultMatches(entry) })
    readonly property int hiddenChips: chipsExpanded ? 0 : Math.max(0, rootChips.length - chipLimit)
    function chipCount(id) {
        return allResults.filter(function(entry) {
            return Array.from(entry.tags || []).some(function(tag) { return tag.id === id })
        }).length
    }
    function resultMatches(entry) {
        const query = studyQuery.trim().toLowerCase()
        return !query.length || [entry.reference, tagNames(entry.tags), entry.excerpt || ""].join("\n").toLowerCase().indexOf(query) >= 0
    }
    function resetChips() {
        let chips = []
        if (installed(relatedSource)) {
            Array.from(bible.tagsForScope(relatedSource, false)).forEach(function(tag) {
                chips.push({ id: tag.id, name: tag.name, overlap: false, reference: "" })
            })
            Array.from(bible.tagsForScope(relatedSource, true)).forEach(function(range) {
                if (sameRange(range, relatedSource)) return
                Array.from(range.tags).forEach(function(tag) {
                    if (!chips.some(function(chip) { return chip.id === tag.id }))
                        chips.push({ id: tag.id, name: tag.name, overlap: true, reference: range.reference })
                })
            })
        }
        const ids = chips.map(function(chip) { return chip.id })
        chipOff = chipOff.filter(function(id) { return ids.indexOf(id) >= 0 })
        rootChips = chips
        chipLimit = 1000
        chipIndex = Math.min(chipIndex, Math.max(0, chips.length - 1))
    }
    function refreshTagResults() {
        const ids = rootChips.map(function(chip) { return chip.id })
        allResults = installed(relatedSource) && ids.length ? Array.from(bible.relatedPassages(relatedSource, ids, false, allVersions)) : []
        results = installed(relatedSource) && onIds.length ? Array.from(bible.relatedPassages(relatedSource, onIds, false, allVersions)) : []
        if (resultList.currentIndex >= resultList.count) resultList.currentIndex = resultList.count - 1
        if (resultList.currentIndex < 0 && resultList.count) resultList.currentIndex = 0
    }
    function setChipOff(ids) {
        chipOff = ids
        refreshTagResults()
        resultList.currentIndex = resultList.count ? 0 : -1
    }
    function toggleChip(id) {
        const off = chipOff.slice()
        const index = off.indexOf(id)
        if (index < 0) off.push(id); else off.splice(index, 1)
        setChipOff(off)
    }
    // Solo: only this chip on; soloing it again turns every chip back on.
    function soloChip(id) {
        if (onIds.length === 1 && onIds[0] === id) setChipOff([])
        else setChipOff(rootChips.map(function(chip) { return chip.id }).filter(function(other) { return other !== id }))
    }
    function restoreChips() { setChipOff([]) }
    // A chip's right-click menu, and the passage count's (Study → Tags).
    function chipMenu(chip, sceneX, sceneY) {
        const only = onIds.length === 1 && onIds[0] === chip.id
        app.showContextMenu("studyChip", [
            { id: "soloChip", text: "Show only this tag", shortcut: KeyNotation.format("Shift+Space"), enabled: !only,
                run: function() { panel.soloChip(chip.id) } },
            { id: "allChips", text: "Show all tags", shortcut: KeyNotation.format("0"), enabled: chipOff.length > 0,
                run: function() { panel.restoreChips() } },
            { separator: true },
            { id: "editTags", text: "Edit tags…", shortcut: KeyNotation.format("T"), enabled: !bible.busy && !app.rangeEditing && installed(relatedSource),
                run: function() { panel.app.openPassageTags(panel.snapshot(panel.relatedSource)) } }
        ], sceneX, sceneY)
    }
    function countMenu(sceneX, sceneY) {
        app.showContextMenu("studyCount", [
            { id: "allVersions", text: allVersions ? "This version only" : "All versions", shortcut: KeyNotation.format("A"), enabled: otherVersions,
                run: function() { panel.toggleAllVersions() } },
            { id: "searchPassages", text: "Search passages…", shortcut: KeyNotation.format("/"), enabled: tagsTab,
                run: function() { panel.showStudySearch() } }
        ], sceneX, sceneY)
    }
    function toggleAllVersions() {
        allVersions = !allVersions
        refreshTagResults()
    }
    readonly property bool chipFocused: !!app.activeFocusItem && app.activeFocusItem.studyChip === true
    function chipItems() {
        const items = []
        for (let i = 0; i < chipRepeater.count; ++i) {
            const cell = chipRepeater.itemAt(i)
            // The cell's only Button is its chip.
            if (cell && cell.visible) items.push(cell.children[cell.children.length - 1])
        }
        if (moreChips.visible) items.push(moreChips)
        return items
    }
    function focusChips() {
        const items = chipItems()
        if (!items.length) return false
        items[Math.max(0, Math.min(items.length - 1, chipIndex))].forceActiveFocus()
        return true
    }
    function moveChip(delta) {
        const items = chipItems()
        if (!items.length) return
        const current = items.indexOf(app.activeFocusItem)
        chipIndex = Math.max(0, Math.min(items.length - 1, (current < 0 ? chipIndex : current) + delta))
        items[chipIndex].forceActiveFocus()
    }
    function showStudySearch() {
        if (!tagsTab) return
        studySearchOpen = true
        Qt.callLater(function() { studySearch.forceActiveFocus() })
    }
    function leaveStudySearch() {
        studySearchOpen = false
        focusResults()
    }
    function tagCurrentResult() {
        if (resultList.activeFocus || !chipFocused) {
            const entry = visibleResults[resultList.currentIndex]
            if (entry) { app.openPassageTags(panel.snapshot(entry)); return }
        }
        if (!rootChips.length && installed(relatedSource)) app.openPassageTags(panel.snapshot(relatedSource))
    }
    function openResultMenu(index, sceneX, sceneY) {
        const entry = visibleResults[index]
        if (!entry || typeof app.showContextMenu !== "function") return
        resultList.currentIndex = index
        resultList.forceActiveFocus()
        const ready = !app.rangeEditing && !bible.busy
        app.showContextMenu("study", RowMenu.entries(app, { busy: bible.busy, scope: entry, open: function() { panel.openTagged(entry) },
            extras: [{ id: "tags", text: "Edit tags…", shortcut: KeyNotation.format("T"), enabled: ready, run: function() { panel.app.openPassageTags(panel.snapshot(entry)) } },
                { id: "editRange", text: "Edit range", shortcut: KeyNotation.format("Shift+E"), enabled: ready, run: function() { panel.app.openRangeEditor(entry, "tag") } }] }),
            sceneX, sceneY)
    }
    // Cross-references and Footnotes rows: the shared row menu on the row's passage.
    function openStudyMenu(index, sceneX, sceneY) {
        const row = studyRows[index]
        if (!row || typeof app.showContextMenu !== "function") return
        studyList.currentIndex = index
        studyList.forceActiveFocus()
        app.showContextMenu("study", RowMenu.entries(app, { busy: bible.busy, scope: studyScope(row),
            openText: studyTab === "footnotes" ? "Go to verse" : "Open", open: function() { panel.openStudyRow(index) } }), sceneX, sceneY)
    }

    // ── Picker
    property string page: "picker"
    property string query: ""
    property var allTags: []
    property var exactIds: []
    property var exactTags: []
    property var overlaps: []
    property bool scopeValid: false
    property string message: ""
    property string managedId: ""
    property string mergeTarget: ""
    property var confirmation: null
    readonly property real uiScale: app && app.scale > 0 ? app.scale : desktop.textScale
    readonly property real controlScale: Math.min(uiScale, 1.4, Math.max(1, body.width / 440))
    readonly property real studyScale: Math.min(uiScale, 1.4, Math.max(1, studyBody.width / 440))
    readonly property var filteredTags: allTags.filter(function(tag) {
        return fold(tag.name).indexOf(fold(query)) !== -1
    })
    onFilteredTagsChanged: updateTagList()
    readonly property bool validName: query.trim().length > 0 && !/[\x00-\x1f\x7f-\x9f\u2028\u2029]/.test(query)

    // Search is deliberately independent of identity: the backend alone resolves
    // NFC/case-folded names, including when Create finds an existing global tag.
    function fold(text) {
        return Array.from(String(text).normalize("NFC")).map(function(character) {
            // Upper/lower expands Unicode ligatures and sharp-s and unifies
            // final sigma. Dotless i retains its distinct default-fold identity.
            return character === "ı" ? character : character.toUpperCase().toLowerCase()
        }).join("").replace(/ß/g, "ss").normalize("NFC")
    }
    function snapshot(scope) {
        if (!scope || !scope.versionId) return null
        let captured = {}
        const keys = ["versionId", "reference", "book", "chapter", "start", "endBook", "endChapter", "end"]
        keys.forEach(function(key) {
            captured[key] = scope[key]
        })
        return captured
    }
    function installed(scope) {
        return !!scope && Array.from(bible.versions).some(function(version) { return version.id === scope.versionId })
    }
    function sameRange(a, b) {
        return a && b && ["versionId", "book", "chapter", "start", "endBook", "endChapter", "end"].every(function(key) {
            return a[key] === b[key]
        })
    }
    function tagById(id) { return allTags.find(function(tag) { return tag.id === id }) }
    function tagNames(tags) { return Array.from(tags || []).map(function(tag) { return tag.name }).join(" · ") }
    function versionName(scope) {
        if (!scope) return ""
        const version = Array.from(bible.versions).find(function(row) { return row.id === scope.versionId })
        return version ? version.abbreviation : scope.versionId
    }
    function canAct() { return !app.rangeEditing && !bible.busy && app.flushNote() }
    function failed() { message = bible.error || "Could not save this change. Please try again." }
    function releaseEditing() {
        if (!editingScope) return
        editingScope = null
        app.editingPanelClosed()
    }
    function open(scope = null) {
        if (!canAct()) return false
        const captured = snapshot(scope || bible.tagScope())
        if (captured && !installed(captured)) return false
        // Reopening another exact range ends the previous editing session first.
        if (editingScope && !sameRange(editingScope, captured)) releaseEditing()
        editingScope = captured
        page = "picker"
        query = ""
        message = ""
        confirmation = null
        refresh()
        surface.open()
        Qt.callLater(function() { search.forceActiveFocus() })
        return true
    }
    // Manage tags (ctrl t): the picker's management page.
    function openManage() {
        if (!open()) return false
        manage()
        return true
    }
    function openRelated() { return openStudy("", null) }
    // Opens Study on a tab ("" keeps the last one), optionally at a footnote {verse, seq}.
    function openStudy(tab, note) {
        if (!showRelated(bible.tagScope())) return false
        setStudyTab(tab || studyTab, note)
        return true
    }
    // Study → Tags with one chip soloed (a tag chip clicked elsewhere).
    function openRelatedForTag(id, source) {
        if (!showRelated(source || bible.tagScope())) return false
        if (id && rootChips.some(function(chip) { return chip.id === id })) soloChip(id)
        setStudyTab("related")
        return true
    }
    function showRelated(source) {
        if (!canAct()) return false
        const captured = snapshot(source)
        if (!installed(captured)) return false
        releaseEditing()
        surface.close()
        rootAt(captured)
        relatedDrawer.open()
        // The list, not the search field, takes focus: / searches.
        Qt.callLater(function() { panel.focusResults() })
        return true
    }
    // (Re)roots Study on a passage: every chip on, this version, no search.
    function rootAt(captured) {
        relatedSource = captured
        message = ""
        allVersions = false
        chipOff = []
        chipIndex = 0
        chipsExpanded = false
        chipLimit = 1000
        studyQuery = ""
        studySearchOpen = false
        allTags = Array.from(bible.tags)
        resetChips()
        refreshTagResults()
        refreshStudy()
        resultList.currentIndex = resultList.count ? 0 : -1
    }
    // Study follows the reader's passage, but not while the panel itself has
    // the keyboard: opening a row from inside keeps the list where it was.
    function followSelection() {
        if (!relatedShown || pickerVisible || focusInStudy()) return
        const captured = snapshot(bible.tagScope())
        if (!installed(captured) || sameRange(captured, relatedSource)) return
        rootAt(captured)
    }
    function close() {
        if (surface.visible) surface.close()
        else if (relatedDrawer.visible) relatedDrawer.close()
    }
    function closeForFocusMode() {
        surface.close()
        relatedDrawer.close()
    }
    function textInputActive() {
        let focused = app.activeFocusItem
        while (focused) {
            if ((focused instanceof TextEdit || focused instanceof TextInput) && !focused.readOnly) return true
            focused = focused.parent
        }
        return false
    }
    function updateTagList() {
        // Replacing a JS-array model destroys delegates and resets ListView's
        // index. Capture identity and focus before handing it the new rows.
        const oldIndex = tagList.currentIndex
        const oldTag = tagList.model && tagList.model[oldIndex]
        const oldId = oldTag ? String(oldTag.id) : ""
        const delegateFocused = !!tagList.currentItem && tagList.currentItem.activeFocus
        const listFocused = tagList.activeFocus
        const scroll = tagList.contentY - tagList.originY
        tagList.updatingModel = true
        tagList.model = filteredTags
        const index = filteredTags.findIndex(function(tag) { return tag.id === oldId })
        tagList.currentIndex = index >= 0 ? index : Math.min(Math.max(0, oldIndex), filteredTags.length - 1)
        tagList.forceLayout()
        if (delegateFocused && tagList.currentItem) tagList.currentItem.forceActiveFocus()
        else if (delegateFocused || listFocused) tagList.forceActiveFocus()
        tagList.contentY = tagList.originY + Math.max(0, Math.min(scroll, tagList.contentHeight - tagList.height))
        tagList.updatingModel = false
    }
    function refresh() {
        allTags = Array.from(bible.tags)
        const validIds = allTags.map(function(tag) { return tag.id })
        if (managedId && validIds.indexOf(managedId) < 0) managedId = ""
        if (mergeTarget && validIds.indexOf(mergeTarget) < 0) mergeTarget = ""
        if (confirmation && (!tagById(confirmation.id) || tagById(confirmation.id).name !== confirmation.name
                || (confirmation.action === "merge" && (!tagById(confirmation.target) || tagById(confirmation.target).name !== confirmation.targetName))))
            confirmation = null
        scopeValid = installed(editingScope)
        exactTags = scopeValid ? Array.from(bible.tagsForScope(editingScope, false)) : []
        exactIds = exactTags.map(function(tag) { return tag.id })
        overlaps = scopeValid ? Array.from(bible.tagsForScope(editingScope, true)).filter(function(range) {
            return !panel.sameRange(range, panel.editingScope)
        }) : []
        if (relatedDrawer.visible) {
            resetChips()
            refreshTagResults()
            refreshStudy()
        }
    }
    function toggleTag(id) {
        if (!tagById(id) || !surface.visible) return
        if (page === "manage") {
            managedId = id
            renameInput.text = tagById(id).name
            mergeTarget = ""
            // Choosing a tag (enter or click) goes on to its new name.
            Qt.callLater(function() { panel.focusRename() })
            return
        }
        if (!scopeValid || !canAct() || !installed(editingScope)) return
        const captured = snapshot(editingScope)
        if (!bible.setTag(captured, id, exactIds.indexOf(id) < 0)) { failed(); return }
        message = ""
        refresh()
        app.tagActionCompleted(captured)
    }
    function createFromQuery() {
        if (!validName || !surface.visible || page !== "picker" || !scopeValid || !canAct() || !installed(editingScope)) return
        const captured = snapshot(editingScope)
        if (!bible.createAndAssignTag(captured, query)) { failed(); return }
        query = ""
        message = ""
        refresh()
        app.tagActionCompleted(captured)
        search.forceActiveFocus()
    }
    function openTagged(entry, edit = false) {
        if (!entry || !canAct() || !installed(entry)) return false
        const captured = snapshot(entry)
        const from = app.beginJump()
        if (!bible.openTaggedPassage(captured)) { failed(); return false }
        app.endJump(from)
        app.syncNote()
        if (edit) {
            // Main captures Visual-mode completion context for the new exact range.
            app.openPassageTags(captured)
        } else if (surface.visible) surface.close()
        Qt.callLater(function() { panel.app.scrollToVerse(panel.bible.cursorVerse, true) })
        return true
    }
    function manage() {
        if (!surface.visible) return
        page = "manage"
        query = ""
        managedId = ""
        confirmation = null
        search.forceActiveFocus()
    }
    function back() {
        confirmation = null
        page = "picker"
        query = ""
        message = ""
        refresh()
        search.forceActiveFocus()
    }
    // Manage tags by key: enter renames the chosen tag (its field takes the
    // keyboard), delete asks to delete it, ctrl m picks a merge target
    // (j k ↑ ↓, enter merges), backspace in an empty filter and ctrl t go back
    // to the picker (ctrl t in the picker opens Manage).
    function focusRename() {
        if (page !== "manage" || !tagById(managedId) || confirmation) return
        renameInput.forceActiveFocus()
        renameInput.selectAll()
    }
    // The tag a management key acts on: the list's current row, else the chosen tag.
    function managementTarget() {
        if (tagList.activeFocus || (tagList.currentItem && tagList.currentItem.activeFocus)) {
            const row = filteredTags[tagList.currentIndex]
            if (row) return row.id
        }
        return managedId
    }
    function requestDeleteTag() {
        const id = managementTarget()
        if (page !== "manage" || !tagById(id) || confirmation) { app.rejected(); return }
        if (id !== managedId) { managedId = id; renameInput.text = tagById(id).name; mergeTarget = "" }
        requestManagement("delete")
    }
    function focusMerge() {
        if (page !== "manage" || !tagById(managedId) || confirmation || !mergeList.count) { app.rejected(); return }
        const at = mergeList.model.findIndex(function(tag) { return tag.id === panel.mergeTarget })
        mergeList.currentIndex = Math.max(0, at)
        mergeList.forceActiveFocus()
    }
    function moveMerge(delta) {
        if (!mergeList.count) return
        mergeList.currentIndex = Math.max(0, Math.min(mergeList.count - 1, mergeList.currentIndex + delta))
        mergeList.positionViewAtIndex(mergeList.currentIndex, ListView.Contain)
    }
    function chooseMerge() {
        const tag = mergeList.model[mergeList.currentIndex]
        if (!tag) { app.rejected(); return }
        mergeTarget = tag.id
        requestManagement("merge")
    }
    // One ctrl t is one switch: inside the popup Qt can deliver the window
    // shortcut twice for a single press (override, then the unhandled press).
    property bool pageToggled: false
    function togglePage() {
        if (pageToggled) return
        pageToggled = true
        Qt.callLater(function() { panel.pageToggled = false })
        if (confirmation || !surface.visible) { app.rejected(); return }
        if (page === "picker") manage()
        else back()
    }
    readonly property bool manageFieldFocused: renameInput.activeFocus || mergeList.activeFocus
    function requestManagement(action) {
        const tag = tagById(managedId)
        if (!tag || !canAct()) return
        if (action === "merge" && (!tagById(mergeTarget) || mergeTarget === managedId)) return
        if (action === "rename" && (!renameInput.text.trim() || /[\x00-\x1f\x7f-\x9f\u2028\u2029]/.test(renameInput.text))) {
            message = "Use a nonempty name without tabs, newlines, or control characters."
            return
        }
        confirmation = { action: action, id: tag.id, name: tag.name,
            newName: renameInput.text, target: mergeTarget,
            targetName: tagById(mergeTarget) ? tagById(mergeTarget).name : "" }
        message = ""
        Qt.callLater(function() { cancelManagement.forceActiveFocus() })
    }
    function confirmManagement() {
        const pending = confirmation
        if (!pending || !canAct() || !confirmation || !tagById(pending.id)) return
        let ok = false
        if (pending.action === "rename") ok = bible.renameTag(pending.id, pending.newName)
        else if (pending.action === "delete") ok = bible.deleteTag(pending.id)
        else if (pending.action === "merge" && tagById(pending.target)) ok = bible.mergeTags(pending.id, pending.target)
        if (!ok) { failed(); return }
        confirmation = null
        message = ""
        refresh()
        if (tagById(managedId)) renameInput.text = tagById(managedId).name
        search.forceActiveFocus()
    }
    // Clickable hint rows (HintRow): each part runs what its key would.
    function currentChip() {
        const chip = chipItems()[chipIndex]
        return chip && chip.modelData ? chip.modelData : null
    }
    function studyHintParts() {
        const tabs = [["Left", "Right"], "tabs"]
        const escape = function() { panel.handleStudyEscape() }
        if (!tagsTab) return [tabs, ["Return", studyTab === "footnotes" ? "go to verse" : "open", function() { panel.openStudyRow(studyList.currentIndex) }, listKeys],
            ["Y", "copy", function() { panel.app.copyScope(panel.listScope()) }, listKeys],
            ["D", "Desk", function() { panel.app.toggleScopeDesk(panel.listScope()) }, listKeys], ["Escape", "close", escape]]
        if (studySearch.activeFocus) return [[["Return", "Down"], "passages", function() { panel.leaveStudySearch() }], ["Escape", "leave search", function() { panel.leaveStudySearch() }]]
        const tag = ["T", "add tags", function() { panel.tagCurrentResult() }, relatedMoveKeys && !bible.busy]
        if (!rootChips.length) return [tabs, tag, ["Escape", "close", escape]]
        const chip = currentChip()
        if (chipFocused) return [[["Left", "Right"], "chips"], ["Space", "toggle", function() { panel.toggleChip(chip.id) }, !!chip],
            ["Shift+Space", "show only this tag", function() { panel.soloChip(chip.id) }, !!chip], ["0", "show all tags", function() { panel.restoreChips() }],
            ["Down", "passages"], ["Escape", "close", escape]]
        return [tabs, ["Return", "open", function() { panel.openTagged(panel.visibleResults[resultList.currentIndex]) }, listKeys], ["T", "tag", tag[2], tag[3]],
            otherVersions ? ["A", allVersions ? "this version" : "all versions", function() { panel.toggleAllVersions() }, relatedMoveKeys] : null,
            ["/", "search", function() { panel.showStudySearch() }, relatedMoveKeys], ["Escape", "close", escape]]
    }
    // The picker's enter: create or toggle from the search field, toggle in the list.
    function pickerEnter() {
        if (search.activeFocus) {
            if (page === "picker" && validName) createFromQuery()
            else if (tagList.count) toggleTag(filteredTags[Math.max(0, tagList.currentIndex)].id)
        } else if (tagList.currentIndex >= 0 && filteredTags[tagList.currentIndex]) toggleTag(filteredTags[tagList.currentIndex].id)
    }
    function pickerHintParts() {
        const inList = !search.activeFocus && tagList.currentIndex >= 0 && tagList.currentIndex < tagList.count
        const enter = search.activeFocus ? (page === "picker" && validName && scopeValid) || tagList.count > 0 : inList
        const escape = ["Escape", "close", function() { panel.handleEscape() }]
        if (confirmation) return [[["Left", "Right"], "choose"], ["Return", "confirm", function() { panel.confirmManagement() }, !bible.busy],
            ["Escape", "cancel", function() { panel.handleEscape() }]]
        const pageKey = ["Ctrl+T", page === "picker" ? "manage" : "picker", function() { panel.togglePage() }]
        if (page !== "picker") {
            const chosen = !!tagById(managedId)
            if (mergeList.activeFocus) return [[["J", "K"], "choose"], ["Return", "merge…", function() { panel.chooseMerge() }, mergeList.count > 0 && !bible.busy],
                ["Escape", "back", function() { panel.handleEscape() }]]
            if (renameInput.activeFocus) return [["Return", "rename…", function() { panel.requestManagement("rename") }, chosen && !bible.busy],
                ["Ctrl+M", "merge", function() { panel.focusMerge() }, chosen && mergeList.count > 0], ["Escape", "back", function() { panel.handleEscape() }]]
            return [["Return", "rename", function() { panel.pickerEnter() }, enter && !bible.busy],
                ["Delete", "delete…", function() { panel.requestDeleteTag() }, (inList || chosen) && !bible.busy],
                ["Ctrl+M", "merge", function() { panel.focusMerge() }, chosen && mergeList.count > 0],
                search.activeFocus && !query.length ? ["Backspace", "back", function() { panel.back() }] : pageKey, escape]
        }
        return [["Space", "toggle", function() { panel.pickerEnter() }, inList && scopeValid && !bible.busy],
            ["Return", "toggle / create", function() { panel.pickerEnter() }, enter && scopeValid && !bible.busy], pageKey, escape]
    }
    // Study keys: off while typing, behind the picker or a menu, and while closing.
    readonly property bool relatedMoveKeys: relatedShown && !pickerVisible && !app.rangeEditing && !app.contextMenuOpen
        && !textInputActive()
    readonly property bool relatedKeys: relatedMoveKeys
    readonly property bool listKeys: !bible.busy && relatedKeys && !chipFocused
        && (studyListTab ? studyList.currentIndex >= 0 : resultList.currentIndex >= 0 && resultList.count > 0)
    function listScope() {
        return studyListTab ? studyScope(studyRows[studyList.currentIndex]) : visibleResults[resultList.currentIndex] || null
    }
    function moveRelated(delta) {
        if (studyListTab) { moveStudy(delta); return }
        // Up from the first result climbs into the chips.
        if (delta < 0 && (resultList.currentIndex <= 0 || !resultList.count) && focusChips()) return
        moveResult(delta)
    }
    function moveStudy(delta) {
        if (!studyList.count) return
        studyList.currentIndex = Math.max(0, Math.min(studyList.count - 1, studyList.currentIndex + delta))
        studyList.positionViewAtIndex(studyList.currentIndex, ListView.Contain)
        studyList.forceActiveFocus()
    }
    function moveResult(delta) {
        if (!resultList.count) { resultList.forceActiveFocus(); return }
        resultList.currentIndex = Math.max(0, Math.min(resultList.count - 1, resultList.currentIndex + delta))
        resultList.positionViewAtIndex(resultList.currentIndex, ListView.Contain)
        resultList.forceActiveFocus()
    }
    function focusIn(container) {
        for (let item = app.activeFocusItem; item; item = item.parent) if (item === container) return true
        return false
    }
    function focusInBody() { return focusIn(body) || focusIn(studyBody) }
    function focusInStudy() { return focusIn(studyBody) }
    function focusResults() {
        if (studyListTab) {
            if (studyList.count && studyList.currentIndex < 0) studyList.currentIndex = 0
            studyList.forceActiveFocus()
            return
        }
        if (resultList.count && resultList.currentIndex < 0) resultList.currentIndex = 0
        resultList.forceActiveFocus()
    }
    function focusSearch() { showStudySearch() }
    // One esc closes the picker (a pending confirmation first).
    function handleEscape() {
        if (confirmation) { confirmation = null; search.forceActiveFocus() }
        // From the rename field or merge list, back to the tag list first.
        else if (page === "manage" && manageFieldFocused) focusTagList()
        else surface.close()
    }
    function focusTagList() {
        const at = filteredTags.findIndex(function(tag) { return tag.id === panel.managedId })
        if (tagList.count) { tagList.currentIndex = Math.max(0, at); tagList.forceActiveFocus() }
        else search.forceActiveFocus()
    }
    // Study: esc in the search field leaves it, keeping the query; otherwise
    // esc closes the panel, as in Annotations (reopening roots Study afresh,
    // clearing the query).
    function handleStudyEscape() {
        if (studySearch.activeFocus) leaveStudySearch()
        else relatedDrawer.close()
    }
    Connections {
        target: panel.bible
        function onTagsChanged() { if (panel.contentVisible) panel.refresh() }
        function onVersionsChanged() { if (panel.contentVisible) panel.refresh() }
        function onVersionChanged() { if (panel.relatedShown) panel.refreshTagResults() }
        function onVersesChanged() {
            // Footnotes follow the chapter; keep the current row in range.
            if (panel.studyTab === "footnotes" && studyList.currentIndex >= studyList.count) studyList.currentIndex = studyList.count - 1
        }
    }

    component TagText: Text {
        color: panel.desktop.foreground
        font.family: panel.desktop.fontFamily
        font.pixelSize: 14 * panel.controlScale
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
    }
    component TagButton: Button {
        id: control
        property bool chosen: false
        property string hint: ""
        property bool wrapText: false
        font.family: panel.desktop.fontFamily
        font.pixelSize: 13 * panel.controlScale
        padding: 8 * panel.controlScale
        implicitHeight: Math.max(34 * panel.controlScale, contentItem.implicitHeight + topPadding + bottomPadding)
        contentItem: Text {
            text: control.text
            font: control.font
            color: control.enabled ? panel.desktop.foreground : panel.desktop.muted
            textFormat: Text.PlainText
            elide: control.wrapText ? Text.ElideNone : Text.ElideRight
            wrapMode: control.wrapText ? Text.Wrap : Text.NoWrap
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            color: control.down || control.chosen || control.hovered ? panel.desktop.surface : "transparent"
            // Quiet like the app's other buttons: a border only marks focus or a chosen toggle.
            border.width: control.activeFocus || control.chosen ? 1 : 0
            border.color: panel.desktop.accent
            radius: Math.min(panel.desktop.cornerRadius, width / 2, height / 2)
        }
        // A focused button owns enter and space (never a window shortcut).
        Keys.onShortcutOverride: function(event) { if ((event.modifiers & ~Qt.KeypadModifier) === Qt.NoModifier && [Qt.Key_Space, Qt.Key_Return, Qt.Key_Enter].includes(event.key)) event.accepted = true }
        // keyActivated: this click came from enter (a merge target merges at once).
        property bool keyActivated: false
        Keys.onReturnPressed: { keyActivated = true; clicked(); keyActivated = false }
        Keys.onEnterPressed: { keyActivated = true; clicked(); keyActivated = false }
        AppToolTip { theme: panel.desktop; uiScale: panel.uiScale; text: control.hint; visible: text.length > 0 && (control.hovered || control.activeFocus) }
    }
    // Study's tag chips: "name N" (N muted: other passages with the tag). On
    // chips keep a light accent tint; off chips are a bare outline, muted.
    component StudyChip: Button {
        id: chip
        required property var modelData
        required property int index
        readonly property bool studyChip: true
        readonly property bool on: panel.onIds.indexOf(modelData.id) >= 0
        objectName: "studyChip:" + modelData.id
        focusPolicy: Qt.StrongFocus
        hoverEnabled: true
        font.family: panel.desktop.fontFamily
        font.pixelSize: 12 * panel.studyScale
        padding: 7 * panel.studyScale
        topPadding: 4 * panel.studyScale
        bottomPadding: 4 * panel.studyScale
        implicitHeight: 28 * panel.studyScale
        contentItem: Row {
            spacing: 5 * panel.studyScale
            Text {
                text: chip.modelData.name
                textFormat: Text.PlainText
                font: chip.font
                color: chip.on ? panel.desktop.foreground : panel.desktop.muted
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, studyBody.width * 0.6)
                elide: Text.ElideRight
            }
            Text {
                objectName: "studyChipCount:" + chip.modelData.id
                text: String(panel.chipCount(chip.modelData.id))
                font: chip.font
                color: panel.desktop.muted
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        background: Rectangle {
            color: chipMouse.pressed ? panel.desktop.surface
                : chip.on ? Qt.rgba(panel.desktop.accent.r, panel.desktop.accent.g, panel.desktop.accent.b, (chip.hovered || chipMouse.containsMouse) ? 0.14 : 0.08)
                : (chip.hovered || chipMouse.containsMouse) ? panel.desktop.surface : "transparent"
            border.width: 1
            border.color: chip.activeFocus ? panel.desktop.accent : (chip.hovered || chipMouse.containsMouse) ? panel.desktop.muted : panel.desktop.border
            radius: Math.min(panel.desktop.cornerRadius, width / 2, height / 2)
        }
        onActiveFocusChanged: if (activeFocus) panel.chipIndex = Math.max(0, panel.chipItems().indexOf(chip))
        // Click toggles; shift click solos the chip (again: every chip back on);
        // right click: Show only this tag / Show all tags / Edit tags.
        MouseArea {
            id: chipMouse
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: function(mouse) {
                chip.forceActiveFocus()
                if (mouse.button === Qt.RightButton) {
                    const p = mapToItem(null, mouse.x, mouse.y)
                    panel.chipMenu(chip.modelData, p.x, p.y)
                } else if (mouse.modifiers & Qt.ShiftModifier) panel.soloChip(chip.modelData.id)
                else panel.toggleChip(chip.modelData.id)
            }
        }
        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                event.accepted = true
                if (event.isAutoRepeat) return
                if (event.modifiers & Qt.ShiftModifier) panel.soloChip(modelData.id)
                else panel.toggleChip(modelData.id)
            } else if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) {
                event.accepted = true
                panel.focusResults()
            }
        }
        Keys.onReleased: function(event) { if (event.key === Qt.Key_Space) event.accepted = true }
        Keys.onShortcutOverride: function(event) { if ((event.modifiers & ~Qt.KeypadModifier) === Qt.NoModifier && [Qt.Key_Space, Qt.Key_Return, Qt.Key_Enter].includes(event.key)) event.accepted = true }
        AppToolTip {
            theme: panel.desktop; uiScale: panel.uiScale
            text: (chip.modelData.overlap ? "On " + chip.modelData.reference + " (overlapping range)\n" : "")
                + "click: turn on/off · " + KeyNotation.format("Shift+Click") + ": show only this tag"
            visible: chipMouse.containsMouse || chip.activeFocus
        }
        Accessible.name: modelData.name + (on ? "" : ", off")
    }
    component TagInput: TextField {
        id: input
        font.family: panel.desktop.fontFamily
        font.pixelSize: 14 * panel.controlScale
        color: panel.desktop.foreground
        placeholderTextColor: panel.desktop.muted
        selectionColor: panel.desktop.accent
        selectedTextColor: panel.desktop.background
        padding: 10 * panel.controlScale
        background: Rectangle {
            color: panel.desktop.background
            border.color: input.activeFocus ? panel.desktop.accent : panel.desktop.border
            radius: Math.min(panel.desktop.cornerRadius, width / 2, height / 2)
        }
    }
    component TagScrollbar: ScrollBar {
        id: bar
        implicitWidth: 18 * panel.controlScale
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
                anchors.right: parent.right
                anchors.rightMargin: 4 * panel.controlScale
                height: parent.height
                width: (bar.hovered || bar.pressed ? 10 : 4) * panel.controlScale
                color: panel.desktop.muted
                radius: Math.min(panel.desktop.cornerRadius, width / 2, height / 2)
                opacity: bar.hovered || bar.pressed ? 0.8 : linger.running ? 0.5 : 0.2
                Behavior on width { NumberAnimation { duration: panel.app.reduceMotion ? 0 : 120 } }
                Behavior on opacity { NumberAnimation { duration: panel.app.reduceMotion ? 0 : 200 } }
            }
        }
        background: Item {}
    }
    component FineRule: Rectangle {
        Layout.fillWidth: true
        implicitHeight: 1
        color: panel.desktop.border
        opacity: 0.65
    }
    Drawer {
        id: relatedDrawer
        objectName: "relatedDrawer"
        parent: Overlay.overlay
        width: Math.min(520 * panel.uiScale, panel.app.width - 48)
        height: panel.app.height
        edge: Qt.RightEdge
        modal: true
        focus: true
        padding: 24 * panel.uiScale
        topPadding: padding
        bottomPadding: padding
        leftPadding: padding
        rightPadding: padding
        closePolicy: Popup.CloseOnPressOutside
        property bool closing: false
        onAboutToShow: closing = false
        onAboutToHide: closing = true
        // A side panel: slides in and out, unless Reduce motion is on.
        enter: Transition { NumberAnimation { property: "position"; duration: panel.app.reduceMotion ? 0 : 220; easing.type: Easing.OutCubic } }
        exit: Transition { NumberAnimation { property: "position"; duration: panel.app.reduceMotion ? 0 : 180; easing.type: Easing.InCubic } }
        background: Rectangle {
            color: panel.desktop.background
            border.color: panel.desktop.border
            radius: Math.min(panel.desktop.cornerRadius, width / 2, height / 2)
        }
        Overlay.modal: Rectangle { color: Qt.rgba(panel.desktop.background.r, panel.desktop.background.g, panel.desktop.background.b, 0.65) }
        onClosed: {
            panel.studySearchOpen = false
            panel.app.restorePanelFocus()
        }
        contentItem: ColumnLayout {
            id: studyBody
            objectName: "studyBody"
            spacing: 8 * panel.studyScale
            Keys.onEscapePressed: panel.handleStudyEscape()
            // Tabs: ←/→ (h/l), except while a chip has the keyboard: there they move between chips.
            Shortcut { objectName: "studyTabPrevious"; sequences: ["H", "Left"]; enabled: panel.relatedMoveKeys && !panel.chipFocused; onActivated: panel.stepStudyTab(-1) }
            Shortcut { objectName: "studyTabNext"; sequences: ["L", "Right"]; enabled: panel.relatedMoveKeys && !panel.chipFocused; onActivated: panel.stepStudyTab(1) }
            Shortcut { sequences: ["H", "Left"]; enabled: panel.relatedMoveKeys && panel.chipFocused; onActivated: panel.moveChip(-1) }
            Shortcut { sequences: ["L", "Right"]; enabled: panel.relatedMoveKeys && panel.chipFocused; onActivated: panel.moveChip(1) }
            Shortcut { sequences: ["J", "Down"]; enabled: panel.relatedMoveKeys; onActivated: { if (panel.chipFocused) panel.focusResults(); else panel.moveRelated(1) } }
            Shortcut { sequences: ["K", "Up"]; enabled: panel.relatedMoveKeys && !panel.chipFocused; onActivated: panel.moveRelated(-1) }
            // Shared list keys on the current row.
            Shortcut { sequence: "Y"; enabled: panel.listKeys; onActivated: panel.app.copyScope(panel.listScope()) }
            Shortcut { sequence: "Shift+Y"; enabled: panel.listKeys; onActivated: panel.app.copyScope(panel.listScope(), true) }
            Shortcut { sequence: "D"; enabled: panel.listKeys; onActivated: panel.app.toggleScopeDesk(panel.listScope()) }
            Shortcut { sequence: "Shift+E"; enabled: panel.listKeys && panel.tagsTab; onActivated: panel.app.openRangeEditor(panel.listScope(), "tag") }
            // Tags tab: t tags the row (or, with no tags yet, the passage), a
            // toggles all installed versions, 0 turns every chip back on.
            Shortcut { objectName: "studyTagKey"; sequence: "T"; enabled: panel.relatedMoveKeys && panel.tagsTab && !panel.bible.busy; onActivated: panel.tagCurrentResult() }
            Shortcut { objectName: "studyAllVersionsKey"; sequence: "A"; enabled: panel.relatedMoveKeys && panel.tagsTab; onActivated: panel.toggleAllVersions() }
            Shortcut { objectName: "studyChipsAllKey"; sequence: "0"; enabled: panel.relatedMoveKeys && panel.tagsTab && (panel.chipFocused || resultList.activeFocus); onActivated: panel.restoreChips() }
            Shortcut { sequence: "/"; enabled: panel.relatedMoveKeys && panel.tagsTab; onActivated: panel.showStudySearch() }
            Shortcut {
                sequences: ["Menu", "Shift+F10"]
                enabled: panel.relatedMoveKeys && panel.tagsTab && !panel.chipFocused && resultList.currentItem !== null
                onActivated: {
                    const point = resultList.currentItem.mapToItem(null, 24 * panel.uiScale, Math.min(resultList.currentItem.height, 28 * panel.uiScale))
                    panel.openResultMenu(resultList.currentIndex, point.x, point.y)
                }
            }
            Shortcut {
                sequences: ["Menu", "Shift+F10"]
                enabled: panel.relatedMoveKeys && panel.studyListTab && studyList.currentItem !== null
                onActivated: {
                    const point = studyList.currentItem.mapToItem(null, 24 * panel.uiScale, Math.min(studyList.currentItem.height, 28 * panel.uiScale))
                    panel.openStudyMenu(studyList.currentIndex, point.x, point.y)
                }
            }
            // g, shift G, ctrl d/u, ctrl f/b, pgdn/pgup on the current tab's list.
            ListMotionKeys {
                objectName: "relatedMotionKeys"
                active: panel.relatedMoveKeys && panel.tagsTab && !panel.chipFocused
                count: resultList.count
                onMove: function(delta) { if (resultList.currentIndex < 0) resultList.currentIndex = 0; panel.moveResult(delta) }
            }
            ListMotionKeys {
                objectName: "studyMotionKeys"
                active: panel.relatedMoveKeys && panel.studyListTab
                count: studyList.count
                onMove: function(delta) { if (studyList.currentIndex < 0) studyList.currentIndex = 0; panel.moveStudy(delta) }
            }
            PanelHeader {
                objectName: "studyHeader"
                Layout.fillWidth: true
                theme: panel.desktop
                title: "Study"
                subtitle: panel.passageSubtitle
                uiScale: panel.studyScale
                titleName: "studyTitle"
                subtitleName: "studyPassage"
            }
            PanelTabs {
                id: studyTabRow
                objectName: "studyTabs"
                Layout.fillWidth: true
                labels: panel.studyTitles
                theme: panel.desktop
                uiScale: panel.studyScale
                namePrefix: "studyTab"
                // ←/→ (h/l) drive the tabs; clicks still switch.
                focusable: false
                currentIndex: panel.studyTabs.indexOf(panel.studyTab)
                onActivated: index => panel.setStudyTab(panel.studyTabs[index])
            }
            FineRule { objectName: "studyTabsRule" }
            TagText {
                Layout.fillWidth: true
                visible: panel.studyTab !== "footnotes" && !panel.installed(panel.relatedSource)
                objectName: "studyScopePrompt"
                text: "This source version is no longer installed. Choose a current passage to continue."
                color: panel.desktop.muted
            }
            // Tags tab: this passage's tags as chips.
            Flow {
                id: chipFlow
                objectName: "studyChips"
                Layout.fillWidth: true
                Layout.topMargin: 4 * panel.studyScale
                visible: panel.tagsTab && panel.rootChips.length > 0
                spacing: 6 * panel.studyScale
                // At most two lines: chips that would wrap further fold into "+N".
                onWidthChanged: panel.chipLimit = 1000
                onPositioningComplete: {
                    if (panel.chipsExpanded || panel.chipLimit <= 1) return
                    const chipHeight = 28 * panel.studyScale
                    let bottom = 0
                    for (let i = 0; i < children.length; ++i) {
                        const child = children[i]
                        if (child.visible && child.width > 0) bottom = Math.max(bottom, child.y + child.height)
                    }
                    if (bottom > 2 * chipHeight + spacing + 1)
                        panel.chipLimit = Math.min(panel.chipLimit, panel.rootChips.length) - 1
                }
                Repeater {
                    id: chipRepeater
                    model: panel.rootChips
                    // Exact-range tags first; a muted · leads the overlapping ones.
                    delegate: Row {
                        id: chipCell
                        required property var modelData
                        required property int index
                        readonly property bool leading: modelData.overlap && index > 0 && !panel.rootChips[index - 1].overlap
                        visible: panel.chipsExpanded || index < panel.chipLimit
                        spacing: chipFlow.spacing
                        Text {
                            objectName: "studyChipSeparator"
                            visible: chipCell.leading
                            height: cellChip.height
                            verticalAlignment: Text.AlignVCenter
                            text: "·"
                            color: panel.desktop.muted
                            font.family: panel.desktop.fontFamily
                            font.pixelSize: 14 * panel.studyScale
                        }
                        StudyChip { id: cellChip; modelData: chipCell.modelData; index: chipCell.index }
                    }
                }
                Button {
                    id: moreChips
                    objectName: "studyChipsMore"
                    readonly property bool studyChip: true
                    visible: panel.hiddenChips > 0
                    focusPolicy: Qt.StrongFocus
                    hoverEnabled: true
                    implicitHeight: 28 * panel.studyScale
                    padding: 7 * panel.studyScale
                    topPadding: 4 * panel.studyScale
                    bottomPadding: 4 * panel.studyScale
                    font.family: panel.desktop.fontFamily
                    font.pixelSize: 12 * panel.studyScale
                    text: "+" + panel.hiddenChips
                    contentItem: Text { text: moreChips.text; font: moreChips.font; color: panel.desktop.muted; verticalAlignment: Text.AlignVCenter }
                    background: Rectangle {
                        color: moreChips.hovered || moreChips.down ? panel.desktop.surface : "transparent"
                        border.width: 1
                        border.color: moreChips.activeFocus ? panel.desktop.accent : panel.desktop.border
                        radius: Math.min(panel.desktop.cornerRadius, width / 2, height / 2)
                    }
                    onClicked: { panel.chipsExpanded = true; Qt.callLater(panel.focusChips) }
                    Keys.onShortcutOverride: function(event) { if ((event.modifiers & ~Qt.KeypadModifier) === Qt.NoModifier && [Qt.Key_Space, Qt.Key_Return, Qt.Key_Enter].includes(event.key)) event.accepted = true }
                    Keys.onPressed: function(event) {
                        if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) { event.accepted = true; panel.focusResults() }
                        else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) { event.accepted = true; moreChips.clicked() }
                    }
                    Accessible.name: panel.hiddenChips + " more tags"
                }
            }
            TagText {
                objectName: "studyTagCount"
                // Clickable: opens the passage search; right click for its menu.
                MouseArea {
                    id: studyCountMouse
                    objectName: "studyTagCountMouse"
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onClicked: function(mouse) {
                        if (mouse.button === Qt.RightButton) {
                            const p = mapToItem(null, mouse.x, mouse.y)
                            panel.countMenu(p.x, p.y)
                        } else panel.showStudySearch()
                    }
                }
                AppToolTip {
                    theme: panel.desktop; uiScale: panel.uiScale
                    text: "Search these passages · " + KeyNotation.format("/") + "\n" + KeyNotation.format("RightClick") + ": all versions, search"
                    visible: studyCountMouse.containsMouse
                }
                Layout.fillWidth: true
                visible: panel.tagsTab && !panel.studySearchOpen && panel.onIds.length > 0 && panel.results.length > 0
                font.pixelSize: 12 * panel.studyScale
                color: panel.desktop.muted
                elide: Text.ElideRight
                maximumLineCount: 1
                text: {
                    const total = panel.results.length
                    const noun = total === 1 ? " passage" : " passages"
                    const query = panel.studyQuery.trim()
                    return (query.length ? panel.visibleResults.length + " of " + total + noun + " · “" + query + "”" : total + noun)
                        + (panel.allVersions ? " · all versions" : "")
                }
            }
            TagInput {
                id: studySearch
                objectName: "studySearch"
                Layout.fillWidth: true
                visible: panel.tagsTab && panel.studySearchOpen
                font.pixelSize: 13 * panel.studyScale
                padding: 8 * panel.studyScale
                placeholderText: "Search passages…"
                text: panel.studyQuery
                onTextEdited: { panel.studyQuery = text; resultList.currentIndex = resultList.count ? 0 : -1 }
                onAccepted: panel.leaveStudySearch()
                Keys.onDownPressed: panel.leaveStudySearch()
                Keys.onEscapePressed: panel.leaveStudySearch()
                onActiveFocusChanged: if (!activeFocus && !panel.studyQuery.length) panel.studySearchOpen = false
                Accessible.name: "Search passages"
            }
            ListView {
                id: resultList
                objectName: "tagResults"
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: panel.tagsTab
                clip: true
                spacing: 4 * panel.studyScale
                model: panel.visibleResults
                currentIndex: count ? 0 : -1
                activeFocusOnTab: true
                keyNavigationEnabled: false
                Keys.onReturnPressed: { if (currentIndex >= 0) panel.openTagged(panel.visibleResults[currentIndex]); else panel.rejected() }
                Keys.onEnterPressed: { if (currentIndex >= 0) panel.openTagged(panel.visibleResults[currentIndex]); else panel.rejected() }
                Keys.onTabPressed: function(event) { event.accepted = panel.focusChips() }
                Keys.onBacktabPressed: function(event) { event.accepted = panel.focusChips() }
                ScrollBar.vertical: TagScrollbar { objectName: "tagResultsScrollbar" }
                delegate: ItemDelegate {
                    id: resultDelegate
                    required property var modelData
                    required property int index
                    readonly property bool current: resultList.currentIndex === index && !panel.chipFocused
                    readonly property bool otherVersion: modelData.versionId !== (panel.relatedSource ? panel.relatedSource.versionId : "")
                    objectName: "tagResult:" + index
                    width: resultList.width - 12 * panel.studyScale
                    focusPolicy: Qt.NoFocus
                    hoverEnabled: true
                    padding: 10 * panel.studyScale
                    onClicked: { resultList.currentIndex = index; resultList.forceActiveFocus(); panel.openTagged(modelData) }
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: function(eventPoint) { panel.openResultMenu(resultDelegate.index, eventPoint.scenePosition.x, eventPoint.scenePosition.y) }
                    }
                    contentItem: Column {
                        spacing: 3 * panel.studyScale
                        Text {
                            objectName: "tagResultReference:" + resultDelegate.index
                            width: parent.width
                            text: resultDelegate.modelData.reference + (panel.allVersions && resultDelegate.otherVersion ? " · " + resultDelegate.modelData.versionAbbreviation : "")
                            textFormat: Text.PlainText
                            color: panel.desktop.foreground
                            font.family: panel.desktop.fontFamily
                            font.pixelSize: 14 * panel.studyScale
                            elide: Text.ElideRight
                        }
                        // The row's tags: shared ones in the accent, the rest muted. Inert.
                        Text {
                            objectName: "tagResultTags:" + resultDelegate.index
                            width: parent.width
                            textFormat: Text.StyledText
                            text: Array.from(resultDelegate.modelData.tags || []).map(function(tag) {
                                return '<font color="' + (panel.onIds.indexOf(tag.id) >= 0 ? panel.desktop.accent : panel.desktop.muted) + '">'
                                    + KeyNotation.escapeHtml(tag.name) + "</font>"
                            }).join('<font color="' + panel.desktop.muted + '"> · </font>')
                            font.family: panel.desktop.fontFamily
                            font.pixelSize: 12 * panel.studyScale
                            wrapMode: Text.Wrap
                        }
                        Text {
                            width: parent.width
                            visible: text.length > 0
                            text: resultDelegate.modelData.excerpt || ""
                            textFormat: Text.PlainText
                            color: panel.desktop.muted
                            font.family: panel.desktop.fontFamily
                            font.pixelSize: 13 * panel.studyScale
                            lineHeight: 1.2
                            wrapMode: Text.Wrap
                            maximumLineCount: 2
                            elide: Text.ElideRight
                        }
                    }
                    background: Rectangle {
                        color: resultDelegate.current ? Qt.rgba(panel.desktop.accent.r, panel.desktop.accent.g, panel.desktop.accent.b, 0.11)
                            : resultDelegate.hovered ? panel.desktop.surface : "transparent"
                        border.width: 0
                        radius: Math.min(panel.desktop.cornerRadius, width / 2, height / 2)
                    }
                }
                ColumnLayout {
                    objectName: "studyTagsEmptyState"
                    anchors.centerIn: parent
                    width: parent.width - 24 * panel.studyScale
                    visible: resultList.count === 0 && panel.installed(panel.relatedSource)
                    spacing: 10 * panel.studyScale
                    TagText {
                        objectName: "studyTagsEmpty"
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        font.pixelSize: 13 * panel.studyScale
                        color: panel.desktop.muted
                        text: !panel.relatedSource ? ""
                            : !panel.rootChips.length ? "No tags on " + panel.relatedSource.reference + "."
                            : !panel.onIds.length ? "Turn on a tag to see passages."
                            : panel.results.length ? "No passages match “" + panel.studyQuery.trim() + "”."
                            : "No other passages are tagged " + panel.rootChips.filter(function(chip) { return panel.onIds.indexOf(chip.id) >= 0 })
                                .map(function(chip) { return chip.name }).join(", ") + "."
                    }
                    HintRow {
                        objectName: "studyTagsVersionsHint"
                        Layout.alignment: Qt.AlignHCenter
                        Layout.maximumWidth: parent.width
                        visible: panel.rootChips.length > 0 && panel.onIds.length > 0 && !panel.results.length && !panel.allVersions && panel.otherVersions
                        theme: panel.desktop
                        parts: [["A", "include other versions", function() { panel.toggleAllVersions() }, panel.relatedMoveKeys]]
                        pixelSize: 12 * panel.studyScale
                    }
                    Button {
                        id: addTags
                        objectName: "studyAddTags"
                        Layout.alignment: Qt.AlignHCenter
                        visible: !panel.rootChips.length
                        hoverEnabled: true
                        focusPolicy: Qt.StrongFocus
                        implicitHeight: 32 * panel.studyScale
                        padding: 10 * panel.studyScale
                        font.family: panel.desktop.fontFamily
                        font.pixelSize: 13 * panel.studyScale
                        text: "Add tags…"
                        contentItem: Text {
                            textFormat: Text.StyledText
                            text: KeyNotation.escapeHtml(addTags.text) + "&nbsp;&nbsp;" + '<font color="' + panel.desktop.muted + '">'
                                + KeyNotation.escapeHtml(KeyNotation.format("T")) + "</font>"
                            font: addTags.font
                            color: panel.desktop.foreground
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            radius: Math.min(panel.desktop.cornerRadius, width / 2, height / 2)
                            color: addTags.down ? Qt.rgba(panel.desktop.accent.r, panel.desktop.accent.g, panel.desktop.accent.b, 0.16)
                                : addTags.hovered ? panel.desktop.surface : "transparent"
                            border.width: addTags.activeFocus ? 1 : 0
                            border.color: panel.desktop.accent
                        }
                        onClicked: panel.app.openPassageTags(panel.snapshot(panel.relatedSource))
                        Keys.onReturnPressed: clicked()
                        Keys.onShortcutOverride: function(event) { if ((event.modifiers & ~Qt.KeypadModifier) === Qt.NoModifier && [Qt.Key_Space, Qt.Key_Return, Qt.Key_Enter].includes(event.key)) event.accepted = true }
                        Keys.onEnterPressed: clicked()
                    }
                }
            }
            TagText {
                objectName: "studyCount"
                visible: panel.studyListTab
                Layout.fillWidth: true
                font.pixelSize: 12 * panel.studyScale
                text: panel.studyTab === "crossrefs"
                    ? (panel.crossRows.length + (panel.crossRows.length === 1 ? " cross reference" : " cross references") + " in " + panel.bible.versionAbbreviation)
                    : (panel.footnoteRows.length + (panel.footnoteRows.length === 1 ? " footnote" : " footnotes") + " in " + panel.bible.passage + " · " + panel.bible.versionAbbreviation)
                color: panel.desktop.muted
            }
            ListView {
                id: studyList
                objectName: "studyList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: panel.studyListTab
                clip: true
                spacing: 8 * panel.uiScale
                model: panel.studyRows
                currentIndex: count ? 0 : -1
                activeFocusOnTab: true
                keyNavigationEnabled: true
                Keys.onReturnPressed: panel.openStudyRow(currentIndex)
                Keys.onEnterPressed: panel.openStudyRow(currentIndex)
                ScrollBar.vertical: TagScrollbar { objectName: "studyScrollbar" }
                delegate: TagButton {
                    id: studyDelegate
                    required property var modelData
                    required property int index
                    readonly property bool footnote: panel.studyTab === "footnotes"
                    // The cursor verse's footnotes stay marked while the list moves.
                    readonly property bool atCursor: footnote && modelData.verse === panel.bible.cursorVerse
                    objectName: "studyRow:" + index
                    // Clear of the scrollbar while it shows.
                    width: studyList.width - (studyList.ScrollBar.vertical.visible ? studyList.ScrollBar.vertical.width : 0)
                    chosen: studyList.activeFocus && studyList.currentIndex === index
                    onClicked: panel.openStudyRow(index)
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: function(eventPoint) { panel.openStudyMenu(studyDelegate.index, eventPoint.scenePosition.x, eventPoint.scenePosition.y) }
                    }
                    background: Rectangle {
                        color: studyDelegate.chosen ? Qt.rgba(panel.desktop.accent.r, panel.desktop.accent.g, panel.desktop.accent.b, 0.11)
                            : studyDelegate.atCursor ? Qt.rgba(panel.desktop.accent.r, panel.desktop.accent.g, panel.desktop.accent.b, 0.06)
                            : studyDelegate.hovered ? panel.desktop.surface : "transparent"
                        border.width: studyDelegate.atCursor ? 1 : 0
                        border.color: Qt.rgba(panel.desktop.accent.r, panel.desktop.accent.g, panel.desktop.accent.b, 0.4)
                        radius: Math.min(panel.desktop.cornerRadius, width / 2, height / 2)
                    }
                    contentItem: Column {
                        spacing: 4 * panel.uiScale
                        TagText {
                            width: parent.width
                            text: studyDelegate.footnote
                                ? panel.bible.passage + ":" + studyDelegate.modelData.verse + " · " + (studyDelegate.modelData.caller && studyDelegate.modelData.caller !== "+" && studyDelegate.modelData.caller !== "-" ? studyDelegate.modelData.caller : "note")
                                : studyDelegate.modelData.reference || ""
                            color: studyDelegate.atCursor ? panel.desktop.accent : panel.desktop.foreground
                        }
                        TagText {
                            width: parent.width
                            text: (studyDelegate.footnote ? studyDelegate.modelData.text : studyDelegate.modelData.excerpt) || ""
                            lineHeight: panel.bible.lineHeight > 0 ? panel.bible.lineHeight / 1.5 * 1.25 : 1.25
                            maximumLineCount: studyDelegate.footnote ? 8 : 3
                            elide: Text.ElideRight
                            color: studyDelegate.footnote ? panel.desktop.foreground : panel.desktop.muted
                        }
                    }
                }
                TagText {
                    objectName: "studyEmpty"
                    anchors.centerIn: parent
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    visible: studyList.count === 0
                    text: panel.studyTab === "footnotes"
                        ? (panel.bible.textSchema > 0 && panel.bible.textSchema < 4 ? "Re-import this version (Versions) to read its footnotes." : "No footnotes in this chapter.")
                        : !panel.relatedSource || !panel.relatedSource.start ? "Select a verse to see its cross references."
                        : panel.bible.textSchema > 0 && panel.bible.textSchema < 4 ? "Re-import this version (Versions) to read its cross-references."
                        : !panel.bible.hasCrossReferences ? "This version has no cross-references."
                        : "No cross-references for this passage in " + panel.bible.versionAbbreviation + "."
                    color: panel.desktop.muted
                }
            }
            TagText { objectName: "studyMessage"; Layout.fillWidth: true; visible: text.length > 0; text: panel.relatedShown ? panel.message : "" }
            FineRule {}
            HintRow {
                Layout.fillWidth: true
                objectName: "studyHints"
                theme: panel.desktop
                parts: panel.studyHintParts()
                pixelSize: 11 * panel.studyScale
            }
        }
    }
    Popup {
        id: surface
        objectName: "tagsSurface"
        parent: Overlay.overlay
        // Above the side drawers it may open over (Annotations, Study).
        z: 10
        width: Math.min(780 * panel.uiScale, parent ? parent.width - 24 : 780)
        height: Math.min(760 * panel.uiScale, parent ? parent.height - 24 : 760)
        x: parent ? (parent.width - width) / 2 : 0
        y: parent ? (parent.height - height) / 2 : 0
        padding: 16 * panel.uiScale
        modal: true
        focus: true
        // Esc or a click on the scrim closes it (a pending confirmation first
        // needs esc or its own buttons).
        closePolicy: panel.confirmation ? Popup.NoAutoClose : Popup.CloseOnPressOutside
        // A modal: fades in, closes at once.
        enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: panel.app.fadeMs || 0; easing.type: Easing.OutQuad } }
        exit: null
        onClosed: {
            panel.confirmation = null
            panel.editingScope = null
            panel.app.editingPanelClosed()
            panel.app.restorePanelFocus()
        }
        background: Rectangle {
            color: panel.desktop.background
            border.color: panel.desktop.border
            radius: Math.min(panel.desktop.cornerRadius, width / 2, height / 2)
        }
        Overlay.modal: Rectangle { color: Qt.rgba(panel.desktop.background.r, panel.desktop.background.g, panel.desktop.background.b, 0.72) }
        contentItem: ColumnLayout {
            id: body
            objectName: "tagsBody"
            spacing: 8 * panel.controlScale
            Keys.onEscapePressed: panel.handleEscape()
            // ctrl m (merge target), delete (delete the tag); ctrl t (pages) is
            // the window's Manage tags key (Main's runCommand toggles the page):
            // keys bubble here from the field and lists, which leave them alone.
            Keys.onPressed: function(event) {
                const mods = event.modifiers & ~Qt.KeypadModifier
                if (event.key === Qt.Key_M && mods === Qt.ControlModifier && panel.page === "manage" && !panel.confirmation) panel.focusMerge()
                else if (event.key === Qt.Key_Delete && mods === Qt.NoModifier && panel.page === "manage" && !panel.confirmation && !panel.textInputActive()) panel.requestDeleteTag()
                else return
                event.accepted = true
            }
            // The confirmation's ← → ↑ ↓ move between Cancel and Confirm.
            Shortcut {
                sequences: ["Left", "Right", "Up", "Down"]
                enabled: surface.opened && !!panel.confirmation
                onActivated: {
                    if (cancelManagement.activeFocus) confirmManagementButton.forceActiveFocus()
                    else cancelManagement.forceActiveFocus()
                }
            }
            RowLayout {
                Layout.fillWidth: true
                TagText {
                    objectName: "tagsTitle"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    text: panel.page === "manage" ? "Manage tags"
                        : panel.editingScope ? panel.editingScope.reference + " · " + panel.versionName(panel.editingScope) : "Tags"
                    font.pixelSize: 14 * panel.controlScale
                }
                TagButton { text: "Back"; visible: panel.page === "manage"; onClicked: panel.back() }
                TagButton { objectName: "manageTags"; text: "Manage"; visible: panel.page === "picker"; onClicked: panel.manage() }
            }
            TagText {
                Layout.fillWidth: true
                visible: panel.page === "picker" && !panel.scopeValid
                objectName: "tagScopePrompt"
                text: !panel.editingScope
                    ? "Select a passage to add or remove tags."
                    : "This source version is no longer installed. Choose a current passage to continue."
                color: panel.desktop.muted
            }
            ColumnLayout {
                Layout.fillWidth: true
                visible: panel.confirmation !== null
                TagText {
                    Layout.fillWidth: true
                    text: !panel.confirmation ? "" : panel.confirmation.action === "delete"
                        ? "Delete “" + panel.confirmation.name + "” and its assignments in every version?"
                        : panel.confirmation.action === "merge" ? "Merge “" + panel.confirmation.name + "” into “" + panel.confirmation.targetName + "” in every version? The source tag will be removed."
                        : "Rename “" + panel.confirmation.name + "” to “" + panel.confirmation.newName + "” everywhere?"
                }
                RowLayout {
                    TagButton { id: cancelManagement; objectName: "cancelTagManagement"; text: "Cancel"; onClicked: { panel.confirmation = null; search.forceActiveFocus() } }
                    TagButton { id: confirmManagementButton; objectName: "confirmTagManagement"; text: "Confirm"; enabled: !panel.bible.busy; onClicked: panel.confirmManagement() }
                }
            }
            TagInput {
                id: search
                objectName: "tagSearch"
                Layout.fillWidth: true
                visible: panel.confirmation === null
                placeholderText: panel.page === "picker" ? "Find or create a tag…" : "Filter tags…"
                text: panel.query
                onTextEdited: panel.query = text
                onAccepted: panel.pickerEnter()
                Keys.onDownPressed: { if (tagList.count) { tagList.currentIndex = 0; tagList.forceActiveFocus() } }
                // Backspace in an empty Manage filter goes back to the picker.
                Keys.onPressed: function(event) {
                    if (event.key === Qt.Key_Backspace && event.modifiers === Qt.NoModifier && !text.length && panel.page === "manage") {
                        event.accepted = true
                        panel.back()
                    }
                }
            }
            ListView {
                id: tagList
                objectName: "tagList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 34 * panel.controlScale
                visible: panel.confirmation === null
                clip: true
                spacing: 4 * panel.uiScale
                model: []
                currentIndex: -1
                property bool updatingModel: false
                onCurrentIndexChanged: {
                    if (!updatingModel && currentIndex >= 0) positionViewAtIndex(currentIndex, ListView.Contain)
                }
                keyNavigationEnabled: true
                activeFocusOnTab: true
                Keys.onSpacePressed: { if (currentIndex >= 0) panel.toggleTag(panel.filteredTags[currentIndex].id) }
                Keys.onReturnPressed: { if (currentIndex >= 0) panel.toggleTag(panel.filteredTags[currentIndex].id) }
                Keys.onEnterPressed: { if (currentIndex >= 0) panel.toggleTag(panel.filteredTags[currentIndex].id) }
                Keys.onUpPressed: { if (currentIndex <= 0) search.forceActiveFocus(); else decrementCurrentIndex() }
                ScrollBar.vertical: TagScrollbar { objectName: "tagListScrollbar" }
                delegate: TagButton {
                    id: tagDelegate
                    required property var modelData
                    required property int index
                    objectName: "tag:" + modelData.id
                    width: tagList.width
                    chosen: panel.page === "manage" ? panel.managedId === modelData.id : panel.exactIds.indexOf(modelData.id) >= 0
                    text: (chosen ? "✓  " : "    ") + modelData.name + " · " + modelData.count
                    hint: modelData.name
                    enabled: panel.page !== "picker" || (panel.scopeValid && !panel.bible.busy)
                    onClicked: { tagList.currentIndex = index; panel.toggleTag(modelData.id) }
                    background: Rectangle {
                        // Like the app's other lists: the current row is a tinted fill, never an accent border.
                        color: tagDelegate.activeFocus || (tagList.activeFocus && tagList.currentIndex === tagDelegate.index)
                            ? Qt.rgba(panel.desktop.accent.r, panel.desktop.accent.g, panel.desktop.accent.b, 0.11)
                            : tagDelegate.chosen || tagDelegate.hovered ? panel.desktop.surface : "transparent"
                        border.width: 0
                        radius: Math.min(panel.desktop.cornerRadius, width / 2, height / 2)
                    }
                }
                TagText { anchors.centerIn: parent; visible: tagList.count === 0; text: panel.allTags.length ? "No matching tags" : "No tags yet"; color: panel.desktop.muted }
            }
            TagButton {
                objectName: "createPassageTag"
                Layout.fillWidth: true
                visible: panel.page === "picker" && panel.query.length > 0
                enabled: panel.validName && panel.scopeValid && !panel.bible.busy
                text: "Create / assign “" + panel.query.trim() + "”"
                onClicked: panel.createFromQuery()
            }
            TagText {
                visible: panel.page === "picker" && panel.overlaps.length > 0
                text: "Other overlapping ranges · assignments are separate"
                color: panel.desktop.muted
                Layout.fillWidth: true
            }
            ListView {
                id: overlapList
                objectName: "tagOverlaps"
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(100 * panel.controlScale, body.height * 0.18, contentHeight)
                visible: panel.page === "picker" && panel.overlaps.length > 0
                model: panel.overlaps
                clip: true
                ScrollBar.vertical: TagScrollbar { objectName: "tagOverlapsScrollbar" }
                delegate: RowLayout {
                    id: overlapDelegate
                    required property var modelData
                    width: overlapList.width
                    TagText { Layout.fillWidth: true; text: overlapDelegate.modelData.reference + " · " + panel.tagNames(overlapDelegate.modelData.tags) }
                    TagButton { objectName: "openTagOverlap:" + overlapDelegate.modelData.reference; text: "Open"; onClicked: panel.openTagged(overlapDelegate.modelData) }
                    TagButton { objectName: "editTagOverlap:" + overlapDelegate.modelData.reference; text: "Edit tags"; onClicked: panel.openTagged(overlapDelegate.modelData, true) }
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                visible: panel.page === "manage" && panel.managedId.length > 0 && panel.confirmation === null
                TagText { Layout.fillWidth: true; text: panel.tagById(panel.managedId) ? panel.tagById(panel.managedId).name : "" }
                RowLayout {
                    Layout.fillWidth: true
                    TagInput { id: renameInput; objectName: "tagRenameInput"; Layout.fillWidth: true; placeholderText: "New name"; onAccepted: panel.requestManagement("rename") }
                    TagButton { objectName: "renameTag"; text: "Rename…"; onClicked: panel.requestManagement("rename") }
                    TagButton { objectName: "deleteTag"; text: "Delete…"; onClicked: panel.requestManagement("delete") }
                }
                TagText { text: "Merge into"; color: panel.desktop.muted }
                ListView {
                    id: mergeList
                    objectName: "tagMergeTargets"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 100 * panel.uiScale
                    clip: true
                    model: panel.allTags.filter(function(tag) { return tag.id !== panel.managedId })
                    ScrollBar.vertical: TagScrollbar { objectName: "tagMergeScrollbar" }
                    currentIndex: -1
                    activeFocusOnTab: true
                    Keys.onPressed: function(event) {
                        if (event.modifiers !== Qt.NoModifier) return
                        if (event.key === Qt.Key_J || event.key === Qt.Key_Down) panel.moveMerge(1)
                        else if (event.key === Qt.Key_K || event.key === Qt.Key_Up) panel.moveMerge(-1)
                        else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) panel.chooseMerge()
                        else return
                        event.accepted = true
                    }
                    delegate: TagButton {
                        id: mergeDelegate
                        required property var modelData
                        required property int index
                        objectName: "mergeTarget:" + modelData.id
                        width: mergeList.width
                        text: modelData.name
                        focusPolicy: Qt.NoFocus
                        chosen: panel.mergeTarget === modelData.id
                        onClicked: {
                            mergeList.currentIndex = index
                            panel.mergeTarget = modelData.id
                            if (keyActivated) panel.chooseMerge()
                        }
                        background: Rectangle {
                            color: mergeList.activeFocus && mergeList.currentIndex === mergeDelegate.index
                                ? Qt.rgba(panel.desktop.accent.r, panel.desktop.accent.g, panel.desktop.accent.b, 0.11)
                                : mergeDelegate.chosen || mergeDelegate.hovered ? panel.desktop.surface : "transparent"
                            border.width: mergeDelegate.chosen ? 1 : 0
                            border.color: panel.desktop.accent
                            radius: Math.min(panel.desktop.cornerRadius, width / 2, height / 2)
                        }
                    }
                }
                TagButton { objectName: "mergeTags"; text: "Merge…"; enabled: panel.mergeTarget.length > 0; onClicked: panel.requestManagement("merge") }
            }
            Item { Layout.fillHeight: true; visible: panel.confirmation !== null }
            TagText { objectName: "tagMessage"; Layout.fillWidth: true; visible: text.length > 0; text: panel.message }
            FineRule {}
            HintRow {
                Layout.fillWidth: true
                objectName: "tagsHints"
                theme: panel.desktop
                parts: panel.pickerHintParts()
                pixelSize: 11 * panel.controlScale
            }
        }
    }
}
