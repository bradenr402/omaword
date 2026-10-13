import QtQuick
import QtQuick.Controls
import "HandDrawn.js" as HandDrawn

// The Verse layout's rail beside a verse for the tag or note ranges over it:
// a hand-drawn line with an end cap and the kind's glyph where a range
// starts; clicking it opens the range (or a menu of them).
ToolButton {
    id: marker
    // The reader window (Main) it belongs to, and the live theme.
    required property var controller
    required property var theme
    required property var backend
    // The menu that lists the ranges when several start here.
    required property var rangeMenu
    required property string kind
    required property int number
    required property int rowIndex
    required property var ranges
    required property real headingHeight
    required property real bodyHeight
    readonly property bool starts: ranges.some(function(r) { return r[kind + "Start"] })
    readonly property bool ends: ranges.some(function(r) { return r[kind + "End"] })
    readonly property bool continuesBefore: ranges.some(function(r) { return r.continuesBefore || r[kind + "ContinuesBefore"] })
    readonly property bool continuesAfter: ranges.some(function(r) { return r.continuesAfter || r[kind + "ContinuesAfter"] })
    readonly property var entries: ranges.filter(function(r) { return r[kind + "Start"] || (rowIndex === 0 && (r.continuesBefore || r[kind + "ContinuesBefore"])) })
    readonly property var choices: entries.length ? entries : ranges
    readonly property string hint: ranges.map(function(r) {
        return r.reference + (marker.kind === "tag" ? " · " + marker.controller.tagsPlainText(r.tags) : "")
    }).join("\n")
    width: 16 * marker.controller.readerScale
    // Mouse only: the passage menu's Open note / tags reach the same ranges by key.
    focusPolicy: Qt.NoFocus
    // Each row's rail lanes (Main.packAnnotationLanes): the chapter read's,
    // unless a chapter shown beside gives its own.
    property var lanes: marker.controller.annotationLanes
    readonly property int lane: (marker.lanes[rowIndex] || {})[kind] || 0
    x: (36 - lane * 18) * marker.controller.readerScale
    padding: 0
    visible: ranges.length > 0 && !marker.backend.focusMode
    Accessible.name: hint
    function openMarker() {
        if (choices.length === 1) {
            if (kind === "tag") marker.controller.openPassageTags(choices[0])
            else marker.controller.openNoteScope(choices[0])
        } else {
            marker.rangeMenu.annotationKind = kind
            marker.rangeMenu.entries = choices
            const p = mapToItem(marker.controller.contentItem, width, headingHeight)
            marker.rangeMenu.origin = mapToItem(null, width, headingHeight)
            marker.rangeMenu.popup(p.x, p.y)
        }
    }
    onClicked: openMarker()
    MouseArea {
        objectName: marker.objectName + "Hit"
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: function(mouse) {
            if (mouse.button === Qt.RightButton) {
                const p = mapToItem(null, mouse.x, mouse.y)
                marker.controller.openMarkerMenu(marker.choices, marker.kind, function() { marker.openMarker() }, p.x, p.y)
            } else marker.openMarker()
        }
    }
    readonly property int markSeed: HandDrawn.seed(kind, marker.backend.versionId,
        ranges.map(function(r) { return r.reference }).sort().join(","))
    contentItem: Item {
        // Geometry hooks for the rail and end cap; the ink is the
        // hand-drawn pen stroke painted by railPaint inside this item.
        Item {
            id: railLine
            objectName: marker.kind + "Line" + marker.number
            x: 5 * marker.controller.readerScale
            y: marker.continuesBefore ? 0 : marker.headingHeight + 8 * marker.controller.readerScale
            width: marker.controller.readerScale
            height: Math.max(0, (marker.continuesAfter ? parent.height : marker.headingHeight + marker.bodyHeight) - y)
        }
        Item {
            id: railEndCap
            objectName: marker.kind + "EndCap" + marker.number
            x: 5 * marker.controller.readerScale; y: marker.headingHeight + marker.bodyHeight - marker.controller.readerScale
            width: 7 * marker.controller.readerScale; height: marker.controller.readerScale
            visible: marker.ends
        }
        Canvas {
            id: railPaint
            objectName: marker.kind + "Rail" + marker.number
            anchors.fill: parent
            property color ink: marker.theme.accent
            readonly property var paintKey: [railLine.y, railLine.height, railEndCap.y, railEndCap.visible,
                marker.continuesBefore, marker.continuesAfter, marker.markSeed, marker.controller.readerScale, width, height]
            onPaintKeyChanged: requestPaint()
            onInkChanged: requestPaint()
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                const k = marker.controller.readerScale
                const cx = railLine.x + railLine.width / 2
                if (railLine.height > 0) {
                    ctx.fillStyle = Qt.rgba(ink.r, ink.g, ink.b, 0.65)
                    HandDrawn.wavyLine(ctx, cx, railLine.y, cx, railLine.y + railLine.height,
                        HandDrawn.seed(marker.markSeed, "rail"), k,
                        { width: k, joinStart: marker.continuesBefore, joinEnd: marker.continuesAfter })
                }
                if (railEndCap.visible) {
                    const cy = railEndCap.y + railEndCap.height / 2
                    ctx.fillStyle = ink
                    HandDrawn.wavyLine(ctx, railEndCap.x, cy, railEndCap.x + railEndCap.width, cy,
                        HandDrawn.seed(marker.markSeed, "cap"), k, { width: k, amp: 0.6 * k, roughStart: false })
                }
            }
        }
        AnnotationGlyph {
            objectName: marker.kind + "Glyph" + marker.number
            kind: marker.kind; ink: marker.theme.accent
            y: marker.headingHeight + 5 * marker.controller.readerScale
            width: 14 * marker.controller.readerScale; height: 16 * marker.controller.readerScale
            visible: marker.starts || (marker.rowIndex === 0 && marker.continuesBefore)
        }
    }
    background: Rectangle {
        color: marker.hovered || marker.activeFocus ? marker.controller.tint(marker.theme.accent, 0.12) : "transparent"
        radius: Math.min(marker.theme.cornerRadius, width / 2, height / 2)
    }
    AppToolTip { theme: marker.theme; uiScale: marker.controller.scale; visible: marker.visible && (marker.hovered || marker.activeFocus); text: marker.hint }
}
