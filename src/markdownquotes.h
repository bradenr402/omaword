#pragma once

#include <QObject>
#include <QPointer>
#include <QQuickTextDocument>
#include <QTimer>
#include <QVariantList>

class QTextDocument;

// Block quotes in a Markdown TextEdit (TextEdit.MarkdownText): Qt indents
// them 40 px on both sides and draws nothing else. This narrows them to
// `indent` per level on the left only, and reports where each level's bar
// goes ({x, y, height, level} in document coordinates) for the QML to draw
// (QuoteBars.qml). Bars follow every change to the text and its layout.
// Qt keeps no trace of where one quote ends and the next begins: a block
// starting with a zero-width space (U+200B) starts new bars.
class MarkdownQuotes : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QQuickTextDocument *document READ document WRITE setDocument NOTIFY documentChanged)
    Q_PROPERTY(qreal indent READ indent WRITE setIndent NOTIFY indentChanged)
    Q_PROPERTY(QVariantList bars READ bars NOTIFY barsChanged)
public:
    explicit MarkdownQuotes(QObject *parent = nullptr);
    QQuickTextDocument *document() const { return m_target; }
    void setDocument(QQuickTextDocument *target);
    qreal indent() const { return m_indent; }
    void setIndent(qreal indent);
    QVariantList bars() const { return m_bars; }
signals:
    void documentChanged();
    void indentChanged();
    void barsChanged();
private:
    void narrow();
    void measure();
    QPointer<QQuickTextDocument> m_target;
    QPointer<QTextDocument> m_document;
    qreal m_indent = 14;
    bool m_narrowing = false;
    QVariantList m_bars;
    // Coalesces layout changes into one measurement per event loop pass.
    QTimer m_measure;
};
