import QtQuick

// A scrollable dialog pane: it takes focus, scrolls by key (scrollKey),
// and keeps the focused control inside it visible (revealFocus).
OverlayScrollView {
    id: keyScroll
    activeFocusOnTab: true
    focusPolicy: Qt.StrongFocus
    Keys.onPressed: function(event) { keyScroll.controller.scrollKey(keyScroll.contentItem as Flickable, event) }
    Connections {
        target: keyScroll.controller
        enabled: keyScroll.visible
        function onActiveFocusItemChanged() { keyScroll.controller.revealFocus(keyScroll.contentItem as Flickable) }
    }
}
