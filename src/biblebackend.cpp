#include "biblebackend.h"

#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <cmath>

namespace {
constexpr qsizetype MaximumNoteSize = 1024 * 1024;
constexpr qint64 MaximumBackupSize = 32 * 1024 * 1024;
QString connectionName() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
bool exactKeys(const QJsonObject &object, const QStringList &keys)
{
    const QStringList actual = object.keys();
    return QSet<QString>(actual.cbegin(), actual.cend()) == QSet<QString>(keys.cbegin(), keys.cend());
}
bool integer(const QJsonValue &value, int minimum, int maximum)
{
    if (!value.isDouble()) return false;
    const double n = value.toDouble();
    return std::isfinite(n) && n == std::floor(n) && n >= minimum && n <= maximum;
}
bool safeString(const QString &text)
{
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text[i];
        if (c.unicode() == 0 || (c.unicode() < 32 && c != '\n' && c != '\r' && c != '\t')) return false;
        if (c.isHighSurrogate()) {
            if (++i >= text.size() || !text[i].isLowSurrogate()) return false;
        } else if (c.isLowSurrogate()) return false;
    }
    return true;
}
// Qt's DOM parser accepts duplicate object keys with last-value-wins behavior.
// Detect them in the original (already syntax-validated) JSON, including keys
// spelled with escapes, so a restore has exactly one interpretation.
bool uniqueJsonKeys(const QByteArray &json)
{
    QVector<QSet<QString>> objects;
    for (qsizetype i = 0; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '{') objects.append(QSet<QString>());
        else if (c == '}') {
            if (objects.isEmpty()) return false;
            objects.removeLast();
        } else if (c == '"') {
            const qsizetype begin = i++;
            for (; i < json.size(); ++i) {
                if (json[i] == '\\') ++i;
                else if (json[i] == '"') break;
            }
            qsizetype next = i + 1;
            while (next < json.size() && (json[next] == ' ' || json[next] == '\n' || json[next] == '\r' || json[next] == '\t')) ++next;
            if (next < json.size() && json[next] == ':') {
                if (objects.isEmpty()) return false;
                const auto keyDocument = QJsonDocument::fromJson('[' + json.mid(begin,i-begin+1) + ']');
                const QString key = keyDocument.array().first().toString();
                if (objects.last().contains(key)) return false;
                objects.last().insert(key);
            }
        }
    }
    return objects.isEmpty();
}
bool writeAtomic(const QString &path, const QByteArray &bytes, QString *error)
{
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        *error = file.errorString();
        return false;
    }
    return true;
}
}

BibleBackend::BibleBackend(QObject *parent) : QObject(parent)
{
    m_directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!QDir().mkpath(m_directory)) {
        setError("Cannot create Omabible data directory: " + m_directory);
        return;
    }
    if (!initializeStore()) return;
    if (QFileInfo::exists(m_directory + "/bible.sqlite")) {
        QString error;
        if (EpubImporter::validateDatabase(m_directory + "/bible.sqlite", &error)) {
            m_ready = openBible();
            if (m_ready) {
                if (!validRange(m_book, m_chapter, m_start, m_end)) {
                    m_book = 1; m_chapter = 1; m_start = m_end = 0; m_position = 0;
                }
                m_anchor = m_start; m_cursor = m_end;
                if (!loadChapter()) m_ready = false;
                refreshCollection();
            }
        } else setError(error + " Re-import your NET Bible 2.1 EPUB to repair it.");
    }
    setStatus(m_ready ? "Ready · NET Bible 2.1" : "Import your NET Bible 2.1 EPUB to begin.");
    m_progressTimer.setInterval(100);
    connect(&m_progressTimer, &QTimer::timeout, this, [this] {
        const double value = m_importProgress ? m_importProgress->load() : 0;
        if (value != m_progress) { m_progress = value; emit importProgressChanged(); }
    });
    connect(&m_importWatcher, &QFutureWatcher<EpubImporter::Result>::finished, this, [this] {
        m_progressTimer.stop();
        const auto result = m_importWatcher.result();
        if (result.ok) {
            closeBible();
            m_ready = openBible();
            if (m_ready) {
                if (!validRange(m_book, m_chapter, m_start, m_end)) {
                    m_book = 1; m_chapter = 1; m_start = m_end = 0; m_position = 0;
                }
                if (!loadChapter()) m_ready = false;
                refreshCollection();
            }
            m_progress = 1;
            emit readyChanged(); emit passageChanged(); emit selectionChanged();
            emit readingPositionChanged(); emit navigationChanged();
            setStatus(m_ready ? "Imported 66 books · 1,189 chapters · NET Bible 2.1" : "Import installed; could not open the Bible database.");
        } else {
            setError(result.error);
            setStatus(m_ready ? "Import failed. Your existing Bible is available." : "Import failed. Choose a complete NET Bible 2.1 EPUB.");
        }
        m_busy = false;
        emit importProgressChanged(); emit busyChanged();
    });
}

