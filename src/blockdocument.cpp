#include "blockdocument.h"
#include "brand.h"

#include <QAbstractTextDocumentLayout>
#include <QCoreApplication>
#include <QQmlEngine>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextFragment>
#include <QTextLayout>
#include <QTextImageFormat>
#include <QImage>
#include <QFontMetricsF>
#include <QSet>
#include <QGuiApplication>
#include <QMimeData>
#include <QScopedValueRollback>
#include <QTextDocumentFragment>
#include <QUuid>
#include <algorithm>

static void registerBlockDocument()
{
    static const auto module = (Brand::name() + QStringLiteral(".Reader")).toUtf8();
    qmlRegisterType<BlockDocument>(module.constData(), 1, 0, "BlockDocument");
}
Q_COREAPP_STARTUP_FUNCTION(registerBlockDocument)

BlockDocument::BlockDocument(QObject *parent)
    : QObject(parent), m_cellName("omaword:inline-cell/" + QUuid::createUuid().toString(QUuid::WithoutBraces))
{
    connect(QGuiApplication::clipboard(), &QClipboard::changed, this, &BlockDocument::filterClipboard);
}

void BlockDocument::trackSelection(int start, int end)
{
    if (m_building) return;
    if (m_selectionStart == qMin(start, end) && m_selectionEnd == qMax(start, end)) return;
    m_selectionStart = qMin(start, end);
    m_selectionEnd = qMax(start, end);
    // Keyboard selections also publish PRIMARY on platforms supporting it.
    // Native publication can occur before or after TextEdit's selection signals;
    // the changed handler handles a subsequent native write as well.
    if (start != end && QGuiApplication::clipboard()->supportsSelection()) copySelection(start, end, true);
}

void BlockDocument::copySelection(int start, int end, bool primary) const
{
    if (!m_document || start == end) return;
    auto clipboard = QGuiApplication::clipboard();
    if (primary && !clipboard->supportsSelection()) return;
    clipboard->setText(selectionText(start, end), primary ? QClipboard::Selection : QClipboard::Clipboard);
}

void BlockDocument::filterClipboard(QClipboard::Mode mode)
{
    if (m_filteringClipboard || m_building || !m_document || m_selectionStart == m_selectionEnd
        || m_selectionEnd >= m_document->characterCount() || mode == QClipboard::FindBuffer) return;
    bool decorated = false;
    for (const auto &segment : m_decorationSegments)
        if (segment.first < m_selectionEnd && segment.second > m_selectionStart) { decorated = true; break; }
    if (!decorated) return;
    auto clipboard = QGuiApplication::clipboard();
    const auto mime = clipboard->mimeData(mode);
    if (!mime || !mime->hasText()) return;
    QTextCursor cursor(m_document);
    cursor.setPosition(m_selectionStart);
    cursor.setPosition(m_selectionEnd, QTextCursor::KeepAnchor);
    // Restrict interception to this reader's exact native serialization. The
    // resource is unique per reader; plain-only exports must match its raw text.
    if (mime->text() != cursor.selection().toPlainText()) return;
    if (mime->hasHtml() && !mime->html().contains(m_cellName)
        && mime->html() != cursor.selection().toHtml()) return;
    const QString filtered = selectionText(m_selectionStart, m_selectionEnd);
    QScopedValueRollback<bool> guard(m_filteringClipboard, true);
    clipboard->setText(filtered, mode); // Also removes the image-bearing HTML MIME.
}

