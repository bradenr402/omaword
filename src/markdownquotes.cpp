#include "markdownquotes.h"
#include "brand.h"

#include <QAbstractTextDocumentLayout>
#include <QCoreApplication>
#include <QQmlEngine>
#include <QQuickTextDocument>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

static void registerMarkdownQuotes()
{
    static const auto module = (Brand::name() + QStringLiteral(".Reader")).toUtf8();
    qmlRegisterType<MarkdownQuotes>(module.constData(), 1, 0, "MarkdownQuotes");
}
Q_COREAPP_STARTUP_FUNCTION(registerMarkdownQuotes)

MarkdownQuotes::MarkdownQuotes(QObject *parent) : QObject(parent)
{
    m_measure.setSingleShot(true);
    m_measure.setInterval(0);
    connect(&m_measure, &QTimer::timeout, this, &MarkdownQuotes::measure);
}

void MarkdownQuotes::setDocument(QQuickTextDocument *target)
{
    if (target == m_target) return;
    if (m_document) {
        disconnect(m_document, nullptr, this, nullptr);
        disconnect(m_document->documentLayout(), nullptr, this, nullptr);
    }
    m_target = target;
    m_document = target ? target->textDocument() : nullptr;
    if (m_document) {
        // A new text (QQuickTextEdit parses its Markdown again) brings Qt's
        // margins back: narrow them before the next frame.
        connect(m_document, &QTextDocument::contentsChanged, this, &MarkdownQuotes::narrow);
        connect(m_document->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
                &m_measure, qOverload<>(&QTimer::start));
        connect(m_document, &QObject::destroyed, this, [this] { m_bars.clear(); emit barsChanged(); });
    }
    narrow();
    emit documentChanged();
}

void MarkdownQuotes::setIndent(qreal indent)
{
    if (qFuzzyCompare(indent, m_indent)) return;
    m_indent = indent;
    narrow();
    emit indentChanged();
}

void MarkdownQuotes::narrow()
{
    if (!m_document || m_narrowing) return;
    m_narrowing = true;
    QTextCursor cursor(m_document);
    bool editing = false;
    for (auto block = m_document->begin(); block.isValid(); block = block.next()) {
        const auto format = block.blockFormat();
        const int level = format.intProperty(QTextFormat::BlockQuoteLevel);
        if (level <= 0) continue;
        if (qFuzzyCompare(format.leftMargin(), level * m_indent) && qFuzzyIsNull(format.rightMargin())) continue;
        if (!editing) { cursor.beginEditBlock(); editing = true; }
        QTextBlockFormat narrowed;
        narrowed.setLeftMargin(level * m_indent);
        narrowed.setRightMargin(0);
        cursor.setPosition(block.position());
        cursor.mergeBlockFormat(narrowed);
    }
    if (editing) cursor.endEditBlock();
    m_narrowing = false;
    m_measure.start();
}

void MarkdownQuotes::measure()
{
    QVariantList bars;
    if (m_document) {
        auto *layout = m_document->documentLayout();
        const qreal margin = m_document->documentMargin();
        // One bar per level for each run of blocks quoted at least that deep,
        // from its first line's top to its last line's bottom.
        QList<QPair<qreal, qreal>> open;
        auto close = [&](int keep) {
            while (open.size() > keep) {
                const auto span = open.takeLast();
                bars.append(QVariantMap{{"x", margin + open.size() * m_indent}, {"y", span.first},
                                        {"height", span.second - span.first}, {"level", open.size() + 1}});
            }
        };
        for (auto block = m_document->begin(); block.isValid(); block = block.next()) {
            const int level = std::max(0, block.blockFormat().intProperty(QTextFormat::BlockQuoteLevel));
            // The preview marks each quote's first line with a zero-width
            // space: a quote right after another starts its own bars.
            close(block.text().startsWith(QChar(0x200b)) ? 0 : level);
            if (!level) continue;
            const QRectF rect = layout->blockBoundingRect(block);
            for (auto &span : open) span.second = rect.bottom();
            while (open.size() < level) open.append({rect.top(), rect.bottom()});
        }
        close(0);
    }
    if (bars != m_bars) {
        m_bars = bars;
        emit barsChanged();
    }
}
