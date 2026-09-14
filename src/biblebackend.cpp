#include "biblebackend.h"
#include "brand.h"
#include "biblebooks.h"

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
#include <QScopeGuard>
#include <QSignalBlocker>
#include <QPointer>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <cmath>
#include <tuple>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>

namespace {
// All readers and commits live on the GUI thread, under the primary's process lock.
QList<BibleBackend *> readers;
constexpr qsizetype MaximumNoteSize = 1024 * 1024;
constexpr qint64 MaximumBackupSize = 32 * 1024 * 1024;
QString connectionName() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
int verseKey(int book, int chapter, int verse) { return (book * 1000 + chapter) * 1000 + verse; }
QString aliasKey(QString name) {
    name = name.toCaseFolded();
    name.remove(QRegularExpression("[\\s.]+"));
    return name;
}
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
bool bibleIdentity(const QString &path, const QString &id)
{
    const QString name = connectionName();
    bool matches = false;
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE",name);
        db.setDatabaseName(path); db.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=5000");
        if (db.open()) {
            QSqlQuery q(db);
            if (q.exec("SELECT key,value FROM metadata")) {
                QVariantMap metadata;
                while (q.next()) metadata.insert(q.value(0).toString(),q.value(1));
                QString actual = metadata["version_id"].toString();
                matches = !q.lastError().isValid() && metadata["source_format"] == "usfm" && actual == id;
            }
        }
    }
    QSqlDatabase::removeDatabase(name);
    return matches;
}

const QStringList scopeFields{"book","chapter","start","endBook","endChapter","end"};
QString normalizedTag(QString name)
{
    // Reject controls before trimming, including edge tabs/newlines. Format
    // characters (e.g. emoji joiners) are legitimate parts of a name.
    if (name.size() > 4096 || !safeString(name)) return {};
    for (const auto c : name)
        if (c.category() == QChar::Other_Control || c.category() == QChar::Separator_Line
            || c.category() == QChar::Separator_Paragraph) return {};
    name = name.trimmed().normalized(QString::NormalizationForm_C);
    return name.size() <= 4096 ? name : QString();
}
QString tagKey(const QString &name)
{
    // Qt provides simple (one-codepoint) folding. Canonical decomposition
    // handles accented/Greek expansions; these remaining full-fold mappings
    // are not decomposable (or have compatibility-only decompositions).
    const QString simple = name.normalized(QString::NormalizationForm_D).toCaseFolded();
    static const QMap<ushort,QString> expansions{
        {0x00df,QStringLiteral("ss")},{0x0149,QStringLiteral("\u02bcn")},
        {0x0587,QStringLiteral("\u0565\u0582")},{0x1e9a,QStringLiteral("a\u02be")},
        {0xfb00,QStringLiteral("ff")},{0xfb01,QStringLiteral("fi")},{0xfb02,QStringLiteral("fl")},
        {0xfb03,QStringLiteral("ffi")},{0xfb04,QStringLiteral("ffl")},
        {0xfb05,QStringLiteral("st")},{0xfb06,QStringLiteral("st")},
        {0xfb13,QStringLiteral("\u0574\u0576")},{0xfb14,QStringLiteral("\u0574\u0565")},
        {0xfb15,QStringLiteral("\u0574\u056b")},{0xfb16,QStringLiteral("\u057e\u0576")},
        {0xfb17,QStringLiteral("\u0574\u056d")}};
    QString folded;
    for (const auto c : simple) {
        const auto it = expansions.constFind(c.unicode());
        if (it == expansions.cend()) folded += c;
        else folded += it.value();
    }
    return folded.normalized(QString::NormalizationForm_C);
}
QString rangeKey(const QVariantMap &scope)
{
    QStringList parts{scope.value("versionId").toString()};
    for (const auto &key : scopeFields) parts << QString::number(scope.value(key).toInt());
    return parts.join(':');
}
void bindScope(QSqlQuery &q, const QVariantMap &scope)
{
    q.addBindValue(scope.value("versionId"));
    for (const auto &key : scopeFields) q.addBindValue(scope.value(key));
}
const QString scopeWhere = "version_id=? AND book=? AND chapter=? AND start=? AND endBook=? AND endChapter=? AND end=?";
// Query only requested endpoint keys and bounded text, never validate/read an
// entire Bible for the tag browser. Connections cannot outlive their queries.
struct TagBible {
    QString connection = connectionName();
    QSqlDatabase db;
    TagBible(const QString &path, const QString &id) {
        db = QSqlDatabase::addDatabase("QSQLITE",connection);
        db.setDatabaseName(path); db.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=5000");
        if (path.isEmpty() || !db.open()) return;
        QSqlQuery q(db);
        q.prepare("SELECT value FROM metadata WHERE key='version_id'");
        if (!q.exec() || !q.next() || q.value(0).toString() != id) db.close();
    }
    ~TagBible() { db.close(); db = {}; QSqlDatabase::removeDatabase(connection); }
    bool scope(const QVariantMap &input, QVariantMap *result) const {
        if (!db.isOpen() || input.value("versionId").metaType().id() != QMetaType::QString
            || input.value("versionId").toString().isEmpty()) return false;
        for (const auto &key : scopeFields) {
            const auto value = QJsonValue::fromVariant(input.value(key));
            if (!integer(value,1,key.endsWith("Book") || key == "book" ? 80 : 999)) return false;
        }
        QVariantMap out{{"versionId",input.value("versionId")}};
        for (const auto &key : scopeFields) out[key] = input.value(key).toInt();
        QStringList names;
        for (int side = 0; side < 2; ++side) {
            QSqlQuery q(db);
            q.prepare("SELECT b.ordinal,b.name FROM verses v JOIN books b ON b.id=v.book WHERE v.book=? AND v.chapter=? AND v.number=?");
            for (int i = 0; i < 3; ++i) q.addBindValue(out.value(scopeFields[side*3+i]));
            if (!q.exec() || !q.next()) return false;
            const qint64 order = q.value(0).toLongLong()*1000000 + out.value(scopeFields[side*3+1]).toInt()*1000 + out.value(scopeFields[side*3+2]).toInt();
            out[side ? "_endOrder" : "_order"] = order;
            names << q.value(1).toString();
        }
        if (out.value("_order").toLongLong() > out.value("_endOrder").toLongLong()) return false;
        QString ref = QString("%1 %2:%3").arg(names[0]).arg(out["chapter"].toInt()).arg(out["start"].toInt());
        if (out["_order"] != out["_endOrder"]) {
            ref += QChar(0x2013);
            if (out["book"] != out["endBook"]) ref += names[1] + ' ';
            if (out["book"] != out["endBook"] || out["chapter"] != out["endChapter"])
                ref += QString::number(out["endChapter"].toInt()) + ':';
            ref += QString::number(out["end"].toInt());
        }
        out["reference"] = ref;
        *result = out; return true;
    }
    QString excerpt(const QVariantMap &scope) const {
        QSqlQuery q(db);
        q.prepare("SELECT substr(v.text,1,512) FROM verses v JOIN books b ON b.id=v.book "
                  "WHERE (b.ordinal,v.chapter,v.number)>=(?,?,?) AND (b.ordinal,v.chapter,v.number)<=(?,?,?) "
                  "ORDER BY b.ordinal,v.chapter,v.number LIMIT 8");
        q.addBindValue(scope.value("_order").toLongLong()/1000000);
        q.addBindValue(scope.value("chapter")); q.addBindValue(scope.value("start"));
        q.addBindValue(scope.value("_endOrder").toLongLong()/1000000);
        q.addBindValue(scope.value("endChapter")); q.addBindValue(scope.value("end"));
        if (!q.exec()) return {};
        QString text;
        while (q.next() && text.size() < 320) { if (!text.isEmpty()) text += ' '; text += q.value(0).toString().simplified(); }
        return text.size() > 320 ? text.left(319) + QChar(0x2026) : text;
    }
};
bool rangesOverlap(const QVariantMap &a, const QVariantMap &b)
{
    return a.value("versionId") == b.value("versionId")
        && a.value("_order").toLongLong() <= b.value("_endOrder").toLongLong()
        && b.value("_order").toLongLong() <= a.value("_endOrder").toLongLong();
}
QVariantMap publicRange(QVariantMap map) { map.remove("_order"); map.remove("_endOrder"); return map; }
QString markdownText(const QString &text)
{
    QString out;
    for (const auto c : text) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else { if (QStringLiteral("\\`*_{}[]()#+-.!|~").contains(c)) out += '\\'; out += c; }
    }
    return out;
}
}

int BibleBackend::absoluteVerse(int b, int c, int v) const
{
    if (b < 1 || b > 80 || c < 1 || c > 999 || v < 1 || v > 999) return 0;
    return m_keyIndices.value(verseKey(b,c,v));
}

BibleBackend::BibleBackend(QObject *parent) : QObject(parent)
{
    connect(this, &BibleBackend::selectionChanged, this, &BibleBackend::tagsChanged);
    m_directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    for (auto *peer : readers) {
        if (peer->m_directory != m_directory) continue;
        connect(this, &BibleBackend::sharedCommitted, peer, &BibleBackend::refreshShared, Qt::DirectConnection);
        connect(peer, &BibleBackend::sharedCommitted, this, &BibleBackend::refreshShared, Qt::DirectConnection);
    }
    readers.append(this);
    // Completion wiring must exist even when startup text or storage is damaged.
    m_progressTimer.setInterval(100);
    connect(&m_progressTimer, &QTimer::timeout, this, [this] {
        const double value = m_importProgress ? m_importProgress->load() : 0;
        if (value != m_progress) { m_progress = value; emit importProgressChanged(); }
    });
    connect(&m_importWatcher, &QFutureWatcher<UsfmImporter::Result>::finished, this, [this] {
        m_progressTimer.stop();
        const auto result = m_importWatcher.result();
        if (result.ok) {
            if (flushDrafts() && changeVersion(result.edition.id,m_importTarget)) m_progress = 1;
            else removeOwnedBibleFiles(m_importTarget);
        } else {
            removeOwnedBibleFiles(m_importTarget);
            setError(result.error);
            setStatus(m_ready ? "Import failed. Your existing Bible is available." : "Import failed. Choose a USFM ZIP or local USFM file.");
        }
        m_importTarget.clear();
        m_busy = false;
        emit importProgressChanged(); notifyBusy();
    });
    m_directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!QDir().mkpath(m_directory)) {
        setError("Cannot create " + Brand::name() + " data directory: " + m_directory);
        return;
    }
    if (!initializeStore()) { m_store.close(); return; }
    m_storeReady = true;
    if (!readVersions(&m_versions)) return;
    QSqlQuery active(m_store);
    if (!active.exec("SELECT active FROM library WHERE id=1") || !active.next()) {
        setError("Cannot read the active Bible version."); return;
    }
    const QString id = active.value(0).toString(); active.finish();
    if (!id.isEmpty()) changeVersion(id,{}, {},false,true);
    if (!m_ready) setStatus("Import a USFM Bible to begin.");
}

BibleBackend::~BibleBackend()
{
    readers.removeAll(this);
    disconnect(this, nullptr, nullptr, nullptr);
    // The worker captures only paths and shared progress, never this QObject.
    // Waiting ensures a normal application exit cannot leave a half-finished import.
    m_importWatcher.waitForFinished();
    // A completed worker whose finished signal was never delivered is uninstalled.
    if (!m_importTarget.isEmpty()) removeOwnedBibleFiles(m_importTarget);
    closeBible();
    m_store.close(); m_store = QSqlDatabase();
    if (!m_storeConnection.isEmpty()) QSqlDatabase::removeDatabase(m_storeConnection);
    if (m_busy) notifyBusy();
}

bool BibleBackend::busy() const
{
    for (auto *peer : readers)
        if (peer->m_directory == m_directory && peer->m_busy) return true;
    return false;
}

