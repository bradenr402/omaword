import QtQuick
import QtQuick.Layouts
import "KeyNotation.js" as KeyNotation

// A side panel's header, the same in every panel of the Margin: its title
// with a quiet line under it (PanelHeader), then the panel's own buttons
// (the header's children) and its close button.
RowLayout {
    id: header
    // The reader window (Main) it belongs to, and the live theme.
    required property var controller
    required property var theme
    property string title: ""
    property string subtitle: ""
    // Tests address the parts by name.
    property string titleName: ""
    property string subtitleName: ""
    property string closeName: ""
    // The close button's tooltip.
    property string closeTip: ""
    default property alias actions: buttons.data
    signal closeRequested()
    spacing: 8 * controller.scale
    PanelHeader {
        Layout.fillWidth: true
        Layout.alignment: Qt.AlignTop
        theme: header.theme
        title: header.title
        subtitle: header.subtitle
        uiScale: header.controller.scale
        titleScale: header.controller.controlScale
        titleName: header.titleName
        subtitleName: header.subtitleName
    }
    RowLayout {
        id: buttons
        Layout.alignment: Qt.AlignTop
        spacing: 4 * header.controller.scale
    }
    // Clicks beside a docked panel reach the text, so it closes here by mouse.
    QuietButton {
        objectName: header.closeName
        Layout.alignment: Qt.AlignTop
        controller: header.controller
        theme: header.theme
        text: "×"
        compact: true
        keys: KeyNotation.format("Escape")
        tip: header.closeTip
        Accessible.name: "Close " + header.title
        onClicked: header.closeRequested()
    }
}
