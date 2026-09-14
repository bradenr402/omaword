#include <QtTest/QTest>
#include "biblebackend.h"
#include "biblebooks.h"
#include <QClipboard>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <archive.h>
#include <archive_entry.h>
#include <limits>
#include <unistd.h>

class BackendTest : public QObject
{
    Q_OBJECT
    QTemporaryDir fixtures;
    std::unique_ptr<QTemporaryDir> session;
    std::unique_ptr<BibleBackend> bible;
    QString net, kjv, ojb;
    static bool write(const QString &path, const QByteArray &bytes) {
        QSaveFile f(path); return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size() && f.commit();
    }
    static bool zip(const QString &path, const QMap<QString,QByteArray> &entries) {
        archive *a = archive_write_new();
        const auto cleanup = qScopeGuard([&] { archive_write_free(a); });
        if (archive_write_set_format_zip(a) != ARCHIVE_OK
            || archive_write_open_filename(a,QFile::encodeName(path).constData()) != ARCHIVE_OK) return false;
        for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
            archive_entry *entry = archive_entry_new();
            const auto cleanupEntry = qScopeGuard([&] { archive_entry_free(entry); });
            archive_entry_set_pathname(entry,it.key().toUtf8().constData());
            archive_entry_set_size(entry,it.value().size());
            archive_entry_set_filetype(entry,AE_IFREG); archive_entry_set_perm(entry,0600);
            if (archive_write_header(a,entry) != ARCHIVE_OK
                || archive_write_data(a,it.value().constData(),it.value().size()) != it.value().size()) return false;
        }
        return archive_write_close(a) == ARCHIVE_OK;
    }
    static bool fixture(const QString &path, const QString &id, bool extras = false, bool sourceNames = false) {
        QMap<QString,QByteArray> entries;
        auto book = [&](int ordinal, QString code, QList<int> chapters, QString name = {}, QString shortName = {}) {
            QString body = "\\id " + code + " Original Backend Bible\n";
            if (!name.isEmpty()) body += "\\toc1 " + name + "\n\\toc2 " + shortName + "\n\\toc3 " + shortName.left(3) + "\n";
            for (int c : chapters) {
                body += QString("\\c %1\n\\s1 Original heading\n\\p\n").arg(c);
                for (int v = 1; v <= 36; ++v) {
                    if (code == "MRK" && v == 16 && !extras) continue;
                    body += QString("\\v %1 Original %2 %3:%1 words with \\it emphasis\\it*.\n").arg(v).arg(code).arg(c);
                    if (v == 5) body += "\\p\n";
                }
            }
            entries[QString("%1-%2%3.usfm").arg(ordinal,2,10,QChar('0')).arg(code,id)] = body.toUtf8();
        };
        book(1,"GEN",{1,2},sourceNames ? "Bereshis" : QString(),"Bereishis");
        book(2,"MAL",{4});
        if (extras) { book(3,"TOB",{1}); book(4,"ESG",{10,11,12,13,14,15,16}); }
        book(5,"MAT",{1}); book(6,"MRK",{7}); book(7,"JHN",{3,4});
        book(8,"JUD",{1}); book(9,"REV",{22});
        if (extras) {
            entries["10-2ES"+id+".usfm"] = "\\id 2ES Original Backend Bible\n\\c 999\n\\p\n\\v 999 Original upper-bound verse.\n";
        }
        return zip(path,entries);
    }
    QSqlDatabase store() const {
        for (const auto &name : QSqlDatabase::connectionNames()) {
            auto db = QSqlDatabase::database(name);
            if (db.databaseName() == bible->dataDirectory()+"/personal.sqlite") return db;
        }
        return {};
    }
    QString installedPath(const QString &id = {}) const {
        QSqlQuery q(store()); q.prepare("SELECT path FROM installed WHERE version_id=?");
        q.addBindValue(id.isEmpty() ? bible->versionId() : id);
        return q.exec() && q.next() ? bible->dataDirectory()+'/'+q.value(0).toString() : QString();
    }
    void install(const QString &path) {
        bible->importUsfm(QUrl::fromLocalFile(path));
        QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(),60000);
        QVERIFY2(bible->ready() && bible->error().isEmpty(),qPrintable(bible->error()));
    }
    QStringList versionIds() const {
        QStringList result;
        for (const auto &row : bible->versions()) result << row.toMap()["id"].toString();
        return result;
    }
    void checkSourceOrderedNote(int nextBook, int previousBook) {
        const int chapter = bible->chaptersForBook(nextBook).first().toInt();
        const auto nextVerses = bible->chapterVerses(nextBook,chapter);
        QVERIFY(nextVerses.size() >= 3);
        const int first = nextVerses[0].toMap()["number"].toInt(), second = nextVerses[1].toMap()["number"].toInt();
        bible->openChapter(nextBook,chapter); bible->selectVerses(first,first); bible->moveSelection(-1);
        QCOMPARE(bible->book(),previousBook);
        const int previousChapter = bible->chapter(), previousVerse = bible->cursorVerse();
        bible->moveSelection(2,true); QCOMPARE(bible->book(),nextBook); QCOMPARE(bible->cursorVerse(),second);
        const auto reference = bible->selectionReference();
        bible->setNoteText("Source-ordered rail note");
        QVERIFY2(bible->error().isEmpty(),qPrintable(bible->error()));
        auto verify = [&](int b, int c, QList<int> covered) {
            bible->openChapter(b,c);
            for (const auto &row : bible->verses()) {
                const auto verse = row.toMap(); const int number = verse["number"].toInt();
                const auto ranges = verse["noteRanges"].toList();
                QCOMPARE(ranges.size(),covered.contains(number) ? 1 : 0);
                QCOMPARE(verse["hasNote"].toBool(),!ranges.isEmpty());
                if (ranges.isEmpty()) continue;
                const auto range = ranges.first().toMap();
                const bool start = b == previousBook && c == previousChapter && number == previousVerse;
                const bool end = b == nextBook && c == chapter && number == second;
                QCOMPARE(range["reference"].toString(),reference);
                QCOMPARE(range["noteStart"].toBool(),start); QCOMPARE(range["noteEnd"].toBool(),end);
                QCOMPARE(range["continuesBefore"].toBool(),!start); QCOMPARE(range["continuesAfter"].toBool(),!end);
            }
        };
        verify(nextBook,chapter,{first,second}); verify(previousBook,previousChapter,{previousVerse});
        verify(1,1,{});
        bible.reset(); bible = std::make_unique<BibleBackend>();
        QVERIFY2(bible->ready(),qPrintable(bible->error()));
        verify(nextBook,chapter,{first,second}); verify(previousBook,previousChapter,{previousVerse});
        QVERIFY(bible->deleteAnnotation(reference)); verify(nextBook,chapter,{}); verify(previousBook,previousChapter,{});
    }
