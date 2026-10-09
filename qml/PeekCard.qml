pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A peek: what the text points to (a footnote) or how other versions read it
// (Compare), read where it is, without leaving the passage or opening a
// panel. It opens at once below its anchor (above when there is no room),
// keeps the keyboard while open, and closes on esc or a click outside, which
// acts on nothing else; the owner plays its exit (menuGhost, toward
// `origin`). Its actions are a HintRow in buttons mode, as on the selection
// bar; other keys go to the owner (keyPressed).
Popup {
    id: card
    required property var theme
    property real uiScale: theme.textScale > 0 ? theme.textScale : 1
    property string title: ""
    property string body: ""
    property var parts: []
    // A list in place of the body (Compare): rows {title, text, muted}, the
    // current one tinted; a click on a row makes it current (rowClicked).
    property var rows: []
    property int currentRow: 0
    property point origin: Qt.point(0, 0)
    signal keyPressed(var event)
    signal rowClicked(int index)

    parent: Overlay.overlay
    modal: true
    dim: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    enter: null
    exit: null
    padding: 0
    width: Math.min((rows.length ? 520 : 420) * uiScale, (parent ? parent.width : 420) - 16)

    // The anchor's line height: the card opens under that line.
    property real anchorLine: 0
    function show(sceneX, sceneY, lineHeight) {
        origin = Qt.point(sceneX, sceneY)
        anchorLine = lineHeight || 0
        open()
        place()
    }
    // Below the anchor's scene point, or above it, kept inside the window;
    // placed again as its size settles (Compare's rows lay out after it opens).
    function place() {
        const area = parent as Item
        const gap = 6 * uiScale
        const below = origin.y + anchorLine + gap
        x = Math.round(Math.max(8, Math.min(origin.x - 12 * uiScale, area.width - width - 8)))
        y = Math.round(below + height <= area.height - 8 ? below : Math.max(8, origin.y - height - gap))
    }
    onWidthChanged: if (visible) place()
    onHeightChanged: if (visible) place()

    background: Rectangle {
        color: card.theme.background
        border.color: card.theme.border
        radius: Math.min(card.theme.cornerRadius || 0, 12 * card.uiScale)
    }
    contentItem: ColumnLayout {
        spacing: 0
        focus: true
        Keys.onPressed: function(event) { card.keyPressed(event) }
        Accessible.role: Accessible.Dialog
        Accessible.name: card.title
        Accessible.description: card.body
        Label {
            objectName: "peekTitle"
            text: card.title
            textFormat: Text.PlainText
            color: card.theme.muted
            font.family: card.theme.fontFamily
            font.pixelSize: 12 * card.uiScale
            elide: Text.ElideRight
            topPadding: 12 * card.uiScale; leftPadding: 14 * card.uiScale; rightPadding: 14 * card.uiScale
            Layout.fillWidth: true
        }
        ListView {
            id: rowList
            objectName: "peekRows"
            visible: card.rows.length > 0
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? Math.min(contentHeight, 0.5 * ((card.parent as Item)?.height ?? 600)) : 0
            Layout.topMargin: visible ? 6 * card.uiScale : 0
            Layout.bottomMargin: visible ? 6 * card.uiScale : 0
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: card.rows
            currentIndex: card.currentRow
            onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Contain)
            delegate: Rectangle {
                id: row
                required property var modelData
                required property int index
                readonly property bool current: index === card.currentRow
                objectName: "peekRow:" + index
                width: rowList.width
                implicitHeight: rowColumn.implicitHeight + 12 * card.uiScale
                color: current ? Qt.rgba(card.theme.accent.r, card.theme.accent.g, card.theme.accent.b, 0.11) : "transparent"
                Column {
                    id: rowColumn
                    x: 14 * card.uiScale
                    y: 6 * card.uiScale
                    width: parent.width - 28 * card.uiScale
                    spacing: 2 * card.uiScale
                    Text {
                        objectName: "peekRowTitle:" + row.index
                        width: parent.width
                        text: row.modelData.title
                        textFormat: Text.PlainText
                        color: row.current ? card.theme.accent : card.theme.muted
                        font.family: card.theme.fontFamily
                        font.pixelSize: 12 * card.uiScale
                        font.weight: Font.Medium
                        elide: Text.ElideRight
                    }
                    Text {
                        objectName: "peekRowText:" + row.index
                        width: parent.width
                        text: row.modelData.text
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        color: row.modelData.muted ? card.theme.muted : card.theme.foreground
                        font.family: card.theme.fontFamily
                        font.pixelSize: 14 * card.uiScale
                        lineHeight: 1.25
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: card.rowClicked(row.index)
                }
            }
        }
        Flickable {
            id: bodyView
            objectName: "peekBodyView"
            visible: card.rows.length === 0
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? Math.min(bodyText.implicitHeight, 0.4 * ((card.parent as Item)?.height ?? 600)) : 0
            contentHeight: bodyText.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            Text {
                id: bodyText
                objectName: "peekBody"
                width: bodyView.width
                text: card.body
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: card.theme.foreground
                font.family: card.theme.fontFamily
                font.pixelSize: 14 * card.uiScale
                lineHeight: 1.25
                topPadding: 6 * card.uiScale; bottomPadding: 12 * card.uiScale
                leftPadding: 14 * card.uiScale; rightPadding: 14 * card.uiScale
            }
        }
        Rectangle { color: card.theme.border; implicitHeight: 1; Layout.fillWidth: true; opacity: 0.65 }
        HintRow {
            objectName: "peekHints"
            theme: card.theme
            parts: card.parts
            pixelSize: 12 * card.uiScale
            partHeight: 30 * card.uiScale
            partPadding: 10 * card.uiScale
            // Its width comes from the card: a Flow sized to its own implicit
            // width keeps any narrower width it was once given.
            Layout.fillWidth: true
            Layout.margins: 1
        }
    }
}