BibleBackend::~BibleBackend()
{
    // The worker captures only paths and shared progress, never this QObject.
    // Waiting ensures a normal application exit cannot leave a half-finished import.
    m_importWatcher.waitForFinished();
    closeBible();
    m_store.close(); m_store = QSqlDatabase();
    if (!m_storeConnection.isEmpty()) QSqlDatabase::removeDatabase(m_storeConnection);
}

bool BibleBackend::setError(const QString &error)
{
    if (m_error != error) { m_error = error; emit errorChanged(); }
    return false;
}
void BibleBackend::clearError() { setError({}); }
void BibleBackend::setStatus(const QString &status)
{
    if (m_status != status) { m_status = status; emit statusChanged(); }
}
QString BibleBackend::dataDirectory() { return m_directory; }
QString BibleBackend::reference(int b, int c, int start, int end)
{
    if (b < 1 || b > 66) return {};
    QString result = QString("%1 %2").arg(EpubImporter::canon()[b - 1].name).arg(c);
    if (start) {
        result += ':' + QString::number(start);
        if (end > start) result += QChar(0x2013) + QString::number(end);
    }
    return result;
}
QString BibleBackend::passage() const { return reference(m_book, m_chapter); }
QString BibleBackend::selectionReference() const { return reference(m_book, m_chapter, m_start, m_end); }
int BibleBackend::chapterCount() const { return EpubImporter::canon()[m_book - 1].chapters; }
QVariantList BibleBackend::books() const
{
    QVariantList list;
    for (int i = 0; i < 66; ++i) {
        const auto &b = EpubImporter::canon()[i];
        list.append(QVariantMap{{"id", i + 1}, {"name", QString::fromUtf8(b.name)}, {"chapters", b.chapters}});
    }
    return list;
}

bool BibleBackend::initializeStore()
{
    m_storeConnection = connectionName();
    m_store = QSqlDatabase::addDatabase("QSQLITE", m_storeConnection);
    m_store.setDatabaseName(m_directory + "/personal.sqlite");
    m_store.setConnectOptions("QSQLITE_BUSY_TIMEOUT=5000");
    if (!m_store.open()) return setError("Cannot open your notes database: " + m_store.lastError().text());
    QSqlQuery q(m_store);
    if (!q.exec("PRAGMA user_version") || !q.next()) return setError("Cannot read notes database version.");
    const int version = q.value(0).toInt();
    if (version > 1) { m_store.close(); return setError("Your notes database was created by a newer Omabible version."); }
    if (!q.exec("PRAGMA journal_mode=WAL") || !q.exec("PRAGMA synchronous=FULL"))
        return setError("Cannot configure reliable note saving: " + q.lastError().text());
    if (!m_store.transaction()) return setError(m_store.lastError().text());
    const QStringList schema = {
        "CREATE TABLE IF NOT EXISTS annotations(book INTEGER NOT NULL CHECK(book BETWEEN 1 AND 66),chapter INTEGER NOT NULL CHECK(chapter BETWEEN 1 AND 150),start INTEGER NOT NULL CHECK(start BETWEEN 1 AND 176),end INTEGER NOT NULL CHECK(end BETWEEN start AND 176),bookmark INTEGER NOT NULL CHECK(bookmark IN(0,1)),highlight INTEGER NOT NULL CHECK(highlight IN(0,1)),note TEXT NOT NULL,PRIMARY KEY(book,chapter,start,end),CHECK(bookmark OR highlight OR length(note)>0)) WITHOUT ROWID",
        "CREATE TABLE IF NOT EXISTS reading(id INTEGER PRIMARY KEY CHECK(id=1),book INTEGER NOT NULL,chapter INTEGER NOT NULL,start INTEGER NOT NULL,end INTEGER NOT NULL,position REAL NOT NULL CHECK(position BETWEEN 0 AND 1))",
        "INSERT OR IGNORE INTO reading VALUES(1,1,1,0,0,0)",
        "PRAGMA user_version=1"
    };
    for (const auto &sql : schema) {
        if (!q.exec(sql)) { m_store.rollback(); return setError("Cannot initialize notes: " + q.lastError().text()); }
    }
    if (!m_store.commit()) { m_store.rollback(); return setError("Cannot save notes database schema: " + m_store.lastError().text()); }
    if (!q.exec("SELECT book,chapter,start,end,position FROM reading WHERE id=1") || !q.next())
        return setError("Cannot read your saved reading location: " + q.lastError().text());
    m_book = q.value(0).toInt(); m_chapter = q.value(1).toInt(); m_start = q.value(2).toInt();
    m_end = q.value(3).toInt(); m_position = q.value(4).toDouble();
    if (m_book < 1 || m_book > 66 || m_chapter < 1 || m_chapter > EpubImporter::canon()[m_book-1].chapters
        || !std::isfinite(m_position) || m_position < 0 || m_position > 1) {
        m_book = 1; m_chapter = 1; m_start = m_end = 0; m_position = 0;
    }
    return loadAnnotations(&m_annotations);
}

