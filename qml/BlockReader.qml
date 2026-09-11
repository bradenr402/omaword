import QtQuick
import QtQuick.Controls
import QtQuick.Shapes
// Registered by blockdocument.cpp at application startup, without a QML plugin.
// qmllint disable import
import OmaWord.Reader 1.0
// qmllint enable import

Item {
    id: root
    required property var controller
    required property var backend
    required property var theme
    required property Flickable viewport
    property alias body: body
    property alias mapping: document
    readonly property bool annotationMenuOpen: annotationMenu.visible
    readonly property real railWidth: 40 * controller.scale
    onRailWidthChanged: Qt.callLater(updateAnnotations)
    property var annotationRanges: []
    property var annotationGroups: []
    property real annotationScale: 0
    readonly property string annotationKey: JSON.stringify(backend.verses) + JSON.stringify(backend.collection)
        + backend.book + ":" + backend.chapter
    onAnnotationKeyChanged: Qt.callLater(updateAnnotations)
    function updateAnnotations() {
        const ranges = document.annotations(backend.verses, backend.collection, backend.book, backend.chapter)
        if (JSON.stringify(ranges) === JSON.stringify(annotationRanges) && annotationScale === controller.scale) return
        annotationScale = controller.scale
        annotationMenu.close()
        // Pack nearby line anchors into one stack, keeping each reference an
        // independent action in the chooser. No icon drifts onto another line.
        const groups = []
        for (const range of ranges) {
            let group = groups.length ? groups[groups.length - 1] : null
            if (!group || range.y >= group.y + 36 * controller.scale) {
                group = { y: range.y, bookmark: [], note: [] }
                groups.push(group)
            }
            group[range.kind].push(range)
        }
        annotationRanges = ranges
        annotationGroups = groups
    }
    function openAnnotation(entry) {
        if (entry.kind === "bookmark") controller.openBookmark(entry.reference)
        else {
            if (!controller.flushNote()) return
            if (backend.navigate(entry.reference)) {
                controller.clearTextSelection()
                controller.syncNote()
                controller.openNotes(true)
            }
        }
    }
    readonly property bool numbers: backend.showVerseNumbers && !backend.focusMode
    readonly property bool headings: backend.showHeadings && !backend.focusMode
    readonly property string contentKey: JSON.stringify(Array.from(backend.verses).map(function(v) {
        return { number: v.number, html: v.html, text: v.text, heading: v.heading }
    })) + numbers + headings + backend.lineHeight
    height: body.height
    onContentKeyChanged: Qt.callLater(rebuild)
    function rebuild() {
        controller.clearTextSelection()
        cancelDrag()
        document.build(body.textDocument, backend.verses, numbers, headings, backend.lineHeight)
        Qt.callLater(refreshGeometry)
    }
    function refreshGeometry() { paint.requestPaint(); updateAnnotations() }
    function cancelDrag() { dragging = false; pressed = false; autoscroll.stop() }
    function clearSelection() { body.deselect() }
    function verseRect(number) { return body.positionToRectangle(document.positionFor(number)) }
    function hit(x, y) { return document.verseAtPoint(x, y) }
    property bool pressed: false
    property bool dragging: false
    property point origin
    property point pointer
    property int anchor: 0
    property int pressedVerse: 0
    function updateDrag() {
        const p = body.mapFromItem(viewport, pointer.x, pointer.y)
        body.select(anchor, body.positionAt(p.x, p.y))
        controller.freeSelectedText = body.selectedText
    }
    // qmllint disable import
    BlockDocument {
        id: document
        onGeometryChanged: Qt.callLater(root.refreshGeometry)
    }
    // qmllint enable import
    // Paint logical ranges behind the single text layout. A range joins through
    // spaces and wrapped lines; adjacent independent highlights keep their gap.
    Canvas {
        id: paint
        anchors.fill: body
        property var annotations: root.backend.verses
        property int first: root.backend.selectedStart
        property int last: root.backend.selectedEnd
        property color accent: root.theme.accent
        property bool focused: root.backend.focusMode
        onAnnotationsChanged: requestPaint()
        onFirstChanged: requestPaint()
        onLastChanged: requestPaint()
        onAccentChanged: requestPaint()
        onFocusedChanged: requestPaint()
        onWidthChanged: Qt.callLater(requestPaint)
        onHeightChanged: Qt.callLater(requestPaint)
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const spans = document.spans
            const ranges = {}
            for (let i = 0; i < spans.length; ++i) {
                const span = spans[i], verse = annotations[i]
                if (!verse) continue
                for (const range of (verse.highlightRanges || [])) {
                    if (!ranges[range.reference]) ranges[range.reference] = { start: span.start, end: span.end }
                    else ranges[range.reference].end = span.end
                }
            }
            function draw(start, end, selected) {
                for (const r of document.rectangles(start, end)) {
                    ctx.fillStyle = root.controller.tint(accent, selected ? 0.22 : 0.16)
                    ctx.fillRect(r.x, r.y, r.width, r.height)
                    if (!selected && !focused) {
                        ctx.fillStyle = root.controller.tint(accent, 0.7)
                        ctx.fillRect(r.x, r.y, 2, r.height)
                    }
                }
            }
            for (const ref in ranges) draw(ranges[ref].start, ranges[ref].end, false)
            const selected = Array.from(spans).filter(function(s) { return first > 0 && s.number >= first && s.number <= last })
            if (selected.length) draw(selected[0].start, selected[selected.length - 1].end, true)
        }
    }
    TextEdit {
        id: body
        objectName: "blockBody"
        width: parent.width
        height: contentHeight
        textFormat: TextEdit.RichText
        font.family: root.theme.fontFamily
        font.pixelSize: root.controller.readerSize * root.controller.scale
        onFontChanged: Qt.callLater(root.refreshGeometry)
        onWidthChanged: Qt.callLater(root.refreshGeometry)
        color: root.theme.foreground
        selectionColor: root.controller.tint(root.theme.accent, 0.35)
        selectedTextColor: root.theme.foreground
        readOnly: true
        persistentSelection: true
        activeFocusOnTab: false
        wrapMode: TextEdit.Wrap
        padding: 0
        onContentHeightChanged: paint.requestPaint()
        Keys.onShortcutOverride: function(event) {
            if (event.key === Qt.Key_C && event.modifiers === Qt.ControlModifier && root.controller.freeSelectedText.length) event.accepted = true
        }
        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_C && event.modifiers === Qt.ControlModifier && root.controller.freeSelectedText.length) {
                root.controller.copyFreeText(); event.accepted = true
            }
        }
        MouseArea {
            id: mouseArea
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
            preventStealing: true
            cursorShape: Qt.IBeamCursor
            onPressed: function(mouse) {
                if (mouse.button !== Qt.LeftButton) return
                root.pressed = true
                root.origin = mapToItem(root.viewport, mouse.x, mouse.y)
                root.pointer = root.origin
                root.anchor = body.positionAt(mouse.x, mouse.y)
                root.pressedVerse = root.hit(mouse.x, mouse.y)
            }
            onPositionChanged: function(mouse) {
                if (!root.pressed) return
                root.pointer = mapToItem(root.viewport, mouse.x, mouse.y)
                // Qt exposes this runtime style hint without QML property metadata.
                // qmllint disable missing-property
                if (!root.dragging && Math.hypot(root.pointer.x - root.origin.x, root.pointer.y - root.origin.y) < Qt.styleHints.startDragDistance) return
                // qmllint enable missing-property
                if (!root.dragging) { root.controller.clearTextSelection(); body.forceActiveFocus(); root.dragging = true; autoscroll.start() }
                root.updateDrag()
            }
            onReleased: function(mouse) {
                if (mouse.button === Qt.LeftButton) {
                    if (!root.dragging) root.controller.selectVerse(root.pressedVerse, !!(mouse.modifiers & Qt.ShiftModifier))
                    root.cancelDrag()
                } else root.controller.copyClickedVerse(root.hit(mouse.x, mouse.y), mouse.button === Qt.MiddleButton)
            }
            onCanceled: root.cancelDrag()
            onDoubleClicked: function(mouse) {
                if (mouse.button !== Qt.LeftButton) return
                root.controller.selectVerse(root.hit(mouse.x, mouse.y), false)
                root.controller.openNotes(true)
            }
        }
    }
    // Outside the text's bounds, above both the highlight canvas and TextEdit.
    // Main reserves this gutter at the BlockReader loader's left edge.
    Item {
        id: rail
        objectName: "blockAnnotationRail"
        x: -root.railWidth
        width: root.railWidth
        height: root.height
        z: 2
        visible: !root.backend.focusMode
        onVisibleChanged: if (!visible) annotationMenu.close()
        Repeater {
            model: root.annotationRanges.filter(function(r) { return r.kind === "bookmark" })
            delegate: Item {
                id: bracket
                required property var modelData
                required property int index
                objectName: "blockBookmarkExtent:" + modelData.reference
                // Separate overlapping and adjacent ranges, even if their
                // endpoints are mid-line. Brackets cover the whole line box.
                x: (2 + (index % 3) * 3) * root.controller.scale
                y: modelData.y
                width: 4 * root.controller.scale
                height: modelData.height
                Rectangle {
                    width: root.controller.scale
                    height: parent.height
                    color: root.theme.accent
                    opacity: 0.5
                }
                Rectangle {
                    width: parent.width; height: root.controller.scale
                    color: root.theme.accent
                    visible: !bracket.modelData.continuesBefore
                }
                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width; height: root.controller.scale
                    color: root.theme.accent
                    visible: !bracket.modelData.continuesAfter
                }
            }
        }
        Repeater {
            model: root.annotationGroups
            delegate: Column {
                id: group
                required property var modelData
                objectName: "blockAnnotationGroup:" + index
                required property int index
                x: 13 * root.controller.scale
                y: modelData.y
                width: 23 * root.controller.scale
                Repeater {
                    model: ["bookmark", "note"].filter(function(kind) { return group.modelData[kind].length > 0 })
                    delegate: ToolButton {
                        id: marker
                        required property string modelData
                        readonly property var entries: group.modelData[modelData]
                        readonly property string hint: (modelData === "bookmark" ? "Bookmark" : "Note")
                            + (entries.length > 1 ? "s" : "") + "\n"
                            + entries.map(function(e) { return e.reference + (e.continuesBefore ? " (continued)" : "") }).join("\n")
                        objectName: "blockAnnotationMarker:" + modelData + ":" + entries[0].reference
                        width: group.width
                        height: 18 * root.controller.scale
                        padding: 0
                        hoverEnabled: true
                        Accessible.name: hint
                        onClicked: {
                            if (entries.length === 1) root.openAnnotation(entries[0])
                            else {
                                annotationMenu.entries = entries
                                const p = mapToItem(root, width, 0)
                                annotationMenu.popup(p.x, p.y)
                            }
                        }
                        background: Rectangle {
                            radius: 0
                            color: root.controller.tint(root.theme.accent, marker.down ? 0.25 : 0.12)
                            visible: marker.hovered || marker.activeFocus
                        }
                        contentItem: Item {
                            Item {
                                width: 10 * root.controller.scale
                                height: 13 * root.controller.scale
                                anchors.centerIn: parent
                                anchors.horizontalCenterOffset: marker.entries.length > 1 ? -3 * root.controller.scale : 0
                                Shape {
                                    anchors.fill: parent
                                    visible: marker.modelData === "bookmark"
                                    ShapePath {
                                        strokeColor: "transparent"
                                        fillColor: root.theme.accent
                                        startX: 0; startY: 0
                                        PathLine { x: 10 * root.controller.scale; y: 0 }
                                        PathLine { x: 10 * root.controller.scale; y: 13 * root.controller.scale }
                                        PathLine { x: 5 * root.controller.scale; y: 9 * root.controller.scale }
                                        PathLine { x: 0; y: 13 * root.controller.scale }
                                        PathLine { x: 0; y: 0 }
                                    }
                                }
                                Rectangle {
                                    anchors.fill: parent
                                    visible: marker.modelData === "note"
                                    color: "transparent"
                                    border.color: root.theme.accent
                                    border.width: root.controller.scale
                                    radius: root.controller.scale
                                    Repeater {
                                        model: 3
                                        Rectangle {
                                            required property int index
                                            x: 3 * root.controller.scale
                                            y: (3 + index * 3) * root.controller.scale
                                            width: (index === 2 ? 3 : 4) * root.controller.scale
                                            height: root.controller.scale
                                            color: root.theme.accent
                                        }
                                    }
                                }
                            }
                            Text {
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                visible: marker.entries.length > 1
                                text: marker.entries.length
                                font.pixelSize: 8 * root.controller.scale
                                color: root.theme.accent
                            }
                        }
                        ToolTip {
                            id: markerTip
                            visible: rail.visible && (marker.hovered || marker.activeFocus) && !annotationMenu.visible
                            delay: 500
                            text: marker.hint
                            palette.window: root.theme.background
                            palette.windowText: root.theme.foreground
                            contentItem: Text {
                                text: markerTip.text
                                color: root.theme.foreground
                                font.family: root.theme.fontFamily
                                font.pixelSize: 12 * root.controller.scale
                            }
                            background: Rectangle {
                                color: root.theme.background
                                border.color: root.theme.accent
                                radius: 4 * root.controller.scale
                            }
                        }
                    }
                }
            }
        }
    }
    Menu {
        id: annotationMenu
        objectName: "blockAnnotationMenu"
        popupType: Popup.Item
        modal: true
        focus: true
        onOpened: {
            currentIndex = 0
            if (itemAt(0)) itemAt(0).forceActiveFocus()
        }
        property var entries: []
        palette.window: root.theme.background
        palette.base: root.theme.background
        palette.text: root.theme.foreground
        palette.windowText: root.theme.foreground
        palette.buttonText: root.theme.foreground
        palette.highlight: root.theme.accent
        palette.highlightedText: root.theme.background
        Instantiator {
            model: annotationMenu.entries
            delegate: MenuItem {
                required property var modelData
                // Menu buttons otherwise treat Ctrl+Space like Space and
                // activate a reference, exposing the reader to subsequent keys.
                Keys.onPressed: function(event) { if (event.modifiers !== Qt.NoModifier) event.accepted = true }
                Keys.onReleased: function(event) { if (event.modifiers !== Qt.NoModifier) event.accepted = true }
                objectName: "blockAnnotationAction:" + modelData.kind + ":" + modelData.reference
                text: modelData.reference + (modelData.continuesBefore ? " (continued)" : "")
                onTriggered: root.openAnnotation(modelData)
            }
            onObjectAdded: function(index, object) { annotationMenu.insertItem(index, object) }
            onObjectRemoved: function(index, object) { annotationMenu.removeItem(object) }
        }
    }
    Timer {
        id: autoscroll
        interval: 16
        repeat: true
        onTriggered: {
            const edge = 48 * root.controller.scale
            const bottom = root.viewport.height - root.controller.floatingSafety
            const speed = root.pointer.y < edge ? -Math.min(edge, edge - root.pointer.y)
                : root.pointer.y > bottom - edge ? Math.min(edge, root.pointer.y - bottom + edge) : 0
            if (speed) { root.controller.scrollBy(speed * 0.35); root.updateDrag() }
        }
    }
    Component.onCompleted: rebuild()
}
