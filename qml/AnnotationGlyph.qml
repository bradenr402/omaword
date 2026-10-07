import QtQuick

Item {
    id: root
    property string kind: "note"
    property color ink: "white"
    property bool filled: false
    width: 18
    height: 18

    Canvas {
        // Supersample filled silhouettes so cutouts survive small raster sizes.
        width: root.width * (filled ? 3 : 1)
        height: root.height * (filled ? 3 : 1)
        scale: filled ? 1 / 3 : 1
        transformOrigin: Item.TopLeft
        property string kind: root.kind
        property color ink: root.ink
        property bool filled: root.filled
        onFilledChanged: requestPaint()
        onKindChanged: requestPaint()
        onInkChanged: requestPaint()
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.scale(width / 24, height / 24)
            ctx.strokeStyle = ink
            ctx.fillStyle = ink
            ctx.lineWidth = 1.6
            ctx.lineCap = "square"
            ctx.lineJoin = "miter"
            ctx.beginPath()
            if (kind === "note") {
                if (filled) {
                    // Slightly larger inside the same reserved annotation cell.
                    ctx.translate(12, 12)
                    ctx.scale(1.1, 1.1)
                    ctx.translate(-12, -12)
                    // The same square and pencil as the outline, with a clear
                    // 1.5-unit transparent halo on either side of the pencil.
                    ctx.fillRect(3, 4, 17, 17)
                    ctx.globalCompositeOperation = "destination-out"
                    ctx.beginPath()
                    ctx.moveTo(10, 11); ctx.lineTo(18, 3); ctx.lineTo(21, 6)
                    ctx.lineTo(13, 14); ctx.lineTo(9, 15); ctx.closePath()
                    ctx.fill()
                    ctx.lineWidth = 3
                    ctx.stroke()
                    ctx.globalCompositeOperation = "source-over"
                    ctx.fill()
                    return
                }
                // Authoritative footer pencil in an open square.
                ctx.moveTo(11, 4); ctx.lineTo(3, 4); ctx.lineTo(3, 21)
                ctx.lineTo(20, 21); ctx.lineTo(20, 13)
                ctx.moveTo(10, 11); ctx.lineTo(18, 3); ctx.lineTo(21, 6)
                ctx.lineTo(13, 14); ctx.lineTo(9, 15); ctx.closePath()
                ctx.moveTo(16, 5); ctx.lineTo(19, 8)
            } else if (kind === "tag") {
                // Mirror the original tag so its point faces left.
                ctx.translate(24, 0)
                ctx.scale(-2, 2)
                ctx.lineWidth = 0.8
                ctx.moveTo(1, 1); ctx.lineTo(6, 1); ctx.lineTo(11, 7)
                ctx.lineTo(6, 12); ctx.lineTo(1, 6); ctx.closePath()
                if (filled) {
                    ctx.fill()
                    ctx.stroke()
                    ctx.globalCompositeOperation = "destination-out"
                    ctx.beginPath()
                    ctx.arc(3.5, 3.5, 0.85, 0, Math.PI * 2)
                    ctx.fill()
                    ctx.globalCompositeOperation = "source-over"
                    return
                }
                ctx.moveTo(3, 3); ctx.lineTo(4, 4)
            }
            ctx.stroke()
        }
    }
}
