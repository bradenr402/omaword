import QtQuick

// The hairline between a panel's sections, in the theme's border color:
// Main's and TagsPanel's FineRule.
Rectangle {
    required property var theme
    color: theme.border
    implicitHeight: 1
    opacity: 0.65
}
