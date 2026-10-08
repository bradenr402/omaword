pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

// A modal dialog card in the theme's colors, centered over a dimmed window.
Dialog {
    id: modalCard
    // The reader window (Main) it belongs to, and the live theme.
    required property var controller
    required property var theme
    modal: true
    anchors.centerIn: parent
    padding: 24
    width: Math.min(600 * modalCard.controller.scale, modalCard.controller.width - 48)
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle {
        color: modalCard.theme.background
        radius: Math.min(modalCard.theme.cornerRadius, width / 2, height / 2)
        border.color: modalCard.theme.border
    }
    Overlay.modal: Rectangle { color: modalCard.controller.tint(modalCard.theme.background, 0.72) }
    // Every modal opens at once and closes at once (it stops counting as
    // open, and focus returns, immediately); modalGhost plays its exit.
    enter: null
    exit: null
    Connections {
        target: modalCard
        function onAboutToHide() { modalCard.controller.modalExitRelease(modalCard) }
    }
}
