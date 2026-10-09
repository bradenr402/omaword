import QtQuick
import QtQuick.Layouts

// A side panel's title with a quiet subtitle under it, shared by Marks, See
// also, Notes, and the Desk: the first three name the passage they follow
// ("John 3:16–17 · NET"), the Desk its passages and words. Display only:
// never focusable or clickable.
ColumnLayout {
    id: header
    required property var theme         // desktop palette (foreground, muted, …)
    property string title: ""
    property string subtitle: ""
    property real uiScale: 1             // subtitle scale (app.scale)
    property real titleScale: uiScale    // title scale (app.controlScale)
    // Tests address the labels by name.
    property string titleName: ""
    property string subtitleName: ""
    spacing: 2 * uiScale

    Text {
        objectName: header.titleName
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        text: header.title
        textFormat: Text.PlainText
        color: header.theme.foreground
        font.family: header.theme.fontFamily
        font.pixelSize: 24 * header.titleScale
        font.weight: Font.Medium
        wrapMode: Text.Wrap
    }
    Text {
        objectName: header.subtitleName
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        visible: text.length > 0
        text: header.subtitle
        textFormat: Text.PlainText
        color: header.theme.muted
        font.family: header.theme.fontFamily
        font.pixelSize: 13 * header.uiScale
        elide: Text.ElideRight
        maximumLineCount: 1
    }
}