void BlockDocument::build(QQuickTextDocument *target, const QVariantList &verses,
                          bool numbers, bool headings, double lineHeight, const QString &mode,
                          bool annotationsVisible, double decorationSize)
{
    if (!target || !target->textDocument()) return;
    QScopedValueRollback<bool> building(m_building, true);
    if (m_document != target->textDocument()) {
        if (m_document) {
            disconnect(m_document, nullptr, this, nullptr);
            disconnect(m_document->documentLayout(), nullptr, this, nullptr);
        }
        m_document = target->textDocument();
        connect(m_document, &QTextDocument::contentsChanged, this, &BlockDocument::geometryChanged);
        connect(m_document->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
                this, &BlockDocument::geometryChanged);
        // TextEdit also requests geometry on font/width changes, including
        // reflows that keep the same size. Do not use layout::update here: it
        // fires for caret/selection repaints as well as actual layout changes.
    }
    m_selectionStart = m_selectionEnd = 0;
    m_document->clear();
    m_document->setDocumentMargin(0);
    m_spans.clear();
    m_decorations.clear();
    m_decorationSegments.clear();
    // Qt Quick lays out image objects with their explicit dimensions. Painting
    // is done by the reader over these transparent cells, never over verse text.
    const QUrl cellUrl(m_cellName);
    QImage cell(1, 1, QImage::Format_ARGB32_Premultiplied);
    cell.fill(Qt::transparent);
    m_document->addResource(QTextDocument::ImageResource, cellUrl, cell);
    const QFontMetricsF bodyMetrics(m_document->defaultFont());
    const double em = decorationSize > 0 ? decorationSize : bodyMetrics.height();
    QTextCursor cursor(m_document);
    QTextBlockFormat paragraph;
    paragraph.setLineHeight(lineHeight * 100, QTextBlockFormat::ProportionalHeight);
    cursor.setBlockFormat(paragraph);
    const QTextCharFormat normal;
    // Invisible word joiners must not select a taller fallback font, which
    // would enlarge every annotated line despite the image fitting its ascent.
    QTextCharFormat joiner;
    QFont joinerFont = m_document->defaultFont();
    joinerFont.setStyleStrategy(QFont::NoFontMerging);
    joiner.setFont(joinerFont);
    // Derive nesting from source-order membership, not reference spelling or
    // numeric book IDs (which need not follow the edition's book order).
    QMap<QString, QPair<int, int>> extents;
    for (int i = 0; annotationsVisible && i < verses.size(); ++i) {
        const auto verse = verses[i].toMap();
        for (const QString &kind : {QStringLiteral("bookmark"), QStringLiteral("note"), QStringLiteral("tag")}) {
            for (const auto &entry : verse.value(kind + "Ranges").toList()) {
                const auto range = entry.toMap();
                const QString key = kind + ':' + range.value("reference").toString();
                if (!extents.contains(key)) extents[key] = {i, i};
                extents[key].second = i;
            }
        }
    }
    for (int verseIndex = 0; verseIndex < verses.size(); ++verseIndex) {
        const auto &value = verses[verseIndex];
        const auto verse = value.toMap();
        const QString heading = verse.value("heading").toString();
        if (headings && !heading.isEmpty()) {
            if (cursor.position()) cursor.insertBlock(paragraph, normal);
            QTextBlockFormat section = paragraph;
            section.setTopMargin(18);
            section.setBottomMargin(12);
            cursor.setBlockFormat(section);
            QTextCharFormat title;
            title.setFontWeight(QFont::DemiBold);
            cursor.insertText(heading, title);
            cursor.insertBlock(paragraph, normal);
        } else if (cursor.position()) {
            if (mode == QStringLiteral("paragraph") && verse.value("paragraphStart").toBool())
                cursor.insertBlock(paragraph, normal);
            else cursor.insertText(" ", normal);
        }
        const int start = cursor.position();
        if (numbers) {
            QTextCharFormat number;
            number.setVerticalAlignment(QTextCharFormat::AlignSuperScript);
            cursor.insertText(verse.value("number").toString(), number);
            cursor.insertText(QString(QChar(0x00a0)), normal);
        }
        auto decorate = [&](bool opening) {
            if (!annotationsVisible) return;
            QVariantList entries;
            for (const QString &kind : {QStringLiteral("bookmark"), QStringLiteral("note"), QStringLiteral("tag")}) {
                QSet<QString> seen;
                for (const auto &entry : verse.value(kind + "Ranges").toList()) {
                    auto range = entry.toMap();
                    const auto reference = range.value("reference").toString();
                    if (seen.contains(reference)) continue;
                    seen.insert(reference);
                    const bool before = range.value(kind == "bookmark" ? "bookmarkContinuesBefore" : "continuesBefore").toBool();
                    const bool after = range.value(kind == "bookmark" ? "bookmarkContinuesAfter" : "continuesAfter").toBool();
                    if (opening ? (before && verseIndex != 0) : after) continue;
                    range["kind"] = kind;
                    range["scope"] = range.value("scope", entry);
                    range["continuesBefore"] = before;
                    range["edge"] = opening ? (before ? "continuation" : "open") : "close";
                    entries.append(range);
                }
            }
            std::sort(entries.begin(), entries.end(), [&](const QVariant &a, const QVariant &b) {
                const auto x = a.toMap(), y = b.toMap();
                const QString xKey = x.value("kind").toString() + ':' + x.value("reference").toString();
                const QString yKey = y.value("kind").toString() + ':' + y.value("reference").toString();
                const auto xExtent = extents.value(xKey), yExtent = extents.value(yKey);
                const int xEdge = opening ? xExtent.second : xExtent.first;
                const int yEdge = opening ? yExtent.second : yExtent.first;
                if (xEdge != yEdge) return xEdge > yEdge; // Outer opens first; inner closes first.
                return opening ? xKey < yKey : xKey > yKey; // Equal ranges close in reverse order.
            });
            for (auto entry : entries) {
                auto marker = entry.toMap();
                const int segmentStart = cursor.position();
                if (!opening) cursor.insertText(QString(QChar(0x2060)), joiner);
                marker["position"] = cursor.position();
                marker["verse"] = verse.value("number");
                marker["paintWidth"] = qMax(1, qRound(em * (opening ? 0.92 : 0.32)));
                // Reserve breathing room inside the image, not as text or as
                // extra height. Joiners and clipboard offsets stay identical.
                marker["textGap"] = qMax(1, qRound(em * 0.15));
                marker["cellWidth"] = marker.value("paintWidth").toInt() + marker.value("textGap").toInt();
                // Qt's baseline image box includes the font descent. Reserve
                // the font height plus two pixels, not an extra em of ascent.
                marker["cellHeight"] = bodyMetrics.height() + 2;
                QTextImageFormat format;
                format.setFont(m_document->defaultFont());
                format.setName(cellUrl.toString());
                format.setWidth(marker.value("cellWidth").toDouble());
                format.setHeight(marker.value("cellHeight").toDouble());
                format.setVerticalAlignment(QTextCharFormat::AlignBaseline);
                cursor.insertImage(format);
                if (opening) cursor.insertText(QString(QChar(0x2060)), joiner);
                m_decorationSegments.append({segmentStart, cursor.position()});
                m_decorations.append(marker);
            }
        };
        decorate(true);
        const int textStart = cursor.position();
        QTextDocument source;
        const auto html = verse.value("html").toString();
        if (html.isEmpty()) source.setPlainText(verse.value("text").toString());
        else source.setHtml(html); // Already sanitized by the USFM importer.
        // Flatten source paragraph wrappers, preserving internal poetry breaks
        // and inline formats. Source verse-boundary paragraphs are handled above.
        for (auto block = source.begin(); block.isValid(); block = block.next()) {
            if (block != source.begin()) cursor.insertText(QString(QChar::LineSeparator), normal);
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const auto fragment = it.fragment();
                if (fragment.isValid()) cursor.insertText(fragment.text(), fragment.charFormat());
            }
        }
        const int textEnd = cursor.position();
        decorate(false);
        m_spans.append(QVariantMap{{"number",verse.value("number")}, {"start",start},
                                  {"textStart",textStart}, {"textEnd",textEnd}, {"end",cursor.position()}});
    }
    emit spansChanged();
    emit geometryChanged();
}

