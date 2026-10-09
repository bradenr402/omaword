#pragma once

#include <QObject>
#include <QPointer>
#include <QQuickTextDocument>
#include <QTimer>
#include <QVariantList>

class QTextDocument;

// A Markdown TextEdit's preview layout (TextEdit.MarkdownText). Qt indents
// block quotes 40 px on both sides and draws nothing else, and puts no space
// between paragraphs. This narrows quotes to `indent` per level on the left
// only, puts `gap` (QuoteBars: a line of the text) above each paragraph, list, quote, heading, and code
// block (not between a paragraph's lines, a list's items, or a code block's
// lines), and reports where each quote level's bar goes ({x, y, height,
// level} in document coordinates) for QuoteBars.qml. Qt keeps no trace of
// where a quote starts or which blocks a line break split, so the text
// (Main.previewMarkdown) marks them with zero-width spaces (U+200B): one
// opening a quote's first block, one closing a block whose paragraph goes
// on in the next. They are read into each block's user state and removed.
class MarkdownQuotes : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QQuickTextDocument *document READ document WRITE setDocument NOTIFY documentChanged)
    Q_PROPERTY(qreal indent READ indent WRITE setIndent NOTIFY indentChanged)
    Q_PROPERTY(qreal gap READ gap WRITE setGap NOTIFY gapChanged)
    Q_PROPERTY(QVariantList bars READ bars NOTIFY barsChanged)
public:
    explicit MarkdownQuotes(QObject *parent = nullptr);
    QQuickTextDocument *document() const { return m_target; }
    void setDocument(QQuickTextDocument *target);
    qreal indent() const { return m_indent; }
    void setIndent(qreal indent);
    qreal gap() const { return m_gap; }
    void setGap(qreal gap);
    QVariantList bars() const { return m_bars; }
signals:
    void documentChanged();
    void indentChanged();
    void gapChanged();
    void barsChanged();
private:
    void narrow();
    void measure();
    QPointer<QQuickTextDocument> m_target;
    QPointer<QTextDocument> m_document;
    qreal m_indent = 14;
    qreal m_gap = 9;
    bool m_narrowing = false;
    QVariantList m_bars;
    // Coalesces layout changes into one measurement per event loop pass.
    QTimer m_measure;
    QTimer m_narrow;
};
