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

// seed("bookmark", version, reference, segment) -> uint32
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