void BibleBackend::notifyBusy()
{
    for (auto *peer : readers)
        if (peer->m_directory == m_directory) emit peer->busyChanged();
}

bool BibleBackend::sharedMutationBusy() const
{
    for (auto *peer : readers)
        if (peer->m_directory == m_directory && (peer->m_busy || peer->m_mutating || peer->m_flushing)) return true;
    return false;
}

bool BibleBackend::flushDrafts()
{
    m_flushing = true;
    const auto guard = qScopeGuard([this] {
        m_flushing = false;
        const auto pending = std::exchange(m_pendingAnnotations, {});
        for (const auto &id : pending) {
            refreshShared(id, false, false);
            emit sharedCommitted(id, false, false);
        }
    });
    QList<QPointer<BibleBackend>> snapshot;
    for (auto *peer : readers) if (peer->m_directory == m_directory) snapshot.append(peer);
    for (const auto &peer : snapshot) {
        if (peer && peer->m_flush && !peer->m_flush())
            return setError("An unsaved note in a reader window could not be saved. The operation was cancelled.");
    }
    return true;
}

void BibleBackend::publishAnnotations()
{
    // Flushing A must not replace B's editor contents before B gets its turn.
    // Writes still read the latest rows inside their own transaction.
    for (auto *peer : readers) {
        if (peer->m_directory == m_directory && peer->m_flushing) {
            peer->m_pendingAnnotations.insert(m_versionId);
            return;
        }
    }
    emit sharedCommitted(m_versionId, false, false);
}

