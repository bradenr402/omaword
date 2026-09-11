#include "storagemigration.h"
#include <QtTest/QTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QProcess>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTextStream>
#include <unistd.h>
#include <sys/stat.h>

namespace {
bool write(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray read(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
QString hash(const QString &path)
{
    return QString::fromLatin1(QCryptographicHash::hash(read(path), QCryptographicHash::Sha256).toHex());
}
bool makeStore(const QString &path)
{
    bool ok;
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", "fixture");
        db.setDatabaseName(path);
        QSqlQuery q(db);
        ok = db.open();
        q = QSqlQuery(db);
        ok = ok && q.exec("CREATE TABLE annotations(note TEXT)")
            && q.exec("INSERT INTO annotations VALUES('preserve notes')")
            && q.exec("CREATE TABLE reading(position REAL)") && q.exec("INSERT INTO reading VALUES(0.63)")
            && q.exec("CREATE TABLE preferences(key TEXT,value TEXT)") && q.exec("INSERT INTO preferences VALUES('focusMode','1')")
            && q.exec("CREATE TABLE installed(path TEXT)") && q.exec("INSERT INTO installed VALUES('bible.sqlite')")
            && q.exec("CREATE TABLE library(active TEXT)") && q.exec("INSERT INTO library VALUES('net-2.1')")
            && q.exec("PRAGMA user_version=4");
    }
    QSqlDatabase::removeDatabase("fixture");
    return ok;
}
struct Fixture {
    QTemporaryDir temp;
    QString old = temp.filePath("omabible"), data = temp.filePath("omaword");
    Fixture() { QDir().mkpath(old); QDir().mkpath(data + "/install-backups/previous"); }
    bool populate() {
        return makeStore(old + "/personal.sqlite") && write(old + "/bible.sqlite", "immutable text")
            && write(old + "/unrelated.txt", "keep") && write(data + "/install-backups/previous/original", "omabible archived bytes");
    }
};
}

class MigrationTest : public QObject
{
    Q_OBJECT
private slots:
    void freshAndEmpty()
    {
        Fixture f;
        QVERIFY(QDir().rmdir(f.old));
        StorageMigration migration(f.data, f.temp.path());
        QVERIFY2(migration.run(), qPrintable(migration.errorString()));
        QVERIFY(!QFile::exists(f.old));
        QVERIFY(QDir().mkpath(f.old));
        QVERIFY(migration.run());
        QVERIFY(!QFile::exists(f.data + "/personal.sqlite"));
    }

