import QtQuick
import OmaWord.Reader 1.0

// A Markdown preview's layout (TextEdit.MarkdownText, MarkdownQuotes): one
// line of the text's height between paragraphs, lists, and quotes, as a blank
// line reads, and for block quotes a small indent per level and a muted bar
// beside each, the way quotes are usually drawn. A child of the TextEdit, so
// the bars scroll with its text.
Item {
    id: root
    required property TextEdit target
    required property color color
    property real uiScale: 1
    anchors.fill: parent
    FontMetrics { id: line; font: root.target.font }
    MarkdownQuotes {
        id: quotes
        document: root.target.textDocument
        indent: Math.round(14 * root.uiScale)
        gap: Math.round(line.lineSpacing)
    }
    Repeater {
        model: quotes.bars
        delegate: Rectangle {
            required property var modelData
            required property int index
            objectName: root.objectName + ":" + index
            x: root.target.leftPadding + modelData.x
            y: root.target.topPadding + modelData.y
            width: Math.max(2, Math.round(3 * root.uiScale))
            height: modelData.height
            radius: width / 2
            color: root.color
        }
    }
}
