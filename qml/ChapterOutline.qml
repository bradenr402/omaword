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
    // { } step between sections from the outline too (the app's heading keys).
    signal stepHeading(int direction)
    property var scrollKey: null
    activeFocusOnTab: true
    // The keyboard's entry: 0 the title, i + 1 heading i. It starts on the
    // current section; ↑ ↓ (j k) move it, enter (space) jumps there.
    property int keyIndex: 0
    readonly property int currentIndex: {
        for (let i = 0; i < headings.length; ++i) if (headings[i].verse === current) return i + 1
        return 0
    }
    onActiveFocusChanged: if (activeFocus) keyIndex = currentIndex
    onCurrentIndexChanged: if (activeFocus) keyIndex = currentIndex
    function moveKey(delta) {
        const next = Math.max(0, Math.min(headings.length, keyIndex + delta))
        if (next === keyIndex) return false
        keyIndex = next
        const entry = next === 0 ? titleEntry : entries.itemAt(next - 1)
        if (entry && entry.y < list.contentY) list.contentY = entry.y
        else if (entry && entry.y + entry.height > list.contentY + list.height) list.contentY = Math.min(list.contentHeight - list.height, entry.y + entry.height - list.height)
        return true
    }
    function activateKey() {
        if (keyIndex === 0) root.activatedTop()
        else if (headings[keyIndex - 1]) root.activated(headings[keyIndex - 1].verse)
    }
    signal boundary()
    Keys.onEscapePressed: root.leave()
    Keys.onPressed: function(event) {
        const plain = (event.modifiers & ~Qt.KeypadModifier) === Qt.NoModifier
        if (plain && [Qt.Key_Up, Qt.Key_Down, Qt.Key_J, Qt.Key_K].includes(event.key)) {
            if (!root.moveKey(event.key === Qt.Key_Down || event.key === Qt.Key_J ? 1 : -1)) root.boundary()
            event.accepted = true
        } else if (plain && [Qt.Key_Return, Qt.Key_Enter, Qt.Key_Space].includes(event.key)) {
            root.activateKey(); event.accepted = true
        } else if (event.text === "{" || event.text === "}") {
            root.stepHeading(event.text === "}" ? 1 : -1); event.accepted = true
        } else if (root.scrollKey) root.scrollKey(list, event)
    }
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
                    objectName: "outlineTitleKey"
                    anchors.fill: parent
                    visible: root.activeFocus && root.keyIndex === 0
                    color: Qt.rgba(root.theme.accent.r, root.theme.accent.g, root.theme.accent.b, 0.11)
                    radius: Math.min(root.theme.cornerRadius || 0, height / 2)
                }
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
                    required property int index
                    readonly property int verse: modelData.verse
                    readonly property bool isCurrent: root.current === verse
                    objectName: "outlineEntry:" + verse
                    width: column.width
                    height: label.implicitHeight + 10 * root.uiScale
                    Accessible.role: Accessible.Button
                    Accessible.name: "Section " + modelData.text + (isCurrent ? ", current" : "")
                    Accessible.onPressAction: root.activated(entry.verse)
                    Rectangle {
                        objectName: "outlineEntryKey:" + entry.verse
                        anchors.fill: parent
                        visible: root.activeFocus && root.keyIndex === entry.index + 1
                        color: Qt.rgba(root.theme.accent.r, root.theme.accent.g, root.theme.accent.b, 0.11)
                        radius: Math.min(root.theme.cornerRadius || 0, height / 2)
                    }
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
