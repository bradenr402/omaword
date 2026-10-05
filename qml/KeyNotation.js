.pragma library
// One key notation for every place a key is shown: the keybindings list,
// tooltips, hint rows, context menus, and the README (bin/shortcuts mirrors
// these rules). Actions store Qt sequences; display text is always generated.
//
// Rules: lowercase modifiers and named keys; arrows as ↑ ↓ ← →; a letter is
// uppercase only when Shift is part of the combo ("shift D", "ctrl k");
// chords and counts join without spaces ("zz", "20g"); mouse gestures are
// spelled out ("right click"). Alternatives are separated by a muted "·".

var named = {
    "escape": "esc", "esc": "esc", "return": "enter", "enter": "enter",
    "delete": "delete", "del": "delete", "backspace": "backspace", "space": "space",
    "tab": "tab", "backtab": "tab", "pgup": "pgup", "pageup": "pgup", "pgdown": "pgdn",
    "pagedown": "pgdn", "pgdn": "pgdn", "home": "home", "end": "end", "menu": "menu",
    "up": "↑", "down": "↓", "left": "←", "right": "→",
    "click": "click", "rightclick": "right click", "middleclick": "middle click",
    "backclick": "mouse back", "forwardclick": "mouse forward"
}
var separator = " · "

function escapeHtml(text) {
    return String(text).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
}

function formatKey(token, shifted) {
    var lower = token.toLowerCase()
    if (named.hasOwnProperty(lower)) return named[lower]
    if (/^f\d{1,2}$/.test(lower)) return token.toUpperCase()
    if (/^[a-z]$/i.test(token)) return shifted ? token.toUpperCase() : token.toLowerCase()
    return token
}

// One combo or chord: "Ctrl+K", "Shift+D", "Ctrl++", "Z, Z", "2, 0, G".
function format(sequence) {
    var text = String(sequence)
    // Inside chords and counts a shifted letter is just its capital ("20G").
    if (text.indexOf(", ") > 0) return text.split(", ").map(function(part) {
        return /^Shift\+[A-Za-z]$/.test(part) ? part.slice(-1).toUpperCase() : format(part)
    }).join("")
    var key = text
    var modifiers = []
    if (text.length > 1 && text.slice(-2) === "++") {
        key = "+"
        modifiers = text.slice(0, -2).split("+")
    } else if (text.indexOf("+") > 0) {
        var parts = text.split("+")
        key = parts.pop()
        modifiers = parts
    }
    modifiers = modifiers.filter(function(modifier) { return modifier.length > 0 })
        .map(function(modifier) { return modifier.toLowerCase() })
    var shifted = modifiers.indexOf("shift") >= 0
    return modifiers.concat([formatKey(key, shifted)]).join(" ")
}

// Plain alternatives, for tooltips, menus, and searching.
function formatList(sequences) {
    return Array.from(sequences || []).map(format).join(separator)
}

// Rich alternatives: keys in the foreground, the separator muted.
function richList(sequences, foreground, muted) {
    return Array.from(sequences || []).map(function(sequence) {
        return '<font color="' + foreground + '">' + escapeHtml(format(sequence)) + "</font>"
    }).join('<font color="' + muted + '">' + escapeHtml(separator) + "</font>")
}

// Hint rows: parts are [sequences, label, run, enabled]; several sequences
// in one part read as a set ("↑ ↓ choose"). Pairs never break internally.
// `run` (optional) makes the part clickable in HintRow while `enabled`
// (default true) holds; parts without it are display only.
function hintParts(parts) {
    return Array.from(parts || []).filter(function(part) { return !!part }).map(function(part) {
        var keys = typeof part[0] === "string" ? [part[0]] : Array.from(part[0])
        return { keys: keys.map(format).join(" "), label: part[1], sequences: keys,
            run: typeof part[2] === "function" ? part[2] : null, enabled: part[3] !== false }
    })
}
function hintPlain(parts) {
    return hintParts(parts).map(function(part) { return part.keys + " " + part.label }).join("    ")
}
function hint(parts, foreground, muted) {
    return hintParts(parts).map(function(part) {
        return '<font color="' + foreground + '">' + escapeHtml(part.keys).replace(/ /g, "&nbsp;") + "</font>"
            + '<font color="' + muted + '">&nbsp;' + escapeHtml(part.label).replace(/ /g, "&nbsp;") + "</font>"
    }).join("&nbsp;&nbsp;&nbsp; ")
}
