.pragma library
.import "KeyNotation.js" as KeyNotation

// One context menu for every list row (Marks, History, See also, the
// palette's results and recents, Notes' Linked from): Open · Copy · Copy
// plain text · Add to Desk, then the row's own extras (Remove, …) after a
// divider. `spec`:
//   scope       canonical passage (copy and Desk act on it)
//   open        run for Open; canOpen false disables it
//   copy, copyPlain   overrides for the copies; canCopy false disables both
//   desk        override for the Desk entry (false: no Desk entry)
//   keys        false where the list does not bind y, shift Y, d
//   busy        the backend is busy
//   extras      further entries (null entries are skipped)
// `app` supplies copyScope and addToDesk.
function entries(app, spec) {
    var busy = !!spec.busy
    var ready = !busy
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
        list.push({ id: "desk", text: "Add to Desk", shortcut: key("D"),
            enabled: ready && (!!scope || typeof spec.desk === "function") && spec.canDesk !== false,
            run: spec.desk || function() { app.addToDesk(scope) } })
    }
    var extras = Array.from(spec.extras || []).filter(function(entry) { return !!entry })
    if (extras.length) list = list.concat([{ separator: true }], extras)
    return list
}
