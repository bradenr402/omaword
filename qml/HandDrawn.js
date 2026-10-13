.pragma library
// Deterministic hand-drawn paths for annotation and search marks.
//
// Every shape derives from a stable seed (see seed()), never Math.random(),
// so repaints, scrolling and re-layout reproduce identical pixels. All shapes
// stay inside the geometry they are given: waviness is folded inward, and
// irregular ends only shorten a stroke.

// FNV-1a, 32-bit.
function hash(text) {
    let h = 0x811c9dc5
    const s = String(text)
    for (let i = 0; i < s.length; ++i) {
        h ^= s.charCodeAt(i)
        h = Math.imul(h, 0x01000193) >>> 0
    }
    return h >>> 0
}

// seed("highlight", version, reference, segment) -> uint32
function seed() {
    return hash(Array.prototype.slice.call(arguments).join("\u241f"))
}

function mulberry32(a) {
    let state = a >>> 0
    return function() {
        state = (state + 0x6D2B79F5) >>> 0
        let t = state
        t = Math.imul(t ^ (t >>> 15), t | 1)
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61)
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296
    }
}

// Default pen amplitude: 0.6–1.2 px at scale 1.
function amplitude(s, scale) {
    return (0.6 + 0.6 * mulberry32(s ^ 0x9e3779b9)()) * (scale > 0 ? scale : 1)
}

// The highlight accent's pen wobble (its perpendicular amplitude). Margin-note
// braces reuse it with linePoints so both read as the same steady pen.
function accentAmp(scale) {
    return 0.6 * (scale > 0 ? scale : 1)
}

// Two low-frequency sines; |value| <= 1. Wavelengths are in device-independent
// pixels multiplied by scale so the shape scales with the app.
function _wave(rand, scale) {
    const k = scale > 0 ? scale : 1
    return {
        f1: 1 / ((70 + 50 * rand()) * k), p1: rand() * Math.PI * 2,
        f2: 1 / ((24 + 16 * rand()) * k), p2: rand() * Math.PI * 2
    }
}
function _at(w, d) {
    return 0.72 * Math.sin(Math.PI * 2 * w.f1 * d + w.p1) + 0.28 * Math.sin(Math.PI * 2 * w.f2 * d + w.p2)
}
function _smooth(t) {
    t = Math.max(0, Math.min(1, t))
    return t * t * (3 - 2 * t)
}

// Centre-line samples {x, y, w} for a pen stroke from (x1,y1) to (x2,y2).
// opts: width (base stroke width), amp (max perpendicular offset),
// joinStart/joinEnd (the stroke continues into a neighbour there: zero offset,
// exact endpoint and base width so pieces meet), roughStart/roughEnd
// (defaults to !join*: slightly shortened, thinner, irregular end).
function linePoints(x1, y1, x2, y2, s, scale, opts) {
    opts = opts || {}
    const k = scale > 0 ? scale : 1
    const rand = mulberry32(s)
    const dx = x2 - x1, dy = y2 - y1
    const length = Math.sqrt(dx * dx + dy * dy)
    const width = opts.width === undefined ? k : opts.width
    if (length < 0.001) return [{ x: x1, y: y1, w: width }, { x: x2, y: y2, w: width }]
    const ux = dx / length, uy = dy / length
    const nx = -uy, ny = ux
    const amp = Math.min(opts.amp === undefined ? amplitude(s, k) : opts.amp, length / 4)
    const wave = _wave(rand, k), thickness = _wave(rand, k)
    const roughStart = opts.roughStart === undefined ? !opts.joinStart : opts.roughStart
    const roughEnd = opts.roughEnd === undefined ? !opts.joinEnd : opts.roughEnd
    // Irregular ends: shorten only, so ink never passes the given endpoints.
    const room = Math.max(0, length / 6)
    const cut0 = roughStart ? Math.min(room, width * 0.5 + rand() * 0.6 * k) : 0
    const cut1 = roughEnd ? Math.min(room, width * 0.5 + rand() * 0.6 * k) : 0
    const from = cut0, to = length - cut1
    const taper = Math.min(10 * k, length / 2)
    const step = Math.max(1, 3 * k)
    const count = Math.max(2, Math.ceil((to - from) / step) + 1)
    const points = []
    for (let i = 0; i < count; ++i) {
        const d = from + (to - from) * i / (count - 1)
        let envelope = 1
        if (opts.joinStart) envelope *= _smooth(d / taper)
        if (opts.joinEnd) envelope *= _smooth((length - d) / taper)
        const offset = amp * envelope * _at(wave, d)
        let w = width * (1 + 0.14 * envelope * _at(thickness, d))
        if (roughStart && i === 0) w *= 0.78
        if (roughEnd && i === count - 1) w *= 0.78
        points.push({ x: x1 + ux * d + nx * offset, y: y1 + uy * d + ny * offset, w: w })
    }
    return points
}

