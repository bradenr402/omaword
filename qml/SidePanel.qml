import QtQuick

// A panel of the Margin (Notes, the Desk, Marks, See also), the same frame
// for each: the theme's background with a hairline toward the text, shown
// while the Margin is on its mode. Its content keeps `inset` from the edges
// and `gap` between its sections, its header first (SidePanelHeader).
Rectangle {
    id: shell
    // The reader window (Main) it belongs to, and the live theme.
    required property var controller
    required property var theme
    // The Margin's mode it shows (Main.marginMode).
    required property string mode
    readonly property real inset: Math.round(22 * controller.controlScale)
    readonly property real gap: Math.round(16 * controller.controlScale)
    anchors.fill: parent
    visible: controller.marginMode === mode
    color: theme.background
    radius: Math.min(theme.cornerRadius, width / 2, height / 2)
    Rectangle { anchors.left: parent.left; width: 1; height: parent.height; color: shell.theme.border }
}
