import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A Reading settings switch for a catalog action: its title, its key, and
// the setting it shows; activating it runs the action.
Switch {
    id: settingSwitch
    // The reader window (Main) it belongs to, and the live theme.
    required property var controller
    required property var theme
    required property var backend
    required property var catalogAction
    objectName: "setting" + catalogAction.id
    text: catalogAction.title
    // Words of Jesus in a version that does not mark them: off and disabled,
    // whatever the stored setting.
    readonly property bool unsupported: catalogAction.id === "wordsOfJesus" && settingSwitch.backend.ready && !settingSwitch.backend.hasWordsOfJesus
    checked: !unsupported && !!settingSwitch.backend[catalogAction.setting]
    enabled: !unsupported && settingSwitch.controller.actionAvailable(catalogAction)
    implicitHeight: Math.max(40 * settingSwitch.controller.scale, settingLabel.implicitHeight + 16 * settingSwitch.controller.scale)
    padding: 0
    // Room inside the focus outline.
    leftPadding: 8 * settingSwitch.controller.scale
    rightPadding: 8 * settingSwitch.controller.scale
    spacing: 12 * settingSwitch.controller.scale
    onClicked: settingSwitch.controller.runCommand(catalogAction.id)
    Keys.onShortcutOverride: function(event) { settingSwitch.controller.buttonShortcutOverride(event) }
    Keys.onReturnPressed: settingSwitch.controller.runCommand(catalogAction.id)
    Keys.onEnterPressed: settingSwitch.controller.runCommand(catalogAction.id)
    indicator: Rectangle {
        objectName: settingSwitch.objectName + "Track"
        x: settingSwitch.width - width - settingSwitch.rightPadding
        y: (settingSwitch.height - height) / 2
        width: 36 * settingSwitch.controller.scale
        height: 20 * settingSwitch.controller.scale
        radius: Math.min(settingSwitch.theme.cornerRadius, width / 2, height / 2)
        color: settingSwitch.checked ? settingSwitch.theme.accent : settingSwitch.theme.surface
        border.color: settingSwitch.activeFocus ? settingSwitch.theme.foreground : settingSwitch.checked ? settingSwitch.theme.accent : settingSwitch.theme.border
        Rectangle {
            objectName: settingSwitch.objectName + "Thumb"
            x: (settingSwitch.checked ? 19 : 3) * settingSwitch.controller.scale
            y: 3 * settingSwitch.controller.scale
            width: 14 * settingSwitch.controller.scale
            height: width
            radius: Math.min(settingSwitch.theme.cornerRadius, width / 2, height / 2)
            color: settingSwitch.checked ? settingSwitch.theme.background : settingSwitch.theme.muted
        }
    }
    contentItem: RowLayout {
        spacing: 12 * settingSwitch.controller.scale
        Label {
            id: settingLabel
            objectName: settingSwitch.objectName + "Label"
            text: settingSwitch.text
            color: settingSwitch.enabled ? settingSwitch.theme.foreground : settingSwitch.theme.muted
            font.pixelSize: 13 * settingSwitch.controller.scale
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }
        Label {
            objectName: settingSwitch.objectName + "Key"
            text: settingSwitch.controller.keyRich(settingSwitch.catalogAction)
            textFormat: Text.StyledText
            color: settingSwitch.theme.muted
            font.pixelSize: 12 * settingSwitch.controller.scale
        }
        Item { Layout.preferredWidth: 36 * settingSwitch.controller.scale; Layout.preferredHeight: 20 * settingSwitch.controller.scale }
    }
    // Keyboard focus outlines the whole row, like a focused button.
    background: Rectangle {
        objectName: settingSwitch.objectName + "Focus"
        radius: Math.min(settingSwitch.theme.cornerRadius, width / 2, height / 2)
        color: settingSwitch.hovered ? settingSwitch.theme.surface : "transparent"
        border.width: settingSwitch.activeFocus ? 1 : 0
        border.color: settingSwitch.theme.accent
    }
}
