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
                    // A desk with a drawer and a reading lamp. The legs and the
                    // drawer end flat under the top's centre line, and the arm
                    // starts in the lamp's foot and ends in its shade, so no
                    // stroke shows through another's edge.
                    ctx.moveTo(2, 13); ctx.lineTo(22, 13)
                    ctx.stroke()
                    ctx.beginPath()
                    ctx.lineCap = "butt"
                    ctx.moveTo(4, 13); ctx.lineTo(4, 21.8)
                    ctx.moveTo(20, 13); ctx.lineTo(20, 21.8)
                    ctx.moveTo(4, 17); ctx.lineTo(11, 17); ctx.lineTo(11, 13)
                    ctx.stroke()
                    ctx.beginPath()
                    ctx.moveTo(14.6, 13); ctx.lineTo(15.4, 11.4); ctx.lineTo(17.6, 11.4); ctx.lineTo(18.4, 13); ctx.closePath()
                    ctx.fill()
                    ctx.beginPath()
                    ctx.lineJoin = "bevel"
                    ctx.moveTo(16.8, 12.2); ctx.lineTo(13, 7.6); ctx.lineTo(16.6, 4.1)
                    ctx.stroke()
                    ctx.beginPath()
                    ctx.moveTo(15.4, 3); ctx.lineTo(21.4, 5.6); ctx.lineTo(18.2, 9.2); ctx.closePath()
                    ctx.fill()
                    ctx.beginPath()
                } else if (glyph.startsWith("layout-")) {
                    floatingButton.controller.paintLayoutGlyph(ctx, glyph.slice(7))
                } else if (glyph === "study") {
                    // See also: a link, two open loops joined by a bar, on the
                    // diagonal (scaled to keep its corners inside the box).
                    ctx.save()
                    ctx.translate(12, 12); ctx.rotate(-Math.PI / 4); ctx.scale(0.94, 0.94); ctx.translate(-12, -12)
                    ctx.moveTo(10, 6.5); ctx.lineTo(5, 6.5); ctx.arcTo(1.5, 6.5, 1.5, 10, 3.5); ctx.lineTo(1.5, 14)
                    ctx.arcTo(1.5, 17.5, 5, 17.5, 3.5); ctx.lineTo(10, 17.5)
                    ctx.moveTo(14, 6.5); ctx.lineTo(19, 6.5); ctx.arcTo(22.5, 6.5, 22.5, 10, 3.5); ctx.lineTo(22.5, 14)
                    ctx.arcTo(22.5, 17.5, 19, 17.5, 3.5); ctx.lineTo(14, 17.5)
                    ctx.moveTo(9, 12); ctx.lineTo(15, 12)
                    ctx.stroke()
                    ctx.restore()
                    ctx.beginPath()
                } else if (glyph === "text") {
                    // Aa (text size and layout): a capital and a small letter,
                    // drawn as tall as the other glyphs.
                    ctx.lineJoin = "bevel"
                    ctx.moveTo(1.5, 20); ctx.lineTo(7.5, 4); ctx.lineTo(13.5, 20)
                    ctx.moveTo(3.6, 14.5); ctx.lineTo(11.4, 14.5)
                    ctx.moveTo(22, 12.2); ctx.lineTo(22, 20)
                    ctx.stroke()
                    ctx.beginPath()
                    ctx.arc(18.1, 16.2, 3.8, 0, 2 * Math.PI)
                } else if (glyph === "more") {
                    ctx.fillRect(4, 11, 3, 3); ctx.fillRect(10.5, 11, 3, 3); ctx.fillRect(17, 11, 3, 3)
                } else if (glyph === "focus") {
                    // Focus mode in miniature: the line being read in full
                    // ink, heavier, the lines around it faint.
                    ctx.globalAlpha = 0.35
                    ctx.moveTo(4, 6); ctx.lineTo(20, 6)
                    ctx.moveTo(4, 18); ctx.lineTo(15, 18)
                    ctx.stroke()
                    ctx.beginPath()
                    ctx.globalAlpha = 1
                    ctx.lineWidth = 2.4
                    ctx.moveTo(3, 12); ctx.lineTo(21, 12)
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
