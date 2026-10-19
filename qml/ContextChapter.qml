pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "TypeScale.js" as TypeScale

// A chapter a selection reaches into, shown whole above or below the chapter
// read while the selection spans them (Main's context chapters): its title,
// its text in the layout chosen, with its marks and footnotes, and its
// selected verses underlined, as the reader shows them. A click on a verse
// selects it there (shift: extends the selection to it), its chapter
// becoming the one read (Main.besideClick).
Column {
    id: chapter
    // The reader window (Main), the live theme, the backend it reads, the
    // reader's view, and the menu of ranges starting on one verse.
    required property var controller
    required property var theme
    required property var backend
    required property Flickable viewport
    required property var rangeMenu
    required property int book
    required property int chapterNumber
    required property string title
    // Its rows with their marks (bible.chapterRows).
    required property var rows
    // The verses of this chapter in the selection.
    property int first: 0
    property int last: 0
    // Above the chapter read (room below it), or below it (room above).
    property bool before: true
    // Parked (the whole book's, far from the view): only its room, the
    // height it had, nothing laid out.
    property bool parked: false
    property real parkedHeight: 0
    height: parked ? parkedHeight : implicitHeight
    readonly property real readerScale: controller.readerScale
    readonly property real bodySize: controller.readerSize * readerScale
    readonly property bool focusLayout: !!controller.focusLayout
    readonly property bool numbers: backend.showVerseNumbers && !focusLayout
    readonly property bool headings: backend.showHeadings && !focusLayout
    readonly property bool continuous: !!controller.continuousReader
    readonly property real gap: 48 * readerScale
    // Its items' names, apart from the chapter read's.
    readonly property string namePrefix: "context:" + title + ":"
    // The Verse layout's rail lanes for its rows.
    readonly property var lanes: controller.packAnnotationLanes(rows)
    topPadding: before ? 0 : gap
    bottomPadding: before ? gap : 0
    function selected(number) { return number >= first && number <= last }
    function verseHtml(row) {
        const html = controller.footnoteHtml(controller.verseHtml ? controller.verseHtml(row.html) : row.html, row.footnotes)
        return '<div style="line-height:' + (backend.lineHeight * 100) + '%">' + html + '</div>'
    }
    function click(number, mouse, item) {
        const point = item.mapToItem(null, mouse.x, mouse.y)
        controller.besideClick(book, chapterNumber, number, mouse.button, mouse.modifiers, point.x, point.y)
    }
    // Its layout settled now (Main measures beside it): the flowing layouts'
    // reader, and its own rows and height (a chapter coming back from parked
    // would otherwise keep a stale height until the next polish).
    function settle() { if (flowing.item) (flowing.item as BlockReader).settle(); forceLayout() }
    // Where each verse's text is: {number, top, bottom}, in this item.
    function verseExtents() {
        const out = []
        const reader = flowing.item as BlockReader
        if (reader) {
            const offset = reader.body.mapToItem(chapter, 0, 0).y
            for (const extent of reader.verseExtents()) out.push({ number: extent.number, top: offset + extent.top, bottom: offset + extent.bottom })
            return out
        }
        for (let i = 0; i < verseRows.count; ++i) {
            const item = verseRows.itemAt(i)
            if (item) out.push({ number: item.number, top: item.y + item.textTop, bottom: item.y + item.textTop + item.textHeight })
        }
        return out
    }

    Label {
        objectName: "contextTitle:" + chapter.title
        visible: !chapter.parked
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

    // Paragraph and Block: the reader's own flowing layout, beside.
    Loader {
        id: flowing
        objectName: "contextFlow:" + chapter.title
        active: chapter.continuous && !chapter.parked
        visible: active
        x: chapter.controller.verseGutter
        width: chapter.width - x - 9
        sourceComponent: BlockReader {
            beside: true
            namePrefix: chapter.namePrefix
            controller: chapter.controller
            backend: chapter.backend
            theme: chapter.theme
            viewport: chapter.viewport
            mode: chapter.backend.readingMode
            verses: chapter.rows
            book: chapter.book
            chapterNumber: chapter.chapterNumber
            selectedFirst: chapter.first
            selectedLast: chapter.last
        }
    }

    // Verse: a row per verse, as the reader's rows; read in place, so new
    // marks update the rows rather than making them again.
    Repeater {
        id: verseRows
        model: chapter.continuous || chapter.parked ? 0 : chapter.rows.length
        delegate: Item {
            id: row
            required property int index
            readonly property var modelData: chapter.rows[index] || ({ number: 0, text: "", html: "" })
            readonly property int number: modelData.number
            readonly property real textTop: heading.height
            readonly property real textHeight: rowBody.height
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
            // Tag and note rails, made only for rows they mark, above the
            // row's click area.
            Loader {
                z: 2
                active: Array.from(row.modelData.tagRanges || []).length > 0
                sourceComponent: RangeMarker {
                    objectName: chapter.namePrefix + "verseTagMarker" + row.modelData.number
                    controller: chapter.controller; theme: chapter.theme; backend: chapter.backend; rangeMenu: chapter.rangeMenu
                    kind: "tag"; number: row.modelData.number; rowIndex: row.index; lanes: chapter.lanes
                    ranges: Array.from(row.modelData.tagRanges || [])
                    headingHeight: heading.height; bodyHeight: rowBody.height
                    height: row.height
                }
            }
            Loader {
                z: 2
                active: Array.from(row.modelData.noteRanges || []).length > 0
                sourceComponent: RangeMarker {
                    objectName: chapter.namePrefix + "verseNoteMarker" + row.modelData.number
                    controller: chapter.controller; theme: chapter.theme; backend: chapter.backend; rangeMenu: chapter.rangeMenu
                    kind: "note"; number: row.modelData.number; rowIndex: row.index; lanes: chapter.lanes
                    ranges: Array.from(row.modelData.noteRanges || [])
                    headingHeight: heading.height; bodyHeight: rowBody.height
                    height: row.height
                    visible: ranges.length > 0 && !chapter.backend.focusMode && chapter.controller.inlineNoteMarks
                }
            }
            VerseTint {
                objectName: chapter.namePrefix + "annotationTint" + row.modelData.number
                controller: chapter.controller; theme: chapter.theme
                row: row.modelData
                previous: row.index > 0 ? chapter.rows[row.index - 1] : null
                next: chapter.rows[row.index + 1] || null
                headingHeight: heading.height
                bodyHeight: rowBody.height
                versionId: chapter.backend.versionId
                focusMode: chapter.backend.focusMode
                x: chapter.controller.verseGutter - 9 * chapter.readerScale
                width: row.width - x
                height: row.height
            }
            VerseHighlights {
                controller: chapter.controller
                namePrefix: chapter.namePrefix
                row: row.modelData
                versionId: chapter.backend.versionId
                gutter: chapter.controller.verseGutter
                rowWidth: row.width
                rowHeight: row.height
                headingHeight: heading.height
                body: rowBody
                lines: rowLines
            }
            // Its selected verses: a range's (the focused verse is the chapter
            // read's), underlined with dots.
            Loader {
                active: row.chosen && chapter.controller.rangeUnderlined
                sourceComponent: DottedUnderline {
                    objectName: chapter.namePrefix + "selectionUnderline" + row.modelData.number
                    ink: chapter.theme.accent
                    dot: chapter.controller.underlineDot
                    lines: {
                        const watch = [rowBody.width, rowBody.height, rowBody.y, rowBody.text, chapter.controller.underlineTop]
                        return !watch ? [] : Array.from(rowLines.lineRects(0, String(row.modelData.text || "").length)).map(function(r) {
                            return { x: rowBody.x + r.x, y: rowBody.y + r.y + chapter.controller.underlineTop, width: r.width }
                        })
                    }
                }
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
            FootnoteMarks {
                controller: chapter.controller; theme: chapter.theme
                namePrefix: chapter.namePrefix
                row: row.modelData
                body: rowBody
                // Its verse is selected first, its chapter becoming the one read.
                onActivated: function(note, x, y, height) {
                    chapter.controller.selectContextVerse(chapter.book, chapter.chapterNumber, row.modelData.number, false)
                    chapter.controller.peekFootnote(row.modelData.number, note.seq, x, y, height)
                }
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
            // Poetry lines indented by level, as in the reader; its line boxes
            // place the highlights.
            SearchHighlights {
                id: rowLines
                x: rowBody.x; y: rowBody.y
                width: rowBody.width; height: rowBody.height
                body: rowBody
                sourceText: row.modelData.text
                lineIndents: row.modelData.indents || []
                indentUnit: chapter.bodySize * TypeScale.poetryIndent
                theme: chapter.theme
            }
            MouseArea {
                id: rowMouse
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
                onClicked: function(mouse) { chapter.click(row.modelData.number, mouse, rowMouse) }
            }
        }
    }
    FontMetrics { id: bodyMetrics; font.family: chapter.theme.fontFamily; font.pixelSize: chapter.bodySize }
}