QString BlockDocument::selectionText(int start, int end) const
{
    if (!m_document) return {};
    if (start > end) std::swap(start, end);
    start = qBound(0, start, m_document->characterCount() - 1);
    end = qBound(start, end, m_document->characterCount() - 1);
    QString result;
    auto append = [&](int a, int b) {
        if (a >= b) return;
        QTextCursor cursor(m_document);
        cursor.setPosition(a);
        cursor.setPosition(b, QTextCursor::KeepAnchor);
        result += cursor.selectedText();
    };
    for (const auto &segment : m_decorationSegments) {
        if (segment.second <= start) continue;
        if (segment.first >= end) break;
        append(start, qMin(end, segment.first));
        start = qMax(start, segment.second);
    }
    append(start, end);
    result.replace(QChar::ParagraphSeparator, '\n');
    result.replace(QChar::LineSeparator, '\n');
    return result;
}

QVariantList BlockDocument::inlineDecorations() const
{
    QVariantList result;
    if (!m_document) return result;
    for (const auto &entry : m_decorations) {
        auto marker = entry.toMap();
        const int position = marker.value("position").toInt();
        const auto block = m_document->findBlock(position);
        const auto line = block.layout()->lineForTextPosition(position - block.position());
        if (!line.isValid()) continue;
        const auto origin = m_document->documentLayout()->blockBoundingRect(block).topLeft();
        const double a = line.cursorToX(position - block.position(), QTextLine::Leading);
        const double b = line.cursorToX(position - block.position() + 1, QTextLine::Leading);
        const double baseline = origin.y() + line.y() + line.ascent();
        // Use the original endpoint's font, not the image or superscript verse
        // number. Proportional line leading must never stretch these brackets.
        int neighbor = position;
        for (const auto &value : m_spans) {
            const auto span = value.toMap();
            if (span.value("number") != marker.value("verse")) continue;
            neighbor = marker.value("edge") == "close"
                ? qMax(span.value("textStart").toInt(), span.value("textEnd").toInt() - 1)
                : span.value("textStart").toInt();
            break;
        }
        QTextCursor cursor(m_document);
        cursor.setPosition(neighbor);
        cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        const QFontMetricsF metrics(cursor.charFormat().font().resolve(m_document->defaultFont()));
        QString letters = block.text().mid(line.textStart(), line.textLength());
        letters.remove(QChar::ObjectReplacementCharacter);
        letters.remove(QChar(0x2060));
        const QRectF ink = metrics.tightBoundingRect(letters);
        const double textTop = baseline - qMax(metrics.capHeight(), -ink.top());
        const double textBottom = baseline + qMax(0.0, ink.bottom());
        const double bracketY = textTop - 1;
        const double bracketBottom = qMax(baseline + metrics.descent(), textBottom + 1);
        const double cellY = baseline - metrics.ascent();
        const double paintY = qMin(cellY, bracketY);
        marker["x"] = origin.x() + qMin(a, b);
        marker["y"] = paintY;
        marker["width"] = qAbs(b - a);
        marker["rtl"] = a > b;
        // Logical paint origin; the QML canvas mirrors it for an RTL cell.
        marker["bracketX"] = marker.value("edge") == "close" ? marker.value("textGap").toDouble() : 0.0;
        marker["height"] = bracketBottom - paintY;
        marker["textTop"] = textTop;
        marker["textBottom"] = textBottom;
        marker["bracketTop"] = bracketY - paintY;
        marker["bracketHeight"] = bracketBottom - bracketY;
        result.append(marker);
    }
    return result;
}

