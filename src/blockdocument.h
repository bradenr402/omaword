#pragma once

#include <QObject>
#include <QPointer>
#include <QQuickTextDocument>
#include <QVariantList>
#include <QClipboard>
#include "searchdocument.h"

// All offsets refer to the actual TextEdit QTextDocument, in UTF-16 units.
// Decoration cells occupy real layout positions; selectionText removes only
// those recorded positions, preserving the original Unicode content.
class BlockDocument : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList spans READ spans NOTIFY spansChanged)
public:
    // noteMarks false (margin notes shown) builds without note cells, so the
    // reflow happens once per toggle and no blank cells remain in the text.
    explicit BlockDocument(QObject *parent = nullptr);
    QVariantList spans() const { return m_spans; }
    // spacing (pixels, from the reader's type scale): headingAbove,
    // headingBelow, and poetryIndent (Paragraph layout's poetry lines).
    Q_INVOKABLE void build(QQuickTextDocument *target, const QVariantList &verses,
                          bool numbers, bool headings, double lineHeight,
                          const QString &mode = QStringLiteral("block"),
                          bool annotationsVisible = false, double decorationSize = 0,
                          bool noteMarks = true, const QVariantMap &spacing = {});
    // Plain clipboard text for actual TextEdit selection offsets (either order).
    // start/end include decorations; textStart/textEnd delimit original verse text.
    Q_INVOKABLE QString selectionText(int start, int end) const;
    // Track the native TextEdit selection for clipboard and PRIMARY filtering.
    Q_INVOKABLE void trackSelection(int start, int end);
    Q_INVOKABLE void copySelection(int start, int end, bool primary = false) const;
    // Paint bounds plus reserved cellWidth/cellHeight and canonical scope.
    // textTop/textBottom are document ink coordinates; bracketTop is relative
    // to y, bracketHeight excludes proportional line leading. paintWidth excludes
    // the reserved textGap; bracketX is its logical origin, mirrored when rtl.
    Q_INVOKABLE QVariantList inlineDecorations() const;
    Q_INVOKABLE int verseAt(int position) const;
    Q_INVOKABLE int verseAtPoint(double x, double y) const;
    Q_INVOKABLE int positionFor(int verse) const;
    Q_INVOKABLE QVariantList rectangles(int start, int end) const;
    Q_INVOKABLE QVariantList searchRectangles(const QVariantList &segments) const;
    Q_INVOKABLE QRectF searchRect(const QVariantMap &segment) const;
    // Character selection over source text: a verse's source [start, start +
    // length) → [documentStart, documentEnd] (empty when unmapped); and a
    // document position → {verse, offset} (forward: the first source unit at or
    // after it; else the exclusive end of the units before it), {} when none.
    Q_INVOKABLE QVariantList documentRange(int verse, int start, int length) const;
    Q_INVOKABLE QVariantMap sourceAt(int position, bool forward) const;
    // One entry per logical annotation intersecting this chapter. Geometry is
    // in document coordinates, including reserved inline decoration cells.
    // Membership comes from per-verse noteRanges/tagRanges in source order.
    // The remaining arguments retain the reader call signature, not ordering data.
    Q_INVOKABLE QVariantList annotations(const QVariantList &verses, const QVariantList &collection,
                                         int book, int chapter) const;
signals:
    void spansChanged();
    void geometryChanged();
private:
    void filterClipboard(QClipboard::Mode mode);
    QString m_cellName;
    int m_selectionStart = 0;
    int m_selectionEnd = 0;
    bool m_filteringClipboard = false;
    bool m_building = false;
    QPointer<QTextDocument> m_document;
    QVariantList m_spans;
    QVariantList m_decorations;
    QList<QPair<int, int>> m_decorationSegments;
    QHash<int, SourcePositionMap> m_sourceMaps;
};
