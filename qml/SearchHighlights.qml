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
    property bool outlined: false
    onOutlinedChanged: refresh()
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
    onBodyChanged: refresh()
    onSourceTextChanged: refresh()
    onSegmentsChanged: refresh()
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
        function onTextChanged() { root.refresh() }
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
        property color accent: root.theme ? root.theme.accent : "#80aaff"
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
                root.theme && root.theme.textScale > 0 ? root.theme.textScale : 1, root.sourceText)
        }
    }
    Timer {
        id: geometryTimer
        interval: 0
        onTriggered: {
            document.attach(root.body.textDocument, root.sourceText)
            paint.requestPaint()
        }
    }
    Component.onCompleted: refresh()
    Component.onDestruction: { destroying = true; geometryTimer.stop() }
}
