#pragma once
#include <QString>
#include <QVector>

namespace BibleBooks {
struct Book { const char *code, *name, *aliases; };
const QVector<Book> &canon();
int bookId(QString name);
}
