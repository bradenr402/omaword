import QtQuick
import QtQuick.Controls
// Registered by blockdocument.cpp at application startup, without a QML plugin.
// qmllint disable import
import OmaWord.Reader 1.0
// qmllint enable import

Item {
    id: root
    property bool destroying: false
    required property var controller
    required property var backend
    required property var theme
    required property Flickable viewport
    property string mode: "block"
    property alias body: body
    property alias mapping: document
    readonly property bool annotationMenuOpen: annotationMenu.visible
    readonly property real railWidth: 40 * controller.scale
    onRailWidthChanged: if (!destroying) geometryTimer.restart()
    property var annotationRanges: []
    property var annotationGroups: []
    property var inlineMarkers: []
    property var contextRanges: []
    readonly property real cornerRadius: theme.cornerRadius === undefined ? 0 : theme.cornerRadius
    function tagsPlainText(tags) { return Array.from(tags || []).map(function(t) { return t.name }).join(", ") }
    property real annotationScale: 0
    readonly property string annotationKey: JSON.stringify(backend.verses) + JSON.stringify(backend.collection)
        + backend.book + ":" + backend.chapter
    onAnnotationKeyChanged: if (!destroying) geometryTimer.restart()
    function updateAnnotations() {
        if (destroying) return
        const ranges = document.annotations(backend.verses, backend.collection, backend.book, backend.chapter)
        const markers = document.inlineDecorations()
        if (JSON.stringify(markers) !== JSON.stringify(inlineMarkers)) inlineMarkers = markers
        if (JSON.stringify(ranges) === JSON.stringify(annotationRanges) && annotationScale === controller.scale) return
        annotationScale = controller.scale
        annotationMenu.close()
        // Pack nearby line anchors into one stack, keeping each reference an
        // independent action in the chooser. No icon drifts onto another line.
        const groups = []
        for (const range of ranges) {
            if (range.kind !== "note") continue
            let group = groups.length ? groups[groups.length - 1] : null
            if (!group || range.y >= group.y + 54 * controller.scale) {
                group = { y: range.y, bookmark: [], note: [], tag: [] }
                groups.push(group)
            }
            group[range.kind].push(range)
        }
        annotationRanges = ranges
        annotationGroups = groups
    }
    function openAnnotation(entry) {
        if (entry.kind === "bookmark") controller.openBookmark(entry.scope || entry.reference)
        else if (entry.kind === "tag") controller.openPassageTags(entry.scope || entry.reference)
        else {
            if (!controller.flushNote()) return
            const scope = entry.scope
            if (scope && scope.versionId ? backend.openTaggedPassage(scope) : backend.navigate(entry.reference)) {
                controller.clearTextSelection()
                controller.syncNote()
                controller.openNotes(true)
            }
        }
    }
    readonly property bool numbers: backend.showVerseNumbers && !backend.focusMode
    readonly property bool headings: backend.showHeadings && !backend.focusMode
    readonly property string contentKey: JSON.stringify(Array.from(backend.verses).map(function(v) {
        return { number: v.number, html: v.html, text: v.text, heading: v.heading, paragraphStart: v.paragraphStart,
            bookmarkRanges: v.bookmarkRanges, tagRanges: v.tagRanges }
    })) + numbers + headings + backend.lineHeight + mode + backend.focusMode + controller.readerSize + controller.scale
    height: body.height
    onContentKeyChanged: if (!destroying) rebuildTimer.restart()
    function rebuild() {
        if (destroying) return
        controller.clearTextSelection()
        cancelDrag()
        document.build(body.textDocument, backend.verses, numbers, headings, backend.lineHeight, mode,
                       !backend.focusMode, controller.readerSize * controller.scale)
        geometryTimer.restart()
    }
    function refreshGeometry() {
        if (!destroying) {
            contextRanges = backend.focusMode ? [] : document.contextRanges(backend.verses, backend.cursorVerse,
                                                                           backend.selectedStart, backend.selectedEnd)
            paint.requestPaint()
            updateAnnotations()
        }
    }
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
        controller.freeSelectedText = document.selectionText(body.selectionStart, body.selectionEnd)
    }
    // qmllint disable import
    BlockDocument {
        id: document
        onGeometryChanged: if (!root.destroying) geometryTimer.restart()
    }
    // qmllint enable import
    // Paint logical ranges behind the single text layout. A range joins through
    // spaces and wrapped lines; adjacent independent highlights keep their gap.
    Canvas {
        id: paint
        objectName: "blockRangeBackground"
        anchors.fill: body
        property var annotations: root.backend.verses
        property int first: root.backend.selectedStart
        property int last: root.backend.selectedEnd
        property color accent: root.theme.accent
        property bool focused: root.backend.focusMode
        property int cursorVerse: root.backend.cursorVerse
        property var contextRanges: root.contextRanges
        onCursorVerseChanged: if (!root.destroying) geometryTimer.restart()
        onContextRangesChanged: requestPaint()
        onAnnotationsChanged: requestPaint()
        onFirstChanged: if (!root.destroying) geometryTimer.restart()
        onLastChanged: if (!root.destroying) geometryTimer.restart()
        onAccentChanged: requestPaint()
        onFocusedChanged: requestPaint()
        onWidthChanged: if (!root.destroying) geometryTimer.restart()
        onHeightChanged: if (!root.destroying) geometryTimer.restart()
        onPaint: {
            if (root.destroying) return
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
            if (!focused) {
                ctx.fillStyle = root.controller.tint(accent, 0.06)
                for (const range of contextRanges)
                    for (const r of document.rectangles(range.start, range.end)) ctx.fillRect(r.x, r.y, r.width, r.height)
            }
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
        onFontChanged: if (!root.destroying) geometryTimer.restart()
        onWidthChanged: if (!root.destroying) geometryTimer.restart()
        color: root.theme.foreground
        selectionColor: root.controller.tint(root.theme.accent, 0.35)
        selectedTextColor: root.theme.foreground
        readOnly: true
        persistentSelection: true
        activeFocusOnTab: false
        wrapMode: TextEdit.Wrap
        padding: 0
        onContentHeightChanged: paint.requestPaint()
        function syncSelection() {
            document.trackSelection(selectionStart, selectionEnd)
            root.controller.freeSelectedText = document.selectionText(selectionStart, selectionEnd)
        }
        onSelectedTextChanged: syncSelection()
        onSelectionStartChanged: syncSelection()
        onSelectionEndChanged: syncSelection()
        Keys.onShortcutOverride: function(event) {
            if (event.matches(StandardKey.Copy)) event.accepted = true
        }
        Keys.onPressed: function(event) {
            if (event.matches(StandardKey.Copy)) {
                root.controller.freeSelectedText = document.selectionText(selectionStart, selectionEnd)
                if (root.controller.freeSelectedText.length) root.controller.copyFreeText()
                else root.controller.copyPassage(false)
                event.accepted = true
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
    // Each delegate occupies an image cell reserved by QTextDocument. The icon
    // sits to the left of the opening bracket, in the cell's superscript area.
    Repeater {
        model: root.inlineMarkers
        delegate: Item {
            id: inlineMarker
            required property var modelData
            readonly property bool opening: modelData.edge !== "close"
            readonly property string hint: (modelData.kind === "bookmark" ? "Bookmark" : "Tag") + "\n"
                + modelData.reference + (modelData.edge === "continuation" ? " (continued from previous chapter)" : "")
                + (modelData.kind === "tag" ? " · " + root.tagsPlainText(modelData.tags) : "")
            objectName: "blockInlineMarker:" + modelData.kind + ":" + modelData.edge + ":" + modelData.reference
            x: modelData.x; y: modelData.y
            width: modelData.width; height: modelData.height
            z: 2
            visible: !root.backend.focusMode
            activeFocusOnTab: opening
            Keys.onPressed: function(event) {
                if (event.modifiers === Qt.NoModifier && (event.key === Qt.Key_Return || event.key === Qt.Key_Space)) {
                    root.openAnnotation(modelData)
                    event.accepted = true
                }
            }
            Canvas {
                id: inlinePaint
                anchors.fill: parent
                property color accent: root.theme.accent
                onAccentChanged: requestPaint()
                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.reset()
                    ctx.strokeStyle = accent
                    ctx.fillStyle = accent
                    ctx.lineWidth = Math.max(1, height / 18)
                    const opening = inlineMarker.opening
                    const bx = opening ? width * 0.78 : width * 0.65
                    const tip = opening ? width * 0.97 : width * 0.1
                    ctx.beginPath()
                    if (inlineMarker.modelData.edge === "continuation") {
                        // A chevron denotes a continued range, not a false start.
                        ctx.moveTo(bx, height * 0.35)
                        ctx.lineTo(tip, height * 0.62)
                        ctx.lineTo(bx, height * 0.9)
                    } else {
                        ctx.moveTo(tip, height * 0.32)
                        ctx.lineTo(bx, height * 0.32)
                        ctx.lineTo(bx, height * 0.96)
                        ctx.lineTo(tip, height * 0.96)
                    }
                    ctx.stroke()
                    if (!opening) return
                    ctx.save()
                    ctx.translate(width * 0.05, height * 0.03)
                    ctx.scale(width * 0.56 / 12, height * 0.54 / 13)
                    if (inlineMarker.modelData.kind === "bookmark") {
                        ctx.beginPath()
                        ctx.moveTo(1, 0); ctx.lineTo(11, 0); ctx.lineTo(11, 13)
                        ctx.lineTo(6, 9); ctx.lineTo(1, 13); ctx.closePath(); ctx.fill()
                    } else {
                        // Horizontal mirror of the existing right-pointing tag.
                        ctx.translate(12, 0); ctx.scale(-1, 1)
                        ctx.beginPath()
                        ctx.moveTo(1, 1); ctx.lineTo(6, 1); ctx.lineTo(11, 7)
                        ctx.lineTo(6, 12); ctx.lineTo(1, 6); ctx.closePath(); ctx.stroke()
                        ctx.beginPath(); ctx.moveTo(3, 3); ctx.lineTo(4, 4); ctx.stroke()
                    }
                    ctx.restore()
                }
            }
            MouseArea {
                id: inlineMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.openAnnotation(inlineMarker.modelData)
            }
            Accessible.role: Accessible.Button
            Accessible.name: hint
            Accessible.onPressAction: root.openAnnotation(modelData)
            AppToolTip {
                theme: root.theme
                visible: inlineMarker.visible && (inlineMouse.containsMouse || inlineMarker.activeFocus) && !annotationMenu.visible
                delay: 500
                text: inlineMarker.hint
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
                    model: ["note"].filter(function(kind) { return group.modelData[kind].length > 0 })
                    delegate: ToolButton {
                        id: marker
                        required property string modelData
                        readonly property var entries: group.modelData[modelData]
                        readonly property string hint: "Note"
                            + (entries.length > 1 ? "s" : "") + "\n"
                            + entries.map(function(e) { return e.reference + (e.continuesBefore ? " (continued)" : "") + (e.kind === "tag" ? " · " + root.tagsPlainText(e.tags) : "") }).join("\n")
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
                            radius: Math.min(root.cornerRadius, width / 2, height / 2)
                            color: root.controller.tint(root.theme.accent, marker.down ? 0.25 : 0.12)
                            visible: marker.hovered || marker.activeFocus
                        }
                        contentItem: Item {
                            Item {
                                width: 10 * root.controller.scale
                                height: 13 * root.controller.scale
                                anchors.centerIn: parent
                                anchors.horizontalCenterOffset: marker.entries.length > 1 ? -3 * root.controller.scale : 0
                                Rectangle {
                                    anchors.fill: parent
                                    color: "transparent"
                                    border.color: root.theme.accent
                                    border.width: root.controller.scale
                                    radius: 0 // Deliberate document glyph, not a widget surface.
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
                        AppToolTip {
                            id: markerTip
                            theme: root.theme
                            visible: rail.visible && (marker.hovered || marker.activeFocus) && !annotationMenu.visible
                            delay: 500
                            text: marker.hint
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
        background: Rectangle {
            implicitWidth: 260 * root.controller.scale
            color: root.theme.background
            border.color: root.theme.accent
            radius: Math.min(root.cornerRadius, width / 2, height / 2)
        }
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
                id: annotationAction
                required property var modelData
                // Menu buttons otherwise treat Ctrl+Space like Space and
                // activate a reference, exposing the reader to subsequent keys.
                Keys.onPressed: function(event) { if (event.modifiers !== Qt.NoModifier) event.accepted = true }
                Keys.onReleased: function(event) { if (event.modifiers !== Qt.NoModifier) event.accepted = true }
                objectName: "blockAnnotationAction:" + modelData.kind + ":" + modelData.reference
                text: modelData.reference + (modelData.continuesBefore ? " (continued)" : "")
                    + (modelData.kind === "tag" ? " · " + root.tagsPlainText(modelData.tags) : "")
                contentItem: Text {
                    text: annotationAction.text
                    textFormat: Text.PlainText
                    color: root.theme.foreground
                    font: annotationAction.font
                }
                background: Rectangle {
                    implicitHeight: 36 * root.controller.scale
                    color: annotationAction.highlighted ? root.controller.tint(root.theme.accent, 0.16) : "transparent"
                    radius: Math.min(root.cornerRadius, width / 2, height / 2)
                }
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
    // Owned timers coalesce layout work and are canceled with this reader. A
    // global callLater callback can outlive the QML context on window teardown.
    Timer { id: rebuildTimer; interval: 0; onTriggered: root.rebuild() }
    Timer { id: geometryTimer; interval: 0; onTriggered: root.refreshGeometry() }
    Component.onCompleted: rebuild()
    Component.onDestruction: {
        destroying = true
        rebuildTimer.stop()
        geometryTimer.stop()
        autoscroll.stop()
    }
}
