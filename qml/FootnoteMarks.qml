import QtQuick
import "TypeScale.js" as TypeScale

// A Verse layout row's footnote marks (the chapter read's, or one shown
// beside it): the caller over each note's reserved cell in the verse's text
// (Main.footnoteHtml), clicked to peek at the note (activated).
Repeater {
    id: marks
    // The reader window (Main) and the live theme.
    required property var controller
    required property var theme
    required property var row
    // The verse's text, its cells reserved.
    required property TextEdit body
    // Names its items apart from the chapter read's ("context:John 3:").
    property string namePrefix: ""
    // A note's mark clicked: where it is in the window (x, y, height).
    signal activated(var note, real x, real y, real height)
    // A value: rows made again for other marks rebuild only when these change.
    readonly property string notesKey: JSON.stringify(row.footnotes || [])
    model: controller.focusLayout ? [] : JSON.parse(notesKey)
    delegate: Item {
        id: footnoteMarker
        required property var modelData
        objectName: marks.namePrefix + "verseFootnoteMark:" + marks.row.number + "." + modelData.seq
        // When the chapter or version changes, a row reads its new
        // verse a moment before its old markers go: their cells
        // may lie past the new text, so they take no place.
        readonly property int at: marks.controller.footnoteCellPosition(marks.row.footnotes, modelData)
        readonly property rect cell: marks.body.width > 0 && marks.body.contentHeight > 0 && at <= marks.body.length
            ? marks.body.positionToRectangle(at) : Qt.rect(0, 0, 0, 0)
        x: marks.body.x + cell.x
        y: marks.body.y + cell.y
        width: marks.controller.footnoteCellWidth
        height: cell.height
        z: 3
        // Out of the tab order: i peeks at the cursor verse's notes.
        activeFocusOnTab: false
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: marks.controller.footnoteMark(footnoteMarker.modelData.caller)
            color: marks.theme.accent
            font.family: marks.theme.fontFamily
            font.pixelSize: Math.max(6, TypeScale.size(marks.controller.readerBodySize, TypeScale.mark))
            textFormat: Text.PlainText
        }
        MouseArea {
            id: footnoteMouse
            objectName: marks.namePrefix + "verseFootnoteHit:" + marks.row.number + "." + footnoteMarker.modelData.seq
            // A taller, invisible target than the small mark (layout unchanged):
            // above and below only, never over the neighboring words.
            anchors.fill: parent
            anchors.topMargin: -marks.controller.markerHitMargin
            anchors.bottomMargin: -marks.controller.markerHitMargin
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                const p = footnoteMarker.mapToItem(null, 0, 0)
                marks.activated(footnoteMarker.modelData, p.x, p.y, footnoteMarker.height)
            }
        }
        Accessible.role: Accessible.Button
        Accessible.name: "Footnote: " + footnoteMarker.modelData.text
        // Made while hovered: a popup per mark is costly to make.
        Loader {
            active: footnoteMouse.containsMouse
            sourceComponent: AppToolTip {
                parent: footnoteMarker
                theme: marks.theme
                uiScale: marks.controller.scale
                visible: true
                text: "Footnote · " + footnoteMarker.modelData.text + "\n" + marks.controller.footnoteHint
            }
        }
    }
}
