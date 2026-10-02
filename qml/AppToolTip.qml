import QtQuick
import QtQuick.Controls
import QtQuick.Window

ToolTip {
    id: control
    required property var theme
    property real uiScale: theme.textScale > 0 ? theme.textScale : 1

    font.family: theme.fontFamily
    font.pixelSize: 12 * control.uiScale
    width: Math.min(implicitWidth, 360 * control.uiScale,
                    Math.max(1, (Overlay.overlay ? Overlay.overlay.width : Screen.width) - 16))

    contentItem: Text {
        text: control.text
        font: control.font
        color: control.theme.foreground
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
    }

    background: Rectangle {
        color: control.theme.surface
        border.color: control.theme.border
        radius: control.theme.cornerRadius
    }
}
