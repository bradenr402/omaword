import QtQuick
import QtQuick.Controls

// Outside an open side panel, a press only closes the panel: it never
// reaches the reader or footer beneath (a Drawer alone lets it through).
// Hidden while a dialog, picker, or menu sits above the panel.
MouseArea {
    id: outside
    // The reader window (Main) it belongs to.
    required property var controller
    required property Drawer drawer
    property bool shown: drawer.visible
    signal dismiss()
    parent: Overlay.overlay
    anchors.left: parent.left
    anchors.top: parent.top
    anchors.bottom: parent.bottom
    width: outside.controller.width - drawer.width * drawer.position
    z: outside.drawer.z + 0.1
    visible: shown && !outside.controller.dialogOpen
    acceptedButtons: Qt.AllButtons
    onPressed: outside.dismiss()
    onWheel: function(wheel) { wheel.accepted = true }
}
