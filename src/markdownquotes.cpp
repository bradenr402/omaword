#include "markdownquotes.h"
#include "brand.h"

#include <QAbstractTextDocumentLayout>
#include <QCoreApplication>
#include <QQmlEngine>
#include <QQuickTextDocument>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
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

void MarkdownQuotes::setLinkColor(const QColor &color)
{
    if (color == m_linkColor) return;
    m_linkColor = color;
    narrow();
    emit linkColorChanged();
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
// previewMarkdown's mark for an image it made a link (U+2063, invisible).
const QChar imageMark(0x2063);
// An image glyph for a link's text em pixels high: a framed picture (a sun
// over hills) in its ink, and the room before the text, drawn for the
// screen's pixel ratio.
QImage imageGlyph(const QColor &ink, int em, QSizeF *size)
{
    *size = QSizeF(std::ceil(em * 1.1), std::ceil(em * 0.72));
    const qreal ratio = qGuiApp ? std::max<qreal>(1, qGuiApp->devicePixelRatio()) : 1;
    QImage image((*size * ratio).toSize(), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(ratio);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal pen = std::max<qreal>(1, em / 14.0);
    const QRectF frame(pen / 2, pen / 2, em * 0.82 - pen, size->height() - pen);
    painter.setPen(QPen(ink, pen));
    painter.drawRoundedRect(frame, em * 0.09, em * 0.09);
    painter.setPen(Qt::NoPen);
    painter.setBrush(ink);
    painter.drawEllipse(QPointF(frame.left() + frame.width() * 0.3, frame.top() + frame.height() * 0.33), em * 0.07, em * 0.07);
    QPainterPath hills;
    hills.moveTo(frame.left() + pen / 2, frame.bottom());
    hills.lineTo(frame.left() + frame.width() * 0.42, frame.top() + frame.height() * 0.46);
    hills.lineTo(frame.left() + frame.width() * 0.62, frame.top() + frame.height() * 0.7);
    hills.lineTo(frame.left() + frame.width() * 0.76, frame.top() + frame.height() * 0.56);
    hills.lineTo(frame.right() - pen / 2, frame.bottom());
    hills.closeSubpath();
    painter.drawPath(hills);
    return image;
}
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
    colorLinks(cursor);
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
            // Qt's import gives a heading's size and weight only to its text
            // outside links, emphasis, and code; from a span on (and in a
            // heading that begins with one, at the page's top, throughout) the
            // text is set as a paragraph's. Every part of a heading takes them,
            // as Qt sets them (H1 to H6: size +3 to -2, bold); a link keeps
            // its color, emphasis its slant, code its face.
            if (const int heading = format.headingLevel(); heading > 0) {
                QTextCharFormat look;
                look.setProperty(QTextFormat::FontSizeAdjustment, 4 - heading);
                look.setFontWeight(QFont::Bold);
                cursor.setPosition(block.position());
                cursor.setPosition(block.position() + block.length() - 1, QTextCursor::KeepAnchor);
                cursor.mergeCharFormat(look);
                cursor.mergeBlockCharFormat(look);
            }
            // An image's mark becomes its glyph, in the size and color of the
            // link the image became, and a part of it: a click follows it.
            for (int at = block.text().indexOf(imageMark); at >= 0; at = block.text().indexOf(imageMark, at + 1)) {
                const int position = block.position() + at;
                QTextCursor probe(m_document);
                probe.setPosition(at + 1 < block.text().size() ? position + 2 : position + 1);
                const QTextCharFormat link = probe.charFormat();
                // Its size: the text's, or a heading's (Qt applies the size
                // adjustment only when it lays the text out: as it scales the
                // text, H6 to H1 at 0.7 to 2 times).
                const QFont text = m_document->defaultFont();
                static const qreal scales[] = {0.7, 0.8, 1.0, 1.2, 1.5, 2.0};
                const qreal scale = link.hasProperty(QTextFormat::FontSizeAdjustment)
                    ? scales[qBound(0, link.intProperty(QTextFormat::FontSizeAdjustment) + 2, 5)] : 1.0;
                const int em = qRound(scale * (text.pixelSize() > 0 ? text.pixelSize() : QFontInfo(text).pixelSize()));
                const QColor ink = link.foreground().style() != Qt::NoBrush ? link.foreground().color() : QColor(Qt::black);
                const QUrl name(QStringLiteral("omaword-glyph:image/%1/%2").arg(ink.name(QColor::HexArgb).mid(1)).arg(em));
                QSizeF size;
                const QImage image = imageGlyph(ink, em, &size);
                m_document->addResource(QTextDocument::ImageResource, name, image);
                QTextImageFormat glyph;
                glyph.merge(link);
                glyph.setName(name.toString());
                glyph.setWidth(size.width());
                glyph.setHeight(size.height());
                cursor.setPosition(position);
                cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
                cursor.insertImage(glyph);
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

void MarkdownQuotes::colorLinks(QTextCursor &cursor)
{
    if (!m_linkColor.isValid()) return;
    // Gathered first: a format merged splits and joins the fragments.
    QList<QTextFragment> links;
    for (auto block = m_document->begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it)
            if (it.fragment().isValid() && it.fragment().charFormat().isAnchor()
                && it.fragment().charFormat().foreground().color() != m_linkColor) links.append(it.fragment());
    for (const auto &fragment : links) {
        QTextCharFormat ink;
        ink.setForeground(m_linkColor);
        // An image's glyph (narrow) is drawn again in it, as large.
        const auto image = fragment.charFormat().toImageFormat();
        const QString glyph = QStringLiteral("omaword-glyph:image/");
        if (image.isValid() && image.name().startsWith(glyph)) {
            const int em = image.name().section('/', -1).toInt();
            const QUrl name(glyph + QStringLiteral("%1/%2").arg(m_linkColor.name(QColor::HexArgb).mid(1)).arg(em));
            QSizeF size;
            m_document->addResource(QTextDocument::ImageResource, name, imageGlyph(m_linkColor, em, &size));
            QTextImageFormat redrawn;
            redrawn.setName(name.toString());
            redrawn.setForeground(m_linkColor);
            ink = redrawn;
        }
        cursor.setPosition(fragment.position());
        cursor.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(ink);
    }
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