// Fill a variable-width stroke along samples. Rounded-ish caps stay inside
// the samples' end points (they were shortened by at least half a width).
function _fillStroke(ctx, points, roundStart, roundEnd) {
    const n = points.length
    if (n < 2) return
    const left = [], right = []
    for (let i = 0; i < n; ++i) {
        const a = points[Math.max(0, i - 1)], b = points[Math.min(n - 1, i + 1)]
        let tx = b.x - a.x, ty = b.y - a.y
        const len = Math.sqrt(tx * tx + ty * ty) || 1
        tx /= len; ty /= len
        const h = points[i].w / 2
        left.push({ x: points[i].x - ty * h, y: points[i].y + tx * h })
        right.push({ x: points[i].x + ty * h, y: points[i].y - tx * h })
    }
    const dir = function(i, j) {
        const dx = points[j].x - points[i].x, dy = points[j].y - points[i].y
        const len = Math.sqrt(dx * dx + dy * dy) || 1
        return { x: dx / len, y: dy / len }
    }
    ctx.beginPath()
    ctx.moveTo(left[0].x, left[0].y)
    for (let i = 1; i < n; ++i) ctx.lineTo(left[i].x, left[i].y)
    if (roundEnd) {
        const d = dir(n - 2, n - 1), h = points[n - 1].w / 2
        ctx.quadraticCurveTo(points[n - 1].x + d.x * h * 0.9, points[n - 1].y + d.y * h * 0.9, right[n - 1].x, right[n - 1].y)
    } else ctx.lineTo(right[n - 1].x, right[n - 1].y)
    for (let i = n - 2; i >= 0; --i) ctx.lineTo(right[i].x, right[i].y)
    if (roundStart) {
        const d = dir(1, 0), h = points[0].w / 2
        ctx.quadraticCurveTo(points[0].x + d.x * h * 0.9, points[0].y + d.y * h * 0.9, left[0].x, left[0].y)
    }
    ctx.closePath()
    ctx.fill()
}

// Pen line in ctx.fillStyle. Returns the centre-line samples.
function wavyLine(ctx, x1, y1, x2, y2, s, scale, opts) {
    opts = opts || {}
    const points = linePoints(x1, y1, x2, y2, s, scale, opts)
    const roughStart = opts.roughStart === undefined ? !opts.joinStart : opts.roughStart
    const roughEnd = opts.roughEnd === undefined ? !opts.joinEnd : opts.roughEnd
    if (ctx) _fillStroke(ctx, points, roughStart, roughEnd)
    return points
}

// Connected pen strokes through vertices; corners meet exactly.
function wavyPolyline(ctx, vertices, s, scale, opts) {
    opts = opts || {}
    const all = []
    for (let i = 0; i + 1 < vertices.length; ++i) {
        const a = vertices[i], b = vertices[i + 1]
        // Overlap at interior corners by half a pen width (like a miter).
        const dx = b.x - a.x, dy = b.y - a.y
        const len = Math.sqrt(dx * dx + dy * dy) || 1
        const half = (opts.width === undefined ? (scale > 0 ? scale : 1) : opts.width) / 2
        const e0 = i > 0 ? half : 0, e1 = i + 2 < vertices.length ? half : 0
        const o = Object.assign({}, opts, {
            joinStart: i > 0, joinEnd: i + 2 < vertices.length,
            roughStart: i === 0 ? opts.roughStart : false,
            roughEnd: i + 2 === vertices.length ? opts.roughEnd : false
        })
        if (i === 0 && o.roughStart === undefined) o.roughStart = true
        if (i + 2 === vertices.length && o.roughEnd === undefined) o.roughEnd = true
        const segmentSeed = seed(s, i)
        all.push(wavyLine(ctx, a.x - dx / len * e0, a.y - dy / len * e0,
            b.x + dx / len * e1, b.y + dy / len * e1, segmentSeed, scale, o))
    }
    return all
}

