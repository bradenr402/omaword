#include "aoimporter.h"
#include "biblebooks.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStringDecoder>
#include <QTemporaryFile>
#include <QUuid>
#include <algorithm>
#include <stdexcept>

namespace AoImporter {
namespace {
[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
constexpr qint64 MaxBytes = 64 * 1024 * 1024;
constexpr int MaxText = 65536;

QString string(const QJsonValue &value, bool empty = false)
{
    if (!value.isString() || value.toString().contains(QChar(0)) || value.toString().size() > MaxText
        || (!empty && value.toString().trimmed().isEmpty())) fail(QStringLiteral("Invalid AO text field."));
    return value.toString();
}

int integer(const QJsonValue &value, int minimum = 1, int maximum = 999)
{
    const double number = value.toDouble(-1);
    if (!value.isDouble() || number < minimum || number > maximum || number != int(number))
        fail(QStringLiteral("Invalid AO integer field."));
    return int(number);
}

QJsonArray array(const QJsonValue &value, int maximum)
{
    if (!value.isArray() || value.toArray().size() > maximum) fail(QStringLiteral("Invalid or oversized AO array."));
    return value.toArray();
}

void bounded(const QJsonValue &value, int depth = 0)
{
    if (depth > 32) fail(QStringLiteral("AO JSON nesting limit exceeded."));
    if (value.isArray()) for (const auto &child : value.toArray()) bounded(child, depth + 1);
    if (value.isObject()) for (const auto &child : value.toObject()) bounded(child, depth + 1);
    if (value.isString()) string(value, true);
}

struct Input { Edition edition; QJsonObject translation; QJsonArray books; QByteArray fingerprint; };
Input load(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) fail(QStringLiteral("Cannot open AO JSON: ") + file.errorString());
    if (file.size() <= 0 || file.size() > MaxBytes) fail(QStringLiteral("AO JSON is empty or exceeds the 64 MiB limit."));
    const auto bytes = file.readAll();
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString decoded = decoder.decode(bytes);
    if (decoder.hasError() || decoded.contains(QChar(0))) fail(QStringLiteral("Invalid UTF-8 AO JSON."));
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (!document.isObject() || error.error != QJsonParseError::NoError)
        fail(QStringLiteral("Invalid AO complete JSON: ") + error.errorString());
    bounded(document.object());
    Input input;
    input.translation = document.object().value(QStringLiteral("translation")).toObject();
    const auto &t = input.translation;
    input.edition.id = string(t.value(QStringLiteral("id")));
    if (!validId(input.edition.id)) fail(QStringLiteral("Invalid AO translation ID."));
    if (string(t.value(QStringLiteral("language"))) != QStringLiteral("eng")
        || string(t.value(QStringLiteral("textDirection"))) != QStringLiteral("ltr"))
        fail(QStringLiteral("This edition is not yet supported: English LTR editions only."));
    input.edition.name = string(t.value(QStringLiteral("name")));
    input.edition.abbreviation = t.value(QStringLiteral("shortName")).toString(input.edition.id);
    if (input.edition.abbreviation.trimmed().isEmpty()) input.edition.abbreviation = input.edition.id;
    if (input.edition.id == QStringLiteral("eng_net")) input.edition.abbreviation = QStringLiteral("NET");
    if (input.edition.id == QStringLiteral("eng_kjv")) input.edition.abbreviation = QStringLiteral("KJV");
    QStringList notices;
    for (const char *key : {"licenseNotice", "licenseNotes", "license", "licenseUrl", "website"}) {
        const auto value = t.value(QLatin1String(key));
        if (value.isString() && !value.toString().isEmpty()) notices.append(value.toString());
        else if (value.isObject()) notices.append(QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact)));
    }
    input.edition.copyright = notices.isEmpty() ? QStringLiteral("") : notices.join(QStringLiteral("\n\n"));
    input.books = array(document.object().value(QStringLiteral("books")), 80);
    if (input.books.isEmpty()) fail(QStringLiteral("AO download contains no books."));
    input.fingerprint = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
    return input;
}

