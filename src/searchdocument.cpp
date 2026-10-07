#include "searchdocument.h"
#include "brand.h"

#include <QAbstractTextDocumentLayout>
#include <QCoreApplication>
#include <QQmlEngine>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextOption>
#include <QTextLayout>
#include <algorithm>

static void registerSearchDocument()
{
    static const auto module = (Brand::name() + QStringLiteral(".Reader")).toUtf8();
    qmlRegisterType<SearchDocument>(module.constData(), 1, 0, "SearchDocument");
}
Q_COREAPP_STARTUP_FUNCTION(registerSearchDocument)

void SourcePositionMap::build(const QString &source, const QString &rendered, int documentStart)
{
    m_positions = QList<QPair<int, int>>(source.size(), {-1, -1});
    int s = 0, r = 0;
    auto advance = [](const QString &text, int at) {
        return at + (text.at(at).isHighSurrogate() && at + 1 < text.size()
                     && text.at(at + 1).isLowSurrogate() ? 2 : 1);
    };
    // Inline decoration cells (word joiner + object, e.g. footnote markers)
    // occupy rendered positions with no source text; step over them.
    auto decoration = [&](int at) {
        const QChar c = rendered.at(at);
        return (c == QChar::ObjectReplacementCharacter || c == QChar(0x2060))
            && (s >= source.size() || source.at(s) != c);
    };
    while (s < source.size() || r < rendered.size()) {
        while (r < rendered.size() && decoration(r)) ++r;
        const int firstS = s, firstR = r;
        while (s < source.size() && source.at(s).isSpace()) ++s;
        while (r < rendered.size() && (rendered.at(r).isSpace() || decoration(r))) ++r;
        if (s != firstS || r != firstR) {
            // HTML may trim boundary whitespace. Internal non-whitespace text
            // never gets resynchronized by substring search (repeated words).
            if ((s == firstS || r == firstR) && firstS != 0 && firstR != 0
                && s != source.size() && r != rendered.size()) {
                m_positions.clear(); return;
            }
            for (int i = firstS; i < s; ++i)
                m_positions[i] = {documentStart + firstR, documentStart + r};
        }
        if (s == source.size() && r == rendered.size()) return;
        if (s == source.size() || r == rendered.size()) { m_positions.clear(); return; }
        const int nextS = advance(source, s), nextR = advance(rendered, r);
        if (QStringView(source).mid(s, nextS - s) != QStringView(rendered).mid(r, nextR - r)) {
            m_positions.clear(); return;
        }
        for (int i = s; i < nextS; ++i)
            m_positions[i] = {documentStart + r, documentStart + nextR};
        s = nextS; r = nextR;
    }
}

QPair<int, int> SourcePositionMap::range(int start, int length) const
{
    if (start < 0 || length <= 0 || start >= m_positions.size()
        || length > m_positions.size() - start) return {-1, -1};
    return {m_positions[start].first, m_positions[start + length - 1].second};
}

int SourcePositionMap::sourceStart(int position) const
{
    for (int i = 0; i < m_positions.size(); ++i)
        if (m_positions[i].first >= 0 && m_positions[i].second > position) return i;
    return -1;
}

int SourcePositionMap::sourceEnd(int position) const
{
    for (int i = m_positions.size() - 1; i >= 0; --i)
        if (m_positions[i].first >= 0 && m_positions[i].first < position) return i + 1;
    return 0;
}

QVariantList SearchGeometry::rectangles(QTextDocument *document, int start, int end)
{
    QVariantList result;
    if (!document || start < 0 || start >= end || end >= document->characterCount()) return result;
    for (auto block = document->findBlock(start); block.isValid() && block.position() < end; block = block.next()) {
        const auto origin = document->documentLayout()->blockBoundingRect(block).topLeft();
        const auto layout = block.layout();
        for (int i = 0; i < layout->lineCount(); ++i) {
            const auto line = layout->lineAt(i);
            int first = qMax(start - block.position(), line.textStart());
            const int last = qMin(end - block.position(), line.textStart() + line.textLength());
            if (first >= last) continue;
            // A backend range can start inside a combining cluster or surrogate
            // pair. Paint its complete glyph, without changing source offsets.
            if (!layout->isValidCursorPosition(first)) first = layout->previousCursorPosition(first);
            QList<QPair<double, double>> runs;
            for (int p = first; p < last;) {
                const int next = layout->nextCursorPosition(p);
                if (next <= p) break;
                const auto ch = block.text().at(p);
                if (ch != QChar::LineSeparator && ch != QChar::ParagraphSeparator) {
                    const double a = line.cursorToX(p, QTextLine::Leading);
                    const double b = line.cursorToX(p, QTextLine::Trailing);
                    if (qAbs(a - b) > 0.01) runs.append({qMin(a, b), qMax(a, b)});
                }
                p = next;
            }
            std::sort(runs.begin(), runs.end());
            QList<QPair<double, double>> merged;
            for (const auto &run : runs) {
                if (!merged.isEmpty() && run.first <= merged.last().second + 0.1)
                    merged.last().second = qMax(merged.last().second, run.second);
                else merged.append(run);
            }
            for (const auto &run : merged)
                result.append(QVariantMap{{"x", origin.x() + run.first}, {"y", origin.y() + line.y()},
                                         {"width", run.second - run.first}, {"height", line.height()}});
        }
    }
    return result;
}

