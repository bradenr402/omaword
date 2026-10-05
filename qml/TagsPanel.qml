pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "KeyNotation.js" as KeyNotation

Item {
    id: panel
    required property var app
    required property var bible
    required property var desktop

    property Item embeddedHost: null
    signal browserRequested()
    signal browserClosed()
    property string hostKind: "none"
    readonly property bool browserVisible: hostKind === "browser" && !!embeddedHost && embeddedHost.visible
    readonly property bool visibleModal: surface.visible || relatedDrawer.visible
    readonly property bool pickerVisible: surface.visible
    readonly property bool relatedShown: relatedDrawer.visible && !relatedDrawer.closing && hostKind === "related"
    readonly property bool contentVisible: visibleModal || browserVisible
    readonly property bool scopePinned: hostKind === "picker" && surface.visible && editingScope !== null
    property var editingScope: null
    property var summaryScope: null
    property var relatedSource: null
    // Study panel (the Related drawer, now tabbed): Related tag passages, the
    // pinned source's cross references, and the current chapter's footnotes.
    // ←/→ (h/l) switch tabs; every tab shares the list keys and motions.
    readonly property var studyTabs: ["related", "crossrefs", "footnotes"]
    readonly property var studyTitles: ["Related", "Cross-references", "Footnotes"]
    property string studyTab: "related"
    property var crossRows: []
    readonly property var footnoteRows: Array.from(bible.footnotes || [])
    readonly property var studyRows: studyTab === "crossrefs" ? crossRows : studyTab === "footnotes" ? footnoteRows : []
    readonly property bool studyListTab: page === "related" && studyTab !== "related"
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
    function openStudyRow(index) {
        const row = studyRows[index]
        if (!row) return
        studyList.currentIndex = index
        openTagged(studyScope(row))
    }
    property string page: "picker"
    property string returnPage: "browser"
    property string query: ""
    property var allTags: []
    property var exactIds: []
    property var exactTags: []
    property var overlaps: []
    property var summaryIds: []
    property var filterIds: []
    property var sourceIds: []
    property var results: []
    property bool matchAll: false
    property bool allVersions: false
    property bool scopeValid: false
    property string message: ""
    property string managedId: ""
    property string mergeTarget: ""
    property var confirmation: null
    // Annotations → Tags search (Main owns the field): filters the summary's
    // overlapping ranges and related passages.
    property string summaryQuery: ""
    readonly property int summaryTotal: overlaps.length + results.length
    function summaryMatches(entry) {
        const query = summaryQuery.trim().toLowerCase()
        return !query.length || [entry.reference, tagNames(entry.tags), entry.excerpt || ""].join("\n").toLowerCase().indexOf(query) >= 0
    }
    readonly property var visibleOverlaps: overlaps.filter(function(entry) { return panel.summaryMatches(entry) })
    readonly property var visibleResults: results.filter(function(entry) { return panel.summaryMatches(entry) })
    function focusSummary() { body.forceActiveFocus() }
    readonly property real uiScale: app && app.scale > 0 ? app.scale : desktop.textScale
    readonly property real controlScale: Math.min(uiScale, 1.4, Math.max(1, body.width / 440))
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
        relatedDrawer.close()
        hostKind = "picker"
        browserClosed()
        page = "picker"
        query = ""
        message = ""
        confirmation = null
        filterIds = []
        refresh()
        surface.open()
        Qt.callLater(function() { search.forceActiveFocus() })
        return true
    }
    function openBrowser(manage = false) {
        if (!manage) return openSummary()
        if (!open()) return false
        panel.manage()
        return true
    }
    function openSummary(scope = null) {
        if (!canAct() || !embeddedHost) return false
        const captured = snapshot(scope || bible.tagScope())
        if (captured && !installed(captured)) return false
        releaseEditing()
        hostKind = "browser"
        surface.close()
        relatedDrawer.close()
        summaryScope = captured
        page = "summary"
        query = ""
        message = ""
        confirmation = null
        filterIds = []
        refresh()
        browserRequested()
        Qt.callLater(function() { body.forceActiveFocus() })
        return true
    }
    function followSelection() {
        // The summary follows focus; an editing session always keeps its snapshot.
        if (!browserVisible || page !== "summary") return
        if (!canAct()) return
        const captured = snapshot(bible.tagScope())
        if (captured && !installed(captured)) return
        summaryScope = captured
        confirmation = null
        message = ""
        refresh()
    }
    function embeddedClosed() {
        if (hostKind !== "browser") return
        hostKind = "none"
        confirmation = null
        summaryScope = null
    }
    function openRelated() { return openStudy("", null) }
    // Opens Study on a tab ("" keeps the last one), optionally at a footnote {verse, seq}.
    function openStudy(tab, note) {
        if (!showRelated(bible.tagScope())) return false
        setStudyTab(tab || studyTab, note)
        return true
    }
    function openRelatedForTag(id, source) {
        return showRelated(source, id)
    }
    function showRelated(source, id = "") {
        if (!canAct()) return false
        const captured = snapshot(source)
        if (!installed(captured)) return false
        if (id && !Array.from(bible.tags).some(function(tag) { return tag.id === id })) return false
        releaseEditing()
        hostKind = "related"
        surface.close()
        // Close Annotations before opening/focusing the exclusive Related drawer.
        browserClosed()
        relatedSource = captured
        page = "related"
        query = ""
        message = ""
        matchAll = false
        allVersions = false
        confirmation = null
        allTags = Array.from(bible.tags)
        resetSourceTags()
        if (id) filterIds = [id]
        refresh()
        relatedDrawer.open()
        // The list, not the search field, takes focus: / searches.
        Qt.callLater(function() { panel.focusResults() })
        return true
    }
    function resetSourceTags() {
        let ids = []
        if (installed(relatedSource)) {
            Array.from(bible.tagsForScope(relatedSource, true)).forEach(function(range) {
                Array.from(range.tags).forEach(function(tag) { if (ids.indexOf(tag.id) < 0) ids.push(tag.id) })
            })
        }
        sourceIds = ids
        filterIds = ids.slice()
    }
    function useCurrent() {
        if (!canAct()) return
        const captured = snapshot(bible.tagScope())
        if (!installed(captured)) return
        relatedSource = captured
        query = ""
        message = ""
        resetSourceTags()
        refresh()
    }
    function close() {
        if (hostKind === "picker") surface.close()
        else if (hostKind === "related") relatedDrawer.close()
        else if (hostKind === "browser") { embeddedClosed(); browserClosed() }
    }
    function closeForFocusMode() { close() }
    function textInputActive() {
        let focused = app.activeFocusItem
        while (focused) {
            if ((focused instanceof TextEdit || focused instanceof TextInput) && !focused.readOnly) return true
            focused = focused.parent
        }
        return false
    }
    function focusedSummaryScope() {
        let focused = app.activeFocusItem
        while (focused && focused !== body) {
            if (focused.rangeEditScope) return focused.rangeEditScope
            focused = focused.parent
        }
        return summaryScope
    }
    function moveSummary(delta) {
        const buttons = []
        function collect(item) {
            Array.from(item.children).forEach(function(child) {
                if (!child.visible) return
                if (child instanceof Button && child.enabled) buttons.push(child)
                else collect(child)
            })
        }
        collect(summaryContent)
        if (!buttons.length) return
        const current = buttons.indexOf(app.activeFocusItem)
        const index = Math.max(0, Math.min(buttons.length - 1, current + delta))
        const target = buttons[index]
        target.forceActiveFocus()
        const y = target.mapToItem(summaryContent, 0, 0).y
        if (y < summaryView.contentY) summaryView.contentY = y
        else if (y + target.height > summaryView.contentY + summaryView.height)
            summaryView.contentY = y + target.height - summaryView.height
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
        filterIds = filterIds.filter(function(id) { return validIds.indexOf(id) >= 0 })
        sourceIds = sourceIds.filter(function(id) { return validIds.indexOf(id) >= 0 })
        if (managedId && validIds.indexOf(managedId) < 0) managedId = ""
        if (mergeTarget && validIds.indexOf(mergeTarget) < 0) mergeTarget = ""
        if (confirmation && (!tagById(confirmation.id) || tagById(confirmation.id).name !== confirmation.name
                || (confirmation.action === "merge" && (!tagById(confirmation.target) || tagById(confirmation.target).name !== confirmation.targetName))))
            confirmation = null
        const scope = page === "summary" ? summaryScope : editingScope
        scopeValid = installed(scope)
        exactTags = scopeValid ? Array.from(bible.tagsForScope(scope, false)) : []
        exactIds = exactTags.map(function(tag) { return tag.id })
        overlaps = scopeValid ? Array.from(bible.tagsForScope(scope, true)).filter(function(range) {
            return !sameRange(range, scope)
        }) : []
        let ids = exactIds.slice()
        overlaps.forEach(function(range) {
            Array.from(range.tags).forEach(function(tag) { if (ids.indexOf(tag.id) < 0) ids.push(tag.id) })
        })
        summaryIds = ids
        refreshResults()
    }
    function refreshResults() {
        if (page === "related") {
            results = installed(relatedSource)
                ? Array.from(bible.relatedPassages(relatedSource, filterIds, matchAll, allVersions)) : []
            refreshStudy()
        }
        else if (page === "summary") results = installed(summaryScope) && summaryIds.length
            ? Array.from(bible.relatedPassages(summaryScope, summaryIds, false, false)) : []
        else results = []
    }
    function toggleTag(id) {
        if (page === "summary") return
        if (!tagById(id)) return
        if (page === "manage") {
            managedId = id
            renameInput.text = tagById(id).name
            mergeTarget = ""
            return
        }
        if (page !== "picker") {
            const ids = filterIds.slice()
            const index = ids.indexOf(id)
            if (index < 0) ids.push(id); else ids.splice(index, 1)
            filterIds = ids
            refreshResults()
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
        if (!validName || page !== "picker" || !scopeValid || !canAct() || !installed(editingScope)) return
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
        } else if (page === "summary") {
            summaryScope = captured
            refresh()
        } else if (page !== "related") close()
        Qt.callLater(function() { app.scrollToVerse(bible.cursorVerse, true) })
        return true
    }
    function manage() {
        if (hostKind !== "picker") return
        returnPage = page
        page = "manage"
        query = ""
        managedId = ""
        confirmation = null
        search.forceActiveFocus()
    }
    function back() {
        confirmation = null
        page = returnPage
        query = ""
        message = ""
        refresh()
        search.forceActiveFocus()
    }
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
    readonly property bool summaryKeys: browserVisible && !visibleModal && !app.rangeEditing && !app.contextMenuOpen && !textInputActive()
    readonly property bool relatedKeys: relatedShown && !pickerVisible && !app.rangeEditing && !app.contextMenuOpen && !textInputActive()
        && !relatedDrawer.closing && !tagList.activeFocus
    readonly property bool listKeys: !bible.busy && ((summaryKeys && page === "summary")
        || (relatedKeys && (studyListTab ? studyList.currentIndex >= 0 : resultList.currentIndex >= 0)))
    function listScope() {
        if (page === "related") return studyListTab ? studyScope(studyRows[studyList.currentIndex]) : results[resultList.currentIndex] || null
        return focusedSummaryScope()
    }
    // Related: j/k and the arrows move within the focused list. In the source
    // tags they step down into the results and up into the search field. The
    // tags used to swallow the keys: they had no j/k, and relatedKeys stood
    // down while they held focus, so nothing moved.
    readonly property bool relatedMoveKeys: relatedShown && !pickerVisible && !app.rangeEditing && !app.contextMenuOpen
        && !textInputActive() && !relatedDrawer.closing
    function moveRelated(delta) {
        if (studyListTab) { moveStudy(delta); return }
        const tags = tagList.visible ? tagList.count : 0
        if (tagList.activeFocus) {
            const next = (tagList.currentIndex < 0 ? (delta > 0 ? -1 : 0) : tagList.currentIndex) + delta
            if (next < 0) { search.forceActiveFocus(); return }
            if (next >= tags) {
                if (resultList.count) { resultList.currentIndex = 0; resultList.positionViewAtIndex(0, ListView.Contain); resultList.forceActiveFocus() }
                return
            }
            tagList.currentIndex = next
            tagList.forceActiveFocus()
            return
        }
        moveResult(delta)
    }
    function moveStudy(delta) {
        if (!studyList.count) return
        studyList.currentIndex = Math.max(0, Math.min(studyList.count - 1, studyList.currentIndex + delta))
        studyList.positionViewAtIndex(studyList.currentIndex, ListView.Contain)
        studyList.forceActiveFocus()
    }
    function moveResult(delta) {
        if (!resultList.count) return
        resultList.currentIndex = Math.max(0, Math.min(resultList.count - 1, resultList.currentIndex + delta))
        resultList.positionViewAtIndex(resultList.currentIndex, ListView.Contain)
        resultList.forceActiveFocus()
    }
    function focusInBody() {
        for (let item = app.activeFocusItem; item; item = item.parent) if (item === body) return true
        return false
    }
    function focusResults() {
        if (studyListTab) {
            if (studyList.count && studyList.currentIndex < 0) studyList.currentIndex = 0
            studyList.forceActiveFocus()
            return
        }
        if (resultList.count && resultList.currentIndex < 0) resultList.currentIndex = 0
        resultList.forceActiveFocus()
    }
    function focusSearch() {
        if (page === "related" && hostKind === "related" && studyTab === "related") search.forceActiveFocus()
    }
    // One esc closes the picker. In Related, esc first leaves the search
    // field (keeping its query); the next esc closes the panel.
    function handleEscape() {
        if (confirmation) { confirmation = null; search.forceActiveFocus() }
        else if (page === "related" && search.activeFocus) focusResults()
        else close()
    }
    Connections {
        target: panel.bible
        function onTagsChanged() { if (panel.contentVisible) panel.refresh() }
        function onVersionsChanged() { if (panel.contentVisible) panel.refresh() }
        function onVersionChanged() { if (panel.contentVisible) panel.refreshResults() }
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
        // Tag chips: one step smaller with a visible resting outline and tint.
        property bool chip: false
        font.family: panel.desktop.fontFamily
        font.pixelSize: (chip ? 12 : 13) * panel.controlScale
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
            color: control.down || control.chosen || control.hovered ? panel.desktop.surface
                : control.chip ? Qt.rgba(panel.desktop.accent.r, panel.desktop.accent.g, panel.desktop.accent.b, 0.08) : "transparent"
            // Quiet like the app's other buttons: a border only marks focus or a chosen toggle;
            // chips keep a resting desktop border so they read as tags before hover.
            border.width: control.activeFocus || control.chosen || control.chip ? 1 : 0
            border.color: control.activeFocus || control.chosen ? panel.desktop.accent
                : control.chip && (control.hovered || control.down) ? panel.desktop.muted : panel.desktop.border
            radius: Math.min(panel.desktop.cornerRadius, width / 2, height / 2)
        }
        Keys.onReturnPressed: clicked()
        Keys.onEnterPressed: clicked()
        AppToolTip { theme: panel.desktop; uiScale: panel.uiScale; text: control.hint; visible: text.length > 0 && (control.hovered || control.activeFocus) }
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
            radius: panel.desktop.cornerRadius
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
                Behavior on width { NumberAnimation { duration: 120 } }
                Behavior on opacity { NumberAnimation { duration: 200 } }
            }
        }
        background: Item {}
    }
    component SummaryChips: Flow {
        id: chips
        required property var tags
        required property string prefix
        spacing: 6 * panel.controlScale
        Repeater {
            model: chips.tags
            delegate: TagButton {
                required property var modelData
                objectName: chips.prefix + modelData.id
                text: modelData.name
                width: Math.min(implicitWidth, chips.width)
                wrapText: true
                chip: true
                onClicked: panel.openRelatedForTag(modelData.id, panel.summaryScope)
            }
        }
    }
    component SummaryRange: ColumnLayout {
        id: range
        required property var entry
        required property string prefix
        readonly property var rangeEditScope: entry
        spacing: 4 * panel.controlScale
        TagButton {
            objectName: range.prefix + range.entry.reference
            Layout.fillWidth: true
            text: range.entry.reference
            wrapText: true
            onClicked: panel.openTagged(range.entry)
        }
        SummaryChips {
            Layout.fillWidth: true
            tags: range.entry.tags
            prefix: range.prefix + range.entry.reference + ":tag:"
        }
        TagText {
            Layout.fillWidth: true
            visible: text.length > 0
            text: range.entry.excerpt || ""
            lineHeight: panel.bible.lineHeight > 0 ? panel.bible.lineHeight / 1.5 * 1.25 : 1.25
            maximumLineCount: 3
            elide: Text.ElideRight
            color: panel.desktop.muted
        }
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
        contentItem: Item { id: relatedHost }
        onClosed: {
            if (panel.hostKind !== "related") return
            panel.hostKind = "none"
            panel.confirmation = null
            panel.app.restorePanelFocus()
        }
    }
    Popup {
        id: surface
        objectName: "tagsSurface"
        parent: Overlay.overlay
        width: Math.min(780 * panel.uiScale, parent ? parent.width - 24 : 780)
        height: Math.min(760 * panel.uiScale, parent ? parent.height - 24 : 760)
        x: parent ? (parent.width - width) / 2 : 0
        y: parent ? (parent.height - height) / 2 : 0
        padding: 16 * panel.uiScale
        modal: true
        focus: true
        closePolicy: Popup.NoAutoClose
        // A modal: fades in, closes at once.
        enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: panel.app.fadeMs || 0; easing.type: Easing.OutQuad } }
        exit: null
        onClosed: {
            if (panel.hostKind !== "picker") return
            panel.hostKind = "none"
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
        contentItem: Item { id: pickerHost }
    }
        ColumnLayout {
            id: body
            objectName: "tagsBody"
            parent: panel.hostKind === "browser" ? panel.embeddedHost : panel.hostKind === "related" ? relatedHost : pickerHost
            anchors.fill: parent
            visible: panel.contentVisible
            spacing: 8 * panel.controlScale
            Keys.onEscapePressed: panel.handleEscape()
            Shortcut { sequence: "Shift+E"; enabled: panel.browserVisible && !panel.visibleModal && !panel.app.rangeEditing && !panel.textInputActive(); onActivated: panel.app.openRangeEditor(panel.focusedSummaryScope(), "tag") }
            Shortcut { sequences: ["J", "Down"]; enabled: panel.browserVisible && !panel.visibleModal && !panel.app.rangeEditing && !panel.textInputActive(); onActivated: panel.moveSummary(1) }
            Shortcut { sequences: ["K", "Up"]; enabled: panel.browserVisible && !panel.visibleModal && !panel.app.rangeEditing && !panel.textInputActive(); onActivated: panel.moveSummary(-1) }
            // Shared list keys on the focused summary range or Related result.
            Shortcut { sequence: "Y"; enabled: panel.listKeys; onActivated: panel.app.copyScope(panel.listScope()) }
            Shortcut { sequence: "Shift+Y"; enabled: panel.listKeys; onActivated: panel.app.copyScope(panel.listScope(), true) }
            Shortcut { sequence: "D"; enabled: panel.listKeys; onActivated: panel.app.toggleScopeDesk(panel.listScope()) }
            Shortcut { sequences: ["Delete", "Backspace"]; enabled: panel.listKeys && !panel.studyListTab; onActivated: panel.app.removeMarkerAnnotation(panel.listScope(), "tag") }
            Shortcut { sequence: "Shift+E"; enabled: panel.relatedKeys && !panel.studyListTab && resultList.currentIndex >= 0; onActivated: panel.app.openRangeEditor(panel.listScope(), "tag") }
            Shortcut { objectName: "studyTabPrevious"; sequences: ["H", "Left"]; enabled: panel.relatedMoveKeys; onActivated: panel.stepStudyTab(-1) }
            Shortcut { objectName: "studyTabNext"; sequences: ["L", "Right"]; enabled: panel.relatedMoveKeys; onActivated: panel.stepStudyTab(1) }
            Shortcut { sequences: ["J", "Down"]; enabled: panel.relatedMoveKeys; onActivated: panel.moveRelated(1) }
            Shortcut { sequences: ["K", "Up"]; enabled: panel.relatedMoveKeys; onActivated: panel.moveRelated(-1) }
            // g, shift G, ctrl d/u, ctrl f/b, pgdn/pgup: the Tags tab's ranges, Related's results.
            ListMotionKeys {
                objectName: "summaryMotionKeys"
                active: panel.browserVisible && panel.page === "summary" && !panel.visibleModal && !panel.app.rangeEditing && !panel.app.contextMenuOpen && !panel.textInputActive()
                count: 1000
                onMove: function(delta) { panel.moveSummary(delta) }
            }
            ListMotionKeys {
                objectName: "relatedMotionKeys"
                active: panel.relatedMoveKeys && !panel.studyListTab
                count: resultList.count
                onMove: function(delta) { if (resultList.currentIndex < 0) resultList.currentIndex = 0; panel.moveResult(delta) }
            }
            ListMotionKeys {
                objectName: "studyMotionKeys"
                active: panel.relatedMoveKeys && panel.studyListTab
                count: studyList.count
                onMove: function(delta) { if (studyList.currentIndex < 0) studyList.currentIndex = 0; panel.moveStudy(delta) }
            }
            Shortcut { sequence: "/"; enabled: panel.relatedShown && panel.studyTab === "related" && !panel.pickerVisible && !panel.app.contextMenuOpen && !panel.textInputActive(); onActivated: panel.focusSearch() }
            RowLayout {
                Layout.fillWidth: true
                TagText {
                    objectName: "tagsTitle"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    text: panel.page === "picker" ? (panel.editingScope ? panel.editingScope.reference + " · " + panel.versionName(panel.editingScope) : "Tags")
                        : panel.page === "related" ? "Study"
                        : panel.page === "summary" ? (panel.summaryScope ? panel.summaryScope.reference + " · " + panel.versionName(panel.summaryScope) : "Tags")
                        : panel.page === "manage" ? "Manage tags" : "Tags"
                    font.pixelSize: (panel.page === "related" ? 25 * panel.uiScale : 14 * panel.controlScale)
                }
                TagButton { text: "Back"; visible: panel.page === "manage"; onClicked: panel.back() }
                TagButton { objectName: "manageTags"; text: "Manage"; visible: panel.page === "picker"; onClicked: panel.manage() }
                TagButton { objectName: "tagsDone"; text: "Done"; visible: panel.page !== "summary"; onClicked: panel.close() }
            }
            PanelTabs {
                id: studyTabRow
                objectName: "studyTabs"
                Layout.fillWidth: true
                visible: panel.page === "related"
                labels: panel.studyTitles
                theme: panel.desktop
                uiScale: panel.controlScale
                namePrefix: "studyTab"
                // ←/→ (h/l) drive the tabs; clicks still switch.
                focusable: false
                currentIndex: panel.studyTabs.indexOf(panel.studyTab)
                onActivated: index => panel.setStudyTab(panel.studyTabs[index])
            }
            TagText {
                objectName: "relatedSourceLabel"
                Layout.fillWidth: true
                visible: panel.page === "related" && panel.studyTab !== "footnotes"
                text: panel.relatedSource ? panel.relatedSource.reference + " · " + panel.versionName(panel.relatedSource) : ""
                color: panel.desktop.muted
            }
            Flickable {
                id: summaryView
                objectName: "tagSummary"
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: panel.page === "summary"
                clip: true
                contentWidth: width
                contentHeight: summaryContent.implicitHeight
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: TagScrollbar {}
                ColumnLayout {
                    id: summaryContent
                    width: summaryView.width - 20 * panel.controlScale
                    spacing: 12 * panel.controlScale
                    TagText {
                        objectName: "summaryEmpty"
                        Layout.fillWidth: true
                        visible: panel.summaryIds.length === 0
                        text: "No tags on this passage or overlapping ranges."
                        color: panel.desktop.muted
                    }
                    TagText {
                        Layout.fillWidth: true
                        visible: panel.summaryIds.length > 0
                        text: "Tags on this range"
                    }
                    SummaryChips {
                        objectName: "summaryExactTags"
                        Layout.fillWidth: true
                        visible: panel.exactTags.length > 0
                        tags: panel.exactTags
                        prefix: "summaryTag:"
                    }
                    TagText {
                        Layout.fillWidth: true
                        visible: panel.summaryIds.length > 0 && panel.exactTags.length === 0
                        text: "No tags attached to this exact range."
                        color: panel.desktop.muted
                    }
                    TagText {
                        Layout.fillWidth: true
                        visible: panel.overlaps.length > 0
                        text: "Tags on overlapping ranges"
                    }
                    Repeater {
                        model: panel.page === "summary" ? panel.visibleOverlaps : []
                        delegate: SummaryRange {
                            required property var modelData
                            Layout.fillWidth: true
                            entry: modelData
                            prefix: "summaryOverlap:"
                        }
                    }
                    TagText {
                        Layout.fillWidth: true
                        visible: panel.summaryIds.length > 0
                        text: "Other passages with these tags"
                    }
                    Repeater {
                        model: panel.page === "summary" ? panel.visibleResults : []
                        delegate: SummaryRange {
                            required property var modelData
                            Layout.fillWidth: true
                            entry: modelData
                            prefix: "summaryPassage:"
                        }
                    }
                    TagText {
                        Layout.fillWidth: true
                        visible: panel.summaryIds.length > 0 && panel.visibleResults.length === 0
                        text: "No other passages in this version."
                        color: panel.desktop.muted
                    }
                }
            }
            TagText {
                Layout.fillWidth: true
                visible: (panel.page === "picker" && !panel.scopeValid) || (panel.page === "related" && panel.studyTab !== "footnotes" && !panel.installed(panel.relatedSource))
                objectName: "tagScopePrompt"
                text: panel.page === "picker" && !panel.editingScope
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
                    TagButton { objectName: "confirmTagManagement"; text: "Confirm"; enabled: !panel.bible.busy; onClicked: panel.confirmManagement() }
                }
            }
            TagInput {
                id: search
                objectName: "tagSearch"
                Layout.fillWidth: true
                visible: panel.page !== "summary" && panel.confirmation === null && !panel.studyListTab
                placeholderText: panel.page === "picker" ? "Find or create a tag…" : "Filter tags…"
                text: panel.query
                onTextEdited: panel.query = text
                onAccepted: {
                    if (panel.page === "picker" && panel.validName) panel.createFromQuery()
                    else if (tagList.count) panel.toggleTag(panel.filteredTags[Math.max(0, tagList.currentIndex)].id)
                }
                Keys.onDownPressed: { if (tagList.count) { tagList.currentIndex = 0; tagList.forceActiveFocus() } }
            }
            RowLayout {
                id: filters
                objectName: "tagFilters"
                Layout.fillWidth: true
                spacing: 4 * panel.controlScale
                visible: panel.page === "related" && panel.studyTab === "related"
                TagButton { objectName: "tagMatchAny"; text: "Any"; chosen: !panel.matchAll; onClicked: { panel.matchAll = false; panel.refreshResults() } }
                TagButton { objectName: "tagMatchAll"; text: "All"; chosen: panel.matchAll; onClicked: { panel.matchAll = true; panel.refreshResults() } }
                Rectangle { objectName: "tagFilterDivider"; Layout.preferredWidth: 1; Layout.preferredHeight: 24 * panel.controlScale; Layout.leftMargin: 4 * panel.controlScale; Layout.rightMargin: 4 * panel.controlScale; color: panel.desktop.border }
                TagButton { objectName: "tagSourceVersion"; text: (panel.page === "related" ? panel.versionName(panel.relatedSource) : panel.bible.versionAbbreviation) + " only"; chosen: !panel.allVersions; hint: panel.page === "related" ? "Pinned source version" : "Active version"; onClicked: { panel.allVersions = false; panel.refreshResults() } }
                TagButton { objectName: "tagAllVersions"; text: "All versions"; chosen: panel.allVersions; onClicked: { panel.allVersions = true; panel.refreshResults() } }
                Item { Layout.fillWidth: true }
            }
            TagButton {
                objectName: "tagUseCurrent"
                Layout.fillWidth: true
                text: "Find related to current passage"
                wrapText: true
                hint: "Replace the pinned source with the currently focused passage and its tags."
                visible: panel.page === "related" && panel.studyTab !== "footnotes"
                onClicked: panel.useCurrent()
            }
            ListView {
                id: tagList
                objectName: "tagList"
                Layout.fillWidth: true
                Layout.fillHeight: panel.page === "picker" || panel.page === "manage"
                Layout.preferredHeight: Math.min(150 * panel.controlScale, body.height * 0.22)
                Layout.minimumHeight: 34 * panel.controlScale
                visible: panel.page !== "summary" && panel.confirmation === null && !panel.studyListTab
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
                    chosen: panel.page === "manage" ? panel.managedId === modelData.id
                        : (panel.page === "picker" ? panel.exactIds : panel.filterIds).indexOf(modelData.id) >= 0
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
                        radius: panel.desktop.cornerRadius
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
                    delegate: TagButton {
                        required property var modelData
                        objectName: "mergeTarget:" + modelData.id
                        width: mergeList.width
                        text: modelData.name
                        chosen: panel.mergeTarget === modelData.id
                        onClicked: panel.mergeTarget = modelData.id
                    }
                }
                TagButton { objectName: "mergeTags"; text: "Merge…"; enabled: panel.mergeTarget.length > 0; onClicked: panel.requestManagement("merge") }
            }
            TagText {
                visible: panel.page === "related" && panel.studyTab === "related"
                Layout.fillWidth: true
                text: panel.filterIds.length ? panel.results.length + " passages · " + panel.tagNames(panel.allTags.filter(function(tag) { return panel.filterIds.indexOf(tag.id) >= 0 }))
                    : "Choose one or more tags"
                color: panel.desktop.muted
            }
            ListView {
                id: resultList
                objectName: "tagResults"
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: panel.page === "related" && panel.studyTab === "related"
                clip: true
                spacing: 8 * panel.uiScale
                model: panel.results
                currentIndex: count ? 0 : -1
                activeFocusOnTab: true
                keyNavigationEnabled: true
                Keys.onReturnPressed: { if (currentIndex >= 0) panel.openTagged(panel.results[currentIndex]) }
                Keys.onEnterPressed: { if (currentIndex >= 0) panel.openTagged(panel.results[currentIndex]) }
                ScrollBar.vertical: TagScrollbar { objectName: "tagResultsScrollbar" }
                delegate: TagButton {
                    id: resultDelegate
                    required property var modelData
                    required property int index
                    objectName: "tagResult:" + index
                    width: resultList.width
                    chosen: resultList.activeFocus && resultList.currentIndex === index
                    onClicked: { resultList.currentIndex = index; panel.openTagged(modelData) }
                    contentItem: Column {
                        spacing: 4 * panel.uiScale
                        TagText { width: parent.width; text: resultDelegate.modelData.reference + " · " + resultDelegate.modelData.versionAbbreviation + " · " + resultDelegate.modelData.sharedCount + " shared" }
                        TagText { width: parent.width; text: panel.tagNames(resultDelegate.modelData.tags); color: panel.desktop.accent }
                        TagText { width: parent.width; text: resultDelegate.modelData.excerpt || ""; lineHeight: panel.bible.lineHeight > 0 ? panel.bible.lineHeight / 1.5 * 1.25 : 1.25; maximumLineCount: 3; elide: Text.ElideRight; color: panel.desktop.muted }
                    }
                }
                TagText { anchors.centerIn: parent; visible: resultList.count === 0 && panel.filterIds.length > 0; text: "No matching passages"; color: panel.desktop.muted }
            }
            TagText {
                objectName: "studyCount"
                visible: panel.studyListTab
                Layout.fillWidth: true
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
                    width: studyList.width
                    chosen: studyList.activeFocus && studyList.currentIndex === index
                    onClicked: panel.openStudyRow(index)
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
            Item { Layout.fillHeight: true; visible: panel.confirmation !== null }
            TagText { objectName: "tagMessage"; Layout.fillWidth: true; visible: text.length > 0; text: panel.message }
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 1
                visible: panel.page !== "summary"
                color: panel.desktop.border
                opacity: 0.65
            }
            TagText {
                Layout.fillWidth: true
                visible: panel.page !== "summary"
                objectName: "tagsHints"
                textFormat: Text.StyledText
                text: KeyNotation.hint(panel.page === "picker" ? [["Space", "toggle"], ["Return", "toggle / create"], ["Escape", "close"]]
                    : panel.page === "related" ? (panel.studyTab === "related" ? [[["Left", "Right"], "tabs"], ["/", "search"], ["Return", "open"], ["Escape", "close"]]
                        : [[["Left", "Right"], "tabs"], ["Return", panel.studyTab === "footnotes" ? "go to verse" : "open"], ["Y", "copy"], ["D", "Desk"], ["Escape", "close"]])
                    : [["Return", "open"], ["Escape", "close"]], panel.desktop.foreground, panel.desktop.muted)
                font.pixelSize: 11 * panel.controlScale
                color: panel.desktop.muted
            }
        }
}