bool BibleBackend::openBible()
{
    m_bibleConnection = connectionName();
    m_bible = QSqlDatabase::addDatabase("QSQLITE", m_bibleConnection);
    m_bible.setDatabaseName(m_directory + "/bible.sqlite");
    m_bible.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=5000");
    if (!m_bible.open()) return setError("Cannot open the imported Bible: " + m_bible.lastError().text());
    return true;
}
void BibleBackend::closeBible()
{
    m_bible.close(); m_bible = QSqlDatabase();
    if (!m_bibleConnection.isEmpty()) QSqlDatabase::removeDatabase(m_bibleConnection);
    m_bibleConnection.clear();
}
bool BibleBackend::requireReady()
{
    return m_ready || setError("Import your NET Bible 2.1 EPUB first.");
}
QString BibleBackend::localPath(const QUrl &url)
{
    if (!url.isValid() || !url.isLocalFile() || url.hasQuery() || url.hasFragment()
        || (!url.host().isEmpty() && url.host() != "localhost")) {
        setError("Please choose a local file or directory."); return {};
    }
    const QString path = url.toLocalFile();
    if (path.isEmpty() || !QFileInfo(path).isAbsolute()) { setError("Please choose an absolute local file path."); return {}; }
    return path;
}
void BibleBackend::importEpub(QUrl file)
{
    if (m_busy) { setError("An EPUB import is already running."); return; }
    if (!m_store.isOpen()) { setError("Your notes database is unavailable. Check the data directory permissions."); return; }
    const QString path = localPath(file);
    if (path.isEmpty()) return;
    if (!QFileInfo(path).isFile() || !QFileInfo(path).isReadable()) { setError("Cannot read the selected EPUB file."); return; }
    clearError();
    m_busy = true; m_progress = 0;
    m_importProgress = std::make_shared<std::atomic<double>>(0);
    setStatus("Importing and validating NET Bible 2.1…");
    emit busyChanged(); emit importProgressChanged();
    const QString target = m_directory + "/bible.sqlite";
    const auto progress = m_importProgress;
    m_importWatcher.setFuture(QtConcurrent::run([path, target, progress] { return EpubImporter::importFile(path, target, progress); }));
    m_progressTimer.start();
}

bool BibleBackend::verseExists(int b, int c, int v) const
{
    QSqlQuery q(m_bible);
    q.prepare("SELECT 1 FROM verses WHERE book=? AND chapter=? AND number=?");
    q.addBindValue(b); q.addBindValue(c); q.addBindValue(v);
    return q.exec() && q.next();
}
bool BibleBackend::validRange(int b, int c, int start, int end) const
{
    if (b < 1 || b > 66 || c < 1 || c > EpubImporter::canon()[b-1].chapters) return false;
    if (!start && !end) return true;
    if (start < 1 || end < start || end > 176) return false;
    return verseExists(b,c,start) && verseExists(b,c,end);
}
bool BibleBackend::persistState(int b, int c, int start, int end, double position)
{
    QSqlQuery q(m_store);
    q.prepare("UPDATE reading SET book=?,chapter=?,start=?,end=?,position=? WHERE id=1");
    q.addBindValue(b); q.addBindValue(c); q.addBindValue(start); q.addBindValue(end); q.addBindValue(position);
    if (!q.exec() || q.numRowsAffected() != 1) return setError("Cannot save your reading location: " + q.lastError().text());
    return true;
}

bool BibleBackend::loadChapter()
{
    QVariantList rows;
    if (!readChapter(m_book,m_chapter,&rows)) return false;
    m_verses = rows;
    updateAnnotations();
    return true;
}

bool BibleBackend::readChapter(int book, int chapter, QVariantList *result)
{
    QSqlQuery q(m_bible);
    q.prepare("SELECT number,text,html,heading,poetry FROM verses WHERE book=? AND chapter=? ORDER BY number");
    q.addBindValue(book); q.addBindValue(chapter);
    if (!q.exec()) return setError("Cannot load chapter: " + q.lastError().text());
    QVariantList rows;
    while (q.next()) rows.append(QVariantMap{{"number",q.value(0)}, {"text",q.value(1)}, {"html",q.value(2)},
        {"heading",q.value(3)}, {"poetry",q.value(4).toBool()}, {"highlighted",false}, {"bookmarked",false}, {"hasNote",false}});
    if (q.lastError().isValid() || rows.size() != EpubImporter::chapterVerseCount(book,chapter))
        return setError("The requested chapter could not be read completely. Please re-import your EPUB.");
    int expected = 1;
    for (const auto &row : rows) {
        while (EpubImporter::omittedVerse(book,chapter,expected)) ++expected;
        const auto map = row.toMap();
        if (map.value("number").toInt() != expected++ || map.value("text").toString().trimmed().isEmpty()
            || map.value("html").toString().trimmed().isEmpty())
            return setError("The requested chapter contains invalid verses. Please re-import your EPUB.");
    }
    *result = rows;
    return true;
}

