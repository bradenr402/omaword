import QtQuick
import "HandDrawn.js" as HandDrawn

// A Verse layout row's highlights (the chapter read's, or one shown beside
// it): for each highlight over the verse, a highlighter stroke along every
// line of its text; the segment joins the rows above and below that the
// highlight continues into.
Repeater {
    id: highlights
    // The reader window (Main): its highlighter ink, band, and scale.
    required property var controller
    required property var row
    required property string versionId
    // The row's geometry: its text column's left, its size, its heading.
    required property real gutter
    required property real rowWidth
    required property real rowHeight
    required property real headingHeight
    // The verse's text, and the SearchHighlights over it (its line boxes).
    required property TextEdit body
    required property var lines
    // Names its items apart from the chapter read's ("context:John 3:").
    property string namePrefix: ""
    // A value: rows made again for other marks repaint only when these change.
    readonly property string rangesKey: JSON.stringify(row.highlightRanges || [])
    model: JSON.parse(rangesKey)
    delegate: Rectangle {
        id: segment
        required property var modelData
        objectName: highlights.namePrefix + "highlightSegment" + highlights.row.number + ":" + modelData.reference
        x: highlights.gutter - 9 * highlights.controller.readerScale
        y: modelData.highlightContinuesBefore ? 0 : highlights.headingHeight
        width: highlights.rowWidth - x
        height: (modelData.highlightContinuesAfter ? highlights.rowHeight : highlights.headingHeight + highlights.body.height + 3) - y
        radius: 0
        // Painted as a highlighter band by highlightPaint.
        color: "transparent"
        Canvas {
            id: highlightPaint
            objectName: highlights.namePrefix + "highlightPaint" + highlights.row.number + ":" + segment.modelData.reference
            anchors.fill: parent
            readonly property var range: segment.modelData
            property color ink: highlights.controller.highlightInk
            readonly property var paintKey: [highlights.controller.readerScale, width, height, highlights.body.width, highlights.body.height,
                highlights.body.y, highlights.controller.highlightBand, highlights.controller.highlightAlpha, range.reference, highlights.versionId]
            onPaintKeyChanged: requestPaint()
            onInkChanged: requestPaint()
            Connections { target: highlights.controller; function onHighlightsRepaint() { highlightPaint.requestPaint() } }
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                const k = highlights.controller.readerScale
                const s = HandDrawn.seed("highlight", highlights.versionId, range.reference, highlights.row.number)
                const band = highlights.controller.highlightBand
                const rects = highlights.lines ? highlights.lines.lineRects(0, String(highlights.row.text || "").length) : []
                for (let i = 0; i < rects.length; ++i) {
                    const r = rects[i]
                    const p = highlights.body.mapToItem(highlightPaint, r.x, r.y)
                    HandDrawn.highlighterStroke(ctx, Qt.rect(p.x, p.y + band.top, r.width, band.height),
                        ink, highlights.controller.highlightAlpha, HandDrawn.seed(s, i), k)
                }
            }
        }
    }
}
