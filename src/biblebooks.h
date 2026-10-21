#pragma once
#include <QString>
#include <QVector>

namespace BibleBooks {
struct Book { const char *code, *name, *aliases; };
const QVector<Book> &canon();
int bookId(QString name);
// The book an AO (USFM) code names, or 0. Exact: an alias never stands in
// for a code ("pss" is Psalms, but the code PSS is the Psalms of Solomon).
int codeId(const QString &code);
// A book with one chapter in every edition (Jude, Philemon): references
// leave its chapter out.
bool oneChapter(int id);
// A passage as OmaWord writes it: "John 3:16–18", "John 3"; a one-chapter
// book's verses without the chapter, "Jude 3", the book alone its chapter,
// "Jude", and a colon after a name ending in a number, "Psalm 151:3".
QString reference(int book, const QString &name, int chapter, int start = 0, int end = 0);
}
