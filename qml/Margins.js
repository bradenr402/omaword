.pragma library
// Pure margin geometry shared by the chapter outline (left) and margin notes
// (right): the space split, note placement, and connector tip offsets.
// Every function is deterministic and free of QML items, so tests can drive
// them directly.

// Splits the space beside the reading column. Margin notes have priority:
// they may shift the column left (never past minLeft) to reach their
// preferred width, and sit right after the connector; the outline uses only
// what is left of the shifted column and never moves it.
// o: width, columnX, columnWidth, minLeft, leftInset, rightInset,
//    wantNotes, connectorWidth, notesMin, notesPreferred, notesMax,
//    wantOutline, outlineGap, outlineMin, outlineMax
// -> { shift, columnX, notes: {visible, x, width}, outline: {visible, x, width} }
function space(o) {
    const result = { shift: 0, columnX: o.columnX,
        notes: { visible: false, x: 0, width: 0 }, outline: { visible: false, x: 0, width: 0 } }
    if (o.wantNotes) {
        const right = o.width - o.rightInset
        const room = right - (o.columnX + o.columnWidth + o.connectorWidth)
        const shift = Math.max(0, Math.min(o.notesPreferred - room, o.columnX - o.minLeft))
        const width = Math.min(o.notesMax, room + shift)
        if (width >= o.notesMin) {
            result.shift = shift
            result.columnX = o.columnX - shift
            // Hug the connector rather than the window edge: short arrows.
            result.notes = { visible: true, x: result.columnX + o.columnWidth + o.connectorWidth, width: width }
        }
    }
    if (o.wantOutline) {
        const room = result.columnX - o.outlineGap - o.leftInset
        const width = Math.min(o.outlineMax, room)
        if (width >= o.outlineMin)
            result.outline = { visible: true, x: result.columnX - o.outlineGap - width, width: width }
    }
    return result
}

// Allowed span for a note so that it and its passage fit one viewport:
// note top >= lo, note bottom <= hi. A passage taller than the viewport
// counts from its top.
function window(item, viewport, bounds) {
    const passageBottom = Math.min(item.bottom, item.top + viewport)
    return { lo: Math.max(bounds.top, passageBottom - viewport), hi: Math.min(bounds.bottom, item.top + viewport) }
}

