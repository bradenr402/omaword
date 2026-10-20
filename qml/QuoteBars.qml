import QtQuick
import OmaWord.Reader 1.0

// A Markdown preview's layout (TextEdit.MarkdownText, MarkdownQuotes): one
// line of the text's height between paragraphs, lists, and quotes, as a blank
// line reads; lists indented 1.6 em a level, room for a bullet or "10." (Qt's
// own 40 px reads as a deep step); and for block quotes a small indent per level
// and a muted bar beside each, the way quotes are usually drawn; links in
// the theme's `linkColor`. A child of the TextEdit, so the bars scroll with
// its text.
Item {
    id: root
    required property TextEdit target
    required property color color
    required property color linkColor
    property real uiScale: 1
    anchors.fill: parent
    FontMetrics { id: line; font: root.target.font }
    MarkdownQuotes {
        id: quotes
        document: root.target.textDocument
        indent: Math.round(14 * root.uiScale)
        listIndent: Math.round(1.6 * root.target.font.pixelSize)
        gap: Math.round(line.lineSpacing)
        linkColor: root.linkColor
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
