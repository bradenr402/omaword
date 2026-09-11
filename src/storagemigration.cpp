#include "storagemigration.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/fs.h>

namespace {
bool present(const QString &path)
{
    return QFileInfo::exists(path) || QFileInfo(path).isSymLink();
}

bool regular(const QString &path)
{
    struct stat info;
    return ::lstat(QFile::encodeName(path).constData(), &info) == 0
        && S_ISREG(info.st_mode) && info.st_nlink == 1 && info.st_uid == ::geteuid();
}

bool directory(const QString &path)
{
    struct stat info;
    return ::lstat(QFile::encodeName(path).constData(), &info) == 0
        && S_ISDIR(info.st_mode) && info.st_uid == ::geteuid();
}

QStringList entries(const QString &path)
{
    return QDir(path).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
}

// lstat records the entry itself, including dangling symlinks. Directory trees
// move intact; never traverse unknown user content or rewrite link targets.
QJsonObject identity(const QString &path)
{
    struct stat info;
    if (::lstat(QFile::encodeName(path).constData(), &info) != 0) return {};
    return {{"device", QString::number(info.st_dev)}, {"inode", QString::number(info.st_ino)},
            {"type", int(info.st_mode & S_IFMT)}};
}

bool renameEntry(const QString &source, const QString &target)
{
    // Atomic no-replace is essential for directories and dangling symlinks.
    // Do not fall back to a copy/unlink or an overwriting POSIX rename.
    return ::syscall(SYS_renameat2, AT_FDCWD, QFile::encodeName(source).constData(),
                     AT_FDCWD, QFile::encodeName(target).constData(), RENAME_NOREPLACE) == 0;
}

bool syncPath(const QString &path)
{
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
}

QString digest(const QString &path)
{
    if (!regular(path)) return {};
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)) return {};
    return QString::fromLatin1(hash.result().toHex());
}

bool databaseName(const QString &name)
{
    static const QRegularExpression generation(QStringLiteral(
        "^bible-[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\.sqlite$"));
    return name == "personal.sqlite" || name == "bible.sqlite" || generation.match(name).hasMatch();
}

bool ownedName(QString name)
{
    for (const auto &suffix : {QString("-wal"), QString("-shm"), QString("-journal")})
        if (name.endsWith(suffix)) { name.chop(suffix.size()); break; }
    return databaseName(name);
}

QStringList ownedFiles(const QString &path)
{
    QStringList result;
    for (const auto &entry : QDir(path).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot))
        if (ownedName(entry)) result.append(entry);
    return result;
}

// A read-only SQLite connection sees committed WAL pages. VACUUM INTO creates a
// standalone snapshot. Use a private copy of the full bundle so even SQLite's
// shared-memory bookkeeping cannot alter historical files before commit.
bool snapshot(const QString &source, const QString &target, QString *error)
{
    const auto connection = QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool ok = false;
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(source);
        db.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=5000");
        if (db.open()) {
            QSqlQuery query(db);
            query.prepare("VACUUM INTO ?");
            query.addBindValue(target);
            ok = query.exec();
            if (!ok) *error = query.lastError().text();
        } else *error = db.lastError().text();
    }
    QSqlDatabase::removeDatabase(connection);
    return ok && syncPath(target);
}

bool validateStore(const QString &stage, QString *error)
{
    if (!present(stage + "/personal.sqlite")) return true;
    const auto connection = QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool ok = false;
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(stage + "/personal.sqlite");
        db.setConnectOptions("QSQLITE_OPEN_READONLY");
        if (db.open()) {
            QSqlQuery query(db);
            ok = query.exec("PRAGMA integrity_check") && query.next() && query.value(0).toString() == "ok";
            if (ok && db.tables().contains("installed")) {
                ok = query.exec("SELECT path FROM installed");
                while (ok && query.next()) {
                    const auto path = query.value(0).toString();
                    ok = databaseName(path) && path != "personal.sqlite" && regular(stage + '/' + path);
                }
                ok = ok && !query.lastError().isValid();
            }
        }
        if (!ok) *error = "The historical personal database or its library paths are invalid or incomplete.";
    }
    QSqlDatabase::removeDatabase(connection);
    return ok;
}
}

StorageMigration::StorageMigration(const QString &dataDirectory, const QString &runtimeDirectory)
    : m_data(QDir::cleanPath(dataDirectory)),
      m_old(QDir(QFileInfo(m_data).absolutePath()).filePath("omabible")), m_runtime(runtimeDirectory)
{
}

