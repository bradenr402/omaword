import QtQuick
// Registered by blockdocument.cpp at application startup, without a QML plugin.
// qmllint disable import
import OmaBible.Reader 1.0
// qmllint enable import

Item {
    id: root
    required property var controller
    required property var backend
    required property var theme
    required property Flickable viewport
    property alias body: body
    property alias mapping: document
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
        Qt.callLater(paint.requestPaint)
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
        controller.freeSelectedText = body.selectedText
    }
    // qmllint disable import
    BlockDocument { id: document }
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
                if (!focused && (verse.bookmarked || verse.hasNote)) {
                    const r = body.positionToRectangle(span.start)
                    ctx.fillStyle = accent
                    ctx.fillRect(r.x, r.y + r.height - 2, verse.bookmarked ? 10 : 4, 2)
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
        onFontChanged: Qt.callLater(paint.requestPaint)
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
