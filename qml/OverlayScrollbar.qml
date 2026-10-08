import QtQuick
import QtQuick.Controls

// A thin scrollbar over the content, wider while hovered or dragged and
// fading after scrolling stops.
ScrollBar {
    id: bar
    // The reader window (Main) it belongs to, and the live theme.
    required property var controller
    required property var theme
    implicitWidth: 18 * bar.controller.scale
    width: implicitWidth
    anchors.right: parent.right
    z: 10
    policy: ScrollBar.AlwaysOn
    visible: size < 1
    hoverEnabled: true
    padding: 0
    minimumSize: 0.035
    onPositionChanged: linger.restart()
    onHoveredChanged: linger.restart()
    Timer { id: linger; interval: 1100 }
    contentItem: Item {
        Rectangle {
            objectName: bar.objectName + "Thumb"
            anchors.right: parent.right
            anchors.rightMargin: 4 * bar.controller.scale
            height: parent.height
            width: (bar.hovered || bar.pressed ? 10 : 4) * bar.controller.scale
            color: bar.theme.muted
            radius: Math.min(bar.theme.cornerRadius, width / 2, height / 2)
            opacity: bar.hovered || bar.pressed ? 0.8 : linger.running ? 0.5 : 0.2
            Behavior on width { NumberAnimation { duration: bar.controller.reduceMotion ? 0 : 85 } }
            Behavior on opacity { NumberAnimation { duration: bar.controller.reduceMotion ? 0 : 140 } }
        }
    }
    background: Item {}
}