bool BibleBackend::goTo(int b, int c, int start, int end)
{
    if (!requireReady()) return false;
    if (!validRange(b,c,start,end)) return setError("That chapter or verse range is not present in NET Bible 2.1.");
    const bool changed = b != m_book || c != m_chapter;
    const double position = changed || start ? 0 : m_position;
    QVariantList rows;
    if (!readChapter(b,c,&rows)) return false;
    if (!persistState(b,c,start,end,position)) return false;
    m_book = b; m_chapter = c; m_start = start; m_end = end;
    m_anchor = start; m_cursor = end; m_position = position;
    m_verses = rows;
    updateAnnotations();
    clearError();
    emit passageChanged(); emit selectionChanged(); emit readingPositionChanged(); emit navigationChanged();
    return true;
}

bool BibleBackend::navigate(QString input)
{
    if (!requireReady()) return false;
    input = input.simplified();
    input.replace(QChar(0x2013), '-'); input.replace(QChar(0x2014), '-'); input.replace(QChar(0x2212), '-');
    if (input.size() > 200 || input.isEmpty()) return setError("Enter a reference such as John 3:16–18.");
    // Try every split: this handles compact names (1Jn3:16), numbered books,
    // punctuation in aliases, and full multi-word names without fuzzy matches.
    static const QRegularExpression suffix("^(\\d+)(?:\\s*:\\s*(\\d+)(?:\\s*-\\s*(?:(\\d+)\\s*:\\s*)?(\\d+))?)?$");
    int b = EpubImporter::bookId(input), c = 1, start = 0, end = 0;
    bool matched = b != 0;
    if (!matched) {
        for (qsizetype split = input.size() - 1; split >= 0; --split) {
            const int candidate = split == 0 ? m_book : EpubImporter::bookId(input.left(split));
            if (!candidate) continue;
            const auto match = suffix.match(input.mid(split).trimmed());
            if (!match.hasMatch()) continue;
            b = candidate; c = match.captured(1).toInt();
            if (!match.captured(2).isEmpty()) {
                start = match.captured(2).toInt();
                end = match.captured(4).isEmpty() ? start : match.captured(4).toInt();
                if (start < 1 || end < 1) return setError("Verse numbers must be greater than zero.");
                if (!match.captured(3).isEmpty() && match.captured(3).toInt() != c)
                    return setError("Select a verse range within one chapter.");
            } else if (EpubImporter::canon()[b-1].chapters == 1 && c != 1) {
                if (c < 1) return setError("Verse numbers must be greater than zero.");
                start = end = c; c = 1;
            }
            matched = true;
            break;
        }
    }
    // Single-chapter book shorthand, for example Jude 5–7.
    if (!matched) {
        static const QRegularExpression single("^(.+?)\\s+(\\d+)\\s*-\\s*(\\d+)$");
        const auto match = single.match(input);
        b = EpubImporter::bookId(match.captured(1));
        if (match.hasMatch() && b && EpubImporter::canon()[b-1].chapters == 1) {
            c = 1; start = match.captured(2).toInt(); end = match.captured(3).toInt(); matched = true;
            if (start < 1 || end < 1) return setError("Verse numbers must be greater than zero.");
        }
    }
    if (!matched) return setError("Reference not recognized. Try John 3, Jn 3:16–18, or 1 Cor 13.");
    if (!validRange(b,c,start,end)) {
        if (b >= 1 && b <= 66 && start == end && EpubImporter::omittedVerse(b,c,start))
            return setError(reference(b,c,start,end) + " is omitted in the NET Bible 2.1 text.");
        return setError("That chapter or verse range is not present in NET Bible 2.1.");
    }
    return goTo(b,c,start,end);
}
void BibleBackend::openChapter(int b, int c) { goTo(b,c,0,0); }
void BibleBackend::previousChapter()
{
    if (!requireReady()) return;
    if (m_chapter > 1) openChapter(m_book,m_chapter-1);
    else if (m_book > 1) openChapter(m_book-1,EpubImporter::canon()[m_book-2].chapters);
}
void BibleBackend::nextChapter()
{
    if (!requireReady()) return;
    if (m_chapter < chapterCount()) openChapter(m_book,m_chapter+1);
    else if (m_book < 66) openChapter(m_book+1,1);
}
void BibleBackend::selectVerses(int start, int end)
{
    if (!requireReady()) return;
    const int low = std::min(start,end), high = std::max(start,end);
    if (!validRange(m_book,m_chapter,low,high) || low == 0) { setError("Choose existing verses within this chapter."); return; }
    if (!persistState(m_book,m_chapter,low,high,m_position)) return;
    m_start = low; m_end = high; m_anchor = start; m_cursor = end;
    updateSelection(); clearError(); emit selectionChanged();
}
void BibleBackend::clearSelection()
{
    if (!m_start) return;
    if (!persistState(m_book,m_chapter,0,0,m_position)) return;
    m_start = m_end = m_anchor = m_cursor = 0;
    updateSelection(); emit selectionChanged();
}
void BibleBackend::moveSelection(int delta, bool extend)
{
    if (!requireReady() || m_verses.isEmpty() || !delta) return;
    QVector<int> numbers;
    for (const auto &row : m_verses) numbers.append(row.toMap().value("number").toInt());
    if (!m_start) {
        const int v = delta < 0 ? numbers.last() : numbers.first();
        selectVerses(v,v);
    } else {
        int index = numbers.indexOf(m_cursor);
        const qint64 next = std::clamp(qint64(index) + delta, qint64(0), qint64(numbers.size()-1));
        const int v = numbers[int(next)], anchor = extend ? m_anchor : v;
        selectVerses(anchor,v);
    }
    emit navigationChanged();
}
void BibleBackend::setReadingPosition(double position)
{
    if (!std::isfinite(position)) { setError("Reading position must be a finite number."); return; }
    position = std::clamp(position,0.0,1.0);
    if (!m_ready || position == m_position) return;
    if (!persistState(m_book,m_chapter,m_start,m_end,position)) return;
    m_position = position; emit readingPositionChanged();
}