StorageMigration::~StorageMigration()
{
    const bool locked = m_legacyLock && m_legacyLock->isLocked();
    m_legacyLock.reset();
    // The caller has already closed the backend. rmdir is non-recursive and
    // refuses a new lock/file created by a racing old executable after unlock.
    struct stat info;
    if (locked && m_removeEmptyDirectory
        && ::lstat(QFile::encodeName(m_old).constData(), &info) == 0 && S_ISDIR(info.st_mode)
        && quint64(info.st_dev) == m_oldDevice && quint64(info.st_ino) == m_oldInode
        && ::rmdir(QFile::encodeName(m_old).constData()) == 0)
        syncPath(QFileInfo(m_old).absolutePath());
}

bool StorageMigration::archiveRemaining()
{
    auto fail = [this](const QString &reason) {
        m_error = "Cannot archive remaining historical data: " + reason + "\nSource: " + m_old
            + "\nArchive: " + m_data + "/.storage-migration/remaining"
              "\nBoth entries are preserved on conflict. Resolve the conflict and retry.";
        return false;
    };
    auto names = entries(m_old);
    names.removeAll("omabible.lock");
    for (const auto &name : names)
        if (ownedName(name)) return fail("Historical database data reappeared: " + name);
    const auto work = m_data + "/.storage-migration";
    const auto archive = work + "/remaining";
    const auto journalPath = work + "/remaining.json";
    for (const auto &path : {work, archive})
        if (present(path) && !directory(path)) return fail("Unsafe archive directory: " + path);
    QJsonObject files;
    if (present(journalPath)) {
        if (!regular(journalPath)) return fail("Unsafe remaining-files journal.");
        QFile file(journalPath);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) return fail("Cannot read remaining-files journal.");
        QJsonParseError error;
        const auto journal = QJsonDocument::fromJson(file.readAll(), &error).object();
        if (error.error != QJsonParseError::NoError || journal.value("version") != QJsonValue(1)
            || journal.value("source") != m_old || !journal.value("files").isObject())
            return fail("Invalid remaining-files journal.");
        files = journal.value("files").toObject();
    }
    for (auto it = files.begin(); it != files.end(); ++it) {
        const auto name = it.key();
        const auto id = it.value().toObject();
        if (name.isEmpty() || name == "." || name == ".." || name.contains('/') || name.contains(QChar::Null)
            || name == "omabible.lock" || ownedName(name) || id.size() != 3
            || !id.value("device").isString() || !id.value("inode").isString() || !id.value("type").isDouble())
            return fail("Invalid remaining-files journal entry.");
    }
    for (const auto &name : names) {
        if (!files.contains(name)) {
            const auto id = identity(m_old + '/' + name);
            if (id.isEmpty()) return fail("Cannot inspect source entry: " + name);
            files.insert(name, id);
        }
    }
    // Preflight the entire batch before any move. A missing source is resumable
    // only if the destination has the recorded inode/type from the same rename.
    for (auto it = files.begin(); it != files.end(); ++it) {
        const auto source = m_old + '/' + it.key(), target = archive + '/' + it.key();
        if (present(source)) {
            if (present(target) || identity(source) != it.value().toObject()) return fail("Archive conflict: " + it.key());
        } else if (identity(target) != it.value().toObject()) return fail("Missing or replaced archived entry: " + it.key());
    }
    if (!files.isEmpty()) {
        if (!QDir().mkpath(archive) || !syncPath(work) || !syncPath(m_data)) return fail("Cannot create archive directory.");
        QSaveFile journal(journalPath);
        const auto bytes = QJsonDocument(QJsonObject{{"version", 1}, {"source", m_old}, {"files", files}}).toJson();
        if (bytes.size() > 4 * 1024 * 1024 || !journal.open(QIODevice::WriteOnly)
            || journal.write(bytes) != bytes.size() || !journal.commit() || !syncPath(work))
            return fail("Cannot commit remaining-files journal.");
        for (auto it = files.begin(); it != files.end(); ++it) {
            const auto source = m_old + '/' + it.key(), target = archive + '/' + it.key();
            if (present(source) && (identity(source) != it.value().toObject() || !renameEntry(source, target)))
                return fail("Cannot move entry: " + it.key());
            if (identity(target) != it.value().toObject() || !syncPath(archive) || !syncPath(m_old))
                return fail("Cannot verify/sync archived entry: " + it.key());
        }
    }
    names = entries(m_old);
    names.removeAll("omabible.lock");
    if (!names.isEmpty()) return fail("Historical entries appeared during archival; retry.");
    m_removeEmptyDirectory = true;
    return true;
}

