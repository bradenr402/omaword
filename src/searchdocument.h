#pragma once

#include <QObject>
#include <QPointer>
#include <QQuickTextDocument>
#include <QRectF>
#include <QVariantList>

// Source offsets are UTF-16, before HTML serialization. Whitespace runs may
// collapse or become Qt line/paragraph separators; all other codepoints must
// agree. A failed alignment deliberately paints nothing rather than wrong text.
class SourcePositionMap
{
public:
    void build(const QString &source, const QString &rendered, int documentStart = 0);
    QPair<int, int> range(int start, int length) const;
    // Inverse lookups for a document position: the first source unit at or
    // after it (-1 if none), and the exclusive end of the source units before
    // it (0 if none). Whitespace collapsed into one rendered run maps as a run.
    int sourceStart(int position) const;
    int sourceEnd(int position) const;
    bool isEmpty() const { return m_positions.isEmpty(); }
private:
    QList<QPair<int, int>> m_positions;
};

namespace SearchGeometry {
QVariantList rectangles(QTextDocument *document, int start, int end);
QRectF firstRect(const QVariantList &rectangles);
// Scripture is laid out left to right. A paragraph opening with a Hebrew
// letter (Psalm 119's acrostic) must not switch its base direction; RTL runs
// still render in place. Display-only: no characters are inserted.
void forceLeftToRight(QTextDocument *document);
}

class SearchDocument : public QObject
{
    Q_OBJECT
public:
    explicit SearchDocument(QObject *parent = nullptr);
    Q_INVOKABLE void attach(QQuickTextDocument *target, const QString &sourceText);
    Q_INVOKABLE QVariantList rectangles(const QVariantList &segments) const;
    Q_INVOKABLE QRectF rectFor(int start, int length) const;
    Q_INVOKABLE QRectF cursorRect(int position) const;
    // Character selection: source [start, start + length) → [documentStart,
    // documentEnd] (empty list when unmapped), and a document position back to
    // a source offset (forward: first unit at/after it; else exclusive end).
    Q_INVOKABLE QVariantList documentRange(int start, int length) const;
    Q_INVOKABLE int sourceAt(int position, bool forward) const;
    Q_INVOKABLE void leftToRight(QQuickTextDocument *target) const;
signals:
    void geometryChanged();
private:
    void rebuildMap();
    QPointer<QTextDocument> m_document;
    QString m_source;
    SourcePositionMap m_map;
};
