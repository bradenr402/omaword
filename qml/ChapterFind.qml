pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    required property var theme
    property real uiScale: 1
    property bool opened: false
    property int matchIndex: -1
    property int matchCount: 0
    property string error: ""
    signal preview(string query)
    signal accepted(string query)
    signal canceled()
    // A key with nothing to do (tab with the hints hidden): the owner sounds the error.
    signal rejected()
    // Right click (or the menu key) in the field: the app's text menu.
    signal textMenu(Item field, point position)
    // Fade only: closed prompts take no input while fading out.
    property int fadeDuration: 120
    visible: opacity > 0
    enabled: opened
    opacity: opened ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: root.fadeDuration } }
    height: 40 * Math.min(uiScale, 1.4)
    color: theme.background
    border.color: theme.border
    radius: Math.min(theme.cornerRadius, width / 2, height / 2)
    function begin(query) {
        debounce.stop()
        hintFocus = -1
        input.text = query
        opened = true
        input.forceActiveFocus()
        input.selectAll()
    }
    function cancelPending() { debounce.stop() }
    function accept() { debounce.stop(); root.accepted(input.text) }
    // Tab and shift tab cycle the prompt → accept → cancel; the keyboard stays
    // in the field while a hint is marked, and enter or space presses it.
    property int hintFocus: -1
    function cycle(delta) {
        const count = hints.visible ? hints.partItems().length : 0
        if (!count) { root.rejected(); return }
        hintFocus = ((hintFocus + 1 + delta) % (count + 1) + count + 1) % (count + 1) - 1
    }
    function cancel() { debounce.stop(); root.canceled() }
    readonly property string query: input.text
    // A click on the prompt's own chrome (its padding, "/", the counter)
    // stays in the prompt: it neither accepts nor reaches the reader below.
    MouseArea {
        objectName: "chapterFindChrome"
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onPressed: input.forceActiveFocus()
        onWheel: function(wheel) { wheel.accepted = false }
    }
    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12 * root.uiScale; anchors.rightMargin: 12 * root.uiScale
        spacing: 8 * root.uiScale
        Label { text: "/"; color: root.theme.accent; font.pixelSize: 16 * root.uiScale }
        TextField {
            id: input
            objectName: "chapterFindInput"
            ContextMenu.menu: null
            ContextMenu.onRequested: function(position) { root.textMenu(input, position) }
            Layout.fillWidth: true; Layout.minimumWidth: 0
            color: root.theme.foreground
            placeholderText: "Find in chapter · regex, \\c any case"
            horizontalAlignment: TextInput.AlignLeft
            placeholderTextColor: root.theme.muted
            font.family: root.theme.fontFamily; font.pixelSize: 14 * root.uiScale
            selectByMouse: true
            padding: 0
            background: Item {}
            onTextEdited: debounce.restart()
            onAccepted: root.accept()
            Keys.onEscapePressed: root.cancel()
            Keys.onTabPressed: function(event) { root.cycle(event.modifiers & Qt.ShiftModifier ? -1 : 1) }
            Keys.onBacktabPressed: root.cycle(-1)
            // A marked hint takes enter and space; any other key returns to typing.
            Keys.onPressed: function(event) {
                if (root.hintFocus < 0 || [Qt.Key_Shift, Qt.Key_Control, Qt.Key_Alt, Qt.Key_Meta].includes(event.key)) return
                const press = (event.modifiers & ~Qt.KeypadModifier) === Qt.NoModifier
                    && [Qt.Key_Space, Qt.Key_Return, Qt.Key_Enter].includes(event.key)
                const marked = root.hintFocus
                root.hintFocus = -1
                if (press) { event.accepted = true; hints.activatePart(marked) }
            }
            Accessible.name: "Find a regular expression in this chapter (smart case)"
        }
        Label {
            objectName: "chapterFindCount"
            text: root.error || (root.matchCount ? (root.matchIndex < 0
                ? root.matchCount + " matches" : (root.matchIndex + 1) + " / " + root.matchCount) : input.text.length ? "No matches" : "")
            color: root.theme.muted; font.pixelSize: 12 * root.uiScale
            Layout.maximumWidth: root.width * 0.3
            elide: Text.ElideRight
        }
        HintRow {
            id: hints
            objectName: "chapterFindHints"
            theme: root.theme
            parts: [["Return", "accept", function() { root.accept() }], ["Escape", "cancel", function() { root.cancel() }]]
            markedPart: root.hintFocus
            visible: root.width > 600 * root.uiScale
            // Same gap between the counter and the hints as between hint pairs.
            Layout.leftMargin: 16 * root.uiScale
            pixelSize: 11 * root.uiScale
        }
    }
    Timer { id: debounce; interval: 150; onTriggered: root.preview(input.text) }
}
