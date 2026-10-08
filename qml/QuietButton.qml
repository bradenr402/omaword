import QtQuick
import QtQuick.Controls

// A panel's or dialog's button: quiet until hovered, accent when emphasized,
// with its keys in the tooltip; Main's key and focus rules apply to it.
Button {
    id: button
    // The reader window (Main) it belongs to, and the live theme.
    required property var controller
    required property var theme
    property bool emphasized: false
    property bool compact: false
    // Accessible description: the keys that do the same (also in the tooltip).
    property string keys: ""
    property string tip: ""
    implicitHeight: compact ? 32 * button.controller.scale : 38 * button.controller.scale
    implicitWidth: Math.max(implicitHeight, buttonLabel.implicitWidth + 24)
    padding: 10
    hoverEnabled: true
    font.family: button.theme.fontFamily
    font.pixelSize: 13 * button.controller.scale
    contentItem: Text {
        id: buttonLabel
        text: button.text
        font: button.font
        color: !button.enabled ? button.controller.tint(button.theme.muted, 0.55) : button.emphasized ? button.theme.accent : button.theme.foreground
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    background: Rectangle {
        radius: Math.min(button.theme.cornerRadius, width / 2, height / 2)
        color: button.down ? button.controller.tint(button.theme.accent, 0.16) : button.emphasized ? button.controller.tint(button.theme.accent, 0.10) : button.hovered ? button.theme.surface : "transparent"
        border.width: button.activeFocus ? 1 : 0
        border.color: button.theme.accent
    }
    // Chrome buttons take the keyboard only by tab; a mouse click leaves it where it was.
    focusPolicy: Qt.TabFocus
    property bool keyActivated: false
    // Where the keyboard goes when a key press hides this button (else restorePanelFocus).
    property Item focusTarget: null
    Keys.onShortcutOverride: function(event) { button.controller.buttonShortcutOverride(event) }
    Keys.onPressed: function(event) { if (event.key === Qt.Key_Space) button.keyActivated = true }
    Keys.onReturnPressed: function(event) { button.controller.buttonEnter(button, event) }
    Keys.onEnterPressed: function(event) { button.controller.buttonEnter(button, event) }
    Connections { target: button; function onClicked() { button.controller.buttonClicked(button) } }
    AppToolTip { theme: button.theme; uiScale: button.controller.scale; visible: (button.hovered || button.activeFocus) && button.tip.length > 0; text: button.tip }
    Accessible.name: text
    Accessible.description: keys
}