// Pen outline inside (x, y, w, h).
function wavyRect(ctx, x, y, w, h, s, scale, opts) {
    opts = opts || {}
    const k = scale > 0 ? scale : 1
    const width = opts.width === undefined ? 1 : opts.width
    const amp = Math.min(opts.amp === undefined ? amplitude(s, k) : opts.amp, w / 6, h / 6)
    const inset = amp + width * 0.6
    const l = x + inset, t = y + inset, r = x + w - inset, b = y + h - inset
    if (r <= l || b <= t) return []
    const o = { width: width, amp: amp, roughStart: false, roughEnd: false }
    return [
        wavyLine(ctx, l, t, r, t, seed(s, "top"), k, o),
        wavyLine(ctx, r, t, r, b, seed(s, "right"), k, o),
        wavyLine(ctx, r, b, l, b, seed(s, "bottom"), k, o),
        wavyLine(ctx, l, b, l, t, seed(s, "left"), k, o)
    ]
}

// Closed highlighter outline inside rect {x, y, width, height}.
// opts.top/bottom: uneven horizontal edges; opts.left/right: softer, rounded,
// irregular ends. Edges that are false stay exactly on the rect so pieces of a
// continuing band join seamlessly.
function bandPolygon(rect, s, scale, opts) {
    opts = opts || {}
    const k = scale > 0 ? scale : 1
    const x = rect.x, y = rect.y, w = rect.width, h = rect.height
    if (w <= 0 || h <= 0) return []
    const rand = mulberry32(s)
    const amp = Math.min(opts.amp === undefined ? amplitude(s, k) : opts.amp, h / 6, w / 2)
    const topWave = _wave(rand, k), bottomWave = _wave(rand, k)
    const leftWave = _wave(rand, k), rightWave = _wave(rand, k)
    const radius = function() { return Math.min((2 + 1.5 * rand()) * k, w / 4, h / 2) }
    const leftR = opts.left ? radius() : 0, rightR = opts.right ? radius() : 0
    const step = Math.max(1, 3 * k)
    const pts = []
    const edge = function(wave, d) { return amp * (0.5 + 0.5 * _at(wave, d)) } // [0, amp]
    const x0 = x + leftR, x1 = x + w - rightR
    const hCount = Math.max(2, Math.ceil((x1 - x0) / step) + 1)
    for (let i = 0; i < hCount; ++i) {
        const px = x0 + (x1 - x0) * i / (hCount - 1)
        pts.push({ x: px, y: y + (opts.top ? edge(topWave, px) : 0) })
    }
    const vCount = Math.max(3, Math.ceil(h / step) + 1)
    // Right end: a soft inward bulge from top to bottom.
    for (let i = 1; i < vCount - 1; ++i) {
        const t = i / (vCount - 1)
        const bulge = rightR * (1 - Math.sin(Math.PI * t)) + (opts.right ? 0.35 * edge(rightWave, y + h * t) : 0)
        pts.push({ x: x + w - bulge, y: y + h * t })
    }
    for (let i = hCount - 1; i >= 0; --i) {
        const px = x0 + (x1 - x0) * i / (hCount - 1)
        pts.push({ x: px, y: y + h - (opts.bottom ? edge(bottomWave, px) : 0) })
    }
    for (let i = vCount - 2; i > 0; --i) {
        const t = i / (vCount - 1)
        const bulge = leftR * (1 - Math.sin(Math.PI * t)) + (opts.left ? 0.35 * edge(leftWave, y + h * t) : 0)
        pts.push({ x: x + bulge, y: y + h * t })
    }
    return pts
}

