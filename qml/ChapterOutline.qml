import QtQuick

// Chapter outline: the book and chapter over its section headings in the left
// margin. It stays put while the reader scrolls, highlights the current
// section (the title before the first heading), and scrolls itself to keep
// that entry visible when it is taller than its space. Clicking a heading asks
// the reader to jump there; clicking the title, to the top of the chapter.
Item {
    id: root
    required property var headings // [{verse, text}]
    required property var theme
    property string title: "" // e.g. "John 3"
    property int current: 0 // a heading's verse; 0 before the first heading
    property real uiScale: 1
    // Tooltips naming the keyboard equivalents (the app fills in its keys).
    property string titleTip: ""
    property string entryTip: ""
    signal activated(int verse)
    signal activatedTop()
    // Reached by tab from the reader: scroll keys move the outline; esc
    // (or tab onward) leaves it. `scrollKey(flickable, event)` is the app's.
    signal leave()
    property var scrollKey: null
    activeFocusOnTab: true
    Keys.onEscapePressed: root.leave()
    Keys.onPressed: function(event) { if (root.scrollKey) root.scrollKey(list, event) }
    Rectangle {
        objectName: "outlineFocus"
        anchors.fill: parent
        anchors.margins: -4 * root.uiScale
        visible: root.activeFocus
        color: "transparent"
        border.width: 1
        border.color: root.theme.accent
        radius: Math.min(root.theme.cornerRadius || 0, 8 * root.uiScale)
    }

    function reveal() {
        if (current === 0) { list.contentY = 0; return }
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
            Item {
                id: titleEntry
                readonly property bool isCurrent: root.current === 0
                objectName: "outlineTitle"
                width: column.width
                height: titleLabel.implicitHeight + 14 * root.uiScale
                Accessible.role: Accessible.Button
                Accessible.name: root.title + (isCurrent ? ", current" : "")
                Accessible.onPressAction: root.activatedTop()
                Rectangle {
                    objectName: "outlineTitleMark"
                    visible: titleEntry.isCurrent
                    x: 0
                    y: 5 * root.uiScale
                    width: 2 * root.uiScale
                    height: titleLabel.implicitHeight
                    color: root.theme.accent
                }
                Text {
                    id: titleLabel
                    objectName: "outlineTitleText"
                    x: 10 * root.uiScale
                    y: 5 * root.uiScale
                    width: parent.width - x
                    text: root.title
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    font.family: root.theme.fontFamily
                    font.pixelSize: 15 * root.uiScale
                    font.weight: Font.Bold
                    color: titleEntry.isCurrent || titleMouse.containsMouse ? root.theme.foreground : root.theme.muted
                }
                MouseArea {
                    id: titleMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.activatedTop()
                }
                AppToolTip {
                    theme: root.theme
                    uiScale: root.uiScale
                    visible: titleMouse.containsMouse && root.titleTip.length > 0
                    text: root.titleTip
                }
            }
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
                    AppToolTip {
                        theme: root.theme
                        uiScale: root.uiScale
                        visible: mouse.containsMouse && root.entryTip.length > 0
                        text: root.entryTip
                    }
                }
            }
        }
    }
}
