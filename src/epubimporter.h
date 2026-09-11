#pragma once

#include <QString>
#include <QVector>
#include <atomic>
#include <memory>

namespace EpubImporter {
struct Book {
    const char *name;
    int chapters;
    int verses;
    const char *aliases;
};
const QVector<Book> &canon();
int bookId(QString name);
bool omittedVerse(int book, int chapter, int verse);
int chapterMaximum(int book, int chapter);
int chapterVerseCount(int book, int chapter);

struct Edition {
    QString id, abbreviation, name, copyright;
};
// Identifies supported structured editions without opening or writing a database.
// An empty id means failure; error explains unsupported/OCR layouts.
Edition inspectFile(const QString &path, QString *error = nullptr);

struct Result {
    bool ok = false;
    QString error;
    int verseCount = 0;
    Edition edition;
};

// Reads ZIP entries in memory via libarchive; never extracts the user's EPUB.
// Builds and validates a private SQLite database, then atomically installs it.
// All SQL connections are local to the calling thread.
Result importFile(const QString &epubPath, const QString &databasePath,
                  const std::shared_ptr<std::atomic<double>> &progress = {});
bool validateDatabase(const QString &databasePath, QString *error = nullptr);
// Haiola layout marker at the beginning of actual verse/boundary text only.
// Also normalizes the corresponding safe importer-generated HTML, if supplied.
// Embedded or quoted pilcrows are Scripture and remain untouched.
bool stripLayoutPilcrow(QString *text, QString *html = nullptr);
}
