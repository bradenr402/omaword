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
}