QVariantList BlockDocument::contextRanges(const QVariantList &verses, int cursorVerse,
                                         int selectedStart, int selectedEnd) const
{
    auto keys = [](const QVariantMap &verse) {
        QSet<QString> result;
        for (const QString &kind : {QStringLiteral("bookmark"), QStringLiteral("note"), QStringLiteral("tag")})
            for (const auto &entry : verse.value(kind + "Ranges").toList())
                result.insert(kind + ':' + entry.toMap().value("reference").toString());
        return result;
    };
    QSet<QString> active;
    for (const auto &verse : verses)
        if (verse.toMap().value("number").toInt() == cursorVerse) active = keys(verse.toMap());
    QVariantList result;
    bool adjacent = false;
    for (int i = 0; i < m_spans.size() && i < verses.size(); ++i) {
        const auto span = m_spans[i].toMap();
        const int number = span.value("number").toInt();
        const bool covered = number != cursorVerse && !(selectedStart > 0 && number >= selectedStart && number <= selectedEnd)
            && keys(verses[i].toMap()).intersects(active);
        if (covered) {
            if (adjacent) {
                auto range = result.last().toMap();
                range["end"] = span.value("end");
                result.last() = range;
            } else result.append(QVariantMap{{"start",span.value("start")}, {"end",span.value("end")}});
        }
        adjacent = covered;
    }
    return result;
}

