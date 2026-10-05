pragma ComponentBehavior: Bound

import QtQuick
import "KeyNotation.js" as KeyNotation

// A row of key hints ("enter open    esc close"), shared by every panel and
// modal. Parts are [sequences, label, run, enabled]: a part with `run` is
// clickable while it is enabled (the action works with the current focus),
// showing a pointing hand and a light tint on hover. Navigation parts (↑ ↓,
// j k, ← →, tabs, choose) carry no `run` and stay display only. Unhovered,
// the row looks exactly like the styled-text hint it replaced; `text` keeps
// that markup. With `focusableParts`, clickable parts also join the tab
// chain, and enter or space activates the focused one.
Flow {
    id: row
    required property var theme
    property var parts: []
    property real pixelSize: 11
    property bool focusableParts: false
    readonly property var entries: KeyNotation.hintParts(parts)
    readonly property string text: KeyNotation.hint(parts, theme.foreground, theme.muted)
    readonly property string plainText: KeyNotation.hintPlain(parts)
    // The focusable parts, in order (tab cycling in the chapter find prompt).
    function partItems() {
        const items = []
        for (let i = 0; i < repeater.count; ++i) {
            const part = repeater.itemAt(i) as HintPart
            if (part && part.clickable) items.push(part)
        }
        return items
    }
    // The same gap the plain hint used between parts: three no-break spaces and a space.
    spacing: gap.advanceWidth
    TextMetrics { id: gap; font.family: row.theme.fontFamily; font.pixelSize: row.pixelSize; text: "\u00a0\u00a0\u00a0 " }
    component HintPart: Item {
        required property var modelData
        readonly property bool clickable: typeof modelData.run === "function" && modelData.enabled
    }
    Repeater {
        id: repeater
        model: row.entries
        delegate: HintPart {
            id: part
            required property int index
            readonly property bool hovered: mouse.containsMouse
            function activate() { if (clickable) modelData.run() }
            objectName: row.objectName.length ? row.objectName + "Part:" + modelData.label : ""
            implicitWidth: label.implicitWidth
            implicitHeight: label.implicitHeight
            width: Math.min(implicitWidth, Math.max(1, row.width))
            height: label.height
            activeFocusOnTab: row.focusableParts && clickable
            Keys.onShortcutOverride: function(event) {
                if ((event.modifiers & ~Qt.KeypadModifier) === Qt.NoModifier && [Qt.Key_Space, Qt.Key_Return, Qt.Key_Enter].includes(event.key)) event.accepted = true
            }
            Keys.onPressed: function(event) {
                if ((event.modifiers & ~Qt.KeypadModifier) !== Qt.NoModifier) return
                if (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                    event.accepted = true
                    part.activate()
                }
            }
            Rectangle {
                objectName: part.objectName.length ? part.objectName + ":tint" : ""
                anchors.fill: parent
                anchors.leftMargin: -3
                anchors.rightMargin: -3
                z: -1
                visible: part.clickable && (part.hovered || part.activeFocus)
                color: Qt.rgba(row.theme.accent.r, row.theme.accent.g, row.theme.accent.b, part.activeFocus && !part.hovered ? 0.06 : 0.12)
                border.width: part.activeFocus ? 1 : 0
                border.color: row.theme.accent
                radius: Math.min(row.theme.cornerRadius || 0, height / 2)
            }
            Text {
                id: label
                width: part.width
                text: KeyNotation.hint([[part.modelData.sequences, part.modelData.label]], row.theme.foreground, row.theme.muted)
                textFormat: Text.StyledText
                wrapMode: Text.Wrap
                color: row.theme.muted
                font.family: row.theme.fontFamily
                font.pixelSize: row.pixelSize
            }
            MouseArea {
                id: mouse
                anchors.fill: parent
                enabled: part.clickable
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: part.activate()
            }
            Accessible.role: part.clickable ? Accessible.Button : Accessible.StaticText
            Accessible.name: part.modelData.keys + " " + part.modelData.label
            Accessible.onPressAction: part.activate()
        }
    }
}
