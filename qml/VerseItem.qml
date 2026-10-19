import QtQuick

// A row of the Verse layout (Main's VerseView): its text, where the text
// sits in the row, and the text's search highlights.
Item {
    property real textOffset: 0
    property real textHeight: 0
    property TextEdit body
    property SearchHighlights searchOverlay
}
