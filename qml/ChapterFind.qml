pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "KeyNotation.js" as KeyNotation

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
        input.text = query
        opened = true
        input.forceActiveFocus()
        input.selectAll()
    }
    function cancelPending() { debounce.stop() }
    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12 * root.uiScale; anchors.rightMargin: 12 * root.uiScale
        spacing: 8 * root.uiScale
        Label { text: "/"; color: root.theme.accent; font.pixelSize: 16 * root.uiScale }
        TextField {
            id: input
            objectName: "chapterFindInput"
            Layout.fillWidth: true; Layout.minimumWidth: 0
            color: root.theme.foreground
            placeholderText: "Find in chapter"
            horizontalAlignment: TextInput.AlignLeft
            placeholderTextColor: root.theme.muted
            font.family: root.theme.fontFamily; font.pixelSize: 14 * root.uiScale
            selectByMouse: true
            padding: 0
            background: Item {}
            onTextEdited: debounce.restart()
            onAccepted: { debounce.stop(); root.accepted(text) }
            Keys.onEscapePressed: { debounce.stop(); root.canceled() }
            Accessible.name: "Find literal text in this chapter"
        }
        Label {
            objectName: "chapterFindCount"
            text: root.error || (root.matchCount ? (root.matchIndex < 0
                ? root.matchCount + " matches" : (root.matchIndex + 1) + " / " + root.matchCount) : input.text.length ? "No matches" : "")
            color: root.theme.muted; font.pixelSize: 12 * root.uiScale
            Layout.maximumWidth: root.width * 0.3
            elide: Text.ElideRight
        }
        Label {
            objectName: "chapterFindHints"
            text: KeyNotation.hint([["Return", "accept"], ["Escape", "cancel"]], root.theme.foreground, root.theme.muted)
            textFormat: Text.StyledText
            visible: root.width > 600 * root.uiScale
            // Same gap between the counter and the hints as between hint pairs.
            Layout.leftMargin: 16 * root.uiScale
            color: root.theme.muted; font.pixelSize: 11 * root.uiScale
        }
    }
    Timer { id: debounce; interval: 150; onTriggered: root.preview(input.text) }
}