QVariantList BlockDocument::annotations(const QVariantList &verses, const QVariantList &collection,
                                       int book, int chapter) const
{
    Q_UNUSED(collection);
    Q_UNUSED(book);
    Q_UNUSED(chapter);
    QMap<QString, QVariantMap> ranges;
    auto include = [&ranges](const QString &kind, const QString &reference, const QVariantMap &span,
                             bool before, bool after, const QVariantMap &metadata = QVariantMap{}) {
        const QString key = kind + ':' + reference;
        if (!ranges.contains(key)) {
            ranges[key] = {{"kind",kind}, {"reference",reference}, {"start",span.value("start")},
                           {"continuesBefore",before}};
            if (!metadata.isEmpty()) {
                // Document offsets use start/end; retain canonical endpoints in
                // their own frozen scope instead of overwriting them with offsets.
                ranges[key]["scope"] = metadata.value("scope", metadata);
                ranges[key]["tags"] = metadata.value("tags");
            }
        }
        ranges[key]["end"] = span.value("end");
        ranges[key]["continuesAfter"] = after;
    };
    for (const auto &value : m_spans) {
        const auto span = value.toMap();
        const int number = span.value("number").toInt();
        for (const auto &row : verses) {
            const auto verse = row.toMap();
            if (verse.value("number").toInt() != number) continue;
            for (const auto &entry : verse.value("bookmarkRanges").toList()) {
                const auto range = entry.toMap();
                include("bookmark",range.value("reference").toString(),span,
                        range.value("bookmarkContinuesBefore").toBool(),
                        range.value("bookmarkContinuesAfter").toBool(),range);
            }
            for (const auto &entry : verse.value("noteRanges").toList()) {
                const auto range = entry.toMap();
                include("note",range.value("reference").toString(),span,
                        range.value("continuesBefore").toBool(),
                        range.value("continuesAfter").toBool(),range);
            }
            for (const auto &entry : verse.value("tagRanges").toList()) {
                const auto range = entry.toMap();
                include("tag",range.value("reference").toString(),span,
                        range.value("continuesBefore").toBool(),
                        range.value("continuesAfter").toBool(),range);
            }
            break;
        }
    }
    QVariantList result;
    for (auto range : ranges) {
        const auto rects = rectangles(range.value("start").toInt(),range.value("end").toInt());
        if (rects.isEmpty()) continue;
        const auto first = rects.first().toMap(), last = rects.last().toMap();
        range["rects"] = rects;
        range["y"] = first.value("y");
        range["height"] = last.value("y").toDouble() + last.value("height").toDouble() - first.value("y").toDouble();
        result.append(range);
    }
    std::stable_sort(result.begin(),result.end(),[](const QVariant &a, const QVariant &b) {
        return a.toMap().value("start").toInt() < b.toMap().value("start").toInt();
    });
    return result;
}

int BlockDocument::verseAt(int position) const
{
    for (const auto &value : m_spans) {
        const auto span = value.toMap();
        if (position < span.value("end").toInt()) return span.value("number").toInt();
    }
    return m_spans.isEmpty() ? 0 : m_spans.last().toMap().value("number").toInt();
}

