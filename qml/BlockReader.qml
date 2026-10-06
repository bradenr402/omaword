import QtQuick
import QtQuick.Controls
import "HandDrawn.js" as HandDrawn
import "KeyNotation.js" as KeyNotation
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
    property var annotationRanges: []
    property var inlineMarkers: []
    readonly property var searchSegments: controller.searchSegments || []
    onSearchSegmentsChanged: if (!destroying) searchPaint.requestPaint()
    readonly property real cornerRadius: theme.cornerRadius === undefined ? 0 : theme.cornerRadius
    // Source callers that are letters or digits show as-is; "+"/"-" become a dagger.
    function footnoteMark(caller) { return /^[A-Za-z0-9]{1,3}$/.test(caller || "") ? caller : "\u2020" }
    function tagsPlainText(tags) { return Array.from(tags || []).map(function(t) { return t.name }).join(", ") }
    property real annotationScale: 0
    // Reader text and marks follow the desktop scale; a controller without
    // readerScale (tests) falls back to its plain scale.
    readonly property real readerScale: controller.readerScale > 0 ? controller.readerScale
        : controller.scale > 0 ? controller.scale : 1
    readonly property string annotationKey: JSON.stringify(backend.verses) + JSON.stringify(backend.collection)
        + backend.book + ":" + backend.chapter
    onAnnotationKeyChanged: if (!destroying) geometryTimer.restart()
    function updateAnnotations() {
        if (destroying) return
        const ranges = document.annotations(backend.verses, backend.collection, backend.book, backend.chapter)
        const markers = document.inlineDecorations()
        if (JSON.stringify(markers) !== JSON.stringify(inlineMarkers)) inlineMarkers = markers
        if (JSON.stringify(ranges) === JSON.stringify(annotationRanges) && annotationScale === root.readerScale) return
        annotationScale = root.readerScale
        annotationRanges = ranges
    }
    function openAnnotation(entry) {
        if (entry.kind === "footnote") { controller.openFootnote(entry.verse, entry.seq); return }
        if (entry.kind === "bookmark") controller.openBookmark(entry.scope || entry.reference)
        else if (entry.kind === "tag") controller.openPassageTags(entry.scope || entry.reference)
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
    // Margin notes shown: rebuild without note cells (one reflow per toggle)
    // rather than leave blank, unpainted gaps in the text.
    readonly property bool noteMarks: controller.inlineNoteMarks === undefined || !!controller.inlineNoteMarks
    // Annotation tint layers per verse (HandDrawn.tintCount), none in focus mode.
    readonly property var tintCounts: backend.focusMode ? [] : Array.from(backend.verses).map(function(v) { return HandDrawn.tintCount(v) })
    readonly property bool numbers: backend.showVerseNumbers && !backend.focusMode
    readonly property bool headings: backend.showHeadings && !backend.focusMode
    // Display copies of the verses: words of Jesus colored (format only).
    function displayVerses() {
        if (!controller.verseHtml) return backend.verses
        return Array.from(backend.verses).map(function(v) {
            const html = controller.verseHtml(v.html)
            if (html === v.html) return v
            const copy = Object.assign({}, v); copy.html = html; return copy
        })
    }
    readonly property string contentKey: JSON.stringify(Array.from(backend.verses).map(function(v) {
        return { number: v.number, html: v.html, text: v.text, heading: v.heading, paragraphStart: v.paragraphStart,
            bookmarkRanges: v.bookmarkRanges, noteRanges: v.noteRanges, tagRanges: v.tagRanges, footnotes: v.footnotes }
    })) + (controller.wordsOfJesusColor || "") + numbers + headings + backend.lineHeight + mode + backend.focusMode + noteMarks + controller.readerSize + root.readerScale + controller.scale
    height: body.height
    onContentKeyChanged: if (!destroying) rebuildTimer.restart()
    function rebuild() {
        if (destroying) return
        // A character selection survives (text size, line height): the
        // controller reselects it from source positions after the build.
        if (controller.relayoutTextSelection) controller.relayoutTextSelection(); else controller.clearTextSelection()
        cancelDrag()
        document.build(body.textDocument, displayVerses(), numbers, headings, backend.lineHeight, mode,
                       !backend.focusMode, controller.readerSize * root.readerScale, noteMarks)
        layoutDirty = true
        geometryTimer.restart()
    }
    function refreshGeometry() {
        if (!destroying) {
            paint.requestPaint()
            searchPaint.requestPaint()
            // Motion geometry is computed only while it can be seen, keeping
            // drag-autoscroll refreshes as cheap as before.
            if (backend.focusMode) updateDimRects()
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
    // Line rectangles of verses first..last in the shared layout (paint only).
    function verseRangeRects(first, last) {
        if (first <= 0) return []
        const spans = Array.from(document.spans).filter(function(s) { return s.number >= first && s.number <= last })
        return spans.length ? document.rectangles(spans[0].start, spans[spans.length - 1].end) : []
    }
    // Focus-mode dimming follows the cursor instantly; only entering or leaving
    // focus mode as a whole fades. Controllers without motion state (tests,
    // embedders) get static defaults.
    readonly property int focusFirst: controller.focusFirst || 0
    readonly property int focusLast: controller.focusLast || 0
    readonly property int fadeMs: controller.fadeMs || 0
    readonly property real dimOpacity: controller.focusDimOpacity === undefined ? 0.32 : controller.focusDimOpacity
    property int dimFirst: 0
    property int dimLast: 0
    onFocusFirstChanged: if (!destroying) focusTimer.restart()
    onFocusLastChanged: if (!destroying) focusTimer.restart()
    function focusMoved() {
        if (destroying || (dimFirst === focusFirst && dimLast === focusLast)) return
        dimFirst = focusFirst; dimLast = focusLast
        updateDimRects()
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
    }
    // qmllint disable import
    BlockDocument {
        id: document
        onGeometryChanged: if (!root.destroying) geometryTimer.restart()
    }
    // qmllint enable import
    // Paint logical ranges behind the single text layout. A range joins through
    // spaces and wrapped lines; adjacent independent highlights keep their gap.
    Canvas {
        id: paint
        objectName: "blockRangeBackground"
        // Paint outside the first/continuation line without changing body layout
        // or the right endpoint. The same inset is reserved at verse separators.
        readonly property real leftInset: 5
        x: body.x - leftInset
        y: body.y
        width: body.width + leftInset
        height: body.height
        property var annotations: root.backend.verses
        property int first: root.controller.charMode ? 0 : root.backend.selectedStart
        property int last: root.backend.selectedEnd
        property color accent: root.theme.accent
        property bool focused: root.backend.focusMode
        property var tintCounts: root.tintCounts
        FontMetrics { id: bandMetrics; font.family: root.theme.fontFamily; font.pixelSize: (root.controller.readerSize || 18) * root.readerScale }
        readonly property var highlightLook: [bandMetrics.ascent, bandMetrics.xHeight, root.controller.highlightInk, root.controller.highlightAlpha]
        onHighlightLookChanged: requestPaint()
        onTintCountsChanged: requestPaint()
        onAnnotationsChanged: requestPaint()
        onFirstChanged: if (!root.destroying) geometryTimer.restart()
        onLastChanged: if (!root.destroying) geometryTimer.restart()
        onAccentChanged: requestPaint()
        onFocusedChanged: requestPaint()
        onWidthChanged: if (!root.destroying) geometryTimer.restart()
        onHeightChanged: if (!root.destroying) geometryTimer.restart()
        onPaint: {
            if (root.destroying) return
            const ctx = getContext("2d")
            ctx.reset()
            ctx.translate(leftInset, 0)
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
            const k = root.readerScale > 0 ? root.readerScale : 1
            const version = root.backend.versionId || ""
            const band = HandDrawn.highlightBand(bandMetrics.ascent, bandMetrics.descent, bandMetrics.xHeight)
            const ink = root.controller.highlightInk || root.theme.color3 || accent
            const inkAlpha = root.controller.highlightAlpha || (root.theme.dark ? 0.34 : 0.36)
            function draw(start, end, selected, reference) {
                const rects = document.rectangles(start, end)
                for (let i = 0; i < rects.length; ++i) {
                    const r = rects[i]
                    if (selected) {
                        // The active selection is not an annotation mark.
                        ctx.fillStyle = root.controller.tint(accent, 0.22)
                        ctx.fillRect(r.x - leftInset, r.y, r.width + leftInset, r.height)
                        continue
                    }
                    // A highlighter stroke per line, about the x-height (Main.highlightBand).
                    HandDrawn.highlighterStroke(ctx, Qt.rect(r.x, r.y + band.top, r.width, Math.min(band.height, r.height - band.top)),
                        ink, inkAlpha, HandDrawn.seed("highlight", version, reference, i), k)
                }
            }
            // Tints first: each covering range adds a layer; highlights go on top.
            ctx.fillStyle = root.controller.tint(accent, HandDrawn.tintAlpha())
            for (const run of HandDrawn.tintRuns(tintCounts)) {
                if (!spans[run.first] || !spans[run.last]) continue
                const rects = document.rectangles(spans[run.first].start, spans[run.last].end)
                for (let i = 0; i < rects.length; ++i) {
                    const r = rects[i]
                    HandDrawn.highlighterBand(ctx, Qt.rect(r.x - leftInset, r.y, r.width + leftInset, r.height),
                        HandDrawn.seed("tint", version, run.layer, spans[run.first].number, i), k,
                        { top: true, bottom: true, left: i === 0, right: i === rects.length - 1 })
                }
            }
            for (const ref in ranges) draw(ranges[ref].start, ranges[ref].end, false, ref)
            const selected = Array.from(spans).filter(function(s) { return first > 0 && s.number >= first && s.number <= last })
            if (selected.length) draw(selected[0].start, selected[selected.length - 1].end, true, "")
        }
    }
    // Search is a separate background above annotations and below native text.
    Canvas {
        id: searchPaint
        objectName: "blockSearchBackground"
        x: body.x; y: body.y
        width: body.width; height: body.height
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
    // Jump pulse and copy flash: translucent accent behind the text. Plain
    // rectangles with animated opacity; nothing repaints per frame.
    property var pulseRects: []
    property var flashRects: []
    readonly property int pulseFirst: controller.pulseFirst || 0
    readonly property int pulseLast: controller.pulseLast || 0
    readonly property int flashFirst: controller.flashFirst || 0
    readonly property int flashLast: controller.flashLast || 0
    readonly property bool pulseActive: !!controller.pulseActive
    readonly property bool flashActive: !!controller.flashActive
    readonly property bool dimShown: backend.focusMode
    onPulseActiveChanged: if (!destroying && pulseActive) geometryTimer.restart()
    onFlashActiveChanged: if (!destroying && flashActive) geometryTimer.restart()
    onDimShownChanged: if (!destroying && dimShown) geometryTimer.restart()
    onPulseFirstChanged: if (!destroying) geometryTimer.restart()
    onPulseLastChanged: if (!destroying) geometryTimer.restart()
    onFlashFirstChanged: if (!destroying) geometryTimer.restart()
    onFlashLastChanged: if (!destroying) geometryTimer.restart()
    Item {
        id: motionLayer
        objectName: "blockMotionBackground"
        x: body.x; y: body.y
        width: body.width; height: body.height
        readonly property real pulse: root.controller.pulseActive ? root.controller.pulseLevel : 0
        readonly property real flash: root.controller.flashActive ? root.controller.flashLevel : 0
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
        objectName: "blockBody"
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
        selectionColor: root.controller.tint(root.theme.accent, 0.35)
        selectedTextColor: root.theme.foreground
        readOnly: true
        persistentSelection: true
        activeFocusOnTab: false
        wrapMode: TextEdit.Wrap
        padding: 0
        onContentHeightChanged: paint.requestPaint()
        function syncSelection() {
            document.trackSelection(selectionStart, selectionEnd)
            root.controller.freeSelectedText = document.selectionText(selectionStart, selectionEnd)
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
                if (mouse.button !== Qt.LeftButton) return
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
                if (!root.dragging) { root.controller.clearTextSelection(); body.forceActiveFocus(); root.dragging = true; autoscroll.start() }
                root.updateDrag()
            }
            onReleased: function(mouse) {
                if (mouse.button === Qt.LeftButton) {
                    // A finished selection becomes PRIMARY (controllers without it: tests).
                    const finished = root.wordClick || root.dragging
                    if (finished && typeof root.controller.publishPrimary === "function") root.controller.publishPrimary()
                    if (root.wordClick) root.wordClick = false
                    else if (!root.dragging) root.controller.selectVerse(root.pressedVerse, !!(mouse.modifiers & Qt.ShiftModifier))
                    root.cancelDrag()
                } else if (mouse.button === Qt.MiddleButton) {
                    root.controller.copyClickedVerse(root.hit(mouse.x, mouse.y), false)
                } else {
                    const point = mapToItem(null, mouse.x, mouse.y)
                    root.controller.openPassageMenu(root.hit(mouse.x, mouse.y), point.x, point.y)
                }
            }
            onCanceled: root.cancelDrag()
            onDoubleClicked: function(mouse) {
                if (mouse.button !== Qt.LeftButton) return
                const number = root.numberAt(mouse.x, mouse.y)
                if (number > 0) {
                    root.controller.selectVerse(number, false)
                    root.controller.openNotes(true)
                } else root.selectWordAt(mouse.x, mouse.y)
            }
            AppToolTip {
                objectName: "blockNumberTip"
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
        objectName: "blockFocusDim"
        x: body.x; y: body.y
        width: body.width; height: body.height
        z: 1
        opacity: root.backend.focusMode ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: root.fadeMs } }
        readonly property color veil: root.theme.background
        readonly property real full: 1 - root.dimOpacity
        Canvas {
            id: dimPaint
            anchors.fill: parent
            property var holes: root.dimRects
            onHolesChanged: requestPaint()
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            onVisibleChanged: if (visible) requestPaint()
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
    // Each delegate occupies an image cell reserved by QTextDocument. The icon
    // sits outside the opening bracket, in the cell's superscript area.
    Repeater {
        model: root.inlineMarkers
        delegate: Item {
            id: inlineMarker
            required property var modelData
            readonly property bool opening: modelData.edge !== "close"
            readonly property bool footnote: modelData.kind === "footnote"
            readonly property string hint: footnote ? "Footnote · " + (modelData.text || "") + "\n" + (root.controller.footnoteHint || "Click: read it in Study")
                : (modelData.kind === "bookmark" ? "Bookmark" : modelData.kind === "note" ? "Note" : "Tag") + "\n"
                + modelData.reference + (modelData.edge === "continuation" ? " (continued from previous chapter)" : "")
                + (modelData.kind === "tag" ? " · " + root.tagsPlainText(modelData.tags) : "")
                + "\n" + KeyNotation.format("Shift+Click") + " to edit range"
            objectName: "blockInlineMarker:" + modelData.kind + ":" + modelData.edge + ":" + modelData.reference
            x: modelData.x; y: modelData.y
            width: modelData.width; height: modelData.height
            z: 2
            visible: !root.backend.focusMode
            // Out of the tab order, as in the Verse layout: the passage menu
            // (menu key) opens the cursor's ranges; shift S opens Study → Footnotes.
            activeFocusOnTab: false
            Keys.onPressed: function(event) {
                if (event.modifiers === Qt.NoModifier && (event.key === Qt.Key_Return || event.key === Qt.Key_Space)) {
                    root.openAnnotation(modelData)
                    event.accepted = true
                }
            }
            Canvas {
                id: inlinePaint
                objectName: "blockInlinePaint:" + inlineMarker.modelData.kind + ":" + inlineMarker.modelData.edge + ":" + inlineMarker.modelData.reference
                anchors.fill: parent
                visible: !inlineMarker.footnote
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
            // A small superscript caller (or dagger) in the reserved cell.
            Text {
                objectName: "blockFootnoteMark:" + inlineMarker.modelData.reference
                visible: inlineMarker.footnote
                anchors.horizontalCenter: parent.horizontalCenter
                y: 0
                text: root.footnoteMark(inlineMarker.modelData.caller)
                color: root.theme.accent
                font.family: root.theme.fontFamily
                font.pixelSize: Math.max(6, root.controller.readerSize * root.readerScale * 0.55)
                textFormat: Text.PlainText
            }
            AnnotationGlyph {
                objectName: "blockInlineGlyph:" + inlineMarker.modelData.kind + ":" + inlineMarker.modelData.reference
                visible: inlineMarker.opening && !inlineMarker.footnote
                kind: inlineMarker.modelData.kind
                filled: true
                ink: root.theme.accent
                x: inlineMarker.modelData.rtl ? parent.width - inlineMarker.modelData.paintWidth * 0.05 - width
                                             : inlineMarker.modelData.paintWidth * 0.05
                y: parent.height * 0.03
                width: Math.min(inlineMarker.modelData.paintWidth * 0.64, parent.height * 0.64)
                height: width
            }
            MouseArea {
                id: inlineMouse
                objectName: "blockInlineHit:" + inlineMarker.modelData.kind + ":" + inlineMarker.modelData.edge + ":" + inlineMarker.modelData.reference
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
                        if (mouse.button === Qt.RightButton) {
                            const p = mapToItem(null, mouse.x, mouse.y)
                            root.controller.openPassageMenu(inlineMarker.modelData.verse, p.x, p.y)
                        } else root.openAnnotation(inlineMarker.modelData)
                    } else if (mouse.button === Qt.RightButton) {
                        const entry = inlineMarker.modelData
                        const point = mapToItem(null, mouse.x, mouse.y)
                        root.controller.openMarkerMenu([entry.scope], entry.kind, function() { root.openAnnotation(entry) },
                                                       entry.scope, point.x, point.y)
                    } else if (mouse.modifiers & Qt.ShiftModifier) {
                        root.controller.openRangeEditor(inlineMarker.modelData.scope, inlineMarker.modelData.kind)
                    } else root.openAnnotation(inlineMarker.modelData)
                }
            }
            Accessible.role: Accessible.Button
            Accessible.name: hint
            Accessible.onPressAction: root.openAnnotation(modelData)
            AppToolTip {
                theme: root.theme
                uiScale: root.controller.scale
                visible: inlineMarker.visible && (inlineMouse.containsMouse || inlineMarker.activeFocus)
                text: inlineMarker.hint
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
