import QtQuick
import QtQuick.Controls

// A ScrollView whose vertical OverlayScrollbar paints over its content.
ScrollView {
    id: view
    // The reader window (Main) it belongs to, and the live theme.
    required property var controller
    required property var theme
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
    ScrollBar.vertical: OverlayScrollbar { controller: view.controller; theme: view.theme; objectName: parent.objectName + "Scrollbar" }
    // Scrollbars paint over the viewport, including when a platform style
    // would otherwise reserve their implicit width as a gutter.
    leftPadding: 0
    rightPadding: 0
}