    void mergeAndIdempotence()
    {
        Fixture f; QVERIFY(f.populate());
        const auto original = read(f.old + "/personal.sqlite");
        {
            QLockFile primary(f.data + "/omaword.lock"); QVERIFY(primary.tryLock());
            StorageMigration migration(f.data, f.temp.path());
            QVERIFY2(migration.run(), qPrintable(migration.errorString()));
            QLockFile old(f.old + "/omabible.lock"); QVERIFY(!old.tryLock());
            QCOMPARE(read(f.data + "/.storage-migration/originals/personal.sqlite"), original);
            QCOMPARE(read(f.data + "/bible.sqlite"), QByteArray("immutable text"));
            QCOMPARE(read(f.data + "/install-backups/previous/original"), QByteArray("omabible archived bytes"));
            QCOMPARE(read(f.data + "/.storage-migration/remaining/unrelated.txt"), QByteArray("keep"));
            QVERIFY(!QFile::exists(f.old + "/personal.sqlite"));
            QVERIFY(!QFile::exists(f.old + "/bible.sqlite"));
        }
        QVERIFY(!QFile::exists(f.old));
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "verify");
            db.setDatabaseName(f.data + "/personal.sqlite"); QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("SELECT note,position,value,active FROM annotations,reading,preferences,library")); QVERIFY(q.next());
            QCOMPARE(q.value(0).toString(), QString("preserve notes")); QCOMPARE(q.value(1).toDouble(), 0.63);
            QCOMPARE(q.value(2).toString(), QString("1")); QCOMPARE(q.value(3).toString(), QString("net-2.1"));
            QVERIFY(q.exec("UPDATE annotations SET note='new app changes'"));
        }
        QSqlDatabase::removeDatabase("verify");
        StorageMigration again(f.data, f.temp.path());
        QVERIFY2(again.run(), qPrintable(again.errorString()));
    }

    void remainingFilesAndShutdown_data()
    {
        QTest::addColumn<bool>("withStore");
        QTest::newRow("with-databases") << true;
        QTest::newRow("exports-only") << false;
    }
    void remainingFilesAndShutdown()
    {
        QFETCH(bool, withStore);
        Fixture f;
        if (withStore) QVERIFY(f.populate());
        QVERIFY(QDir().mkpath(f.old + "/exports/nested"));
        QVERIFY(write(f.old + "/exports/nested/omabible-notes.md", "historical export bytes"));
        QVERIFY(write(f.old + "/abandoned.temp", "unfinished user work"));
        QVERIFY(write(f.old + "/.hidden", "hidden bytes"));
        QVERIFY(write(f.data + "/abandoned.temp", "active new file"));
        const auto external = f.temp.filePath("external");
        QVERIFY(QDir().mkpath(external)); QVERIFY(write(external + "/keep", "do not follow"));
        QVERIFY(QFile::link(external, f.old + "/external-link"));
        QVERIFY(QFile::link(f.temp.filePath("missing"), f.old + "/dangling"));
        QVERIFY(QFile::link(f.temp.filePath("missing-nested"), f.old + "/exports/nested/link"));
        const auto archive = f.data + "/.storage-migration/remaining";
        {
            StorageMigration migration(f.data, f.temp.path());
            QVERIFY2(migration.run(), qPrintable(migration.errorString()));
            QCOMPARE(QDir(f.old).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), QStringList{"omabible.lock"});
            QLockFile old(f.old + "/omabible.lock"); QVERIFY(!old.tryLock());
            QCOMPARE(read(archive + "/exports/nested/omabible-notes.md"), QByteArray("historical export bytes"));
            QCOMPARE(read(archive + "/abandoned.temp"), QByteArray("unfinished user work"));
            QCOMPARE(read(archive + "/.hidden"), QByteArray("hidden bytes"));
            QCOMPARE(read(f.data + "/abandoned.temp"), QByteArray("active new file"));
            QVERIFY(QFileInfo(archive + "/external-link").isSymLink());
            QCOMPARE(QFileInfo(archive + "/external-link").symLinkTarget(), external);
            QVERIFY(QFileInfo(archive + "/dangling").isSymLink());
            QCOMPARE(QFileInfo(archive + "/dangling").symLinkTarget(), f.temp.filePath("missing"));
            QVERIFY(QFileInfo(archive + "/exports/nested/link").isSymLink());
            QCOMPARE(read(external + "/keep"), QByteArray("do not follow"));
            // Repeated calls on the same object keep the historical lock held.
            QVERIFY2(migration.run(), qPrintable(migration.errorString()));
            QVERIFY(!old.tryLock());
        }
        QVERIFY(!QFileInfo::exists(f.old));
        StorageMigration next(f.data, f.temp.path()); QVERIFY(next.run());
        QVERIFY(!QFileInfo::exists(f.old));
    }

    void shutdownPreservesNewEntries()
    {
        Fixture f;
        {
            StorageMigration migration(f.data, f.temp.path()); QVERIFY(migration.run());
            QVERIFY(write(f.old + "/late-user-file", "keep"));
        }
        QCOMPARE(read(f.old + "/late-user-file"), QByteArray("keep"));
        QVERIFY(!QFileInfo::exists(f.old + "/omabible.lock"));
        {
            StorageMigration next(f.data, f.temp.path()); QVERIFY(next.run());
        }
        QVERIFY(!QFileInfo::exists(f.old));
        QCOMPARE(read(f.data + "/.storage-migration/remaining/late-user-file"), QByteArray("keep"));
    }

    void remainingConflict_data()
    {
        QTest::addColumn<QString>("kind");
        for (const auto &kind : {"file", "directory", "dangling", "archive-symlink", "journal-symlink"})
            QTest::newRow(kind) << QString(kind);
    }
    void remainingConflict()
    {
        QFETCH(QString, kind);
        Fixture f; QVERIFY(f.populate());
        // Commit the database phase first, as on an upgrade of the migrator.
        {
            StorageMigration migration(f.data, f.temp.path()); QVERIFY(migration.run());
        }
        QVERIFY(QDir().mkpath(f.old));
        QVERIFY(write(f.old + "/a-first", "must not move before preflight"));
        QVERIFY(write(f.old + "/collision", "source"));
        const auto work = f.data + "/.storage-migration", archive = work + "/remaining";
        if (kind == "file") QVERIFY(write(archive + "/collision", "destination"));
        if (kind == "directory") QVERIFY(QDir().mkpath(archive + "/collision"));
        if (kind == "dangling") QVERIFY(QFile::link(f.temp.filePath("absent"), archive + "/collision"));
        if (kind == "archive-symlink") {
            QVERIFY(QDir().rename(archive, work + "/saved-remaining"));
            QVERIFY(QFile::link(work + "/saved-remaining", archive));
        }
        if (kind == "journal-symlink") {
            QVERIFY(QFile::rename(work + "/remaining.json", work + "/saved-journal"));
            QVERIFY(QFile::link(work + "/saved-journal", work + "/remaining.json"));
        }
        {
            StorageMigration migration(f.data, f.temp.path());
            QVERIFY(!migration.run()); QVERIFY(!migration.errorString().isEmpty());
            QCOMPARE(read(f.old + "/collision"), QByteArray("source"));
            QCOMPARE(read(f.old + "/a-first"), QByteArray("must not move before preflight"));
            if (kind == "file") QCOMPARE(read(archive + "/collision"), QByteArray("destination"));
            if (kind == "directory") QVERIFY(QFileInfo(archive + "/collision").isDir());
            if (kind == "dangling") QVERIFY(QFileInfo(archive + "/collision").isSymLink());
        }
        QVERIFY(QFileInfo::exists(f.old));
    }

    void remainingResume_data()
    {
        QTest::addColumn<QString>("kind");
        for (const auto &kind : {"pending", "partial", "all-moved", "changed-destination", "recreated-source", "unsafe-name"})
            QTest::newRow(kind) << QString(kind);
    }
    void remainingResume()
    {
        QFETCH(QString, kind);
        Fixture f;
        QVERIFY(QDir().mkpath(f.old + "/exports"));
        QVERIFY(write(f.old + "/exports/notes.md", "export"));
        QVERIFY(QFile::link(f.temp.filePath("missing"), f.old + "/link"));
        QJsonObject files;
        for (const auto &name : {QString("exports"), QString("link")}) {
            struct stat info;
            QCOMPARE(::lstat(QFile::encodeName(f.old + '/' + name).constData(), &info), 0);
            files.insert(name, QJsonObject{{"device", QString::number(info.st_dev)}, {"inode", QString::number(info.st_ino)},
                                          {"type", int(info.st_mode & S_IFMT)}});
        }
        const auto work = f.data + "/.storage-migration", archive = work + "/remaining";
        QVERIFY(QDir().mkpath(archive));
        if (kind != "pending") QVERIFY(QDir().rename(f.old + "/exports", archive + "/exports"));
        if (kind == "all-moved") QCOMPARE(::rename(QFile::encodeName(f.old + "/link").constData(), QFile::encodeName(archive + "/link").constData()), 0);
        if (kind == "changed-destination") {
            QVERIFY(QDir().rename(archive + "/exports", f.temp.filePath("held")));
            QVERIFY(QDir().mkpath(archive + "/exports"));
        }
        if (kind == "recreated-source") QVERIFY(QDir().mkpath(f.old + "/exports"));
        if (kind == "unsafe-name") files.insert("../outside", files.value("exports"));
        QVERIFY(write(work + "/remaining.json", QJsonDocument(QJsonObject{{"version", 1}, {"source", f.old}, {"files", files}}).toJson()));
        const bool success = kind == "pending" || kind == "partial" || kind == "all-moved";
        {
            StorageMigration migration(f.data, f.temp.path());
            QCOMPARE(migration.run(), success);
            if (success) {
                QCOMPARE(read(archive + "/exports/notes.md"), QByteArray("export"));
                QVERIFY(QFileInfo(archive + "/link").isSymLink());
            } else QVERIFY(!migration.errorString().isEmpty());
        }
        QCOMPARE(QFileInfo::exists(f.old), !success);
    }

    void liveOldLock()
    {
        Fixture f; QVERIFY(f.populate());
        QLockFile old(f.old + "/omabible.lock"); old.setStaleLockTime(0); QVERIFY(old.tryLock());
        StorageMigration migration(f.data, f.temp.path());
        QVERIFY(!migration.run()); QVERIFY(migration.errorString().contains("Please close OmaBible"));
        QVERIFY(!QFile::exists(f.data + "/personal.sqlite")); QVERIFY(old.isLocked());
    }

    void interruptedConflict_data()
    {
        QTest::addColumn<QString>("kind");
        for (const auto &kind : {"source-changed", "destination-changed", "forged-path", "committed-store-missing", "old-store-recreated"})
            QTest::newRow(kind) << QString(kind);
    }
    void interruptedConflict()
    {
        QFETCH(QString, kind);
        Fixture f; QVERIFY(f.populate());
        const auto work = f.data + "/.storage-migration", stage = work + "/staged";
        QVERIFY(QDir().mkpath(stage));
        QJsonObject files{{"bible.sqlite", hash(f.old + "/bible.sqlite")}, {"personal.sqlite", hash(f.old + "/personal.sqlite")}};
        QJsonObject journal{{"version", 1}, {"source", f.old}, {"files", files}, {"snapshots", files}, {"phase", "publishing"}};
        QVERIFY(QFile::copy(f.old + "/bible.sqlite", f.data + "/bible.sqlite"));
        QVERIFY(QFile::copy(f.old + "/personal.sqlite", stage + "/personal.sqlite"));
        if (kind == "source-changed") QVERIFY(write(f.old + "/bible.sqlite", "changed source"));
        if (kind == "destination-changed") QVERIFY(write(f.data + "/bible.sqlite", "destination conflict"));
        if (kind == "forged-path") { files.insert("../outside", hash(f.old + "/bible.sqlite")); journal["files"] = files; }
        if (kind == "committed-store-missing") journal["phase"] = "archiving";
        if (kind == "old-store-recreated") journal["phase"] = "done";
        QVERIFY(write(work + "/journal.json", QJsonDocument(journal).toJson()));
        const auto source = read(f.old + "/bible.sqlite"), destination = read(f.data + "/bible.sqlite");
        StorageMigration migration(f.data, f.temp.path());
        QVERIFY(!migration.run()); QVERIFY(!migration.errorString().isEmpty());
        QCOMPARE(read(f.old + "/bible.sqlite"), source); QCOMPARE(read(f.data + "/bible.sqlite"), destination);
        QVERIFY(QFile::exists(f.old + "/personal.sqlite"));
    }

    void preservesHistoricalSchemas_data()
    {
        QTest::addColumn<int>("version");
        for (int version = 1; version <= 4; ++version) QTest::newRow(qPrintable(QString::number(version))) << version;
    }
    void preservesHistoricalSchemas()
    {
        QFETCH(int, version);
        Fixture f; QVERIFY(f.populate());
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "schema");
            db.setDatabaseName(f.old + "/personal.sqlite"); QVERIFY(db.open());
            QSqlQuery q(db); QVERIFY(q.exec(QString("PRAGMA user_version=%1").arg(version)));
            if (version < 3) QVERIFY(q.exec("DROP TABLE installed"));
        }
        QSqlDatabase::removeDatabase("schema");
        const auto generation = QString("bible-00000000-0000-0000-0000-000000000000.sqlite");
        QVERIFY(write(f.old + '/' + generation, "second immutable generation"));
        StorageMigration migration(f.data, f.temp.path());
        QVERIFY2(migration.run(), qPrintable(migration.errorString()));
        QCOMPARE(read(f.data + '/' + generation), QByteArray("second immutable generation"));
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "schema");
            db.setDatabaseName(f.data + "/personal.sqlite"); QVERIFY(db.open());
            QSqlQuery q(db); QVERIFY(q.exec("PRAGMA user_version")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(), version);
        }
        QSqlDatabase::removeDatabase("schema");
    }

    void conflictAndUnsafePaths_data()
    {
        QTest::addColumn<QString>("kind");
        for (const auto &kind : {"both-stores", "text-collision", "source-symlink", "source-hardlink", "sidecar-symlink",
                                 "directory-symlink", "stage-symlink", "registry-traversal", "orphan-wal", "corrupt-store"})
            QTest::newRow(kind) << QString(kind);
    }
    void conflictAndUnsafePaths()
    {
        QFETCH(QString, kind);
        Fixture f; QVERIFY(f.populate());
        if (kind == "both-stores") QVERIFY(write(f.data + "/personal.sqlite", "destination notes"));
        if (kind == "text-collision") QVERIFY(write(f.data + "/bible.sqlite", "destination text"));
        if (kind == "source-symlink" || kind == "source-hardlink") {
            QVERIFY(QFile::rename(f.old + "/personal.sqlite", f.temp.filePath("external")));
            if (kind == "source-symlink") QVERIFY(QFile::link(f.temp.filePath("external"), f.old + "/personal.sqlite"));
            else QCOMPARE(::link(QFile::encodeName(f.temp.filePath("external")).constData(), QFile::encodeName(f.old + "/personal.sqlite").constData()), 0);
        }
        if (kind == "sidecar-symlink") QVERIFY(QFile::link(f.temp.filePath("missing"), f.old + "/personal.sqlite-wal"));
        if (kind == "directory-symlink") {
            QVERIFY(QDir().rename(f.old, f.temp.filePath("external")));
            QVERIFY(QFile::link(f.temp.filePath("external"), f.old));
        }
        if (kind == "stage-symlink") QVERIFY(QFile::link(f.old, f.data + "/.storage-migration"));
        if (kind == "orphan-wal") QVERIFY(write(f.old + "/bible-00000000-0000-0000-0000-000000000000.sqlite-wal", "orphan"));
        if (kind == "corrupt-store") QVERIFY(write(f.old + "/personal.sqlite", "not sqlite"));
        if (kind == "registry-traversal") {
            {
                auto db = QSqlDatabase::addDatabase("QSQLITE", "bad-path");
                db.setDatabaseName(f.old + "/personal.sqlite"); QVERIFY(db.open());
                QSqlQuery q(db); QVERIFY(q.exec("UPDATE installed SET path='../outside.sqlite'"));
            }
            QSqlDatabase::removeDatabase("bad-path");
        }
        const auto old = read(f.old + "/personal.sqlite"), destination = read(f.data + "/personal.sqlite");
        StorageMigration migration(f.data, f.temp.path());
        QVERIFY(!migration.run()); QVERIFY(!migration.errorString().isEmpty());
        QCOMPARE(read(f.old + "/personal.sqlite"), old); QCOMPARE(read(f.data + "/personal.sqlite"), destination);
    }

    void interruptedMigration_data()
    {
        QTest::addColumn<QString>("phase");
        QTest::newRow("incomplete-copy") << QString("copying");
        QTest::newRow("text-published") << QString("publishing");
        QTest::newRow("commit-before-marker") << QString("commit-before-marker");
        QTest::newRow("partial-archive") << QString("archiving");
    }
    void interruptedMigration()
    {
        QFETCH(QString, phase);
        Fixture f; QVERIFY(f.populate());
        const auto work = f.data + "/.storage-migration", stage = work + "/staged", archive = work + "/originals";
        QVERIFY(QDir().mkpath(stage)); QVERIFY(QDir().mkpath(archive));
        QJsonObject files{{"bible.sqlite", hash(f.old + "/bible.sqlite")}, {"personal.sqlite", hash(f.old + "/personal.sqlite")}};
        QJsonObject journal{{"version", 1}, {"source", f.old}, {"files", files}, {"snapshots", files},
                            {"phase", phase == "commit-before-marker" ? "publishing" : phase}};
        if (phase == "copying") QVERIFY(write(stage + "/personal.sqlite", "partial snapshot"));
        else {
            QVERIFY(QFile::copy(f.old + "/bible.sqlite", f.data + "/bible.sqlite"));
            QVERIFY(QFile::copy(f.old + "/personal.sqlite", (phase == "publishing" ? stage : f.data) + "/personal.sqlite"));
            if (phase == "archiving") QVERIFY(QFile::rename(f.old + "/bible.sqlite", archive + "/bible.sqlite"));
        }
        QVERIFY(write(work + "/journal.json", QJsonDocument(journal).toJson()));
        StorageMigration migration(f.data, f.temp.path());
        QVERIFY2(migration.run(), qPrintable(migration.errorString()));
        QCOMPARE(read(f.data + "/bible.sqlite"), QByteArray("immutable text"));
        QVERIFY(!QFile::exists(f.old + "/personal.sqlite"));
        QCOMPARE(QJsonDocument::fromJson(read(work + "/journal.json")).object().value("phase").toString(), QString("done"));
    }

    void walAndStaleRuntimeRecovery()
    {
        Fixture f;
        QVERIFY(write(f.old + "/bible.sqlite", "immutable text"));
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--wal-writer", f.old});
        QVERIFY(child.waitForStarted()); QVERIFY(child.waitForFinished()); QCOMPARE(child.exitCode(), 0);
        QVERIFY(QFileInfo(f.old + "/personal.sqlite-wal").size() > 0);
        const auto original = read(f.old + "/personal.sqlite"), wal = read(f.old + "/personal.sqlite-wal");
        const auto scope = QCryptographicHash::hash((f.old + "/omabible").toUtf8(), QCryptographicHash::Sha256).toHex().left(32);
        const auto endpoint = f.temp.filePath("omabible-" + QString::fromLatin1(scope) + ".socket");
        QVERIFY(QFile::exists(endpoint));
        StorageMigration migration(f.data, f.temp.path());
        QVERIFY2(migration.run(), qPrintable(migration.errorString()));
        QVERIFY(!QFile::exists(endpoint));
        QCOMPARE(read(f.data + "/.storage-migration/originals/personal.sqlite"), original);
        QCOMPARE(read(f.data + "/.storage-migration/originals/personal.sqlite-wal"), wal);
        QVERIFY(!QFile::exists(f.old + "/personal.sqlite-wal"));
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "wal-check");
            db.setDatabaseName(f.data + "/personal.sqlite"); QVERIFY(db.open());
            QSqlQuery q(db); QVERIFY(q.exec("SELECT note FROM annotations")); QVERIFY(q.next());
            QCOMPARE(q.value(0).toString(), QString("committed only in WAL"));
        }
        QSqlDatabase::removeDatabase("wal-check");
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().value(1) == "--wal-writer") {
        const auto old = app.arguments().value(2);
        if (!makeStore(old + "/personal.sqlite")) return 2;
        QLockFile lock(old + "/omabible.lock"); if (!lock.tryLock()) return 3;
        const auto hash = QCryptographicHash::hash((old + "/omabible").toUtf8(), QCryptographicHash::Sha256).toHex().left(32);
        QLocalServer server;
        if (!server.listen(QFileInfo(old).absolutePath() + "/omabible-" + QString::fromLatin1(hash) + ".socket")) return 4;
        auto db = QSqlDatabase::addDatabase("QSQLITE", "writer");
        db.setDatabaseName(old + "/personal.sqlite"); if (!db.open()) return 5;
        QSqlQuery q(db);
        if (!q.exec("PRAGMA journal_mode=WAL") || !q.exec("PRAGMA wal_autocheckpoint=0")
            || !q.exec("UPDATE annotations SET note='committed only in WAL'")) return 6;
        ::_exit(0); // Real uncheckpointed WAL + stale lock/socket, no destructors.
    }
    MigrationTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_migration.moc"