int BlockDocument::positionFor(int verse) const
{
    for (const auto &value : m_spans) {
        const auto span = value.toMap();
        if (span.value("number").toInt() == verse) return span.value("start").toInt();
    }
    return 0;
}

int BlockDocument::verseAtPoint(double x, double y) const
{
    if (!m_document) return 0;
    // Image cells at bidi boundaries may share a caret with the neighboring
    // visual run. Their reserved box has an unambiguous logical owner.
    for (const auto &entry : inlineDecorations()) {
        const auto marker = entry.toMap();
        if (QRectF(marker.value("x").toDouble(), marker.value("y").toDouble(),
                   marker.value("width").toDouble(), marker.value("height").toDouble()).contains(x, y))
            return marker.value("verse").toInt();
    }
    const auto documentLayout = m_document->documentLayout();
    const int nearest = documentLayout->hitTest(QPointF(x,y), Qt::FuzzyHit);
    const auto block = m_document->findBlock(nearest);
    if (!block.isValid()) return verseAt(nearest);
    const auto origin = documentLayout->blockBoundingRect(block).topLeft();
    const auto layout = block.layout();
    for (int i = 0; i < layout->lineCount(); ++i) {
        const auto line = layout->lineAt(i);
        if (y < origin.y() + line.y() || y >= origin.y() + line.y() + line.height()) continue;
        // Pointer actions target the character under the pointer, not the
        // nearest insertion caret (which can be past the final glyph). Qt's
        // visual-run hit testing also handles RTL and grapheme boundaries.
        const int character = line.xToCursor(x - origin.x(), QTextLine::CursorOnCharacter);
        // Inter-verse separators belong to the following verse, as in verseAt.
        return verseAt(block.position() + character);
    }
    // Heading margins and other blank space keep the existing nearest policy.
    return verseAt(nearest);
}

QVariantList BlockDocument::rectangles(int start, int end) const
{
    QVariantList result;
    if (!m_document) return result;
    for (auto block = m_document->findBlock(start); block.isValid() && block.position() < end; block = block.next()) {
        const auto origin = m_document->documentLayout()->blockBoundingRect(block).topLeft();
        const auto layout = block.layout();
        for (int i = 0; i < layout->lineCount(); ++i) {
            const auto line = layout->lineAt(i);
            const int first = qMax(start - block.position(),line.textStart());
            const int last = qMin(end - block.position(),line.textStart() + line.textLength());
            if (first >= last) continue;
            double height = line.height();
            // Include proportional leading and heading margins whenever this
            // logical range continues, so wrapped range paint has no seams.
            if (end > block.position() + line.textStart() + line.textLength()) {
                if (i + 1 < layout->lineCount()) height = layout->lineAt(i + 1).y() - line.y();
                else if (block.next().isValid())
                    height = m_document->documentLayout()->blockBoundingRect(block.next()).top() - origin.y() - line.y();
            }
            // A logical interval can have several visual runs in bidi text.
            // Endpoint-only rectangles miss the RTL run at a verse boundary.
            QList<QPair<double,double>> runs;
            for (int p = first; p < last; p = layout->nextCursorPosition(p)) {
                const double a = line.cursorToX(p,QTextLine::Leading);
                const double b = block.text().at(p) == QChar::ObjectReplacementCharacter
                    ? line.cursorToX(p + 1, QTextLine::Leading) : line.cursorToX(p,QTextLine::Trailing);
                runs.append({qMin(a,b),qMax(a,b)});
            }
            std::sort(runs.begin(),runs.end());
            QList<QPair<double,double>> merged;
            for (const auto &run : runs) {
                if (!merged.isEmpty() && run.first <= merged.last().second + 0.1)
                    merged.last().second = qMax(merged.last().second,run.second);
                else merged.append(run);
            }
            for (const auto &run : merged)
                result.append(QVariantMap{{"x",origin.x() + run.first}, {"y",origin.y() + line.y()},
                                         {"width",run.second-run.first}, {"height",height}});
        }
    }
    return result;
}
