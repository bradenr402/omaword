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
    // A drawn glyph in place of the label: "close" (a side panel, away to
    // the right edge), "close-down" (a strip, away down), "plus", "minus".
    property string glyph: ""
    property real glyphSize: 14 * button.controller.scale
    // Accessible description: the keys that do the same (also in the tooltip).
    property string keys: ""
    property string tip: ""
    implicitHeight: compact ? 32 * button.controller.scale : 38 * button.controller.scale
    implicitWidth: glyph.length ? implicitHeight : Math.max(implicitHeight, buttonLabel.implicitWidth + 24)
    padding: 10
    hoverEnabled: true
    font.family: button.theme.fontFamily
    font.pixelSize: 13 * button.controller.scale
    contentItem: Text {
        id: buttonLabel
        // A glyph button keeps its text for its accessible name only.
        text: button.glyph.length ? "" : button.text
        font: button.font
        color: !button.enabled ? button.controller.tint(button.theme.muted, 0.55) : button.emphasized ? button.theme.accent : button.theme.foreground
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        Canvas {
            objectName: button.objectName.length ? button.objectName + "Glyph" : ""
            visible: button.glyph.length > 0
            anchors.centerIn: parent
            width: button.glyphSize
            height: width
            property color ink: buttonLabel.color
            property string glyph: button.glyph
            onInkChanged: requestPaint()
            onGlyphChanged: requestPaint()
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                ctx.scale(width / 24, height / 24)
                ctx.strokeStyle = ink
                ctx.lineWidth = 1.6
                ctx.lineCap = "square"
                ctx.lineJoin = "miter"
                ctx.beginPath()
                if (glyph === "close-down") { ctx.translate(24, 0); ctx.rotate(Math.PI / 2) }
                if (glyph === "close" || glyph === "close-down") {
                    // A chevron into the edge it goes away to.
                    ctx.moveTo(10, 6.5); ctx.lineTo(15.5, 12); ctx.lineTo(10, 17.5)
                    ctx.moveTo(19.5, 4.5); ctx.lineTo(19.5, 19.5)
                } else if (glyph === "plus") {
                    ctx.moveTo(12, 5); ctx.lineTo(12, 19); ctx.moveTo(5, 12); ctx.lineTo(19, 12)
                } else if (glyph === "minus") {
                    ctx.moveTo(5, 12); ctx.lineTo(19, 12)
                }
                ctx.stroke()
            }
        }
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
