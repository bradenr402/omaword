pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: editor
    required property var app
    required property var bible
    required property var desktop
    property bool sessionOpen: false
    property bool moving: false
    readonly property bool active: sessionOpen && moving
    readonly property bool visibleModal: chooser.visible || merge.visible
    readonly property bool modal: visibleModal
    property var original: null
    property var source: null
    property var ranges: []
    property var components: ({bookmark: false, highlight: false, note: false, tags: []})
    property string contextKind: ""
    property string message: ""
    property var pending: null
    property string coincidentEndpoint: "end"
    property string pinnedVersion: ""
    property bool restoring: false
    readonly property real uiScale: Math.min(desktop.textScale, 1.4)
    readonly property string endpoint: {
        // Invokable results do not register dependencies, including the singleton branch.
        const start = bible.selectedStart, end = bible.selectedEnd
        const cursor = bible.cursorVerse, book = bible.book, chapter = bible.chapter
        const scope = bible.tagScope()
        if (start === end && scope && scope.book === scope.endBook && scope.chapter === scope.endChapter && scope.start === scope.end) return coincidentEndpoint
        return scope && book === scope.book && chapter === scope.chapter && cursor === scope.start ? "start" : "end"
    }
    signal finished(bool saved)

    function copy(value) { return JSON.parse(JSON.stringify(value)) }
    function freeze(value) {
        if (value && typeof value === "object") {
            Object.keys(value).forEach(function(key) { freeze(value[key]) })
            Object.freeze(value)
        }
        return value
    }
    function open(scope = null, kind = "") {
        if (sessionOpen || bible.busy || !bible.ready || !app.flushNote()) return false
        app.savePosition()
        const initial = bible.tagScope()
        original = freeze(copy({scope: initial, versionId: bible.versionId, book: bible.book,
            chapter: bible.chapter, cursor: bible.cursorVerse, selected: app.hasSelection,
            reversed: app.hasSelection && bible.book === initial.book && bible.chapter === initial.chapter && bible.cursorVerse === initial.start,
            position: bible.readingPosition, visual: app.verseMode}))
        ranges = copy(Array.isArray(scope) ? scope : scope ? [scope] : Array.from(bible.annotationRanges(bible.tagScope())))
        if (!ranges.length) { app.notify("No annotations on this range", true); return false }
        contextKind = kind || ""
        source = null
        pinnedVersion = ""
        message = ""
        sessionOpen = true
        app.prepareRangeEditor()
        if (ranges.length === 1) choose(ranges[0])
        chooser.open()
        return true
    }
    function choose(scope) {
        if (scope.versionId !== bible.versionId && !bible.openTaggedPassage(copy(scope))) { message = bible.error; return }
        const snapshot = bible.annotationSnapshot(copy(scope))
        if (snapshot.ok === false || !snapshot.scope || !snapshot.scope.versionId) { message = snapshot.error || bible.error || "This annotation is no longer available."; return }
        source = freeze(copy(snapshot))
        pinnedVersion = source.scope.versionId
        const all = !contextKind
        components = {bookmark: !!source.bookmark && (all || contextKind === "bookmark"),
            highlight: !!source.highlight && (all || contextKind === "highlight"),
            note: !!source.note && (all || contextKind === "note"),
            tags: all || contextKind === "tag" || contextKind === "tags" ? Array.from(source.tags || []).map(function(t) { return t.id }) : []}
    }
    function selectAll() {
        if (!source) return
        components = {bookmark: !!source.bookmark, highlight: !!source.highlight, note: !!source.note,
            tags: Array.from(source.tags || []).map(function(tag) { return tag.id })}
    }
    function toggle(kind, id = "") {
        const next = copy(components)
        if (kind === "tags") {
            const index = next.tags.indexOf(id)
            if (index < 0) next.tags.push(id); else next.tags.splice(index, 1)
        } else next[kind] = !next[kind]
        components = next
    }
    readonly property bool hasComponents: components.bookmark || components.highlight || components.note || components.tags.length > 0
    function begin() {
        if (!source || !hasComponents || bible.busy) return
        if (!bible.openTaggedPassage(copy(source.scope))) { message = bible.error; return }
        moving = true
        coincidentEndpoint = "end"
        app.verseMode = false
        chooser.close()
        app.clearCount()
        app.restorePanelFocus()
    }
    function swap() {
        if (!active || visibleModal || bible.busy) return
        coincidentEndpoint = endpoint === "start" ? "end" : "start"
        if (!bible.swapSelectionEnds()) { message = bible.error; return }
        app.clearCount()
        app.scrollToVerse(bible.cursorVerse, false)
    }
    function accept() {
        if (!active || visibleModal || bible.busy) return
        const destination = copy(bible.tagScope())
        const preview = bible.previewRangeChange(source, destination, components)
        if (!preview.ok) { message = preview.error || bible.error || "Could not save this range."; return }
        pending = copy({destination: destination, preview: preview})
        if (preview.needsNoteMerge) merge.open()
        else commit(false)
    }
    function commit(combine) {
        if (!pending || bible.busy) return
        if (!bible.changeAnnotationRange(source, pending.destination, components, combine, pending.preview.destinationToken || "")) {
            message = bible.error || "Could not save this range. Your edit is still active."
            pending = null
            merge.close()
            app.restorePanelFocus()
            return
        }
        finish(true)
    }
    function cancel() {
        if (!sessionOpen || restoring) return
        const initial = original
        if (!bible.ready || bible.versionId !== initial.versionId || !installed(initial.versionId)) {
            finish(false)
            app.notify("Range edit canceled. Original reading version is no longer active.", true)
            return
        }
        const scope = initial.selected ? initial.scope : {versionId: initial.versionId,
            book: initial.book, chapter: initial.chapter, start: 0, end: 0}
        restoring = true
        const restored = bible.restoreSelection(scope, initial.reversed, initial.position)
        restoring = false
        if (!restored) {
            message = (bible.error || "Could not restore the original reading selection.") + " Press Esc to retry cancellation."
            pending = null
            merge.close()
            app.restorePanelFocus()
            return
        }
        app.restorePosition(false)
        finish(false)
        app.verseMode = initial.visual
    }
    function installed(id) { return Array.from(bible.versions).some(function(version) { return version.id === id }) }
    function checkSource() {
        if (!sessionOpen || !pinnedVersion || installed(pinnedVersion)) return
        // A removed source invalidates the session even if restoration is impossible.
        if (bible.ready && original.versionId === bible.versionId && installed(original.versionId)) cancel()
        if (sessionOpen) finish(false)
        app.notify("Range edit canceled because its source version was removed.", true)
    }
    Connections {
        target: editor.bible
        function onVersionsChanged() { editor.checkSource() }
    }
    function finish(saved) {
        moving = false
        sessionOpen = false
        chooser.close()
        merge.close()
        pending = null
        message = ""
        app.clearCount()
        app.verseMode = false
        app.selectionAnchor = bible.crossChapterSelection ? 0 : (endpoint === "start" ? bible.selectedEnd : bible.selectedStart)
        app.syncNote()
        app.restorePanelFocus()
        finished(saved)
    }
    component RangeText: Label {
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: editor.desktop.foreground
        font.family: editor.desktop.fontFamily
        font.pixelSize: 14 * editor.uiScale
        Layout.fillWidth: true
    }
    component RangeButton: Button {
        id: button
        property string hint: ""
        font.family: editor.desktop.fontFamily
        font.pixelSize: 13 * editor.uiScale
        padding: 8 * editor.uiScale
        contentItem: Text {
            text: button.text; textFormat: Text.PlainText; font: button.font
            color: button.enabled ? editor.desktop.foreground : editor.desktop.muted
            wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            color: button.down || button.hovered ? editor.desktop.surface : editor.desktop.background
            border.color: button.activeFocus ? editor.desktop.accent : editor.desktop.border
            radius: editor.desktop.cornerRadius
        }
        Keys.onReturnPressed: clicked()
        Keys.onEnterPressed: clicked()
        AppToolTip { theme: editor.desktop; text: button.hint; visible: text.length > 0 && (button.hovered || button.activeFocus) }
    }
    component RangeCheckBox: CheckBox {
        id: check
        Layout.fillWidth: true
        font.family: editor.desktop.fontFamily
        font.pixelSize: 14 * editor.uiScale
        padding: 8 * editor.uiScale
        spacing: 8 * editor.uiScale
        indicator: Rectangle {
            x: check.leftPadding; y: (check.height - height) / 2
            implicitWidth: 20 * editor.uiScale; implicitHeight: implicitWidth
            color: check.checked ? editor.desktop.accent : editor.desktop.background
            border.color: check.activeFocus ? editor.desktop.accent : editor.desktop.border
            radius: Math.min(editor.desktop.cornerRadius, width / 2)
            Text { anchors.centerIn: parent; text: check.checked ? "✓" : ""; font: check.font; color: editor.desktop.background }
        }
        contentItem: Text {
            text: check.text; textFormat: Text.PlainText; font: check.font
            leftPadding: check.indicator.width + check.spacing
            color: check.enabled ? editor.desktop.foreground : editor.desktop.muted
            wrapMode: Text.Wrap; verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            color: check.hovered ? editor.desktop.surface : "transparent"
            border.width: check.activeFocus ? 1 : 0; border.color: editor.desktop.accent
            radius: editor.desktop.cornerRadius
        }
    }
    component RangeDialog: Popup {
        parent: Overlay.overlay
        width: Math.min(600 * editor.desktop.textScale, editor.app.width - 32)
        height: Math.min(contentItem.implicitHeight + 32, editor.app.height - 32)
        anchors.centerIn: parent
        padding: 16
        modal: true
        focus: true
        closePolicy: Popup.NoAutoClose
        background: Rectangle { color: editor.desktop.background; border.color: editor.desktop.border; radius: editor.desktop.cornerRadius }
    }
    RangeDialog {
        id: chooser
        objectName: "rangeChooser"
        onOpened: contentItem.forceActiveFocus()
        contentItem: ColumnLayout {
            Keys.onEscapePressed: editor.cancel()
            RangeText { text: editor.source ? "Choose components · " + editor.source.scope.reference : "Choose an exact annotation range"; font.bold: true }
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                implicitHeight: Math.min(380, choices.implicitHeight)
                contentWidth: availableWidth
                ColumnLayout {
                    id: choices
                    width: parent.width
                    Repeater {
                        model: editor.source ? [] : editor.ranges
                        delegate: RangeButton {
                            required property var modelData
                            objectName: "rangeChoice:" + modelData.reference
                            text: modelData.reference
                            Layout.fillWidth: true
                            onClicked: editor.choose(modelData)
                        }
                    }
                    RangeButton {
                        objectName: "rangeSelectAll"; text: "Select all components"
                        visible: !!editor.source
                        hint: "Move every component attached to this exact range"
                        onClicked: editor.selectAll()
                    }
                    Repeater {
                        model: editor.source ? ["bookmark", "highlight", "note"] : []
                        delegate: RangeCheckBox {
                            required property string modelData
                            objectName: "rangeComponent:" + modelData
                            text: modelData.charAt(0).toUpperCase() + modelData.slice(1)
                            enabled: !!editor.source && !!editor.source[modelData]
                            checked: editor.components[modelData]
                            onClicked: editor.toggle(modelData)
                        }
                    }
                    Repeater {
                        model: editor.source ? editor.source.tags || [] : []
                        delegate: RangeCheckBox {
                            required property var modelData
                            objectName: "rangeTag:" + modelData.id
                            text: "Tag · " + modelData.name
                            checked: editor.components.tags.indexOf(modelData.id) >= 0
                            onClicked: editor.toggle("tags", modelData.id)
                        }
                    }
                }
            }
            RangeText { text: editor.message; visible: text.length > 0 }
            RowLayout {
                RangeButton { text: "Cancel"; onClicked: editor.cancel() }
                RangeButton { text: "Choose another range"; visible: !!editor.source && editor.ranges.length > 1; onClicked: editor.source = null }
                RangeButton { objectName: "rangeContinue"; text: "Continue"; enabled: !!editor.source && editor.hasComponents && !editor.bible.busy; onClicked: editor.begin() }
            }
        }
    }
    RangeDialog {
        id: merge
        objectName: "rangeNoteMerge"
        onOpened: contentItem.forceActiveFocus()
        contentItem: ColumnLayout {
            Keys.onEscapePressed: editor.cancel()
            RangeText { text: "Combine notes?"; font.bold: true }
            RangeText { text: "Both ranges have notes. Combine keeps both, separated by a blank line." }
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                implicitHeight: Math.min(320, noteTexts.implicitHeight)
                contentWidth: availableWidth
                ColumnLayout {
                    id: noteTexts
                    width: parent.width
                    RangeText { text: "Source note"; font.bold: true }
                    RangeText { objectName: "rangeSourceNote"; text: editor.pending ? editor.pending.preview.sourceNote : "" }
                    RangeText { text: "Destination note"; font.bold: true }
                    RangeText { objectName: "rangeDestinationNote"; text: editor.pending ? editor.pending.preview.destinationNote : "" }
                }
            }
            RowLayout {
                RangeButton { objectName: "rangeMergeCancel"; text: "Cancel"; onClicked: editor.cancel() }
                RangeButton { objectName: "rangeMergeCombine"; text: "Combine and save"; enabled: !editor.bible.busy; onClicked: editor.commit(true) }
            }
        }
    }
}
