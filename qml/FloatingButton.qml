import QtQuick
import QtQuick.Controls

// A footer button: a glyph or text, its label and keys in the tooltip;
// focus mode's quiet footer hides it in place unless it is persistent.
Button {
    id: floatingButton
    // The reader window (Main) it belongs to, and the live theme.
    required property var controller
    required property var theme
    property string glyph: ""
    // What it does (its accessible name) and the keys that do the same
    // (its accessible description); the tooltip shows both.
    property string label: ""
    property string keys: ""
    property string tip: label + (keys ? " · " + keys : "")
    property bool selected: false
    // Whether the button can act now; focus mode's quiet footer also hides
    // and disables every button but the persistent one, in place.
    property bool available: true
    property bool persistent: false
    readonly property bool footerButton: true
    readonly property bool quiet: floatingButton.controller.footerQuiet && !persistent
    enabled: available && !quiet
    opacity: quiet ? 0 : 1
    Behavior on opacity { NumberAnimation { duration: floatingButton.controller.fadeMs } }
    onActiveFocusChanged: floatingButton.controller.footerFocusChanged()
    onHoveredChanged: floatingButton.controller.updateFooterHover()
    implicitWidth: glyph.length ? implicitHeight : Math.ceil(floatingText.implicitWidth + leftPadding + rightPadding) + 1
    implicitHeight: 40 * floatingButton.controller.controlScale
    padding: 8 * floatingButton.controller.controlScale
    hoverEnabled: true
    // Footer chrome: the keyboard reaches it by tab only, and a mouse click
    // returns the keyboard to the reader (or the open panel).
    focusPolicy: Qt.TabFocus
    font.pixelSize: 13 * floatingButton.controller.scale
    onPressed: floatingButton.controller.clearCount()
    property bool keyActivated: false
    // Where the keyboard goes when a key press hides this button (else restorePanelFocus).
    property Item focusTarget: null
    // esc on a footer button first hands the keyboard back to the reader
    // (or the open panel); the reader's esc rules apply from there.
    Keys.onShortcutOverride: function(event) {
        floatingButton.controller.buttonShortcutOverride(event)
        if (event.key === Qt.Key_Escape && (event.modifiers & ~Qt.KeypadModifier) === Qt.NoModifier) event.accepted = true
    }
    Keys.onEscapePressed: function(event) { event.accepted = true; floatingButton.controller.restorePanelFocus() }
    Keys.onPressed: function(event) { if (event.key === Qt.Key_Space) floatingButton.keyActivated = true }
    Keys.onReturnPressed: function(event) { floatingButton.controller.buttonEnter(floatingButton, event) }
    Keys.onEnterPressed: function(event) { floatingButton.controller.buttonEnter(floatingButton, event) }
    Connections { target: floatingButton; function onClicked() { floatingButton.controller.buttonClicked(floatingButton) } }
    // Just revealed by hover: a click that was aimed elsewhere is ignored briefly.
    MouseArea {
        anchors.fill: parent
        z: 10
        visible: floatingButton.controller.footerGuard && !floatingButton.persistent
        acceptedButtons: Qt.AllButtons
    }
    Accessible.name: label || tip
    Accessible.description: keys
    AppToolTip { theme: floatingButton.theme; uiScale: floatingButton.controller.scale; visible: floatingButton.hovered || floatingButton.activeFocus; text: floatingButton.tip }
    background: Rectangle {
        color: floatingButton.down ? floatingButton.controller.tint(floatingButton.theme.accent, 0.18)
            : floatingButton.hovered ? floatingButton.controller.tint(floatingButton.theme.accent, 0.09) : "transparent"
        radius: Math.min(floatingButton.theme.cornerRadius, width / 2, height / 2)
        border.width: floatingButton.activeFocus ? 1 : 0
        border.color: floatingButton.theme.accent
    }
    contentItem: Item {
        Text {
            id: floatingText
            objectName: floatingButton.objectName + "Label"
            anchors.fill: parent
            text: floatingButton.text
            visible: !floatingButton.glyph.length
            color: floatingButton.theme.foreground
            font: floatingButton.font
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        Canvas {
            id: floatingIcon
            anchors.centerIn: parent
            width: 22 * floatingButton.controller.controlScale
            height: width
            visible: floatingButton.glyph.length > 0 && floatingButton.glyph !== "saved"
            property color ink: !floatingButton.enabled ? floatingButton.theme.muted : floatingButton.selected ? floatingButton.theme.accent : floatingButton.theme.foreground
            property string glyph: floatingButton.glyph
            onInkChanged: requestPaint()
            onGlyphChanged: requestPaint()
            onPaint: {
                let ctx = getContext("2d")
                ctx.reset()
                ctx.scale(width / 24, height / 24)
                ctx.strokeStyle = ink
                ctx.fillStyle = ink
                ctx.lineWidth = 1.6
                ctx.lineCap = "square"
                ctx.lineJoin = "miter"
                ctx.beginPath()
                if (glyph === "chevron-left") {
                    ctx.moveTo(15, 5); ctx.lineTo(8, 12); ctx.lineTo(15, 19)
                } else if (glyph === "chevron-right") {
                    ctx.moveTo(9, 5); ctx.lineTo(16, 12); ctx.lineTo(9, 19)
                } else if (glyph === "search") {
                    ctx.arc(10.5, 10.5, 7, 0, 2 * Math.PI)
                    ctx.moveTo(15.45, 15.45); ctx.lineTo(20, 20)
                } else if (glyph === "help") {
                    ctx.moveTo(8, 7); ctx.bezierCurveTo(8, 1, 19, 2, 16, 9)
                    ctx.bezierCurveTo(15, 11, 12, 11, 12, 15)
                    ctx.moveTo(12, 19); ctx.lineTo(12, 19.3)
                } else if (glyph === "desk") {
                    // Stacked pages resting in an open tray.
                    ctx.moveTo(7, 4); ctx.lineTo(17, 4)
                    ctx.moveTo(5, 8); ctx.lineTo(19, 8)
                    ctx.moveTo(2, 13); ctx.lineTo(8, 13); ctx.lineTo(9.5, 16)
                    ctx.lineTo(14.5, 16); ctx.lineTo(16, 13); ctx.lineTo(22, 13)
                    ctx.lineTo(22, 21); ctx.lineTo(2, 21); ctx.closePath()
                } else if (glyph.startsWith("layout-")) {
                    floatingButton.controller.paintLayoutGlyph(ctx, glyph.slice(7))
                } else if (glyph === "study") {
                    // An open book under a magnifier: reading closely.
                    // The lens hides the book's lower-right corner.
                    ctx.moveTo(18.5, 9.5); ctx.lineTo(18.5, 3.5); ctx.lineTo(13.5, 3.5)
                    ctx.lineTo(10, 5); ctx.lineTo(6.5, 3.5); ctx.lineTo(1.5, 3.5)
                    ctx.lineTo(1.5, 16); ctx.lineTo(6.5, 16); ctx.lineTo(10, 17.5); ctx.lineTo(11, 17.1)
                    ctx.moveTo(10, 5); ctx.lineTo(10, 11.5)
                    ctx.moveTo(4, 7); ctx.lineTo(7, 7)
                    ctx.moveTo(4, 10.5); ctx.lineTo(7, 10.5)
                    ctx.moveTo(22, 22); ctx.lineTo(19.6, 19.6)
                    ctx.stroke()
                    ctx.beginPath()
                    ctx.arc(16, 16, 5, 0, 2 * Math.PI)
                } else if (glyph === "more") {
                    ctx.fillRect(4, 11, 3, 3); ctx.fillRect(10.5, 11, 3, 3); ctx.fillRect(17, 11, 3, 3)
                } else if (glyph === "focus") {
                    // A viewfinder: four corner brackets around a dot.
                    ctx.moveTo(3, 8); ctx.lineTo(3, 3); ctx.lineTo(8, 3)
                    ctx.moveTo(16, 3); ctx.lineTo(21, 3); ctx.lineTo(21, 8)
                    ctx.moveTo(21, 16); ctx.lineTo(21, 21); ctx.lineTo(16, 21)
                    ctx.moveTo(8, 21); ctx.lineTo(3, 21); ctx.lineTo(3, 16)
                    ctx.stroke()
                    ctx.beginPath()
                    ctx.arc(12, 12, 2.2, 0, 2 * Math.PI)
                    ctx.fill()
                    ctx.beginPath()
                } else if (glyph === "visual") {
                    // Three lines of text, whole lines (verses) under a selection's tint.
                    ctx.globalAlpha = 0.4
                    ctx.fillRect(1.5, 8.5, 21, 14)
                    ctx.globalAlpha = 1
                    ctx.moveTo(4, 5); ctx.lineTo(20, 5)
                    ctx.moveTo(4, 12); ctx.lineTo(20, 12)
                    ctx.moveTo(4, 19); ctx.lineTo(14, 19)
                } else if (glyph === "characters") {
                    // A text cursor (I-beam) ending a few selected characters:
                    // two shapes, so it still reads at the footer's small size.
                    ctx.globalAlpha = 0.45
                    ctx.fillRect(3, 8, 9.5, 8)
                    ctx.globalAlpha = 1
                    ctx.moveTo(15, 4); ctx.lineTo(15, 20)
                    ctx.moveTo(12, 4); ctx.lineTo(18, 4)
                    ctx.moveTo(12, 20); ctx.lineTo(18, 20)
                } else {
                    ctx.moveTo(12, 6); ctx.lineTo(8, 4); ctx.lineTo(2, 4)
                    ctx.lineTo(2, 19); ctx.lineTo(8, 19); ctx.lineTo(12, 21)
                    ctx.lineTo(16, 19); ctx.lineTo(22, 19); ctx.lineTo(22, 4)
                    ctx.lineTo(16, 4); ctx.closePath()
                    ctx.moveTo(12, 6); ctx.lineTo(12, 21)
                    ctx.moveTo(5, 8); ctx.lineTo(8, 8)
                    ctx.moveTo(16, 8); ctx.lineTo(19, 8)
                }
                ctx.stroke()
            }
        }
        AnnotationGlyph {
            objectName: floatingButton.objectName + "Glyph"
            anchors.centerIn: parent
            width: 22 * floatingButton.controller.controlScale; height: width
            // Annotations wears the note glyph.
            visible: floatingButton.glyph === "saved"
            kind: "note"
            ink: floatingIcon.ink
        }
    }
}
