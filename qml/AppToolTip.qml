import QtQuick
import QtQuick.Controls
import QtQuick.Window

ToolTip {
    id: control
    required property var theme

    font.family: theme.fontFamily
    font.pixelSize: 12 * theme.textScale
    width: Math.min(implicitWidth, 360 * theme.textScale,
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