struct Note { QString caller, text; int offset = 0; };
struct Span { int start = 0, end = 0; };
struct Verse {
    int number = 0;
    QString text, heading, levels;
    bool paragraph = false;
    QVector<Note> notes;
    QVector<Span> words;
};

Verse content(const QJsonArray &pieces, const QMap<int, Note> &notes, QSet<int> &used)
{
    Verse verse;
    QVector<int> levels;
    auto line = [&] {
        if (!verse.text.isEmpty() && !verse.text.endsWith('\n')) { verse.text += '\n'; levels.append(0); }
    };
    auto append = [&](const QString &text, int level, bool wj) {
        if (level > 0) line();
        if (levels.isEmpty()) levels.append(0);
        if (level > 0) levels.last() = level;
        const int start = verse.text.size();
        verse.text += text;
        for (QChar c : text) if (c == '\n') levels.append(level);
        if (wj && verse.text.size() > start) verse.words.append({start, int(verse.text.size())});
        if (verse.text.size() > MaxText) fail(QStringLiteral("AO verse text limit exceeded."));
    };
    for (qsizetype i = 0; i < pieces.size(); ++i) {
        const auto &piece = pieces[i];
        if (piece.isString()) { append(string(piece, true), 0, false); continue; }
        if (!piece.isObject()) fail(QStringLiteral("Unsupported AO verse content."));
        const auto object = piece.toObject();
        if (object.contains(QStringLiteral("text"))) {
            const int level = object.contains(QStringLiteral("poem")) ? integer(object.value(QStringLiteral("poem")), 0, 4) : 0;
            if (object.contains(QStringLiteral("wordsOfJesus")) && !object.value(QStringLiteral("wordsOfJesus")).isBool())
                fail(QStringLiteral("Invalid AO words-of-Jesus flag."));
            append(string(object.value(QStringLiteral("text")), true), level, object.value(QStringLiteral("wordsOfJesus")).toBool());
        } else if (object.contains(QStringLiteral("lineBreak"))) {
            if (object.value(QStringLiteral("lineBreak")) != true) fail(QStringLiteral("Invalid AO line break."));
            line();
        } else {
            if (object.contains(QStringLiteral("noteId"))) {
                const int id = integer(object.value(QStringLiteral("noteId")), 0, 100000);
                if (!notes.contains(id)) fail(QStringLiteral("AO content refers to a missing footnote."));
                auto note = notes.value(id); note.offset = verse.text.size(); verse.notes.append(note); used.insert(id);
            } else if (object.contains(QStringLiteral("heading"))) {
                if (!verse.heading.isEmpty()) verse.heading += '\n';
                verse.heading += string(object.value(QStringLiteral("heading")));
            } else fail(QStringLiteral("Unsupported AO verse content object."));
            // Markers separate fragments, but never insert a space before closing punctuation.
            if (i + 1 < pieces.size() && !verse.text.isEmpty() && !verse.text.back().isSpace()) {
                const auto next = pieces[i + 1];
                const QString text = next.isString() ? next.toString() : next.toObject().value(QStringLiteral("text")).toString();
                if (!text.isEmpty() && !text.front().isSpace() && !text.front().isPunct()
                    && !next.toObject().contains(QStringLiteral("poem"))) append(QStringLiteral(" "), 0, false);
            }
        }
    }
    int first = 0, last = verse.text.size();
    while (first < last && verse.text[first].isSpace()) ++first;
    if (verse.text.mid(first).startsWith(QStringLiteral("¶ "))) first += 2;
    while (last > first && verse.text[last - 1].isSpace()) --last;
    const int skippedLines = verse.text.left(first).count('\n');
    verse.text = verse.text.mid(first, last - first);
    for (auto &note : verse.notes) note.offset = std::clamp(note.offset - first, 0, last - first);
    for (auto &span : verse.words) {
        span.start = std::clamp(span.start - first, 0, last - first);
        span.end = std::clamp(span.end - first, 0, last - first);
    }
    for (int i = 0; i <= verse.text.count('\n'); ++i) verse.levels += QChar('0' + levels.value(skippedLines + i));
    return verse;
}

