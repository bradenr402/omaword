#include "markdownquotes.h"
#include "brand.h"

#include <QAbstractTextDocumentLayout>
#include <QCoreApplication>
#include <QQmlEngine>
#include <QQuickTextDocument>
#include <QFontMetricsF>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextList>

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
    // New text is laid out once Qt has finished importing it, never while
    // it is still inserting it.
    m_narrow.setSingleShot(true);
    m_narrow.setInterval(0);
    connect(&m_narrow, &QTimer::timeout, this, &MarkdownQuotes::narrow);
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
        connect(m_document, &QTextDocument::contentsChanged, this, [this] { if (!m_narrowing) m_narrow.start(); });
        connect(m_document->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
                &m_measure, qOverload<>(&QTimer::start));
        connect(m_document, &QObject::destroyed, this, [this] { m_bars.clear(); emit barsChanged(); });
    }
    narrow();
    emit documentChanged();
}

void MarkdownQuotes::setGap(qreal gap)
{
    if (qFuzzyCompare(gap, m_gap)) return;
    m_gap = gap;
    narrow();
    emit gapChanged();
}

void MarkdownQuotes::setIndent(qreal indent)
{
    if (qFuzzyCompare(indent, m_indent)) return;
    m_indent = indent;
    narrow();
    emit indentChanged();
}

void MarkdownQuotes::setListIndent(qreal indent)
{
    if (qFuzzyCompare(indent, m_listIndent)) return;
    m_listIndent = indent;
    narrow();
    emit listIndentChanged();
}

namespace {
enum Flag { StartsQuote = 1, Continues = 2 };
enum Kind { Paragraph, List, Code, Heading };
Kind kind(const QTextBlock &block)
{
    const auto format = block.blockFormat();
    if (format.hasProperty(QTextFormat::BlockCodeFence) || format.nonBreakableLines()) return Code;
    if (block.textList()) return List;
    if (format.headingLevel() > 0) return Heading;
    return Paragraph;
}
}

qreal MarkdownQuotes::markerRoom(const QTextList *list)
{
    // As QTextDocumentLayout draws them: a bullet a third of a line wide, a
    // number as wide as its text ("10."), a task's box about a line high.
    const int style = list->format().style();
    const bool bullet = style == QTextListFormat::ListDisc || style == QTextListFormat::ListCircle || style == QTextListFormat::ListSquare;
    qreal room = 0;
    for (int i = 0; i < list->count(); ++i) {
        const QTextBlock item = list->item(i);
        const QFontMetricsF metrics(item.charFormat().font());
        const qreal width = item.blockFormat().marker() != QTextBlockFormat::MarkerType::NoMarker ? metrics.height()
            : bullet ? metrics.lineSpacing() / 3 : metrics.horizontalAdvance(list->itemText(item));
        room = std::max(room, std::ceil(metrics.horizontalAdvance(QLatin1Char(' ')) + width));
    }
    return room;
}

void MarkdownQuotes::narrow()
{
    if (!m_document || m_narrowing) return;
    m_narrowing = true;
    // A list level's indent (its bullet or number sits a space before its text).
    if (!qFuzzyCompare(m_document->indentWidth(), m_listIndent)) m_document->setIndentWidth(m_listIndent);
    QTextCursor cursor(m_document);
    cursor.beginEditBlock();
    const QChar marker(0x200b);
    QTextBlock previous;
    // How much further in each list stands than its levels' indent, its
    // markers' room beyond them added to its parent's (nested: the latest
    // list at each depth).
    QHash<const QTextList *, qreal> shifts;
    QHash<int, qreal> depths;
    for (auto block = m_document->begin(); block.isValid(); block = block.next()) {
        const auto format = block.blockFormat();
        const int level = std::max(0, format.intProperty(QTextFormat::BlockQuoteLevel));
        const QTextList *list = block.textList();
        const bool resumed = list && shifts.contains(list);
        qreal shift = 0;
        if (list) {
            const int depth = list->format().indent();
            if (!resumed) shifts.insert(list, depths.value(depth - 1) + std::max<qreal>(0, markerRoom(list) - m_listIndent));
            shift = depths[depth] = shifts.value(list);
        }
        // Fresh text (previewMarkdown's): read its markers into the block, then drop them.
        if (block.userState() < 0) {
            const QString text = block.text();
            const bool starts = level > 0 && text.startsWith(marker), continues = text.endsWith(marker) && text.size() > (starts ? 1 : 0);
            block.setUserState((starts ? StartsQuote : 0) | (continues ? Continues : 0));
            if (continues) {
                cursor.setPosition(block.position() + text.size() - 1);
                cursor.deleteChar();
            }
            if (starts) {
                cursor.setPosition(block.position());
                cursor.deleteChar();
            }
        }
        // A paragraph's next line (a single Enter) follows without a gap, as
        // do a list's items and a code block's lines; anything else starts
        // after one.
        qreal top = m_gap;
        if (!previous.isValid()) top = 0;
        else {
            const Kind before = kind(previous), now = kind(block);
            const int previousLevel = std::max(0, previous.blockFormat().intProperty(QTextFormat::BlockQuoteLevel));
            if (before == Paragraph && now == Paragraph && (previous.userState() & Continues) && previousLevel == level
                && !(block.userState() & StartsQuote)) top = 0;
            else if (before == Code && now == Code) top = 0;
            // A list's items run on, into a list nested in one and back out
            // to it; a new list beside them starts after a gap.
            else if (before == List && now == List && (previous.textList() == list || resumed
                     || list->format().indent() > previous.textList()->format().indent())) top = 0;
        }
        const qreal left = (level > 0 ? level * m_indent : list ? 0 : format.leftMargin()) + shift;
        if (!qFuzzyCompare(format.leftMargin() + 1, left + 1) || !qFuzzyIsNull(format.rightMargin())
            || !qFuzzyCompare(format.topMargin() + 1, top + 1) || !qFuzzyIsNull(format.bottomMargin())) {
            QTextBlockFormat spaced;
            if (level > 0) spaced.setRightMargin(0);
            if (level > 0 || list) spaced.setLeftMargin(left);
            spaced.setTopMargin(top);
            spaced.setBottomMargin(0);
            cursor.setPosition(block.position());
            cursor.mergeBlockFormat(spaced);
        }
        previous = block;
    }
    cursor.endEditBlock();
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
            // A quote right after another starts its own bars.
            close(block.userState() > 0 && (block.userState() & StartsQuote) ? 0 : level);
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
