#include "biblebackend.h"
#include "brand.h"

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
#include <tuple>

namespace {
constexpr qsizetype MaximumNoteSize = 1024 * 1024;
constexpr qint64 MaximumBackupSize = 32 * 1024 * 1024;
struct Verse { int book, chapter, verse; };
// Index zero means no selection. Only published NET verses occupy positions,
// so stepping and range subtraction automatically skip omitted verse numbers.
const QVector<Verse> &canonVerses()
{
    static const QVector<Verse> verses = [] {
        QVector<Verse> result{{0,0,0}};
        for (int b = 1; b <= 66; ++b)
            for (int c = 1; c <= EpubImporter::canon()[b-1].chapters; ++c)
                for (int v = 1; v <= EpubImporter::chapterMaximum(b,c); ++v)
                    if (!EpubImporter::omittedVerse(b,c,v)) result.append({b,c,v});
        return result;
    }();
    return verses;
}
int absoluteVerse(int b, int c, int v)
{
    const auto &verses = canonVerses();
    const auto it = std::lower_bound(verses.cbegin()+1, verses.cend(), Verse{b,c,v}, [](Verse a, Verse z) {
        return std::tie(a.book,a.chapter,a.verse) < std::tie(z.book,z.chapter,z.verse);
    });
    return it != verses.cend() && it->book == b && it->chapter == c && it->verse == v ? int(it-verses.cbegin()) : 0;
}
Verse endpoint(int index) { return canonVerses().value(index); }
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
        setError("Cannot create " + Brand::name() + " data directory: " + m_directory);
        return;
    }
    if (!initializeStore()) return;
    if (QFileInfo::exists(m_directory + "/bible.sqlite")) {
        QString error;
        if (EpubImporter::validateDatabase(m_directory + "/bible.sqlite", &error)) {
            m_ready = openBible();
            if (m_ready) {
                if (!validRange(m_book, m_chapter, m_start, m_end)) {
                    m_book = 1; m_chapter = 1; m_start = m_end = m_anchor = m_cursor = 0; m_position = 0;
                }
                intersectSelection();
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
                    m_book = 1; m_chapter = 1; m_start = m_end = m_anchor = m_cursor = 0; m_position = 0;
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
QString BibleBackend::rangeReference(int start, int end)
{
    const auto a = endpoint(start), z = endpoint(end);
    if (a.book == z.book && a.chapter == z.chapter) return reference(a.book,a.chapter,a.verse,z.verse);
    return reference(a.book,a.chapter,a.verse) + QChar(0x2013)
        + (a.book == z.book ? QString("%1:%2").arg(z.chapter).arg(z.verse) : reference(z.book,z.chapter,z.verse));
}
QString BibleBackend::selectionReference() const
{
    return m_anchor ? rangeReference(std::min(m_anchor,m_cursor),std::max(m_anchor,m_cursor)) : passage();
}
int BibleBackend::cursorVerse() const { return endpoint(m_cursor).verse; }
bool BibleBackend::crossChapterSelection() const
{
    const auto a = endpoint(m_anchor), z = endpoint(m_cursor);
    return m_anchor && (a.book != z.book || a.chapter != z.chapter);
}
void BibleBackend::intersectSelection()
{
    m_start = m_end = 0;
    if (!m_anchor) return;
    for (int v = 1; v <= EpubImporter::chapterMaximum(m_book,m_chapter); ++v) {
        const int index = absoluteVerse(m_book,m_chapter,v);
        if (index && index >= std::min(m_anchor,m_cursor) && index <= std::max(m_anchor,m_cursor)) {
            if (!m_start) m_start = v;
            m_end = v;
        }
    }
}
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
    if (version > 2) { m_store.close(); return setError("Your notes database was created by a newer " + Brand::name() + " version."); }
    if (!q.exec("PRAGMA journal_mode=WAL") || !q.exec("PRAGMA synchronous=FULL"))
        return setError("Cannot configure reliable note saving: " + q.lastError().text());
    if (!m_store.transaction()) return setError(m_store.lastError().text());
    QStringList schema;
    if (version < 2) schema = {
        "CREATE TABLE IF NOT EXISTS annotations(book INTEGER NOT NULL CHECK(book BETWEEN 1 AND 66),chapter INTEGER NOT NULL CHECK(chapter BETWEEN 1 AND 150),start INTEGER NOT NULL CHECK(start BETWEEN 1 AND 176),end INTEGER NOT NULL CHECK(end BETWEEN start AND 176),bookmark INTEGER NOT NULL CHECK(bookmark IN(0,1)),highlight INTEGER NOT NULL CHECK(highlight IN(0,1)),note TEXT NOT NULL,PRIMARY KEY(book,chapter,start,end),CHECK(bookmark OR highlight OR length(note)>0)) WITHOUT ROWID",
        "CREATE TABLE IF NOT EXISTS reading(id INTEGER PRIMARY KEY CHECK(id=1),book INTEGER NOT NULL,chapter INTEGER NOT NULL,start INTEGER NOT NULL,end INTEGER NOT NULL,position REAL NOT NULL CHECK(position BETWEEN 0 AND 1))",
        "INSERT OR IGNORE INTO reading(id,book,chapter,start,end,position) VALUES(1,1,1,0,0,0)",
        "ALTER TABLE annotations RENAME TO annotations_v1",
        "CREATE TABLE annotations(book INTEGER NOT NULL CHECK(book BETWEEN 1 AND 66),chapter INTEGER NOT NULL CHECK(chapter BETWEEN 1 AND 150),start INTEGER NOT NULL CHECK(start BETWEEN 1 AND 176),end INTEGER NOT NULL CHECK(end BETWEEN 1 AND 176),bookmark INTEGER NOT NULL CHECK(bookmark IN(0,1)),highlight INTEGER NOT NULL CHECK(highlight IN(0,1)),note TEXT NOT NULL,endBook INTEGER NOT NULL CHECK(endBook BETWEEN 1 AND 66),endChapter INTEGER NOT NULL CHECK(endChapter BETWEEN 1 AND 150),PRIMARY KEY(book,chapter,start,endBook,endChapter,end),CHECK((book,chapter,start)<=(endBook,endChapter,end)),CHECK(bookmark OR highlight OR length(note)>0)) WITHOUT ROWID",
        "INSERT INTO annotations SELECT book,chapter,start,end,bookmark,highlight,note,book,chapter FROM annotations_v1",
        "DROP TABLE annotations_v1",
        "ALTER TABLE reading ADD COLUMN anchorBook INTEGER NOT NULL DEFAULT 1",
        "ALTER TABLE reading ADD COLUMN anchorChapter INTEGER NOT NULL DEFAULT 1",
        "ALTER TABLE reading ADD COLUMN cursorBook INTEGER NOT NULL DEFAULT 1",
        "ALTER TABLE reading ADD COLUMN cursorChapter INTEGER NOT NULL DEFAULT 1",
        "UPDATE reading SET anchorBook=book,anchorChapter=chapter,cursorBook=book,cursorChapter=chapter",
        "PRAGMA user_version=2"
    };
    // Additive to schema 2: annotation backups intentionally exclude preferences.
    schema << "CREATE TABLE IF NOT EXISTS preferences(key TEXT PRIMARY KEY,value NOT NULL) WITHOUT ROWID"
           << "INSERT OR IGNORE INTO preferences(key,value) VALUES('showVerseNumbers',1),('showHeadings',1),('versePicker',1),('lineHeight',1.5),('focusMode',0),('blockMode',0)";
    for (const auto &sql : schema) {
        if (!q.exec(sql)) { m_store.rollback(); return setError("Cannot initialize notes: " + q.lastError().text()); }
    }
    if (!m_store.commit()) { m_store.rollback(); return setError("Cannot save notes database schema: " + m_store.lastError().text()); }
    if (!q.exec("SELECT book,chapter,start,end,position,anchorBook,anchorChapter,cursorBook,cursorChapter FROM reading WHERE id=1") || !q.next())
        return setError("Cannot read your saved reading location: " + q.lastError().text());
    m_book = q.value(0).toInt(); m_chapter = q.value(1).toInt(); m_start = q.value(2).toInt();
    m_end = q.value(3).toInt(); m_position = q.value(4).toDouble();
    m_anchor = absoluteVerse(q.value(5).toInt(),q.value(6).toInt(),m_start);
    m_cursor = absoluteVerse(q.value(7).toInt(),q.value(8).toInt(),m_end);
    if (!m_anchor || !m_cursor || endpoint(m_cursor).book != m_book || endpoint(m_cursor).chapter != m_chapter)
        m_anchor = m_cursor = 0;
    if (m_book < 1 || m_book > 66 || m_chapter < 1 || m_chapter > EpubImporter::canon()[m_book-1].chapters
        || !std::isfinite(m_position) || m_position < 0 || m_position > 1) {
        m_book = 1; m_chapter = 1; m_start = m_end = m_anchor = m_cursor = 0; m_position = 0;
    }
    intersectSelection();
    return loadPreferences() && loadAnnotations(&m_annotations);
}

bool BibleBackend::loadPreferences()
{
    QSqlQuery q(m_store);
    if (!q.exec("SELECT key,value FROM preferences")) return setError("Cannot read your preferences: " + q.lastError().text());
    QVariantMap values;
    while (q.next()) values.insert(q.value(0).toString(),q.value(1));
    if (q.lastError().isValid()) return setError("Cannot read your preferences: " + q.lastError().text());
    auto numeric = [](const QVariant &value) {
        return value.metaType().id() == QMetaType::LongLong || value.metaType().id() == QMetaType::Int
            || value.metaType().id() == QMetaType::Double;
    };
    auto boolean = [&](const QString &key, bool fallback) {
        const auto value = values.value(key);
        return numeric(value) && (value.toDouble() == 0 || value.toDouble() == 1) ? value.toBool() : fallback;
    };
    m_showVerseNumbers = boolean("showVerseNumbers",true);
    m_showHeadings = boolean("showHeadings",true);
    m_versePicker = boolean("versePicker",true);
    m_focusMode = boolean("focusMode",false);
    m_blockMode = boolean("blockMode",false);
    const auto value = values.value("lineHeight");
    const double height = value.toDouble();
    m_lineHeight = numeric(value) && std::isfinite(height) && height >= 1 && height <= 3
        && std::abs(height * 10 - std::round(height * 10)) < 1e-9 ? std::round(height * 10) / 10 : 1.5;
    return true;
}

bool BibleBackend::savePreference(const QString &key, const QVariant &value)
{
    if (!m_store.transaction()) return setError("Cannot start preference save: " + m_store.lastError().text());
    QSqlQuery q(m_store);
    q.prepare("INSERT INTO preferences(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    q.addBindValue(key); q.addBindValue(value);
    if (!q.exec()) { m_store.rollback(); return setError("Cannot save your preferences: " + q.lastError().text()); }
    q.prepare("SELECT value FROM preferences WHERE key=?"); q.addBindValue(key);
    if (!q.exec() || !q.next() || q.value(0) != value) {
        m_store.rollback(); return setError("Cannot verify saved preferences: " + q.lastError().text());
    }
    q.finish();
    if (!m_store.commit()) { m_store.rollback(); return setError("Cannot commit your preferences: " + m_store.lastError().text()); }
    clearError(); return true;
}

void BibleBackend::setShowVerseNumbers(bool value)
{
    if (value == m_showVerseNumbers || !savePreference("showVerseNumbers",int(value))) return;
    m_showVerseNumbers = value; emit preferencesChanged();
}
void BibleBackend::setShowHeadings(bool value)
{
    if (value == m_showHeadings || !savePreference("showHeadings",int(value))) return;
    m_showHeadings = value; emit preferencesChanged();
}
void BibleBackend::setVersePicker(bool value)
{
    if (value == m_versePicker || !savePreference("versePicker",int(value))) return;
    m_versePicker = value; emit preferencesChanged();
}
void BibleBackend::setFocusMode(bool value)
{
    if (value == m_focusMode || !savePreference("focusMode",int(value))) return;
    m_focusMode = value; emit preferencesChanged();
}
void BibleBackend::setBlockMode(bool value)
{
    if (value == m_blockMode || !savePreference("blockMode",int(value))) return;
    m_blockMode = value; emit preferencesChanged();
}
void BibleBackend::setLineHeight(double value)
{
    if (!std::isfinite(value)) { setError("Line height must be a finite number."); return; }
    value = std::round(std::clamp(value,1.0,3.0) * 10) / 10;
    if (value == m_lineHeight || !savePreference("lineHeight",value)) return;
    m_lineHeight = value; emit preferencesChanged();
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
QVariantList BibleBackend::chapterVerses(int book, int chapter) const
{
    if (!m_ready || book < 1 || book > 66 || chapter < 1 || chapter > EpubImporter::canon()[book-1].chapters) return {};
    QSqlQuery q(m_bible);
    q.prepare("SELECT number,text FROM verses WHERE book=? AND chapter=? ORDER BY number");
    q.addBindValue(book); q.addBindValue(chapter);
    if (!q.exec()) return {};
    QVariantList rows;
    while (q.next()) rows.append(QVariantMap{{"number",q.value(0)},{"text",q.value(1)}});
    return q.lastError().isValid() ? QVariantList() : rows;
}
bool BibleBackend::persistState(int b, int c, int start, int end, double position)
{
    const auto a = endpoint(start), z = endpoint(end);
    QSqlQuery q(m_store);
    q.prepare("UPDATE reading SET book=?,chapter=?,start=?,end=?,position=?,anchorBook=?,anchorChapter=?,cursorBook=?,cursorChapter=? WHERE id=1");
    q.addBindValue(b); q.addBindValue(c); q.addBindValue(a.verse); q.addBindValue(z.verse); q.addBindValue(position);
    q.addBindValue(a.book); q.addBindValue(a.chapter); q.addBindValue(z.book); q.addBindValue(z.chapter);
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
    // Display the active endpoint, matching subsequent cursor movement.
    return goToSelection(b,c,absoluteVerse(b,c,start),absoluteVerse(b,c,end),
                         b != m_book || c != m_chapter || start ? 0 : m_position);
}
bool BibleBackend::goToSelection(int b, int c, int anchor, int cursor, double position)
{
    QVariantList rows;
    if (!readChapter(b,c,&rows)) return false;
    if (!persistState(b,c,anchor,cursor,position)) return false;
    m_book = b; m_chapter = c;
    m_anchor = anchor; m_cursor = cursor; m_position = position;
    intersectSelection();
    m_verses = rows;
    updateAnnotations();
    clearError();
    emit passageChanged(); emit selectionChanged(); emit readingPositionChanged(); emit navigationChanged();
    return true;
}

bool BibleBackend::navigate(QString input)
{
    int b, c, start, end;
    QString error;
    if (!parseReference(input,&b,&c,&start,&end,&error)) return setError(error);
    return goToSelection(b,c,start,end,start || b != m_book || c != m_chapter ? 0 : m_position);
}
QVariantMap BibleBackend::resolveReference(QString input) const
{
    int b, c, start, end;
    QString error;
    const bool valid = parseReference(input,&b,&c,&start,&end,&error);
    return {{"valid",valid},{"reference",valid ? (start ? rangeReference(start,end) : reference(b,c)) : QString()}, {"error",error}};
}
bool BibleBackend::parseReference(QString input, int *book, int *chapter, int *first, int *last, QString *error) const
{
    auto fail = [error](const QString &message) { *error = message; return false; };
    if (!m_ready) return fail("Import your NET Bible 2.1 EPUB first.");
    input = input.simplified();
    input.replace(QChar(0x2013), '-'); input.replace(QChar(0x2014), '-'); input.replace(QChar(0x2212), '-');
    if (input.size() > 200 || input.isEmpty()) return fail("Enter a reference such as John 3:16–18.");
    // A fully qualified right endpoint enables collection references spanning books.
    const int dash = input.indexOf('-');
    if (dash >= 0) {
        const QString right = input.mid(dash+1).trimmed();
        static const QRegularExpression qualified("[A-Za-z]");
        if (right.contains(qualified)) {
            int lb, lc, ls, le, rb, rc, rs, re;
            if (!parseReference(input.left(dash),&lb,&lc,&ls,&le,error)
                || !parseReference(right,&rb,&rc,&rs,&re,error)) return false;
            if (!ls || !rs || ls != le || rs != re || ls > rs) return fail("Choose an ordered verse range.");
            *book = rb; *chapter = rc; *first = ls; *last = rs; return true;
        }
    }
    // Try every split: this handles compact names (1Jn3:16), numbered books,
    // punctuation in aliases, and full multi-word names without fuzzy matches.
    static const QRegularExpression suffix("^(\\d+)(?:\\s*:\\s*(\\d+)(?:\\s*-\\s*(?:(\\d+)\\s*:\\s*)?(\\d+))?)?$");
    int b = EpubImporter::bookId(input), c = 1, endChapter = -1, start = 0, end = 0;
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
                if (start < 1 || end < 1) return fail("Verse numbers must be greater than zero.");
                if (!match.captured(3).isEmpty()) endChapter = match.captured(3).toInt();
                else endChapter = c;
            } else if (EpubImporter::canon()[b-1].chapters == 1 && c != 1) {
                if (c < 1) return fail("Verse numbers must be greater than zero.");
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
            if (start < 1 || end < 1) return fail("Verse numbers must be greater than zero.");
        }
    }
    if (!matched) return fail("Reference not recognized. Try John 3, Jn 3:16–18, or 1 Cor 13.");
    if (endChapter < 0) endChapter = c;
    const int a = absoluteVerse(b,c,start), z = absoluteVerse(b,endChapter,end);
    if (!validRange(b,c,start,start) || !validRange(b,endChapter,end,end) || (start && (!a || !z || a > z))) {
        if (b >= 1 && b <= 66 && start == end && EpubImporter::omittedVerse(b,c,start))
            return fail(reference(b,c,start,end) + " is omitted in the NET Bible 2.1 text.");
        return fail("That chapter or verse range is not present in NET Bible 2.1.");
    }
    *book = b; *chapter = endChapter; *first = a; *last = z;
    return true;
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
    const int anchor = absoluteVerse(m_book,m_chapter,start), cursor = absoluteVerse(m_book,m_chapter,end);
    if (!persistState(m_book,m_chapter,anchor,cursor,m_position)) return;
    m_start = low; m_end = high; m_anchor = anchor; m_cursor = cursor;
    updateSelection(); clearError(); emit selectionChanged();
}
void BibleBackend::clearSelection()
{
    if (!m_anchor) return;
    if (!persistState(m_book,m_chapter,0,0,m_position)) return;
    m_start = m_end = m_anchor = m_cursor = 0;
    updateSelection(); clearError(); emit selectionChanged();
}
void BibleBackend::moveSelection(int delta, bool extend)
{
    if (!requireReady() || m_verses.isEmpty() || !delta) return;
    // Without a cursor, the first step selects the chapter edge; remaining
    // steps count existing canonical verses, crossing chapters as usual.
    const int edge = absoluteVerse(m_book,m_chapter,
        (delta < 0 ? m_verses.last() : m_verses.first()).toMap().value("number").toInt());
    const qint64 target = m_cursor ? qint64(m_cursor) + delta
        : qint64(edge) + delta - (delta > 0 ? 1 : -1);
    const int cursor = int(std::clamp(target,qint64(1),qint64(canonVerses().size()-1)));
    const auto v = endpoint(cursor);
    goToSelection(v.book,v.chapter,extend && m_anchor ? m_anchor : cursor,cursor,
                  v.book != m_book || v.chapter != m_chapter ? 0 : m_position);
}
void BibleBackend::setReadingPosition(double position)
{
    if (!std::isfinite(position)) { setError("Reading position must be a finite number."); return; }
    position = std::clamp(position,0.0,1.0);
    if (!m_ready || position == m_position) return;
    if (!persistState(m_book,m_chapter,m_anchor,m_cursor,position)) return;
    m_position = position; emit readingPositionChanged();
}

void BibleBackend::moveByVerses(int delta, bool extend)
{
    if (!requireReady() || m_verses.isEmpty() || !delta) return;
    const int first = absoluteVerse(m_book,m_chapter,m_verses.first().toMap().value("number").toInt());
    const int last = absoluteVerse(m_book,m_chapter,m_verses.last().toMap().value("number").toInt());
    const int start = m_cursor ? m_cursor : first;
    qint64 target = qint64(start) + delta;
    if (delta > 1 || delta < -1) {
        if (m_cursor && ((delta > 0 && start == last) || (delta < 0 && start == first)))
            target = qint64(start) + (delta > 0 ? 1 : -1);
        else target = std::clamp(target,qint64(first),qint64(last));
    }
    const int cursor = int(std::clamp(target,qint64(1),qint64(canonVerses().size()-1)));
    const auto v = endpoint(cursor);
    goToSelection(v.book,v.chapter,extend ? (m_anchor ? m_anchor : first) : cursor,cursor,
                  v.book != m_book || v.chapter != m_chapter ? 0 : m_position);
}

bool BibleBackend::loadAnnotations(QVector<Annotation> *result)
{
    QSqlQuery q(m_store);
    if (!q.exec("SELECT book,chapter,start,end,bookmark,highlight,note,endBook,endChapter FROM annotations ORDER BY book,chapter,start,endBook,endChapter,end"))
        return setError("Cannot read your annotations: " + q.lastError().text());
    QVector<Annotation> rows;
    while (q.next()) {
        const int start = absoluteVerse(q.value(0).toInt(),q.value(1).toInt(),q.value(2).toInt());
        const int end = absoluteVerse(q.value(7).toInt(),q.value(8).toInt(),q.value(3).toInt());
        if (!start || end < start) return setError("Cannot read your annotations: invalid range endpoints.");
        rows.append({start,end,q.value(4).toBool(),q.value(5).toBool(),q.value(6).toString()});
    }
    if (q.lastError().isValid()) return setError("Cannot read your annotations: " + q.lastError().text());
    *result = rows;
    return true;
}

void BibleBackend::updateSelection()
{
    QString note;
    for (const auto &a : m_annotations)
        if (a.start == std::min(m_anchor,m_cursor) && a.end == std::max(m_anchor,m_cursor)) note = a.note;
    if (note != m_note) { m_note = note; emit noteTextChanged(); }
    m_bookmarked = flagCovered(m_annotations,false); m_highlighted = flagCovered(m_annotations,true);
    emit annotationChanged();
}
bool BibleBackend::flagCovered(const QVector<Annotation> &annotations, bool highlight) const
{
    if (!m_anchor) return false;
    QVector<QPair<int,int>> ranges;
    for (const auto &a : annotations)
        if (highlight ? a.highlight : a.bookmark) ranges.append({a.start,a.end});
    std::sort(ranges.begin(),ranges.end());
    int next = std::min(m_anchor,m_cursor);
    const int end = std::max(m_anchor,m_cursor);
    for (const auto &range : ranges) {
        if (range.first > next) break;
        next = std::max(next,range.second+1);
        if (next > end) return true;
    }
    return false;
}
void BibleBackend::updateAnnotations()
{
    for (auto &row : m_verses) {
        auto map = row.toMap();
        const int v = absoluteVerse(m_book,m_chapter,map.value("number").toInt());
        bool bookmark = false, highlight = false, note = false;
        bool start = false, end = false, before = false, after = false;
        bool highlightStart = false, highlightEnd = false, highlightBefore = false, highlightAfter = false;
        QVariantList ranges, highlightRanges;
        for (const auto &a : m_annotations) {
            if (v >= a.start && v <= a.end) {
                bookmark |= a.bookmark; highlight |= a.highlight; note |= !a.note.isEmpty();
                if (a.bookmark) {
                    start |= v == a.start; end |= v == a.end; before |= v > a.start; after |= v < a.end;
                    ranges.append(QVariantMap{{"reference",rangeReference(a.start,a.end)},
                        {"bookmarkStart",v == a.start},{"bookmarkEnd",v == a.end},
                        {"bookmarkContinuesBefore",v > a.start},{"bookmarkContinuesAfter",v < a.end}});
                }
                if (a.highlight) {
                    highlightStart |= v == a.start; highlightEnd |= v == a.end;
                    highlightBefore |= v > a.start; highlightAfter |= v < a.end;
                    highlightRanges.append(QVariantMap{{"reference",rangeReference(a.start,a.end)},
                        {"highlightStart",v == a.start},{"highlightEnd",v == a.end},
                        {"highlightContinuesBefore",v > a.start},{"highlightContinuesAfter",v < a.end}});
                }
            }
        }
        map["bookmarked"] = bookmark; map["highlighted"] = highlight; map["hasNote"] = note;
        map["bookmarkStart"] = start; map["bookmarkEnd"] = end;
        map["bookmarkContinuesBefore"] = before; map["bookmarkContinuesAfter"] = after;
        map["bookmarkRanges"] = ranges;
        map["highlightStart"] = highlightStart; map["highlightEnd"] = highlightEnd;
        map["highlightContinuesBefore"] = highlightBefore; map["highlightContinuesAfter"] = highlightAfter;
        map["highlightRanges"] = highlightRanges;
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
    for (const auto &a : rows) {
        const auto start = endpoint(a.start), end = endpoint(a.end);
        collection.append(QVariantMap{{"book",start.book},{"chapter",start.chapter},
            {"start",start.verse},{"end",end.verse},{"endBook",end.book},{"endChapter",end.chapter},
            {"reference",rangeReference(a.start,a.end)},
            {"bookmark",a.bookmark},{"highlight",a.highlight},{"note",a.note}});
    }
    return collection;
}

bool BibleBackend::saveAnnotations(const QVector<Annotation> &annotations)
{
    // Called inside an existing transaction. Coalesce identical ranges so that
    // flags and the exact-range note share a single collection entry.
    QMap<QString,Annotation> merged;
    for (const auto &a : annotations) {
        if (!a.bookmark && !a.highlight && a.note.isEmpty()) continue;
        const QString key = QString("%1:%2").arg(a.start).arg(a.end);
        if (!merged.contains(key)) merged.insert(key,a);
        else {
            auto &target = merged[key];
            target.bookmark |= a.bookmark; target.highlight |= a.highlight;
            if (!a.note.isEmpty()) target.note = a.note;
        }
    }
    QSqlQuery q(m_store);
    if (!q.exec("DELETE FROM annotations")) return setError("Cannot save annotations: " + q.lastError().text());
    if (!q.prepare("INSERT INTO annotations(book,chapter,start,end,bookmark,highlight,note,endBook,endChapter) VALUES(?,?,?,?,?,?,?,?,?)")) return setError(q.lastError().text());
    for (const auto &a : merged) {
        const auto start = endpoint(a.start), end = endpoint(a.end);
        q.bindValue(0,start.book); q.bindValue(1,start.chapter); q.bindValue(2,start.verse); q.bindValue(3,end.verse);
        q.bindValue(4,a.bookmark); q.bindValue(5,a.highlight); q.bindValue(6,a.note.isNull() ? QString("") : a.note);
        q.bindValue(7,end.book); q.bindValue(8,end.chapter);
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
    const bool covered = flagCovered(rows,highlight);
    const int start = std::min(m_anchor,m_cursor), end = std::max(m_anchor,m_cursor);
    if (covered) {
        QVector<Annotation> pieces;
        for (auto &a : rows) {
            bool &flag = highlight ? a.highlight : a.bookmark;
            if (!flag || a.end < start || a.start > end) continue;
            flag = false;
            if (a.start < start) pieces.append({a.start,start-1,!highlight,highlight,{}});
            if (a.end > end) pieces.append({end+1,a.end,!highlight,highlight,{}});
        }
        rows += pieces;
    } else rows.append({start,end,!highlight,highlight,{}});
    commitAnnotations(rows);
}
bool BibleBackend::commitAnnotations(const QVector<Annotation> &rows)
{
    QVector<Annotation> saved;
    if (!saveAnnotations(rows) || !loadAnnotations(&saved)) { m_store.rollback(); return false; }
    const auto collection = collectionRows(saved);
    if (!m_store.commit()) { m_store.rollback(); return setError("Cannot commit annotations: " + m_store.lastError().text()); }
    m_annotations = saved; m_collection = collection;
    clearError(); emit collectionChanged(); updateAnnotations(); return true;
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
        if (a.start == std::min(m_anchor,m_cursor) && a.end == std::max(m_anchor,m_cursor)) {
            a.note = text; found = true; break;
        }
    }
    if (!found && !text.isEmpty()) rows.append({std::min(m_anchor,m_cursor),std::max(m_anchor,m_cursor),false,false,text});
    commitAnnotations(rows);
}

bool BibleBackend::deleteAnnotation(QString input)
{
    int b, c, first, last;
    QString error;
    if (!parseReference(input,&b,&c,&first,&last,&error)) return setError(error);
    if (!first) return setError("Choose an exact saved verse range to delete.");
    if (!m_store.transaction()) return setError("Cannot start annotation deletion: " + m_store.lastError().text());
    const auto a = endpoint(first), z = endpoint(last);
    QSqlQuery q(m_store);
    q.prepare("DELETE FROM annotations WHERE book=? AND chapter=? AND start=? AND endBook=? AND endChapter=? AND end=?");
    q.addBindValue(a.book); q.addBindValue(a.chapter); q.addBindValue(a.verse);
    q.addBindValue(z.book); q.addBindValue(z.chapter); q.addBindValue(z.verse);
    if (!q.exec()) { m_store.rollback(); return setError("Cannot delete annotation: " + q.lastError().text()); }
    if (q.numRowsAffected() != 1) { m_store.rollback(); return setError("No saved annotation exists for that exact range."); }
    q.finish();
    QVector<Annotation> saved;
    if (!loadAnnotations(&saved)) { m_store.rollback(); return false; }
    for (const auto &row : saved) {
        if (row.start == first && row.end == last) {
            m_store.rollback(); return setError("Cannot verify annotation deletion.");
        }
    }
    if (!m_store.commit()) { m_store.rollback(); return setError("Cannot commit annotation deletion: " + m_store.lastError().text()); }
    m_annotations = saved; m_collection = collectionRows(saved);
    clearError(); emit collectionChanged(); updateAnnotations(); return true;
}

QString BibleBackend::copySelection(bool plainText)
{
    if (!requireReady()) return {};
    const int first = m_anchor ? std::min(m_anchor,m_cursor) : absoluteVerse(m_book,m_chapter,m_verses.first().toMap().value("number").toInt());
    const int last = m_anchor ? std::max(m_anchor,m_cursor) : absoluteVerse(m_book,m_chapter,m_verses.last().toMap().value("number").toInt());
    const QString text = copyRange(first,last,selectionReference(),plainText);
    if (text.isEmpty()) return {};
    copyText(text); clearError(); return text;
}

QString BibleBackend::copyVerse(int number, bool plainText)
{
    if (!requireReady()) return {};
    const int index = absoluteVerse(m_book,m_chapter,number);
    if (!index) { setError("Choose an existing verse within this chapter."); return {}; }
    const bool selected = m_anchor && index >= std::min(m_anchor,m_cursor) && index <= std::max(m_anchor,m_cursor);
    const int first = selected ? std::min(m_anchor,m_cursor) : index;
    const int last = selected ? std::max(m_anchor,m_cursor) : index;
    const QString text = copyRange(first,last,rangeReference(first,last),plainText);
    if (text.isEmpty()) return {};
    // Read the entire old range before changing focus, and save focus before publishing the copy.
    if (!persistState(m_book,m_chapter,index,index,m_position)) return {};
    m_anchor = m_cursor = index; m_start = m_end = number;
    updateSelection(); emit selectionChanged();
    copyText(text); clearError(); return text;
}

QString BibleBackend::copyRange(int first, int last, const QString &label, bool plainText)
{
    QStringList lines;
    const QString digits = QStringLiteral("⁰¹²³⁴⁵⁶⁷⁸⁹");
    int b = 0, c = 0;
    QVariantList rows;
    for (int i = first; i <= last; ++i) {
        const auto v = endpoint(i);
        if (v.book != b || v.chapter != c) {
            b = v.book; c = v.chapter;
            if (!readChapter(b,c,&rows)) return {};
            for (const auto &row : rows) {
                const auto map = row.toMap(); const int number = map.value("number").toInt();
                const int index = absoluteVerse(b,c,number);
                if (index >= first && index <= last) {
                    const QString text = map.value("text").toString().simplified();
                    QString superscript;
                    for (const QChar digit : QString::number(number)) superscript += digits[digit.digitValue()];
                    lines << (plainText ? text : superscript + ' ' + text);
                }
            }
        }
    }
    return plainText ? lines.join(' ') : QStringLiteral("“") + lines.join(' ') + QStringLiteral("”\n\n—") + label;
}

void BibleBackend::copyText(QString text)
{
    if (qobject_cast<QGuiApplication *>(QCoreApplication::instance())) QGuiApplication::clipboard()->setText(text);
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
    // The export filename remains stable for existing scripts and consumers.
    QString markdown = "# " + Brand::name() + " notes\n\nEdition: NET Bible 2.1\n\n";
    for (const auto &a : rows) {
        markdown += "## " + rangeReference(a.start,a.end) + "\n\n";
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
    if (!q.exec("SELECT book,chapter,start,end,position,anchorBook,anchorChapter,cursorBook,cursorChapter FROM reading WHERE id=1") || !q.next()) {
        m_store.rollback(); return setError("Cannot read your reading position for backup.");
    }
    const QJsonObject reading{{"book",q.value(0).toInt()},{"chapter",q.value(1).toInt()},
        {"start",q.value(2).toInt()},{"end",q.value(3).toInt()},{"position",q.value(4).toDouble()},
        {"anchorBook",q.value(5).toInt()},{"anchorChapter",q.value(6).toInt()},
        {"cursorBook",q.value(7).toInt()},{"cursorChapter",q.value(8).toInt()}};
    q.finish();
    if (!m_store.commit()) { m_store.rollback(); return setError("Cannot finish reading the backup: " + m_store.lastError().text()); }
    QJsonArray annotations;
    for (const auto &a : rows) {
        const auto start = endpoint(a.start), end = endpoint(a.end);
        annotations.append(QJsonObject{{"book",start.book},{"chapter",start.chapter},
            {"start",start.verse},{"end",end.verse},{"endBook",end.book},{"endChapter",end.chapter},
            {"bookmark",a.bookmark},{"highlight",a.highlight},{"note",a.note}});
    }
    const QJsonObject root{{"format","omabible-backup"},{"version",2},{"edition",edition()},
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
        || root.value("format") != "omabible-backup" || !integer(root.value("version"),1,2)
        || root.value("edition") != edition() || !root.value("created").isString()
        || !QDateTime::fromString(root.value("created").toString(),Qt::ISODateWithMs).isValid()
        || !root.value("reading").isObject() || !root.value("annotations").isArray())
        return setError("Unsupported or invalid " + Brand::name() + " backup format.");
    const auto reading = root.value("reading").toObject();
    const bool v2 = root.value("version").toInt() == 2;
    auto verse = [this](const QJsonObject &object, const QString &b, const QString &c, const QString &v) {
        if (!integer(object.value(b),1,66) || !integer(object.value(c),1,150) || !integer(object.value(v),1,176)) return 0;
        const int book = object.value(b).toInt(), chapter = object.value(c).toInt(), number = object.value(v).toInt();
        return verseExists(book,chapter,number) ? absoluteVerse(book,chapter,number) : 0;
    };
    QStringList readingKeys{"book","chapter","start","end","position"};
    if (v2) readingKeys << "anchorBook" << "anchorChapter" << "cursorBook" << "cursorChapter";
    const int b = reading.value("book").toInt(), c = reading.value("chapter").toInt();
    const bool none = integer(reading.value("start"),0,0) && integer(reading.value("end"),0,0);
    const int anchor = verse(reading,v2 ? "anchorBook" : "book",v2 ? "anchorChapter" : "chapter","start");
    const int cursor = verse(reading,v2 ? "cursorBook" : "book",v2 ? "cursorChapter" : "chapter","end");
    bool endpointsValid = none || (anchor && cursor && endpoint(cursor).book == b && endpoint(cursor).chapter == c && (v2 || anchor <= cursor));
    if (v2 && none) {
        // Migrated v1 chapter-only state may retain its chapter in these fields.
        for (const QString &key : {QString("anchorBook"),QString("anchorChapter"),QString("cursorBook"),QString("cursorChapter")})
            endpointsValid &= integer(reading.value(key),0,key.endsWith("Book") ? 66 : 150);
    }
    if (!exactKeys(reading,readingKeys) || !integer(reading.value("book"),1,66) || !integer(reading.value("chapter"),1,150)
        || !validRange(b,c,0,0) || !endpointsValid
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
        QStringList keys{"book","chapter","start","end","bookmark","highlight","note"};
        if (v2) keys << "endBook" << "endChapter";
        const int start = verse(object,"book","chapter","start");
        const int end = verse(object,v2 ? "endBook" : "book",v2 ? "endChapter" : "chapter","end");
        if (!exactKeys(object,keys) || !start || end < start
            || !object.value("bookmark").isBool() || !object.value("highlight").isBool() || !object.value("note").isString())
            return setError("The backup contains an invalid annotation reference or field type.");
        Annotation a{start,end,object.value("bookmark").toBool(),object.value("highlight").toBool(),object.value("note").toString()};
        if (a.note.size() > MaximumNoteSize || !safeString(a.note) || (!a.bookmark && !a.highlight && a.note.isEmpty()))
            return setError("The backup contains an empty, oversized, or invalid annotation.");
        const QString key = rangeReference(a.start,a.end);
        if (ranges.contains(key)) return setError("The backup contains duplicate annotation ranges.");
        ranges.insert(key); rows.append(a);
    }
    // Restore is replacement, not a lossy merge. Validation precedes all writes;
    // notes, flags and reading state commit together or not at all.
    if (!m_store.transaction()) return setError("Cannot start backup restore: " + m_store.lastError().text());
    const double position = reading.value("position").toDouble();
    QVariantList chapterRows;
    QVector<Annotation> savedRows;
    // All fallible reads happen before commit. Read back inside the transaction
    // so triggers/SQL errors cannot leave published state differing from storage.
    if (!readChapter(b,c,&chapterRows) || !saveAnnotations(rows) || !persistState(b,c,anchor,cursor,position)
        || !loadAnnotations(&savedRows)) { m_store.rollback(); return false; }
    const QVariantList savedCollection = collectionRows(savedRows);
    if (!m_store.commit()) { m_store.rollback(); return setError("Cannot commit backup restore: " + m_store.lastError().text()); }
    m_book = b; m_chapter = c; m_anchor = anchor; m_cursor = cursor; m_position = position;
    intersectSelection();
    m_verses = chapterRows; m_annotations = savedRows; m_collection = savedCollection;
    updateAnnotations(); emit collectionChanged();
    emit passageChanged(); emit selectionChanged(); emit readingPositionChanged(); emit navigationChanged();
    clearError(); setStatus(QString("Restored %1 annotations from backup.").arg(rows.size())); return true;
}