bool BibleBackend::loadAnnotations(QVector<Annotation> *result)
{
    QSqlQuery q(m_store);
    if (!q.exec("SELECT book,chapter,start,end,bookmark,highlight,note FROM annotations ORDER BY book,chapter,start,end"))
        return setError("Cannot read your annotations: " + q.lastError().text());
    QVector<Annotation> rows;
    while (q.next()) rows.append({q.value(0).toInt(),q.value(1).toInt(),q.value(2).toInt(),q.value(3).toInt(),
                                  q.value(4).toBool(),q.value(5).toBool(),q.value(6).toString()});
    if (q.lastError().isValid()) return setError("Cannot read your annotations: " + q.lastError().text());
    *result = rows;
    return true;
}

void BibleBackend::updateSelection()
{
    QString note;
    bool bookmark = m_start != 0, highlight = m_start != 0;
    for (const auto &row : m_verses) {
        const auto map = row.toMap();
        const int v = map.value("number").toInt();
        if (v >= m_start && v <= m_end) {
            bookmark = bookmark && map.value("bookmarked").toBool();
            highlight = highlight && map.value("highlighted").toBool();
        }
    }
    for (const auto &a : m_annotations)
        if (a.book == m_book && a.chapter == m_chapter && a.start == m_start && a.end == m_end) note = a.note;
    if (note != m_note) { m_note = note; emit noteTextChanged(); }
    m_bookmarked = bookmark; m_highlighted = highlight;
    emit annotationChanged();
}
void BibleBackend::updateAnnotations()
{
    for (auto &row : m_verses) {
        auto map = row.toMap();
        const int v = map.value("number").toInt();
        bool bookmark = false, highlight = false, note = false;
        for (const auto &a : m_annotations) {
            if (a.book == m_book && a.chapter == m_chapter && v >= a.start && v <= a.end) {
                bookmark |= a.bookmark; highlight |= a.highlight; note |= !a.note.isEmpty();
            }
        }
        map["bookmarked"] = bookmark; map["highlighted"] = highlight; map["hasNote"] = note;
        row = map;
    }
    emit versesChanged(); updateSelection();
}
void BibleBackend::refreshCollection()
{
    QVector<Annotation> rows;
    if (!loadAnnotations(&rows)) return;
    m_annotations = rows;
    m_collection = collectionRows(rows);
    emit collectionChanged(); updateAnnotations();
}
QVariantList BibleBackend::collectionRows(const QVector<Annotation> &rows) const
{
    QVariantList collection;
    for (const auto &a : rows) collection.append(QVariantMap{{"book",a.book},{"chapter",a.chapter},
        {"start",a.start},{"end",a.end},{"reference",reference(a.book,a.chapter,a.start,a.end)},
        {"bookmark",a.bookmark},{"highlight",a.highlight},{"note",a.note}});
    return collection;
}