// Annotation tints (paint only): each highlight, note, or tag range
// covering a verse adds one translucent layer of the accent, capped so text
// stays readable. Highlights still paint their stronger band on top.
function tintAlpha() { return 0.045 }
function tintLayers() { return 3 }
function tintCount(verse) {
    if (!verse) return 0
    const count = function(list) { return list ? list.length : 0 }
    return Math.min(tintLayers(), count(verse.highlightRanges) + count(verse.noteRanges) + count(verse.tagRanges))
}
// counts: per-verse layer counts in reading order -> [{layer, first, last}],
// the runs of consecutive indexes with at least `layer` layers.
function tintRuns(counts) {
    const runs = []
    for (let layer = 1; layer <= tintLayers(); ++layer) {
        let first = -1
        for (let i = 0; i <= counts.length; ++i) {
            const on = i < counts.length && counts[i] >= layer
            if (on && first < 0) first = i
            if (!on && first >= 0) { runs.push({ layer: layer, first: first, last: i - 1 }); first = -1 }
        }
    }
    return runs
}

// Highlighter band in ctx.fillStyle. Returns its polygon.
function highlighterBand(ctx, rect, s, scale, opts) {
    const pts = bandPolygon(rect, s, scale, opts)
    if (ctx && pts.length) {
        ctx.beginPath()
        ctx.moveTo(pts[0].x, pts[0].y)
        for (let i = 1; i < pts.length; ++i) ctx.lineTo(pts[i].x, pts[i].y)
        ctx.closePath()
        ctx.fill()
    }
    return pts
}

// The highlighter band of a line of text, from its font metrics: from a
// little above the x-height to a little below the baseline, offset from the
// line's top (text sits at the top of its line box). {top, height, xHeight}
function highlightBand(ascent, descent, xHeight) {
    const x = xHeight > 0 ? xHeight : ascent * 0.5
    const top = Math.max(0, ascent - 1.3 * x)
    return { top: top, height: ascent + 0.45 * descent - top, xHeight: x }
}

// Highlighter pen (paint only). One stroke per line of text, inside `rect`
// (the band about the x-height, see Main's highlightBand): a faint feathered
// pass, the core pass with a chisel tip (slanted ends) that tapers slightly
// at both ends, and a shorter second pass over part of the stroke, darker
// where the passes overlap, as a pen run twice. `ink` is a color, `alpha` the
// core's opacity. Deterministic per seed; every pass stays inside `bounds`
// (default: rect grown by the feather). Returns the core polygon.
function highlighterStroke(ctx, rect, ink, alpha, s, scale, opts) {
    opts = opts || {}
    const k = scale > 0 ? scale : 1
    if (!rect || rect.width <= 0 || rect.height <= 0) return []
    const rand = mulberry32(seed(s, "stroke"))
    const rgba = function(a) { return Qt.rgba(ink.r, ink.g, ink.b, a) }
    const grow = Math.min(1.2 * k, rect.height * 0.12)
    const fill = function(points, a) {
        if (!ctx || !points.length) return
        ctx.fillStyle = rgba(a)
        ctx.beginPath()
        ctx.moveTo(points[0].x, points[0].y)
        for (let i = 1; i < points.length; ++i) ctx.lineTo(points[i].x, points[i].y)
        ctx.closePath()
        ctx.fill()
    }
    // Feather: a faint, slightly larger pass with soft uneven edges.
    fill(strokePolygon({ x: rect.x, y: rect.y - grow, width: rect.width, height: rect.height + 2 * grow },
        seed(s, "feather"), k, 0.9 * k), alpha * 0.32)
    const core = strokePolygon(rect, seed(s, "core"), k, 0.7 * k)
    fill(core, alpha)
    // A second, thinner pass over part of the stroke.
    if (rect.width > 28 * k) {
        const from = rand() * 0.3, to = 0.55 + rand() * 0.45
        const h = rect.height * (0.55 + 0.2 * rand())
        const y = rect.y + (rect.height - h) * (0.3 + 0.4 * rand())
        fill(strokePolygon({ x: rect.x + rect.width * from, y: y, width: rect.width * (to - from), height: h },
            seed(s, "again"), k, 0.5 * k), alpha * 0.45)
    }
    return core
}

