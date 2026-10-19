import QtQuick
import QtQuick.Shapes

// Dotted underlines (paint only): under a range of selected verses, where
// the focused verse alone keeps a tint, and under text selected by
// character. `lines` are [{x, y, width}] in this item's coordinates, y the
// dots' top; dots and gaps are `dot` pixels, on whole pixels so they stay crisp.
Shape {
    id: underline
    required property var lines
    required property color ink
    property real dot: 2
    readonly property int count: lines.length
    ShapePath {
        strokeColor: underline.ink
        strokeWidth: underline.dot
        strokeStyle: ShapePath.DashLine
        dashPattern: [1, 1]
        capStyle: ShapePath.FlatCap
        fillColor: "transparent"
        PathMultiline {
            paths: Array.from(underline.lines).map(function(line) {
                const x = Math.round(line.x), y = Math.round(line.y) + underline.dot / 2
                return [Qt.point(x, y), Qt.point(x + Math.max(0, Math.round(line.width)), y)]
            })
        }
    }
}