void BibleBackend::refreshShared(const QString &id, bool library, bool preferences)
{
    if (!m_storeReady) return;
    if (preferences && loadPreferences()) emit preferencesChanged();
    if (library) {
        if (!readVersions(&m_versions)) return;
        QString next = m_versionId;
        if (next.isEmpty() || installedPath(next).isEmpty()) {
            next = m_versions.isEmpty() ? QString() : m_versions.first().toMap()["id"].toString();
        }
        if (next != m_versionId || (!id.isEmpty() && id == m_versionId)) {
            if (!changeVersion(next, {}, {}, false, false, true)) {
                // Never retain an obsolete generation or writable annotation scope.
                closeBible(); m_ready = false; m_versionId.clear();
                m_edition.clear(); m_abbreviation.clear(); m_copyright.clear(); m_hasParagraphs = false;
                m_keys = {{0,0,0}}; m_keyIndices.clear(); m_books.clear(); m_chapters.clear(); m_bookAliases.clear();
                m_anchor = m_cursor = m_start = m_end = 0;
                m_verses.clear(); m_annotations.clear(); m_collection.clear();
                updateAnnotations(); emit readyChanged(); emit versionChanged(); emit booksChanged(); emit passageChanged();
                emit collectionChanged(); emit selectionChanged();
            }
        }
        emit versionsChanged();
    } else if (!id.isEmpty() && id == m_versionId) refreshCollection();
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

QVariantMap BibleBackend::tagScope() const
{
    if (!m_ready || !m_anchor || !m_cursor) return {};
    const auto a = endpoint(std::min(m_anchor,m_cursor)), z = endpoint(std::max(m_anchor,m_cursor));
    return {{"versionId",m_versionId},{"reference",selectionReference()},
        {"book",a.book},{"chapter",a.chapter},{"start",a.verse},
        {"endBook",z.book},{"endChapter",z.chapter},{"end",z.verse}};
}

bool BibleBackend::validateScope(const QVariantMap &scope, QVariantMap *canonical) const
{
    if (!m_storeReady) return false;
    const QString id = scope.value("versionId").toString(), path = installedPath(id);
    if (id.isEmpty() || path.isEmpty()) return false;
    TagBible bible(path,id);
    QVariantMap result;
    if (!bible.scope(scope,&result)) return false;
    if (canonical) *canonical = result;
    return true;
}

QVariantList BibleBackend::tags() const
{
    if (!m_storeReady) return {};
    QSqlQuery q(m_store);
    if (!q.exec("SELECT t.id,t.name,COUNT(i.version_id) FROM tags t LEFT JOIN tag_assignments a ON a.tag_id=t.id "
                "LEFT JOIN installed i ON i.version_id=a.version_id GROUP BY t.id ORDER BY t.name_key,t.id")) return {};
    QVariantList rows;
    while (q.next()) rows.append(QVariantMap{{"id",q.value(0)},{"name",q.value(1)},{"count",q.value(2).toInt()}});
    return q.lastError().isValid() ? QVariantList() : rows;
}

QVariantList BibleBackend::tagRangeRows(const QString &id, bool excerpts) const
{
    if (!m_storeReady || id.isEmpty()) return {};
    TagBible bible(installedPath(id),id);
    if (!bible.db.isOpen()) return {};
    QSqlQuery q(m_store);
    q.prepare("SELECT a.book,a.chapter,a.start,a.endBook,a.endChapter,a.end,t.id,t.name,i.abbreviation,"
              "COALESCE(n.bookmark,0),COALESCE(n.highlight,0),COALESCE(n.note,'') "
              "FROM tag_assignments a JOIN tags t ON t.id=a.tag_id JOIN installed i ON i.version_id=a.version_id "
              "LEFT JOIN annotations n ON n.version_id=a.version_id AND n.book=a.book AND n.chapter=a.chapter "
              "AND n.start=a.start AND n.endBook=a.endBook AND n.endChapter=a.endChapter AND n.end=a.end "
              "WHERE a.version_id=? ORDER BY t.name_key,t.id");
    q.addBindValue(id);
    if (!q.exec()) return {};
    QMap<QString,QVariantMap> ranges;
    while (q.next()) {
        QVariantMap scope{{"versionId",id}};
        for (int i = 0; i < scopeFields.size(); ++i) scope[scopeFields[i]] = q.value(i);
        const QString key = rangeKey(scope);
        if (!ranges.contains(key)) {
            QVariantMap row;
            if (!bible.scope(scope,&row)) continue;
            row["versionAbbreviation"] = q.value(8);
            row["bookmark"] = q.value(9).toBool(); row["highlight"] = q.value(10).toBool(); row["note"] = q.value(11);
            row["sharedCount"] = 0;
            row["excerpt"] = excerpts ? bible.excerpt(row) : QString();
            ranges.insert(key,row);
        }
        auto &row = ranges[key];
        auto assigned = row.value("tags").toList();
        assigned.append(QVariantMap{{"id",q.value(6)},{"name",q.value(7)}});
        row["tags"] = assigned;
    }
    if (q.lastError().isValid()) return {};
    QVariantList rows;
    for (const auto &row : ranges) rows.append(row);
    std::sort(rows.begin(),rows.end(),[](const QVariant &a, const QVariant &b) {
        const auto x = a.toMap(), y = b.toMap();
        return std::make_pair(x.value("_order").toLongLong(),x.value("_endOrder").toLongLong())
            < std::make_pair(y.value("_order").toLongLong(),y.value("_endOrder").toLongLong());
    });
    return rows;
}

QVariantList BibleBackend::selectionTags() { return tagsForScope(tagScope()); }
QVariantList BibleBackend::overlappingTags() { return tagsForScope(tagScope(),true); }
QVariantList BibleBackend::tagsForScope(QVariantMap scope, bool overlapping)
{
    QVariantMap canonical;
    if (!validateScope(scope,&canonical)) return {};
    QVariantList result;
    QSet<QString> ids;
    for (const auto &value : tagRangeRows(canonical.value("versionId").toString(),false)) {
        const auto row = value.toMap();
        if (overlapping) {
            if (rangesOverlap(canonical,row)) result.append(publicRange(row));
        } else if (rangeKey(canonical) == rangeKey(row)) {
            for (const auto &tag : row.value("tags").toList()) ids.insert(tag.toMap().value("id").toString());
        }
    }
    if (!overlapping) for (const auto &tag : tags()) if (ids.contains(tag.toMap().value("id").toString())) result.append(tag);
    return result;
}

QString BibleBackend::ensureTag(const QString &input)
{
    const QString name = normalizedTag(input);
    if (name.isEmpty()) { setError("Enter a tag name without tabs, newlines or control characters."); return {}; }
    QSqlQuery q(m_store);
    q.prepare("SELECT id FROM tags WHERE name_key=?"); q.addBindValue(tagKey(name));
    if (!q.exec()) { setError("Cannot read tags: " + q.lastError().text()); return {}; }
    if (q.next()) return q.value(0).toString();
    const QString id = connectionName();
    q.prepare("INSERT INTO tags(id,name,name_key) VALUES(?,?,?)");
    q.addBindValue(id); q.addBindValue(name); q.addBindValue(tagKey(name));
    if (!q.exec() || q.numRowsAffected() != 1) { setError("Cannot create tag: " + q.lastError().text()); return {}; }
    q.prepare("SELECT name,name_key FROM tags WHERE id=?"); q.addBindValue(id);
    if (!q.exec() || !q.next() || q.value(0).toString() != name || q.value(1).toString() != tagKey(name)) {
        setError("Cannot verify tag creation."); return {};
    }
    return id;
}

bool BibleBackend::writeTagAssignment(const QVariantMap &scope, const QString &id, bool assigned)
{
    QSqlQuery q(m_store);
    q.prepare("SELECT id FROM tags WHERE id=?"); q.addBindValue(id);
    if (!q.exec() || !q.next()) return setError("That tag no longer exists.");
    q.finish();
    q.prepare(assigned ? "INSERT INTO tag_assignments(version_id,book,chapter,start,endBook,endChapter,end,tag_id) VALUES(?,?,?,?,?,?,?,?) ON CONFLICT DO NOTHING"
                       : "DELETE FROM tag_assignments WHERE " + scopeWhere + " AND tag_id=?");
    bindScope(q,scope); q.addBindValue(id);
    if (!q.exec()) return setError("Cannot save tag assignment: " + q.lastError().text());
    q.prepare("SELECT COUNT(*) FROM tag_assignments WHERE " + scopeWhere + " AND tag_id=?");
    bindScope(q,scope); q.addBindValue(id);
    if (!q.exec() || !q.next() || q.value(0).toInt() != int(assigned)) return setError("Cannot verify tag assignment.");
    return true;
}

void BibleBackend::publishTags()
{
    QList<QPointer<BibleBackend>> snapshot;
    for (auto *peer : readers) if (peer->m_directory == m_directory) snapshot.append(peer);
    for (const auto &peer : snapshot) if (peer) {
        peer->refreshCollection();
        emit peer->tagsChanged();
    }
}

bool BibleBackend::commitTagMutation()
{
    if (!m_store.commit()) return setError("Cannot commit tags: " + m_store.lastError().text());
    clearError(); publishTags(); return true;
}

QString BibleBackend::createTag(QString name)
{
    if (!m_storeReady || sharedMutationBusy()) { setError("The personal store is unavailable or busy."); return {}; }
    m_mutating = true;
    const auto guard = qScopeGuard([this] { m_store.rollback(); m_mutating = false; });
    if (!m_store.transaction()) { setError(m_store.lastError().text()); return {}; }
    const QString id = ensureTag(name);
    return !id.isEmpty() && commitTagMutation() ? id : QString();
}

bool BibleBackend::setTag(QVariantMap scope, QString id, bool assigned)
{
    if (!m_storeReady || sharedMutationBusy()) return setError("The personal store is unavailable or busy.");
    m_mutating = true;
    const auto guard = qScopeGuard([this] { m_store.rollback(); m_mutating = false; });
    if (!m_store.transaction()) return setError(m_store.lastError().text());
    QVariantMap canonical;
    if (!validateScope(scope,&canonical)) return setError("That Bible version or passage is no longer available.");
    return writeTagAssignment(canonical,id,assigned) && commitTagMutation();
}

QString BibleBackend::createAndAssignTag(QVariantMap scope, QString name)
{
    if (!m_storeReady || sharedMutationBusy()) { setError("The personal store is unavailable or busy."); return {}; }
    m_mutating = true;
    const auto guard = qScopeGuard([this] { m_store.rollback(); m_mutating = false; });
    if (!m_store.transaction()) { setError(m_store.lastError().text()); return {}; }
    QVariantMap canonical;
    if (!validateScope(scope,&canonical)) { setError("That Bible version or passage is no longer available."); return {}; }
    const QString id = ensureTag(name);
    return !id.isEmpty() && writeTagAssignment(canonical,id,true) && commitTagMutation() ? id : QString();
}

bool BibleBackend::renameTag(QString id, QString input)
{
    const QString name = normalizedTag(input);
    if (name.isEmpty()) return setError("Enter a tag name without tabs, newlines or control characters.");
    if (!m_storeReady || sharedMutationBusy()) return setError("The personal store is unavailable or busy.");
    m_mutating = true;
    const auto guard = qScopeGuard([this] { m_store.rollback(); m_mutating = false; });
    if (!m_store.transaction()) return setError(m_store.lastError().text());
    QSqlQuery q(m_store);
    q.prepare("UPDATE tags SET name=?,name_key=? WHERE id=?");
    q.addBindValue(name); q.addBindValue(tagKey(name)); q.addBindValue(id);
    if (!q.exec() || q.numRowsAffected() != 1) return setError("Cannot rename tag (use Merge for an existing name): " + q.lastError().text());
    q.prepare("SELECT name,name_key FROM tags WHERE id=?"); q.addBindValue(id);
    if (!q.exec() || !q.next() || q.value(0).toString() != name || q.value(1).toString() != tagKey(name)) return setError("Cannot verify tag rename.");
    q.finish();
    return commitTagMutation();
}

bool BibleBackend::deleteTag(QString id)
{
    if (!m_storeReady || sharedMutationBusy()) return setError("The personal store is unavailable or busy.");
    m_mutating = true;
    const auto guard = qScopeGuard([this] { m_store.rollback(); m_mutating = false; });
    if (!m_store.transaction()) return setError(m_store.lastError().text());
    QSqlQuery q(m_store);
    q.prepare("DELETE FROM tags WHERE id=?"); q.addBindValue(id);
    if (!q.exec() || q.numRowsAffected() != 1) return setError("Cannot delete tag: " + q.lastError().text());
    q.prepare("DELETE FROM tag_assignments WHERE tag_id=?"); q.addBindValue(id);
    if (!q.exec()) return setError("Cannot delete tag assignments: " + q.lastError().text());
    q.prepare("SELECT (SELECT COUNT(*) FROM tags WHERE id=?)+(SELECT COUNT(*) FROM tag_assignments WHERE tag_id=?)");
    q.addBindValue(id); q.addBindValue(id);
    if (!q.exec() || !q.next() || q.value(0).toInt()) return setError("Cannot verify tag deletion.");
    q.finish();
    return commitTagMutation();
}

bool BibleBackend::mergeTags(QString source, QString target)
{
    if (!m_storeReady || sharedMutationBusy()) return setError("The personal store is unavailable or busy.");
    m_mutating = true;
    const auto guard = qScopeGuard([this] { m_store.rollback(); m_mutating = false; });
    if (!m_store.transaction()) return setError(m_store.lastError().text());
    QSqlQuery q(m_store);
    q.prepare("SELECT COUNT(*) FROM tags WHERE id IN (?,?)"); q.addBindValue(source); q.addBindValue(target);
    if (!q.exec() || !q.next() || q.value(0).toInt() != (source == target ? 1 : 2)) return setError("A tag no longer exists.");
    q.finish();
    if (source == target) return commitTagMutation();
    q.prepare("INSERT INTO tag_assignments SELECT version_id,book,chapter,start,endBook,endChapter,end,? FROM tag_assignments WHERE tag_id=? ON CONFLICT DO NOTHING");
    q.addBindValue(target); q.addBindValue(source);
    if (!q.exec()) return setError("Cannot merge tags: " + q.lastError().text());
    q.prepare("SELECT COUNT(*) FROM (SELECT version_id,book,chapter,start,endBook,endChapter,end FROM tag_assignments WHERE tag_id=? EXCEPT SELECT version_id,book,chapter,start,endBook,endChapter,end FROM tag_assignments WHERE tag_id=?)");
    q.addBindValue(source); q.addBindValue(target);
    if (!q.exec() || !q.next() || q.value(0).toInt()) return setError("Cannot verify merged assignments.");
    q.finish();
    for (const auto &table : {QString("tag_assignments"),QString("tags")}) {
        const QString where = table == "tags" ? "id=?" : "tag_id=?";
        q.prepare("DELETE FROM " + table + " WHERE " + where); q.addBindValue(source);
        if (!q.exec()) return setError("Cannot finish tag merge: " + q.lastError().text());
        q.prepare("SELECT COUNT(*) FROM " + table + " WHERE " + where); q.addBindValue(source);
        if (!q.exec() || !q.next() || q.value(0).toInt()) return setError("Cannot verify tag merge.");
        q.finish();
    }
    return commitTagMutation();
}

QVariantList BibleBackend::taggedPassages(QVariantList tagIds, bool matchAll, QString version)
{
    QSet<QString> ids;
    for (const auto &id : tagIds) if (!id.toString().isEmpty()) ids.insert(id.toString());
    if (ids.isEmpty() || !m_storeReady) return {};
    QSqlQuery q(m_store);
    if (!q.exec("SELECT version_id FROM installed ORDER BY version_id")) return {};
    QStringList versions;
    while (q.next()) if (version.isEmpty() || version == q.value(0).toString()) versions << q.value(0).toString();
    if (q.lastError().isValid()) return {};
    QVariantList result;
    for (const auto &id : versions) for (const auto &value : tagRangeRows(id,true)) {
        auto row = value.toMap();
        int count = 0;
        for (const auto &tag : row.value("tags").toList()) count += ids.contains(tag.toMap().value("id").toString());
        if (!count || (matchAll && count != ids.size())) continue;
        row["sharedCount"] = count; result.append(row);
    }
    std::sort(result.begin(),result.end(),[](const QVariant &a, const QVariant &b) {
        const auto x = a.toMap(), y = b.toMap();
        return std::make_tuple(-x.value("sharedCount").toInt(),x.value("_order").toLongLong(),x.value("_endOrder").toLongLong(),x.value("versionId").toString())
            < std::make_tuple(-y.value("sharedCount").toInt(),y.value("_order").toLongLong(),y.value("_endOrder").toLongLong(),y.value("versionId").toString());
    });
    for (auto &row : result) row = publicRange(row.toMap());
    return result;
}

QVariantList BibleBackend::relatedPassages(QVariantMap source, QVariantList ids, bool matchAll, bool allVersions)
{
    QVariantMap canonical;
    if (!validateScope(source,&canonical)) return {};
    const QString version = canonical.value("versionId").toString();
    TagBible bible(installedPath(version),version);
    QVariantList result;
    for (const auto &value : taggedPassages(ids,matchAll,allVersions ? QString() : version)) {
        const auto row = value.toMap();
        QVariantMap candidate;
        if (row.value("versionId").toString() == version && bible.scope(row,&candidate) && rangesOverlap(canonical,candidate)) continue;
        result.append(row);
    }
    return result;
}

bool BibleBackend::openTaggedPassage(QVariantMap scope)
{
    if (sharedMutationBusy()) return setError("Wait for the shared library operation to finish.");
    QVariantMap canonical;
    if (!validateScope(scope,&canonical)) return setError("That Bible version or passage is no longer available.");
    m_mutating = true;
    const auto guard = qScopeGuard([this] { m_mutating = false; });
    if (!flushDrafts() || !validateScope(scope,&canonical)) return false;
    const QString id = canonical.value("versionId").toString();
    if (id != m_versionId) return changeVersion(id,{}, {},false,false,false,canonical);
    const int a = absoluteVerse(canonical.value("book").toInt(),canonical.value("chapter").toInt(),canonical.value("start").toInt());
    const int z = absoluteVerse(canonical.value("endBook").toInt(),canonical.value("endChapter").toInt(),canonical.value("end").toInt());
    if (!a || z < a) return setError("The tagged passage endpoints are no longer available.");
    return goToSelection(endpoint(z).book,endpoint(z).chapter,a,z,0);
}

QString BibleBackend::reference(int b, int c, int start, int end) const
{
    if (b < 1 || b > BibleBooks::canon().size()) return {};
    QString name = QString::fromUtf8(BibleBooks::canon()[b - 1].name);
    for (const auto &row : m_books) if (row.toMap()["id"].toInt() == b) { name = row.toMap()["name"].toString(); break; }
    QString result = QString("%1 %2").arg(name).arg(c);
    if (start) {
        result += ':' + QString::number(start);
        if (end > start) result += QChar(0x2013) + QString::number(end);
    }
    return result;
}
QString BibleBackend::passage() const { return reference(m_book, m_chapter); }
QString BibleBackend::rangeReference(int start, int end) const
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
    for (int v = 1; v <= 999; ++v) {
        const int index = absoluteVerse(m_book,m_chapter,v);
        if (index && index >= std::min(m_anchor,m_cursor) && index <= std::max(m_anchor,m_cursor)) {
            if (!m_start) m_start = v;
            m_end = v;
        }
    }
}
int BibleBackend::chapterCount() const { return chapterNumbers().size(); }
QVariantList BibleBackend::books() const { return m_books; }
QVariantList BibleBackend::chaptersForBook(int book) const { return m_chapters.value(book); }
int BibleBackend::resolveBook(QString name) const
{
    const int canonical = BibleBooks::bookId(name);
    return canonical ? canonical : m_bookAliases.value(aliasKey(name));
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
    if (version > 5) { m_store.close(); return setError("Your notes database was created by a newer " + Brand::name() + " version."); }
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
           << "INSERT OR IGNORE INTO preferences(key,value) VALUES('showVerseNumbers',1),('showHeadings',1),('versePicker',1),('lineHeight',1.5),('focusMode',0),('blockMode',0),('fontSize',18)"
           << "INSERT OR IGNORE INTO preferences(key,value) SELECT 'readingMode',CASE WHEN typeof(value) IN ('integer','real') AND value=1 THEN 'block' ELSE 'paragraph' END FROM preferences WHERE key='blockMode'";
    // Schema 3 design: one root transaction owns installation metadata, active ID,
    // version-scoped references/annotations, and global preferences. Removed IDs
    // have no installed row; their retained annotations/reading are an archive.
    // Text databases are immutable generations: import to a fresh random filename,
    // validate/read everything, then commit its registry pointer. A crash before
    // commit leaves only an unreferenced file, never a half-installed edition.
    // Removal commits registry deletion before garbage collection; a crash or
    // unlink failure likewise leaves an inaccessible orphan, not lost live data.
    // Unscoped personal references retain their archived edition identity.
    if (version < 3) schema <<
        "ALTER TABLE annotations RENAME TO annotations_v2" <<
        "CREATE TABLE annotations(book INTEGER NOT NULL CHECK(book BETWEEN 1 AND 66),chapter INTEGER NOT NULL CHECK(chapter BETWEEN 1 AND 150),start INTEGER NOT NULL CHECK(start BETWEEN 1 AND 176),end INTEGER NOT NULL CHECK(end BETWEEN 1 AND 176),bookmark INTEGER NOT NULL CHECK(bookmark IN(0,1)),highlight INTEGER NOT NULL CHECK(highlight IN(0,1)),note TEXT NOT NULL,endBook INTEGER NOT NULL CHECK(endBook BETWEEN 1 AND 66),endChapter INTEGER NOT NULL CHECK(endChapter BETWEEN 1 AND 150),version_id TEXT NOT NULL,PRIMARY KEY(version_id,book,chapter,start,endBook,endChapter,end),CHECK((book,chapter,start)<=(endBook,endChapter,end)),CHECK(bookmark OR highlight OR length(note)>0)) WITHOUT ROWID" <<
        "INSERT INTO annotations SELECT *, 'net-2.1' FROM annotations_v2" <<
        "DROP TABLE annotations_v2" <<
        "ALTER TABLE reading RENAME TO reading_v2" <<
        "CREATE TABLE reading(id INTEGER NOT NULL CHECK(id=1),book INTEGER NOT NULL,chapter INTEGER NOT NULL,start INTEGER NOT NULL,end INTEGER NOT NULL,position REAL NOT NULL CHECK(position BETWEEN 0 AND 1),anchorBook INTEGER NOT NULL,anchorChapter INTEGER NOT NULL,cursorBook INTEGER NOT NULL,cursorChapter INTEGER NOT NULL,version_id TEXT PRIMARY KEY) WITHOUT ROWID" <<
        "INSERT INTO reading SELECT *, 'net-2.1' FROM reading_v2" <<
        "DROP TABLE reading_v2" <<
        "CREATE TABLE installed(version_id TEXT PRIMARY KEY,path TEXT NOT NULL UNIQUE,abbreviation TEXT NOT NULL,name TEXT NOT NULL,copyright TEXT NOT NULL) WITHOUT ROWID" <<
        "CREATE TABLE library(id INTEGER PRIMARY KEY CHECK(id=1),active TEXT NOT NULL)" <<
        "INSERT INTO library VALUES(1,'')" <<
        "PRAGMA user_version=3";
    if (version < 4) schema <<
        "ALTER TABLE installed ADD COLUMN last_used INTEGER NOT NULL DEFAULT 0 CHECK(last_used>=0)" <<
        "ALTER TABLE library ADD COLUMN use_counter INTEGER NOT NULL DEFAULT 0 CHECK(use_counter>=0 AND typeof(use_counter)='integer')" <<
        "PRAGMA user_version=4";
    if (version < 5) schema <<
        "ALTER TABLE annotations RENAME TO annotations_v4" <<
        "CREATE TABLE annotations(book INTEGER NOT NULL CHECK(book BETWEEN 1 AND 80),chapter INTEGER NOT NULL CHECK(chapter BETWEEN 1 AND 999),start INTEGER NOT NULL CHECK(start BETWEEN 1 AND 999),end INTEGER NOT NULL CHECK(end BETWEEN 1 AND 999),bookmark INTEGER NOT NULL CHECK(bookmark IN(0,1)),highlight INTEGER NOT NULL CHECK(highlight IN(0,1)),note TEXT NOT NULL,endBook INTEGER NOT NULL CHECK(endBook BETWEEN 1 AND 80),endChapter INTEGER NOT NULL CHECK(endChapter BETWEEN 1 AND 999),version_id TEXT NOT NULL,PRIMARY KEY(version_id,book,chapter,start,endBook,endChapter,end),CHECK(bookmark OR highlight OR length(note)>0)) WITHOUT ROWID" <<
        "INSERT INTO annotations SELECT * FROM annotations_v4" <<
        "DROP TABLE annotations_v4" << "PRAGMA user_version=5";
    schema << "CREATE TABLE IF NOT EXISTS tags(id TEXT PRIMARY KEY,name TEXT NOT NULL,name_key TEXT NOT NULL UNIQUE) WITHOUT ROWID"
           << "CREATE TABLE IF NOT EXISTS tag_assignments(version_id TEXT NOT NULL,book INTEGER NOT NULL,chapter INTEGER NOT NULL,start INTEGER NOT NULL,endBook INTEGER NOT NULL,endChapter INTEGER NOT NULL,end INTEGER NOT NULL,tag_id TEXT NOT NULL,PRIMARY KEY(version_id,book,chapter,start,endBook,endChapter,end,tag_id)) WITHOUT ROWID"
           << "CREATE INDEX IF NOT EXISTS tag_assignments_tag ON tag_assignments(tag_id,version_id)";
    for (const auto &sql : schema) {
        if (!q.exec(sql)) { m_store.rollback(); return setError("Cannot initialize notes: " + q.lastError().text()); }
    }
    if (!m_store.commit()) { m_store.rollback(); return setError("Cannot save notes database schema: " + m_store.lastError().text()); }
    return loadPreferences();
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
    const QString mode = values.value("readingMode").toString();
    m_readingMode = mode == "block" || mode == "verse" ? mode : "paragraph";
    const auto size = values.value("fontSize");
    m_fontSize = numeric(size) && size.toDouble() >= 8 && size.toDouble() <= 40
        && size.toDouble() == size.toInt() ? size.toInt() : 18;
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
    emit sharedCommitted({}, false, true);
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
    setReadingMode(value ? "block" : "verse");
}
void BibleBackend::setReadingMode(QString value)
{
    if (value != "paragraph" && value != "block" && value != "verse") {
        setError("Reading mode must be paragraph, block, or verse."); return;
    }
    if (value == m_readingMode || !savePreference("readingMode",value)) return;
    m_readingMode = value; emit preferencesChanged();
}
void BibleBackend::setFontSize(int value)
{
    value = std::clamp(value,8,40);
    if (value == m_fontSize || !savePreference("fontSize",value)) return;
    m_fontSize = value; emit preferencesChanged();
}
void BibleBackend::setLineHeight(double value)
{
    if (!std::isfinite(value)) { setError("Line height must be a finite number."); return; }
    value = std::round(std::clamp(value,1.0,3.0) * 10) / 10;
    if (value == m_lineHeight || !savePreference("lineHeight",value)) return;
    m_lineHeight = value; emit preferencesChanged();
}

