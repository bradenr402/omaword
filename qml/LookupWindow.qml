pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import QtCore
import "KeyNotation.js" as KeyNotation

// Look up (omaword lookup): a small window opened where you are, for a
// passage without going to OmaWord. Type a reference (compact forms too:
// jn316, ps23) and enter shows it; there y copies it (shift Y the plain
// text) and enter opens it in your reading window. It can be resized, and
// opens at the size it had last (a window rule floats it: Hyprland tiles a
// window it may resize). Its own backend reads the version you read, or the one
// asked for, and saves nothing; with others installed, tab and shift tab
// (or the version button) read another, and enter opens the passage in
// that version.
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
    minimumWidth: Math.round(360 * controlScale)
    minimumHeight: Math.round(260 * controlScale)
    // Its size, read and written straight through (no Settings properties:
    // those would write their defaults back as a lookup closes).
    Settings {
        id: size
        category: "lookup"
        location: StandardPaths.writableLocation(StandardPaths.AppConfigLocation) + "/lookup.ini"
    }
    property bool sizeKept: false
    function restoreSize() {
        const width = Number(size.value("width", 0)), height = Number(size.value("height", 0))
        if (width > 0 && height > 0) { lookup.width = width; lookup.height = height }
        sizeKept = true
    }
    onWidthChanged: if (sizeKept) size.setValue("width", width)
    onHeightChanged: if (sizeKept) size.setValue("height", height)
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
    function tint(color, alpha) { return Qt.rgba(color.r, color.g, color.b, alpha) }

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
        // A verse the version joined to another is read as the one holding it, and says so.
        status = row.note || passage.note || ""
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
    // Another version: the list and the passage shown follow at once, the
    // same passages by book, chapter, and verse (another version may name
    // the book otherwise: Genesis, Bereshis).
    function readVersion(id) {
        if (id === lookup.backend.versionId) return
        const highlighted = candidates[current] || null
        if (!lookup.backend.readVersion(id)) { status = lookup.backend.error; lookup.effects.play("error"); return }
        chosenVersion = id === initialVersion ? "" : id
        const index = current
        refresh()
        const same = highlighted ? candidates.findIndex(function(row) {
            return row.book === highlighted.book && row.chapter === highlighted.chapter && row.start === highlighted.start && row.end === highlighted.end
        }) : -1
        current = same >= 0 ? same : Math.max(0, Math.min(index, candidates.length - 1))
        if (!shown) return
        const passage = lookup.backend.readScope(shown.scope)
        if (!passage.error) { shown = passage; status = ""; return }
        back(); status = passage.error
    }
    function cycleVersion(delta) {
        if (!choosable) { lookup.effects.play("error"); return }
        const at = versionOrder.findIndex(function(row) { return row.id === lookup.backend.versionId })
        readVersion(versionOrder[(at + delta + versionOrder.length) % versionOrder.length].id)
    }
    // The version button: with two versions a click reads the other; with
    // more it lists them (a dropdown).
    function chooseVersion() {
        if (versionOrder.length > 2) openVersionMenu()
        else cycleVersion(1)
    }
    // Under the button, its right edge on the button's, the version read current.
    function openVersionMenu() {
        const at = versionControl.mapToItem(null, versionControl.width, versionControl.height + Math.round(4 * controlScale))
        versionMenu.show("lookupVersion", versionOrder.map(function(row) {
            return { id: row.id, text: (row.abbreviation ? row.abbreviation + " · " : "") + row.name,
                shortcut: row.id === lookup.backend.versionId ? "reading" : "", run: function() { lookup.readVersion(row.id) } }
        }), at.x, at.y)
        versionMenu.x = Math.max(0, Math.round(at.x - versionMenu.width))
        versionMenu.currentIndex = Math.max(0, versionOrder.findIndex(function(row) { return row.id === lookup.backend.versionId }))
    }
    function preview(row) {
        const passage = lookup.backend.readPassage(row.reference)
        return passage.error ? "" : passage.plain.replace(/\s+/g, " ")
    }

    Component.onCompleted: {
        restoreSize()
        // Copies of the rows: the list's own rows would read the backend's
        // list as it is later, which puts the version read first.
        versionOrder = Array.from(lookup.backend.versions).map(function(row) {
            return { id: String(row.id), abbreviation: String(row.abbreviation || ""), name: String(row.name || "") }
        })
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
            // The version it reads. With others installed it is a button,
            // framed like the input beside it, its glyph saying what a click
            // does: with two versions it switches (⇄) to the other, with more
            // it opens a list (a chevron); a right click, like tab, reads the
            // next. With one it is only its name.
            Label {
                id: versionControl
                objectName: "lookupVersion"
                readonly property bool choosable: lookup.choosable
                readonly property bool dropdown: lookup.versionOrder.length > 2
                readonly property real gap: Math.round(10 * lookup.controlScale)
                readonly property var other: lookup.versionOrder.find(function(row) { return row.id !== lookup.backend.versionId }) || null
                readonly property string tip: dropdown
                    ? "Choose a version · " + KeyNotation.format("RightClick") + " or " + KeyNotation.format("Tab") + " next · " + KeyNotation.format("Shift+Tab") + " previous"
                    : other ? "Switch to " + (other.abbreviation || other.name) + " · " + KeyNotation.format("Tab") : ""
                text: lookup.backend.versionAbbreviation
                textFormat: Text.PlainText
                color: choosable ? lookup.theme.foreground : lookup.theme.muted
                font.family: lookup.theme.fontFamily
                font.pixelSize: Math.round(12 * lookup.controlScale)
                // As tall as the input beside it, its name centred.
                Layout.preferredHeight: query.implicitHeight
                verticalAlignment: Text.AlignVCenter
                leftPadding: gap
                rightPadding: choosable ? gap + versionGlyph.width + Math.round(6 * lookup.controlScale) : gap
                background: Rectangle {
                    objectName: "lookupVersionFrame"
                    visible: versionControl.choosable
                    radius: Math.min(lookup.theme.cornerRadius, width / 2, height / 2)
                    color: versionMouse.pressed ? lookup.tint(lookup.theme.accent, 0.18)
                        : versionMouse.containsMouse ? lookup.tint(lookup.theme.accent, 0.09) : lookup.theme.surface
                    border.color: lookup.theme.border
                }
                Canvas {
                    id: versionGlyph
                    objectName: "lookupVersionGlyph"
                    visible: versionControl.choosable
                    anchors.right: parent.right
                    anchors.rightMargin: versionControl.gap
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.round(12 * lookup.controlScale)
                    height: width
                    property color ink: lookup.theme.foreground
                    readonly property string glyph: versionControl.dropdown ? "chevron-down" : "swap"
                    onInkChanged: requestPaint()
                    onGlyphChanged: requestPaint()
                    onPaint: {
                        const ctx = getContext("2d")
                        ctx.reset()
                        ctx.scale(width / 24, height / 24)
                        ctx.strokeStyle = ink
                        ctx.lineWidth = 2.6
                        ctx.lineCap = "square"
                        ctx.lineJoin = "miter"
                        ctx.beginPath()
                        if (glyph === "chevron-down") {
                            ctx.moveTo(5, 9); ctx.lineTo(12, 16); ctx.lineTo(19, 9)
                        } else {
                            // Two arrows, one each way: to the other version and back.
                            ctx.moveTo(4, 7); ctx.lineTo(19, 7); ctx.moveTo(15, 3); ctx.lineTo(19, 7); ctx.lineTo(15, 11)
                            ctx.moveTo(20, 17); ctx.lineTo(5, 17); ctx.moveTo(9, 13); ctx.lineTo(5, 17); ctx.lineTo(9, 21)
                        }
                        ctx.stroke()
                    }
                }
                MouseArea {
                    id: versionMouse
                    anchors.fill: parent
                    enabled: versionControl.choosable
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    // A disabled MouseArea still sets the cursor.
                    cursorShape: versionControl.choosable ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: function(mouse) {
                        if (mouse.button === Qt.RightButton) lookup.cycleVersion(1)
                        else lookup.chooseVersion()
                    }
                }
                AppToolTip {
                    theme: lookup.theme
                    uiScale: lookup.controlScale
                    visible: versionMouse.containsMouse && !versionMenu.visible
                    text: versionControl.tip
                }
                Accessible.role: choosable ? Accessible.Button : Accessible.StaticText
                Accessible.name: "Version " + text
                Accessible.description: choosable ? tip : ""
                Accessible.onPressAction: if (choosable) lookup.chooseVersion()
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
                text: lookup.shown ? lookup.shown.body : ""
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
        // No owner's catcher here: a press outside closes it and acts on
        // nothing else (modal), so a click on the version button closes
        // the list rather than opening it again.
        modal: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    }
}