// The outline of one highlighter pass inside rect: uneven top and bottom
// edges, a chisel tip (the top edge starts and ends a little to the right of
// the bottom one) and ends that narrow slightly.
function strokePolygon(rect, s, scale, amp) {
    const k = scale > 0 ? scale : 1
    const x = rect.x, y = rect.y, w = rect.width, h = rect.height
    if (w <= 0 || h <= 0) return []
    const rand = mulberry32(s)
    const a = Math.min(amp, h / 5)
    const topWave = _wave(rand, k), bottomWave = _wave(rand, k)
    const slant = Math.min(h * (0.28 + 0.1 * rand()), w / 4)
    const taper = Math.min((6 + 4 * rand()) * k, w / 3)
    const narrow = h * (0.16 + 0.08 * rand())
    const edge = function(wave, d) { return a * (0.5 + 0.5 * _at(wave, d)) }
    const squeeze = function(px, left, right) {
        const u = Math.min(1, (px - left) / taper, (right - px) / taper)
        return narrow * (1 - u) * (1 - u) / 2
    }
    const step = Math.max(1, 3 * k)
    const count = Math.max(2, Math.ceil(w / step) + 1)
    const pts = []
    // Top edge, left to right, from x + slant to x + w.
    for (let i = 0; i < count; ++i) {
        const px = x + slant + (w - slant) * i / (count - 1)
        pts.push({ x: px, y: y + edge(topWave, px) + squeeze(px, x + slant, x + w) })
    }
    // Bottom edge, right to left, from x + w - slant to x.
    for (let i = count - 1; i >= 0; --i) {
        const px = x + (w - slant) * i / (count - 1)
        pts.push({ x: px, y: y + h - edge(bottomWave, px) - squeeze(px, x, x + w - slant) })
    }
    return pts
}

// Search matches: highlighter fill plus an inset pen outline for active (or
// outlined) matches. Active paint wins where matches overlap.
function searchMarks(ctx, rects, accent, outlined, scale, scope) {
    const k = scale > 0 ? scale : 1
    for (let active = 0; active < 2; ++active) {
        for (let i = 0; i < rects.length; ++i) {
            const r = rects[i]
            if (!!r.active !== !!active) continue
            const s = seed("search", scope, i, Math.round(r.x), Math.round(r.y), Math.round(r.width))
            const rect = { x: r.x, y: r.y, width: r.width, height: r.height }
            ctx.fillStyle = Qt.rgba(accent.r, accent.g, accent.b, active ? 0.55 : 0.28)
            highlighterBand(ctx, rect, s, k, { top: true, bottom: true, left: true, right: true, amp: 0.6 * k })
            if (active || outlined) {
                ctx.fillStyle = accent
                wavyRect(ctx, r.x, r.y, r.width, r.height, seed(s, "outline"), k, { width: 1, amp: 0.5 * k })
            }
        }
    }
}