bool BibleBackend::saveAnnotations(const QVector<Annotation> &annotations)
{
    // Called inside an existing transaction. Coalesce identical ranges so that
    // flags and the exact-range note share a single collection entry.
    QMap<QString,Annotation> merged;
    for (const auto &a : annotations) {
        if (!a.bookmark && !a.highlight && a.note.isEmpty()) continue;
        const QString key = QString("%1:%2:%3:%4").arg(a.book).arg(a.chapter).arg(a.start).arg(a.end);
        if (!merged.contains(key)) merged.insert(key,a);
        else {
            auto &target = merged[key];
            target.bookmark |= a.bookmark; target.highlight |= a.highlight;
            if (!a.note.isEmpty()) target.note = a.note;
        }
    }
    QSqlQuery q(m_store);
    if (!q.exec("DELETE FROM annotations")) return setError("Cannot save annotations: " + q.lastError().text());
    if (!q.prepare("INSERT INTO annotations VALUES(?,?,?,?,?,?,?)")) return setError(q.lastError().text());
    for (const auto &a : merged) {
        q.bindValue(0,a.book); q.bindValue(1,a.chapter); q.bindValue(2,a.start); q.bindValue(3,a.end);
        q.bindValue(4,a.bookmark); q.bindValue(5,a.highlight); q.bindValue(6,a.note.isNull() ? QString("") : a.note);
        if (!q.exec()) return setError("Cannot save annotations: " + q.lastError().text());
    }
    return true;
}

void BibleBackend::toggleFlag(bool highlight)
{
    if (!requireReady()) return;
    if (!m_start) { setError("Select one or more verses first."); return; }
    if (!m_store.transaction()) { setError("Cannot start annotation save: " + m_store.lastError().text()); return; }
    QVector<Annotation> rows;
    if (!loadAnnotations(&rows)) { m_store.rollback(); return; }
    bool covered = true;
    for (const auto &row : m_verses) {
        const int v = row.toMap().value("number").toInt();
        if (v < m_start || v > m_end) continue;
        bool found = false;
        for (const auto &a : rows)
            if (a.book == m_book && a.chapter == m_chapter && a.start <= v && a.end >= v && (highlight ? a.highlight : a.bookmark)) found = true;
        covered &= found;
    }
    if (covered) {
        QVector<Annotation> pieces;
        for (auto &a : rows) {
            bool &flag = highlight ? a.highlight : a.bookmark;
            if (!flag || a.book != m_book || a.chapter != m_chapter || a.end < m_start || a.start > m_end) continue;
            flag = false;
            auto piece = [&](int start, int end) {
                while (start <= end && !verseExists(m_book,m_chapter,start)) ++start;
                while (end >= start && !verseExists(m_book,m_chapter,end)) --end;
                if (start <= end) pieces.append({m_book,m_chapter,start,end,!highlight,highlight,{}});
            };
            if (a.start < m_start) piece(a.start,m_start-1);
            if (a.end > m_end) piece(m_end+1,a.end);
        }
        rows += pieces;
    } else rows.append({m_book,m_chapter,m_start,m_end,!highlight,highlight,{}});
    if (!saveAnnotations(rows)) { m_store.rollback(); return; }
    if (!m_store.commit()) { m_store.rollback(); setError("Cannot commit annotations: " + m_store.lastError().text()); return; }
    clearError(); refreshCollection();
}
void BibleBackend::toggleBookmark() { toggleFlag(false); }
void BibleBackend::toggleHighlight() { toggleFlag(true); }

void BibleBackend::setNoteText(QString text)
{
    if (text == m_note) return;
    if (!requireReady()) return;
    if (!m_start) { setError("Select the verse or range for this note first."); return; }
    if (text.size() > MaximumNoteSize || !safeString(text)) { setError("The note is too large or contains unsupported control characters."); return; }
    if (!m_store.transaction()) { setError("Cannot start note save: " + m_store.lastError().text()); return; }
    QVector<Annotation> rows;
    if (!loadAnnotations(&rows)) { m_store.rollback(); return; }
    bool found = false;
    for (auto &a : rows) {
        if (a.book == m_book && a.chapter == m_chapter && a.start == m_start && a.end == m_end) {
            a.note = text; found = true; break;
        }
    }
    if (!found && !text.isEmpty()) rows.append({m_book,m_chapter,m_start,m_end,false,false,text});
    if (!saveAnnotations(rows)) { m_store.rollback(); return; }
    if (!m_store.commit()) { m_store.rollback(); setError("Cannot save note: " + m_store.lastError().text()); return; }
    clearError(); refreshCollection();
}

