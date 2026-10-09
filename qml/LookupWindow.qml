pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

// Look up (omaword lookup): a small window opened where you are, for a
// passage without going to OmaWord. Type a reference (compact forms too:
// jn316, ps23) and enter shows it; there y copies it (shift Y the plain
// text) and enter opens it in your reading window. Its size is fixed, which
// Hyprland floats. Its own backend reads the version you read, or the one
// asked for, and saves nothing.
Window {
    id: lookup
    objectName: "lookupWindow"
    // The live theme (Desktop), its own BibleBackend, and the sound effects.
    required property var theme
    required property var backend
    required property var effects
    property string initialText: ""
    signal openRequested(string reference)
    readonly property real uiScale: lookup.theme.textScale > 0 ? lookup.theme.textScale : 1
    readonly property real controlScale: Math.min(uiScale, 1.4)
    readonly property real inset: Math.round(20 * controlScale)
    width: Math.round(560 * controlScale)
    height: Math.round(420 * controlScale)
    minimumWidth: width; maximumWidth: width
    minimumHeight: height; maximumHeight: height
    title: "Look up · " + lookup.theme.appName
    color: lookup.theme.background

    // The palette's suggestions for what is typed; the passage shown, once chosen.
    property var candidates: []
    property int current: 0
    property var shown: null
    readonly property bool reading: shown !== null
    property string status: ""

    function refresh() {
        candidates = query.text.trim().length ? Array.from(lookup.backend.referenceCandidates(query.text, 6)) : []
        current = 0
    }
    function move(delta) {
        if (!candidates.length) return
        current = Math.max(0, Math.min(candidates.length - 1, current + delta))
    }
    function show(index) {
        const row = candidates[index]
        if (!row) { lookup.effects.play("error"); return }
        const passage = lookup.backend.readPassage(row.reference)
        if (passage.error) { status = passage.error; lookup.effects.play("error"); return }
        shown = passage
        status = ""
        passageView.contentY = 0
        passageView.forceActiveFocus()
    }
    function back() { shown = null; status = ""; query.forceActiveFocus() }
    function copy(plain) {
        if (!shown) return
        lookup.backend.copyText(plain ? shown.plain : shown.quotation)
        status = shown.reference + (plain ? " copied as plain text" : " copied")
        lookup.effects.play("chime")
    }
    function openInReader() {
        if (!shown) return
        openRequested(shown.reference)
        close()
    }
    // The passage as the reader shows it: a number before each verse where there are several.
    function passageText(passage) {
        const verses = Array.from(passage.verses || [])
        if (verses.length < 2) return passage.plain
        const digits = "⁰¹²³⁴⁵⁶⁷⁸⁹"
        return verses.map(function(verse) {
            return String(verse.verse).split("").map(function(digit) { return digits[Number(digit)] }).join("") + " " + verse.text
        }).join(" ")
    }
    function preview(row) {
        const passage = lookup.backend.readPassage(row.reference)
        return passage.error ? "" : passage.plain.replace(/\s+/g, " ")
    }

    Component.onCompleted: {
        query.text = initialText
        refresh()
        query.forceActiveFocus()
        query.cursorPosition = query.length
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: lookup.inset
        spacing: Math.round(12 * lookup.controlScale)
        RowLayout {
            Layout.fillWidth: true
            visible: !lookup.reading
            spacing: Math.round(10 * lookup.controlScale)
            TextField {
                id: query
                objectName: "lookupQuery"
                Layout.fillWidth: true
                implicitHeight: Math.round(38 * lookup.controlScale)
                font.family: lookup.theme.fontFamily
                font.pixelSize: Math.round(15 * lookup.controlScale)
                color: lookup.theme.foreground
                placeholderText: "John 3:16, jn316, ps23…"
                placeholderTextColor: lookup.theme.muted
                selectByMouse: true
                leftPadding: Math.round(10 * lookup.controlScale); rightPadding: leftPadding
                background: Rectangle {
                    radius: Math.min(lookup.theme.cornerRadius, width / 2, height / 2)
                    color: lookup.theme.surface
                    border.color: query.activeFocus ? lookup.theme.accent : lookup.theme.border
                }
                onTextEdited: lookup.refresh()
                Keys.onDownPressed: lookup.move(1)
                Keys.onUpPressed: lookup.move(-1)
                Keys.onReturnPressed: lookup.show(lookup.current)
                Keys.onEnterPressed: lookup.show(lookup.current)
                Keys.onEscapePressed: lookup.close()
                Accessible.name: "Look up a passage"
            }
            Label {
                objectName: "lookupVersion"
                text: lookup.backend.versionAbbreviation
                textFormat: Text.PlainText
                color: lookup.theme.muted
                font.family: lookup.theme.fontFamily
                font.pixelSize: Math.round(12 * lookup.controlScale)
            }
        }
        ListView {
            id: candidateList
            objectName: "lookupCandidates"
            visible: !lookup.reading
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: Math.round(2 * lookup.controlScale)
            model: lookup.candidates
            currentIndex: lookup.current
            delegate: Rectangle {
                id: candidate
                required property var modelData
                required property int index
                objectName: "lookupCandidate:" + index
                width: candidateList.width
                implicitHeight: candidateColumn.implicitHeight + Math.round(12 * lookup.controlScale)
                radius: Math.min(lookup.theme.cornerRadius, width / 2, height / 2)
                color: index === lookup.current ? Qt.rgba(lookup.theme.accent.r, lookup.theme.accent.g, lookup.theme.accent.b, 0.11) : "transparent"
                Column {
                    id: candidateColumn
                    x: Math.round(10 * lookup.controlScale)
                    y: Math.round(6 * lookup.controlScale)
                    width: parent.width - 2 * x
                    spacing: Math.round(2 * lookup.controlScale)
                    Text {
                        width: parent.width
                        text: candidate.modelData.reference
                        textFormat: Text.PlainText
                        color: lookup.theme.foreground
                        font.family: lookup.theme.fontFamily
                        font.pixelSize: Math.round(14 * lookup.controlScale)
                        font.weight: Font.Medium
                    }
                    Text {
                        width: parent.width
                        text: lookup.preview(candidate.modelData)
                        textFormat: Text.PlainText
                        color: lookup.theme.muted
                        font.family: lookup.theme.fontFamily
                        font.pixelSize: Math.round(12 * lookup.controlScale)
                        elide: Text.ElideRight
                        maximumLineCount: 1
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: lookup.show(candidate.index)
                }
            }
            Label {
                anchors.centerIn: parent
                width: parent.width
                visible: candidateList.count === 0
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: query.text.trim().length ? "No passage matches “" + query.text.trim() + "”." : "Type a passage to look it up."
                color: lookup.theme.muted
                font.family: lookup.theme.fontFamily
                font.pixelSize: Math.round(13 * lookup.controlScale)
            }
        }
        Label {
            objectName: "lookupTitle"
            visible: lookup.reading
            Layout.fillWidth: true
            text: lookup.shown ? lookup.shown.reference + " · " + lookup.shown.abbreviation : ""
            textFormat: Text.PlainText
            color: lookup.theme.foreground
            font.family: lookup.theme.fontFamily
            font.pixelSize: Math.round(20 * lookup.controlScale)
            font.weight: Font.Medium
            elide: Text.ElideRight
        }
        Flickable {
            id: passageView
            objectName: "lookupPassage"
            visible: lookup.reading
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentHeight: passageText.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            activeFocusOnTab: true
            Keys.onPressed: function(event) {
                const plain = (event.modifiers & ~Qt.KeypadModifier) === Qt.NoModifier
                if (event.key === Qt.Key_Y) lookup.copy(!!(event.modifiers & Qt.ShiftModifier))
                else if (plain && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) lookup.openInReader()
                else if (plain && (event.key === Qt.Key_Backspace || event.key === Qt.Key_Slash)) lookup.back()
                else if (plain && event.key === Qt.Key_Escape) lookup.close()
                else if (plain && (event.key === Qt.Key_J || event.key === Qt.Key_Down)) passageView.contentY = Math.min(Math.max(0, passageView.contentHeight - passageView.height), passageView.contentY + 40 * lookup.controlScale)
                else if (plain && (event.key === Qt.Key_K || event.key === Qt.Key_Up)) passageView.contentY = Math.max(0, passageView.contentY - 40 * lookup.controlScale)
                else return
                event.accepted = true
            }
            Accessible.role: Accessible.StaticText
            Accessible.name: lookup.shown ? lookup.shown.reference : ""
            Accessible.description: passageText.text
            Text {
                id: passageText
                objectName: "lookupText"
                width: passageView.width
                text: lookup.shown ? lookup.passageText(lookup.shown) : ""
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: lookup.theme.foreground
                font.family: lookup.theme.fontFamily
                font.pixelSize: Math.round(16 * lookup.controlScale)
                lineHeight: 1.4
            }
        }
        Label {
            objectName: "lookupStatus"
            visible: text.length > 0
            Layout.fillWidth: true
            text: lookup.status
            textFormat: Text.PlainText
            color: lookup.theme.muted
            font.family: lookup.theme.fontFamily
            font.pixelSize: Math.round(12 * lookup.controlScale)
            wrapMode: Text.Wrap
        }
        HintRow {
            objectName: "lookupHints"
            Layout.fillWidth: true
            theme: lookup.theme
            pixelSize: Math.round(11 * lookup.controlScale)
            parts: lookup.reading
                ? [["Y", "copy", function() { lookup.copy(false) }], ["Shift+Y", "plain", function() { lookup.copy(true) }],
                   ["Return", "open in " + lookup.theme.appName, lookup.openInReader], ["Backspace", "back", lookup.back],
                   ["Escape", "close", function() { lookup.close() }]]
                : [[["Up", "Down"], "choose"], ["Return", "show", function() { lookup.show(lookup.current) }, lookup.candidates.length > 0],
                   ["Escape", "close", function() { lookup.close() }]]
        }
    }
}