// Appends a further part of the verse, on a line of its own where the source
// breaks between them or either side is poetry; its notes and spans follow it.
void join(Verse &verse, const Verse &part, bool lineBreak)
{
    const bool line = lineBreak || verse.levels.back() != '0' || part.levels.front() != '0';
    verse.text += line ? '\n' : ' ';
    const int shift = verse.text.size();
    verse.text += part.text;
    if (verse.text.size() > MaxText) fail(QStringLiteral("AO verse text limit exceeded."));
    verse.levels += line ? part.levels : part.levels.mid(1);
    for (auto note : part.notes) { note.offset += shift; verse.notes.append(note); }
    for (auto span : part.words) { span.start += shift; span.end += shift; verse.words.append(span); }
}

struct Connection {
    QString name = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSqlDatabase db;
    Connection(const QString &path, bool readOnly = false) {
        db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name); db.setDatabaseName(path);
        if (readOnly) db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (!db.open()) {
            const auto error = db.lastError().text(); db = {}; QSqlDatabase::removeDatabase(name);
            fail(QStringLiteral("Cannot open AO text database: ") + error);
        }
    }
    ~Connection() { db.close(); db = {}; QSqlDatabase::removeDatabase(name); }
};
void exec(QSqlQuery &q, const QString &sql) { if (!q.exec(sql)) fail(q.lastError().text()); }
void insert(QSqlDatabase db, const QString &sql, const QVariantList &values) {
    QSqlQuery q(db); q.prepare(sql); for (const auto &value : values) q.addBindValue(value);
    if (!q.exec()) fail(q.lastError().text());
}
const QStringList queries = {
    QStringLiteral("SELECT key,value FROM metadata WHERE key<>'content_digest' ORDER BY key"),
    QStringLiteral("SELECT id,code,name,abbreviation,ordinal FROM books ORDER BY ordinal"),
    QStringLiteral("SELECT book,name FROM book_aliases ORDER BY book,name"),
    QStringLiteral("SELECT book,chapter FROM source_chapters ORDER BY book,chapter"),
    QStringLiteral("SELECT book,chapter,number,retained FROM source_keys ORDER BY book,chapter,number"),
    QStringLiteral("SELECT book,chapter,number,text,html,heading,poetry,paragraphStart FROM verses ORDER BY book,chapter,number"),
    QStringLiteral("SELECT book,chapter,verse,seq,kind,caller,offset,text,targets FROM notes ORDER BY book,chapter,verse,seq"),
    QStringLiteral("SELECT book,chapter,verse,start,end FROM words_of_jesus ORDER BY book,chapter,verse,start"),
    QStringLiteral("SELECT book,chapter,verse,levels FROM indents ORDER BY book,chapter,verse")
};
QString digest(QSqlDatabase db)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    auto field = [&](const QByteArray &bytes) { hash.addData(QByteArray::number(bytes.size())); hash.addData(":"); hash.addData(bytes); };
    QSqlQuery q(db);
    for (const auto &sql : queries) {
        field(sql.toUtf8()); exec(q, sql);
        while (q.next()) for (int i = 0; i < q.record().count(); ++i) field(q.value(i).toString().toUtf8());
        if (q.lastError().isValid()) fail(q.lastError().text());
    }
    return QString::fromLatin1(hash.result().toHex());
}

