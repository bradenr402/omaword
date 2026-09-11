#include "blockdocument.h"

#include <QAbstractTextDocumentLayout>
#include <QCoreApplication>
#include <QQmlEngine>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextFragment>
#include <QTextLayout>
#include <algorithm>

static void registerBlockDocument()
{
    qmlRegisterType<BlockDocument>("OmaBible.Reader", 1, 0, "BlockDocument");
}
Q_COREAPP_STARTUP_FUNCTION(registerBlockDocument)

void BlockDocument::build(QQuickTextDocument *target, const QVariantList &verses,
                          bool numbers, bool headings, double lineHeight)
{
    if (!target || !target->textDocument()) return;
    m_document = target->textDocument();
    m_document->clear();
    m_document->setDocumentMargin(0);
    m_spans.clear();
    QTextCursor cursor(m_document);
    QTextBlockFormat paragraph;
    paragraph.setLineHeight(lineHeight * 100, QTextBlockFormat::ProportionalHeight);
    cursor.setBlockFormat(paragraph);
    const QTextCharFormat normal;
    for (const auto &value : verses) {
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
        } else if (cursor.position()) cursor.insertText(" ", normal);
        const int start = cursor.position();
        if (numbers) {
            QTextCharFormat number;
            number.setVerticalAlignment(QTextCharFormat::AlignSuperScript);
            cursor.insertText(verse.value("number").toString(), number);
            cursor.insertText(QString(QChar(0x00a0)), normal);
        }
        const int textStart = cursor.position();
        QTextDocument source;
        const auto html = verse.value("html").toString();
        if (html.isEmpty()) source.setPlainText(verse.value("text").toString());
        else source.setHtml(html); // Already sanitized by the EPUB importer.
        // Flatten source paragraph wrappers, preserving internal poetry breaks
        // and inline formats. Only visible section headings start paragraphs.
        for (auto block = source.begin(); block.isValid(); block = block.next()) {
            if (block != source.begin()) cursor.insertText(QString(QChar::LineSeparator), normal);
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const auto fragment = it.fragment();
                if (fragment.isValid()) cursor.insertText(fragment.text(), fragment.charFormat());
            }
        }
        m_spans.append(QVariantMap{{"number",verse.value("number")}, {"start",start},
                                  {"textStart",textStart}, {"end",cursor.position()}});
    }
    emit spansChanged();
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
                const double b = line.cursorToX(p,QTextLine::Trailing);
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
