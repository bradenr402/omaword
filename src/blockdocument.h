#pragma once

#include <QObject>
#include <QPointer>
#include <QQuickTextDocument>
#include <QVariantList>
#include <QClipboard>

// All offsets refer to the actual TextEdit QTextDocument, in UTF-16 units.
// Decoration cells occupy real layout positions; selectionText removes only
// those recorded positions, preserving the original Unicode content.
class BlockDocument : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList spans READ spans NOTIFY spansChanged)
public:
    explicit BlockDocument(QObject *parent = nullptr);
    QVariantList spans() const { return m_spans; }
    Q_INVOKABLE void build(QQuickTextDocument *target, const QVariantList &verses,
                          bool numbers, bool headings, double lineHeight,
                          const QString &mode = QStringLiteral("block"),
                          bool annotationsVisible = false, double decorationSize = 0);
    // Plain clipboard text for actual TextEdit selection offsets (either order).
    // start/end include decorations; textStart/textEnd delimit original verse text.
    Q_INVOKABLE QString selectionText(int start, int end) const;
    // Track the native TextEdit selection for clipboard and PRIMARY filtering.
    Q_INVOKABLE void trackSelection(int start, int end);
    Q_INVOKABLE void copySelection(int start, int end, bool primary = false) const;
    // Reserved cell geometry plus kind/reference/scope/edge/verse/position.
    Q_INVOKABLE QVariantList inlineDecorations() const;
    // Union of cursor-containing bookmark/note/tag ranges, excluding selected verses.
    Q_INVOKABLE QVariantList contextRanges(const QVariantList &verses, int cursorVerse,
                                           int selectedStart = 0, int selectedEnd = 0) const;
    Q_INVOKABLE int verseAt(int position) const;
    Q_INVOKABLE int verseAtPoint(double x, double y) const;
    Q_INVOKABLE int positionFor(int verse) const;
    Q_INVOKABLE QVariantList rectangles(int start, int end) const;
    // One entry per logical annotation intersecting this chapter. Geometry is
    // in document coordinates, including reserved inline decoration cells.
    // Membership comes from per-verse bookmarkRanges/noteRanges/tagRanges in source order.
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
};
