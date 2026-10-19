import QtQuick
import "HandDrawn.js" as HandDrawn
// qmllint disable import
import OmaWord.Reader 1.0
// qmllint enable import

Item {
    id: root
    required property TextEdit body
    property var segments: []
    property string sourceText: ""
    property var theme
    property real markScale: theme && theme.textScale > 0 ? theme.textScale : 1
    property bool outlined: false
    onOutlinedChanged: refresh()
    // Poetry: each source line's level and the indent per level, in pixels
    // (SearchDocument.indentLines); empty for prose and search results.
    property var lineIndents: []
    property real indentUnit: 0
    onLineIndentsChanged: leftToRight()
    onIndentUnitChanged: leftToRight()
    property bool destroying: false

    function refresh() { if (!destroying) geometryTimer.restart() }
    function bodyOrigin() {
        // QTextDocument coordinates exclude TextEdit padding and alignment.
        const nativeRect = body.positionToRectangle(0)
        const layoutRect = document.cursorRect(0)
        return Qt.point(nativeRect.x - layoutRect.x, nativeRect.y - layoutRect.y)
    }
    function rectFor(start, length) {
        document.attach(body.textDocument, sourceText)
        const r = document.rectFor(start, length)
        if (r.width <= 0 || r.height <= 0) return Qt.rect(0, 0, 0, 0)
        const origin = bodyOrigin()
        return Qt.rect(r.x + origin.x, r.y + origin.y, r.width, r.height)
    }
    // Character selection: source [start, start + length) → [documentStart,
    // documentEnd] in the body ([] when unmapped), and back (see SearchDocument).
    function documentRange(start, length) {
        document.attach(body.textDocument, sourceText)
        return document.documentRange(start, length)
    }
    // Line boxes (body coordinates) of source [start, start + length).
    function lineRects(start, length) {
        document.attach(body.textDocument, sourceText)
        const origin = bodyOrigin()
        return Array.from(document.rectangles([{ start: start, length: length }])).map(function(r) {
            return Qt.rect(r.x + origin.x, r.y + origin.y, r.width, r.height)
        })
    }
    function sourceAt(position, forward) {
        document.attach(body.textDocument, sourceText)
        return document.sourceAt(position, forward)
    }
    // Scripture paragraphs are always LTR, even when they open with Hebrew.
    function leftToRight() {
        if (destroying || !body) return
        document.leftToRight(body.textDocument)
        document.indentLines(body.textDocument, lineIndents, indentUnit)
    }
    onBodyChanged: { leftToRight(); refresh() }
    onSourceTextChanged: refresh()
    // As a value: an equal list (a row moving beside the chapter read, or
    // back) repaints nothing.
    property string segmentsKey: ""
    onSegmentsChanged: {
        const key = JSON.stringify(segments || [])
        if (key !== segmentsKey) { segmentsKey = key; refresh() }
    }
    onXChanged: refresh()
    onYChanged: refresh()
    onWidthChanged: refresh()
    onHeightChanged: refresh()
    // qmllint disable import
    SearchDocument {
        id: document
        onGeometryChanged: root.refresh()
    }
    // qmllint enable import
    Connections {
        target: root.body
        function onTextChanged() { root.leftToRight(); root.refresh() }
        function onFontChanged() { root.refresh() }
        function onWidthChanged() { root.refresh() }
        function onHeightChanged() { root.refresh() }
        function onContentWidthChanged() { root.refresh() }
        function onContentHeightChanged() { root.refresh() }
        function onXChanged() { root.refresh() }
        function onYChanged() { root.refresh() }
        function onPaddingChanged() { root.refresh() }
        function onHorizontalAlignmentChanged() { root.refresh() }
        function onVerticalAlignmentChanged() { root.refresh() }
    }
    Canvas {
        id: paint
        objectName: "searchHighlightsCanvas"
        anchors.fill: parent
        // Marks painted: a canvas with none to paint or erase paints nothing
        // (most rows, made a chapter at a time, have no matches).
        property bool inked: false
        property color accent: root.theme ? root.theme.accent : "#80aaff" // fallback: a test without a theme
        onAccentChanged: root.refresh()
        onPaint: {
            if (root.destroying) return
            const ctx = getContext("2d")
            ctx.reset() // Also clears the last match when segments becomes empty.
            const origin = root.bodyOrigin()
            const offset = root.body.mapToItem(paint, origin.x, origin.y)
            ctx.translate(offset.x, offset.y)
            const rects = document.rectangles(root.segments || [])
            HandDrawn.searchMarks(ctx, rects, accent, root.outlined,
                root.markScale, root.sourceText)
            inked = rects.length > 0
        }
    }
    Timer {
        id: geometryTimer
        interval: 0
        onTriggered: {
            document.attach(root.body.textDocument, root.sourceText)
            if (paint.inked || (root.segments || []).length) paint.requestPaint()
        }
    }
    Component.onCompleted: { leftToRight(); refresh() }
    Component.onDestruction: { destroying = true; geometryTimer.stop() }
}