QString BibleBackend::installedPath(const QString &id) const
{
    QSqlQuery q(m_store);
    q.prepare("SELECT path FROM installed WHERE version_id=?"); q.addBindValue(id);
    if (!q.exec() || !q.next()) return {};
    const QString name = q.value(0).toString();
    const QString path = m_directory + '/' + name;
    return ownedBiblePath(path,id) ? path : QString();
}

bool BibleBackend::ownedBiblePath(const QString &path, const QString &id) const
{
    Q_UNUSED(id);
    // Registry contents are untrusted. Only our exact generation namespace
    // can own text files. Inspect with lstat before
    // SQLite opens anything: reject symlinks, hardlinks and non-regular files,
    // including sidecars, and physical aliases of the personal store.
    static const QRegularExpression generation("^bible-[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\.sqlite$");
    const QFileInfo file(path);
    if (file.absolutePath() != m_directory || path != m_directory + '/' + file.fileName()
        || !generation.match(file.fileName()).hasMatch()) return false;
    const QStringList suffixes{"","-wal","-shm","-journal"};
    for (const auto &suffix : suffixes) {
        struct stat candidate;
        if (::lstat(QFile::encodeName(path+suffix).constData(),&candidate) != 0) {
            if (errno == ENOENT) continue;
            return false;
        }
        if (!S_ISREG(candidate.st_mode) || candidate.st_nlink != 1 || candidate.st_uid != ::geteuid()) return false;
        for (const auto &reserved : suffixes) {
            struct stat personal;
            if (::stat(QFile::encodeName(m_directory+"/personal.sqlite"+reserved).constData(),&personal) == 0
                && candidate.st_dev == personal.st_dev && candidate.st_ino == personal.st_ino) return false;
        }
    }
    return true;
}

void BibleBackend::removeOwnedBibleFiles(const QString &path, const QString &id)
{
    // Recheck after commit too. If ownership has changed, leave an orphan rather
    // than unlinking an unrelated database. Never follow aliases during cleanup.
    if (!ownedBiblePath(path,id)) return;
    for (const QString &suffix : {QString(""),QString("-wal"),QString("-shm"),QString("-journal")})
        QFile::remove(path+suffix);
}

bool BibleBackend::readVersions(QVariantList *result)
{
    QSqlQuery q(m_store);
    q.prepare("SELECT version_id,abbreviation,name,version_id=? FROM installed ORDER BY 4 DESC,last_used DESC,version_id");
    q.addBindValue(m_versionId.isNull() ? QString("") : m_versionId);
    if (!q.exec())
        return setError("Cannot read installed versions: " + q.lastError().text());
    QVariantList rows;
    while (q.next()) rows.append(QVariantMap{{"id",q.value(0)},{"abbreviation",q.value(1)},
        {"name",q.value(2)},{"active",q.value(3).toBool()}});
    if (q.lastError().isValid()) return setError("Cannot read installed versions: " + q.lastError().text());
    *result = rows; return true;
}

bool BibleBackend::switchVersion(QString id)
{
    if (m_busy) return setError("Wait for the USFM import to finish before switching versions.");
    if (id.isEmpty() || installedPath(id).isEmpty()) return setError("That Bible version is not installed.");
    if (id == m_versionId && m_ready) { clearError(); return true; }
    return changeVersion(id);
}

bool BibleBackend::removeVersion(QString id, bool removeAnnotations)
{
    if (sharedMutationBusy()) return setError("Wait for the shared library operation to finish before removing versions.");
    if (id.isEmpty() || installedPath(id).isEmpty()) return setError("That Bible version is not installed.");
    m_mutating = true;
    const auto guard = qScopeGuard([this] { m_mutating = false; });
    if (!flushDrafts()) return false;
    QString next = m_versionId;
    if (next == id) {
        next.clear();
        for (const auto &row : m_versions)
            if (row.toMap()["id"].toString() != id) { next = row.toMap()["id"].toString(); break; }
    }
    return changeVersion(next,{},id,removeAnnotations);
}

