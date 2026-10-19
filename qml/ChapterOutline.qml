import QtQuick

// Chapter outline: the book and chapter over its section headings in the left
// margin (in the whole book, the book over its chapters, each with its
// headings under it). It stays put while the reader scrolls, highlights the
// current section (the title before the first heading), and scrolls itself to
// keep that entry visible when it is taller than its space. Clicking an entry
// asks the reader to jump there; clicking the title, to the top.
Item {
    id: root
    // [{verse, text}]; in the whole book [{key, chapter, verse, text, level}]:
    // a chapter (level 0, verse 0) and its headings (level 1).
    required property var headings
    required property var theme
    property string title: "" // e.g. "John 3", or the book's name
    // The current entry's key (an entry's key, else its verse); 0: the title.
    property var current: 0
    property real uiScale: 1
    // How long the list glides to an entry it brings into view (0: at once).
    property int slideMs: 0
    // Tooltips naming the keyboard equivalents (the app fills in its keys).
    property string titleTip: ""
    property string entryTip: ""
    signal activated(var entry)
    signal activatedTop()
    function entryKey(entry) { return entry.key !== undefined ? entry.key : entry.verse }
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
        for (let i = 0; i < headings.length; ++i) if (entryKey(headings[i]) === current) return i + 1
        return 0
    }
    onActiveFocusChanged: if (activeFocus) keyIndex = currentIndex
    onCurrentIndexChanged: if (activeFocus) keyIndex = currentIndex
    function moveKey(delta) {
        const next = Math.max(0, Math.min(headings.length, keyIndex + delta))
        if (next === keyIndex) return false
        keyIndex = next
        listSlide.stop()
        const entry = next === 0 ? titleEntry : entries.itemAt(next - 1)
        if (entry && entry.y < list.contentY) list.contentY = entry.y
        else if (entry && entry.y + entry.height > list.contentY + list.height) list.contentY = Math.min(list.contentHeight - list.height, entry.y + entry.height - list.height)
        return true
    }
    function activateKey() {
        if (keyIndex === 0) root.activatedTop()
        else if (headings[keyIndex - 1]) root.activated(headings[keyIndex - 1])
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

    // The entry last brought into view: a current entry that only flickered
    // (the reader placing a chapter it loaded above) scrolls nothing.
    property var revealed: null
    function revealCurrent() { if (current !== revealed) reveal() }
    function reveal() {
        revealed = current
        if (current === 0) { scrollList(0); return }
        for (let i = 0; i < entries.count; ++i) {
            const entry = entries.itemAt(i)
            if (!entry || !root.headings[i] || root.entryKey(root.headings[i]) !== current) continue
            const top = listSlide.running ? listSlide.to : list.contentY
            if (entry.y < top || entry.y + entry.height > top + list.height)
                scrollList(Math.max(0, Math.min(list.contentHeight - list.height, entry.y - (list.height - entry.height) / 2)))
            return
        }
    }
    function scrollList(y) {
        listSlide.stop()
        if (!slideMs || !visible || Math.abs(y - list.contentY) < 1) { list.contentY = y; return }
        listSlide.to = y
        listSlide.start()
    }
    NumberAnimation { id: listSlide; objectName: "outlineSlide"; target: list; property: "contentY"; duration: root.slideMs; easing.type: Easing.OutCubic }
    onCurrentChanged: Qt.callLater(root.revealCurrent)
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
        onMovementStarted: listSlide.stop()
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
                    readonly property var key: root.entryKey(modelData)
                    readonly property bool isCurrent: root.current === key
                    // A chapter in the whole book's outline (its headings under it).
                    readonly property bool chapter: modelData.level === 0
                    objectName: "outlineEntry:" + key
                    width: column.width
                    height: label.implicitHeight + 10 * root.uiScale
                    Accessible.role: Accessible.Button
                    Accessible.name: (chapter ? "" : "Section ") + modelData.text + (isCurrent ? ", current" : "")
                    Accessible.onPressAction: root.activated(entry.modelData)
                    Rectangle {
                        objectName: "outlineEntryKey:" + entry.key
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
                        objectName: "outlineEntryText:" + entry.key
                        x: (entry.modelData.level === 1 ? 22 : 10) * root.uiScale
                        y: 5 * root.uiScale
                        width: parent.width - x
                        // An LRM keeps a Hebrew-first heading ("א Aleph") in an
                        // LTR paragraph, as the reader shows it; display only.
                        text: entry.modelData.text.split("\n").map(function(line) { return "\u200E" + line }).join("\n")
                        horizontalAlignment: Text.AlignLeft
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        font.family: root.theme.fontFamily
                        font.pixelSize: (entry.chapter ? 13.5 : 12.5) * root.uiScale
                        font.weight: entry.isCurrent || entry.chapter ? Font.DemiBold : Font.Normal
                        color: entry.isCurrent || mouse.containsMouse ? root.theme.foreground : root.theme.muted
                    }
                    MouseArea {
                        id: mouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.activated(entry.modelData)
                    }
                    // Made while hovered: a tooltip per entry is costly to make,
                    // and a chapter's entries are made again with each chapter.
                    Loader {
                        active: mouse.containsMouse && root.entryTip.length > 0
                        sourceComponent: AppToolTip {
                            parent: entry
                            theme: root.theme
                            uiScale: root.uiScale
                            visible: true
                            text: root.entryTip
                        }
                    }
                }
            }
        }
    }
}