// Centre-line samples of a large wavy curly brace inside the box
// [x, x + width] × [top, bottom]. direction -1 draws "{" (tip on the left at
// x), +1 draws "}" (tip on the right at x + width); the tip sits at tipY
// (clamped to braceTipBand–(1 - braceTipBand) of the brace's height, as
// Margins.tips does). The straight spines are the highlight accent's pen
// stroke (linePoints with accentAmp, joined at both ends) so their wobble
// and width variation match it exactly; the curls are exact, so the ends and
// the tip meet the returned `tip` point.
const braceTipBand = 0.2
function bracePoints(x, top, bottom, tipY, width, direction, s, scale, opts) {
    opts = opts || {}
    const k = scale > 0 ? scale : 1
    const h = Math.max(1, bottom - top)
    tipY = Math.max(top + braceTipBand * h, Math.min(bottom - braceTipBand * h, tipY))
    // Curls tighten when the tip nears an end.
    const r = Math.min(width / 2, h / 4, (tipY - top) / 2, (bottom - tipY) / 2)
    // Columns: ends at the open side, spine in the middle, tip at the far side.
    const open = direction < 0 ? x + width : x
    const spine = x + width / 2
    const point = direction < 0 ? x : x + width
    const pen = opts.width === undefined ? 1.3 * k : opts.width
    const line = { width: pen, amp: opts.amp === undefined ? accentAmp(k) : opts.amp, joinStart: true, joinEnd: true }
    // Quadratic curls, or a straight spine (null control point).
    const pieces = [
        [{ x: open, y: top }, { x: spine, y: top }, { x: spine, y: top + r }],
        [{ x: spine, y: top + r }, null, { x: spine, y: tipY - r }, "upper"],
        [{ x: spine, y: tipY - r }, { x: spine, y: tipY }, { x: point, y: tipY }],
        [{ x: point, y: tipY }, { x: spine, y: tipY }, { x: spine, y: tipY + r }],
        [{ x: spine, y: tipY + r }, null, { x: spine, y: bottom - r }, "lower"],
        [{ x: spine, y: bottom - r }, { x: spine, y: bottom }, { x: open, y: bottom }]
    ]
    const step = Math.max(1, 2.5 * k)
    const points = []
    for (let p = 0; p < pieces.length; ++p) {
        const a = pieces[p][0], c = pieces[p][1], b = pieces[p][2]
        if (!c) {
            const run = linePoints(a.x, a.y, b.x, b.y, seed(s, pieces[p][3]), k, line)
            for (let i = 1; i < run.length; ++i) points.push(run[i])
            continue
        }
        const count = Math.max(2, Math.ceil(Math.hypot(b.x - a.x, b.y - a.y) * 1.2 / step) + 1)
        for (let i = p === 0 ? 0 : 1; i < count; ++i) {
            const t = i / (count - 1)
            const end = p === 0 && i === 0 || p === pieces.length - 1 && i === count - 1
            points.push({ x: (1 - t) * (1 - t) * a.x + 2 * (1 - t) * t * c.x + t * t * b.x,
                y: (1 - t) * (1 - t) * a.y + 2 * (1 - t) * t * c.y + t * t * b.y, w: pen * (end ? 0.75 : 1) })
        }
    }
    return { points: points, tip: { x: point, y: tipY } }
}

function wavyBrace(ctx, x, top, bottom, tipY, width, direction, s, scale, opts) {
    const brace = bracePoints(x, top, bottom, tipY, width, direction, s, scale, opts)
    if (ctx) _fillStroke(ctx, brace.points, true, true)
    return brace
}