QRectF SearchGeometry::firstRect(const QVariantList &rectangles)
{
    if (rectangles.isEmpty()) return {};
    const auto r = rectangles.first().toMap();
    return {r.value("x").toDouble(), r.value("y").toDouble(),
            r.value("width").toDouble(), r.value("height").toDouble()};
}

SearchDocument::SearchDocument(QObject *parent) : QObject(parent) {}

void SearchGeometry::forceLeftToRight(QTextDocument *document)
{
    if (!document) return;
    auto option = document->defaultTextOption();
    if (option.textDirection() != Qt::LeftToRight) {
        option.setTextDirection(Qt::LeftToRight);
        document->setDefaultTextOption(option);
    }
    // QQuickTextEdit recomputes its default option from the first strong
    // character, mirroring AlignLeft to AlignRight for Hebrew-first text. Block
    // formats take precedence, so pin every paragraph to absolute-left LTR.
    for (auto block = document->begin(); block.isValid(); block = block.next()) {
        const auto format = block.blockFormat();
        if (format.layoutDirection() == Qt::LeftToRight
                && format.alignment() == (Qt::AlignLeft | Qt::AlignAbsolute)) continue;
        QTextBlockFormat pinned;
        pinned.setLayoutDirection(Qt::LeftToRight);
        pinned.setAlignment(Qt::AlignLeft | Qt::AlignAbsolute);
        QTextCursor(block).mergeBlockFormat(pinned);
    }
}

void SearchDocument::leftToRight(QQuickTextDocument *target) const
{
    if (target) SearchGeometry::forceLeftToRight(target->textDocument());
}

QTextBlockFormat SearchGeometry::poetryFormat(QTextBlockFormat format, int level, qreal unit)
{
    format.setLeftMargin(level > 0 ? (level + 1) * unit : 0);
    format.setTextIndent(level > 0 ? -unit : 0);
    return format;
}

void SearchGeometry::indentLines(QTextDocument *document, const QVariantList &levels, qreal unit)
{
    if (!document || !std::any_of(levels.cbegin(), levels.cend(), [](const QVariant &level) { return level.toInt() > 0; })) return;
    for (QTextCursor found(document); !(found = document->find(QString(QChar::LineSeparator), found)).isNull();)
        found.insertBlock();
    int line = 0;
    for (auto block = document->begin(); block.isValid(); block = block.next(), ++line) {
        const int level = levels.value(qMin(line, int(levels.size()) - 1)).toInt();
        const auto format = poetryFormat(block.blockFormat(), level, unit);
        if (format != block.blockFormat()) QTextCursor(block).setBlockFormat(format);
    }
}

void SearchDocument::indentLines(QQuickTextDocument *target, const QVariantList &levels, qreal unit) const
{
    if (target) SearchGeometry::indentLines(target->textDocument(), levels, unit);
}

void SearchDocument::attach(QQuickTextDocument *target, const QString &sourceText)
{
    auto document = target ? target->textDocument() : nullptr;
    if (m_document == document && m_source == sourceText) return;
    if (m_document != document) {
        if (m_document) {
            disconnect(m_document, nullptr, this, nullptr);
            disconnect(m_document->documentLayout(), nullptr, this, nullptr);
        }
        m_document = document;
        if (m_document) {
            connect(m_document, &QTextDocument::contentsChanged, this, &SearchDocument::rebuildMap);
            connect(m_document->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
                    this, &SearchDocument::geometryChanged);
            connect(m_document, &QObject::destroyed, this, [this] { m_map = {}; emit geometryChanged(); });
        }
    }
    m_source = sourceText;
    rebuildMap();
}

void SearchDocument::rebuildMap()
{
    m_map.build(m_source, m_document ? m_document->toRawText() : QString());
    emit geometryChanged();
}

QVariantList SearchDocument::rectangles(const QVariantList &segments) const
{
    QVariantList result;
    for (const auto &value : segments) {
        const auto segment = value.toMap();
        const auto range = m_map.range(segment.value("start").toInt(), segment.value("length").toInt());
        for (const auto &rect : SearchGeometry::rectangles(m_document, range.first, range.second)) {
            auto entry = rect.toMap();
            entry["active"] = segment.value("active").toBool();
            result.append(entry);
        }
    }
    return result;
}

QRectF SearchDocument::rectFor(int start, int length) const
{
    const auto range = m_map.range(start, length);
    return SearchGeometry::firstRect(SearchGeometry::rectangles(m_document, range.first, range.second));
}

QVariantList SearchDocument::documentRange(int start, int length) const
{
    const auto range = m_map.range(start, length);
    if (range.first < 0) return {};
    return {range.first, range.second};
}

int SearchDocument::sourceAt(int position, bool forward) const
{
    return forward ? m_map.sourceStart(position) : m_map.sourceEnd(position);
}

QRectF SearchDocument::cursorRect(int position) const
{
    if (!m_document) return {};
    const auto block = m_document->findBlock(position);
    if (!block.isValid()) return {};
    const auto origin = m_document->documentLayout()->blockBoundingRect(block).topLeft();
    const auto line = block.layout()->lineForTextPosition(position - block.position());
    if (!line.isValid()) return {};
    return {origin.x() + line.cursorToX(position - block.position()), origin.y() + line.y(), 1, line.height()};
}
