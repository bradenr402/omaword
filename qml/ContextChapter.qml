pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "TypeScale.js" as TypeScale

// A chapter a selection reaches into, shown whole above or below the chapter
// read while the selection spans them (Main's context chapters), and the
// whole book's chapters: its title, and its chapter view (Main's chapter
// views: a VerseView or a BlockReader, beside), with its marks and footnotes
// and its selected verses underlined, as the reader shows them. A click on
// a verse selects it there (shift: extends the selection to it), its
// chapter becoming the one read (Main.besideClick).
Column {
    id: chapter
    // The reader window (Main), which makes its chapter view, and the live theme.
    required property var controller
    required property var theme
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
    readonly property bool continuous: !!controller.continuousReader
    readonly property real gap: 48 * readerScale
    // Its items' names, apart from the chapter read's.
    readonly property string namePrefix: "context:" + title + ":"
    topPadding: before ? 0 : gap
    bottomPadding: before ? gap : 0
    // Its view's rows and its own height laid out now: a Column lays out on
    // its own polish, which the reader's column does not reach (a chapter
    // given its view, or back from parked, would keep a stale height).
    // (A Verse view's rows showing given a new text size or width first.)
    function layOut() {
        if (viewHost.item && !chapter.continuous) { viewHost.item.settle(); viewHost.item.forceLayout() }
        forceLayout()
    }
    // That, and a flowing view's pending build (Main measures beside it).
    function settle() {
        if (viewHost.item && chapter.continuous) (viewHost.item as BlockReader).settle()
        layOut()
    }
    // Where each verse's text is: {number, top, bottom}, in this item.
    function verseExtents() {
        const view = viewHost.item
        if (!view) return []
        // The flowing layouts measure in their text's coordinates.
        const offset = (chapter.continuous ? (view as BlockReader).body : view).mapToItem(chapter, 0, 0).y
        return Array.from(view.verseExtents()).map(function(extent) { return { number: extent.number, top: offset + extent.top, bottom: offset + extent.bottom } })
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

    // Its chapter view (the chapter read's, when this one was), unless parked.
    // The flowing layouts' text starts at the gutter; a VerseView's rows
    // span the column, their numbers in it.
    Item {
        id: viewHost
        objectName: "contextView:" + chapter.title
        property Item item: null
        visible: !chapter.parked
        x: chapter.continuous ? chapter.controller.verseGutter : 0
        width: chapter.continuous ? chapter.width - x - 9 : chapter.width
        height: item && !chapter.parked ? item.height : 0
    }
    readonly property Item view: viewHost.item
    // A view of the layout read in, while not parked.
    function updateView() {
        const key = chapter.parked || chapter.book <= 0 ? "" : controller.chapterViewKey(chapter.book, chapter.chapterNumber)
        if (viewHost.item && viewHost.item.viewKey !== key) { viewHost.item.destroy(); viewHost.item = null }
        if (key && !viewHost.item) viewHost.item = controller.adoptChapterView(chapter, viewHost)
        layOut()
    }
    onParkedChanged: updateView()
    onContinuousChanged: updateView()
    // Rows of another version or text: its view shows them, keyed so.
    onRowsChanged: if (viewHost.item) viewHost.item.viewKey = controller.chapterViewKey(chapter.book, chapter.chapterNumber)
    // Gives its view up (to the chapter read, or the shelf).
    function takeView() {
        const view = viewHost.item
        viewHost.item = null
        return view
    }
    Component.onCompleted: updateView()
    // Views belong to the window (Main's viewShelf), so one taken by the
    // chapter read outlives this; one still here goes with it.
    Component.onDestruction: if (viewHost.item) viewHost.item.destroy()
}
