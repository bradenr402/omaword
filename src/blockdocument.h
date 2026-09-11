#pragma once

#include <QObject>
#include <QPointer>
#include <QQuickTextDocument>
#include <QVariantList>

// All offsets refer to the actual TextEdit QTextDocument, in UTF-16 units.
// No HTML-length estimates or invisible marker characters enter copied text.
class BlockDocument : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList spans READ spans NOTIFY spansChanged)
public:
    using QObject::QObject;
    QVariantList spans() const { return m_spans; }
    Q_INVOKABLE void build(QQuickTextDocument *target, const QVariantList &verses,
                          bool numbers, bool headings, double lineHeight);
    Q_INVOKABLE int verseAt(int position) const;
    Q_INVOKABLE int verseAtPoint(double x, double y) const;
    Q_INVOKABLE int positionFor(int verse) const;
    Q_INVOKABLE QVariantList rectangles(int start, int end) const;
signals:
    void spansChanged();
private:
    QPointer<QTextDocument> m_document;
    QVariantList m_spans;
};