private slots:
    void initTestCase() {
        QVERIFY(fixtures.isValid());
        QCoreApplication::setOrganizationName("OmaWordTests");
        QCoreApplication::setApplicationName("omaword-backend-test");
        net = fixtures.filePath("engnet_usfm.zip"); kjv = fixtures.filePath("eng-kjv_usfm.zip"); ojb = fixtures.filePath("engojb_usfm.zip");
        QVERIFY(fixture(net,"engnet")); QVERIFY(fixture(kjv,"eng-kjv",true)); QVERIFY(fixture(ojb,"engojb",false,true));
    }
    void init() {
        session = std::make_unique<QTemporaryDir>(); QVERIFY(session->isValid());
        qputenv("XDG_DATA_HOME",session->path().toUtf8());
        bible = std::make_unique<BibleBackend>();
        QVERIFY2(bible->error().isEmpty(),qPrintable(bible->error()));
    }
    void cleanup() { bible.reset(); session.reset(); }
    void emptyAndDurablePreferences() {
        QVERIFY(!bible->ready()); QVERIFY(bible->books().isEmpty()); QVERIFY(bible->chapterNumbers().isEmpty());
        QVERIFY(!bible->navigate("John 3")); bible->clearError();
        const auto property = bible->metaObject()->property(bible->metaObject()->indexOfProperty("books"));
        QVERIFY(!property.isConstant()); QCOMPARE(property.notifySignal().name(),QByteArray("booksChanged"));
        QSignalSpy changed(bible.get(),&BibleBackend::preferencesChanged);
        bible->setShowVerseNumbers(false); bible->setShowHeadings(false); bible->setVersePicker(false);
        bible->setFocusMode(true); bible->setReadingMode("block"); bible->setFontSize(23); bible->setLineHeight(2.26);
        QCOMPARE(changed.count(),7); QCOMPARE(bible->lineHeight(),2.3);
        bible->setLineHeight(std::numeric_limits<double>::quiet_NaN()); QCOMPARE(changed.count(),7);
        bible.reset(); bible = std::make_unique<BibleBackend>();
        QVERIFY(!bible->showVerseNumbers()); QVERIFY(!bible->showHeadings()); QVERIFY(!bible->versePicker());
        QVERIFY(bible->focusMode()); QCOMPARE(bible->readingMode(),QString("block"));
        QCOMPARE(bible->fontSize(),23); QCOMPARE(bible->lineHeight(),2.3);
        QSqlQuery q(store());
        QVERIFY(q.exec("PRAGMA user_version")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),5); q.finish();
        QVERIFY(q.exec("CREATE TRIGGER reject_preferences BEFORE UPDATE ON preferences BEGIN SELECT RAISE(ABORT,'preference failure'); END"));
        bible->setFontSize(19); QCOMPARE(bible->fontSize(),23); QVERIFY(bible->error().contains("preference failure"));
    }
    void dynamicBooksAndSourceOrder() {
        QSignalSpy books(bible.get(),&BibleBackend::booksChanged);
        install(kjv); QCOMPARE(books.count(),1); QCOMPARE(bible->versionId(),QString("eng-kjv"));
        const QVariantList chapters{10,11,12,13,14,15,16};
        QCOMPARE(bible->chaptersForBook(69),chapters);
        QVERIFY(bible->navigate("ESG")); QCOMPARE(bible->chapter(),10); QCOMPARE(bible->chapterCount(),7);
        QCOMPARE(bible->chapterNumbers(),chapters);
        bible->nextChapter(); QCOMPARE(bible->chapter(),11); bible->previousChapter(); QCOMPARE(bible->chapter(),10);
        bible->previousChapter(); QCOMPARE(bible->book(),67); QCOMPARE(bible->chapter(),1);
        QVERIFY(bible->navigate("ESG 16:36")); bible->moveSelection(1,true);
        QCOMPARE(bible->book(),40); QCOMPARE(bible->cursorVerse(),1); QVERIFY(bible->crossChapterSelection());
        const auto range = bible->selectionReference();
        QVERIFY(range.contains("Matthew")); bible->setNoteText("Across source ordinal boundary"); bible->toggleBookmark();
        QVERIFY2(bible->error().isEmpty(),qPrintable(bible->error()));
        QVERIFY(bible->navigate(range)); QCOMPARE(bible->noteText(),QString("Across source ordinal boundary"));
        bible.reset(); bible = std::make_unique<BibleBackend>();
        QVERIFY2(bible->ready(),qPrintable(bible->error())); QCOMPARE(bible->selectionReference(),range);
        QCOMPARE(bible->noteText(),QString("Across source ordinal boundary"));
        QVERIFY(bible->navigate("MAT 1")); bible->previousChapter(); QCOMPARE(bible->book(),69); QCOMPARE(bible->chapter(),16);
        QVERIFY(bible->navigate("2ES 999:999")); bible->setNoteText("Upper bounds");
        QVERIFY2(bible->error().isEmpty(),qPrintable(bible->error()));
        QVERIFY(bible->copySelection().contains("⁹⁹⁹ Original upper-bound verse."));
        const auto backup = QUrl::fromLocalFile(session->filePath("bounds.json"));
        QVERIFY(bible->exportBackup(backup)); bible->setNoteText("Changed");
        QVERIFY2(bible->importBackup(backup),qPrintable(bible->error())); QCOMPARE(bible->noteText(),QString("Upper bounds"));
    }
    void aliasesAndPurePreview() {
        install(ojb);
        QVERIFY(bible->navigate("GEN 1:2")); QCOMPARE(bible->book(),1);
        QCOMPARE(bible->passage(),QString("Bereishis 1"));
        for (const QString alias : {"Genesis", "GEN", "Bereshis", "Bereishis", "Ber"}) {
            QVERIFY2(bible->navigate(alias+" 1:2"),qPrintable(bible->error())); QCOMPARE(bible->book(),1);
        }
        QVERIFY(bible->navigate("Jn3:35-4:2")); bible->setReadingPosition(0.37);
        const auto reference = bible->selectionReference();
        const auto verses = bible->verses();
        QSignalSpy navigation(bible.get(),&BibleBackend::navigationChanged);
        const auto result = bible->resolveReference("Jude 5-7");
        QVERIFY(result["valid"].toBool()); QCOMPARE(result["reference"].toString(),QString("Jude 1:5–7"));
        for (const QString invalid : {"John 0", "John 3:0", "John 3:99", "John 4:2-3:35", "J 1", "MRK 7:16", "John;DROP TABLE"})
            QVERIFY2(!bible->resolveReference(invalid)["valid"].toBool(),qPrintable(invalid));
        QCOMPARE(bible->selectionReference(),reference); QCOMPARE(bible->verses(),verses);
        QCOMPARE(bible->readingPosition(),0.37); QCOMPARE(navigation.count(),0);
    }
    void sourceOrderedNoteMetadata() {
        install(kjv);
        checkSourceOrderedNote(40,69);
    }
    void sourceOmissionsCopyAndRangeAnnotations() {
        install(net); QVERIFY(bible->navigate("MRK 7:15"));
        bible->moveSelection(1,true); QCOMPARE(bible->selectedEnd(),17);
        bible->setNoteText("Original range note"); bible->toggleBookmark(); bible->toggleHighlight();
        QVERIFY(bible->selectionHighlighted()); QVERIFY(bible->selectionBookmarked());
        const QString copy = bible->copySelection();
        QVERIFY(copy.startsWith("“¹⁵ ")); QVERIFY(copy.contains("¹⁷ ")); QVERIFY(!copy.contains("¹⁶ "));
        QCOMPARE(QGuiApplication::clipboard()->text(),copy);
        QVERIFY(bible->copyVerse(16).isEmpty()); QVERIFY(!bible->error().isEmpty());
        bible->selectVerses(17,17); bible->toggleHighlight(); QVERIFY(!bible->selectionHighlighted());
        bible->selectVerses(15,15); QVERIFY(bible->selectionHighlighted()); QVERIFY(bible->noteText().isEmpty());
        bible->selectVerses(15,17); QCOMPARE(bible->noteText(),QString("Original range note"));
        const auto plain = bible->copySelection(true); QVERIFY(!plain.contains("¹⁵")); QVERIFY(!plain.contains("—Mark"));
        QVERIFY(bible->navigate("JHN 3:36")); bible->moveSelection(2,true);
        QCOMPARE(bible->selectionReference(),QString("John 3:36–4:2"));
        bible->moveSelection(-3,true); QCOMPARE(bible->selectionReference(),QString("John 3:35–36"));
    }
    void schemaFourMigrationPreservesArchivesAndRollsBack() {
        const QString path = bible->dataDirectory()+"/personal.sqlite";
        bible.reset();
        const auto cleanup = qScopeGuard([] { QSqlDatabase::removeDatabase("migration"); });
        auto db = QSqlDatabase::addDatabase("QSQLITE","migration"); db.setDatabaseName(path); QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec("DROP TABLE annotations"));
        QVERIFY(q.exec("CREATE TABLE annotations(book INTEGER,chapter INTEGER,start INTEGER,end INTEGER,bookmark INTEGER,highlight INTEGER,note TEXT,endBook INTEGER,endChapter INTEGER,version_id TEXT,PRIMARY KEY(version_id,book,chapter,start,endBook,endChapter,end),CHECK(book BETWEEN 1 AND 66),CHECK(chapter BETWEEN 1 AND 150),CHECK(start BETWEEN 1 AND 176)) WITHOUT ROWID"));
        QVERIFY(q.exec("INSERT INTO annotations VALUES(1,1,1,1,1,0,'Archived note',1,1,'net-2.1')"));
        QVERIFY(q.exec("INSERT OR REPLACE INTO reading VALUES(1,1,1,1,1,0.42,1,1,1,1,'net-2.1')"));
        QVERIFY(q.exec("PRAGMA user_version=4"));
        // An invalid row must fail copying into the replacement table atomically.
        QVERIFY(q.exec("PRAGMA ignore_check_constraints=ON"));
        QVERIFY(q.exec("UPDATE annotations SET start=1000"));
        bible = std::make_unique<BibleBackend>(); QVERIFY(!bible->error().isEmpty()); bible.reset();
        QVERIFY(q.exec("PRAGMA user_version")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),4); q.finish();
        QVERIFY(q.exec("SELECT note FROM annotations")); QVERIFY(q.next()); QCOMPARE(q.value(0).toString(),QString("Archived note")); q.finish();
        QVERIFY(q.exec("UPDATE annotations SET start=1"));
        bible = std::make_unique<BibleBackend>(); QVERIFY2(bible->error().isEmpty(),qPrintable(bible->error()));
        install(net); QVERIFY(bible->collection().isEmpty());
        QVERIFY(q.exec("SELECT note,version_id FROM annotations")); QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(),QString("Archived note")); QCOMPARE(q.value(1).toString(),QString("net-2.1")); q.finish();
        QVERIFY(q.exec("SELECT position FROM reading WHERE version_id='net-2.1'")); QVERIFY(q.next()); QCOMPARE(q.value(0).toDouble(),0.42); q.finish();
        QVERIFY(q.exec("PRAGMA user_version")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),5);
    }
    void backupValidationAndTransactionalFailures() {
        install(net); QVERIFY(bible->navigate("John 3:16-18")); bible->setNoteText("Keep this note"); bible->setReadingPosition(0.73);
        const auto backup = QUrl::fromLocalFile(session->filePath("backup.json")); QVERIFY(bible->exportBackup(backup));
        QFile source(backup.toLocalFile()); QVERIFY(source.open(QIODevice::ReadOnly));
        const auto document = QJsonDocument::fromJson(source.readAll());
        const auto original = bible->collection();
        for (const QString kind : {"signature","version","fractional","unknown-key","out-of-range","omitted","duplicate","reading"}) {
            auto root = document.object(); auto rows = root["annotations"].toArray(); auto a = rows.first().toObject();
            if (kind == "signature") root["format"] = "unknown-backup";
            if (kind == "version") root["version"] = 2;
            if (kind == "fractional") a["start"] = 16.5;
            if (kind == "unknown-key") a["unexpected"] = true;
            if (kind == "out-of-range") a["chapter"] = 1000;
            if (kind == "omitted") { a["book"] = 41; a["chapter"] = 7; a["start"] = 16; }
            if (kind == "reading") { auto r = root["reading"].toObject(); r["position"] = 2; root["reading"] = r; }
            rows[0] = a; if (kind == "duplicate") rows.append(a); root["annotations"] = rows;
            const auto invalid = QUrl::fromLocalFile(session->filePath("invalid.json"));
            QVERIFY(write(invalid.toLocalFile(),QJsonDocument(root).toJson()));
            QVERIFY2(!bible->importBackup(invalid),qPrintable(kind)); QCOMPARE(bible->collection(),original); QCOMPARE(bible->readingPosition(),0.73);
        }
        QByteArray duplicate = document.toJson();
        const QByteArray backupVersion = QByteArray::number(document.object().value("version").toInt());
        duplicate.replace("\"version\": " + backupVersion,"\"version\": " + backupVersion + ", \"ver\\u0073ion\": " + backupVersion);
        QVERIFY(write(session->filePath("duplicate.json"),duplicate));
        QVERIFY(!bible->importBackup(QUrl::fromLocalFile(session->filePath("duplicate.json"))));
        QVERIFY(bible->error().contains("duplicate JSON"));
        QSqlQuery q(store());
        QVERIFY(q.exec("CREATE TRIGGER reject_notes BEFORE INSERT ON annotations BEGIN SELECT RAISE(ABORT,'injected disk failure'); END"));
        bible->setNoteText("Must fail"); QVERIFY(bible->error().contains("injected disk failure"));
        QVERIFY(!bible->importBackup(backup)); QCOMPARE(bible->collection(),original); QCOMPARE(bible->noteText(),QString("Keep this note"));
        QVERIFY(q.exec("DROP TRIGGER reject_notes"));
        // Deferred constraint tests a COMMIT failure after successful write/readback.
        QVERIFY(q.exec("PRAGMA foreign_keys=ON"));
        QVERIFY(q.exec("CREATE TABLE parent(id INTEGER PRIMARY KEY)"));
        QVERIFY(q.exec("CREATE TABLE failure(id INTEGER REFERENCES parent(id) DEFERRABLE INITIALLY DEFERRED)"));
        QVERIFY(q.exec("CREATE TRIGGER fail_commit AFTER INSERT ON annotations BEGIN INSERT INTO failure VALUES(1); END"));
        bible->setNoteText("Commit must fail"); QVERIFY(bible->error().contains("Cannot commit annotations")); QCOMPARE(bible->collection(),original);
        QVERIFY(q.exec("DROP TRIGGER fail_commit"));
        bible->setNoteText("Changed"); QVERIFY(bible->importBackup(backup)); QCOMPARE(bible->collection(),original);
        QVERIFY(bible->exportNotes(QUrl::fromLocalFile(session->path())));
        QVERIFY(!bible->exportBackup(QUrl::fromLocalFile(bible->dataDirectory()+"/personal.sqlite")));
    }
    void peerNotificationsFallbackAndMru() {
        install(net); BibleBackend peer;
        QVERIFY(peer.navigate("John 3:16"));
        QSignalSpy peerBooks(&peer,&BibleBackend::booksChanged);
        install(kjv); QCOMPARE(peer.versionId(),QString("engnet")); QCOMPARE(peerBooks.count(),0);
        QVERIFY(peer.switchVersion("eng-kjv")); QCOMPARE(peerBooks.count(),1);
        QCOMPARE(peer.chaptersForBook(69),QVariantList({10,11,12,13,14,15,16}));
        QVERIFY(bible->navigate("ESG 10:2")); bible->setNoteText("Edition-scoped note");
        QVERIFY(bible->switchVersion("engnet")); QCOMPARE(bible->book(),1); QCOMPARE(bible->chapter(),1);
        QVERIFY(bible->status().contains("absent")); QVERIFY(bible->collection().isEmpty());
        install(ojb); QCOMPARE(versionIds(),QStringList({"engojb","engnet","eng-kjv"}));
        QVERIFY(bible->switchVersion("eng-kjv")); QCOMPARE(versionIds(),QStringList({"eng-kjv","engojb","engnet"}));
        const auto old = installedPath();
        install(kjv); QCOMPARE(peerBooks.count(),2); QVERIFY(installedPath() != old); QVERIFY(!QFile::exists(old));
        QVERIFY(bible->removeVersion("eng-kjv",false)); QCOMPARE(bible->versionId(),QString("engojb"));
        QVERIFY(peer.ready()); QCOMPARE(peer.versionId(),QString("engojb"));
        install(kjv); QVERIFY(bible->navigate("ESG 10:2")); QCOMPARE(bible->noteText(),QString("Edition-scoped note"));
        QVERIFY(bible->removeVersion("eng-kjv",true)); install(kjv); QVERIFY(bible->collection().isEmpty());
    }
    void multiwindowConflictsAndRemovalGates() {
        install(net); BibleBackend peer;
        QVERIFY(bible->navigate("John 3:16")); QVERIFY(peer.navigate("John 3:16"));
        bible->setNoteText("Shared original"); QCOMPARE(peer.noteText(),bible->noteText());
        peer.setFlushCallback([&] { peer.setNoteText("Second draft"); return peer.noteText() == "Second draft"; });
        bible->setFlushCallback([&] { bible->setNoteText("First draft"); return bible->noteText() == "First draft"; });
        const auto path = installedPath();
        QVERIFY(!bible->removeVersion("engnet",true)); QVERIFY(bible->error().contains("unsaved note"));
        QVERIFY(peer.error().contains("changed in another window")); QVERIFY(QFile::exists(path));
        QCOMPARE(peer.noteText(),QString("First draft"));
        bible->setFlushCallback({}); peer.setFlushCallback({});
        peer.setNoteText("Resolved draft"); QCOMPARE(bible->noteText(),QString("Resolved draft"));
        int flushes = 0; peer.setFlushCallback([&] { return ++flushes == 1; });
        bible->importUsfm(QUrl::fromLocalFile(net)); QVERIFY(bible->busy()); QVERIFY(peer.busy());
        QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(),30000);
        QCOMPARE(flushes,2); QCOMPARE(installedPath(),path); QVERIFY(bible->error().contains("unsaved note"));
        peer.setFlushCallback({});
        QVERIFY(bible->removeVersion("engnet",false)); QVERIFY(!bible->ready()); QVERIFY(!peer.ready());
        QVERIFY(peer.books().isEmpty()); QVERIFY(!QFile::exists(path));
        peer.setNoteText("Cannot resurrect removed edition"); QVERIFY(!peer.error().isEmpty());
    }
    void failedLibraryTransactionAndPathOwnership() {
        install(net); const auto original = installedPath();
        QVERIFY(bible->navigate("John 3:16")); bible->setNoteText("Keep");
        const auto rows = bible->collection(), books = bible->books();
        QSqlQuery q(store());
        QVERIFY(q.exec("CREATE TRIGGER reject_library BEFORE UPDATE ON library BEGIN SELECT RAISE(ABORT,'library disk failure'); END"));
        bible->importUsfm(QUrl::fromLocalFile(kjv)); QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(),30000);
        QVERIFY(bible->error().contains("library disk failure")); QCOMPARE(bible->versionId(),QString("engnet"));
        QCOMPARE(installedPath(),original); QCOMPARE(bible->collection(),rows); QCOMPARE(bible->books(),books);
        QVERIFY(!bible->removeVersion("engnet",true)); QVERIFY(QFile::exists(original));
        QVERIFY(q.exec("DROP TRIGGER reject_library"));
        q.prepare("UPDATE installed SET path=? WHERE version_id='engnet'"); q.addBindValue("personal.sqlite"); QVERIFY(q.exec());
        QVERIFY(!bible->removeVersion("engnet",true)); QVERIFY(QFile::exists(bible->dataDirectory()+"/personal.sqlite"));
        q.prepare("UPDATE installed SET path=? WHERE version_id='engnet'"); q.addBindValue(QFileInfo(original).fileName()); QVERIFY(q.exec());
        const QString saved = session->filePath("saved.sqlite"); QVERIFY(QFile::rename(original,saved));
        QVERIFY(QFile::link(saved,original)); QVERIFY(!bible->removeVersion("engnet",true));
        QVERIFY(QFile::remove(original)); QVERIFY(QFile::rename(saved,original));
        QVERIFY(::link(QFile::encodeName(original).constData(),QFile::encodeName(saved).constData()) == 0);
        QVERIFY(!bible->removeVersion("engnet",true)); QVERIFY(QFile::exists(saved)); QVERIFY(QFile::remove(saved));
        QVERIFY(bible->removeVersion("engnet",false));
    }
    void invalidImportPreservesTarget() {
        const auto target = session->filePath("target.sqlite"), invalid = session->filePath("bad.zip");
        QVERIFY(write(target,"Original target")); QVERIFY(write(invalid,"Not a ZIP"));
        const auto result = UsfmImporter::importFile(invalid,target); QVERIFY(!result.ok); QVERIFY(!result.error.isEmpty());
        QFile file(target); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(),QByteArray("Original target"));
        install(net); const auto path = installedPath(); bible->importUsfm(QUrl::fromLocalFile(invalid));
        QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(),30000); QVERIFY(!bible->error().isEmpty()); QCOMPARE(installedPath(),path);
    }
    void navigationAndRestoreReadFailureAreAtomic() {
        install(net); QVERIFY(bible->navigate("Genesis 1"));
        const auto backup = QUrl::fromLocalFile(session->filePath("read-failure.json"));
        QVERIFY(bible->exportBackup(backup));
        QVERIFY(bible->navigate("John 3:16-18")); bible->setReadingPosition(0.61); bible->setNoteText("Keep on read failure");
        const auto before = bible->collection(), verses = bible->verses();
        const auto selection = bible->selectionReference();
        QSignalSpy navigation(bible.get(),&BibleBackend::navigationChanged);
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","read-failure"); db.setDatabaseName(installedPath()); QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("ALTER TABLE verses RENAME TO saved_verses"));
            // Source endpoints remain queryable while reading Genesis bodies fails.
            QVERIFY(q.exec("CREATE VIEW verses AS SELECT book,chapter,number,CASE WHEN book=1 THEN abs(-9223372036854775808) ELSE text END AS text,html,heading,poetry,paragraphStart FROM saved_verses"));
            QVERIFY(!bible->navigate("Genesis 1")); QVERIFY(!bible->importBackup(backup));
            QCOMPARE(bible->selectionReference(),selection); QCOMPARE(bible->verses(),verses);
            QCOMPARE(bible->collection(),before); QCOMPARE(bible->readingPosition(),0.61); QCOMPARE(navigation.count(),0);
            QVERIFY(q.exec("DROP VIEW verses")); QVERIFY(q.exec("ALTER TABLE saved_verses RENAME TO verses"));
        }
        QSqlDatabase::removeDatabase("read-failure");
        bible.reset(); bible = std::make_unique<BibleBackend>();
        QVERIFY2(bible->ready(),qPrintable(bible->error())); QCOMPARE(bible->selectionReference(),selection);
        QCOMPARE(bible->collection(),before); QCOMPARE(bible->noteText(),QString("Keep on read failure"));
    }
    void sourceKeyCorruptionRejected() {
        install(net);
        const QString path = session->filePath("corrupt.sqlite"); QVERIFY(QFile::copy(installedPath(),path));
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","corruption"); db.setDatabaseName(path); QVERIFY(db.open());
            QSqlQuery q(db);
            // A compensating move preserves total verse count but changes source keys.
            QVERIFY(q.exec("UPDATE verses SET chapter=2,number=37 WHERE book=1 AND chapter=1 AND number=36"));
        }
        QSqlDatabase::removeDatabase("corruption");
        QString error; QVERIFY(!UsfmImporter::validateDatabase(path,&error)); QVERIFY(!error.isEmpty());
    }
    void optionalSourceArchives_data() {
        QTest::addColumn<QString>("id"); QTest::addColumn<QString>("path");
        const auto directory = qEnvironmentVariable("OMAWORD_TEST_USFM_DIR");
        for (const QString id : {"engnet","eng-kjv2006","eng-kjv","engojb","engylt","eng-asv","engbsb"}) {
            QString path;
            if (id == "engnet") path = qEnvironmentVariable("OMAWORD_TEST_NET_ZIP");
            if (id == "eng-kjv2006") path = qEnvironmentVariable("OMAWORD_TEST_KJV_ZIP");
            if (path.isEmpty() && !directory.isEmpty()) path = QDir(directory).filePath(id+"_usfm.zip");
            QTest::newRow(qPrintable(id)) << id << path;
        }
    }
    void optionalSourceArchives() {
        QFETCH(QString,id); QFETCH(QString,path);
        if (path.isEmpty()) QSKIP("Set OMAWORD_TEST_USFM_DIR or OMAWORD_TEST_NET_ZIP / OMAWORD_TEST_KJV_ZIP for source archive checks.");
        QVERIFY2(QFileInfo::exists(path),qPrintable(path)); install(path); QCOMPARE(bible->versionId(),id);
        QString error; QVERIFY2(UsfmImporter::validateDatabase(installedPath(),&error),qPrintable(error));
        if (id == "eng-kjv") {
            QCOMPARE(bible->books().size(),80);
            QCOMPARE(bible->chaptersForBook(69),QVariantList({10,11,12,13,14,15,16}));
            const int first = bible->chapterVerses(69,10).first().toMap()["number"].toInt();
            QVERIFY(bible->navigate(QString("ESG 10:%1").arg(first)));
            QVERIFY(!bible->copySelection().isEmpty());
            checkSourceOrderedNote(40,80);
        }
        if (id == "engojb") {
            QVERIFY(bible->navigate("Bereshis 1:1")); QCOMPARE(bible->book(),1);
            QVERIFY(bible->navigate("Genesis 1:1"));
            QVERIFY(bible->passage().contains("Bereshis"));
            const auto books = bible->books();
            int acts = -1;
            for (int i = 0; i < books.size(); ++i) if (books[i].toMap()["id"].toInt() == 44) acts = i;
            QVERIFY(acts >= 0 && acts + 1 < books.size());
            const int next = books[acts+1].toMap()["id"].toInt();
            QVERIFY(next != 45); // This source does not place Romans after Acts.
            checkSourceOrderedNote(next,44);
        }
        for (const auto &row : bible->books()) {
            const auto book = row.toMap();
            QCOMPARE(book["chapters"].toInt(),book["chapterNumbers"].toList().size());
            for (const auto &chapter : book["chapterNumbers"].toList()) {
                const auto verses = bible->chapterVerses(book["id"].toInt(),chapter.toInt());
                QVERIFY(!verses.isEmpty());
                for (const auto &verse : verses) QVERIFY(!verse.toMap()["text"].toString().trimmed().isEmpty());
            }
        }
    }
};

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM","offscreen");
    QGuiApplication app(argc,argv);
    BackendTest test;
    return QTest::qExec(&test,argc,argv);
}
#include "tst_backend.moc"
