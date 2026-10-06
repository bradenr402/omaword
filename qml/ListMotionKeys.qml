import QtQuick

// Shared list motions for the side panels, mirroring the reader: g (home)
// first, shift G (end) last, ctrl d/u half a page (5 items), ctrl f/b and pgdn/pgup a page
// (10 items). `move` receives a clamped-by-the-list delta. Invisible, so
// layouts skip it; its shortcuts still follow `active`.
Item {
    id: motion
    property bool active: false
    property int count: 0
    signal move(int delta)
    visible: false
    width: 0
    height: 0
    Shortcut { sequences: ["G", "Home"]; enabled: motion.active; onActivated: motion.move(-Math.max(1, motion.count)) }
    Shortcut { sequences: ["Shift+G", "End"]; enabled: motion.active; onActivated: motion.move(Math.max(1, motion.count)) }
    Shortcut { sequence: "Ctrl+D"; enabled: motion.active; onActivated: motion.move(5) }
    Shortcut { sequence: "Ctrl+U"; enabled: motion.active; onActivated: motion.move(-5) }
    Shortcut { sequences: ["Ctrl+F", "PgDown"]; enabled: motion.active; onActivated: motion.move(10) }
    Shortcut { sequences: ["Ctrl+B", "PgUp"]; enabled: motion.active; onActivated: motion.move(-10) }
}
