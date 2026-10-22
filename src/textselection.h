#pragma once

#include <QList>
#include <QPair>
#include <QString>

// Character selection over a chapter's source text (the verses' plain `text`,
// never the rendered document). Positions are a verse index into the list and
// a UTF-16 offset into that verse's text. Verse breaks act as whitespace, so
// every motion crosses them; the cursor never rests on a break.
namespace TextSelection {
struct Verse {
    int number = 0;
    QString text;
    bool paragraphStart = false;
    // Its first and last lines are poem lines.
    bool poemStart = false, poemEnd = false;
};
// How a verse joins the one before it in copies, as the Paragraph layout
// sets them: a paragraph after a blank line, a poem line (its first, or the
// last before it) on a line of its own, otherwise after one space.
inline QString verseJoin(bool paragraphStart, bool previousPoemEnd, bool poemStart)
{
    return paragraphStart ? QStringLiteral("\n\n") : previousPoemEnd || poemStart ? QStringLiteral("\n") : QStringLiteral(" ");
}
// A verse number as copies number verses: "¹⁶".
QString superscript(int number);
struct Position {
    int verse = 0;
    int offset = 0;
    bool operator==(const Position &o) const { return verse == o.verse && offset == o.offset; }
};

// Vim motions with a count: h l w b e W B E ge ( ); gg G ignore the count.
// Unknown motions return the position unchanged.
Position move(const QList<Verse> &verses, Position from, const QString &motion, int count = 1);
// Text objects iw aw is as with a count. With anchor == cursor the object
// replaces the selection; otherwise it extends the selection past the cursor
// in the cursor's direction. Returns {anchor, cursor}, both inclusive.
QPair<Position, Position> textObject(const QList<Verse> &verses, Position anchor, Position cursor,
                                     const QString &object, int count = 1);
// Snaps a position onto a character (never a verse break or past the end).
Position clamp(const QList<Verse> &verses, Position at);
// The exclusive end offset of the character (grapheme) at `at`.
int characterEnd(const QList<Verse> &verses, Position at);
// Copy text for [start, end) (end exclusive, either verse may be partial).
// Verses join as verseJoin says. Plain text is the selection as is.
// A quotation replaces omitted text with an ellipsis where the selection
// starts after a verse's start or ends before its end (trimming whitespace and
// punctuation at the cut; the ellipsis is spaced from the words but not from
// the quotation marks), numbers only verses whose beginning is selected
// (never a single verse), and appends the reference.
QString text(const QList<Verse> &verses, Position start, Position end, bool quotation,
             const QString &reference = QString());
// The verse indexes [first, last] that [start, end) touches; {-1, -1} if empty.
QPair<int, int> touched(const QList<Verse> &verses, Position start, Position end);
}
