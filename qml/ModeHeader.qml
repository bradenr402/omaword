import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A selection mode's header across the top of the window, the same for each
// mode (Visual, Characters): its name and what waits on it, in the accent,
// beside the keys it takes (HintRow parts, clickable where they act).
Rectangle {
    id: root
    // The reader window (Main) it belongs to, and the live theme.
    required property var controller
    required property var theme
    property string title: ""
    property alias parts: hints.parts
    // Tests address the parts by name: "<names>Title", "<names>Hints".
    property string names: ""
    implicitHeight: content.implicitHeight + 16
    color: theme.surface
    RowLayout {
        id: content
        anchors.fill: parent
        anchors.margins: 8
        anchors.leftMargin: 24
        spacing: 16
        Label {
            objectName: root.names + "Title"
            Layout.alignment: Qt.AlignTop
            text: root.title
            textFormat: Text.PlainText
            color: root.theme.accent
            font.pixelSize: 13 * root.controller.scale
        }
        HintRow {
            id: hints
            objectName: root.names + "Hints"
            Layout.fillWidth: true
            Layout.preferredWidth: 0
            Layout.alignment: Qt.AlignTop
            theme: root.theme
            pixelSize: 13 * root.controller.scale
        }
    }
}
