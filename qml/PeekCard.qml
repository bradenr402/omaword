pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A peek: what the text points to (a footnote), read where it is, without
// leaving the passage or opening a panel. It opens at once below its anchor
// (above when there is no room), keeps the keyboard while open, and closes
// on esc or a click outside, which acts on nothing else; the owner plays its
// exit (menuGhost, toward `origin`). Its actions are a HintRow in buttons
// mode, as on the selection bar; other keys go to the owner (keyPressed).
Popup {
    id: card
    required property var theme
    property real uiScale: theme.textScale > 0 ? theme.textScale : 1
    property string title: ""
    property string body: ""
    property var parts: []
    property point origin: Qt.point(0, 0)
    signal keyPressed(var event)

    parent: Overlay.overlay
    modal: true
    dim: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    enter: null
    exit: null
    padding: 0
    width: Math.min(420 * uiScale, (parent ? parent.width : 420) - 16)

    // Below the anchor's scene point, or above it, kept inside the window.
    function show(sceneX, sceneY, lineHeight) {
        origin = Qt.point(sceneX, sceneY)
        open()
        const area = parent as Item
        const gap = 6 * uiScale
        const below = sceneY + (lineHeight || 0) + gap
        x = Math.round(Math.max(8, Math.min(sceneX - 12 * uiScale, area.width - width - 8)))
        y = Math.round(below + height <= area.height - 8 ? below : Math.max(8, sceneY - height - gap))
    }

    background: Rectangle {
        color: card.theme.background
        border.color: card.theme.border
        radius: Math.min(card.theme.cornerRadius || 0, 12 * card.uiScale)
    }
    contentItem: ColumnLayout {
        spacing: 0
        focus: true
        Keys.onPressed: function(event) { card.keyPressed(event) }
        Accessible.role: Accessible.Dialog
        Accessible.name: card.title
        Accessible.description: card.body
        Label {
            objectName: "peekTitle"
            text: card.title
            textFormat: Text.PlainText
            color: card.theme.muted
            font.family: card.theme.fontFamily
            font.pixelSize: 12 * card.uiScale
            elide: Text.ElideRight
            topPadding: 12 * card.uiScale; leftPadding: 14 * card.uiScale; rightPadding: 14 * card.uiScale
            Layout.fillWidth: true
        }
        Flickable {
            id: bodyView
            objectName: "peekBodyView"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(bodyText.implicitHeight, 0.4 * ((card.parent as Item)?.height ?? 600))
            contentHeight: bodyText.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            Text {
                id: bodyText
                objectName: "peekBody"
                width: bodyView.width
                text: card.body
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: card.theme.foreground
                font.family: card.theme.fontFamily
                font.pixelSize: 14 * card.uiScale
                lineHeight: 1.25
                topPadding: 6 * card.uiScale; bottomPadding: 12 * card.uiScale
                leftPadding: 14 * card.uiScale; rightPadding: 14 * card.uiScale
            }
        }
        Rectangle { color: card.theme.border; implicitHeight: 1; Layout.fillWidth: true; opacity: 0.65 }
        HintRow {
            objectName: "peekHints"
            theme: card.theme
            parts: card.parts
            pixelSize: 12 * card.uiScale
            partHeight: 30 * card.uiScale
            partPadding: 10 * card.uiScale
            Layout.margins: 1
        }
    }
}
