#include "annotationpaint.h"
#include "brand.h"

#include <QCoreApplication>
#include <QPainter>
#include <QQmlEngine>
#include <cmath>

static void registerAnnotationPaint()
{
    static const auto module = (Brand::name() + QStringLiteral(".Reader")).toUtf8();
    qmlRegisterType<AnnotationPaint>(module.constData(), 1, 0, "AnnotationPaint");
}
Q_COREAPP_STARTUP_FUNCTION(registerAnnotationPaint)

namespace HandDrawn {
namespace {
// FNV-1a over UTF-16 code units, as HandDrawn.hash.
quint32 hash(const QString &text)
{
    quint32 h = 0x811c9dc5u;
    for (const QChar c : text) { h ^= c.unicode(); h *= 0x01000193u; }
    return h;
}
// HandDrawn.mulberry32.
struct Random {
    quint32 state;
    double operator()() {
        state += 0x6D2B79F5u;
        quint32 t = state;
        t = (t ^ (t >> 15)) * (t | 1u);
        t ^= t + (t ^ (t >> 7)) * (t | 61u);
        return double(t ^ (t >> 14)) / 4294967296.0;
    }
};
struct Wave { double f1, p1, f2, p2; };
Wave wave(Random &rand, qreal k)
{
    Wave w;
    w.f1 = 1 / ((70 + 50 * rand()) * k); w.p1 = rand() * M_PI * 2;
    w.f2 = 1 / ((24 + 16 * rand()) * k); w.p2 = rand() * M_PI * 2;
    return w;
}
double at(const Wave &w, double d)
{
    return 0.72 * std::sin(M_PI * 2 * w.f1 * d + w.p1) + 0.28 * std::sin(M_PI * 2 * w.f2 * d + w.p2);
}
quint32 child(quint32 seed, const char *name) { return HandDrawn::seed({QString::number(seed), QString::fromLatin1(name)}); }
}

quint32 seed(const QStringList &parts) { return hash(parts.join(QChar(0x241f))); }

QPolygonF strokePolygon(const QRectF &rect, quint32 s, qreal scale, qreal amp)
{
    const qreal k = scale > 0 ? scale : 1;
    const double x = rect.x(), y = rect.y(), w = rect.width(), h = rect.height();
    if (w <= 0 || h <= 0) return {};
    Random rand{s};
    const double a = std::min(amp, h / 5);
    const Wave top = wave(rand, k), bottom = wave(rand, k);
    const double slant = std::min(h * (0.28 + 0.1 * rand()), w / 4);
    const double taper = std::min((6 + 4 * rand()) * k, w / 3);
    const double narrow = h * (0.16 + 0.08 * rand());
    const auto edge = [&](const Wave &wv, double d) { return a * (0.5 + 0.5 * at(wv, d)); };
    const auto squeeze = [&](double px, double left, double right) {
        const double u = std::min({1.0, (px - left) / taper, (right - px) / taper});
        return narrow * (1 - u) * (1 - u) / 2;
    };
    const double step = std::max(1.0, 3 * k);
    const int count = std::max(2, int(std::ceil(w / step)) + 1);
    QPolygonF points;
    points.reserve(2 * count);
    for (int i = 0; i < count; ++i) {
        const double px = x + slant + (w - slant) * i / (count - 1);
        points << QPointF(px, y + edge(top, px) + squeeze(px, x + slant, x + w));
    }
    for (int i = count - 1; i >= 0; --i) {
        const double px = x + (w - slant) * i / (count - 1);
        points << QPointF(px, y + h - edge(bottom, px) - squeeze(px, x, x + w - slant));
    }
    return points;
}

QPolygonF bandPolygon(const QRectF &rect, quint32 s, qreal scale, bool top, bool bottom, bool left, bool right)
{
    const qreal k = scale > 0 ? scale : 1;
    const double x = rect.x(), y = rect.y(), w = rect.width(), h = rect.height();
    if (w <= 0 || h <= 0) return {};
    Random rand{s};
    Random amplitude{s ^ 0x9e3779b9u};
    const double amp = std::min({(0.6 + 0.6 * amplitude()) * k, h / 6, w / 2});
    const Wave topWave = wave(rand, k), bottomWave = wave(rand, k), leftWave = wave(rand, k), rightWave = wave(rand, k);
    const auto radius = [&] { return std::min({(2 + 1.5 * rand()) * k, w / 4, h / 2}); };
    const double leftR = left ? radius() : 0;
    const double rightR = right ? radius() : 0;
    const double step = std::max(1.0, 3 * k);
    const auto edge = [&](const Wave &wv, double d) { return amp * (0.5 + 0.5 * at(wv, d)); };
    const double x0 = x + leftR, x1 = x + w - rightR;
    const int hCount = std::max(2, int(std::ceil((x1 - x0) / step)) + 1);
    const int vCount = std::max(3, int(std::ceil(h / step)) + 1);
    QPolygonF points;
    points.reserve(2 * (hCount + vCount));
    for (int i = 0; i < hCount; ++i) {
        const double px = x0 + (x1 - x0) * i / (hCount - 1);
        points << QPointF(px, y + (top ? edge(topWave, px) : 0));
    }
    for (int i = 1; i < vCount - 1; ++i) {
        const double t = double(i) / (vCount - 1);
        const double bulge = rightR * (1 - std::sin(M_PI * t)) + (right ? 0.35 * edge(rightWave, y + h * t) : 0);
        points << QPointF(x + w - bulge, y + h * t);
    }
    for (int i = hCount - 1; i >= 0; --i) {
        const double px = x0 + (x1 - x0) * i / (hCount - 1);
        points << QPointF(px, y + h - (bottom ? edge(bottomWave, px) : 0));
    }
    for (int i = vCount - 2; i > 0; --i) {
        const double t = double(i) / (vCount - 1);
        const double bulge = leftR * (1 - std::sin(M_PI * t)) + (left ? 0.35 * edge(leftWave, y + h * t) : 0);
        points << QPointF(x + bulge, y + h * t);
    }
    return points;
}

QList<QPair<QPolygonF, qreal>> highlighterStroke(const QRectF &rect, quint32 s, qreal scale)
{
    const qreal k = scale > 0 ? scale : 1;
    if (rect.width() <= 0 || rect.height() <= 0) return {};
    Random rand{child(s, "stroke")};
    const double grow = std::min(1.2 * k, rect.height() * 0.12);
    QList<QPair<QPolygonF, qreal>> passes;
    // Feather: a faint, slightly larger pass with soft uneven edges.
    passes.append({strokePolygon(rect.adjusted(0, -grow, 0, grow), child(s, "feather"), k, 0.9 * k), 0.32});
    passes.append({strokePolygon(rect, child(s, "core"), k, 0.7 * k), 1.0});
    // A second, thinner pass over part of the stroke.
    if (rect.width() > 28 * k) {
        const double from = rand() * 0.3;
        const double to = 0.55 + rand() * 0.45;
        const double h = rect.height() * (0.55 + 0.2 * rand());
        const double y = rect.y() + (rect.height() - h) * (0.3 + 0.4 * rand());
        passes.append({strokePolygon(QRectF(rect.x() + rect.width() * from, y, rect.width() * (to - from), h),
                                     child(s, "again"), k, 0.5 * k), 0.45});
    }
    return passes;
}
}

