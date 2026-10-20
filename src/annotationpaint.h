#pragma once

#include <QColor>
#include <QQuickPaintedItem>
#include <QVariantList>

// The Paragraph and Block layouts' annotation marks: highlighter strokes and
// translucent tint bands, the shapes HandDrawn.js would draw (the same
// seeds and arithmetic, so the same pixels), painted in C++. The QML lays
// out where they go; this repaints only when they change, never for a
// selection move, and then only the area of the shapes that changed.
//   strokes: [{x, y, width, height, seed}] highlighter strokes in `ink`
//   bands:   [{x, y, width, height, seed, left, right}] tint bands in `tint`
// Coordinates are those of the area the marks lie in (the text's, shifted
// right by `inset`); the item covers only `area`, the part of it holding
// marks, where QML places it: a long chapter's whole height of texture would
// take long to make and to show, for marks a few lines tall.
class AnnotationPaint : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(QVariantList strokes READ strokes WRITE setStrokes NOTIFY changed)
    Q_PROPERTY(QVariantList bands READ bands WRITE setBands NOTIFY changed)
    Q_PROPERTY(QColor ink READ ink WRITE setInk NOTIFY changed)
    Q_PROPERTY(qreal inkAlpha READ inkAlpha WRITE setInkAlpha NOTIFY changed)
    Q_PROPERTY(QColor tint READ tint WRITE setTint NOTIFY changed)
    Q_PROPERTY(qreal penScale READ penScale WRITE setPenScale NOTIFY changed)
    Q_PROPERTY(qreal inset READ inset WRITE setInset NOTIFY changed)
    Q_PROPERTY(int paintCount READ paintCount NOTIFY painted)
    Q_PROPERTY(QRectF area READ area NOTIFY areaChanged)
public:
    explicit AnnotationPaint(QQuickItem *parent = nullptr);
    void paint(QPainter *painter) override;
protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
public:
    QVariantList strokes() const { return m_strokes; }
    void setStrokes(const QVariantList &strokes);
    QVariantList bands() const { return m_bands; }
    void setBands(const QVariantList &bands);
    QColor ink() const { return m_ink; }
    void setInk(const QColor &ink);
    qreal inkAlpha() const { return m_inkAlpha; }
    void setInkAlpha(qreal alpha);
    QColor tint() const { return m_tint; }
    void setTint(const QColor &tint);
    qreal penScale() const { return m_penScale; }
    void setPenScale(qreal scale);
    qreal inset() const { return m_inset; }
    void setInset(qreal inset);
    // Test hook: how many times it has painted.
    int paintCount() const { return m_paintCount; }
    QRectF area() const { return m_area; }
signals:
    void changed();
    void painted();
    void areaChanged();
private:
    void changedLook() { updateAll(); emit changed(); }
    void updateArea(const QRectF &area);
    void updateAll();
    void measureArea();
    bool m_dirtyAll = false;
    QVariantList m_strokes, m_bands;
    QColor m_ink, m_tint;
    qreal m_inkAlpha = 0.35, m_penScale = 1, m_inset = 0;
    int m_paintCount = 0;
    QRectF m_area;
};

// HandDrawn.js's deterministic shapes, for C++ painters.
namespace HandDrawn {
quint32 seed(const QStringList &parts);
QPolygonF strokePolygon(const QRectF &rect, quint32 seed, qreal scale, qreal amplitude);
QPolygonF bandPolygon(const QRectF &rect, quint32 seed, qreal scale, bool top, bool bottom, bool left, bool right);
// One highlighter stroke: the feathered, core, and second passes, with
// their opacities relative to the core's.
QList<QPair<QPolygonF, qreal>> highlighterStroke(const QRectF &rect, quint32 seed, qreal scale);
}
