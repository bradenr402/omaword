import QtQuick

// Chapter outline: the chapter's section headings in the left margin. It
// stays put while the reader scrolls, highlights the current section, and
// scrolls itself to keep that heading visible when it is taller than its
// space. Clicking a heading asks the reader to jump there.
Item {
    id: root
    required property var headings // [{verse, text}]
    required property var theme
    property int current: 0
    property real uiScale: 1
    signal activated(int verse)

    function reveal() {
        for (let i = 0; i < entries.count; ++i) {
            const entry = entries.itemAt(i)
            if (!entry || !root.headings[i] || root.headings[i].verse !== current) continue
            if (entry.y < list.contentY || entry.y + entry.height > list.contentY + list.height)
                list.contentY = Math.max(0, Math.min(list.contentHeight - list.height, entry.y - (list.height - entry.height) / 2))
            return
        }
    }
    onCurrentChanged: Qt.callLater(root.reveal)
    onHeightChanged: Qt.callLater(root.reveal)

    Flickable {
        id: list
        objectName: "outlineList"
        anchors.fill: parent
        clip: true
        contentWidth: width
        contentHeight: column.height
        interactive: contentHeight > height
        boundsBehavior: Flickable.StopAtBounds
        Column {
            id: column
            width: list.width
            Repeater {
                id: entries
                model: root.headings
                delegate: Item {
                    id: entry
                    required property var modelData
                    readonly property int verse: modelData.verse
                    readonly property bool isCurrent: root.current === verse
                    objectName: "outlineEntry:" + verse
                    width: column.width
                    height: label.implicitHeight + 10 * root.uiScale
                    Accessible.role: Accessible.Button
                    Accessible.name: "Section " + modelData.text + (isCurrent ? ", current" : "")
                    Accessible.onPressAction: root.activated(entry.verse)
                    Rectangle {
                        objectName: "outlineCurrentMark"
                        visible: entry.isCurrent
                        x: 0
                        y: 5 * root.uiScale
                        width: 2 * root.uiScale
                        height: label.implicitHeight
                        color: root.theme.accent
                    }
                    Text {
                        id: label
                        x: 10 * root.uiScale
                        y: 5 * root.uiScale
                        width: parent.width - x
                        text: entry.modelData.text
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        font.family: root.theme.fontFamily
                        font.pixelSize: 12.5 * root.uiScale
                        font.weight: entry.isCurrent ? Font.DemiBold : Font.Normal
                        color: entry.isCurrent || mouse.containsMouse ? root.theme.foreground : root.theme.muted
                    }
                    MouseArea {
                        id: mouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.activated(entry.verse)
                    }
                }
            }
        }
    }
}
