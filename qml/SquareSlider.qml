import QtQuick
import QtQuick.Controls

// A slider in the theme's colors and corners.
Slider {
    id: slider
    // The reader window (Main) it belongs to, and the live theme.
    required property var controller
    required property var theme
    implicitHeight: 32 * slider.controller.scale
    implicitWidth: 200 * slider.controller.scale
    padding: 9 * slider.controller.scale
    Keys.priority: Keys.BeforeItem
    // ← → and ↑ ↓ all adjust by one step (right and up increase).
    Keys.onPressed: function(event) {
        if (event.modifiers === Qt.NoModifier && [Qt.Key_Left, Qt.Key_Right, Qt.Key_Up, Qt.Key_Down].includes(event.key)) {
            if (event.key === Qt.Key_Up || event.key === Qt.Key_Right) increase()
            else decrease()
            moved()
            event.accepted = true
        }
    }
    background: Rectangle {
        x: slider.leftPadding
        y: slider.topPadding + (slider.availableHeight - height) / 2
        width: slider.availableWidth
        height: 3 * slider.controller.scale
        radius: Math.min(slider.theme.cornerRadius, width / 2, height / 2)
        color: slider.theme.border
        Rectangle {
            objectName: slider.objectName + "Track"
            x: slider.mirrored ? parent.width - width : 0
            width: slider.position * parent.width
            height: parent.height
            radius: Math.min(slider.theme.cornerRadius, width / 2, height / 2)
            color: slider.theme.accent
        }
    }
    handle: Rectangle {
        objectName: slider.objectName + "Handle"
        x: slider.leftPadding + slider.visualPosition * (slider.availableWidth - width)
        y: slider.topPadding + (slider.availableHeight - height) / 2
        implicitWidth: 18 * slider.controller.scale
        implicitHeight: 18 * slider.controller.scale
        radius: Math.min(slider.theme.cornerRadius, width / 2, height / 2)
        color: slider.pressed ? slider.theme.accent : slider.theme.surface
        border.color: slider.theme.accent
        border.width: slider.activeFocus ? 2 : 1
    }
}