bool BibleBackend::changeVersion(const QString &id, const QString &importPath,
                                const QString &removedId, bool deleteAnnotations, bool startup, bool peerReload,
                                const QVariantMap &targetScope)
{
    if (!m_storeReady) return setError("Your notes database is unavailable.");
    const QString path = importPath.isEmpty() ? installedPath(id) : importPath;
    if (!id.isEmpty() && path.isEmpty()) return setError("That Bible version is not installed.");
    if (!id.isEmpty() && !ownedBiblePath(path,id)) return setError("Unsafe Bible database path.");
    QString obsolete;
    const QString obsoleteId = !removedId.isEmpty() ? removedId : !importPath.isEmpty() ? id : QString();
    if (!obsoleteId.isEmpty()) {
        QSqlQuery candidate(m_store);
        candidate.prepare("SELECT path FROM installed WHERE version_id=?"); candidate.addBindValue(obsoleteId);
        if (!candidate.exec()) return setError("Cannot inspect the Bible removal target: " + candidate.lastError().text());
        if (candidate.next()) {
            obsolete = m_directory + '/' + candidate.value(0).toString();
            if (!ownedBiblePath(obsolete,obsoleteId)) return setError("Unsafe Bible removal target.");
            if (QFileInfo::exists(obsolete) && !bibleIdentity(obsolete,obsoleteId))
                return setError("The Bible removal target does not belong to this edition.");
            // A valid generation belonging to another installed edition is also
            // not ours to delete, even if a corrupt registry points at it.
            candidate.finish();
            candidate.prepare("SELECT COUNT(*) FROM installed WHERE path=? AND version_id<>?");
            candidate.addBindValue(QFileInfo(obsolete).fileName()); candidate.addBindValue(obsoleteId);
            if (!candidate.exec() || !candidate.next() || candidate.value(0).toInt()) return setError("Ambiguous Bible removal target.");
        } else if (candidate.lastError().isValid() || !removedId.isEmpty()) return setError("Cannot find the Bible removal target.");
    }
    QString validationError;
    if (!id.isEmpty() && !UsfmImporter::validateDatabase(path,&validationError)) return setError(validationError);
    const auto oldAnchor = endpoint(m_anchor), oldCursor = endpoint(m_cursor);
    // Stage using the ordinary SQL/reference readers with signals blocked. Keep
    // the old read-only connection open until commit; rollback restores every
    // observable member before reporting the failure. No event loop is entered.
    auto oldBible = m_bible;
    const auto oldConnection = m_bibleConnection;
    const auto oldId = m_versionId, oldEdition = m_edition, oldAbbreviation = m_abbreviation, oldCopyright = m_copyright;
    const auto oldKeys = m_keys;
    const bool oldParagraphs = m_hasParagraphs;
    const auto oldBooks = m_books;
    const auto oldChapters = m_chapters;
    const auto oldAliases = m_bookAliases;
    const auto oldIndices = m_keyIndices;
    const int oldBook = m_book, oldChapter = m_chapter, oldStart = m_start, oldEnd = m_end, oldA = m_anchor, oldC = m_cursor;
    const double oldPosition = m_position;
    QSignalBlocker blocker(this);
    bool committed = false;
    auto rollback = qScopeGuard([&] {
        if (committed) return;
        m_store.rollback();
        closeBible(); m_bible = oldBible; m_bibleConnection = oldConnection;
        m_versionId = oldId; m_edition = oldEdition; m_abbreviation = oldAbbreviation; m_copyright = oldCopyright;
        m_keys = oldKeys; m_book = oldBook; m_chapter = oldChapter; m_start = oldStart; m_end = oldEnd;
        m_anchor = oldA; m_cursor = oldC; m_position = oldPosition;
        m_hasParagraphs = oldParagraphs; m_books = oldBooks; m_chapters = oldChapters;
        m_bookAliases = oldAliases; m_keyIndices = oldIndices;
        blocker.unblock(); emit errorChanged();
    });
    m_bible = QSqlDatabase(); m_bibleConnection.clear();
    m_versionId = id; m_edition.clear(); m_abbreviation.clear(); m_copyright.clear(); m_keys = {{0,0,0}};
    m_hasParagraphs = false; m_books.clear(); m_chapters.clear(); m_bookAliases.clear(); m_keyIndices.clear();
    if (!id.isEmpty()) {
        m_bibleConnection = connectionName(); m_bible = QSqlDatabase::addDatabase("QSQLITE",m_bibleConnection);
        m_bible.setDatabaseName(path); m_bible.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=5000");
        if (!m_bible.open()) return setError("Cannot open imported Bible: " + m_bible.lastError().text());
        QSqlQuery text(m_bible);
        if (!text.exec("SELECT key,value FROM metadata")) return setError("Cannot read Bible metadata: " + text.lastError().text());
        QVariantMap metadata;
        while (text.next()) metadata.insert(text.value(0).toString(),text.value(1));
        if (text.lastError().isValid()) return setError("Cannot read Bible metadata.");
        QString actualId = metadata["version_id"].toString();
        if (actualId != id) return setError("The imported Bible's stable version ID does not match the library.");
        m_edition = metadata["edition"].toString(); m_abbreviation = metadata["abbreviation"].toString();
        m_copyright = metadata["copyright"].toString();
        m_hasParagraphs = metadata["paragraph_layout"] == "1";
        if (!text.exec("SELECT v.book,v.chapter,v.number,v.paragraphStart FROM verses v JOIN books b ON b.id=v.book ORDER BY b.ordinal,v.chapter,v.number")) return setError("Cannot read Bible verse keys.");
        while (text.next()) {
            const int b = text.value(0).toInt(), c = text.value(1).toInt(), v = text.value(2).toInt();
            m_keyIndices.insert(verseKey(b,c,v),m_keys.size());
            m_keys.append({b,c,v,text.value(3).toBool()});
            if (!m_chapters[b].contains(c)) m_chapters[b].append(c);
        }
        if (text.lastError().isValid() || m_keys.size() < 2) return setError("Cannot read complete Bible verse keys.");
        if (!text.exec("SELECT id,code,name,abbreviation FROM books ORDER BY ordinal")) return setError("Cannot read Bible books.");
        while (text.next()) {
            const int b = text.value(0).toInt();
            m_books.append(QVariantMap{{"id",b},{"code",text.value(1)},{"name",text.value(2)},
                {"abbreviation",text.value(3)},{"chapters",m_chapters.value(b).size()},{"chapterNumbers",m_chapters.value(b)}});
            for (int column = 1; column <= 3; ++column) {
                const auto alias = aliasKey(text.value(column).toString());
                if (!alias.isEmpty()) m_bookAliases.insert(alias,b);
            }
        }
        if (text.lastError().isValid()) return setError("Cannot read Bible books.");
        if (!text.exec("SELECT book,name FROM book_aliases")) return setError("Cannot read source book aliases.");
        while (text.next()) {
            const auto alias = aliasKey(text.value(1).toString());
            const int b = text.value(0).toInt();
            if (m_bookAliases.contains(alias) && m_bookAliases.value(alias) != b) m_bookAliases[alias] = 0;
            else m_bookAliases.insert(alias,b);
        }
        if (text.lastError().isValid()) return setError("Cannot read source book aliases.");
    }
    if (!m_store.transaction()) return setError("Cannot start library update: " + m_store.lastError().text());
    QSqlQuery q(m_store);
    // m_keys now belongs to the candidate generation, not the installed file.
    // Tags-only ranges must gate replacement just like annotated ranges do.
    // Read every retained assignment, including editions being reinstalled after
    // removal; missing or reversed endpoints would otherwise disappear in queries.
    if (!id.isEmpty()) {
        q.prepare("SELECT DISTINCT book,chapter,start,endBook,endChapter,end FROM tag_assignments WHERE version_id=?");
        q.addBindValue(id);
        if (!q.exec()) return setError("Cannot validate retained passage tags: " + q.lastError().text());
        while (q.next()) {
            const int first = absoluteVerse(q.value(0).toInt(),q.value(1).toInt(),q.value(2).toInt());
            const int last = absoluteVerse(q.value(3).toInt(),q.value(4).toInt(),q.value(5).toInt());
            if (!first || last < first)
                return setError("This Bible cannot replace the installed edition: a retained tagged passage has missing or reversed endpoints.");
        }
        if (q.lastError().isValid()) return setError("Cannot read all retained passage tags: " + q.lastError().text());
        q.finish();
    }
    if (!importPath.isEmpty()) {
        q.prepare("INSERT INTO installed(version_id,path,abbreviation,name,copyright) VALUES(?,?,?,?,?) ON CONFLICT(version_id) DO UPDATE SET path=excluded.path,abbreviation=excluded.abbreviation,name=excluded.name,copyright=excluded.copyright");
        q.addBindValue(id); q.addBindValue(QFileInfo(path).fileName()); q.addBindValue(m_abbreviation);
        q.addBindValue(m_edition); q.addBindValue(m_copyright.isNull() ? QString("") : m_copyright);
        if (!q.exec()) return setError("Cannot install Bible version: " + q.lastError().text());
    }
    if (!removedId.isEmpty()) {
        q.prepare("DELETE FROM installed WHERE version_id=?"); q.addBindValue(removedId);
        if (!q.exec() || q.numRowsAffected() != 1) return setError("Cannot remove Bible version: " + q.lastError().text());
        if (deleteAnnotations) {
            for (const QString &table : {QString("annotations"),QString("reading"),QString("tag_assignments")}) {
                q.prepare("DELETE FROM " + table + " WHERE version_id=?"); q.addBindValue(removedId);
                if (!q.exec()) return setError("Cannot remove version annotations: " + q.lastError().text());
                q.prepare("SELECT COUNT(*) FROM " + table + " WHERE version_id=?"); q.addBindValue(removedId);
                if (!q.exec() || !q.next() || q.value(0).toInt()) return setError("Cannot verify version annotation removal.");
            }
        }
    }
    if (!peerReload) {
        q.prepare("UPDATE library SET active=? WHERE id=1"); q.addBindValue(id.isNull() ? QString("") : id);
        if (!q.exec() || q.numRowsAffected() != 1) return setError("Cannot save active version: " + q.lastError().text());
        q.prepare("SELECT active FROM library WHERE id=1");
        if (!q.exec() || !q.next() || q.value(0).toString() != id) return setError("Cannot verify active version.");
        q.finish();
    }
    // Rank is a library-owned monotonic counter, never a wall-clock timestamp
    // or MAX(installed): deleting/readding the newest edition gets a fresh rank.
    // Startup and removal of an inactive edition are not new uses.
    if (!peerReload && !id.isEmpty() && !startup && (id != oldId || !importPath.isEmpty())) {
        if (!q.exec("SELECT use_counter FROM library WHERE id=1") || !q.next()) return setError("Cannot read version usage counter.");
        const qint64 previous = q.value(0).toLongLong(); q.finish();
        if (!q.exec("UPDATE library SET use_counter=use_counter+1 WHERE id=1")) return setError("Cannot advance version usage counter: " + q.lastError().text());
        q.prepare("UPDATE installed SET last_used=(SELECT use_counter FROM library WHERE id=1) WHERE version_id=?"); q.addBindValue(id);
        if (!q.exec() || q.numRowsAffected() != 1) return setError("Cannot save version usage rank.");
        q.prepare("SELECT last_used,use_counter FROM installed,library WHERE version_id=? AND library.id=1"); q.addBindValue(id);
        if (!q.exec() || !q.next() || q.value(0).toLongLong() <= previous || q.value(0) != q.value(1))
            return setError("Cannot verify version usage rank.");
        q.finish();
    }
    bool fallback = false;
    QVariantList rows, versions;
    QVector<Annotation> annotations;
    if (!id.isEmpty()) {
        q.prepare("INSERT OR IGNORE INTO reading VALUES(1,1,1,0,0,0,0,0,0,0,?)"); q.addBindValue(id);
        if (!q.exec()) return setError("Cannot initialize version reading location: " + q.lastError().text());
        m_anchor = absoluteVerse(oldAnchor.book,oldAnchor.chapter,oldAnchor.verse);
        m_cursor = absoluteVerse(oldCursor.book,oldCursor.chapter,oldCursor.verse);
        if (startup) {
            q.prepare("SELECT book,chapter,start,end,position,anchorBook,anchorChapter,cursorBook,cursorChapter FROM reading WHERE version_id=?"); q.addBindValue(id);
            if (!q.exec() || !q.next()) return setError("Cannot read your saved reading location: " + q.lastError().text());
            m_book = q.value(0).toInt(); m_chapter = q.value(1).toInt(); m_position = q.value(4).toDouble();
            m_anchor = absoluteVerse(q.value(5).toInt(),q.value(6).toInt(),q.value(2).toInt());
            m_cursor = absoluteVerse(q.value(7).toInt(),q.value(8).toInt(),q.value(3).toInt());
            q.finish();
        } else fallback = oldA && (!m_anchor || !m_cursor);
        if (!m_anchor || !m_cursor || endpoint(m_cursor).book != m_book || endpoint(m_cursor).chapter != m_chapter)
            m_anchor = m_cursor = 0;
        if (!validRange(m_book,m_chapter,0,0)) {
            fallback = !oldId.isEmpty() || startup;
            const auto chapters = chaptersForBook(m_book);
            if (!chapters.isEmpty()) {
                const int requestedChapter = m_chapter;
                m_chapter = std::min_element(chapters.cbegin(),chapters.cend(),[&](const QVariant &a, const QVariant &b) {
                    return std::abs(a.toInt()-requestedChapter) < std::abs(b.toInt()-requestedChapter);
                })->toInt();
            } else {
                m_book = m_chapters.contains(1) ? 1 : m_books.first().toMap()["id"].toInt();
                m_chapter = chaptersForBook(m_book).first().toInt();
            }
            m_anchor = m_cursor = 0;
        }
        if (fallback || !std::isfinite(m_position) || m_position < 0 || m_position > 1) m_position = 0;
        if (!targetScope.isEmpty()) {
            m_anchor = absoluteVerse(targetScope.value("book").toInt(),targetScope.value("chapter").toInt(),targetScope.value("start").toInt());
            m_cursor = absoluteVerse(targetScope.value("endBook").toInt(),targetScope.value("endChapter").toInt(),targetScope.value("end").toInt());
            if (!m_anchor || m_cursor < m_anchor) return setError("The tagged passage endpoints are no longer available.");
            m_book = endpoint(m_cursor).book; m_chapter = endpoint(m_cursor).chapter; m_position = 0; fallback = false;
        }
        intersectSelection();
        if (!readChapter(m_book,m_chapter,&rows) || !loadAnnotations(&annotations)
            || (!peerReload && !persistState(m_book,m_chapter,m_anchor,m_cursor,m_position))) return false;
    } else { m_book = m_chapter = 1; m_start = m_end = m_anchor = m_cursor = 0; m_position = 0; }
    if (!readVersions(&versions)) return false;
    if ((!id.isEmpty() && installedPath(id) != path) || (!removedId.isEmpty() && !installedPath(removedId).isEmpty()))
        return setError("Cannot verify library update.");
    if (!id.isEmpty()) {
        q.prepare("SELECT abbreviation,name,copyright FROM installed WHERE version_id=?"); q.addBindValue(id);
        if (!q.exec() || !q.next() || q.value(0).toString() != m_abbreviation || q.value(1).toString() != m_edition
            || (!importPath.isEmpty() && q.value(2).toString() != m_copyright)) return setError("Cannot verify installed version metadata.");
        q.finish();
    }
    if (!m_store.commit()) return setError("Cannot commit library update: " + m_store.lastError().text());
    committed = true;
    oldBible.close(); oldBible = QSqlDatabase();
    if (!oldConnection.isEmpty()) QSqlDatabase::removeDatabase(oldConnection);
    m_ready = !id.isEmpty(); m_verses = rows; m_annotations = annotations;
    m_collection = collectionRows(annotations); m_versions = versions;
    blocker.unblock();
    // Direct delivery closes/reloads every peer before obsolete files are unlinked.
    if (!peerReload) { emit sharedCommitted(obsoleteId, true, false); publishTags(); }
    clearError();
    setStatus(fallback ? "The selected endpoint is absent in " + edition() + "; opened " + passage() + " instead."
        : m_ready ? "Ready · " + edition() : "Import a USFM Bible to begin.");
    updateAnnotations();
    emit versionChanged(); emit versionsChanged(); emit booksChanged(); emit readyChanged(); emit collectionChanged();
    emit passageChanged(); emit selectionChanged(); emit readingPositionChanged(); emit navigationChanged();
    if (!obsolete.isEmpty() && obsolete != path) removeOwnedBibleFiles(obsolete,obsoleteId);
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
    return m_ready || setError("Import a USFM Bible first.");
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
void BibleBackend::importUsfm(QUrl file)
{
    if (sharedMutationBusy()) { setError("A shared library operation is already running."); return; }
    if (!m_storeReady || !m_store.isOpen()) { setError("Your notes database is unavailable. Check the data directory permissions."); return; }
    const QString path = localPath(file);
    if (path.isEmpty()) return;
    if (!QFileInfo(path).isFile() || !QFileInfo(path).isReadable()) { setError("Cannot read the selected USFM file."); return; }
    QString error;
    const auto edition = UsfmImporter::inspectFile(path,&error);
    if (edition.id.isEmpty()) { setError(error); return; }
    if (!flushDrafts()) return;
    clearError();
    m_busy = true; m_progress = 0;
    m_importProgress = std::make_shared<std::atomic<double>>(0);
    setStatus("Importing and validating " + edition.name + "…");
    notifyBusy(); emit importProgressChanged();
    const QString target = m_directory + "/bible-" + connectionName() + ".sqlite";
    m_importTarget = target;
    const auto progress = m_importProgress;
    m_importWatcher.setFuture(QtConcurrent::run([path, target, progress] { return UsfmImporter::importFile(path, target, progress); }));
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
    if (!chaptersForBook(b).contains(c)) return false;
    if (!start && !end) return true;
    if (start < 1 || end < start || end > 999) return false;
    return verseExists(b,c,start) && verseExists(b,c,end);
}
QVariantList BibleBackend::chapterVerses(int book, int chapter) const
{
    if (!m_ready || !chaptersForBook(book).contains(chapter)) return {};
    QSqlQuery q(m_bible);
    q.prepare("SELECT number,text FROM verses WHERE book=? AND chapter=? ORDER BY number");
    q.addBindValue(book); q.addBindValue(chapter);
    if (!q.exec()) return {};
    QVariantList rows;
    while (q.next()) {
        QString text = q.value(1).toString();
        rows.append(QVariantMap{{"number",q.value(0)},{"text",text},
            {"paragraphStart",endpoint(absoluteVerse(book,chapter,q.value(0).toInt())).paragraphStart}});
    }
    return q.lastError().isValid() ? QVariantList() : rows;
}
bool BibleBackend::persistState(int b, int c, int start, int end, double position)
{
    const auto a = endpoint(start), z = endpoint(end);
    QSqlQuery q(m_store);
    if (!q.exec("SAVEPOINT reading_save")) return setError("Cannot start reading location save: " + q.lastError().text());
    bool saved = false;
    const auto rollback = qScopeGuard([&] {
        if (!saved) { QSqlQuery undo(m_store); undo.exec("ROLLBACK TO reading_save"); undo.exec("RELEASE reading_save"); }
    });
    q.prepare("UPDATE reading SET book=?,chapter=?,start=?,end=?,position=?,anchorBook=?,anchorChapter=?,cursorBook=?,cursorChapter=? WHERE id=1 AND version_id=?");
    q.addBindValue(b); q.addBindValue(c); q.addBindValue(a.verse); q.addBindValue(z.verse); q.addBindValue(position);
    q.addBindValue(a.book); q.addBindValue(a.chapter); q.addBindValue(z.book); q.addBindValue(z.chapter);
    q.addBindValue(m_versionId);
    if (!q.exec() || q.numRowsAffected() != 1) return setError("Cannot save your reading location: " + q.lastError().text());
    q.prepare("SELECT book,chapter,start,end,position,anchorBook,anchorChapter,cursorBook,cursorChapter FROM reading WHERE version_id=?");
    q.addBindValue(m_versionId);
    if (!q.exec() || !q.next() || q.value(0).toInt() != b || q.value(1).toInt() != c
        || q.value(2).toInt() != a.verse || q.value(3).toInt() != z.verse || q.value(4).toDouble() != position
        || q.value(5).toInt() != a.book || q.value(6).toInt() != a.chapter
        || q.value(7).toInt() != z.book || q.value(8).toInt() != z.chapter)
        return setError("Cannot verify your saved reading location: " + q.lastError().text());
    q.finish();
    if (!q.exec("RELEASE reading_save")) return setError("Cannot commit your reading location: " + q.lastError().text());
    saved = true;
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
    while (q.next()) {
        QString text = q.value(1).toString(), html = q.value(2).toString();
        rows.append(QVariantMap{{"number",q.value(0)}, {"text",text}, {"html",html},
            {"paragraphStart",endpoint(absoluteVerse(book,chapter,q.value(0).toInt())).paragraphStart},
            {"heading",q.value(3)}, {"poetry",q.value(4).toBool()}, {"highlighted",false}, {"bookmarked",false}, {"hasNote",false}});
    }
    QVector<int> expected;
    for (const auto &key : m_keys) if (key.book == book && key.chapter == chapter) expected.append(key.verse);
    if (q.lastError().isValid() || rows.isEmpty() || rows.size() != expected.size())
        return setError("The requested chapter could not be read completely. Please re-import your USFM Bible.");
    int index = 0;
    for (const auto &row : rows) {
        const auto map = row.toMap();
        if (map.value("number").toInt() != expected[index++] || map.value("text").toString().trimmed().isEmpty()
            || map.value("html").toString().trimmed().isEmpty())
            return setError("The requested chapter contains invalid verses. Please re-import your USFM Bible.");
    }
    *result = rows;
    return true;
}

bool BibleBackend::goTo(int b, int c, int start, int end)
{
    if (!requireReady()) return false;
    if (!validRange(b,c,start,end)) return setError("That chapter or verse range is not present in " + edition() + '.');
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
    if (!m_ready) return fail("Import a USFM Bible first.");
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
    int b = resolveBook(input), c = chaptersForBook(b).value(0,1).toInt(), endChapter = -1, start = 0, end = 0;
    bool matched = b != 0;
    if (!matched) {
        for (qsizetype split = input.size() - 1; split >= 0; --split) {
            const int candidate = split == 0 ? m_book : resolveBook(input.left(split));
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
            } else if (chaptersForBook(b) == QVariantList{1} && c != 1) {
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
        b = resolveBook(match.captured(1));
        if (match.hasMatch() && b && chaptersForBook(b) == QVariantList{1}) {
            c = 1; start = match.captured(2).toInt(); end = match.captured(3).toInt(); matched = true;
            if (start < 1 || end < 1) return fail("Verse numbers must be greater than zero.");
        }
    }
    if (!matched) return fail("Reference not recognized. Try John 3, Jn 3:16–18, or 1 Cor 13.");
    if (endChapter < 0) endChapter = c;
    const int a = absoluteVerse(b,c,start), z = absoluteVerse(b,endChapter,end);
    if (!validRange(b,c,start,start) || !validRange(b,endChapter,end,end) || (start && (!a || !z || a > z))) {
        if (b >= 1 && b <= 80 && start == end && start && !a)
            return fail(reference(b,c,start,end) + " is absent in " + edition() + '.');
        return fail("That chapter or verse range is not present in " + edition() + '.');
    }
    *book = b; *chapter = endChapter; *first = a; *last = z;
    return true;
}
void BibleBackend::openChapter(int b, int c) { goTo(b,c,0,0); }
void BibleBackend::previousChapter()
{
    if (!requireReady()) return;
    for (int i = 1; i < m_keys.size(); ++i) {
        if (m_keys[i].book == m_book && m_keys[i].chapter == m_chapter) {
            if (i > 1) openChapter(m_keys[i-1].book,m_keys[i-1].chapter);
            return;
        }
    }
}
void BibleBackend::nextChapter()
{
    if (!requireReady()) return;
    bool current = false;
    for (const auto &key : m_keys) {
        if (key.book == m_book && key.chapter == m_chapter) current = true;
        else if (current) { openChapter(key.book,key.chapter); return; }
    }
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
    q.prepare("SELECT book,chapter,start,end,bookmark,highlight,note,endBook,endChapter FROM annotations WHERE version_id=? ORDER BY book,chapter,start,endBook,endChapter,end");
    q.addBindValue(m_versionId);
    if (!q.exec())
        return setError("Cannot read your annotations: " + q.lastError().text());
    QVector<Annotation> rows;
    while (q.next()) {
        const int start = absoluteVerse(q.value(0).toInt(),q.value(1).toInt(),q.value(2).toInt());
        const int end = absoluteVerse(q.value(7).toInt(),q.value(8).toInt(),q.value(3).toInt());
        if (!start || end < start) return setError("Cannot read your annotations: invalid range endpoints.");
        rows.append({start,end,q.value(4).toBool(),q.value(5).toBool(),q.value(6).toString()});
    }
    if (q.lastError().isValid()) return setError("Cannot read your annotations: " + q.lastError().text());
    std::sort(rows.begin(),rows.end(),[](const Annotation &a, const Annotation &b) {
        return std::tie(a.start,a.end) < std::tie(b.start,b.end);
    });
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
    const auto tagged = tagRangeRows(m_versionId,false);
    for (auto &row : m_verses) {
        auto map = row.toMap();
        const int v = absoluteVerse(m_book,m_chapter,map.value("number").toInt());
        bool bookmark = false, highlight = false, note = false;
        bool start = false, end = false, before = false, after = false;
        bool highlightStart = false, highlightEnd = false, highlightBefore = false, highlightAfter = false;
        QVariantList ranges, highlightRanges, noteRanges;
        for (const auto &a : m_annotations) {
            if (v >= a.start && v <= a.end) {
                bookmark |= a.bookmark; highlight |= a.highlight; note |= !a.note.isEmpty();
                if (!a.note.isEmpty()) {
                    noteRanges.append(QVariantMap{{"reference",rangeReference(a.start,a.end)},
                        {"noteStart",v == a.start},{"noteEnd",v == a.end},
                        {"continuesBefore",v > a.start},{"continuesAfter",v < a.end}});
                }
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
        map["noteRanges"] = noteRanges;
        QVariantList tagRanges;
        for (const auto &value : tagged) {
            auto range = value.toMap();
            const int a = absoluteVerse(range.value("book").toInt(),range.value("chapter").toInt(),range.value("start").toInt());
            const int z = absoluteVerse(range.value("endBook").toInt(),range.value("endChapter").toInt(),range.value("end").toInt());
            if (v < a || v > z) continue;
            range = publicRange(range);
            range["tagStart"] = v == a; range["tagEnd"] = v == z;
            range["continuesBefore"] = v > a; range["continuesAfter"] = v < z;
            tagRanges.append(range);
        }
        map["hasTags"] = !tagRanges.isEmpty(); map["tagRanges"] = tagRanges;
        map["highlightStart"] = highlightStart; map["highlightEnd"] = highlightEnd;
        map["highlightContinuesBefore"] = highlightBefore; map["highlightContinuesAfter"] = highlightAfter;
        map["highlightRanges"] = highlightRanges;
        row = map;
    }
    emit versesChanged(); updateSelection();
}
void BibleBackend::refreshCollection()
{
    if (!m_ready) return;
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
            {"reference",rangeReference(a.start,a.end)},{"versionId",m_versionId},
            {"versionAbbreviation",m_abbreviation},{"tags",QVariantList()},
            {"bookmark",a.bookmark},{"highlight",a.highlight},{"note",a.note}});
    }
    for (const auto &value : tagRangeRows(m_versionId,false)) {
        const auto tagged = publicRange(value.toMap());
        auto found = std::find_if(collection.begin(),collection.end(),[&](const QVariant &row) {
            return rangeKey(row.toMap()) == rangeKey(tagged);
        });
        if (found == collection.end()) collection.append(tagged);
        else { auto row = found->toMap(); row["tags"] = tagged.value("tags"); *found = row; }
    }
    std::sort(collection.begin(),collection.end(),[this](const QVariant &a, const QVariant &b) {
        auto order = [this](const QVariant &v) {
            const auto r = v.toMap();
            return std::make_pair(absoluteVerse(r.value("book").toInt(),r.value("chapter").toInt(),r.value("start").toInt()),
                absoluteVerse(r.value("endBook").toInt(),r.value("endChapter").toInt(),r.value("end").toInt()));
        };
        return order(a) < order(b);
    });
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
    q.prepare("DELETE FROM annotations WHERE version_id=?"); q.addBindValue(m_versionId);
    if (!q.exec()) return setError("Cannot save annotations: " + q.lastError().text());
    if (!q.prepare("INSERT INTO annotations(book,chapter,start,end,bookmark,highlight,note,endBook,endChapter,version_id) VALUES(?,?,?,?,?,?,?,?,?,?)")) return setError(q.lastError().text());
    for (const auto &a : merged) {
        const auto start = endpoint(a.start), end = endpoint(a.end);
        q.bindValue(0,start.book); q.bindValue(1,start.chapter); q.bindValue(2,start.verse); q.bindValue(3,end.verse);
        q.bindValue(4,a.bookmark); q.bindValue(5,a.highlight); q.bindValue(6,a.note.isNull() ? QString("") : a.note);
        q.bindValue(7,end.book); q.bindValue(8,end.chapter);
        q.bindValue(9,m_versionId);
        if (!q.exec()) return setError("Cannot save annotations: " + q.lastError().text());
    }
    QVector<Annotation> verified;
    if (!loadAnnotations(&verified)) return false;
    if (verified.size() != merged.size()) return setError("Cannot verify saved annotations.");
    for (const auto &a : verified) {
        const auto it = merged.constFind(QString("%1:%2").arg(a.start).arg(a.end));
        if (it == merged.cend() || it->bookmark != a.bookmark || it->highlight != a.highlight || it->note != a.note)
            return setError("Cannot verify saved annotations.");
    }
    return true;
}

bool BibleBackend::toggleFlag(bool highlight)
{
    if (!requireReady()) return false;
    if (!m_start) return setError("Select one or more verses first.");
    if (!m_store.transaction()) return setError("Cannot start annotation save: " + m_store.lastError().text());
    QVector<Annotation> rows;
    if (!loadAnnotations(&rows)) { m_store.rollback(); return false; }
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
    return commitAnnotations(rows);
}
bool BibleBackend::commitAnnotations(const QVector<Annotation> &rows)
{
    QVector<Annotation> saved;
    if (!saveAnnotations(rows) || !loadAnnotations(&saved)) { m_store.rollback(); return false; }
    const auto collection = collectionRows(saved);
    if (!m_store.commit()) { m_store.rollback(); return setError("Cannot commit annotations: " + m_store.lastError().text()); }
    m_annotations = saved; m_collection = collection;
    publishAnnotations();
    clearError(); emit collectionChanged(); updateAnnotations(); return true;
}
bool BibleBackend::toggleBookmark() { return toggleFlag(false); }
bool BibleBackend::toggleHighlight() { return toggleFlag(true); }

void BibleBackend::setNoteText(QString text)
{
    if (text == m_note) return;
    if (!requireReady()) return;
    if (!m_start) { setError("Select the verse or range for this note first."); return; }
    if (text.size() > MaximumNoteSize || !safeString(text)) { setError("The note is too large or contains unsupported control characters."); return; }
    if (!m_store.transaction()) { setError("Cannot start note save: " + m_store.lastError().text()); return; }
    QVector<Annotation> rows;
    if (!loadAnnotations(&rows)) { m_store.rollback(); return; }
    const int start = std::min(m_anchor,m_cursor), end = std::max(m_anchor,m_cursor);
    const auto found = std::find_if(rows.begin(),rows.end(),[&](const Annotation &a) {
        return a.start == start && a.end == end;
    });
    const QString savedNote = found == rows.end() ? QString() : found->note;
    // Peer delivery is deferred while all windows flush drafts. Compare against
    // this reader's last observed note inside the same transaction as the write,
    // including a missing/deleted note; reading fresh rows alone is not a merge.
    if (savedNote != m_note) {
        m_store.rollback();
        setError("This note changed in another window. Your draft was not saved. Review the saved text before replacing it.");
        return;
    }
    if (found != rows.end()) found->note = text;
    else if (!text.isEmpty()) rows.append({start,end,false,false,text});
    commitAnnotations(rows);
}

bool BibleBackend::deleteAnnotation(QString input, QString expectedVersionId)
{
    if (!expectedVersionId.isEmpty() && expectedVersionId != m_versionId)
        return setError("The Bible version changed. Choose the annotation again before deleting it.");
    if (!requireReady()) return false;
    int b, c, first, last;
    QString error;
    if (!parseReference(input,&b,&c,&first,&last,&error)) return setError(error);
    if (!first) return setError("Choose an exact saved verse range to delete.");
    const auto a = endpoint(first), z = endpoint(last);
    return deleteAnnotationScope({{"versionId",m_versionId},{"book",a.book},{"chapter",a.chapter},{"start",a.verse},
        {"endBook",z.book},{"endChapter",z.chapter},{"end",z.verse}});
}

bool BibleBackend::deleteAnnotationScope(QVariantMap scope)
{
    if (!m_storeReady || sharedMutationBusy()) return setError("The personal store is unavailable or busy.");
    m_mutating = true;
    const auto guard = qScopeGuard([this] { m_store.rollback(); m_mutating = false; });
    if (!m_store.transaction()) return setError("Cannot start annotation deletion: " + m_store.lastError().text());
    QVariantMap canonical;
    if (!validateScope(scope,&canonical)) return setError("That Bible version or passage is no longer available.");
    QSqlQuery q(m_store);
    qint64 deleted = 0;
    for (const auto &table : {QString("annotations"),QString("tag_assignments")}) {
        q.prepare("DELETE FROM " + table + " WHERE " + scopeWhere); bindScope(q,canonical);
        if (!q.exec()) return setError("Cannot delete saved passage: " + q.lastError().text());
        deleted += q.numRowsAffected();
        q.prepare("SELECT COUNT(*) FROM " + table + " WHERE " + scopeWhere); bindScope(q,canonical);
        if (!q.exec() || !q.next() || q.value(0).toLongLong()) return setError("Cannot verify saved passage deletion.");
        q.finish();
    }
    if (!deleted) return setError("No saved annotation exists for that exact range.");
    QVector<Annotation> saved;
    if (!loadAnnotations(&saved)) return false;
    if (!m_store.commit()) return setError("Cannot commit annotation deletion: " + m_store.lastError().text());
    m_annotations = saved; m_collection = collectionRows(saved);
    emit sharedCommitted(canonical.value("versionId").toString(),false,false);
    publishTags();
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
    if (!requireReady()) return false;
    if (!m_store.isOpen()) return setError("Your notes database is unavailable.");
    const QString path = localPath(directory);
    if (path.isEmpty()) return false;
    if (!QFileInfo(path).isDir()) return setError("Choose an existing directory for the Markdown export.");
    if (!protectDestination(QDir(path).filePath("omaword-notes.md"))) return false;
    QVector<Annotation> rows;
    if (!loadAnnotations(&rows)) return false;
    // The export filename remains stable for existing scripts and consumers.
    QString markdown = "# " + Brand::name() + " notes\n\nEdition: " + edition() + " (" + versionId() + ")\n\n";
    for (const auto &value : collectionRows(rows)) {
        const auto a = value.toMap();
        markdown += "## " + markdownText(a.value("reference").toString()) + "\n\n";
        QStringList flags;
        if (a.value("bookmark").toBool()) flags << "Bookmark";
        if (a.value("highlight").toBool()) flags << "Highlight";
        if (!flags.isEmpty()) markdown += "*" + flags.join(" · ") + "*\n\n";
        QStringList names;
        for (const auto &tag : a.value("tags").toList()) names << markdownText(tag.toMap().value("name").toString());
        if (!names.isEmpty()) markdown += "Tags: " + names.join(" · ") + "\n\n";
        if (!a.value("note").toString().isEmpty()) markdown += a.value("note").toString() + "\n\n";
    }
    QString error;
    if (!writeAtomic(QDir(path).filePath("omaword-notes.md"),markdown.toUtf8(),&error))
        return setError("Cannot export Markdown notes: " + error);
    clearError(); setStatus("Notes exported to " + QDir(path).filePath("omaword-notes.md")); return true;
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
    // Reserve every SQLite generation and sidecar, including unreferenced import
    // staging/garbage files. Canonical parent resolution covers directory aliases.
    if ((resolvedTarget.absolutePath() == QFileInfo(m_directory).canonicalFilePath()
        && (resolvedTarget.fileName().startsWith("personal.sqlite") || resolvedTarget.fileName().startsWith("bible.sqlite")
            || resolvedTarget.fileName().startsWith("bible-")))
        || resolved.startsWith(QFileInfo(m_directory).canonicalFilePath() + "/versions/"))
        return setError("Choose an export destination outside the live database files and sidecars.");
    // Also protect the physical target of an externally symlinked database and
    // its SQLite sidecars. Never trust just the spelling of the requested path.
    QStringList databases{m_directory + "/personal.sqlite",m_directory + "/bible.sqlite"};
    QSqlQuery q(m_store);
    if (!q.exec("SELECT path FROM installed")) return setError("Cannot verify the export destination: " + q.lastError().text());
    while (q.next()) databases << m_directory + '/' + q.value(0).toString();
    if (q.lastError().isValid()) return setError("Cannot verify the export destination.");
    for (const auto &database : databases) {
        const QString physical = QFileInfo(database).canonicalFilePath();
        for (const QString &base : {database,physical}) {
            if (base.isEmpty()) continue;
            for (const QString &suffix : {QString(""),QString("-wal"),QString("-shm"),QString("-journal")}) {
                const QFileInfo candidate(base+suffix);
                const QString candidatePath = candidate.exists() ? candidate.canonicalFilePath()
                    : QFileInfo(candidate.absolutePath()).canonicalFilePath() + '/' + candidate.fileName();
                if (resolved == candidatePath) return setError("Choose an export destination outside the live database files and sidecars.");
            }
        }
    }
    return true;
}

bool BibleBackend::exportBackup(QUrl file)
{
    if (!requireReady()) return false;
    if (!m_store.isOpen()) return setError("Your notes database is unavailable.");
    const QString path = localPath(file);
    if (path.isEmpty() || !protectDestination(path)) return false;
    if (!m_store.transaction()) return setError("Cannot read a consistent backup: " + m_store.lastError().text());
    QVector<Annotation> rows;
    if (!loadAnnotations(&rows)) { m_store.rollback(); return false; }
    QSqlQuery q(m_store);
    q.prepare("SELECT book,chapter,start,end,position,anchorBook,anchorChapter,cursorBook,cursorChapter FROM reading WHERE id=1 AND version_id=?");
    q.addBindValue(m_versionId);
    if (!q.exec() || !q.next()) {
        m_store.rollback(); return setError("Cannot read your reading position for backup.");
    }
    const QJsonObject reading{{"book",q.value(0).toInt()},{"chapter",q.value(1).toInt()},
        {"start",q.value(2).toInt()},{"end",q.value(3).toInt()},{"position",q.value(4).toDouble()},
        {"anchorBook",q.value(5).toInt()},{"anchorChapter",q.value(6).toInt()},
        {"cursorBook",q.value(7).toInt()},{"cursorChapter",q.value(8).toInt()}};
    q.finish();
    QJsonArray tagDefinitions, assignments;
    q.prepare("SELECT DISTINCT t.id,t.name FROM tags t JOIN tag_assignments a ON a.tag_id=t.id WHERE a.version_id=? ORDER BY t.id");
    q.addBindValue(m_versionId);
    if (!q.exec()) { m_store.rollback(); return setError("Cannot read backup tags: " + q.lastError().text()); }
    while (q.next()) tagDefinitions.append(QJsonObject{{"id",q.value(0).toString()},{"name",q.value(1).toString()}});
    if (q.lastError().isValid()) { m_store.rollback(); return setError("Cannot read complete backup tags."); }
    q.prepare("SELECT book,chapter,start,endBook,endChapter,end,tag_id FROM tag_assignments WHERE version_id=? ORDER BY book,chapter,start,endBook,endChapter,end,tag_id");
    q.addBindValue(m_versionId);
    if (!q.exec()) { m_store.rollback(); return setError("Cannot read backup tag assignments: " + q.lastError().text()); }
    while (q.next()) {
        QJsonObject assignment;
        for (int i = 0; i < scopeFields.size(); ++i) assignment[scopeFields[i]] = q.value(i).toInt();
        assignment["tagId"] = q.value(6).toString(); assignments.append(assignment);
    }
    if (q.lastError().isValid()) { m_store.rollback(); return setError("Cannot read complete backup tag assignments."); }
    q.finish();
    if (!m_store.commit()) { m_store.rollback(); return setError("Cannot finish reading the backup: " + m_store.lastError().text()); }
    QJsonArray annotations;
    for (const auto &a : rows) {
        const auto start = endpoint(a.start), end = endpoint(a.end);
        annotations.append(QJsonObject{{"book",start.book},{"chapter",start.chapter},
            {"start",start.verse},{"end",end.verse},{"endBook",end.book},{"endChapter",end.chapter},
            {"bookmark",a.bookmark},{"highlight",a.highlight},{"note",a.note}});
    }
    const QJsonObject root{{"format","omaword-backup"},{"version",4},{"edition",edition()},{"versionId",versionId()},
        {"created",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}, {"reading",reading},{"annotations",annotations},
        {"tags",tagDefinitions},{"tagAssignments",assignments}};
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (bytes.size() > MaximumBackupSize) return setError("This backup exceeds the supported 32 MiB size. Export Markdown notes instead.");
    QString error;
    if (!writeAtomic(path,bytes,&error)) return setError("Cannot export backup: " + error);
    clearError(); setStatus("Backup exported to " + path); return true;
}

bool BibleBackend::importBackup(QUrl file)
{
    if (!requireReady()) return false;
    const QString restoreVersion = m_versionId;
    if (sharedMutationBusy()) return setError("Wait for the shared library operation to finish before restoring annotations.");
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
    QStringList rootKeys{"format","version","edition","created","reading","annotations","versionId"};
    const bool withTags = root.value("version").toInt() == 4;
    if (withTags) rootKeys << "tags" << "tagAssignments";
    if (!exactKeys(root,rootKeys)
        || root.value("format") != "omaword-backup"
        || !integer(root.value("version"),3,4)
        || !root.value("edition").isString() || !root.value("created").isString()
        || !QDateTime::fromString(root.value("created").toString(),Qt::ISODateWithMs).isValid()
        || !root.value("reading").isObject() || !root.value("annotations").isArray())
        return setError("Unsupported or invalid " + Brand::name() + " backup format.");
    if (root.value("versionId") != versionId())
        return setError("This backup belongs to a different Bible version. Switch to its version before restoring.");
    if (root.value("edition") != edition()) return setError("The backup edition does not match the active Bible version.");
    const auto reading = root.value("reading").toObject();
    auto verse = [this](const QJsonObject &object, const QString &b, const QString &c, const QString &v) {
        if (!integer(object.value(b),1,80) || !integer(object.value(c),1,999) || !integer(object.value(v),1,999)) return 0;
        const int book = object.value(b).toInt(), chapter = object.value(c).toInt(), number = object.value(v).toInt();
        return verseExists(book,chapter,number) ? absoluteVerse(book,chapter,number) : 0;
    };
    QStringList readingKeys{"book","chapter","start","end","position","anchorBook","anchorChapter","cursorBook","cursorChapter"};
    const int b = reading.value("book").toInt(), c = reading.value("chapter").toInt();
    const bool none = integer(reading.value("start"),0,0) && integer(reading.value("end"),0,0);
    const int anchor = verse(reading,"anchorBook","anchorChapter","start");
    const int cursor = verse(reading,"cursorBook","cursorChapter","end");
    bool endpointsValid = none || (anchor && cursor && endpoint(cursor).book == b && endpoint(cursor).chapter == c);
    if (none) {
        for (const QString &key : {QString("anchorBook"),QString("anchorChapter"),QString("cursorBook"),QString("cursorChapter")})
            endpointsValid &= integer(reading.value(key),0,key.endsWith("Book") ? 80 : 999);
    }
    if (!exactKeys(reading,readingKeys) || !integer(reading.value("book"),1,80) || !integer(reading.value("chapter"),1,999)
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
        QStringList keys{"book","chapter","start","end","bookmark","highlight","note","endBook","endChapter"};
        const int start = verse(object,"book","chapter","start");
        const int end = verse(object,"endBook","endChapter","end");
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
    QMap<QString,QString> definitions;
    QVariantList assignments;
    if (withTags) {
        if (!root.value("tags").isArray() || !root.value("tagAssignments").isArray()
            || root.value("tags").toArray().size() > 100000 || root.value("tagAssignments").toArray().size() > 100000)
            return setError("The backup contains invalid or excessive tags.");
        QSet<QString> names, used, assignmentKeys;
        for (const auto &value : root.value("tags").toArray()) {
            const auto object = value.toObject();
            const QString id = object.value("id").toString(), name = normalizedTag(object.value("name").toString());
            if (!value.isObject() || !exactKeys(object,{"id","name"}) || !object.value("id").isString()
                || id.isEmpty() || id.size() > 4096 || !safeString(id) || !object.value("name").isString() || name.isEmpty()
                || definitions.contains(id) || names.contains(tagKey(name))) return setError("The backup contains invalid or duplicate tag definitions.");
            definitions[id] = name; names.insert(tagKey(name));
        }
        for (const auto &value : root.value("tagAssignments").toArray()) {
            const auto object = value.toObject();
            const QString id = object.value("tagId").toString();
            auto keys = scopeFields; keys << "tagId";
            QVariantMap scope = object.toVariantMap(), canonical;
            scope["versionId"] = restoreVersion;
            if (!value.isObject() || !exactKeys(object,keys) || !definitions.contains(id) || !validateScope(scope,&canonical))
                return setError("The backup contains an invalid tag assignment.");
            const QString key = rangeKey(canonical) + ':' + id;
            if (assignmentKeys.contains(key)) return setError("The backup contains duplicate tag assignments.");
            assignmentKeys.insert(key); used.insert(id);
            canonical["tagId"] = id; assignments.append(canonical);
        }
        if (used.size() != definitions.size()) return setError("The scoped backup contains unused tag definitions.");
    }
    // Restore is replacement, not a lossy merge. Validation precedes all writes;
    // notes, flags and reading state commit together or not at all.
    m_mutating = true;
    const auto guard = qScopeGuard([this] { m_mutating = false; });
    if (!flushDrafts()) return false;
    if (m_versionId != restoreVersion || installedPath(restoreVersion).isEmpty()) return setError("The Bible version changed before restore. Open the backup again.");
    if (!m_store.transaction()) return setError("Cannot start backup restore: " + m_store.lastError().text());
    const auto rollback = qScopeGuard([this] { m_store.rollback(); });
    if (withTags) {
        QSqlQuery q(m_store);
        q.prepare("DELETE FROM tag_assignments WHERE version_id=?"); q.addBindValue(restoreVersion);
        if (!q.exec()) return setError("Cannot replace backup tags: " + q.lastError().text());
        q.prepare("SELECT COUNT(*) FROM tag_assignments WHERE version_id=?"); q.addBindValue(restoreVersion);
        if (!q.exec() || !q.next() || q.value(0).toInt()) return setError("Cannot verify backup tag replacement.");
        q.finish();
        // Names define global identity. Never rename or delete a global definition
        // to accommodate an old backup ID: other editions may still use it.
        QMap<QString,QString> ids;
        for (auto it = definitions.cbegin(); it != definitions.cend(); ++it) {
            q.prepare("SELECT id FROM tags WHERE id=? OR name_key=?"); q.addBindValue(it.key()); q.addBindValue(tagKey(it.value()));
            if (!q.exec()) return setError("Cannot inspect restored tag identity: " + q.lastError().text());
            const bool existing = q.next(); q.finish();
            QString id;
            if (existing) id = ensureTag(it.value());
            else {
                id = it.key();
                q.prepare("INSERT INTO tags(id,name,name_key) VALUES(?,?,?)");
                q.addBindValue(id); q.addBindValue(it.value()); q.addBindValue(tagKey(it.value()));
                if (!q.exec() || q.numRowsAffected() != 1) return setError("Cannot restore tag definition: " + q.lastError().text());
                q.prepare("SELECT name,name_key FROM tags WHERE id=?"); q.addBindValue(id);
                if (!q.exec() || !q.next() || q.value(0).toString() != it.value() || q.value(1).toString() != tagKey(it.value()))
                    return setError("Cannot verify restored tag definition.");
                q.finish();
            }
            if (id.isEmpty()) return false;
            ids[it.key()] = id;
        }
        for (const auto &value : assignments) {
            const auto scope = value.toMap();
            if (!writeTagAssignment(scope,ids.value(scope.value("tagId").toString()),true)) return false;
        }
    }
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
    publishAnnotations();
    if (withTags) publishTags();
    updateAnnotations(); emit collectionChanged();
    emit passageChanged(); emit selectionChanged(); emit readingPositionChanged(); emit navigationChanged();
    clearError(); setStatus(QString("Restored %1 annotations from backup.").arg(rows.size())); return true;
}
