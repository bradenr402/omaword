import QtQuick

// A reader mark beside or inside the text, filled in its ink: a pencil where
// a note is, a hash where tags are (as they are written, #grace).
Item {
    id: root
    property string kind: "note"
    property color ink: "white" // fallback: a theme without an accent (standalone tests)
    width: 18
    height: 18

    Canvas {
        // Supersampled, so the pencil's cuts survive small raster sizes.
        width: root.width * 3
        height: root.height * 3
        scale: 1 / 3
        transformOrigin: Item.TopLeft
        property string kind: root.kind
        property color ink: root.ink
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
            ctx.lineCap = "butt"
            ctx.beginPath()
            if (kind === "note") {
                // A pencil on the diagonal, point down-left; two cuts through
                // it mark the point's wood and the ferrule.
                ctx.translate(12, 12)
                ctx.rotate(Math.PI / 4)
                ctx.moveTo(-3.6, -11); ctx.lineTo(3.6, -11); ctx.lineTo(3.6, 5); ctx.lineTo(0, 11); ctx.lineTo(-3.6, 5); ctx.closePath()
                ctx.fill()
                ctx.globalCompositeOperation = "destination-out"
                ctx.beginPath()
                ctx.lineWidth = 1.6
                ctx.moveTo(-4, 4.6); ctx.lineTo(4, 4.6)
                ctx.moveTo(-4, -7); ctx.lineTo(4, -7)
                ctx.stroke()
                ctx.globalCompositeOperation = "source-over"
            } else if (kind === "tag") {
                // A hash, slanted.
                ctx.lineWidth = 2.4
                ctx.moveTo(9, 2); ctx.lineTo(6.5, 22); ctx.moveTo(18, 2); ctx.lineTo(15.5, 22)
                ctx.moveTo(2.5, 8); ctx.lineTo(22, 8); ctx.moveTo(2, 16); ctx.lineTo(21.5, 16)
                ctx.stroke()
            }
        }
    }
}
