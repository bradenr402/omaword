import QtQuick
import "HandDrawn.js" as HandDrawn

// A Verse layout row's annotation tint (the chapter read's, or one shown
// beside it): one translucent layer per range over the verse
// (HandDrawn.tintCount, capped), joined with the neighbouring rows that keep
// the layer; highlights paint on top.
Canvas {
    id: tint
    // The reader window (Main) and the live theme.
    required property var controller
    required property var theme
    // The verse's row and its neighbours' (null at the chapter's edges).
    required property var row
    property var previous: null
    property var next: null
    required property real headingHeight
    required property real bodyHeight
    required property string versionId
    // Focus mode draws no marks.
    property bool focusMode: false
    readonly property int layers: focusMode ? 0 : HandDrawn.tintCount(row)
    readonly property int layersBefore: previous ? HandDrawn.tintCount(previous) : 0
    readonly property int layersAfter: next ? HandDrawn.tintCount(next) : 0
    visible: layers > 0
    property color ink: theme.accent
    readonly property var paintKey: [layers, layersBefore, layersAfter, width, height, headingHeight,
        bodyHeight, controller.readerScale, versionId]
    onPaintKeyChanged: requestPaint()
    onInkChanged: requestPaint()
    onPaint: {
        const ctx = getContext("2d")
        ctx.reset()
        const k = controller.readerScale
        ctx.fillStyle = Qt.rgba(ink.r, ink.g, ink.b, HandDrawn.tintAlpha())
        for (let layer = 1; layer <= layers; ++layer) {
            const before = layersBefore >= layer, after = layersAfter >= layer
            const top = before ? 0 : headingHeight
            const bottom = after ? height : headingHeight + bodyHeight + 3
            HandDrawn.highlighterBand(ctx, Qt.rect(0, top, width, bottom - top),
                HandDrawn.seed("tint", versionId, layer, row.number), k,
                { top: !before, bottom: !after })
        }
    }
}
