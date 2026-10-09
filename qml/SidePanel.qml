import QtQuick

// A panel of the Margin (Notes, the Desk, Marks, See also), the same frame
// for each: the theme's background with an edge toward the text, an accent
// while the panel has the keyboard (the Margin's keys, controller.marginKeys)
// and a hairline while the reader has it, shown while the Margin is on its
// mode. Its content keeps `inset` from the edges and `gap` between its
// sections, its header first (SidePanelHeader).
Rectangle {
    id: shell
    // The reader window (Main) it belongs to, and the live theme.
    required property var controller
    required property var theme
    // The Margin's mode it shows (Main.marginMode).
    required property string mode
    readonly property real inset: Math.round(22 * controller.controlScale)
    readonly property real gap: Math.round(16 * controller.controlScale)
    readonly property bool keys: !!controller.marginKeys
    anchors.fill: parent
    visible: controller.marginMode === mode
    color: theme.background
    radius: Math.min(theme.cornerRadius, width / 2, height / 2)
    Rectangle {
        objectName: shell.objectName.length ? shell.objectName + "Edge" : ""
        anchors.left: parent.left
        width: shell.keys ? Math.max(2, Math.round(2 * shell.controller.controlScale)) : 1
        height: parent.height
        color: shell.keys ? shell.theme.accent : shell.theme.border
    }
}
