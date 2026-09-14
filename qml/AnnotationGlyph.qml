import QtQuick

Item {
    id: root
    property string kind: "note"
    property color ink: "white"
    property bool filled: false
    width: 18
    height: 18

    Canvas {
        anchors.fill: parent
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
                    // Solid sheet with a transparent pencil, on any background.
                    ctx.rect(3, 3, 18, 18)
                    ctx.fill()
                    ctx.globalCompositeOperation = "destination-out"
                    ctx.beginPath()
                    ctx.moveTo(8, 13); ctx.lineTo(16, 5); ctx.lineTo(19, 8)
                    ctx.lineTo(11, 16); ctx.lineTo(7, 17); ctx.closePath()
                    ctx.fill()
                    ctx.globalCompositeOperation = "source-over"
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
            } else if (kind === "bookmark") {
                ctx.moveTo(6, 3); ctx.lineTo(18, 3); ctx.lineTo(18, 21)
                ctx.lineTo(12, 17); ctx.lineTo(6, 21); ctx.closePath()
                if (filled) ctx.fill()
            }
            ctx.stroke()
        }
    }
}
