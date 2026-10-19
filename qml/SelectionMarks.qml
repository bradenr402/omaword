import QtQuick
import QtQuick.Shapes

// A selection's marks under its text (paint only): a faint wash behind each
// line box, where `fill` is not transparent, and dotted underlines below. A
// range of verses and text selected by character are marked so; the focused
// verse (or character) has its own, stronger tint. `lines` are line boxes
// [{x, y, width, height}] in this item's coordinates; the dots' top lies
// `dotTop` below a box's top. Dots and gaps are `dot` pixels, on whole
// pixels so they stay crisp.
Item {
    id: marks
    required property var lines
    required property color ink
    property color fill: "transparent"
    // The wash reaches this far left of the text (a range's, as the focused verse's tint).
    property real washInset: 0
    property real dot: 2
    property real dotTop: 0
    readonly property int count: lines.length
    Repeater {
        model: marks.fill.a > 0 ? marks.lines : []
        delegate: Rectangle {
            required property var modelData
            x: modelData.x - marks.washInset; y: modelData.y
            width: modelData.width + marks.washInset; height: modelData.height
            color: marks.fill
        }
    }
    Shape {
        ShapePath {
            strokeColor: marks.ink
            strokeWidth: marks.dot
            strokeStyle: ShapePath.DashLine
            dashPattern: [1, 1]
            capStyle: ShapePath.FlatCap
            fillColor: "transparent"
            PathMultiline {
                paths: Array.from(marks.lines).map(function(line) {
                    const x = Math.round(line.x), y = Math.round(line.y + marks.dotTop) + marks.dot / 2
                    return [Qt.point(x, y), Qt.point(x + Math.max(0, Math.round(line.width)), y)]
                })
            }
        }
    }
}