// Places notes beside their passages.
// items: [{top, bottom, lines, lineHeight, pad}] (passage span in content
//   coordinates; the note's natural line count; height = lines * lineHeight + pad)
// opts: viewport, gap, upMax (how far a crowded note may rise above its
//   passage), minLines (default 1), bounds {top, bottom}
// -> per item (input order): {y, height, lines, truncated, hidden}
// Each note aligns with its passage top. Overlaps push later notes down, and
// a crowded run rises slightly (half its mean displacement); no note ever
// rises more than upMax above its passage. Every note must stay within its
// passage's viewport window; if that cannot be met, the longest note of the
// offending run loses one line at a time (ending with "…") until it can. A
// note that cannot fit even at minLines is hidden.
function place(items, opts) {
    const n = items.length
    const viewport = Math.max(1, opts.viewport)
    const gap = opts.gap || 0
    const upMax = opts.upMax || 0
    const minLines = Math.max(1, opts.minLines || 1)
    const bounds = opts.bounds || { top: -Infinity, bottom: Infinity }
    const order = []
    for (let i = 0; i < n; ++i) order.push(i)
    order.sort(function(a, b) { return items[a].top - items[b].top || items[a].bottom - items[b].bottom || a - b })
    const lines = items.map(function(item) { return Math.max(minLines, Math.round(item.lines)) })
    const hidden = items.map(function() { return false })
    // A note may rise at most upMax above its passage top.
    const windows = items.map(function(item) {
        const span = window(item, viewport, bounds)
        span.lo = Math.max(span.lo, item.top - upMax)
        return span
    })
    const height = function(i) { return lines[i] * items[i].lineHeight + (items[i].pad || 0) }
    let y = []
    const eps = 0.5
    function solve() {
        y = items.map(function() { return 0 })
        const shown = order.filter(function(i) { return !hidden[i] })
        // Down: align, then push below the previous note.
        let bottom = -Infinity
        for (const i of shown) {
            y[i] = Math.max(items[i].top, windows[i].lo, bottom + gap)
            bottom = y[i] + height(i)
        }
        // Up a little: each crowded run rises by half its mean displacement,
        // bounded by upMax, its notes' windows, and the run above it.
        let start = 0
        let previousBottom = -Infinity
        while (start < shown.length) {
            let end = start
            while (end + 1 < shown.length && y[shown[end + 1]] - (y[shown[end]] + height(shown[end])) <= gap + eps) ++end
            if (end > start) {
                let displaced = 0
                let rise = upMax
                for (let k = start; k <= end; ++k) {
                    const i = shown[k]
                    displaced += y[i] - items[i].top
                    rise = Math.min(rise, y[i] - windows[i].lo)
                }
                rise = Math.max(0, Math.min(rise, displaced / (end - start + 1) / 2, y[shown[start]] - (previousBottom + gap)))
                for (let k = start; k <= end; ++k) y[shown[k]] -= rise
            }
            previousBottom = y[shown[end]] + height(shown[end])
            start = end + 1
        }
        // Up where needed: no note may end below its window.
        let nextTop = Infinity
        for (let k = shown.length - 1; k >= 0; --k) {
            const i = shown[k]
            y[i] = Math.min(y[i], windows[i].hi - height(i), nextTop - gap - height(i))
            nextTop = y[i]
        }
        // First violation (a note pushed above its window), with its run.
        for (let k = 0; k < shown.length; ++k) {
            const i = shown[k]
            if (y[i] >= windows[i].lo - eps && y[i] + height(i) <= windows[i].hi + eps) continue
            let first = k, last = k
            while (first > 0 && y[shown[first]] - (y[shown[first - 1]] + height(shown[first - 1])) <= gap + eps) --first
            while (last + 1 < shown.length && y[shown[last + 1]] - (y[shown[last]] + height(shown[last])) <= gap + eps) ++last
            return { violator: i, run: shown.slice(first, last + 1) }
        }
        return null
    }
    let guard = 0
    for (let violation = solve(); violation && guard < 100000; violation = solve(), ++guard) {
        let longest = -1
        for (const i of violation.run) {
            if (lines[i] <= minLines) continue
            if (longest < 0 || height(i) > height(longest) || (height(i) === height(longest) && i > longest)) longest = i
        }
        if (longest >= 0) --lines[longest]
        else hidden[violation.violator] = true
    }
    return items.map(function(item, i) {
        return { y: y[i], height: height(i), lines: lines[i], truncated: lines[i] < Math.round(item.lines), hidden: hidden[i] }
    })
}

// Connector tips lean toward each other: a note below its passage raises
// its "{" tip and lowers the passage's "}" tip (and vice versa), each by
// factor * the centres' offset, clamped inside the middle of its brace.
// -> {note, passage}: offsets from each span's centre (positive = down).
function tips(note, passage, opts) {
    opts = opts || {}
    const factor = opts.factor === undefined ? 0.35 : opts.factor
    const inset = opts.inset === undefined ? 0 : opts.inset
    const offset = (note.top + note.bottom) / 2 - (passage.top + passage.bottom) / 2
    const clamp = function(span, value) {
        const h = span.bottom - span.top
        const limit = Math.max(0, h / 2 - Math.max(inset, 0.2 * h))
        return Math.max(-limit, Math.min(limit, value))
    }
    return { note: clamp(note, -factor * offset), passage: clamp(passage, factor * offset) }
}

// Current section: the last heading at or before `verse`; 0 before the first.
function sectionOf(headings, verse) {
    let current = 0
    for (const heading of headings) if (heading.verse <= verse) current = heading.verse
    return current
}

// Target heading for `{` (direction -1) / `}` (+1) with a count, from `verse`.
// -> {verse, moved, short}: moved < count when the chapter ran out (short).
// With `top` (the chapter's first verse), `{` ends at the chapter top: it is
// the last stop above, merged with a heading on that verse.
function headingStep(headings, verse, direction, count, top) {
    const candidates = headings.map(function(h) { return h.verse })
        .filter(function(v) { return direction > 0 ? v > verse : v < verse })
    if (direction < 0 && top !== undefined && top < verse && candidates.indexOf(top) < 0) candidates.unshift(top)
    if (direction < 0) candidates.reverse()
    if (!candidates.length) return { verse: 0, moved: 0, short: true }
    const moved = Math.min(count, candidates.length)
    return { verse: candidates[moved - 1], moved: moved, short: moved < count }
}
