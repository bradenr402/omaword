import QtQuick
import QtQuick.Controls
// Registered by blockdocument.cpp at application startup, without a QML plugin.
// qmllint disable import
import OmaWord.Reader 1.0
// qmllint enable import

Item {
    id: root
    property bool destroying: false
    required property var controller
    required property var backend
    required property var theme
    required property Flickable viewport
    property string mode: "block"
    property alias body: body
    property alias mapping: document
    property var annotationRanges: []
    property var inlineMarkers: []
    property var contextRanges: []
    readonly property var searchSegments: controller.searchSegments || []
    onSearchSegmentsChanged: if (!destroying) searchPaint.requestPaint()
    readonly property real cornerRadius: theme.cornerRadius === undefined ? 0 : theme.cornerRadius
    function tagsPlainText(tags) { return Array.from(tags || []).map(function(t) { return t.name }).join(", ") }
    property real annotationScale: 0
    readonly property string annotationKey: JSON.stringify(backend.verses) + JSON.stringify(backend.collection)
        + backend.book + ":" + backend.chapter
    onAnnotationKeyChanged: if (!destroying) geometryTimer.restart()
    function updateAnnotations() {
        if (destroying) return
        const ranges = document.annotations(backend.verses, backend.collection, backend.book, backend.chapter)
        const markers = document.inlineDecorations()
        if (JSON.stringify(markers) !== JSON.stringify(inlineMarkers)) inlineMarkers = markers
        if (JSON.stringify(ranges) === JSON.stringify(annotationRanges) && annotationScale === controller.scale) return
        annotationScale = controller.scale
        annotationRanges = ranges
    }
    function openAnnotation(entry) {
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
    readonly property bool numbers: backend.showVerseNumbers && !backend.focusMode
    readonly property bool headings: backend.showHeadings && !backend.focusMode
    readonly property string contentKey: JSON.stringify(Array.from(backend.verses).map(function(v) {
        return { number: v.number, html: v.html, text: v.text, heading: v.heading, paragraphStart: v.paragraphStart,
            bookmarkRanges: v.bookmarkRanges, noteRanges: v.noteRanges, tagRanges: v.tagRanges }
    })) + numbers + headings + backend.lineHeight + mode + backend.focusMode + controller.readerSize + controller.scale
    height: body.height
    onContentKeyChanged: if (!destroying) rebuildTimer.restart()
    function rebuild() {
        if (destroying) return
        controller.clearTextSelection()
        cancelDrag()
        document.build(body.textDocument, backend.verses, numbers, headings, backend.lineHeight, mode,
                       !backend.focusMode, controller.readerSize * controller.scale)
        geometryTimer.restart()
    }
    function refreshGeometry() {
        if (!destroying) {
            contextRanges = backend.focusMode ? [] : document.contextRanges(backend.verses, backend.cursorVerse,
                                                                           backend.selectedStart, backend.selectedEnd)
            paint.requestPaint()
            searchPaint.requestPaint()
            updateAnnotations()
        }
    }
    function cancelDrag() { dragging = false; pressed = false; autoscroll.stop() }
    function clearSelection() { body.deselect() }
    function verseRect(number) { return body.positionToRectangle(document.positionFor(number)) }
    function searchRect(segment) { return document.searchRect(segment) }
    function hit(x, y) { return document.verseAtPoint(x, y) }
    property bool pressed: false
    property bool dragging: false
    property point origin
    property point pointer
    property int anchor: 0
    property int pressedVerse: 0
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
        property int first: root.backend.selectedStart
        property int last: root.backend.selectedEnd
        property color accent: root.theme.accent
        property bool focused: root.backend.focusMode
        property int cursorVerse: root.backend.cursorVerse
        property var contextRanges: root.contextRanges
        onCursorVerseChanged: if (!root.destroying) geometryTimer.restart()
        onContextRangesChanged: requestPaint()
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
            function draw(start, end, selected) {
                for (const r of document.rectangles(start, end)) {
                    ctx.fillStyle = root.controller.tint(accent, selected ? 0.22 : 0.16)
                    ctx.fillRect(r.x - leftInset, r.y, r.width + leftInset, r.height)
                    if (!selected && !focused) {
                        ctx.fillStyle = root.controller.tint(accent, 0.7)
                        ctx.fillRect(r.x - leftInset, r.y, 2, r.height)
                    }
                }
            }
            for (const ref in ranges) draw(ranges[ref].start, ranges[ref].end, false)
            if (!focused) {
                ctx.fillStyle = root.controller.tint(accent, 0.06)
                for (const range of contextRanges)
                    for (const r of document.rectangles(range.start, range.end)) ctx.fillRect(r.x, r.y, r.width, r.height)
            }
            const selected = Array.from(spans).filter(function(s) { return first > 0 && s.number >= first && s.number <= last })
            if (selected.length) draw(selected[0].start, selected[selected.length - 1].end, true)
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
            for (let active = 0; active < 2; ++active) {
                for (const r of rects) {
                    if (!!r.active !== !!active) continue
                    ctx.fillStyle = Qt.rgba(accent.r, accent.g, accent.b, active ? 0.55 : 0.28)
                    ctx.fillRect(r.x, r.y, r.width, r.height)
                    if (active) {
                        ctx.strokeStyle = accent
                        ctx.lineWidth = 1
                        ctx.strokeRect(r.x + 0.5, r.y + 0.5, Math.max(0, r.width - 1), Math.max(0, r.height - 1))
                    }
                }
            }
        }
    }
    TextEdit {
        id: body
        objectName: "blockBody"
        width: parent.width
        height: contentHeight
        textFormat: TextEdit.RichText
        font.family: root.theme.fontFamily
        font.pixelSize: root.controller.readerSize * root.controller.scale
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
                root.controller.freeSelectedText = document.selectionText(selectionStart, selectionEnd)
                if (root.controller.freeSelectedText.length) root.controller.copyFreeText()
                else root.controller.copyPassage(false)
                event.accepted = true
            }
        }
        MouseArea {
            id: mouseArea
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
            preventStealing: true
            cursorShape: Qt.IBeamCursor
            onPressed: function(mouse) {
                if (mouse.button !== Qt.LeftButton) return
                root.pressed = true
                root.origin = mapToItem(root.viewport, mouse.x, mouse.y)
                root.pointer = root.origin
                root.anchor = body.positionAt(mouse.x, mouse.y)
                root.pressedVerse = root.hit(mouse.x, mouse.y)
            }
            onPositionChanged: function(mouse) {
                if (!root.pressed) return
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
                    if (!root.dragging) root.controller.selectVerse(root.pressedVerse, !!(mouse.modifiers & Qt.ShiftModifier))
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
                root.controller.selectVerse(root.hit(mouse.x, mouse.y), false)
                root.controller.openNotes(true)
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
            readonly property string hint: (modelData.kind === "bookmark" ? "Bookmark" : modelData.kind === "note" ? "Note" : "Tag") + "\n"
                + modelData.reference + (modelData.edge === "continuation" ? " (continued from previous chapter)" : "")
                + (modelData.kind === "tag" ? " · " + root.tagsPlainText(modelData.tags) : "")
                + "\nShift-click to edit range"
            objectName: "blockInlineMarker:" + modelData.kind + ":" + modelData.edge + ":" + modelData.reference
            x: modelData.x; y: modelData.y
            width: modelData.width; height: modelData.height
            z: 2
            visible: !root.backend.focusMode
            activeFocusOnTab: opening
            Keys.onPressed: function(event) {
                if (event.modifiers === Qt.NoModifier && (event.key === Qt.Key_Return || event.key === Qt.Key_Space)) {
                    root.openAnnotation(modelData)
                    event.accepted = true
                }
            }
            Canvas {
                id: inlinePaint
                anchors.fill: parent
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
                    ctx.strokeStyle = accent
                    ctx.fillStyle = accent
                    ctx.lineWidth = Math.max(1, height / 18)
                    if (rtl) { ctx.translate(width, 0); ctx.scale(-1, 1) }
                    ctx.translate(bracketX, 0)
                    const opening = inlineMarker.opening
                    const bx = opening ? paintWidth * 0.78 : paintWidth * 0.65
                    const tip = opening ? paintWidth - ctx.lineWidth / 2 : ctx.lineWidth / 2
                    const top = bracketTop + ctx.lineWidth / 2
                    const bottom = bracketTop + bracketHeight - ctx.lineWidth / 2
                    ctx.beginPath()
                    if (inlineMarker.modelData.edge === "continuation") {
                        // A chevron denotes a continued range, not a false start.
                        ctx.moveTo(bx, top)
                        ctx.lineTo(tip, (top + bottom) / 2)
                        ctx.lineTo(bx, bottom)
                    } else {
                        ctx.moveTo(tip, top)
                        ctx.lineTo(bx, top)
                        ctx.lineTo(bx, bottom)
                        ctx.lineTo(tip, bottom)
                    }
                    ctx.stroke()
                }
            }
            AnnotationGlyph {
                objectName: "blockInlineGlyph:" + inlineMarker.modelData.kind + ":" + inlineMarker.modelData.reference
                visible: inlineMarker.opening
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
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                cursorShape: Qt.PointingHandCursor
                onClicked: function(mouse) {
                    if (mouse.button === Qt.RightButton) {
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
                visible: inlineMarker.visible && (inlineMouse.containsMouse || inlineMarker.activeFocus)
                delay: 500
                text: inlineMarker.hint
            }
        }
    }
    Timer {
        id: autoscroll
        interval: 16
        repeat: true
        onTriggered: {
            const edge = 48 * root.controller.scale
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
    Component.onCompleted: rebuild()
    Component.onDestruction: {
        destroying = true
        rebuildTimer.stop()
        geometryTimer.stop()
        autoscroll.stop()
    }
}
