.pragma library
.import "KeyNotation.js" as KeyNotation

// One context menu for every list row (Annotations, Desk, History, Study,
// the palette's results and recents, Notes' Linked from): Open · Copy ·
// Copy plain text · Desk, then the row's own extras (Edit range, Remove, …)
// after a divider. `spec`:
//   scope       canonical passage (copy and Desk act on it)
//   open        run for Open; canOpen false disables it
//   copy, copyPlain   overrides for the copies; canCopy false disables both
//   desk        override for the Desk entry (false: no Desk entry);
//               deskText / inDesk override its label
//   keys        false where the list does not bind y, shift Y, d
//   busy        the backend is busy
//   extras      further entries (null entries are skipped)
// `app` supplies rangeEditing, copyScope, toggleScopeDesk, and deskRowFor.
function entries(app, spec) {
    var busy = !!spec.busy
    var ready = !busy && !app.rangeEditing
    var scope = spec.scope || null
    var keys = spec.keys !== false
    function key(sequence) { return keys ? KeyNotation.format(sequence) : "" }
    var copyable = !busy && spec.canCopy !== false && (!!spec.copy || !!scope)
    var list = [
        { id: "open", text: spec.openText || "Open", shortcut: KeyNotation.format("Return"),
            enabled: ready && typeof spec.open === "function" && spec.canOpen !== false, run: spec.open },
        { id: "copy", text: "Copy", shortcut: key("Y"), enabled: copyable,
            run: spec.copy || function() { app.copyScope(scope) } },
        { id: "copyPlain", text: "Copy plain text", shortcut: key("Shift+Y"), enabled: copyable,
            run: spec.copyPlain || function() { app.copyScope(scope, true) } }
    ]
    if (spec.desk !== false) {
        var inDesk = spec.inDesk !== undefined ? !!spec.inDesk
            : !!scope && typeof app.deskRowFor === "function" && !!app.deskRowFor(scope)
        list.push({ id: "desk", text: spec.deskText || (inDesk ? "Remove from Desk" : "Add to Desk"), shortcut: key("D"),
            enabled: ready && (!!scope || typeof spec.desk === "function") && spec.canDesk !== false,
            run: spec.desk || function() { app.toggleScopeDesk(scope) } })
    }
    var extras = Array.from(spec.extras || []).filter(function(entry) { return !!entry })
    if (extras.length) list = list.concat([{ separator: true }], extras)
    return list
}