void validate(QSqlDatabase db)
{
    QSqlQuery q(db); exec(q, QStringLiteral("PRAGMA quick_check"));
    if (!q.next() || q.value(0).toString() != QStringLiteral("ok")) fail(QStringLiteral("AO database integrity check failed."));
    QMap<QString, QString> metadata;
    exec(q, QStringLiteral("SELECT key,value FROM metadata"));
    while (q.next()) metadata.insert(q.value(0).toString(), q.value(1).toString());
    if (metadata.value(QStringLiteral("schema")) != QStringLiteral("7")
        || metadata.value(QStringLiteral("source_format")) != QStringLiteral("ao-standard")
        || !validId(metadata.value(QStringLiteral("version_id"))) || metadata.value(QStringLiteral("edition")).isEmpty()
        || metadata.value(QStringLiteral("abbreviation")).isEmpty()) fail(QStringLiteral("Unsupported AO text database metadata."));
    exec(q, QStringLiteral("SELECT id,code,name,abbreviation,ordinal FROM books ORDER BY ordinal"));
    int books = 0;
    while (q.next()) {
        const int id = q.value(0).toInt();
        if (id < 1 || id > BibleBooks::canon().size() || q.value(1).toString() != BibleBooks::canon()[id - 1].code
            || q.value(2).toString().isEmpty() || q.value(3).toString().isEmpty() || q.value(4).toInt() != ++books)
            fail(QStringLiteral("Invalid AO book identity or ordering."));
    }
    if (!books || books != metadata.value(QStringLiteral("book_count")).toInt()) fail(QStringLiteral("AO book count mismatch."));
    const QStringList invalid = {
        QStringLiteral("SELECT 1 FROM source_keys WHERE book NOT IN (SELECT id FROM books) OR chapter NOT BETWEEN 1 AND 999 OR number NOT BETWEEN 1 AND 999 OR retained NOT IN (0,1) LIMIT 1"),
        QStringLiteral("SELECT 1 FROM source_keys s LEFT JOIN source_chapters c ON c.book=s.book AND c.chapter=s.chapter WHERE c.book IS NULL LIMIT 1"),
        QStringLiteral("SELECT 1 FROM source_chapters c WHERE c.book NOT IN (SELECT id FROM books) OR c.chapter NOT BETWEEN 1 AND 999 LIMIT 1"),
        QStringLiteral("SELECT 1 FROM books b WHERE NOT EXISTS(SELECT 1 FROM verses v WHERE v.book=b.id) LIMIT 1"),
        QStringLiteral("SELECT 1 FROM source_keys s LEFT JOIN verses v ON v.book=s.book AND v.chapter=s.chapter AND v.number=s.number WHERE (s.retained=1 AND v.book IS NULL) OR (s.retained=0 AND v.book IS NOT NULL) LIMIT 1"),
        QStringLiteral("SELECT 1 FROM verses v LEFT JOIN source_keys s ON v.book=s.book AND v.chapter=s.chapter AND v.number=s.number WHERE s.book IS NULL OR length(trim(v.text))=0 OR length(v.html)>262144 OR v.poetry NOT IN (0,1) OR v.paragraphStart NOT IN (0,1) LIMIT 1"),
        QStringLiteral("SELECT 1 FROM book_aliases WHERE book NOT IN (SELECT id FROM books) OR length(trim(name))=0 LIMIT 1")
    };
    for (const auto &sql : invalid) { exec(q, sql); if (q.next()) fail(QStringLiteral("AO source-key integrity mismatch.")); }
    // SQLite length() counts Unicode scalars; source offsets are UTF-16, so check them in QString.
    exec(q, QStringLiteral("SELECT n.offset,n.text,v.text,n.kind FROM notes n LEFT JOIN verses v ON v.book=n.book AND v.chapter=n.chapter AND v.number=n.verse"));
    while (q.next()) if (q.value(2).isNull() || q.value(0).toInt() < 0 || q.value(0).toInt() > q.value(2).toString().size()
        || q.value(1).toString().size() > MaxText || q.value(3).toString() != QStringLiteral("f")) fail(QStringLiteral("Invalid AO footnote offset."));
    exec(q, QStringLiteral("SELECT w.start,w.end,v.text FROM words_of_jesus w LEFT JOIN verses v ON v.book=w.book AND v.chapter=w.chapter AND v.number=w.verse"));
    while (q.next()) if (q.value(2).isNull() || q.value(0).toInt() < 0 || q.value(1).toInt() <= q.value(0).toInt()
        || q.value(1).toInt() > q.value(2).toString().size()) fail(QStringLiteral("Invalid AO words-of-Jesus span."));
    exec(q, QStringLiteral("SELECT i.levels,v.text FROM indents i LEFT JOIN verses v ON v.book=i.book AND v.chapter=i.chapter AND v.number=i.verse"));
    while (q.next()) if (q.value(1).isNull() || q.value(0).toString().size() != q.value(1).toString().count('\n') + 1
        || q.value(0).toString().contains(QRegularExpression(QStringLiteral("[^0-4]")))) fail(QStringLiteral("Invalid AO poetry levels."));
    exec(q, QStringLiteral("SELECT text FROM verses"));
    int verses = 0;
    while (q.next()) { if (q.value(0).toString().size() > MaxText) fail(QStringLiteral("AO verse limit exceeded.")); ++verses; }
    if (!verses || verses != metadata.value(QStringLiteral("verse_count")).toInt()) fail(QStringLiteral("AO verse count mismatch."));
    if (digest(db) != metadata.value(QStringLiteral("content_digest"))) fail(QStringLiteral("AO text database content digest mismatch."));
}
}

