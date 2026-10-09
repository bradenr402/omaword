pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import "KeyNotation.js" as KeyNotation

// Look up (omaword lookup): a small window opened where you are, for a
// passage without going to OmaWord. Type a reference (compact forms too:
// jn316, ps23) and enter shows it; there y copies it (shift Y the plain
// text) and enter opens it in your reading window. Its size is fixed, which
// Hyprland floats. Its own backend reads the version you read, or the one
// asked for, and saves nothing; with others installed, tab and shift tab
// (or a click on the version) read another, and enter opens the passage
// in that version.
Window {
    id: lookup
    objectName: "lookupWindow"
    // The live theme (Desktop), its own BibleBackend, and the sound effects.
    required property var theme
    required property var backend
    required property var effects
    property string initialText: ""
    // versionId: the version chosen here, empty when it is the one it opened with.
    signal openRequested(string reference, string versionId)
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
    // The installed versions in their order when it opened (the backend
    // lists the one it reads first), for tab to step through.
    property var versionOrder: []
    property string initialVersion: ""
    property string chosenVersion: ""
    readonly property bool choosable: versionOrder.length > 1

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
        openRequested(shown.reference, chosenVersion)
        close()
    }
    // Another version: the list and the passage shown follow at once.
    function readVersion(id) {
        if (id === lookup.backend.versionId) return
        if (!lookup.backend.readVersion(id)) { status = lookup.backend.error; lookup.effects.play("error"); return }
        chosenVersion = id === initialVersion ? "" : id
        const index = current
        refresh()
        current = Math.max(0, Math.min(index, candidates.length - 1))
        if (!shown) return
        const passage = lookup.backend.readPassage(shown.reference)
        if (!passage.error) { shown = passage; status = ""; return }
        // Its name for the book may not be this version's: the typed text's best match.
        if (candidates.length) show(0)
        else { back(); status = passage.error }
    }
    function cycleVersion(delta) {
        if (!choosable) { lookup.effects.play("error"); return }
        const at = versionOrder.findIndex(function(row) { return row.id === lookup.backend.versionId })
        readVersion(versionOrder[(at + delta + versionOrder.length) % versionOrder.length].id)
    }
    function openVersionMenu() {
        const at = versionControl.mapToItem(null, 0, versionControl.height)
        versionMenu.show("lookupVersion", versionOrder.map(function(row) {
            return { id: row.id, text: (row.abbreviation ? row.abbreviation + " · " : "") + row.name,
                shortcut: row.id === lookup.backend.versionId ? "reading" : "", run: function() { lookup.readVersion(row.id) } }
        }), at.x, at.y)
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
        versionOrder = Array.from(lookup.backend.versions)
        initialVersion = lookup.backend.versionId
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
            spacing: Math.round(10 * lookup.controlScale)
            TextField {
                id: query
                objectName: "lookupQuery"
                visible: !lookup.reading
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
                Keys.onTabPressed: lookup.cycleVersion(1)
                Keys.onBacktabPressed: lookup.cycleVersion(-1)
                Accessible.name: "Look up a passage"
            }
            Label {
                objectName: "lookupTitle"
                visible: lookup.reading
                Layout.fillWidth: true
                text: lookup.shown ? lookup.shown.reference : ""
                textFormat: Text.PlainText
                color: lookup.theme.foreground
                font.family: lookup.theme.fontFamily
                font.pixelSize: Math.round(20 * lookup.controlScale)
                font.weight: Font.Medium
                elide: Text.ElideRight
            }
            // The version it reads; with others installed, a click lists them.
            Label {
                id: versionControl
                objectName: "lookupVersion"
                readonly property bool choosable: lookup.choosable
                text: lookup.backend.versionAbbreviation
                textFormat: Text.PlainText
                color: choosable && versionMouse.containsMouse ? lookup.theme.foreground : lookup.theme.muted
                font.family: lookup.theme.fontFamily
                font.pixelSize: Math.round(12 * lookup.controlScale)
                leftPadding: Math.round(8 * lookup.controlScale); rightPadding: leftPadding
                topPadding: Math.round(5 * lookup.controlScale); bottomPadding: topPadding
                background: Rectangle {
                    visible: versionControl.choosable && versionMouse.containsMouse
                    color: lookup.theme.surface
                    radius: Math.min(lookup.theme.cornerRadius, width / 2, height / 2)
                }
                MouseArea {
                    id: versionMouse
                    anchors.fill: parent
                    enabled: versionControl.choosable
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: lookup.openVersionMenu()
                }
                AppToolTip {
                    theme: lookup.theme
                    uiScale: lookup.controlScale
                    visible: versionMouse.containsMouse
                    text: "Choose a version · " + KeyNotation.format("Tab") + " next · " + KeyNotation.format("Shift+Tab") + " previous"
                }
                Accessible.role: choosable ? Accessible.Button : Accessible.StaticText
                Accessible.name: "Version " + text
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
                else if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) lookup.cycleVersion(event.key === Qt.Key_Tab ? 1 : -1)
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
            readonly property var version: lookup.choosable ? ["Tab", "version", function() { lookup.cycleVersion(1) }] : null
            parts: lookup.reading
                ? [["Y", "copy", function() { lookup.copy(false) }], ["Shift+Y", "plain", function() { lookup.copy(true) }],
                   ["Return", "open in " + lookup.theme.appName, lookup.openInReader], ["Backspace", "back", lookup.back], version,
                   ["Escape", "close", function() { lookup.close() }]]
                : [[["Up", "Down"], "choose"], ["Return", "show", function() { lookup.show(lookup.current) }, lookup.candidates.length > 0],
                   version, ["Escape", "close", function() { lookup.close() }]]
        }
    }
    AppContextMenu {
        id: versionMenu
        objectName: "lookupVersionMenu"
        theme: lookup.theme
        uiScale: lookup.controlScale
        // No owner's catcher here: a press outside closes it.
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    }
}