bool StorageMigration::run()
{
    m_error.clear();
    m_removeEmptyDirectory = false;
    auto fail = [this](const QString &reason) {
        m_error = "Cannot migrate historical OmaBible data to OmaWord: " + reason
            + "\nSource: " + m_old + "\nDestination: " + m_data
            + "\nNo existing destination data is overwritten. Close older instances and retry; "
              "if both stores contain data, preserve both and resolve the conflict first.";
        return false;
    };
    if (!directory(m_data) || m_data == m_old || QFileInfo(m_data).isSymLink())
        return fail("Unsafe destination directory.");
    if (!present(m_old)) return true;
    if (!directory(m_old) || QFileInfo(m_old).canonicalFilePath() == QFileInfo(m_data).canonicalFilePath())
        return fail("Unsafe historical directory.");

    const auto lockPath = m_old + "/omabible.lock";
    if (present(lockPath) && !regular(lockPath)) return fail("Unsafe historical lock file.");
    if (!m_legacyLock || !m_legacyLock->isLocked()) {
        m_legacyLock = std::make_unique<QLockFile>(lockPath);
        m_legacyLock->setStaleLockTime(0);
        if (!m_legacyLock->tryLock()) return fail("OmaBible is still running, or its lock cannot be acquired. Please close OmaBible.");
    }
    struct stat oldInfo;
    if (::lstat(QFile::encodeName(m_old).constData(), &oldInfo) != 0) return fail("Cannot inspect historical directory.");
    m_oldDevice = oldInfo.st_dev;
    m_oldInode = oldInfo.st_ino;
    // Match the historical endpoint exactly; never unlink another scope's socket.
    const auto hash = QCryptographicHash::hash((QFileInfo(m_old).canonicalFilePath() + "/omabible").toUtf8(),
                                               QCryptographicHash::Sha256).toHex().left(32);
    if (!m_runtime.isEmpty()) QLocalServer::removeServer(QDir(m_runtime).filePath("omabible-" + QString::fromLatin1(hash) + ".socket"));

    const auto work = m_data + "/.storage-migration";
    const auto journalPath = work + "/journal.json";
    const auto stage = work + "/staged";
    const auto input = work + "/input";
    const auto archive = work + "/originals";
    QJsonObject journal;
    auto save = [&] {
        QSaveFile file(journalPath);
        const auto bytes = QJsonDocument(journal).toJson();
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size()
            && file.commit() && syncPath(work);
    };
    for (const auto &path : {work, stage, input, archive})
        if (present(path) && !directory(path)) return fail("Unsafe migration workspace: " + path);
    if (present(journalPath)) {
        if (!regular(journalPath)) return fail("Unsafe migration journal.");
        QFile file(journalPath);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) return fail("Cannot read migration journal.");
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(file.readAll(), &error);
        journal = document.object();
        if (error.error != QJsonParseError::NoError || journal.value("version") != QJsonValue(1)
            || journal.value("source") != m_old || !journal.value("files").isObject()
            || !QStringList{"copying", "publishing", "archiving", "done"}.contains(journal.value("phase").toString()))
            return fail("Invalid migration journal; preserve it for recovery.");
    } else {
        const auto files = ownedFiles(m_old);
        if (files.isEmpty()) return archiveRemaining();
        if (!ownedFiles(m_data).isEmpty()) return fail("Both storage directories contain application data.");
        if (present(work) && !QDir(work).isEmpty()) return fail("Unrecognized migration workspace.");
        QJsonObject originals;
        for (const auto &name : files) {
            const auto hash = digest(m_old + '/' + name);
            if (hash.isEmpty()) return fail("Unsafe or unreadable source file: " + name);
            originals.insert(name, hash);
        }
        for (const auto &name : files) {
            if (!databaseName(name)) {
                bool found = false;
                for (const auto &suffix : {QString("-wal"), QString("-shm"), QString("-journal")})
                    if (name.endsWith(suffix) && files.contains(name.left(name.size() - suffix.size()))) found = true;
                if (!found) return fail("Orphan SQLite sidecar: " + name);
            }
        }
        if (!QDir().mkpath(work)) return fail("Cannot create migration workspace.");
        journal = {{"version", 1}, {"source", m_old}, {"phase", "copying"}, {"files", originals}};
        if (!save() || !syncPath(m_data)) return fail("Cannot save migration journal.");
    }

    const auto originals = journal.value("files").toObject();
    static const QRegularExpression sha256("^[0-9a-f]{64}$");
    for (auto it = originals.begin(); it != originals.end(); ++it)
        if (!ownedName(it.key()) || !sha256.match(it.value().toString()).hasMatch()) return fail("Invalid journal file entry.");
    const auto phase = journal.value("phase").toString();
    if (phase == "done") {
        if (!ownedFiles(m_old).isEmpty()) return fail("Historical data was recreated after migration; automatic merging is unsafe.");
        return archiveRemaining();
    }
    // Until commit, a restart must not publish a snapshot of a changed source.
    if (phase == "copying" || phase == "publishing") {
        if (ownedFiles(m_old) != originals.keys()) return fail("Historical file set changed during migration.");
        for (auto it = originals.begin(); it != originals.end(); ++it)
            if (digest(m_old + '/' + it.key()) != it.value().toString()) return fail("Historical data changed during migration: " + it.key());
    }
    if (!QDir().mkpath(stage) || !QDir().mkpath(input) || !QDir().mkpath(archive)) return fail("Cannot create staging/archive directories.");
    if (phase == "copying") {
        if (!ownedFiles(m_data).isEmpty()) return fail("Destination data appeared during migration.");
        for (const auto &name : QDir(input).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot))
            if (!ownedName(name) || !regular(input + '/' + name) || !QFile::remove(input + '/' + name))
                return fail("Cannot reset private SQLite input: " + name);
        for (auto it = originals.begin(); it != originals.end(); ++it)
            if (!QFile::copy(m_old + '/' + it.key(), input + '/' + it.key())
                || digest(input + '/' + it.key()) != it.value().toString()) return fail("Cannot copy historical bundle: " + it.key());
        QJsonObject snapshots;
        for (auto it = originals.begin(); it != originals.end(); ++it) {
            const auto name = it.key();
            if (!databaseName(name)) continue;
            const auto target = stage + '/' + name;
            if (present(target) && (!regular(target) || !QFile::remove(target))) return fail("Cannot reset incomplete snapshot: " + name);
            QString error;
            // Text generations are immutable; preserve their exact bytes unless
            // SQLite sidecars require a consolidated snapshot.
            const bool sqlite = name == "personal.sqlite" || originals.contains(name + "-wal") || originals.contains(name + "-journal");
            if (!(sqlite ? snapshot(input + '/' + name, target, &error)
                         : QFile::copy(m_old + '/' + name, target) && syncPath(target)))
                return fail("Cannot stage " + name + ": " + error);
            const auto hash = digest(target);
            if (hash.isEmpty()) return fail("Cannot verify staged file: " + name);
            snapshots.insert(name, hash);
        }
        QString error;
        if (!validateStore(stage, &error)) return fail(error);
        if (!syncPath(stage)) return fail("Cannot sync staged data.");
        journal.insert("snapshots", snapshots);
        journal.insert("phase", "publishing");
        if (!save()) return fail("Cannot commit snapshot journal.");
    }
    const auto snapshots = journal.value("snapshots").toObject();
    QStringList databases;
    for (auto it = originals.begin(); it != originals.end(); ++it)
        if (databaseName(it.key())) databases.append(it.key());
    if (snapshots.keys() != databases) return fail("Incomplete snapshot journal.");
    for (auto it = snapshots.begin(); it != snapshots.end(); ++it)
        if (!sha256.match(it.value().toString()).hasMatch()) return fail("Invalid snapshot digest.");
    if (journal.value("phase") == "publishing") {
        // Recheck every destination before doing any further publishing.
        for (const auto &name : ownedFiles(m_data))
            if (!snapshots.contains(name) || digest(m_data + '/' + name) != snapshots.value(name).toString()
                || present(stage + '/' + name)) return fail("Destination conflict: " + name);
        databases.removeAll("personal.sqlite");
        if (snapshots.contains("personal.sqlite")) databases.append("personal.sqlite");
        for (const auto &name : databases) {
            const auto target = m_data + '/' + name;
            if (present(target)) continue; // Verified rename completed before interruption.
            if (digest(stage + '/' + name) != snapshots.value(name).toString()
                || !QFile::rename(stage + '/' + name, target) || !syncPath(m_data) || !syncPath(stage))
                return fail("Cannot publish snapshot: " + name);
        }
        journal.insert("phase", "archiving");
        if (!save()) return fail("Cannot save migration commit marker.");
    }
    // Commit is durable. Preserve the original SQLite files AND sidecars as an
    // untouched recovery bundle, removing their active historical names. Each
    // rename can be resumed independently after interruption.
    for (auto it = snapshots.begin(); it != snapshots.end(); ++it)
        if (digest(m_data + '/' + it.key()) != it.value().toString()) return fail("Committed snapshot changed before archival: " + it.key());
    for (auto it = originals.begin(); it != originals.end(); ++it) {
        const auto source = m_old + '/' + it.key(), target = archive + '/' + it.key();
        if (present(target)) {
            if (present(source) || digest(target) != it.value().toString()) return fail("Archive conflict: " + it.key());
        } else if (digest(source) != it.value().toString() || !QFile::rename(source, target)
                   || !syncPath(m_old) || !syncPath(archive)) return fail("Cannot archive historical file: " + it.key());
    }
    journal.insert("phase", "done");
    if (!save()) return fail("Cannot finish migration journal.");
    // This private input is expendable after durable archival; never recursively
    // delete an unrecognized tree. The journal and originals remain for recovery.
    for (const auto &name : ownedFiles(input))
        if (regular(input + '/' + name)) QFile::remove(input + '/' + name);
    QDir().rmdir(input);
    QDir().rmdir(stage);
    return archiveRemaining();
}
