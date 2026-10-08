pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "KeyNotation.js" as KeyNotation
import "RowMenu.js" as RowMenu

// Two surfaces share the tag state: the modal tag picker (with Manage tags)
// and the See also drawer (cross references, parallels, tagged alike). The picker may
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
    // See also (the Study drawer; internal id "related"): one list, in
    // sections, of what goes with the reader's passage: the version's own
    // cross references, its parallel passages, the passages tagged alike in
    // every version, and last OpenBible.info's community cross references.
    // The filter (/) narrows every section, and #name keeps a tag's passages.
    property var crossRows: []
    property var tagRows: []
    // The passage's tags: its exact range's, then those of ranges over it.
    property var rootTags: []
    property string studyQuery: ""
    property bool studySearchOpen: false
    readonly property var studyRows: seeAlsoRows()
    function seeAlsoRows() {
        const own = [], parallel = [], openBible = []
        crossRows.forEach(function(row) { (row.source === "parallel" ? parallel : row.openBible ? openBible : own).push(row) })
        const rows = []
        for (const section of [["own", own], ["parallel", parallel], ["tags", tagRows], ["openBible", openBible]]) {
            const matching = section[1].filter(function(row) { return panel.rowMatches(row) })
            matching.forEach(function(row, index) {
                rows.push(Object.assign({}, row, { section: section[0], firstOfSection: index === 0, sectionCount: matching.length }))
            })
        }
        return rows
    }
    function sectionTitle(section) {
        return section === "own" ? "Cross-references in " + bible.versionAbbreviation
            : section === "parallel" ? "Parallel passages" : section === "openBible" ? "OpenBible.info" : "Tagged alike"
    }
    // Every word must match: #gr a tag starting "gr", others the reference,
    // version, tags, or verse text, ignoring case.
    function rowMatches(row) {
        const words = studyQuery.trim().toLowerCase().split(/\s+/).filter(function(word) { return word.length > 0 })
        const tags = Array.from(row.tags || []).map(function(tag) { return tag.name.toLowerCase() })
        const text = [row.reference, row.versionAbbreviation || "", tags.join("\n"), row.excerpt || ""].join("\n").toLowerCase()
        return words.every(function(word) {
            if (word.length > 1 && word[0] === "#") return tags.some(function(name) { return name.startsWith(word.slice(1)) })
            return text.indexOf(word) >= 0
        })
    }
    function resetRootTags() {
        const tags = []
        if (installed(relatedSource)) {
            Array.from(bible.tagsForScope(relatedSource, false)).forEach(function(tag) { tags.push({ id: tag.id, name: tag.name }) })
            Array.from(bible.tagsForScope(relatedSource, true)).forEach(function(range) {
                if (sameRange(range, relatedSource)) return
                Array.from(range.tags).forEach(function(tag) {
                    if (!tags.some(function(other) { return other.id === tag.id })) tags.push({ id: tag.id, name: tag.name })
                })
            })
        }
        rootTags = tags
    }
    function refreshStudy() {
        const ready = installed(relatedSource)
        crossRows = ready ? Array.from(bible.crossReferences(relatedSource)) : []
        // The passages sharing any of its tags, in every version, most shared first.
        const ids = rootTags.map(function(tag) { return tag.id })
        tagRows = ready && ids.length ? Array.from(bible.relatedPassages(relatedSource, ids, false, true)) : []
        if (studyList.currentIndex >= studyList.count) studyList.currentIndex = studyList.count - 1
        if (studyList.currentIndex < 0 && studyList.count) studyList.currentIndex = 0
    }
    // A tag clicked (a section heading's, or one elsewhere): only its passages.
    function filterTag(name) {
        studyQuery = "#" + String(name).split(/\s+/)[0]
        studySearchOpen = true
        studyList.currentIndex = studyList.count ? 0 : -1
        focusResults()
    }
    function rejected(reason) { if (typeof app.rejected === "function") app.rejected(reason) }
    function openStudyRow(index) {
        const row = studyRows[index]
        if (!row) { rejected(); return }
        studyList.currentIndex = index
        openTagged(row)
    }
    // t: the current row's tags, else (an empty list) the passage's own.
    function tagCurrentRow() {
        const scope = studyRows[studyList.currentIndex] || relatedSource
        if (!installed(scope) || bible.busy) { rejected(); return }
        app.openPassageTags(snapshot(scope))
    }
    // The shared row menu on a row's passage, with Edit tags (and Move here
    // for a tagged passage).
    function openStudyMenu(index, sceneX, sceneY) {
        const row = studyRows[index]
        if (!row || typeof app.showContextMenu !== "function") return
        studyList.currentIndex = index
        studyList.forceActiveFocus()
        const ready = !bible.busy
        const extras = [{ id: "tags", text: "Edit tags…", shortcut: KeyNotation.format("T"), enabled: ready, run: function() { panel.app.openPassageTags(panel.snapshot(row)) } }]
        if (row.section === "tags") extras.push(panel.app.moveHereEntry([row], sceneX, sceneY))
        app.showContextMenu("study", RowMenu.entries(app, { busy: bible.busy, scope: row, open: function() { panel.openStudyRow(index) }, extras: extras }),
            sceneX, sceneY)
    }
    function showStudySearch() {
        if (!relatedShown) return
        studySearchOpen = true
        Qt.callLater(function() { studySearch.forceActiveFocus() })
    }
    // Leaving the filter keeps it while it has words.
    function leaveStudySearch() {
        studySearchOpen = studyQuery.length > 0
        focusResults()
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
    function canAct() { return !bible.busy && app.flushNote() }
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
    function openRelated() { return showRelated(bible.tagScope()) }
    // See also filtered to one tag's passages (a tag clicked elsewhere).
    function openRelatedForTag(name, source) {
        if (!showRelated(source || bible.tagScope())) return false
        if (name) filterTag(name)
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
    // (Re)roots See also on a passage, without a filter.
    function rootAt(captured) {
        relatedSource = captured
        message = ""
        studyQuery = ""
        studySearchOpen = false
        allTags = Array.from(bible.tags)
        resetRootTags()
        refreshStudy()
        studyList.currentIndex = studyList.count ? 0 : -1
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
            resetRootTags()
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
    function studyHintParts() {
        const escape = function() { panel.handleStudyEscape() }
        if (studySearch.activeFocus) return [[["Return", "Down"], "passages", function() { panel.leaveStudySearch() }], ["Escape", "leave filter", function() { panel.leaveStudySearch() }]]
        return [["Return", "open", function() { panel.openStudyRow(studyList.currentIndex) }, listKeys],
            ["T", "tag", function() { panel.tagCurrentRow() }, relatedMoveKeys && !bible.busy],
            ["Y", "copy", function() { panel.app.copyScope(panel.listScope()) }, listKeys],
            ["D", "Desk", function() { panel.app.addToDesk(panel.listScope()) }, listKeys],
            ["/", "filter", function() { panel.showStudySearch() }, relatedMoveKeys], ["Escape", "close", escape]]
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
        // Confirmation: enter presses the focused button (Cancel first), as other confirmations do.
        if (confirmation) return [[["Left", "Right"], "choose"], ["Return", "select", function() { (confirmManagementButton.activeFocus ? confirmManagementButton : cancelManagement).clicked() },
                cancelManagement.activeFocus || (confirmManagementButton.activeFocus && !bible.busy)],
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
    // See also keys: off while typing, behind the picker or a menu, and while closing.
    readonly property bool relatedMoveKeys: relatedShown && !pickerVisible && !app.contextMenuOpen
        && !textInputActive()
    readonly property bool relatedKeys: relatedMoveKeys
    readonly property bool listKeys: !bible.busy && relatedKeys && studyList.currentIndex >= 0 && studyList.count > 0
    function listScope() { return studyRows[studyList.currentIndex] || null }
    function moveStudy(delta) {
        if (!studyList.count) return
        studyList.currentIndex = Math.max(0, Math.min(studyList.count - 1, studyList.currentIndex + delta))
        studyList.positionViewAtIndex(studyList.currentIndex, ListView.Contain)
        studyList.forceActiveFocus()
    }
    function focusIn(container) {
        for (let item = app.activeFocusItem; item; item = item.parent) if (item === container) return true
        return false
    }
    function focusInBody() { return focusIn(body) || focusIn(studyBody) }
    function focusInStudy() { return focusIn(studyBody) }
    function focusResults() {
        if (studyList.count && studyList.currentIndex < 0) studyList.currentIndex = 0
        studyList.forceActiveFocus()
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
    // See also: esc in the filter leaves it, keeping the words; otherwise esc
    // closes the panel, as in Marks (reopening roots it afresh, unfiltered).
    function handleStudyEscape() {
        if (studySearch.activeFocus) leaveStudySearch()
        else relatedDrawer.close()
    }
    Connections {
        target: panel.bible
        function onTagsChanged() { if (panel.contentVisible) panel.refresh() }
        function onVersionsChanged() { if (panel.contentVisible) panel.refresh() }
        function onVersionChanged() { if (panel.relatedShown) panel.refreshStudy() }
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
            // Quiet like the app's other buttons: a border only marks focus or a
            // chosen toggle; focus draws it heavier, so the two never read alike.
            objectName: control.objectName.length ? control.objectName + ":focus" : ""
            border.width: control.activeFocus ? 2 : control.chosen ? 1 : 0
            border.color: panel.desktop.accent
            radius: Math.min(panel.desktop.cornerRadius, width / 2, height / 2)
        }
        // A focused button owns enter and space (never a window shortcut).
        Keys.onShortcutOverride: function(event) { if ((event.modifiers & ~Qt.KeypadModifier) === Qt.NoModifier && [Qt.Key_Space, Qt.Key_Return, Qt.Key_Enter].includes(event.key)) event.accepted = true }
        // keyActivated: this click came from enter (a merge target merges at once).
        property bool keyActivated: false
        Keys.onReturnPressed: { keyActivated = true; clicked(); keyActivated = false }
        Keys.onEnterPressed: { keyActivated = true; clicked(); keyActivated = false }
        // Focus shows the tip only when the keyboard put it there (a click does not).
        AppToolTip { theme: panel.desktop; uiScale: panel.uiScale; text: control.hint; visible: text.length > 0 && (control.hovered || (control.activeFocus && control.focusReason !== Qt.MouseFocusReason)) }
    }
    component TagInput: TextField {
        id: input
        // The app's text menu, not Qt's built-in one.
        ContextMenu.menu: null
        ContextMenu.onRequested: function(position) { if (typeof panel.app.openTextMenu === "function") panel.app.openTextMenu(input, position) }
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
    // Rows stay clear of a list's scrollbar while it shows (as Main's scrollGutter).
    function gutter(list) {
        const bar = list ? list.ScrollBar.vertical : null
        return bar && bar.visible ? bar.width : 0
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
                Behavior on width { NumberAnimation { duration: panel.app.reduceMotion ? 0 : 85 } }
                Behavior on opacity { NumberAnimation { duration: panel.app.reduceMotion ? 0 : 140 } }
            }
        }
        background: Item {}
    }
    component FineRule: Rule { theme: panel.desktop; Layout.fillWidth: true }
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
        // A side panel, like the app's: enter slides fully and fades in;
        // exit is subtler (a short slide, a quicker fade). Instant without animations.
        enter: Transition {
            NumberAnimation { property: "position"; to: 1; duration: panel.app.panelEnterMs || 0; easing.type: Easing.OutCubic }
            NumberAnimation { property: "opacity"; to: 1; duration: panel.app.panelEnterMs || 0; easing.type: Easing.OutQuad }
        }
        exit: Transition {
            NumberAnimation { property: "position"; to: panel.app.panelExitPosition || 0; duration: panel.app.panelExitMs || 0; easing.type: Easing.InCubic }
            NumberAnimation { property: "opacity"; to: 0; duration: panel.app.panelExitMs || 0; easing.type: Easing.InQuad }
        }
        background: Rectangle {
            color: panel.desktop.background
            border.color: panel.desktop.border
            radius: Math.min(panel.desktop.cornerRadius, width / 2, height / 2)
        }
        Overlay.modal: Rectangle { color: Qt.rgba(panel.desktop.background.r, panel.desktop.background.g, panel.desktop.background.b, 0.65) }
        onClosed: {
            position = 0
            panel.studySearchOpen = false
            panel.app.restorePanelFocus()
        }
        contentItem: ColumnLayout {
            id: studyBody
            objectName: "studyBody"
            spacing: 8 * panel.studyScale
            Keys.onEscapePressed: panel.handleStudyEscape()
            Shortcut { sequences: ["J", "Down"]; enabled: panel.relatedMoveKeys; onActivated: panel.moveStudy(1) }
            Shortcut { sequences: ["K", "Up"]; enabled: panel.relatedMoveKeys; onActivated: panel.moveStudy(-1) }
            // Shared list keys on the current row.
            Shortcut { sequence: "Y"; enabled: panel.listKeys; onActivated: panel.app.copyScope(panel.listScope()) }
            Shortcut { sequence: "Shift+Y"; enabled: panel.listKeys; onActivated: panel.app.copyScope(panel.listScope(), true) }
            Shortcut { sequence: "D"; enabled: panel.listKeys; onActivated: panel.app.addToDesk(panel.listScope()) }
            // t tags the row (or, with none, the passage); / filters.
            Shortcut { objectName: "studyTagKey"; sequence: "T"; enabled: panel.relatedMoveKeys && !panel.bible.busy; onActivated: panel.tagCurrentRow() }
            Shortcut { sequence: "/"; enabled: panel.relatedMoveKeys; onActivated: panel.showStudySearch() }
            Shortcut {
                sequences: ["Menu", "Shift+F10"]
                enabled: panel.relatedMoveKeys && studyList.currentItem !== null
                onActivated: {
                    const point = studyList.currentItem.mapToItem(null, 24 * panel.uiScale, Math.min(studyList.currentItem.height, 28 * panel.uiScale))
                    panel.openStudyMenu(studyList.currentIndex, point.x, point.y)
                }
            }
            // g, shift G, ctrl d/u, ctrl f/b, pgdn/pgup.
            ListMotionKeys {
                objectName: "studyMotionKeys"
                active: panel.relatedMoveKeys
                count: studyList.count
                onMove: function(delta) { if (studyList.currentIndex < 0) studyList.currentIndex = 0; panel.moveStudy(delta) }
            }
            PanelHeader {
                objectName: "studyHeader"
                Layout.fillWidth: true
                theme: panel.desktop
                title: "See also"
                subtitle: panel.passageSubtitle
                uiScale: panel.studyScale
                titleName: "studyTitle"
                subtitleName: "studyPassage"
            }
            FineRule { objectName: "studyHeaderRule" }
            TagText {
                Layout.fillWidth: true
                visible: !panel.installed(panel.relatedSource)
                objectName: "studyScopePrompt"
                text: "This source version is no longer installed. Choose a current passage to continue."
                color: panel.desktop.muted
            }
            TagInput {
                id: studySearch
                objectName: "studySearch"
                Layout.fillWidth: true
                visible: panel.studySearchOpen
                font.pixelSize: 13 * panel.studyScale
                padding: 8 * panel.studyScale
                placeholderText: "Filter… #name for a tag"
                text: panel.studyQuery
                onTextEdited: { panel.studyQuery = text; studyList.currentIndex = studyList.count ? 0 : -1 }
                onAccepted: panel.leaveStudySearch()
                Keys.onDownPressed: panel.leaveStudySearch()
                Keys.onEscapePressed: panel.leaveStudySearch()
                onActiveFocusChanged: if (!activeFocus && !panel.studyQuery.length) panel.studySearchOpen = false
                Accessible.name: "Filter passages"
            }
            ListView {
                id: studyList
                objectName: "studyList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 4 * panel.studyScale
                model: panel.studyRows
                currentIndex: count ? 0 : -1
                activeFocusOnTab: true
                keyNavigationEnabled: false
                Keys.onReturnPressed: panel.openStudyRow(currentIndex)
                Keys.onEnterPressed: panel.openStudyRow(currentIndex)
                ScrollBar.vertical: TagScrollbar { objectName: "studyScrollbar" }
                // Each section's heading sits above its first row, in the row's
                // block: j/k and the motions never stop on it.
                delegate: Column {
                    id: studyBlock
                    required property var modelData
                    required property int index
                    readonly property bool otherVersion: modelData.versionId !== (panel.relatedSource ? panel.relatedSource.versionId : "")
                    width: studyList.width - panel.gutter(studyList)
                    Flow {
                        objectName: "studySection:" + studyBlock.modelData.section
                        visible: !!studyBlock.modelData.firstOfSection
                        width: parent.width
                        spacing: 6 * panel.studyScale
                        topPadding: studyBlock.index ? 12 * panel.studyScale : 0
                        bottomPadding: 4 * panel.studyScale
                        leftPadding: 10 * panel.studyScale
                        Text {
                            objectName: "studySectionTitle:" + studyBlock.modelData.section
                            text: panel.sectionTitle(studyBlock.modelData.section)
                            textFormat: Text.PlainText
                            color: panel.desktop.muted
                            font.family: panel.desktop.fontFamily
                            font.pixelSize: 12 * panel.studyScale
                            font.weight: Font.Medium
                        }
                        // The passage's tags: click one to keep only its passages.
                        Repeater {
                            model: studyBlock.modelData.section === "tags" ? panel.rootTags : []
                            delegate: Text {
                                required property var modelData
                                objectName: "studySectionTag:" + modelData.name
                                text: "#" + modelData.name
                                textFormat: Text.PlainText
                                color: tagMouse.containsMouse ? panel.desktop.foreground : panel.desktop.accent
                                font.family: panel.desktop.fontFamily
                                font.pixelSize: 12 * panel.studyScale
                                MouseArea {
                                    id: tagMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: panel.filterTag(parent.modelData.name)
                                }
                                AppToolTip { theme: panel.desktop; uiScale: panel.uiScale; text: "Only the passages tagged " + parent.modelData.name; visible: tagMouse.containsMouse }
                            }
                        }
                        Text {
                            text: "· " + studyBlock.modelData.sectionCount
                            color: panel.desktop.muted
                            font.family: panel.desktop.fontFamily
                            font.pixelSize: 12 * panel.studyScale
                        }
                    }
                    TagButton {
                        id: studyDelegate
                        objectName: "studyRow:" + studyBlock.index
                        // The list holds the keyboard (j k, enter); rows never take it.
                        focusPolicy: Qt.NoFocus
                        width: parent.width
                        chosen: studyList.activeFocus && studyList.currentIndex === studyBlock.index
                        onClicked: panel.openStudyRow(studyBlock.index)
                        TapHandler {
                            acceptedButtons: Qt.RightButton
                            onTapped: function(eventPoint) { panel.openStudyMenu(studyBlock.index, eventPoint.scenePosition.x, eventPoint.scenePosition.y) }
                        }
                        background: Rectangle {
                            color: studyDelegate.chosen ? Qt.rgba(panel.desktop.accent.r, panel.desktop.accent.g, panel.desktop.accent.b, 0.11)
                                : studyDelegate.hovered ? panel.desktop.surface : "transparent"
                            border.width: 0
                            radius: Math.min(panel.desktop.cornerRadius, width / 2, height / 2)
                        }
                        contentItem: Column {
                            spacing: 4 * panel.uiScale
                            TagText {
                                objectName: "studyRowTitle:" + studyBlock.index
                                width: parent.width
                                text: (studyBlock.modelData.reference || "") + (studyBlock.otherVersion ? " · from " + studyBlock.modelData.versionAbbreviation : "")
                            }
                            // A tagged passage's tags: the ones it shares in the accent.
                            Text {
                                objectName: "studyRowTags:" + studyBlock.index
                                width: parent.width
                                visible: Array.from(studyBlock.modelData.tags || []).length > 0
                                textFormat: Text.StyledText
                                text: Array.from(studyBlock.modelData.tags || []).map(function(tag) {
                                    const shared = panel.rootTags.some(function(own) { return own.id === tag.id })
                                    return '<font color="' + (shared ? panel.desktop.accent : panel.desktop.muted) + '">#' + KeyNotation.escapeHtml(tag.name) + "</font>"
                                }).join("&nbsp; ")
                                font.family: panel.desktop.fontFamily
                                font.pixelSize: 12 * panel.studyScale
                                wrapMode: Text.Wrap
                            }
                            TagText {
                                width: parent.width
                                text: studyBlock.modelData.excerpt || ""
                                visible: text.length > 0
                                lineHeight: panel.bible.lineHeight > 0 ? panel.bible.lineHeight / 1.5 * 1.25 : 1.25
                                maximumLineCount: 3
                                elide: Text.ElideRight
                                color: panel.desktop.muted
                            }
                        }
                    }
                }
                TagText {
                    objectName: "studyEmpty"
                    anchors.centerIn: parent
                    width: parent.width - 24 * panel.studyScale
                    horizontalAlignment: Text.AlignHCenter
                    visible: studyList.count === 0 && panel.installed(panel.relatedSource)
                    color: panel.desktop.muted
                    text: !panel.relatedSource || !panel.relatedSource.start ? "Select a verse to see what goes with it."
                        : panel.studyQuery.trim().length && panel.crossRows.length + panel.tagRows.length ? "Nothing matches “" + panel.studyQuery.trim() + "”."
                        : "Nothing goes with " + panel.relatedSource.reference + " yet."
                            + (panel.bible.textSchema > 0 && panel.bible.textSchema < 4 ? " Re-import " + panel.bible.versionAbbreviation + " (Versions) to read its own cross-references." : "")
                            + (panel.rootTags.length ? "" : " Tag it (" + KeyNotation.format("T") + ") to gather the passages you tag alike.")
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
        // Esc or a click on the scrim closes it; during a confirmation a click
        // on the scrim cancels the confirmation instead (as esc does).
        closePolicy: panel.confirmation ? Popup.NoAutoClose : Popup.CloseOnPressOutside
        // Presses anywhere over the open picker (Overlay.onPressed fires for
        // both): one inside marks itself first through a passive PointHandler.
        property bool pressInside: false
        Overlay.onPressed: Qt.callLater(function() {
            const outside = !surface.pressInside
            surface.pressInside = false
            if (outside && surface.opened && panel.confirmation && !panel.app.contextMenuOpen) panel.handleEscape()
        })
        // A modal: opens and closes at once; the app's modal ghost plays the exit.
        enter: null
        exit: null
        onAboutToHide: if (panel.app.modalExitRelease) panel.app.modalExitRelease(surface)
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
            // Over the whole picker, above its content (reparented to the
            // popup's item): sees every press inside without taking it.
            Item {
                parent: body.parent
                width: surface.width
                height: surface.height
                z: 100
                PointHandler { onActiveChanged: if (active) surface.pressInside = true }
            }
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
                    // The list holds the keyboard; rows never become tab stops.
                    focusPolicy: Qt.NoFocus
                    width: tagList.width - panel.gutter(tagList)
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
                    width: overlapList.width - panel.gutter(overlapList)
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
                        width: mergeList.width - panel.gutter(mergeList)
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