QString BibleBackend::copySelection()
{
    if (!requireReady()) return {};
    QStringList lines;
    lines << selectionReference() + " (NET 2.1)";
    for (const auto &row : m_verses) {
        const auto map = row.toMap(); const int v = map.value("number").toInt();
        if (!m_start || (v >= m_start && v <= m_end)) lines << QString::number(v) + ' ' + map.value("text").toString();
    }
    const QString text = lines.join('\n');
    if (qobject_cast<QGuiApplication *>(QCoreApplication::instance())) QGuiApplication::clipboard()->setText(text);
    clearError(); return text;
}

bool BibleBackend::exportNotes(QUrl directory)
{
    if (!m_store.isOpen()) return setError("Your notes database is unavailable.");
    const QString path = localPath(directory);
    if (path.isEmpty()) return false;
    if (!QFileInfo(path).isDir()) return setError("Choose an existing directory for the Markdown export.");
    if (!protectDestination(QDir(path).filePath("omabible-notes.md"))) return false;
    QVector<Annotation> rows;
    if (!loadAnnotations(&rows)) return false;
    QString markdown = "# Omabible notes\n\nEdition: NET Bible 2.1\n\n";
    for (const auto &a : rows) {
        markdown += "## " + reference(a.book,a.chapter,a.start,a.end) + "\n\n";
        QStringList flags;
        if (a.bookmark) flags << "Bookmark";
        if (a.highlight) flags << "Highlight";
        if (!flags.isEmpty()) markdown += "*" + flags.join(" · ") + "*\n\n";
        if (!a.note.isEmpty()) markdown += a.note + "\n\n";
    }
    QString error;
    if (!writeAtomic(QDir(path).filePath("omabible-notes.md"),markdown.toUtf8(),&error))
        return setError("Cannot export Markdown notes: " + error);
    clearError(); setStatus("Notes exported to " + QDir(path).filePath("omabible-notes.md")); return true;
}

bool BibleBackend::protectDestination(const QString &path)
{
    // Resolve dangling symlinks too: a sidecar may not exist until the next SQL
    // transaction. A fixed Markdown filename must obey the same rules as JSON.
    QString destination = path;
    QSet<QString> seen;
    for (int hops = 0; QFileInfo(destination).isSymLink(); ++hops) {
        if (hops >= 40 || seen.contains(destination)) return setError("The export destination contains a symlink loop.");
        seen.insert(destination);
        destination = QFileInfo(destination).symLinkTarget();
    }
    const QFileInfo target(destination);
    const QString resolved = target.exists() ? target.canonicalFilePath()
        : QFileInfo(target.absolutePath()).canonicalFilePath() + '/' + target.fileName();
    const QFileInfo resolvedTarget(resolved);
    if (resolvedTarget.absolutePath() == QFileInfo(m_directory).canonicalFilePath()
        && (resolvedTarget.fileName().startsWith("personal.sqlite") || resolvedTarget.fileName().startsWith("bible.sqlite")))
        return setError("Choose an export destination outside the live database files and sidecars.");
    return true;
}

bool BibleBackend::exportBackup(QUrl file)
{
    if (!m_store.isOpen()) return setError("Your notes database is unavailable.");
    const QString path = localPath(file);
    if (path.isEmpty() || !protectDestination(path)) return false;
    if (!m_store.transaction()) return setError("Cannot read a consistent backup: " + m_store.lastError().text());
    QVector<Annotation> rows;
    if (!loadAnnotations(&rows)) { m_store.rollback(); return false; }
    QSqlQuery q(m_store);
    if (!q.exec("SELECT book,chapter,start,end,position FROM reading WHERE id=1") || !q.next()) {
        m_store.rollback(); return setError("Cannot read your reading position for backup.");
    }
    const QJsonObject reading{{"book",q.value(0).toInt()},{"chapter",q.value(1).toInt()},
        {"start",q.value(2).toInt()},{"end",q.value(3).toInt()},{"position",q.value(4).toDouble()}};
    q.finish();
    if (!m_store.commit()) { m_store.rollback(); return setError("Cannot finish reading the backup: " + m_store.lastError().text()); }
    QJsonArray annotations;
    for (const auto &a : rows) annotations.append(QJsonObject{{"book",a.book},{"chapter",a.chapter},
        {"start",a.start},{"end",a.end},{"bookmark",a.bookmark},{"highlight",a.highlight},{"note",a.note}});
    const QJsonObject root{{"format","omabible-backup"},{"version",1},{"edition",edition()},
        {"created",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}, {"reading",reading},{"annotations",annotations}};
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (bytes.size() > MaximumBackupSize) return setError("This backup exceeds the supported 32 MiB size. Export Markdown notes instead.");
    QString error;
    if (!writeAtomic(path,bytes,&error)) return setError("Cannot export backup: " + error);
    clearError(); setStatus("Backup exported to " + path); return true;
}

