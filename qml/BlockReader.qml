import QtQuick
import QtQuick.Controls
import "HandDrawn.js" as HandDrawn
import "KeyNotation.js" as KeyNotation
import "TypeScale.js" as TypeScale
// Registered by blockdocument.cpp at application startup, without a QML plugin.
// qmllint disable import
import OmaWord.Reader 1.0
// qmllint enable import

Item {
    id: root
    property bool destroying: false
    // After the geometry refresh that follows a rebuild: margin users re-measure.
    signal layoutRefreshed()
    property bool layoutDirty: false
    required property var controller
    required property var backend
    required property var theme
    required property Flickable viewport
    property string mode: "block"
    property alias body: body
    property alias mapping: document
    // The chapter shown: the one read (the backend's), or one shown beside it
    // (ContextChapter, beside: its rows and selected verses). Beside, it only
    // shows: a click selects through the controller (selectContextVerse), and
    // the reader's motion, search, and focus state (all of its own chapter)
    // stay off.
    property bool beside: false
    // Main's chapter views: the chapter this view shows (Main.chapterViewKey).
    property string viewKey: ""
    property var verses: backend.verses
    property int book: backend.book
    property int chapterNumber: backend.chapter
    property int selectedFirst: controller.charMode ? 0 : backend.selectedStart
    property int selectedLast: backend.selectedEnd
    // The focused verse is tinted (beside: none, it is the chapter read's);
    // a range's verses, and text selected by character, are underlined with
    // dots over a faint wash, and character selection's focused character is
    // tinted as a focused verse (Main's cursorTinted, rangeUnderlined, and
    // charCursor; controllers without them, tests: a selection of several
    // verses is a range).
    readonly property int tintedVerse: beside || controller.cursorTinted === false ? 0 : backend.cursorVerse || 0
    readonly property bool rangeUnderlined: controller.rangeUnderlined === undefined ? selectedLast > selectedFirst : !!controller.rangeUnderlined
    readonly property var charCursor: beside || !controller.charMode ? null : controller.charCursor
    readonly property color focusTint: controller.focusTint !== undefined ? controller.focusTint : controller.tint(theme.accent, 0.22)
    readonly property color rangeTint: controller.rangeTint !== undefined ? controller.rangeTint : controller.tint(theme.accent, 0.08)
    onTintedVerseChanged: if (!destroying) selectionTimer.restart()
    onRangeUnderlinedChanged: if (!destroying) selectionTimer.restart()
    onCharCursorChanged: if (!destroying) selectionTimer.restart()
    // Names its items apart from the chapter read's ("context:John 3:").
    property string namePrefix: ""
    property var annotationRanges: []
    property var inlineMarkers: []
    readonly property var searchSegments: beside ? [] : controller.searchSegments || []
    // As a value: an equal list (the view moving beside the chapter read, or
    // back) repaints nothing.
    readonly property string searchKey: JSON.stringify(searchSegments)
    onSearchKeyChanged: if (!destroying && searchLoader.item) searchLoader.item.requestPaint()
    readonly property real cornerRadius: theme.cornerRadius === undefined ? 0 : theme.cornerRadius
    // Source callers that are letters or digits show as-is; "+"/"-" become a dagger.
    function footnoteMark(caller) { return /^[A-Za-z0-9]{1,3}$/.test(caller || "") ? caller : "\u2020" }
    function tagsPlainText(tags) { return Array.from(tags || []).map(function(t) { return t.name }).join(", ") }
    property real annotationScale: 0
    // Reader text and marks follow the desktop scale; a controller without
    // readerScale (tests) falls back to its plain scale.
    readonly property real readerScale: controller.readerScale > 0 ? controller.readerScale
        : controller.scale > 0 ? controller.scale : 1
    readonly property string annotationKey: JSON.stringify(verses) + JSON.stringify(backend.collection)
        + book + ":" + chapterNumber
    onAnnotationKeyChanged: if (!destroying) geometryTimer.restart()
    function updateAnnotations() {
        if (destroying) return
        const ranges = document.annotations(verses, backend.collection, book, chapterNumber)
        const markers = document.inlineDecorations()
        if (JSON.stringify(markers) !== JSON.stringify(inlineMarkers)) inlineMarkers = markers
        if (JSON.stringify(ranges) === JSON.stringify(annotationRanges) && annotationScale === root.readerScale) return
        annotationScale = root.readerScale
        annotationRanges = ranges
    }
    function openAnnotation(entry) {
        if (entry.kind === "footnote") { controller.peekFootnote(entry.verse, entry.seq); return }
        if (entry.kind === "tag") controller.openPassageTags(entry.scope || entry.reference)
        else {
            if (!controller.flushNote()) return
            const scope = entry.scope
            if (scope && backend.openTaggedPassage(scope)) {
                controller.clearTextSelection()
                controller.syncNote()
                controller.openNotes(true)
            }
        }
    }
    // The layout's focus state: the controller's focusLayout (flipped once per
    // focus transition, see Main.applyFocusLayout), else the backend's.
    readonly property bool focusLayout: controller.focusLayout === undefined ? backend.focusMode : !!controller.focusLayout
    // Margin notes shown: rebuild without note cells (one reflow per toggle)
    // rather than leave blank, unpainted gaps in the text. Focus mode builds
    // no annotation cells at all, so margins coming or going there never
    // rebuild.
    readonly property bool noteMarks: focusLayout || controller.inlineNoteMarks === undefined || !!controller.inlineNoteMarks
    // Annotation tint layers per verse (HandDrawn.tintCount), none in focus mode.
    readonly property var tintCounts: backend.focusMode ? [] : Array.from(verses).map(function(v) { return HandDrawn.tintCount(v) })
    readonly property bool numbers: backend.showVerseNumbers && !focusLayout
    readonly property bool headings: backend.showHeadings && !focusLayout
    // Display copies of the verses: words of Jesus colored (format only).
    function displayVerses() {
        if (!controller.verseHtml) return verses
        return Array.from(verses).map(function(v) {
            const html = controller.verseHtml(v.html)
            if (html === v.html) return v
            const copy = Object.assign({}, v); copy.html = html; return copy
        })
    }
    // Note text is not layout: a typed note repaints the marks, never rebuilds.
    readonly property string contentKey: JSON.stringify(Array.from(verses).map(function(v) {
        return { number: v.number, html: v.html, text: v.text, heading: v.heading, paragraphStart: v.paragraphStart, indents: v.indents,
            tagRanges: v.tagRanges, footnotes: v.footnotes,
            noteRanges: Array.from(v.noteRanges || []).map(function(r) { return [r.reference, r.continuesBefore, r.continuesAfter] }) }
    })) + (controller.wordsOfJesusColor || "") + numbers + headings + backend.lineHeight + mode + focusLayout + noteMarks + controller.readerSize + root.readerScale + controller.scale
    height: body.height
    onContentKeyChanged: if (!destroying) rebuildTimer.restart()
    // Document builds so far (tests: one reflow per toggle).
    property int rebuildCount: 0
    // The content last built: a view given the same content again (moving
    // between the chapter read and beside it) builds nothing.
    property string builtKey: ""
    // Made ahead (Main.prepareChapterView): its document builds a few verses
    // a turn, each turn within a few milliseconds, so no turn lays out a whole
    // chapter; its geometry refreshes once it is built.
    property bool incremental: false
    property bool building: false
    signal built()
    function rebuild() {
        if (destroying || builtKey === contentKey) return
        builtKey = contentKey
        ++rebuildCount
        // A character selection survives (text size, line height): the
        // controller reselects it from source positions after the build.
        // (Only the chapter read's: a view beside, or made ahead, building
        // must not end a selection or a drag under way in the reader.)
        if (!beside) {
            if (controller.relayoutTextSelection) controller.relayoutTextSelection(); else controller.clearTextSelection()
        }
        cancelDrag()
        const em = controller.readerSize * root.readerScale
        const spacing = { headingAbove: TypeScale.size(em, TypeScale.headingAbove),
            headingBelow: TypeScale.size(em, TypeScale.headingBelow), poetryIndent: em * TypeScale.poetryIndent }
        building = incremental
        if (incremental) {
            document.beginBuild(body.textDocument, displayVerses(), numbers, headings, backend.lineHeight, mode, !focusLayout, em, noteMarks, spacing)
            Qt.callLater(root.buildSome)
            return
        }
        document.build(body.textDocument, displayVerses(), numbers, headings, backend.lineHeight, mode, !focusLayout, em, noteMarks, spacing)
        layoutDirty = true
        geometryTimer.restart()
    }
    function builtAll() {
        building = false
        layoutDirty = true
        geometryTimer.restart()
        built()
    }
    // Shown before it was done: the rest now; built at once from then on.
    function finish() {
        incremental = false
        if (!building) return
        document.buildMore(-1)
        builtAll()
    }
    // A few verses (a few milliseconds) a turn, the next as soon as the event
    // loop is free (an animation-paced Timer would take a long chapter a second).
    function buildSome() {
        if (destroying || !building) return
        if (document.buildMore(6)) builtAll()
        else Qt.callLater(root.buildSome)
    }
    // Runs a pending rebuild and geometry refresh now rather than on the
    // timers' next turn: a transition measures the new layout before the
    // frame that first shows it.
    function settle() {
        if (destroying) return
        if (rebuildTimer.running) { rebuildTimer.stop(); rebuild() }
        if (building) finish()
        if (geometryTimer.running) { geometryTimer.stop(); refreshGeometry() }
        if (shapesTimer.running) { shapesTimer.stop(); paint.layOut() }
        if (selectionTimer.running) { selectionTimer.stop(); selectionPaint.refresh() }
    }
    function refreshGeometry() {
        if (!destroying && !building) {
            paint.requestPaint()
            if (searchLoader.item) searchLoader.item.requestPaint()
            // Motion geometry is computed only while it can be seen, keeping
            // drag-autoscroll refreshes as cheap as before. The focus veil
            // still fading out after a rebuild keeps its holes on the text.
            if (backend.focusMode || dimLayer.visible) updateDimRects()
            pulseRects = pulseActive ? verseRangeRects(pulseFirst, pulseLast) : []
            flashRects = flashActive ? verseRangeRects(flashFirst, flashLast) : []
            updateAnnotations()
            if (layoutDirty) { layoutDirty = false; layoutRefreshed() }
        }
    }
    function cancelDrag() { dragging = false; pressed = false; autoscroll.stop() }
    function clearSelection() { body.deselect() }
    function repaintAnnotations() { paint.requestPaint() }
    function verseRect(number) { return body.positionToRectangle(document.positionFor(number)) }
    function searchRect(segment) { return document.searchRect(segment) }
    function hit(x, y) { return document.verseAtPoint(x, y) }
    // Vertical extent {number, top, bottom} of every verse (body coordinates),
    // reading the span list once.
    function verseExtents() {
        const out = []
        for (const span of Array.from(document.spans)) {
            const rects = document.rectangles(span.start, span.end)
            if (!rects.length) continue
            let top = Infinity, bottom = -Infinity
            for (const rect of rects) { top = Math.min(top, rect.y); bottom = Math.max(bottom, rect.y + rect.height) }
            const last = out.length ? out[out.length - 1] : null
            if (last && last.number === span.number) { last.top = Math.min(last.top, top); last.bottom = Math.max(last.bottom, bottom) }
            else out.push({ number: span.number, top: top, bottom: bottom })
        }
        return out
    }
    // verseExtents for one verse (null when it has no lines).
    function verseExtent(number) {
        let top = Infinity, bottom = -Infinity
        for (const span of Array.from(document.spans)) {
            if (span.number !== number) continue
            for (const rect of document.rectangles(span.start, span.end)) { top = Math.min(top, rect.y); bottom = Math.max(bottom, rect.y + rect.height) }
        }
        return top < Infinity ? { number: number, top: top, bottom: bottom } : null
    }
    // Line rectangles of verses first..last in the shared layout (paint only).
    function verseRangeRects(first, last) {
        if (first <= 0) return []
        const spans = Array.from(document.spans).filter(function(s) { return s.number >= first && s.number <= last })
        return spans.length ? document.rectangles(spans[0].start, spans[spans.length - 1].end) : []
    }
    // Focus-mode dimming follows the cursor instantly; only entering or leaving
    // focus mode as a whole fades. Controllers without motion state (tests,
    // embedders) get static defaults.
    readonly property int focusFirst: beside ? selectedFirst : controller.focusFirst || 0
    readonly property int focusLast: beside ? selectedLast : controller.focusLast || 0
    readonly property int fadeMs: controller.fadeMs || 0
    readonly property real dimOpacity: controller.focusDimOpacity === undefined ? 0.32 : controller.focusDimOpacity
    property int dimFirst: 0
    property int dimLast: 0
    onFocusFirstChanged: if (!destroying) focusTimer.restart()
    onFocusLastChanged: if (!destroying) focusTimer.restart()
    function focusMoved() {
        if (destroying || (dimFirst === focusFirst && dimLast === focusLast)) return
        dimFirst = focusFirst; dimLast = focusLast
        // The veil (a canvas the size of the chapter) repaints only while it
        // shows: outside focus mode a verse step must not repaint it.
        if (backend.focusMode || dimLayer.visible) updateDimRects()
    }
    // Overlay alpha (0 = undimmed) currently painted over a verse; for tests.
    function dimLevel(number) {
        if (!backend.focusMode) return 0
        return number >= dimFirst && number <= dimLast ? 0 : 1 - dimOpacity
    }
    property bool pressed: false
    property bool dragging: false
    property point origin
    property point pointer
    property int anchor: 0
    property int pressedVerse: 0
    // Double click: a verse number opens Notes on that verse; text selects the
    // word under the pointer (the release ending the double click keeps it).
    property bool wordClick: false
    function numberAt(x, y) {
        if (!numbers) return 0
        const position = body.positionAt(x, y)
        const span = Array.from(document.spans).find(function(s) { return position >= s.start && position < s.textStart })
        return span ? span.number : 0
    }
    function selectWordAt(x, y) {
        controller.clearTextSelection()
        body.cursorPosition = body.positionAt(x, y)
        body.selectWord()
        wordClick = body.selectionStart !== body.selectionEnd
    }
    function updateDrag() {
        const p = body.mapFromItem(viewport, pointer.x, pointer.y)
        body.select(anchor, body.positionAt(p.x, p.y))
        controller.freeSelectedText = document.selectionText(body.selectionStart, body.selectionEnd)
        // The verses it runs over become the selection (controllers without it: tests).
        if (typeof controller.dragSelectVerses === "function") controller.dragSelectVerses(pressedVerse, hit(p.x, p.y))
    }
    // qmllint disable import
    BlockDocument {
        id: document
        onGeometryChanged: if (!root.destroying) geometryTimer.restart()
    }
    // qmllint enable import
    // Paint logical ranges behind the single text layout. A range joins through
    // spaces and wrapped lines; adjacent independent highlights keep their gap.
    // Annotation marks (AnnotationPaint, painted in C++): tint bands, then a
    // highlighter stroke per line of each highlight. They repaint only when
    // their shapes change, never for a selection move (selectionPaint).
    AnnotationPaint {
        id: paint
        objectName: root.namePrefix + "blockRangeBackground"
        // Paint outside the first/continuation line without changing body layout
        // or the right endpoint. The same inset is reserved at verse separators.
        readonly property real leftInset: 5
        // Over the marks only (area: the marks' place in the text's, inset).
        x: body.x - leftInset + area.x
        y: body.y + area.y
        width: area.width
        height: area.height
        inset: leftInset
        penScale: root.readerScale > 0 ? root.readerScale : 1
        property var annotations: root.verses
        property int first: root.selectedFirst
        property int last: root.selectedLast
        property color accent: root.theme.accent
        property var tintCounts: root.tintCounts
        ink: root.controller.highlightInk || root.theme.color3 || accent
        inkAlpha: root.controller.highlightAlpha || (root.theme.dark ? 0.34 : 0.36)
        tint: root.controller.tint(accent, HandDrawn.tintAlpha())
        FontMetrics { id: bandMetrics; font.family: root.theme.fontFamily; font.pixelSize: (root.controller.readerSize || 18) * root.readerScale }
        readonly property var highlightLook: [bandMetrics.ascent, bandMetrics.xHeight]
        onHighlightLookChanged: requestPaint()
        onTintCountsChanged: requestPaint()
        onAnnotationsChanged: requestPaint()
        // A selection move repaints only the selection's own layer.
        onFirstChanged: if (!root.destroying) selectionTimer.restart()
        onLastChanged: if (!root.destroying) selectionTimer.restart()
        // Lays the marks out again (once per event loop turn, however many
        // changes asked); they repaint only if they moved or changed.
        function requestPaint() { if (!root.destroying) shapesTimer.restart() }
        Timer { id: shapesTimer; interval: 0; onTriggered: paint.layOut() }
        Timer { id: selectionTimer; interval: 0; onTriggered: selectionPaint.refresh() }
        function layOut() {
            if (root.destroying || root.building) return
            const spans = document.spans
            const ranges = {}
            for (let i = 0; i < spans.length; ++i) {
                const span = spans[i], verse = annotations[i]
                if (!verse) continue
                for (const range of (verse.highlightRanges || [])) {
                    if (!ranges[range.reference]) ranges[range.reference] = { start: span.start, end: span.end }
                    else ranges[range.reference].end = span.end
                }
            }
            const version = root.backend.versionId || ""
            const band = HandDrawn.highlightBand(bandMetrics.ascent, bandMetrics.descent, bandMetrics.xHeight)
            const bandShapes = []
            for (const run of HandDrawn.tintRuns(tintCounts)) {
                if (!spans[run.first] || !spans[run.last]) continue
                const rects = document.rectangles(spans[run.first].start, spans[run.last].end)
                for (let i = 0; i < rects.length; ++i) {
                    const r = rects[i]
                    bandShapes.push({ x: r.x - leftInset, y: r.y, width: r.width + leftInset, height: r.height,
                        seed: HandDrawn.seed("tint", version, run.layer, spans[run.first].number, i),
                        left: i === 0, right: i === rects.length - 1 })
                }
            }
            const strokeShapes = []
            for (const reference in ranges) {
                const rects = document.rectangles(ranges[reference].start, ranges[reference].end)
                // A highlighter stroke per line, about the x-height (Main.highlightBand).
                for (let i = 0; i < rects.length; ++i) {
                    const r = rects[i]
                    strokeShapes.push({ x: r.x, y: r.y + band.top, width: r.width, height: Math.min(band.height, r.height - band.top),
                        seed: HandDrawn.seed("highlight", version, reference, i) })
                }
            }
            bands = bandShapes
            strokes = strokeShapes
            selectionPaint.refresh()
        }
    }
    // The selection, above the marks and below search and the text: the
    // focused verse's tint, the marks (wash and dots), the focused character.
    Item {
        id: selectionPaint
        objectName: root.namePrefix + "blockSelectionBackground"
        x: body.x - paint.leftInset; y: body.y
        width: body.width + paint.leftInset; height: body.height
        property var rects: []
        property var lines: []
        property bool characters: false
        property var cursorRects: []
        function refresh() {
            const tinted = paint.first > 0 && root.tintedVerse >= paint.first && root.tintedVerse <= paint.last ? root.tintedVerse : 0
            rects = root.verseRangeRects(tinted, tinted)
            characters = body.selectionStart !== body.selectionEnd
            lines = markLines()
            cursorRects = focusedCharacter()
        }
        // Text selected by character, else a range's verses (their own
        // lines, not the headings among them), one box per line.
        function markLines() {
            let rects = []
            if (characters) rects = Array.from(document.rectangles(body.selectionStart, body.selectionEnd))
            else if (root.rangeUnderlined && paint.first > 0)
                for (const span of Array.from(document.spans))
                    if (span.number >= paint.first && span.number <= paint.last) rects = rects.concat(Array.from(document.rectangles(span.start, span.end)))
            const out = []
            for (const r of rects) {
                const last = out.length ? out[out.length - 1] : null
                if (last && last.y === r.y && r.x <= last.x + last.width + 0.5) {
                    last.width = Math.max(last.width, r.x + r.width - last.x); last.height = Math.max(last.height, r.height)
                } else out.push({ x: r.x, y: r.y, width: r.width, height: r.height })
            }
            return out.map(function(line) { return { x: line.x + paint.leftInset, y: line.y, width: line.width, height: line.height } })
        }
        function focusedCharacter() {
            const cursor = root.charCursor
            if (!cursor || typeof root.controller.charLength !== "function") return []
            const range = Array.from(document.documentRange(cursor.verse, cursor.offset, root.controller.charLength(cursor)))
            return range.length === 2 ? Array.from(document.rectangles(range[0], range[1])).slice(0, 1) : []
        }
        Repeater {
            model: selectionPaint.rects
            delegate: Rectangle {
                required property var modelData
                x: modelData.x; y: modelData.y
                width: modelData.width + paint.leftInset; height: modelData.height
                color: root.focusTint
            }
        }
        SelectionMarks {
            id: underline
            objectName: root.namePrefix + "blockSelectionUnderline"
            lines: selectionPaint.lines
            ink: paint.accent
            fill: root.rangeTint
            washInset: selectionPaint.characters ? 0 : paint.leftInset
            dot: HandDrawn.underlineDot(root.readerScale)
            dotTop: HandDrawn.underlineTop(bandMetrics.ascent, bandMetrics.descent, bandMetrics.xHeight, dot)
        }
        Repeater {
            model: selectionPaint.cursorRects
            delegate: Rectangle {
                required property var modelData
                objectName: root.namePrefix + "blockCharCursorTint"
                x: modelData.x + paint.leftInset; y: modelData.y
                width: modelData.width; height: modelData.height
                color: root.focusTint
            }
        }
    }
    // Search is a separate background above annotations and below native text.
    // Made only with marks to paint: as tall as the chapter, a canvas paints
    // itself again whenever its size changes, which takes long.
    Loader {
        id: searchLoader
        x: body.x; y: body.y
        width: body.width; height: body.height
        active: root.searchSegments.length > 0
        sourceComponent: Canvas {
            id: searchPaint
            objectName: root.namePrefix + "blockSearchBackground"
            property color accent: root.theme.accent
            onAccentChanged: requestPaint()
            onWidthChanged: if (!root.destroying) geometryTimer.restart()
            onHeightChanged: if (!root.destroying) geometryTimer.restart()
            onPaint: {
                if (root.destroying) return
                const ctx = getContext("2d")
                ctx.reset()
                const rects = document.searchRectangles(root.searchSegments)
                HandDrawn.searchMarks(ctx, rects, accent, false, root.readerScale, "block")
            }
        }
    }
    // Jump pulse and copy flash: translucent accent behind the text. Plain
    // rectangles with animated opacity; nothing repaints per frame.
    property var pulseRects: []
    property var flashRects: []
    readonly property int pulseFirst: beside ? 0 : controller.pulseFirst || 0
    readonly property int pulseLast: beside ? 0 : controller.pulseLast || 0
    readonly property int flashFirst: beside ? 0 : controller.flashFirst || 0
    readonly property int flashLast: beside ? 0 : controller.flashLast || 0
    readonly property bool pulseActive: !beside && !!controller.pulseActive
    readonly property bool flashActive: !beside && !!controller.flashActive
    readonly property bool dimShown: backend.focusMode
    onPulseActiveChanged: if (!destroying && pulseActive) geometryTimer.restart()
    onFlashActiveChanged: if (!destroying && flashActive) geometryTimer.restart()
    // The veil's holes only: the layout itself is refreshed by its own rebuild.
    onDimShownChanged: if (!destroying && dimShown) updateDimRects()
    onPulseFirstChanged: if (!destroying) geometryTimer.restart()
    onPulseLastChanged: if (!destroying) geometryTimer.restart()
    onFlashFirstChanged: if (!destroying) geometryTimer.restart()
    onFlashLastChanged: if (!destroying) geometryTimer.restart()
    Item {
        id: motionLayer
        objectName: root.namePrefix + "blockMotionBackground"
        x: body.x; y: body.y
        width: body.width; height: body.height
        readonly property real pulse: root.pulseActive ? root.controller.pulseLevel : 0
        readonly property real flash: root.flashActive ? root.controller.flashLevel : 0
        visible: pulse > 0 || flash > 0
        Repeater {
            model: motionLayer.pulse > 0 ? root.pulseRects : []
            delegate: Rectangle {
                required property var modelData
                x: modelData.x; y: modelData.y; width: modelData.width; height: modelData.height
                color: root.theme.accent
                opacity: 0.24 * motionLayer.pulse
            }
        }
        Repeater {
            model: motionLayer.flash > 0 ? root.flashRects : []
            delegate: Rectangle {
                required property var modelData
                x: modelData.x; y: modelData.y; width: modelData.width; height: modelData.height
                color: root.theme.accent
                opacity: 0.3 * motionLayer.flash
            }
        }
    }
    TextEdit {
        id: body
        objectName: root.namePrefix + "blockBody"
        width: parent.width
        height: contentHeight
        // Explicit, so Hebrew-first text cannot mirror the alignment.
        horizontalAlignment: TextEdit.AlignLeft
        textFormat: TextEdit.RichText
        font.family: root.theme.fontFamily
        font.pixelSize: root.controller.readerSize * root.readerScale
        onFontChanged: if (!root.destroying) geometryTimer.restart()
        onWidthChanged: if (!root.destroying) geometryTimer.restart()
        color: root.theme.foreground
        // Selected text keeps its look: a dotted underline marks it (the
        // background at no alpha: Qt veils selected inline cells with this
        // color at half).
        selectionColor: Qt.rgba(root.theme.background.r, root.theme.background.g, root.theme.background.b, 0)
        selectedTextColor: "transparent"
        readOnly: true
        persistentSelection: true
        activeFocusOnTab: false
        wrapMode: TextEdit.Wrap
        padding: 0
        onContentHeightChanged: paint.requestPaint()
        function syncSelection() {
            document.trackSelection(selectionStart, selectionEnd)
            // The selected text is the chapter read's (a view beside, or one
            // made ahead or going, has its part painted by the controller).
            if (!root.beside && !root.destroying) root.controller.freeSelectedText = document.selectionText(selectionStart, selectionEnd)
            if (!root.destroying) selectionTimer.restart()
        }
        onSelectedTextChanged: syncSelection()
        onSelectionStartChanged: syncSelection()
        onSelectionEndChanged: syncSelection()
        Keys.onShortcutOverride: function(event) {
            if (event.matches(StandardKey.Copy)) event.accepted = true
        }
        Keys.onPressed: function(event) {
            if (event.matches(StandardKey.Copy)) {
                if (!root.controller.charMode) root.controller.freeSelectedText = document.selectionText(selectionStart, selectionEnd)
                // Controllers without quotation copy (tests, embedders) copy the plain text.
                if (root.controller.freeSelectedText.length) {
                    if (typeof root.controller.copySelectedText === "function") root.controller.copySelectedText(false)
                    else root.controller.copyFreeText()
                }
                else root.controller.copyPassage(false)
                event.accepted = true
            }
        }
        MouseArea {
            id: mouseArea
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
            preventStealing: true
            // Over a verse number: a pointing hand and "Select verse N" tooltip.
            hoverEnabled: true
            property int hoveredNumber: 0
            property point hoverPoint: Qt.point(0, 0)
            cursorShape: hoveredNumber > 0 ? Qt.PointingHandCursor : Qt.IBeamCursor
            onExited: hoveredNumber = 0
            onPressed: function(mouse) {
                hoveredNumber = 0
                if (mouse.button !== Qt.LeftButton || root.beside) return
                root.pressed = true
                root.origin = mapToItem(root.viewport, mouse.x, mouse.y)
                root.pointer = root.origin
                root.anchor = body.positionAt(mouse.x, mouse.y)
                root.pressedVerse = root.hit(mouse.x, mouse.y)
            }
            onPositionChanged: function(mouse) {
                if (!root.pressed) {
                    if (!pressedButtons) { hoveredNumber = root.numberAt(mouse.x, mouse.y); hoverPoint = Qt.point(mouse.x, mouse.y) }
                    return
                }
                root.pointer = mapToItem(root.viewport, mouse.x, mouse.y)
                // Qt exposes this runtime style hint without QML property metadata.
                // qmllint disable missing-property
                if (!root.dragging && Math.hypot(root.pointer.x - root.origin.x, root.pointer.y - root.origin.y) < Qt.styleHints.startDragDistance) return
                // qmllint enable missing-property
                if (!root.dragging) { root.controller.clearTextSelection(); root.controller.textDragFocus(body); root.dragging = true; autoscroll.start() }
                root.updateDrag()
            }
            onReleased: function(mouse) {
                if (root.beside) {
                    const point = mapToItem(null, mouse.x, mouse.y)
                    root.controller.besideClick(root.book, root.chapterNumber, root.hit(mouse.x, mouse.y), mouse.button, mouse.modifiers, point.x, point.y)
                    return
                }
                if (mouse.button === Qt.LeftButton) {
                    // A finished selection becomes PRIMARY, and a character
                    // selection unless it holds no Scripture text (a heading
                    // alone); a click offers the selection bar (controllers
                    // without them: tests).
                    const finished = root.wordClick || root.dragging
                    // Let go before where it began: the cursor is the selection's start.
                    const backward = root.dragging && body.cursorPosition < root.anchor
                    if (finished && typeof root.controller.publishPrimary === "function") root.controller.publishPrimary()
                    if (root.wordClick) root.wordClick = false
                    else if (!root.dragging) root.controller.selectVerse(root.pressedVerse, !!(mouse.modifiers & Qt.ShiftModifier))
                    root.cancelDrag()
                    const characters = finished && typeof root.controller.beginCharMode === "function" && root.controller.beginCharMode(true, backward)
                    if (!characters && typeof root.controller.mouseSelected === "function") root.controller.mouseSelected()
                } else if (mouse.button === Qt.MiddleButton) {
                    root.controller.copyClickedVerse(root.hit(mouse.x, mouse.y), false)
                } else {
                    const point = mapToItem(null, mouse.x, mouse.y)
                    root.controller.openPassageMenu(root.hit(mouse.x, mouse.y), point.x, point.y)
                }
            }
            onCanceled: root.cancelDrag()
            onDoubleClicked: function(mouse) {
                if (mouse.button !== Qt.LeftButton || root.beside) return
                const number = root.numberAt(mouse.x, mouse.y)
                if (number > 0) {
                    root.controller.selectVerse(number, false)
                    root.controller.openNotes(true)
                } else root.selectWordAt(mouse.x, mouse.y)
            }
            AppToolTip {
                objectName: root.namePrefix + "blockNumberTip"
                theme: root.theme
                uiScale: root.controller.scale
                x: mouseArea.hoverPoint.x + 12 * uiScale
                y: mouseArea.hoverPoint.y + 18 * uiScale
                visible: mouseArea.hoveredNumber > 0 && !root.pressed && !root.backend.focusMode
                text: "Select verse " + mouseArea.hoveredNumber + " · " + KeyNotation.format("Shift+Click") + " extends · " + KeyNotation.format("MiddleClick") + " copies"
            }
        }
    }
    // Focus-mode dimming: a background-colored veil over unfocused text, above
    // the text but without input handling, so layout, copy, and hits are unchanged.
    // The canvas repaints once per focus change, with no cross-fade.
    property var dimRects: []
    function updateDimRects() { dimRects = verseRangeRects(dimFirst, dimLast) }
    Item {
        id: dimLayer
        objectName: root.namePrefix + "blockFocusDim"
        x: body.x; y: body.y
        width: body.width; height: body.height
        z: 1
        opacity: root.backend.focusMode ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: root.fadeMs } }
        readonly property color veil: root.theme.background
        readonly property real full: 1 - root.dimOpacity
        // Made only while shown: as tall as the chapter, a canvas paints
        // itself again whenever its size changes, which takes long (and a
        // view made ahead, or left, never shows it).
        Loader {
            anchors.fill: parent
            active: dimLayer.visible
            sourceComponent: Canvas {
                id: dimPaint
                property var holes: root.dimRects
                onHolesChanged: requestPaint()
                property color veil: dimLayer.veil
                onVeilChanged: requestPaint()
                onPaint: {
                    if (root.destroying) return
                    const ctx = getContext("2d")
                    ctx.reset()
                    ctx.fillStyle = Qt.rgba(veil.r, veil.g, veil.b, dimLayer.full)
                    ctx.fillRect(0, 0, width, height)
                    for (const r of holes) ctx.clearRect(r.x, r.y, r.width, r.height)
                }
            }
        }
    }
    // Each delegate occupies an image cell reserved by QTextDocument. The icon
    // sits outside the opening bracket, in the cell's superscript area.
    Repeater {
        model: root.inlineMarkers
        delegate: Item {
            id: inlineMarker
            required property var modelData
            readonly property bool opening: modelData.edge !== "close"
            readonly property bool footnote: modelData.kind === "footnote"
            readonly property string hint: footnote ? "Footnote · " + (modelData.text || "") + "\n" + (root.controller.footnoteHint || "Click: peek")
                : (modelData.kind === "note" ? "Note" : "Tag") + "\n"
                + modelData.reference + (modelData.edge === "continuation" ? " (continued from previous chapter)" : "")
                + (modelData.kind === "tag" ? " · " + root.tagsPlainText(modelData.tags) : "")
            objectName: root.namePrefix + "blockInlineMarker:" + modelData.kind + ":" + modelData.edge + ":" + modelData.reference
            x: modelData.x; y: modelData.y
            width: modelData.width; height: modelData.height
            z: 2
            visible: !root.backend.focusMode
            // Out of the tab order, as in the Verse layout: the passage menu
            // (menu key) opens the cursor's ranges; i peeks at the cursor verse's notes.
            activeFocusOnTab: false
            Keys.onPressed: function(event) {
                if (event.modifiers === Qt.NoModifier && (event.key === Qt.Key_Return || event.key === Qt.Key_Space)) {
                    root.openAnnotation(modelData)
                    event.accepted = true
                }
            }
            // Each part is made only where it shows (a footnote's caller; a
            // range's bracket and glyph): a chapter's markers are many.
            Loader {
                anchors.fill: parent
                active: !inlineMarker.footnote
                sourceComponent: Canvas {
                    id: inlinePaint
                    objectName: root.namePrefix + "blockInlinePaint:" + inlineMarker.modelData.kind + ":" + inlineMarker.modelData.edge + ":" + inlineMarker.modelData.reference
                    property color accent: root.theme.accent
                    property real bracketTop: inlineMarker.modelData.bracketTop
                    property real bracketHeight: inlineMarker.modelData.bracketHeight
                    property real bracketX: inlineMarker.modelData.bracketX
                    property real paintWidth: inlineMarker.modelData.paintWidth
                    property bool rtl: inlineMarker.modelData.rtl
                    onBracketTopChanged: requestPaint()
                    onBracketHeightChanged: requestPaint()
                    onBracketXChanged: requestPaint()
                    onPaintWidthChanged: requestPaint()
                    onRtlChanged: requestPaint()
                    onAccentChanged: requestPaint()
                    onWidthChanged: requestPaint()
                    onHeightChanged: requestPaint()
                    onPaint: {
                        const ctx = getContext("2d")
                        ctx.reset()
                        ctx.fillStyle = accent
                        const lineWidth = Math.max(1, height / 18)
                        if (rtl) { ctx.translate(width, 0); ctx.scale(-1, 1) }
                        ctx.translate(bracketX, 0)
                        const opening = inlineMarker.opening
                        const bx = opening ? paintWidth * 0.78 : paintWidth * 0.65
                        const tip = opening ? paintWidth - lineWidth / 2 : lineWidth / 2
                        const k = root.readerScale > 0 ? root.readerScale : 1
                        // Fold the wobble inward so ink stays in the bracket box.
                        const amp = Math.min(0.8 * k, Math.abs(tip - bx) / 3, bracketHeight / 8)
                        const top = bracketTop + lineWidth / 2 + amp
                        const bottom = bracketTop + bracketHeight - lineWidth / 2 - amp
                        const data = inlineMarker.modelData
                        const s = HandDrawn.seed(data.kind, root.backend.versionId || "", data.reference, data.edge, data.verse, data.position)
                        const pen = { width: lineWidth, amp: amp }
                        if (data.edge === "continuation") {
                            // A chevron denotes a continued range, not a false start.
                            HandDrawn.wavyPolyline(ctx, [Qt.point(bx, top), Qt.point(tip, (top + bottom) / 2), Qt.point(bx, bottom)], s, k, pen)
                        } else {
                            HandDrawn.wavyPolyline(ctx, [Qt.point(tip, top), Qt.point(bx, top), Qt.point(bx, bottom), Qt.point(tip, bottom)], s, k, pen)
                        }
                    }
                }
            }
            // A small superscript caller (or dagger) in the reserved cell.
            Loader {
                anchors.horizontalCenter: parent.horizontalCenter
                active: inlineMarker.footnote
                sourceComponent: Text {
                    objectName: root.namePrefix + "blockFootnoteMark:" + inlineMarker.modelData.reference
                    text: root.footnoteMark(inlineMarker.modelData.caller)
                    color: root.theme.accent
                    font.family: root.theme.fontFamily
                    font.pixelSize: Math.max(6, TypeScale.size(root.controller.readerSize * root.readerScale, TypeScale.mark))
                    textFormat: Text.PlainText
                }
            }
            Loader {
                active: inlineMarker.opening && !inlineMarker.footnote
                x: inlineMarker.modelData.rtl ? parent.width - inlineMarker.modelData.paintWidth * 0.05 - width
                                             : inlineMarker.modelData.paintWidth * 0.05
                y: parent.height * 0.03
                width: Math.min(inlineMarker.modelData.paintWidth * 0.64, parent.height * 0.64)
                height: width
                sourceComponent: AnnotationGlyph {
                    objectName: root.namePrefix + "blockInlineGlyph:" + inlineMarker.modelData.kind + ":" + inlineMarker.modelData.reference
                    kind: inlineMarker.modelData.kind
                    ink: root.theme.accent
                    width: parent ? parent.width : 0
                    height: width
                }
            }
            MouseArea {
                id: inlineMouse
                objectName: root.namePrefix + "blockInlineHit:" + inlineMarker.modelData.kind + ":" + inlineMarker.modelData.edge + ":" + inlineMarker.modelData.reference
                // A taller, invisible target than the narrow cell (layout unchanged):
                // it grows above and below only, so it never covers the next word.
                anchors.fill: parent
                anchors.topMargin: -(root.controller.markerHitMargin || 0)
                anchors.bottomMargin: -(root.controller.markerHitMargin || 0)
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                cursorShape: Qt.PointingHandCursor
                onClicked: function(mouse) {
                    if (inlineMarker.footnote) {
                        const note = inlineMarker.modelData, p = mapToItem(null, mouse.x, mouse.y)
                        const at = inlineMarker.mapToItem(null, 0, 0), height = inlineMarker.height
                        // Beside, its verse is selected first (its chapter becoming the one read).
                        if (root.beside) root.controller.selectContextVerse(root.book, root.chapterNumber, note.verse, false)
                        if (mouse.button === Qt.RightButton) root.controller.openPassageMenu(note.verse, p.x, p.y)
                        else root.controller.peekFootnote(note.verse, note.seq, at.x, at.y, height)
                    } else if (mouse.button === Qt.RightButton) {
                        const entry = inlineMarker.modelData
                        const point = mapToItem(null, mouse.x, mouse.y)
                        root.controller.openMarkerMenu([entry.scope], entry.kind, function() { root.openAnnotation(entry) },
                                                       point.x, point.y)
                    } else root.openAnnotation(inlineMarker.modelData)
                }
            }
            Accessible.role: Accessible.Button
            Accessible.name: hint
            Accessible.onPressAction: root.openAnnotation(modelData)
            // Made while hovered or focused: a popup per marker is costly to make.
            Loader {
                active: inlineMarker.visible && (inlineMouse.containsMouse || inlineMarker.activeFocus)
                sourceComponent: AppToolTip {
                    parent: inlineMarker
                    theme: root.theme
                    uiScale: root.controller.scale
                    visible: true
                    text: inlineMarker.hint
                }
            }
        }
    }
    Timer {
        id: autoscroll
        interval: 16
        repeat: true
        onTriggered: {
            const edge = 48 * root.readerScale
            const bottom = root.viewport.height - root.controller.floatingSafety
            const speed = root.pointer.y < edge ? -Math.min(edge, edge - root.pointer.y)
                : root.pointer.y > bottom - edge ? Math.min(edge, root.pointer.y - bottom + edge) : 0
            if (speed) { root.controller.scrollBy(speed * 0.35); root.updateDrag() }
        }
    }
    // Owned timers coalesce layout work and are canceled with this reader. A
    // global callLater callback can outlive the QML context on window teardown.
    Timer { id: rebuildTimer; interval: 0; onTriggered: root.rebuild() }
    Timer { id: geometryTimer; interval: 0; onTriggered: root.refreshGeometry() }
    Timer { id: focusTimer; interval: 0; onTriggered: root.focusMoved() }
    Component.onCompleted: { dimFirst = focusFirst; dimLast = focusLast; rebuild() }
    Component.onDestruction: {
        destroying = true
        rebuildTimer.stop()
        geometryTimer.stop()
        focusTimer.stop()
        autoscroll.stop()
    }
}
