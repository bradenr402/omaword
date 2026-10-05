import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One row of quiet tabs, shared by every tabbed panel (Annotations, Study,
// the palette's Passage/Search/Command modes) so they size, space, and mark
// the selected and focused tab the same way: accent label on a light accent
// tint when selected, an accent outline only for keyboard focus.
RowLayout {
    id: root
    required property var labels        // tab titles
    required property var theme         // desktop palette (foreground, accent, …)
    property int currentIndex: -1
    property real uiScale: 1
    // Per-tab objectNames (tests); defaults to namePrefix + index.
    property string namePrefix: "tab"
    property var names: []
    // Per-tab enabled flags; missing entries are enabled.
    property var enabledTabs: []
    // Tabs take keyboard focus (Return activates) unless the panel drives them by keys.
    property bool focusable: true
    // Equal-width cells instead of natural label widths.
    property bool uniform: false
    signal activated(int index)

    spacing: 4 * uiScale
    uniformCellSizes: uniform
    // One row takes priority over unbounded text scaling: natural label
    // widths, with the same bounded font for all tabs.
    readonly property real labelSize: Math.max(12, Math.min(14 * uiScale, width / (7 * Math.max(1, labels.length) + 1)))

    Repeater {
        model: root.labels
        delegate: Button {
            id: tab
            required property string modelData
            required property int index
            readonly property bool emphasized: root.currentIndex === index // selected
            objectName: root.names[index] || root.namePrefix + index
            text: modelData
            enabled: root.enabledTabs[index] !== false
            checkable: false
            focusPolicy: root.focusable ? Qt.StrongFocus : Qt.NoFocus
            hoverEnabled: true
            implicitHeight: 32 * root.uiScale
            // Measured at the largest label size so the row's implicit width
            // never depends on its own width (no layout feedback loop).
            implicitWidth: Math.max(implicitHeight, natural.advanceWidth + 2 * padding)
            TextMetrics { id: natural; text: tab.text; font.family: root.theme.fontFamily; font.pixelSize: 14 * root.uiScale }
            padding: 6 * root.uiScale
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.preferredWidth: root.uniform ? 1 : implicitWidth
            font.family: root.theme.fontFamily
            font.pixelSize: root.labelSize
            contentItem: Text {
                id: label
                text: tab.text
                font: tab.font
                color: !tab.enabled ? Qt.rgba(root.theme.muted.r, root.theme.muted.g, root.theme.muted.b, 0.55)
                    : tab.emphasized ? root.theme.accent : root.theme.foreground
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }
            background: Rectangle {
                radius: Math.min(root.theme.cornerRadius, width / 2, height / 2)
                color: tab.down ? Qt.rgba(root.theme.accent.r, root.theme.accent.g, root.theme.accent.b, 0.16)
                    : tab.emphasized ? Qt.rgba(root.theme.accent.r, root.theme.accent.g, root.theme.accent.b, 0.10)
                    : tab.hovered ? root.theme.surface : "transparent"
                border.width: tab.activeFocus ? 1 : 0
                border.color: root.theme.accent
            }
            onClicked: root.activated(index)
            // A focused tab owns enter and space (never a window shortcut).
            Keys.onShortcutOverride: function(event) { if ((event.modifiers & ~Qt.KeypadModifier) === Qt.NoModifier && [Qt.Key_Space, Qt.Key_Return, Qt.Key_Enter].includes(event.key)) event.accepted = true }
            Keys.onReturnPressed: clicked()
            Keys.onEnterPressed: clicked()
            Accessible.role: Accessible.PageTab
            Accessible.name: text
        }
    }
}