// Pen samples {x, y, w} along a cubic curve from p0 to p3 (controls p1, p2),
// wobbling as linePoints does along a line: the offset runs along the curve's
// normal, by arc length, with the same envelope, thickness, and ends.
function curvePoints(p0, p1, p2, p3, s, scale, opts) {
    opts = opts || {}
    const k = scale > 0 ? scale : 1
    const at = function(t) {
        const u = 1 - t
        return { x: u * u * u * p0.x + 3 * u * u * t * p1.x + 3 * u * t * t * p2.x + t * t * t * p3.x,
                 y: u * u * u * p0.y + 3 * u * u * t * p1.y + 3 * u * t * t * p2.y + t * t * t * p3.y }
    }
    // The curve, finely, with its arc length so far at each sample.
    const fine = [], steps = 96
    let length = 0
    for (let i = 0; i <= steps; ++i) {
        const p = at(i / steps)
        if (i > 0) length += Math.hypot(p.x - fine[i - 1].x, p.y - fine[i - 1].y)
        fine.push({ x: p.x, y: p.y, d: length })
    }
    const width = opts.width === undefined ? k : opts.width
    if (length < 0.001) return [{ x: p0.x, y: p0.y, w: width }, { x: p3.x, y: p3.y, w: width }]
    const rand = mulberry32(s)
    const amp = Math.min(opts.amp === undefined ? amplitude(s, k) : opts.amp, length / 4)
    const wave = _wave(rand, k), thickness = _wave(rand, k)
    const roughStart = opts.roughStart === undefined ? !opts.joinStart : opts.roughStart
    const roughEnd = opts.roughEnd === undefined ? !opts.joinEnd : opts.roughEnd
    const room = Math.max(0, length / 6)
    const cut0 = roughStart ? Math.min(room, width * 0.5 + rand() * 0.6 * k) : 0
    const cut1 = roughEnd ? Math.min(room, width * 0.5 + rand() * 0.6 * k) : 0
    const from = cut0, to = length - cut1
    const taper = Math.min(10 * k, length / 2)
    const count = Math.max(2, Math.ceil((to - from) / Math.max(1, 3 * k)) + 1)
    // The point and unit tangent at arc length d, between the fine samples.
    let j = 0
    const along = function(d) {
        while (j + 1 < fine.length - 1 && fine[j + 1].d < d) ++j
        const a = fine[j], b = fine[j + 1], span = Math.max(1e-9, b.d - a.d)
        const t = Math.max(0, Math.min(1, (d - a.d) / span)), len = Math.hypot(b.x - a.x, b.y - a.y) || 1
        return { x: a.x + (b.x - a.x) * t, y: a.y + (b.y - a.y) * t, ux: (b.x - a.x) / len, uy: (b.y - a.y) / len }
    }
    const points = []
    for (let i = 0; i < count; ++i) {
        const d = from + (to - from) * i / (count - 1)
        let envelope = 1
        if (opts.joinStart) envelope *= _smooth(d / taper)
        if (opts.joinEnd) envelope *= _smooth((length - d) / taper)
        const c = along(d)
        const offset = amp * envelope * _at(wave, d)
        let w = width * (1 + 0.14 * envelope * _at(thickness, d))
        if (roughStart && i === 0) w *= 0.78
        if (roughEnd && i === count - 1) w *= 0.78
        points.push({ x: c.x - c.uy * offset, y: c.y + c.ux * offset, w: w })
    }
    if (opts.joinEnd) points[points.length - 1] = { x: p3.x, y: p3.y, w: points[points.length - 1].w }
    return points
}

// A wavier pen arrow from (x1, y1) to (x2, y2), arrowhead at the end. Its
// shaft swoops: it leaves and arrives level, an S between ends at different
// heights (a straight line between ends at one height).
// Returns {shaft, head: [left, right]} samples.
function wavyArrow(ctx, x1, y1, x2, y2, s, scale, opts) {
    opts = opts || {}
    const k = scale > 0 ? scale : 1
    const pen = opts.width === undefined ? 1.2 * k : opts.width
    const length = Math.hypot(x2 - x1, y2 - y1)
    const amp = opts.amp === undefined ? Math.min(2.4 * k, length / 8) : opts.amp
    // Controls halfway across, level with each end.
    const reach = (x2 - x1) / 2
    const c1 = { x: x1 + reach, y: y1 }, c2 = { x: x2 - reach, y: y2 }
    const shaft = curvePoints({ x: x1, y: y1 }, c1, c2, { x: x2, y: y2 }, seed(s, "shaft"), k,
        { width: pen, amp: Math.min(amp, 1.2 * k), joinEnd: true, roughStart: true })
    if (ctx) _fillStroke(ctx, shaft, true, false)
    // The head follows the curve as it arrives: level, unless the ends are level too.
    const hx = x2 - c2.x, hy = y2 - c2.y, hl = Math.hypot(hx, hy)
    const ux = hl > 0 ? hx / hl : length > 0 ? (x2 - x1) / length : 1, uy = hl > 0 ? hy / hl : length > 0 ? (y2 - y1) / length : 0
    const size = Math.min(opts.head === undefined ? 6 * k : opts.head, Math.max(2, length / 2))
    const head = []
    for (const side of [-1, 1]) {
        const angle = side * 0.5
        const bx = -(ux * Math.cos(angle) - uy * Math.sin(angle)) * size
        const by = -(ux * Math.sin(angle) + uy * Math.cos(angle)) * size
        head.push(wavyLine(ctx, x2, y2, x2 + bx, y2 + by, seed(s, "head", side), k,
            { width: pen, amp: 0.3 * k, joinStart: true, roughEnd: true }))
    }
    return { shaft: shaft, head: head }
}