AnnotationPaint::AnnotationPaint(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
}

namespace {
QRectF shapeRect(const QVariant &value)
{
    const auto shape = value.toMap();
    return QRectF(shape.value("x").toDouble(), shape.value("y").toDouble(),
                  shape.value("width").toDouble(), shape.value("height").toDouble());
}
// The area that shapes added or removed between two lists cover (each grown
// by what a stroke's feather and wobble may reach beyond its rect).
QRectF changedArea(const QVariantList &before, const QVariantList &after, qreal margin)
{
    QRectF area;
    for (const auto &value : before) if (!after.contains(value)) area |= shapeRect(value).adjusted(-margin, -margin, margin, margin);
    for (const auto &value : after) if (!before.contains(value)) area |= shapeRect(value).adjusted(-margin, -margin, margin, margin);
    return area;
}
}

void AnnotationPaint::setStrokes(const QVariantList &strokes)
{
    if (strokes == m_strokes) return;
    updateArea(changedArea(m_strokes, strokes, 4 * m_penScale));
    m_strokes = strokes;
    emit changed();
}
void AnnotationPaint::setBands(const QVariantList &bands)
{
    if (bands == m_bands) return;
    updateArea(changedArea(m_bands, bands, 4 * m_penScale));
    m_bands = bands;
    emit changed();
}
// A few shapes changed (a highlight toggled): only their area repaints. Many
// (a new layout) repaint everything.
void AnnotationPaint::updateArea(const QRectF &area)
{
    if (area.isEmpty()) return;
    // A partial update after a full one would narrow it: keep the full one.
    if (m_dirtyAll || area.height() > height() / 2) { updateAll(); return; }
    update(area.translated(m_inset, 0).toAlignedRect());
}
void AnnotationPaint::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) updateAll();
}
void AnnotationPaint::updateAll()
{
    m_dirtyAll = true;
    update();
}
void AnnotationPaint::setInk(const QColor &ink) { if (ink != m_ink) { m_ink = ink; changedLook(); } }
void AnnotationPaint::setInkAlpha(qreal alpha) { if (!qFuzzyCompare(alpha, m_inkAlpha)) { m_inkAlpha = alpha; changedLook(); } }
void AnnotationPaint::setTint(const QColor &tint) { if (tint != m_tint) { m_tint = tint; changedLook(); } }
void AnnotationPaint::setPenScale(qreal scale) { if (!qFuzzyCompare(scale, m_penScale)) { m_penScale = scale; changedLook(); } }
void AnnotationPaint::setInset(qreal inset) { if (!qFuzzyCompare(inset, m_inset)) { m_inset = inset; changedLook(); } }

