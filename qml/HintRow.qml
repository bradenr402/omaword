pragma ComponentBehavior: Bound

import QtQuick
import "KeyNotation.js" as KeyNotation

// A row of key hints ("enter open    esc close"), shared by every panel and
// modal. Parts are [sequences, label, run, enabled]: a part with `run` is
// clickable while it is enabled (the action works with the current focus),
// showing a pointing hand and a light tint on hover. Navigation parts (↑ ↓,
// j k, ← →, tabs, choose) carry no `run` and stay display only. Unhovered,
// the row looks exactly like the styled-text hint it replaced; `text` keeps
// that markup. An owner that cycles through its parts by key (the chapter
// find prompt) marks the current one with `markedPart` (an index among the
// clickable parts) and runs it with `activatePart`.
Flow {
    id: row
    required property var theme
    property var parts: []
    property real pixelSize: 11
    property int markedPart: -1
    // A toolbar (the selection bar): with a partHeight, every part is a
    // button that tall, its hint centred between partPadding on each side,
    // edge to edge with the next, its hover and press tint filling it.
    property real partHeight: 0
    property real partPadding: 0
    readonly property bool buttons: partHeight > 0
    readonly property var entries: KeyNotation.hintParts(parts)
    readonly property string text: KeyNotation.hint(parts, theme.foreground, theme.muted)
    readonly property string plainText: KeyNotation.hintPlain(parts)
    // The muted ink of the labels (keys are in the foreground), as on the plain hint.
    readonly property color color: theme.muted
    function activatePart(index) {
        const part = partItems()[index]
        if (part) part.activate()
    }
    // The clickable parts, in order.
    function partItems() {
        const items = []
        for (let i = 0; i < repeater.count; ++i) {
            const part = repeater.itemAt(i) as HintPart
            if (part && part.clickable) items.push(part)
        }
        return items
    }
    // The same gap the plain hint used between parts (three no-break spaces and
    // a space) trails each part but the last. Flow spacing would also open the
    // same gap between wrapped lines.
    readonly property real gapWidth: gap.advanceWidth
    spacing: 0
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
            readonly property real trailing: !row.buttons && index < repeater.count - 1 ? row.gapWidth : 0
            readonly property real padding: row.buttons ? row.partPadding : 0
            implicitWidth: label.implicitWidth + trailing + 2 * padding
            implicitHeight: row.buttons ? row.partHeight : label.implicitHeight
            width: Math.min(implicitWidth, Math.max(1, row.width))
            height: row.buttons ? row.partHeight : label.height
            readonly property bool marked: row.markedPart >= 0 && row.partItems().indexOf(part) === row.markedPart
            Rectangle {
                objectName: part.objectName.length ? part.objectName + ":tint" : ""
                anchors.fill: row.buttons ? part : label
                anchors.leftMargin: row.buttons ? 0 : -3
                anchors.rightMargin: row.buttons ? 0 : -3
                z: -1
                visible: part.clickable && (part.hovered || part.marked)
                color: Qt.rgba(row.theme.accent.r, row.theme.accent.g, row.theme.accent.b,
                    mouse.pressed ? 0.18 : part.marked && !part.hovered ? 0.06 : row.buttons ? 0.09 : 0.12)
                border.width: part.marked ? 1 : 0
                border.color: row.theme.accent
                radius: Math.min(row.theme.cornerRadius || 0, height / 2)
            }
            Text {
                id: label
                x: part.padding
                anchors.verticalCenter: row.buttons ? part.verticalCenter : undefined
                width: Math.max(1, part.width - part.trailing - 2 * part.padding)
                text: KeyNotation.hint([[part.modelData.sequences, part.modelData.label]], row.theme.foreground, row.theme.muted)
                textFormat: Text.StyledText
                wrapMode: Text.Wrap
                color: row.theme.muted
                font.family: row.theme.fontFamily
                font.pixelSize: row.pixelSize
            }
            MouseArea {
                id: mouse
                anchors.fill: row.buttons ? part : label
                enabled: part.clickable
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: part.activate()
            }
            Accessible.role: part.clickable ? Accessible.Button : Accessible.StaticText
            Accessible.name: part.modelData.name
            Accessible.description: part.modelData.keys
            Accessible.onPressAction: part.activate()
        }
    }
}
