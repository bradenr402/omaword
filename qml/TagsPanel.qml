pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: panel
    required property var app
    required property var bible
    required property var desktop

    readonly property bool visibleModal: surface.visible
    readonly property bool scopePinned: surface.visible && editingScope !== null
    property var editingScope: null
    property var relatedSource: null
    property string page: "picker"
    property string returnPage: "browser"
    property string query: ""
    property var allTags: []
    property var exactIds: []
    property var overlaps: []
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
    readonly property real uiScale: desktop.textScale
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
        if (!installed(captured)) return false
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
    function openBrowser(manage = false) {
        if (!canAct()) return false
        releaseEditing()
        relatedSource = null
        page = manage ? "manage" : "browser"
        returnPage = "browser"
        query = ""
        message = ""
        filterIds = []
        matchAll = false
        allVersions = false
        managedId = ""
        confirmation = null
        refresh()
        surface.open()
        Qt.callLater(function() { search.forceActiveFocus() })
        return true
    }
    function openRelated() {
        if (!canAct()) return false
        const captured = snapshot(bible.tagScope())
        if (!installed(captured)) return false
        releaseEditing()
        relatedSource = captured
        page = "related"
        query = ""
        message = ""
        matchAll = false
        allVersions = false
        confirmation = null
        allTags = Array.from(bible.tags)
        resetSourceTags()
        refresh()
        surface.open()
        Qt.callLater(function() { search.forceActiveFocus() })
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
    function closeForFocusMode() { surface.close() }
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
        scopeValid = installed(editingScope)
        exactIds = scopeValid ? Array.from(bible.tagsForScope(editingScope, false)).map(function(tag) { return tag.id }) : []
        overlaps = scopeValid ? Array.from(bible.tagsForScope(editingScope, true)).filter(function(range) {
            return !sameRange(range, editingScope)
        }) : []
        refreshResults()
    }
    function refreshResults() {
        if (page === "browser") results = Array.from(bible.taggedPassages(filterIds, matchAll, allVersions ? "" : bible.versionId))
        else if (page === "related") results = installed(relatedSource)
            ? Array.from(bible.relatedPassages(relatedSource, filterIds, matchAll, allVersions)) : []
        else results = []
    }
    function toggleTag(id) {
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
        app.savePosition()
        if (!bible.openTaggedPassage(captured)) { failed(); return false }
        app.syncNote()
        if (edit) {
            // Main captures Visual-mode completion context for the new exact range.
            app.openPassageTags(captured)
        } else if (page !== "related") surface.close()
        Qt.callLater(function() { app.scrollToVerse(bible.cursorVerse, true) })
        return true
    }
    function manage() {
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
    function handleEscape() {
        if (confirmation) { confirmation = null; search.forceActiveFocus() }
        else if (query.length) query = ""
        else surface.close()
    }
    Connections {
        target: panel.bible
        function onTagsChanged() { if (surface.visible) panel.refresh() }
        function onVersionsChanged() { if (surface.visible) panel.refresh() }
        function onVersionChanged() { if (surface.visible) panel.refreshResults() }
    }

    component TagText: Text {
        color: panel.desktop.foreground
        font.family: panel.desktop.fontFamily
        font.pixelSize: 14 * panel.uiScale
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
    }
    component TagButton: Button {
        id: control
        property bool chosen: false
        property string hint: ""
        font.family: panel.desktop.fontFamily
        font.pixelSize: 13 * panel.uiScale
        padding: 8 * panel.uiScale
        implicitHeight: Math.max(34 * panel.uiScale, contentItem.implicitHeight + topPadding + bottomPadding)
        contentItem: Text {
            text: control.text
            font: control.font
            color: control.enabled ? panel.desktop.foreground : panel.desktop.muted
            textFormat: Text.PlainText
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            color: control.down || control.chosen || control.hovered ? panel.desktop.surface : "transparent"
            border.color: control.activeFocus || control.chosen ? panel.desktop.accent : panel.desktop.border
            radius: panel.desktop.cornerRadius
        }
        Keys.onReturnPressed: clicked()
        Keys.onEnterPressed: clicked()
        AppToolTip { theme: panel.desktop; text: control.hint; visible: text.length > 0 && (control.hovered || control.activeFocus) }
    }
    component TagInput: TextField {
        id: input
        font.family: panel.desktop.fontFamily
        font.pixelSize: 14 * panel.uiScale
        color: panel.desktop.foreground
        placeholderTextColor: panel.desktop.muted
        selectionColor: panel.desktop.accent
        selectedTextColor: panel.desktop.background
        padding: 10 * panel.uiScale
        background: Rectangle {
            color: panel.desktop.background
            border.color: input.activeFocus ? panel.desktop.accent : panel.desktop.border
            radius: panel.desktop.cornerRadius
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
        onClosed: {
            panel.confirmation = null
            panel.editingScope = null
            panel.app.editingPanelClosed()
            panel.app.restorePanelFocus()
        }
        background: Rectangle {
            color: panel.desktop.background
            border.color: panel.desktop.border
            radius: panel.desktop.cornerRadius
        }
        contentItem: ColumnLayout {
            spacing: 10 * panel.uiScale
            Keys.onEscapePressed: panel.handleEscape()
            RowLayout {
                Layout.fillWidth: true
                TagText {
                    Layout.fillWidth: true
                    text: panel.page === "picker" ? (panel.editingScope ? panel.editingScope.reference + " · " + panel.versionName(panel.editingScope) : "Tags")
                        : panel.page === "related" ? "Related · " + (panel.relatedSource ? panel.relatedSource.reference + " · " + panel.versionName(panel.relatedSource) : "")
                        : panel.page === "manage" ? "Manage tags" : "Saved tags"
                }
                TagButton { text: "Back"; visible: panel.page === "manage"; onClicked: panel.back() }
                TagButton { text: "Manage"; visible: panel.page !== "manage"; onClicked: panel.manage() }
                TagButton { text: "Done"; onClicked: surface.close() }
            }
            TagText {
                Layout.fillWidth: true
                visible: (panel.page === "picker" && !panel.scopeValid) || (panel.page === "related" && !panel.installed(panel.relatedSource))
                text: "This source version is no longer installed. Choose a current passage to continue."
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
                visible: panel.confirmation === null
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
                visible: panel.page === "browser" || panel.page === "related"
                TagButton { objectName: "tagMatchAny"; text: "Any"; chosen: !panel.matchAll; onClicked: { panel.matchAll = false; panel.refreshResults() } }
                TagButton { objectName: "tagMatchAll"; text: "All"; chosen: panel.matchAll; onClicked: { panel.matchAll = true; panel.refreshResults() } }
                TagButton { objectName: "tagAllVersions"; text: panel.allVersions ? "All versions" : "Current version"; chosen: panel.allVersions; hint: panel.page === "related" ? "Current version means the pinned source version" : "Include all installed versions"; onClicked: { panel.allVersions = !panel.allVersions; panel.refreshResults() } }
                TagButton { objectName: "tagUseCurrent"; text: "Use current"; visible: panel.page === "related"; onClicked: panel.useCurrent() }
            }
            ListView {
                id: tagList
                objectName: "tagList"
                Layout.fillWidth: true
                Layout.fillHeight: panel.page === "picker" || panel.page === "manage"
                Layout.preferredHeight: 150 * panel.uiScale
                Layout.minimumHeight: 50 * panel.uiScale
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
                ScrollBar.vertical: ScrollBar { }
                delegate: TagButton {
                    id: tagDelegate
                    required property var modelData
                    required property int index
                    objectName: "tag:" + modelData.id
                    width: tagList.width
                    chosen: panel.page === "manage" ? panel.managedId === modelData.id
                        : (panel.page === "picker" ? panel.exactIds : panel.filterIds).indexOf(modelData.id) >= 0
                    text: (chosen ? "✓  " : "    ") + modelData.name + "  · " + modelData.count
                    hint: modelData.name
                    enabled: panel.page !== "picker" || (panel.scopeValid && !panel.bible.busy)
                    onClicked: { tagList.currentIndex = index; panel.toggleTag(modelData.id) }
                    background: Rectangle {
                        color: tagDelegate.chosen || tagDelegate.hovered ? panel.desktop.surface : "transparent"
                        border.color: tagDelegate.activeFocus || (tagList.activeFocus && tagList.currentIndex === tagDelegate.index) ? panel.desktop.accent : "transparent"
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
                Layout.preferredHeight: Math.min(150 * panel.uiScale, contentHeight)
                visible: panel.page === "picker" && panel.overlaps.length > 0
                model: panel.overlaps
                clip: true
                ScrollBar.vertical: ScrollBar { }
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
                    ScrollBar.vertical: ScrollBar { }
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
                visible: panel.page === "browser" || panel.page === "related"
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
                visible: panel.page === "browser" || panel.page === "related"
                clip: true
                spacing: 8 * panel.uiScale
                model: panel.results
                currentIndex: count ? 0 : -1
                activeFocusOnTab: true
                keyNavigationEnabled: true
                Keys.onReturnPressed: { if (currentIndex >= 0) panel.openTagged(panel.results[currentIndex]) }
                Keys.onEnterPressed: { if (currentIndex >= 0) panel.openTagged(panel.results[currentIndex]) }
                ScrollBar.vertical: ScrollBar { }
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
                        TagText { width: parent.width; text: resultDelegate.modelData.excerpt || ""; maximumLineCount: 3; elide: Text.ElideRight; color: panel.desktop.muted }
                    }
                }
                TagText { anchors.centerIn: parent; visible: resultList.count === 0 && panel.filterIds.length > 0; text: "No matching passages"; color: panel.desktop.muted }
            }
            Item { Layout.fillHeight: true; visible: panel.confirmation !== null }
            TagText { objectName: "tagMessage"; Layout.fillWidth: true; visible: text.length > 0; text: panel.message }
            TagText {
                Layout.fillWidth: true
                text: panel.page === "picker" ? "Space / Enter toggles · Enter in search creates / assigns · Esc clears, then closes"
                    : "Enter opens a passage · Esc clears, then closes"
                font.pixelSize: 11 * panel.uiScale
                color: panel.desktop.muted
            }
        }
    }
}