bool BibleBackend::importBackup(QUrl file)
{
    if (!requireReady()) return false;
    if (m_busy) return setError("Wait for the EPUB import to finish before restoring annotations.");
    const QString path = localPath(file);
    if (path.isEmpty()) return false;
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly)) return setError("Cannot read backup: " + source.errorString());
    if (source.size() > MaximumBackupSize) return setError("The backup exceeds the supported 32 MiB size.");
    const QByteArray bytes = source.read(MaximumBackupSize + 1);
    if (source.error() != QFileDevice::NoError || bytes.size() > MaximumBackupSize) return setError("Cannot read the complete backup.");
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes,&parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return setError("Invalid backup JSON: " + parseError.errorString());
    if (!uniqueJsonKeys(bytes)) return setError("The backup contains duplicate JSON fields.");
    const auto root = document.object();
    if (!exactKeys(root,{"format","version","edition","created","reading","annotations"})
        || root.value("format") != "omabible-backup" || !integer(root.value("version"),1,1)
        || root.value("edition") != edition() || !root.value("created").isString()
        || !QDateTime::fromString(root.value("created").toString(),Qt::ISODateWithMs).isValid()
        || !root.value("reading").isObject() || !root.value("annotations").isArray())
        return setError("Unsupported or invalid Omabible backup format.");
    const auto reading = root.value("reading").toObject();
    auto rangeValid = [this](const QJsonObject &object, bool allowNone) {
        if (!integer(object.value("book"),1,66) || !integer(object.value("chapter"),1,150)
            || !integer(object.value("start"),allowNone ? 0 : 1,176) || !integer(object.value("end"),allowNone ? 0 : 1,176)) return false;
        return validRange(object.value("book").toInt(),object.value("chapter").toInt(),object.value("start").toInt(),object.value("end").toInt());
    };
    if (!exactKeys(reading,{"book","chapter","start","end","position"}) || !rangeValid(reading,true)
        || !reading.value("position").isDouble() || !std::isfinite(reading.value("position").toDouble())
        || reading.value("position").toDouble() < 0 || reading.value("position").toDouble() > 1)
        return setError("The backup has an invalid reading location.");
    const auto array = root.value("annotations").toArray();
    if (array.size() > 100000) return setError("The backup has too many annotations.");
    QVector<Annotation> rows;
    QSet<QString> ranges;
    for (const auto &value : array) {
        if (!value.isObject()) return setError("The backup contains an invalid annotation.");
        const auto object = value.toObject();
        if (!exactKeys(object,{"book","chapter","start","end","bookmark","highlight","note"}) || !rangeValid(object,false)
            || !object.value("bookmark").isBool() || !object.value("highlight").isBool() || !object.value("note").isString())
            return setError("The backup contains an invalid annotation reference or field type.");
        Annotation a{object.value("book").toInt(),object.value("chapter").toInt(),object.value("start").toInt(),
            object.value("end").toInt(),object.value("bookmark").toBool(),object.value("highlight").toBool(),object.value("note").toString()};
        if (a.note.size() > MaximumNoteSize || !safeString(a.note) || (!a.bookmark && !a.highlight && a.note.isEmpty()))
            return setError("The backup contains an empty, oversized, or invalid annotation.");
        const QString key = reference(a.book,a.chapter,a.start,a.end);
        if (ranges.contains(key)) return setError("The backup contains duplicate annotation ranges.");
        ranges.insert(key); rows.append(a);
    }
    // Restore is replacement, not a lossy merge. Validation precedes all writes;
    // notes, flags and reading state commit together or not at all.
    if (!m_store.transaction()) return setError("Cannot start backup restore: " + m_store.lastError().text());
    const int b = reading.value("book").toInt(), c = reading.value("chapter").toInt();
    const int start = reading.value("start").toInt(), end = reading.value("end").toInt();
    const double position = reading.value("position").toDouble();
    QVariantList chapterRows;
    QVector<Annotation> savedRows;
    // All fallible reads happen before commit. Read back inside the transaction
    // so triggers/SQL errors cannot leave published state differing from storage.
    if (!readChapter(b,c,&chapterRows) || !saveAnnotations(rows) || !persistState(b,c,start,end,position)
        || !loadAnnotations(&savedRows)) { m_store.rollback(); return false; }
    const QVariantList savedCollection = collectionRows(savedRows);
    if (!m_store.commit()) { m_store.rollback(); return setError("Cannot commit backup restore: " + m_store.lastError().text()); }
    m_book = b; m_chapter = c; m_start = start; m_end = end; m_anchor = start; m_cursor = end; m_position = position;
    m_verses = chapterRows; m_annotations = savedRows; m_collection = savedCollection;
    updateAnnotations(); emit collectionChanged();
    emit passageChanged(); emit selectionChanged(); emit readingPositionChanged(); emit navigationChanged();
    clearError(); setStatus(QString("Restored %1 annotations from backup.").arg(rows.size())); return true;
}
