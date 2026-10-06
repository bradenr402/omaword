#include "textselection.h"

#include <QTextBoundaryFinder>
#include <algorithm>

namespace TextSelection {
namespace {
// One grapheme (or a verse break) of the chapter text.
struct Cell {
    int verse = -1;
    int start = 0;
    int end = 0;
    int kind = 0; // 0 blank (incl. verse breaks), 1 punctuation/other, 2 word
    bool brk = false;
    QChar first;
};

int kindOf(QChar c)
{
    if (c.isSpace()) return 0;
    if (c.isLetterOrNumber() || c.isMark() || c == u'_') return 2;
    return 1;
}

struct Model {
    QList<Cell> cells;
    QList<int> first; // first cell of each verse, -1 when empty
    QList<int> last;
    explicit Model(const QList<Verse> &verses)
    {
        for (int v = 0; v < verses.size(); ++v) {
            if (v > 0) { Cell b; b.brk = true; cells.append(b); }
            const QString &text = verses[v].text;
            first.append(-1); last.append(-1);
            QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
            int at = 0;
            while (at < text.size()) {
                int next = finder.toNextBoundary();
                if (next <= at) next = at + 1;
                Cell c; c.verse = v; c.start = at; c.end = next; c.first = text.at(at); c.kind = kindOf(c.first);
                if (first[v] < 0) first[v] = cells.size();
                last[v] = cells.size();
                cells.append(c);
                at = next;
            }
        }
    }
    int size() const { return cells.size(); }
    bool real(int k) const { return k >= 0 && k < cells.size() && !cells[k].brk; }
    int nextReal(int k) const { for (++k; k < size(); ++k) if (real(k)) return k; return -1; }
    int previousReal(int k) const { for (--k; k >= 0; --k) if (real(k)) return k; return -1; }
    int firstReal() const { return nextReal(-1); }
    int lastReal() const { return previousReal(size()); }
    int at(Position p) const
    {
        if (cells.isEmpty() || first.isEmpty()) return -1;
        const int v = qBound(0, p.verse, int(first.size()) - 1);
        if (first[v] < 0) {
            for (int w = v + 1; w < first.size(); ++w) if (first[w] >= 0) return first[w];
            for (int w = v - 1; w >= 0; --w) if (last[w] >= 0) return last[w];
            return -1;
        }
        if (p.offset <= 0) return first[v];
        for (int k = first[v]; k <= last[v]; ++k) if (p.offset < cells[k].end) return k;
        return last[v];
    }
    Position position(int k) const { return k < 0 ? Position{} : Position{cells[k].verse, cells[k].start}; }
    // Snap a (possibly break) cell onto a character, inward toward `toward`.
    int snap(int k, int toward) const
    {
        if (real(k)) return k;
        const int step = toward >= k ? nextReal(k) : previousReal(k);
        return step >= 0 ? step : (toward >= k ? previousReal(k) : nextReal(k));
    }
    bool sentenceEnd(int k) const
    {
        if (!real(k) || !QStringLiteral(".!?").contains(cells[k].first)) return false;
        int j = k + 1;
        static const QString closers = QStringLiteral(")]\"'\u2019\u201d\u00bb");
        while (real(j) && closers.contains(cells[j].first)) ++j;
        return j >= size() || cells[j].kind == 0;
    }
    QList<int> sentenceStarts(const QList<Verse> &verses) const
    {
        QList<int> starts;
        bool pending = true;
        int verse = -1;
        for (int k = 0; k < size(); ++k) {
            const Cell &c = cells[k];
            if (!c.brk && c.verse != verse) {
                verse = c.verse;
                if (verses[verse].paragraphStart) pending = true;
            }
            if (c.kind != 0 && pending) { starts.append(k); pending = false; }
            if (sentenceEnd(k)) pending = true;
        }
        return starts;
    }
};

int classOf(const Model &m, int k, bool big)
{
    const int kind = m.cells[k].kind;
    return big ? (kind ? 1 : 0) : kind;
}
int runStart(const Model &m, int k, bool big)
{
    const int c = classOf(m, k, big);
    while (k > 0 && classOf(m, k - 1, big) == c) --k;
    return k;
}
int runEnd(const Model &m, int k, bool big)
{
    const int c = classOf(m, k, big);
    while (k + 1 < m.size() && classOf(m, k + 1, big) == c) ++k;
    return k;
}
QString superscript(int number)
{
    static const QString digits = QStringLiteral("⁰¹²³⁴⁵⁶⁷⁸⁹");
    QString out;
    for (const QChar digit : QString::number(number)) out += digits[digit.digitValue()];
    return out;
}
int firstInk(const QString &text) { int i = 0; while (i < text.size() && text.at(i).isSpace()) ++i; return i; }
int lastInkEnd(const QString &text) { int i = text.size(); while (i > 0 && text.at(i - 1).isSpace()) --i; return i; }
bool cutChar(QChar c) { return c.isSpace() || c.isPunct(); }
}

Position clamp(const QList<Verse> &verses, Position at)
{
    const Model m(verses);
    return m.position(m.at(at));
}

int characterEnd(const QList<Verse> &verses, Position at)
{
    const Model m(verses);
    const int k = m.at(at);
    return k < 0 ? 0 : m.cells[k].end;
}

Position move(const QList<Verse> &verses, Position from, const QString &motion, int count)
{
    const Model m(verses);
    int k = m.at(from);
    if (k < 0) return from;
    count = qBound(1, count, 999999);
    const int n = m.size();
    if (motion == "gg") return m.position(m.firstReal());
    if (motion == "G") return m.position(m.lastReal());
    const bool big = motion == "W" || motion == "B" || motion == "E" || motion == "gE";
    for (int i = 0; i < count; ++i) {
        if (motion == "l" || motion == "h") {
            const int j = motion == "l" ? m.nextReal(k) : m.previousReal(k);
            if (j < 0) break;
            k = j;
        } else if (motion == "w" || motion == "W") {
            int j = k;
            const int c = classOf(m, j, big);
            if (c) while (j < n && classOf(m, j, big) == c) ++j;
            while (j < n && classOf(m, j, big) == 0) ++j;
            if (j >= n) { k = m.lastReal(); break; }
            k = j;
        } else if (motion == "e" || motion == "E") {
            int j = k + 1;
            while (j < n && classOf(m, j, big) == 0) ++j;
            if (j >= n) { k = m.lastReal(); break; }
            k = runEnd(m, j, big);
        } else if (motion == "b" || motion == "B") {
            int j = k - 1;
            while (j >= 0 && classOf(m, j, big) == 0) --j;
            if (j < 0) { k = m.firstReal(); break; }
            k = runStart(m, j, big);
        } else if (motion == "ge" || motion == "gE") {
            int j = k;
            const int c = classOf(m, j, big);
            if (c) while (j >= 0 && classOf(m, j, big) == c) --j;
            while (j >= 0 && classOf(m, j, big) == 0) --j;
            if (j < 0) { k = m.firstReal(); break; }
            k = j;
        } else if (motion == ")" || motion == "(") {
            const auto starts = m.sentenceStarts(verses);
            int j = -1;
            if (motion == ")") { for (int s : starts) if (s > k) { j = s; break; } }
            else for (auto it = starts.crbegin(); it != starts.crend(); ++it) if (*it < k) { j = *it; break; }
            if (j < 0) { k = motion == ")" ? m.lastReal() : m.firstReal(); break; }
            k = j;
        } else break;
    }
    return m.position(k);
}

QPair<Position, Position> textObject(const QList<Verse> &verses, Position anchor, Position cursor,
                                     const QString &object, int count)
{
    const Model m(verses);
    int a = m.at(anchor), c = m.at(cursor);
    if (a < 0 || c < 0) return {anchor, cursor};
    count = qBound(1, count, 999999);
    const int n = m.size();
    const bool extend = a != c;
    const bool forward = !extend || c > a;
    int k = c;
    if (extend) {
        const int j = forward ? c + 1 : c - 1;
        if (j < 0 || j >= n) return {m.position(a), m.position(c)};
        k = j;
    }
    int s = k, e = k;
    if (object == "iw" || object == "aw") {
        const bool around = object == "aw";
        s = runStart(m, k, false); e = runEnd(m, k, false);
        const bool onBlank = classOf(m, k, false) == 0;
        // Each further count adds one run (iw) or one word (aw), in the direction of travel.
        auto grow = [&](bool words) {
            if (forward) {
                int j = e + 1;
                if (j >= n) return;
                if (words) { while (j < n && classOf(m, j, false) == 0) ++j; if (j >= n) return; }
                e = runEnd(m, j, false);
            } else {
                int j = s - 1;
                if (j < 0) return;
                if (words) { while (j >= 0 && classOf(m, j, false) == 0) --j; if (j < 0) return; }
                s = runStart(m, j, false);
            }
        };
        if (!around) { for (int i = 1; i < count; ++i) grow(false); }
        else if (onBlank) { for (int i = 0; i < count; ++i) grow(true); }
        else {
            for (int i = 1; i < count; ++i) grow(true);
            // Trailing white space, else leading.
            if (forward && e + 1 < n && classOf(m, e + 1, false) == 0) e = runEnd(m, e + 1, false);
            else if (!forward && s > 0 && classOf(m, s - 1, false) == 0) s = runStart(m, s - 1, false);
            else if (s > 0 && classOf(m, s - 1, false) == 0) s = runStart(m, s - 1, false);
            else if (e + 1 < n && classOf(m, e + 1, false) == 0) e = runEnd(m, e + 1, false);
        }
    } else if (object == "is" || object == "as") {
        const auto starts = m.sentenceStarts(verses);
        auto startAtOrBefore = [&](int at) { int r = m.firstReal(); for (int x : starts) if (x <= at) r = x; return r; };
        auto nextAfter = [&](int at) { for (int x : starts) if (x > at) return x; return n; };
        auto sentenceEndOf = [&](int st, bool around) {
            int end = nextAfter(st) - 1;
            if (!around) while (end > st && m.cells[end].kind == 0) --end;
            return end;
        };
        const bool around = object == "as";
        s = startAtOrBefore(k);
        e = sentenceEndOf(s, around);
        for (int i = 1; i < count; ++i) {
            if (forward) { const int next = nextAfter(s == e ? e : e); if (next >= n) break; e = sentenceEndOf(next, around); }
            else { if (s <= 0) break; const int previous = startAtOrBefore(s - 1); if (previous >= s) break; s = previous; }
        }
    } else return {m.position(a), m.position(c)};
    s = m.snap(s, e); e = m.snap(e, s);
    if (s < 0 || e < 0) return {m.position(a), m.position(c)};
    if (extend) return forward ? qMakePair(m.position(a), m.position(e)) : qMakePair(m.position(a), m.position(s));
    return {m.position(s), m.position(e)};
}

QPair<int, int> touched(const QList<Verse> &verses, Position start, Position end)
{
    if (verses.isEmpty()) return {-1, -1};
    int first = qBound(0, start.verse, int(verses.size()) - 1), last = qBound(0, end.verse, int(verses.size()) - 1);
    int s = first == start.verse ? qMax(0, start.offset) : 0;
    int e = last == end.verse ? end.offset : int(verses[last].text.size());
    // A start at a verse's end or an end at a verse's start selects nothing there.
    while (first < last && s >= verses[first].text.size()) { ++first; s = 0; }
    while (last > first && e <= 0) { --last; e = verses[last].text.size(); }
    if (first > last || (first == last && s >= e)) return {-1, -1};
    return {first, last};
}

QString text(const QList<Verse> &verses, Position start, Position end, bool quotation, const QString &reference)
{
    const auto range = touched(verses, start, end);
    if (range.first < 0) return {};
    const bool single = range.first == range.second;
    QString body;
    for (int v = range.first; v <= range.second; ++v) {
        const QString &whole = verses[v].text;
        const int s = v == range.first && v == start.verse ? qBound(0, start.offset, int(whole.size())) : 0;
        const int e = v == range.second && v == end.verse ? qBound(s, end.offset, int(whole.size())) : int(whole.size());
        QString piece = whole.mid(s, e - s);
        if (v > range.first) {
            if (verses[v].paragraphStart) body += QStringLiteral("\n\n");
            else if (verses[v].poetry && verses[v - 1].poetry) body += u'\n';
            else body += u' ';
        }
        if (quotation) {
            const bool cutStart = s > firstInk(whole);
            const bool cutEnd = e < lastInkEnd(whole);
            QString trimmed = piece;
            if (cutStart) { int i = 0; while (i < trimmed.size() && cutChar(trimmed.at(i))) ++i; trimmed = trimmed.mid(i); }
            if (cutEnd) { int i = trimmed.size(); while (i > 0 && cutChar(trimmed.at(i - 1))) --i; trimmed.truncate(i); }
            if (trimmed.isEmpty() && (cutStart || cutEnd)) trimmed = piece.trimmed();
            // "…" is spaced from the words, never from the quotation marks:
            // "… so loved", "the world …", and "word … word" in between.
            piece = (cutStart ? QStringLiteral("\u2026 ") : QString()) + trimmed + (cutEnd ? QStringLiteral(" \u2026") : QString());
            if (!single && !cutStart) piece = superscript(verses[v].number) + u' ' + piece;
        }
        body += piece;
    }
    if (!quotation) return body;
    return QStringLiteral("\u201c") + body + QStringLiteral("\u201d\n\n\u2014") + reference;
}
}
