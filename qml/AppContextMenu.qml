pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

// One themed context menu for every target. Entries are plain objects:
// { id, text, shortcut, enabled, run } or { separator: true } between groups.
// Actions run after the menu closes and focus returns to where it was.
Popup {
    id: menu
    required property var theme
    property real uiScale: theme.textScale > 0 ? theme.textScale : 1
    property string target: ""
    property var entries: []
    property int currentIndex: -1
    property Item restoreFocusItem: null
    property var pendingRun: null
    // Data-only view of the entries for tests and accessibility.
    readonly property var layout: entries.map(function(entry) {
        return entry.separator ? { separator: true }
            : { id: entry.id, text: entry.text, shortcut: entry.shortcut || "", enabled: entry.enabled !== false }
    })
    signal triggered(string id)
    // A click on a disabled row: the owner sounds the error.
    signal rejected()

    parent: Overlay.overlay
    // Above drawers and dialogs that may be declared later in the window.
    z: 1000
    // Not modal: the owner's outside catcher (`pressedOutside`) decides what a
    // press outside the menu does. It must not be declared inside this popup:
    // an overlay child owned by a popup makes Qt treat the closed menu as the
    // top popup and block every window shortcut.
    modal: false
    dim: false
    focus: true
    padding: 4 * uiScale
    closePolicy: Popup.CloseOnEscape
    // Outside the menu a left (or middle) press only closes it, acting on
    // nothing underneath. A right press closes it and falls through (returns
    // false), so the target under the pointer opens its own menu there.
    function pressedOutside(button) {
        close()
        return button !== Qt.RightButton
    }
    // No motion, ever: the menu appears and disappears at once, so actions and
    // focus restoration never wait on animation.
    enter: null
    exit: null
    width: Math.ceil(rowWidth + leftPadding + rightPadding)
    readonly property real rowWidth: {
        let widest = 180 * uiScale
        for (const entry of entries) {
            if (entry.separator) continue
            widest = Math.max(widest, metrics.advanceWidth(entry.text) + metrics.advanceWidth(entry.shortcut || "") + 52 * uiScale)
        }
        return widest
    }
    FontMetrics { id: metrics; font.family: menu.theme.fontFamily; font.pixelSize: 13 * menu.uiScale }

    function enabledAt(index) {
        const entry = entries[index]
        return !!entry && !entry.separator && entry.enabled !== false
    }
    // Opens at a scene point, kept inside the window.
    function show(kind, items, sceneX, sceneY) {
        target = kind
        entries = items
        restoreFocusItem = (menu.parent as Item)?.Window.activeFocusItem ?? null
        pendingRun = null
        currentIndex = -1
        for (let i = 0; i < entries.length; ++i) if (enabledAt(i)) { currentIndex = i; break }
        const area = parent as Item
        const full = entries.reduce(function(sum, entry) { return sum + rowHeight(entry) }, topPadding + bottomPadding)
        // Taller than the window: the menu scrolls inside it.
        const height = area ? Math.min(full, area.height) : full
        x = Math.max(0, Math.min(sceneX, (area ? area.width : sceneX + width) - width))
        y = Math.max(0, Math.min(sceneY, (area ? area.height : sceneY + height) - height))
        open()
        scroller.contentY = 0
        revealCurrent()
    }
    function rowHeight(entry) { return (entry && entry.separator ? 9 : 30) * uiScale }
    // Keeps the current row inside the visible part of a scrolling menu.
    function revealCurrent() {
        if (currentIndex < 0) return
        let top = 0
        for (let i = 0; i < currentIndex; ++i) top += rowHeight(entries[i])
        const bottom = top + rowHeight(entries[currentIndex])
        if (top < scroller.contentY) scroller.contentY = top
        else if (bottom > scroller.contentY + scroller.height) scroller.contentY = bottom - scroller.height
    }
    onCurrentIndexChanged: revealCurrent()
    height: Math.min(implicitHeight, (parent as Item) ? (parent as Item).height : implicitHeight)
    function move(delta) {
        if (!entries.length) return
        let index = currentIndex
        for (let step = 0; step < entries.length; ++step) {
            index = (index + delta + entries.length) % entries.length
            if (enabledAt(index)) { currentIndex = index; return }
        }
    }
    function trigger(index) {
        if (!enabledAt(index)) return
        pendingRun = entries[index]
        close()
    }
    function triggerId(id) {
        trigger(entries.findIndex(function(entry) { return entry.id === id }))
    }
    onClosed: {
        const entry = pendingRun
        pendingRun = null
        if (restoreFocusItem && restoreFocusItem.visible) restoreFocusItem.forceActiveFocus()
        restoreFocusItem = null
        if (entry) {
            triggered(entry.id)
            if (entry.run) entry.run()
        }
    }

    background: Rectangle {
        objectName: "contextMenuBackground"
        color: menu.theme.surface
        border.color: menu.theme.border
        border.width: 1
        radius: Math.min(menu.theme.cornerRadius || 0, width / 2, height / 2)
    }
    contentItem: Flickable {
        id: scroller
        objectName: "contextMenuContent"
        focus: true
        clip: true
        implicitWidth: column.width
        implicitHeight: column.height
        contentWidth: column.width
        contentHeight: column.height
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.VerticalFlick
        interactive: contentHeight > height
        ScrollBar.vertical: ScrollBar {
            id: menuBar
            objectName: "contextMenuScrollbar"
            // The app's overlay scrollbar: a thin thumb over the rows' right edge.
            implicitWidth: 18 * menu.uiScale
            policy: ScrollBar.AlwaysOn
            visible: size < 1
            hoverEnabled: true
            padding: 0
            contentItem: Item {
                Rectangle {
                    anchors.right: parent.right
                    anchors.rightMargin: 4 * menu.uiScale
                    height: parent.height
                    width: menuBar.hovered || menuBar.pressed ? 10 * menu.uiScale : 4 * menu.uiScale
                    color: menu.theme.muted
                    radius: Math.min(menu.theme.cornerRadius || 0, width / 2, height / 2)
                    opacity: 0.6
                }
            }
            background: Item {}
        }
        Keys.onPressed: function(event) {
            const plain = (event.modifiers & ~Qt.KeypadModifier) === Qt.NoModifier
            if (!plain && !(event.key === Qt.Key_F10 && event.modifiers === Qt.ShiftModifier)
                    && event.key !== Qt.Key_Backtab) { event.accepted = true; return }
            if (event.key === Qt.Key_Up || event.key === Qt.Key_Backtab) menu.move(-1)
            else if (event.key === Qt.Key_Down || event.key === Qt.Key_Tab) menu.move(1)
            else if (event.key === Qt.Key_Home) { menu.currentIndex = -1; menu.move(1) }
            else if (event.key === Qt.Key_End) { menu.currentIndex = menu.entries.length; menu.move(-1) }
            else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) menu.trigger(menu.currentIndex)
            else if (event.key === Qt.Key_Escape || event.key === Qt.Key_Menu || (event.key === Qt.Key_F10 && event.modifiers & Qt.ShiftModifier)) menu.close()
            // Swallow everything else so reader shortcuts never act behind the menu.
            event.accepted = true
        }
        Column {
            id: column
            objectName: "contextMenuColumn"
            Repeater {
                model: menu.entries.length
                delegate: Item {
                    id: row
                    required property int index
                    readonly property var entry: menu.entries[index] || ({})
                    readonly property bool separator: !!entry.separator
                    readonly property bool active: menu.enabledAt(index)
                    objectName: separator ? "contextMenuDivider" + index : "contextMenuItem:" + entry.id
                    width: menu.rowWidth
                    height: separator ? 9 * menu.uiScale : 30 * menu.uiScale
                    Accessible.role: separator ? Accessible.Separator : Accessible.MenuItem
                    Accessible.name: separator ? "" : entry.text
                    Rectangle {
                        objectName: "contextMenuRule" + row.index
                        visible: row.separator
                        anchors.verticalCenter: parent.verticalCenter
                        x: 6 * menu.uiScale
                        width: parent.width - 12 * menu.uiScale
                        height: 1
                        color: menu.theme.border
                    }
                    Rectangle {
                        objectName: "contextMenuHighlight" + row.index
                        visible: !row.separator && row.active && menu.currentIndex === row.index
                        anchors.fill: parent
                        color: Qt.rgba(menu.theme.accent.r, menu.theme.accent.g, menu.theme.accent.b, 0.16)
                        radius: Math.min(menu.theme.cornerRadius || 0, width / 2, height / 2)
                    }
                    Text {
                        objectName: "contextMenuLabel:" + row.entry.id
                        visible: !row.separator
                        anchors.left: parent.left; anchors.leftMargin: 10 * menu.uiScale
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.entry.text || ""
                        textFormat: Text.PlainText
                        font: metrics.font
                        color: row.active ? menu.theme.foreground : menu.theme.muted
                    }
                    Text {
                        objectName: "contextMenuShortcut:" + row.entry.id
                        visible: !row.separator && text.length > 0
                        anchors.right: parent.right; anchors.rightMargin: 10 * menu.uiScale
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.entry.shortcut || ""
                        textFormat: Text.PlainText
                        horizontalAlignment: Text.AlignRight
                        font.family: menu.theme.fontFamily
                        font.pixelSize: 12 * menu.uiScale
                        color: menu.theme.muted
                    }
                    MouseArea {
                        anchors.fill: parent
                        enabled: !row.separator
                        hoverEnabled: true
                        cursorShape: row.active ? Qt.PointingHandCursor : Qt.ArrowCursor
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        // Follow real pointer motion, not a cursor resting where the menu appears.
                        onPositionChanged: if (row.active) menu.currentIndex = row.index
                        onClicked: {
                            if (row.active) menu.trigger(row.index)
                            else menu.rejected()
                        }
                    }
                }
            }
        }
    }
}
