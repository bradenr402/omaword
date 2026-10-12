pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "TypeScale.js" as TypeScale
// qmllint disable import
import OmaWord.Reader 1.0
// qmllint enable import

// A chapter a selection reaches into, shown whole above or below the chapter
// read while the selection spans them (Main's context chapters): its title,
// its text in the layout chosen, and its selected verses tinted, as the
// reader shows them; marks and footnotes are left out. A click on a verse
// selects it there (shift: extends the selection to it).
Column {
    id: chapter
    // The reader window (Main), the live theme, and the backend it reads.
    required property var controller
    required property var theme
    required property var backend
    required property int book
    required property int chapterNumber
    required property string title
    required property var rows
    // The verses of this chapter in the selection.
    property int first: 0
    property int last: 0
    // Above the chapter read (room below it), or below it (room above).
    property bool before: true
    readonly property real readerScale: controller.readerScale
    readonly property real bodySize: controller.readerSize * readerScale
    readonly property bool focusLayout: !!controller.focusLayout
    readonly property bool numbers: backend.showVerseNumbers && !focusLayout
    readonly property bool headings: backend.showHeadings && !focusLayout
    readonly property bool continuous: !!controller.continuousReader
    readonly property real gap: 48 * readerScale
    topPadding: before ? 0 : gap
    bottomPadding: before ? gap : 0
    function selected(number) { return number >= first && number <= last }
    function verseHtml(row) {
        const html = controller.verseHtml ? controller.verseHtml(row.html) : row.html
        return '<div style="line-height:' + (backend.lineHeight * 100) + '%">' + html + '</div>'
    }

    Label {
        objectName: "contextTitle:" + chapter.title
        text: chapter.title
        opacity: chapter.controller.focusDimmedOpacity
        color: chapter.theme.foreground
        font.pixelSize: TypeScale.size(chapter.bodySize, TypeScale.title)
        font.weight: Font.Medium
        font.letterSpacing: -0.9 * chapter.controller.fontScale
        leftPadding: chapter.controller.verseGutter
        bottomPadding: 24 * chapter.readerScale * chapter.controller.fontScale
        width: chapter.width
        wrapMode: Text.Wrap
    }

    // Paragraph and Block: one flowing document, as BlockReader builds it.
    Item {
        id: flowing
        visible: chapter.continuous
        x: chapter.controller.verseGutter
        width: chapter.width - x - 9
        height: visible ? body.height : 0
        TextEdit {
            id: body
            objectName: "contextBody:" + chapter.title
            width: parent.width
            height: contentHeight
            horizontalAlignment: TextEdit.AlignLeft
            textFormat: TextEdit.RichText
            font.family: chapter.theme.fontFamily
            font.pixelSize: chapter.bodySize
            color: chapter.theme.foreground
            readOnly: true
            selectByMouse: false
            activeFocusOnTab: false
            wrapMode: TextEdit.Wrap
            padding: 0
            Accessible.name: chapter.title
        }
        // qmllint disable import
        BlockDocument { id: document }
        // qmllint enable import
        readonly property string buildKey: chapter.continuous ? JSON.stringify([chapter.rows.length, chapter.numbers, chapter.headings,
            chapter.backend.lineHeight, chapter.backend.readingMode, chapter.bodySize, chapter.controller.wordsOfJesusColor || ""]) : ""
        onBuildKeyChanged: build()
        onWidthChanged: refresh()
        Component.onCompleted: build()
        function build() {
            if (!chapter.continuous) return
            const em = chapter.bodySize
            const verses = Array.from(chapter.rows).map(function(row) {
                const html = chapter.controller.verseHtml ? chapter.controller.verseHtml(row.html) : row.html
                return html === row.html ? row : Object.assign({}, row, { html: html })
            })
            document.build(body.textDocument, verses, chapter.numbers, chapter.headings, chapter.backend.lineHeight,
                chapter.backend.readingMode, false, em, false, { headingAbove: TypeScale.size(em, TypeScale.headingAbove),
                    headingBelow: TypeScale.size(em, TypeScale.headingBelow), poetryIndent: em * TypeScale.poetryIndent })
            refresh()
        }
        // The selected verses' line boxes, refreshed after a build or a resize.
        property var selectionRects: []
        function refresh() {
            if (!chapter.continuous) return
            const spans = Array.from(document.spans).filter(function(span) { return chapter.selected(span.number) })
            selectionRects = spans.length ? Array.from(document.rectangles(spans[0].start, spans[spans.length - 1].end)) : []
            veil.requestPaint()
        }
        Connections {
            target: chapter
            function onFirstChanged() { flowing.refresh() }
            function onLastChanged() { flowing.refresh() }
            function onRowsChanged() { flowing.build() }
        }
        Repeater {
            model: flowing.selectionRects
            delegate: Rectangle {
                required property var modelData
                x: modelData.x - 5; y: modelData.y
                width: modelData.width + 5; height: modelData.height
                z: -1
                color: chapter.controller.tint(chapter.theme.accent, 0.22)
            }
        }
        // Focus mode: all but the selected verses recede, as the reader's do.
        Canvas {
            id: veil
            anchors.fill: body
            readonly property real alpha: 1 - chapter.controller.focusDimmedOpacity
            visible: alpha > 0
            onAlphaChanged: requestPaint()
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                ctx.fillStyle = Qt.rgba(chapter.theme.background.r, chapter.theme.background.g, chapter.theme.background.b, alpha)
                ctx.fillRect(0, 0, width, height)
                for (const r of flowing.selectionRects) ctx.clearRect(r.x - 5, r.y, r.width + 5, r.height)
            }
        }
        MouseArea {
            anchors.fill: body
            acceptedButtons: Qt.LeftButton
            onClicked: function(mouse) {
                const number = document.verseAtPoint(mouse.x, mouse.y)
                if (number > 0) chapter.controller.selectContextVerse(chapter.book, chapter.chapterNumber, number, !!(mouse.modifiers & Qt.ShiftModifier))
            }
        }
    }

    // Verse: a row per verse, as the reader's rows.
    Repeater {
        model: chapter.continuous ? [] : chapter.rows
        delegate: Item {
            id: row
            required property var modelData
            required property int index
            readonly property bool chosen: chapter.selected(modelData.number)
            readonly property bool hasHeading: chapter.headings && (modelData.heading || "").length > 0
            objectName: "contextVerse:" + chapter.title + ":" + modelData.number
            width: chapter.width
            height: heading.height + rowBody.height + 7 * chapter.readerScale
            opacity: row.chosen ? 1 : chapter.controller.focusDimmedOpacity
            Item {
                id: heading
                x: chapter.controller.verseGutter
                width: parent.width - x - 6
                height: row.hasHeading ? headingText.implicitHeight + headingText.y + TypeScale.size(chapter.bodySize, TypeScale.headingBelow) : 0
                visible: row.hasHeading
                Text {
                    id: headingText
                    width: parent.width
                    y: row.index === 0 ? 0 : TypeScale.size(chapter.bodySize, TypeScale.headingAbove)
                    text: chapter.controller.leftToRightText(row.modelData.heading)
                    horizontalAlignment: Text.AlignLeft
                    textFormat: Text.PlainText
                    font.family: chapter.theme.fontFamily
                    font.pixelSize: TypeScale.size(chapter.bodySize, TypeScale.heading)
                    font.weight: Font.DemiBold
                    wrapMode: Text.Wrap
                    color: chapter.theme.foreground
                }
            }
            Rectangle {
                x: chapter.controller.verseGutter - 9 * chapter.readerScale
                readonly property var previous: row.index > 0 ? chapter.rows[row.index - 1] : null
                y: previous && chapter.selected(previous.number) ? 0 : heading.height
                width: parent.width - x
                height: (row.modelData.number < chapter.last ? row.height : heading.height + rowBody.height + 3) - y
                visible: row.chosen
                color: chapter.controller.tint(chapter.theme.accent, 0.22)
            }
            Label {
                id: numberLabel
                visible: chapter.numbers
                // Where the reader puts its numbers: right-aligned 8 px before the gutter's lane at 71.
                x: 54 * chapter.readerScale + 17 * chapter.readerScale - 8 * chapter.readerScale - width
                y: heading.height + Math.max(0, bodyMetrics.ascent + bodyMetrics.tightBoundingRect("H").y
                    - numberMetrics.ascent - numberMetrics.tightBoundingRect("0").y)
                width: 12 * chapter.readerScale
                horizontalAlignment: Text.AlignRight
                fontSizeMode: Text.HorizontalFit
                minimumPixelSize: 1
                text: row.modelData.number
                font.pixelSize: TypeScale.size(chapter.bodySize, TypeScale.number)
                font.weight: row.chosen ? Font.Bold : Font.Normal
                color: row.chosen ? chapter.theme.accent : chapter.theme.muted
                FontMetrics { id: numberMetrics; font: numberLabel.font }
            }
            TextEdit {
                id: rowBody
                x: chapter.controller.verseGutter
                y: heading.height
                width: parent.width - x - 9
                height: contentHeight
                text: chapter.verseHtml(row.modelData)
                textFormat: TextEdit.RichText
                horizontalAlignment: TextEdit.AlignLeft
                font.family: chapter.theme.fontFamily
                font.pixelSize: chapter.bodySize
                color: chapter.theme.foreground
                readOnly: true
                selectByMouse: false
                activeFocusOnTab: false
                wrapMode: TextEdit.Wrap
                padding: 0
                Accessible.name: "Verse " + row.modelData.number + ": " + row.modelData.text
            }
            // Poetry lines indented by level, as in the reader.
            SearchHighlights {
                body: rowBody
                sourceText: row.modelData.text
                lineIndents: row.modelData.indents || []
                indentUnit: chapter.bodySize * TypeScale.poetryIndent
                theme: chapter.theme
            }
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                onClicked: function(mouse) {
                    chapter.controller.selectContextVerse(chapter.book, chapter.chapterNumber, row.modelData.number, !!(mouse.modifiers & Qt.ShiftModifier))
                }
            }
        }
    }
    FontMetrics { id: bodyMetrics; font.family: chapter.theme.fontFamily; font.pixelSize: chapter.bodySize }
}
