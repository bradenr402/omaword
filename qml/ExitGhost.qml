pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
// qmllint disable import
import OmaWord.Reader 1.0
// qmllint enable import

// The exit motion of popups that close at once (modals, context menus).
// The popup itself has no exit transition: the moment it closes it stops
// counting as open, the keyboard goes back, and it can reopen (or another
// one open) immediately. This ghost, a still picture of the popup taken as it
// closes (ExitSnapshot, which keeps no reference to the popup), fades and
// shrinks slightly toward `origin` on its own and never takes input. Two
// pictures take turns, so a second close during an exit leaves the first.
//
// Owners call release(popup, origin) from the popup's onAboutToHide; origin
// is a scene point (null = the popup's centre).
Item {
    id: ghost
    property int duration: 120
    property real endScale: 0.96
    // A modal's dim layer fades out with it, unless another modal dims the
    // window already (suppressDim).
    property color dimColor: "transparent"
    property bool dimmed: false
    property bool suppressDim: false
    readonly property bool running: first.shown || second.shown
    // The last exit, for tests: {x, y, width, height, originX, originY}.
    property var lastExit: null
    // How many exits have started (tests).
    property int exits: 0
    property int turn: 0
    property real popupZ: 0

    parent: Overlay.overlay
    anchors.fill: parent
    // Below the popups (a new menu or modal shows above the old one's ghost).
    z: popupZ - 0.5
    enabled: false

    // Animations turned off mid-exit: the pictures go at once.
    onDurationChanged: if (duration <= 0) { first.stop(); second.stop() }
    function release(popup, origin) {
        if (duration <= 0 || !popup) return
        const source = popup.background ? popup.background.parent : null
        if (!source || !source.visible || source.width <= 0 || source.height <= 0) return
        const shot = turn === 0 ? first : second
        if (!shot.picture.capture(source)) return
        turn = 1 - turn
        const at = source.mapToItem(ghost, 0, 0)
        const o = origin ? ghost.mapFromItem(null, origin.x, origin.y) : Qt.point(at.x + source.width / 2, at.y + source.height / 2)
        // The origin, clamped to the popup.
        const ox = Math.max(0, Math.min(source.width, o.x - at.x)), oy = Math.max(0, Math.min(source.height, o.y - at.y))
        popupZ = popup.z
        exits += 1
        lastExit = { x: at.x, y: at.y, width: source.width, height: source.height, originX: at.x + ox, originY: at.y + oy }
        shot.start(at.x, at.y, source.width, source.height, ox, oy, dimmed && popup.modal)
    }

    component Shot: Item {
        id: shot
        property bool shown: false
        property bool withDim: false
        readonly property alias picture: picture
        anchors.fill: parent
        visible: shown
        function stop() { motion.stop(); shown = false; picture.clear() }
        function start(x, y, width, height, ox, oy, dim) {
            picture.x = x; picture.y = y; picture.width = width; picture.height = height
            zoom.origin.x = ox; zoom.origin.y = oy
            withDim = dim
            shown = true
            motion.restart()
        }
        Rectangle {
            anchors.fill: parent
            visible: shot.withDim && !ghost.suppressDim
            color: ghost.dimColor
            opacity: picture.opacity
        }
        // qmllint disable import
        ExitSnapshot {
            id: picture
            objectName: "exitGhostPicture"
            transform: Scale { id: zoom }
        }
        // qmllint enable import
        ParallelAnimation {
            id: motion
            // qmllint disable incompatible-type
            NumberAnimation { target: picture; property: "opacity"; from: 1; to: 0; duration: ghost.duration; easing.type: Easing.InQuad }
            // qmllint enable incompatible-type
            NumberAnimation { target: zoom; properties: "xScale,yScale"; from: 1; to: ghost.endScale; duration: ghost.duration; easing.type: Easing.OutQuad }
            onFinished: { shot.shown = false; picture.clear() }
        }
    }
    Shot { id: first; objectName: "exitGhostFirst" }
    Shot { id: second; objectName: "exitGhostSecond" }
}