Edition inspectFile(const QString &path, QString *error)
{
    if (error) error->clear();
    try { return load(path).edition; }
    catch (const std::exception &e) { if (error) *error = QString::fromUtf8(e.what()); return {}; }
}

bool validateDatabase(const QString &path, QString *error)
{
    if (error) error->clear();
    try { Connection connection(path, true); validate(connection.db); return true; }
    catch (const std::exception &e) { if (error) *error = QString::fromUtf8(e.what()); return false; }
}

Result importFile(const QString &path, const QString &target, const std::shared_ptr<std::atomic<double>> &progress)
{
    Result result;
    if (progress) progress->store(0);
    try {
        const auto input = load(path); result.edition = input.edition;
        QTemporaryFile temporary(QFileInfo(target).absolutePath() + QStringLiteral("/.ao-XXXXXX.sqlite"));
        if (!temporary.open()) fail(QStringLiteral("Cannot create AO staging database: ") + temporary.errorString());
        const auto staging = temporary.fileName(); temporary.close();
        {
            Connection connection(staging); const auto db = connection.db; QSqlQuery q(db);
            exec(q, QStringLiteral("PRAGMA journal_mode=DELETE")); exec(q, QStringLiteral("BEGIN IMMEDIATE"));
            for (const auto &sql : QStringList{
                QStringLiteral("CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL) WITHOUT ROWID"),
                QStringLiteral("CREATE TABLE books(id INTEGER PRIMARY KEY,code TEXT NOT NULL UNIQUE,name TEXT NOT NULL,abbreviation TEXT NOT NULL,ordinal INTEGER NOT NULL UNIQUE)"),
                QStringLiteral("CREATE TABLE verses(book INTEGER,chapter INTEGER,number INTEGER,text TEXT NOT NULL,html TEXT NOT NULL,heading TEXT NOT NULL,poetry INTEGER NOT NULL,paragraphStart INTEGER NOT NULL,PRIMARY KEY(book,chapter,number)) WITHOUT ROWID"),
                QStringLiteral("CREATE TABLE source_keys(book INTEGER,chapter INTEGER,number INTEGER,retained INTEGER NOT NULL,PRIMARY KEY(book,chapter,number)) WITHOUT ROWID"),
                QStringLiteral("CREATE TABLE source_chapters(book INTEGER,chapter INTEGER,PRIMARY KEY(book,chapter)) WITHOUT ROWID"),
                QStringLiteral("CREATE TABLE book_aliases(book INTEGER,name TEXT,PRIMARY KEY(book,name)) WITHOUT ROWID"),
                QStringLiteral("CREATE TABLE notes(book INTEGER,chapter INTEGER,verse INTEGER,seq INTEGER,kind TEXT NOT NULL,caller TEXT NOT NULL,offset INTEGER NOT NULL,text TEXT NOT NULL,targets TEXT NOT NULL,PRIMARY KEY(book,chapter,verse,seq)) WITHOUT ROWID"),
                QStringLiteral("CREATE TABLE words_of_jesus(book INTEGER,chapter INTEGER,verse INTEGER,start INTEGER,end INTEGER NOT NULL,PRIMARY KEY(book,chapter,verse,start)) WITHOUT ROWID"),
                QStringLiteral("CREATE TABLE indents(book INTEGER,chapter INTEGER,verse INTEGER,levels TEXT NOT NULL,PRIMARY KEY(book,chapter,verse)) WITHOUT ROWID")}) exec(q, sql);
            QSet<int> bookIds, orders;
            auto books = input.books.toVariantList();
            std::sort(books.begin(), books.end(), [](const QVariant &a, const QVariant &b) { return a.toMap().value(QStringLiteral("order")).toInt() < b.toMap().value(QStringLiteral("order")).toInt(); });
            int ordinal = 0, noteCount = 0, words = 0, canonicalBooks = 0, canonicalChapters = 0, canonicalVerses = 0;
            for (const auto &value : books) {
                const auto book = QJsonObject::fromVariantMap(value.toMap());
                const auto code = string(book.value(QStringLiteral("id")));
                const int id = BibleBooks::bookId(code), order = integer(book.value(QStringLiteral("order")), 1, 1000);
                if (!id || QString::fromLatin1(BibleBooks::canon()[id - 1].code) != code) fail(QStringLiteral("Unsupported AO book code: ") + code);
                if (bookIds.contains(id) || orders.contains(order)) fail(QStringLiteral("Duplicate AO book or order."));
                bookIds.insert(id); orders.insert(order);
                const auto name = string(book.value(QStringLiteral("name")));
                insert(db, QStringLiteral("INSERT INTO books VALUES(?,?,?,?,?)"), {id, code, name, code, ++ordinal});
                QStringList aliases = {code, name, book.value(QStringLiteral("commonName")).toString(), book.value(QStringLiteral("title")).toString(), QString::fromUtf8(BibleBooks::canon()[id - 1].name)};
                aliases.append(QString::fromLatin1(BibleBooks::canon()[id - 1].aliases).split(' ', Qt::SkipEmptyParts));
                for (const auto &alias : aliases) if (!alias.isEmpty()) insert(db, QStringLiteral("INSERT OR IGNORE INTO book_aliases VALUES(?,?)"), {id, alias});
                const auto chapters = array(book.value(QStringLiteral("chapters")), 999);
                if (chapters.isEmpty() || integer(book.value(QStringLiteral("numberOfChapters")), 1, 999) != chapters.size()) fail(QStringLiteral("AO book chapter count mismatch."));
                int previousChapter = 0, bookVerses = 0;
                for (const auto &chapterValue : chapters) {
                    const auto wrapper = chapterValue.toObject(), chapter = wrapper.value(QStringLiteral("chapter")).toObject();
                    const int number = integer(chapter.value(QStringLiteral("number")));
                    if (number <= previousChapter) fail(QStringLiteral("Duplicate or unordered AO chapter."));
                    previousChapter = number;
                    insert(db, QStringLiteral("INSERT INTO source_chapters VALUES(?,?)"), {id, number});
                    QMap<int, Note> notes; QMap<int, int> references; QSet<int> used;
                    for (const auto &noteValue : array(chapter.value(QStringLiteral("footnotes")), 100000)) {
                        const auto n = noteValue.toObject(); const int noteId = integer(n.value(QStringLiteral("noteId")), 0, 100000);
                        if (notes.contains(noteId)) fail(QStringLiteral("Duplicate AO footnote ID."));
                        const auto caller = n.value(QStringLiteral("caller"));
                        if (!caller.isNull() && !caller.isString()) fail(QStringLiteral("Invalid AO footnote caller."));
                        notes.insert(noteId, {caller.isNull() ? QStringLiteral("") : string(caller, true), string(n.value(QStringLiteral("text")), true), 0});
                        if (n.contains(QStringLiteral("reference"))) {
                            const auto ref = n.value(QStringLiteral("reference")).toObject();
                            if (integer(ref.value(QStringLiteral("chapter"))) != number) fail(QStringLiteral("AO footnote chapter mismatch."));
                            // AO uses zero for subtitle notes, not a Scripture verse.
                            references.insert(noteId, integer(ref.value(QStringLiteral("verse")), 0));
                        }
                    }
                    QVector<Verse> verses; QString heading; QVector<Note> subtitleNotes; bool paragraph = true; int previousVerse = 0, chapterVerses = 0;
                    for (const auto &itemValue : array(chapter.value(QStringLiteral("content")), 100000)) {
                        const auto item = itemValue.toObject(); const auto type = string(item.value(QStringLiteral("type")));
                        if (type == QStringLiteral("line_break")) { paragraph = true; continue; }
                        if (type == QStringLiteral("heading")) {
                            QStringList parts; for (const auto &part : array(item.value(QStringLiteral("content")), 1000)) parts.append(string(part, true));
                            if (!heading.isEmpty()) heading += '\n';
                            heading += parts.join(' '); paragraph = true; continue;
                        }
                        auto verse = content(array(item.value(QStringLiteral("content")), 100000), notes, used);
                        if (type == QStringLiteral("hebrew_subtitle")) {
                            if (!heading.isEmpty()) heading += '\n';
                            heading += verse.text;
                            for (auto note : verse.notes) { note.offset = 0; subtitleNotes.append(note); }
                            continue;
                        }
                        if (type != QStringLiteral("verse")) fail(QStringLiteral("Unsupported AO chapter content type: ") + type);
                        verse.number = integer(item.value(QStringLiteral("number")), 0);
                        ++bookVerses; ++chapterVerses;
                        // A number that does not increase continues the verse before it, in the
                        // source's reading order: Brenton's Septuagint repeats a split verse's
                        // number, Onkelos numbers a second half 0, and a retelling may reorder
                        // verses. Headings between the parts move up to the verse they continue.
                        if (verse.number <= previousVerse) {
                            if (verses.isEmpty()) fail(QStringLiteral("Duplicate or unordered AO verse."));
                            if (verse.text.isEmpty()) continue;
                            if (!verses.last().text.isEmpty()) {
                                auto &whole = verses.last();
                                join(whole, verse, paragraph);
                                QStringList headings{whole.heading, heading, verse.heading};
                                headings.removeAll(QString());
                                whole.heading = headings.join('\n');
                                whole.notes = subtitleNotes + whole.notes;
                                heading.clear(); subtitleNotes.clear(); paragraph = false;
                                continue;
                            }
                            verse.number = verses.takeLast().number;
                        } else previousVerse = verse.number;
                        if (!verse.text.isEmpty()) {
                            if (!verse.heading.isEmpty() && !heading.isEmpty()) heading += '\n';
                            verse.heading = heading + verse.heading; heading.clear();
                            verse.notes = subtitleNotes + verse.notes; subtitleNotes.clear();
                            verse.paragraph = paragraph; paragraph = false;
                        }
                        verses.append(verse);
                    }
                    // A heading after the chapter's last verse (the KJV's subscriptions to
                    // Paul's letters) is dropped: headings stand above the verse they open.
                    if (integer(wrapper.value(QStringLiteral("numberOfVerses")), 0, 999) != chapterVerses) fail(QStringLiteral("AO chapter verse count mismatch."));
                    for (auto it = notes.cbegin(); it != notes.cend(); ++it) if (!used.contains(it.key())) {
                        auto found = std::find_if(verses.begin(), verses.end(), [&](const Verse &v) { return v.number == references.value(it.key()) && !v.text.isEmpty(); });
                        if (found == verses.end()) fail(QStringLiteral("Unanchored AO footnote."));
                        auto note = it.value(); note.offset = found->text.size(); found->notes.append(note);
                    }
                    for (const auto &verse : verses) {
                        insert(db, QStringLiteral("INSERT INTO source_keys VALUES(?,?,?,?)"), {id, number, verse.number, !verse.text.isEmpty()});
                        if (verse.text.isEmpty()) continue;
                        ++result.verseCount;
                        QString html;
                        int position = 0;
                        for (const auto &span : verse.words) if (span.start < span.end) {
                            html += verse.text.mid(position, span.start - position).toHtmlEscaped();
                            html += QStringLiteral("<span class=\"wj\">") + verse.text.mid(span.start, span.end - span.start).toHtmlEscaped() + QStringLiteral("</span>");
                            position = span.end;
                        }
                        html += verse.text.mid(position).toHtmlEscaped();
                        html.replace('\n', QStringLiteral("<br/>"));
                        insert(db, QStringLiteral("INSERT INTO verses VALUES(?,?,?,?,?,?,?,?)"), {id, number, verse.number, verse.text, html, verse.heading.isNull() ? QStringLiteral("") : verse.heading, verse.levels.contains(QRegularExpression(QStringLiteral("[1-4]"))), verse.paragraph});
                        if (verse.levels.contains(QRegularExpression(QStringLiteral("[1-4]")))) insert(db, QStringLiteral("INSERT INTO indents VALUES(?,?,?,?)"), {id, number, verse.number, verse.levels});
                        int seq = 0;
                        for (const auto &note : verse.notes) {
                            insert(db, QStringLiteral("INSERT INTO notes VALUES(?,?,?,?,?,?,?,?,?)"), {id, number, verse.number, ++seq, QStringLiteral("f"), note.caller, note.offset, note.text, QStringLiteral("")}); ++noteCount;
                        }
                        for (const auto &span : verse.words) if (span.start < span.end) {
                            insert(db, QStringLiteral("INSERT INTO words_of_jesus VALUES(?,?,?,?,?)"), {id, number, verse.number, span.start, span.end}); ++words;
                        }
                    }
                }
                if (integer(book.value(QStringLiteral("totalNumberOfVerses")), 0, 100000) != bookVerses) fail(QStringLiteral("AO book verse count mismatch."));
                if (!book.value(QStringLiteral("isApocryphal")).toBool()) { ++canonicalBooks; canonicalChapters += chapters.size(); canonicalVerses += bookVerses; }
                if (progress) progress->store(0.9 * ordinal / books.size());
            }
            const auto &t = input.translation;
            if (integer(t.value(QStringLiteral("numberOfBooks")), 1, 80) != canonicalBooks
                || integer(t.value(QStringLiteral("totalNumberOfChapters")), 1, 80000) != canonicalChapters
                || integer(t.value(QStringLiteral("totalNumberOfVerses")), 1, 1000000) != canonicalVerses)
                fail(QStringLiteral("AO translation coverage counts do not match its content."));
            QMap<QString, QString> metadata = {{QStringLiteral("schema"), QStringLiteral("7")}, {QStringLiteral("source_format"), QStringLiteral("ao-standard")},
                {QStringLiteral("version_id"), input.edition.id}, {QStringLiteral("edition"), input.edition.name}, {QStringLiteral("abbreviation"), input.edition.abbreviation},
                {QStringLiteral("copyright"), input.edition.copyright}, {QStringLiteral("paragraph_layout"), QStringLiteral("1")},
                {QStringLiteral("book_count"), QString::number(ordinal)}, {QStringLiteral("verse_count"), QString::number(result.verseCount)},
                {QStringLiteral("note_count"), QString::number(noteCount)}, {QStringLiteral("words_of_jesus"), QString::number(words)},
                {QStringLiteral("source_fingerprint"), QString::fromLatin1(input.fingerprint)},
                {QStringLiteral("ao_translation"), QString::fromUtf8(QJsonDocument(t).toJson(QJsonDocument::Compact))}};
            for (auto it = metadata.cbegin(); it != metadata.cend(); ++it) insert(db, QStringLiteral("INSERT INTO metadata VALUES(?,?)"), {it.key(), it.value()});
            insert(db, QStringLiteral("INSERT INTO metadata VALUES('content_digest',?)"), {digest(db)});
            exec(q, QStringLiteral("COMMIT")); validate(db);
        }
        QFile database(staging);
        if (!database.open(QIODevice::ReadOnly)) fail(QStringLiteral("Cannot reopen AO staging database."));
        QSaveFile output(target); output.setDirectWriteFallback(false);
        if (!output.open(QIODevice::WriteOnly)) fail(output.errorString());
        while (!database.atEnd()) {
            const auto bytes = database.read(65536);
            if ((bytes.isEmpty() && database.error() != QFile::NoError) || output.write(bytes) != bytes.size()) fail(QStringLiteral("Cannot publish AO text database."));
        }
        if (!output.commit()) fail(output.errorString());
        result.ok = true; if (progress) progress->store(1);
    } catch (const std::exception &e) { result.error = QString::fromUtf8(e.what()); }
    return result;
}
}
