import QtQuick
import QtQuick.Controls

// A panel section's small title in the theme's muted color: Main's SectionLabel.
Label {
    required property var theme
    property real uiScale: 1
    color: theme.muted
    font.pixelSize: 11 * uiScale
    font.weight: Font.DemiBold
    font.letterSpacing: 1.5
}