void AnnotationPaint::paint(QPainter *painter)
{
    ++m_paintCount;
    m_dirtyAll = false;
    // Only shapes reaching the area being repainted are drawn.
    const QRectF clip = painter->hasClipping() ? painter->clipBoundingRect().translated(-m_inset, 0) : QRectF();
    const qreal margin = 4 * m_penScale;
    const auto shown = [&](const QRectF &rect) { return clip.isNull() || clip.intersects(rect.adjusted(-margin, -margin, margin, margin)); };
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(Qt::NoPen);
    painter->translate(m_inset, 0);
    // Tints first: each covering range adds a layer; highlights go on top.
    painter->setBrush(m_tint);
    for (const auto &value : std::as_const(m_bands)) {
        const QRectF rect = shapeRect(value);
        if (!shown(rect)) continue;
        const auto band = value.toMap();
        painter->drawPolygon(HandDrawn::bandPolygon(rect, band.value("seed").toUInt(), m_penScale, true, true,
                                                    band.value("left").toBool(), band.value("right").toBool()), Qt::WindingFill);
    }
    for (const auto &value : std::as_const(m_strokes)) {
        const QRectF rect = shapeRect(value);
        if (!shown(rect)) continue;
        for (const auto &[polygon, opacity] : HandDrawn::highlighterStroke(rect, value.toMap().value("seed").toUInt(), m_penScale)) {
            QColor ink = m_ink;
            ink.setAlphaF(float(m_inkAlpha * opacity));
            painter->setBrush(ink);
            painter->drawPolygon(polygon, Qt::WindingFill);
        }
    }
    emit painted();
}
