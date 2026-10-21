#include "testenvironment.h"
#include <QtTest/QTest>
#include "biblebackend.h"
#include "legacybackup.h"
#include "biblebooks.h"
#include "textselection.h"
#include <QClipboard>
#include <QCryptographicHash>
#include <QSqlRecord>
#include <QUuid>
#include <QDateTime>
#include <QGuiApplication>
#include <QProcess>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include "aofixture.h"
#include <limits>
#include <sys/stat.h>
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
    static bool fixture(const QString &path, const QString &id, bool extras = false, bool sourceNames = false) {
        QJsonArray books;
        auto book = [&](int ordinal, QString code, QList<int> chapters, QString name = {}, QString shortName = {}) {
            QJsonArray chapterList;
            for (int c : chapters) {
                QJsonArray content{QJsonObject{{"type", "heading"}, {"content", QJsonArray{"Original heading"}}}};
                for (int v = 1; v <= 36; ++v) {
                    if (code == "MRK" && v == 16 && !extras) continue;
                    content.append(AoFixture::verse(v, {QString("Original %1 %2:%3 words with emphasis.").arg(code).arg(c).arg(v)}));
                    if (v == 5) content.append(QJsonObject{{"type", "line_break"}});
                }
                chapterList.append(AoFixture::chapter(c, content));
            }
            auto entry = AoFixture::book(code, ordinal, chapterList, name.isEmpty() ? QString() : shortName);
            if (!name.isEmpty()) { entry["commonName"] = name; entry["title"] = shortName.left(3); }
            books.append(entry);
        };
        book(1,"GEN",{1,2},sourceNames ? "Bereshis" : QString(),"Bereishis");
        book(2,"MAL",{4});
        if (extras) { book(3,"TOB",{1}); book(4,"ESG",{10,11,12,13,14,15,16}); }
        book(5,"MAT",{1}); book(6,"MRK",{7}); book(7,"JHN",{3,4});
        book(8,"JUD",{1}); book(9,"REV",{22});
        if (extras) {
            books.append(AoFixture::book("DAG", 10, {AoFixture::chapter(999, {AoFixture::verse(999, {"Original upper-bound verse."})})}));
        }
        return AoFixture::write(path, AoFixture::complete(id, books, "Original Backend Bible"));
    }
    QSqlDatabase store() const {
        for (const auto &name : QSqlDatabase::connectionNames()) {
            auto db = QSqlDatabase::database(name);
            if (db.databaseName() == bible->dataDirectory()+"/personal.sqlite") return db;
        }
        return {};
    }
    // A store as schema 10 left it: book IDs stopped at 80.
    static bool schemaTen(QSqlQuery &q) {
        for (const QString table : {
                "annotations(version_id TEXT NOT NULL,book INTEGER NOT NULL CHECK(book BETWEEN 1 AND 80),chapter INTEGER NOT NULL CHECK(chapter BETWEEN 1 AND 999),start INTEGER NOT NULL CHECK(start BETWEEN 1 AND 999),endBook INTEGER NOT NULL CHECK(endBook BETWEEN 1 AND 80),endChapter INTEGER NOT NULL CHECK(endChapter BETWEEN 1 AND 999),end INTEGER NOT NULL CHECK(end BETWEEN 1 AND 999),highlight INTEGER NOT NULL CHECK(highlight IN(0,1)),note TEXT NOT NULL,created TEXT NOT NULL DEFAULT '',updated TEXT NOT NULL DEFAULT '',PRIMARY KEY(version_id,book,chapter,start,endBook,endChapter,end),CHECK(highlight OR length(note)>0)) WITHOUT ROWID",
                "history(id INTEGER PRIMARY KEY AUTOINCREMENT,seq INTEGER NOT NULL UNIQUE CHECK(seq>=1),version_id TEXT NOT NULL CHECK(length(version_id) BETWEEN 1 AND 128),book INTEGER NOT NULL CHECK(book BETWEEN 1 AND 80),chapter INTEGER NOT NULL CHECK(chapter BETWEEN 1 AND 999),start INTEGER NOT NULL CHECK(start BETWEEN 0 AND 999),endBook INTEGER NOT NULL CHECK(endBook BETWEEN 1 AND 80),endChapter INTEGER NOT NULL CHECK(endChapter BETWEEN 1 AND 999),end INTEGER NOT NULL CHECK(end BETWEEN 0 AND 999),position REAL NOT NULL CHECK(position BETWEEN 0 AND 1),visited TEXT NOT NULL,UNIQUE(version_id,book,chapter,start,endBook,endChapter,end))",
                "ribbon(id INTEGER PRIMARY KEY CHECK(id=1),version_id TEXT NOT NULL CHECK(length(version_id) BETWEEN 1 AND 128),book INTEGER NOT NULL CHECK(book BETWEEN 1 AND 80),chapter INTEGER NOT NULL CHECK(chapter BETWEEN 1 AND 999),start INTEGER NOT NULL CHECK(start BETWEEN 0 AND 999),endBook INTEGER NOT NULL CHECK(endBook BETWEEN 1 AND 80),endChapter INTEGER NOT NULL CHECK(endChapter BETWEEN 1 AND 999),end INTEGER NOT NULL CHECK(end BETWEEN 0 AND 999),position REAL NOT NULL CHECK(position BETWEEN 0 AND 1),placed TEXT NOT NULL)"}) {
            const QString name = table.section('(', 0, 0);
            if (!q.exec("ALTER TABLE " + name + " RENAME TO " + name + "_v11") || !q.exec("CREATE TABLE " + table)
                || !q.exec("INSERT INTO " + name + " SELECT * FROM " + name + "_v11") || !q.exec("DROP TABLE " + name + "_v11")) return false;
        }
        return q.exec("PRAGMA user_version=10");
    }
    // A store as schema 9 left it: the Desk was a list of passages.
    static bool schemaNine(QSqlQuery &q) {
        return schemaTen(q) && q.exec("DROP TABLE desk_page")
            && q.exec("CREATE TABLE desk(id INTEGER PRIMARY KEY AUTOINCREMENT,position INTEGER NOT NULL UNIQUE CHECK(position>=1),version_id TEXT NOT NULL CHECK(length(version_id) BETWEEN 1 AND 128),book INTEGER NOT NULL CHECK(book BETWEEN 1 AND 80),chapter INTEGER NOT NULL CHECK(chapter BETWEEN 1 AND 999),start INTEGER NOT NULL CHECK(start BETWEEN 1 AND 999),endBook INTEGER NOT NULL CHECK(endBook BETWEEN 1 AND 80),endChapter INTEGER NOT NULL CHECK(endChapter BETWEEN 1 AND 999),end INTEGER NOT NULL CHECK(end BETWEEN 1 AND 999),created TEXT NOT NULL,UNIQUE(version_id,book,chapter,start,endBook,endChapter,end))")
            && q.exec("PRAGMA user_version=9");
    }
    static bool deskRow(QSqlQuery &q, int position, const QString &versionId, int book, int chapter, int start, int end) {
        q.prepare("INSERT INTO desk(position,version_id,book,chapter,start,endBook,endChapter,end,created) VALUES(?,?,?,?,?,?,?,?,'2026-01-01T00:00:00.000Z')");
        for (const QVariant &value : QVariantList{position,versionId,book,chapter,start,book,chapter,end}) q.addBindValue(value);
        return q.exec();
    }
    // The Desk's quotation of a passage in the active version: its text laid
    // out as copySelection() lays it out (numbered when it spans verses), as
    // a Markdown blockquote ending with its reference and version.
    QString deskQuote(const QString &reference) {
        if (!bible->navigate(reference)) return {};
        const bool several = bible->selectedStart() != bible->selectedEnd() || bible->crossChapterSelection();
        QString body = bible->copySelection(!several);
        if (several) body = body.mid(1, body.lastIndexOf(QStringLiteral("\u201d\n\n\u2014")) - 1);
        QStringList lines;
        for (const auto &paragraph : body.split("\n\n")) {
            if (!lines.isEmpty()) lines << ">";
            const auto parts = paragraph.split('\n');
            for (int i = 0; i < parts.size(); ++i) lines << "> " + parts[i] + (i + 1 < parts.size() ? "  " : "");
        }
        lines << ">" << QStringLiteral("> \u2014 ") + bible->selectionReference() + " (" + bible->versionAbbreviation() + ")";
        return lines.join('\n');
    }
    // A store as schema 8 left it: annotations kept a bookmark flag, and
    // neither they nor tag assignments had times.
    static bool schemaEight(QSqlQuery &q) {
        return schemaNine(q)
            && q.exec("ALTER TABLE annotations RENAME TO annotations_v9")
            && q.exec("CREATE TABLE annotations(book INTEGER NOT NULL CHECK(book BETWEEN 1 AND 80),chapter INTEGER NOT NULL CHECK(chapter BETWEEN 1 AND 999),start INTEGER NOT NULL CHECK(start BETWEEN 1 AND 999),end INTEGER NOT NULL CHECK(end BETWEEN 1 AND 999),bookmark INTEGER NOT NULL CHECK(bookmark IN(0,1)),highlight INTEGER NOT NULL CHECK(highlight IN(0,1)),note TEXT NOT NULL,endBook INTEGER NOT NULL CHECK(endBook BETWEEN 1 AND 80),endChapter INTEGER NOT NULL CHECK(endChapter BETWEEN 1 AND 999),version_id TEXT NOT NULL,PRIMARY KEY(version_id,book,chapter,start,endBook,endChapter,end),CHECK(bookmark OR highlight OR length(note)>0)) WITHOUT ROWID")
            && q.exec("INSERT INTO annotations SELECT book,chapter,start,end,0,highlight,note,endBook,endChapter,version_id FROM annotations_v9")
            && q.exec("DROP TABLE annotations_v9")
            && q.exec("ALTER TABLE tag_assignments DROP COLUMN created")
            && q.exec("PRAGMA user_version=8");
    }
    QString installedPath(const QString &id = {}) const {
        QSqlQuery q(store()); q.prepare("SELECT path FROM installed WHERE version_id=?");
        q.addBindValue(id.isEmpty() ? bible->versionId() : id);
        return q.exec() && q.next() ? bible->dataDirectory()+'/'+q.value(0).toString() : QString();
    }
    void install(const QString &path) {
        bible->importAo(QUrl::fromLocalFile(path));
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
    static bool study(const QString &path) {
        using namespace AoFixture;
        return AoFixture::write(path, complete("study", {
            book("GEN", 1, {chapter(1, {verse(1, {"In the beginning.", QJsonObject{{"noteId", 0}}}), verse(2, {"Formless."}), verse(3, {"Light.", QJsonObject{{"noteId", 1}}})},
                {QJsonObject{{"noteId", 0}, {"caller", "+"}, {"text", "1:1 First note"}}, QJsonObject{{"noteId", 1}, {"caller", "b"}, {"text", "Second note"}}}), chapter(2, {verse(1, {"Finished."})})}),
            book("MAT", 2, {chapter(1, {QJsonObject{{"type", "heading"}, {"content", QJsonArray{"The record"}}}, verse(1, {"Book."}), verse(2, {"Abraham."}), verse(3, {"Judah."})})}),
            book("JHN", 3, {chapter(1, {verse(1, {"Word."}), verse(2, {"Same."}), verse(3, {"Made."})}), chapter(3, {verse(16, {"For God so loved. He said, ", QJsonObject{{"text", "Truly I say"}, {"wordsOfJesus", true}}, " again."})})})
        }));
    }
    static QVariantMap versionRow(BibleBackend *b, const QString &id) {
        for (const auto &row : b->versions()) if (row.toMap()["id"] == id) return row.toMap();
        return {};
    }
private slots:
    void explicitOneOffMigration() {
        const QString script = qEnvironmentVariable("OMAWORD_TEST_AO_MIGRATION_SCRIPT");
        if (script.isEmpty()) QSKIP("Supply the explicit one-off script path to verify the migration handoff.");
        install(net); QVERIFY(bible->navigate("John 3:16")); bible->setNoteText("One-off note"); QVERIFY(bible->toggleHighlight());
        QVERIFY(!bible->createAndAssignTag(bible->tagScope(), "One-off tag").isEmpty());
        bible->setReadingPosition(0.37); QVERIFY(bible->recordHistory()); QVERIFY(bible->placeRibbon());
        QVERIFY(bible->setDeskPage("Original Markdown with engnet kept literally", bible->deskPage()));
        { QSqlQuery q(store()); QVERIFY(q.exec("DELETE FROM reading WHERE version_id='net-2.1'")); }
        const QString directory = bible->dataDirectory(), source = directory + "/personal.sqlite";
        bible.reset();
        const QString output = session->filePath("migrated.sqlite");
        QProcess process; process.start("python3", {script, "migrate", "--source", source, "--output", output});
        QVERIFY(process.waitForFinished()); QVERIFY2(process.exitCode() == 0, process.readAllStandardError().constData());
        QVERIFY(QFile::rename(source, directory + "/original-personal.sqlite"));
        QVERIFY(QFile::rename(output, source));
        bible = std::make_unique<BibleBackend>(); QVERIFY(!bible->ready()); QVERIFY(bible->versions().isEmpty());
        QCOMPARE(bible->deskPage(), QString("Original Markdown with engnet kept literally"));
        const QString ao = fixtures.filePath("one-off.json"); QVERIFY(fixture(ao, "eng_net")); install(ao);
        QCOMPARE(bible->selectionReference(), QString("John 3:16")); QCOMPARE(bible->noteText(), QString("One-off note"));
        QCOMPARE(bible->readingPosition(), 0.37); QVERIFY(bible->selectionHighlighted());
        QVERIFY(!bible->tagsForScope(bible->tagScope()).isEmpty());
        QVERIFY(!bible->history().isEmpty()); QCOMPARE(bible->history().first().toMap()["versionId"].toString(), QString("eng_net"));
        const auto backup = QUrl::fromLocalFile(session->filePath("one-off-backup.json"));
        QVERIFY(bible->exportBackup(backup)); QVERIFY2(bible->importBackup(backup), qPrintable(bible->error()));
        process.start("python3", {script, "verify", "--personal", source, "--texts", directory});
        QVERIFY(process.waitForFinished()); QCOMPARE(process.exitCode(), 0);
        QVERIFY(process.readAllStandardOutput().contains("\"unmatched\": []"));
    }
    void orphanAoStateReconnectsOnInstallation() {
        const auto source = fixtures.filePath("mapped.json");
        QVERIFY(fixture(source, "eng_net"));
        QSqlQuery q(store());
        QVERIFY(q.exec("INSERT INTO annotations(version_id,book,chapter,start,endBook,endChapter,end,highlight,note,created,updated) VALUES('eng_net',43,3,16,43,3,16,1,'Migrated note','2026-01-01T00:00:00.000Z','2026-01-02T00:00:00.000Z')"));
        QVERIFY(q.exec("INSERT INTO reading VALUES(1,43,3,16,16,0.37,43,3,43,3,'eng_net')"));
        install(source);
        QCOMPARE(bible->selectionReference(), QString("John 3:16"));
        QCOMPARE(bible->noteText(), QString("Migrated note")); QCOMPARE(bible->readingPosition(), 0.37);
        const auto backup = QUrl::fromLocalFile(session->filePath("mapped-backup.json"));
        QVERIFY(bible->exportBackup(backup)); bible->setNoteText("Changed");
        QVERIFY2(bible->importBackup(backup), qPrintable(bible->error())); QCOMPARE(bible->noteText(), QString("Migrated note"));
        QVERIFY(q.exec("SELECT created,updated FROM annotations WHERE version_id='eng_net'")); QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(), QString("2026-01-01T00:00:00.000Z")); QCOMPARE(q.value(1).toString(), QString("2026-01-02T00:00:00.000Z"));
    }
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
        bible->setShowVerseNumbers(false); bible->setShowHeadings(false); bible->setChapterOutline(true);
        bible->setFocusMode(true); bible->setReadingMode("block"); bible->setFontSize(23); bible->setLineHeight(2.26);
        QCOMPARE(changed.count(),7); QCOMPARE(bible->lineHeight(),2.3);
        bible->setLineHeight(std::numeric_limits<double>::quiet_NaN()); QCOMPARE(changed.count(),7);
        bible.reset(); bible = std::make_unique<BibleBackend>();
        QVERIFY(!bible->showVerseNumbers()); QVERIFY(!bible->showHeadings()); QVERIFY(bible->chapterOutline());
        QVERIFY(bible->focusMode()); QCOMPARE(bible->readingMode(),QString("block"));
        QCOMPARE(bible->fontSize(),23); QCOMPARE(bible->lineHeight(),2.3);
        QSqlQuery q(store());
        QVERIFY(q.exec("PRAGMA user_version")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),11); q.finish();
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
        QVERIFY(range.contains("Matthew")); bible->setNoteText("Across source ordinal boundary"); bible->toggleHighlight();
        QVERIFY2(bible->error().isEmpty(),qPrintable(bible->error()));
        QVERIFY(bible->navigate(range)); QCOMPARE(bible->noteText(),QString("Across source ordinal boundary"));
        bible.reset(); bible = std::make_unique<BibleBackend>();
        QVERIFY2(bible->ready(),qPrintable(bible->error())); QCOMPARE(bible->selectionReference(),range);
        QCOMPARE(bible->noteText(),QString("Across source ordinal boundary"));
        QVERIFY(bible->navigate("MAT 1")); bible->previousChapter(); QCOMPARE(bible->book(),69); QCOMPARE(bible->chapter(),16);
        QVERIFY(bible->navigate("DAG 999:999")); bible->setNoteText("Upper bounds");
        QVERIFY2(bible->error().isEmpty(),qPrintable(bible->error()));
        QVERIFY(bible->copySelection().contains("⁹⁹⁹ Original upper-bound verse."));
        const auto backup = QUrl::fromLocalFile(session->filePath("bounds.json"));
        QVERIFY(bible->exportBackup(backup)); bible->setNoteText("Changed");
        QVERIFY2(bible->importBackup(backup),qPrintable(bible->error())); QCOMPARE(bible->noteText(),QString("Upper bounds"));
    }
    // Psalm 151 is a book of its own in some versions (the WEB) and Psalms'
    // last chapter in others (LXX2012): each reads it as the one it has.
    void psalmOneFiftyOne() {
        auto verses = [](int count) {
            QJsonArray content;
            for (int v = 1; v <= count; ++v) content.append(AoFixture::verse(v, {QString("Original psalm verse %1.").arg(v)}));
            return content;
        };
        const auto separate = session->filePath("separate.json"), numbered = session->filePath("numbered.json");
        QVERIFY(AoFixture::write(separate, AoFixture::complete("engweb", {
            AoFixture::book("PSA", 1, {AoFixture::chapter(23, verses(6)), AoFixture::chapter(150, verses(6))}),
            AoFixture::book("PS2", 2, {AoFixture::chapter(1, verses(7))}, "Psalm 151", true)})));
        QVERIFY(AoFixture::write(numbered, AoFixture::complete("englxx", {
            AoFixture::book("PSA", 1, {AoFixture::chapter(23, verses(6)), AoFixture::chapter(151, verses(7))})})));
        auto reads = [&](const QList<QPair<QString,QString>> &cases) {
            for (const auto &[typed, read] : cases) QCOMPARE(bible->resolveReference(typed)["reference"].toString(), read);
        };
        install(separate);
        reads({{"Psalm 151","Psalm 151 1"},{"Psalm 151:3","Psalm 151 1:3"},{"Psalm 151:3-5","Psalm 151 1:3–5"},{"Ps 151 3","Psalm 151 1:3"},
               {"Psalms 151","Psalm 151 1"},{"Psalm 23","Psalms 23"},{"Psalm 150:2","Psalms 150:2"}});
        QCOMPARE(bible->referenceCandidates("psalm 151:3").value(0).toMap()["reference"].toString(), QString("Psalm 151 1:3"));
        install(numbered);
        reads({{"Psalm 151","Psalms 151"},{"Psalm 151:3","Psalms 151:3"},{"Ps 151","Psalms 151"}});
        QCOMPARE(bible->referenceCandidates("psalm 151").value(0).toMap()["reference"].toString(), QString("Psalms 151"));
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
    void fuzzyReferenceCandidates() {
        install(net);
        auto refs = [&](const QString &input) {
            QStringList result;
            for (const auto &row : bible->referenceCandidates(input)) result << row.toMap()["reference"].toString();
            return result;
        };
        auto first = [&](const QString &input) { return refs(input).value(0); };
        // Exact forms keep their meaning and rank first.
        QCOMPARE(first("John 3:16"),QString("John 3:16"));
        QCOMPARE(first("Jn 3:16–18"),QString("John 3:16–18"));
        QCOMPARE(first("John 3:35-4:2"),QString("John 3:35–4:2"));
        // Compact and spaced forms.
        QCOMPARE(first("jn316"),QString("John 3:16"));
        QCOMPARE(first("jn3:16"),QString("John 3:16"));
        QCOMPARE(first("jn 3 16"),QString("John 3:16"));
        QCOMPARE(first("jn3.16"),QString("John 3:16"));
        QCOMPARE(first("jn316-18"),QString("John 3:16–18"));
        QCOMPARE(first("rev 22 14-20"),QString("Revelation 22:14–20"));
        QCOMPARE(first("rev22"),QString("Revelation 22"));
        QCOMPARE(first("mk7"),QString("Mark 7"));
        QCOMPARE(first("jud5"),QString("Jude 1:5"));
        // A digit run splits into chapter:verse only where that verse exists.
        QCOMPARE(refs("jn3"),QStringList{"John 3"});
        QCOMPARE(refs("jn336"),QStringList{"John 3:36"});
        QVERIFY(refs("jn337").isEmpty());
        QCOMPARE(first("gen12"),QString("Genesis 1:2"));
        // Ambiguity: ranked by book similarity, then canonical order.
        QCOMPARE(refs("j4"),(QStringList{"John 4","Jude 1:4"}));
        QCOMPARE(refs("jn4").value(0),QString("John 4"));
        QCOMPARE(first("mal4"),QString("Malachi 4"));
        QCOMPARE(first("ma4"),QString("Malachi 4"));
        const auto row = bible->referenceCandidates("jn316").value(0).toMap();
        QCOMPARE(row["book"].toInt(),43); QCOMPARE(row["chapter"].toInt(),3);
        QCOMPARE(row["start"].toInt(),16); QCOMPARE(row["end"].toInt(),16);
        QCOMPARE(bible->referenceCandidates("j4",1).size(),1);
        // Bare numbers are relative to the current book.
        QVERIFY(bible->navigate("John 4")); QCOMPARE(first("316"),QString("John 3:16"));
        for (const QString &none : QStringList{"", "zz9", "jn0", "John;DROP TABLE", QString(300,'j')})
            QVERIFY2(refs(none).isEmpty(),qPrintable(none));
        // Every candidate navigates.
        for (const auto &reference : refs("j4")) QVERIFY2(bible->navigate(reference),qPrintable(reference));
    }
    void exactPassageLinks() {
        install(net);
        const QString text = "See John 3:16 and Jn 3:16-18, also 1 Cor 13, Jn3:16, John 99, is 3, "
            "Mark 7. Then Revelation 22:1\u20135 and John 3:16-4:2 and John 3:99.";
        QStringList found, references;
        for (const auto &value : bible->passageLinks(text)) {
            const auto link = value.toMap();
            found << link["text"].toString(); references << link["reference"].toString();
            QCOMPARE(text.mid(link["start"].toInt(),link["length"].toInt()),link["text"].toString());
        }
        QCOMPARE(found,(QStringList{"John 3:16","Jn 3:16-18","Mark 7","Revelation 22:1\u20135"}));
        QCOMPARE(references,(QStringList{"John 3:16","John 3:16–18","Mark 7","Revelation 22:1–5"}));
        // Only the book words link, never the prose before them.
        const auto prose = bible->passageLinks("Read In John 4:2 today").value(0).toMap();
        QCOMPARE(prose["text"].toString(),QString("John 4:2"));
        QVERIFY(bible->passageLinks("Jn3:16 jn 3:16 John3").isEmpty());
        // Linked from: other notes whose links overlap the current passage.
        QVERIFY(bible->navigate("John 4:1")); bible->setNoteText("Compare John 3:16-18 here.");
        QVERIFY(bible->navigate("Mark 7:2")); bible->setNoteText("Echoes John 3.");
        QVERIFY(bible->navigate("Mark 7:3")); bible->setNoteText("Nothing linked; jn316.");
        QVERIFY2(bible->error().isEmpty(),qPrintable(bible->error()));
        auto linked = [&]() {
            QStringList result;
            for (const auto &row : bible->linkedNotes()) result << row.toMap()["reference"].toString();
            return result;
        };
        QVERIFY(bible->navigate("John 3:17")); QCOMPARE(linked(),(QStringList{"Mark 7:2","John 4:1"}));
        QVERIFY(bible->navigate("John 3:20")); QCOMPARE(linked(),QStringList{"Mark 7:2"});
        QVERIFY(bible->navigate("John 3")); QCOMPARE(linked(),(QStringList{"Mark 7:2","John 4:1"}));
        QVERIFY(bible->navigate("John 4")); QVERIFY(linked().isEmpty());
        const auto row = bible->linkedNotes().value(0).toMap();
        QVERIFY(row.isEmpty());
        QVERIFY(bible->navigate("John 3:16"));
        const auto note = bible->linkedNotes().value(1).toMap();
        QCOMPARE(note["excerpt"].toString(),QString("Compare John 3:16-18 here."));
        QVERIFY(bible->openTaggedPassage(note)); QCOMPARE(bible->selectionReference(),QString("John 4:1"));
        // A note never links to itself.
        QVERIFY(bible->navigate("John 3:16")); bible->setNoteText("Self John 3:16");
        QVERIFY(!linked().contains("John 3:16"));
    }
    void sourceOrderedNoteMetadata() {
        install(kjv);
        checkSourceOrderedNote(40,69);
    }
    void sourceOmissionsCopyAndRangeAnnotations() {
        install(net); QVERIFY(bible->navigate("MRK 7:15"));
        bible->moveSelection(1,true); QCOMPARE(bible->selectedEnd(),17);
        bible->setNoteText("Original range note"); bible->toggleHighlight();
        QVERIFY(bible->selectionHighlighted());
        const QString copy = bible->copySelection();
        QVERIFY(copy.startsWith("“¹⁵ ")); QVERIFY(copy.contains("¹⁷ ")); QVERIFY(!copy.contains("¹⁶ "));
        QCOMPARE(QGuiApplication::clipboard()->text(),copy);
        QVERIFY(bible->copyVerse(16).isEmpty()); QVERIFY(!bible->error().isEmpty());
        bible->selectVerses(17,17); bible->toggleHighlight(); QVERIFY(!bible->selectionHighlighted());
        bible->selectVerses(15,15); QVERIFY(bible->selectionHighlighted()); QVERIFY(bible->noteText().isEmpty());
        bible->selectVerses(15,17); QCOMPARE(bible->noteText(),QString("Original range note"));
        const auto plain = bible->copySelection(true); QVERIFY(!plain.contains("¹⁵")); QVERIFY(!plain.contains("— Mark"));
        QVERIFY(bible->navigate("JHN 3:36")); bible->moveSelection(2,true);
        QCOMPARE(bible->selectionReference(),QString("John 3:36–4:2"));
        bible->moveSelection(-3,true); QCOMPARE(bible->selectionReference(),QString("John 3:35–36"));
    }
    // A verse step within the chapter shown moves only the selection: the
    // chapter is neither read again nor announced (held, j steps 30 times a
    // second, and every verse row would be read again for each); a step into
    // another chapter opens it.
    void verseStepsWithinAChapterMoveOnlyTheSelection() {
        install(net); QVERIFY(bible->navigate("JHN 3:34"));
        QSignalSpy verses(bible.get(), &BibleBackend::versesChanged), navigation(bible.get(), &BibleBackend::navigationChanged),
            passage(bible.get(), &BibleBackend::passageChanged), selection(bible.get(), &BibleBackend::selectionChanged);
        bible->moveSelection(1); QCOMPARE(bible->selectionReference(), QString("John 3:35"));
        bible->moveSelection(1, true); QCOMPARE(bible->selectionReference(), QString::fromUtf8("John 3:35–36"));
        bible->moveByVerses(-5); QCOMPARE(bible->cursorVerse(), 31);
        QCOMPARE(selection.count(), 3);
        QCOMPARE(verses.count(), 0); QCOMPARE(navigation.count(), 0); QCOMPARE(passage.count(), 0);
        // Saved as it moves.
        { BibleBackend reopened; QCOMPARE(reopened.selectionReference(), QString("John 3:31")); }
        // From the last verse, into the next chapter: that one opens.
        bible->selectVerses(36, 36); verses.clear(); navigation.clear();
        bible->moveSelection(1); QCOMPARE(bible->selectionReference(), QString("John 4:1"));
        QCOMPARE(bible->chapter(), 4); QCOMPARE(navigation.count(), 1); QVERIFY(verses.count() >= 1);
        bible->moveSelection(-1); QCOMPARE(bible->selectionReference(), QString("John 3:36")); QCOMPARE(bible->chapter(), 3);
    }
    void quotationReferenceTypography_data() {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("label");
        QTest::newRow("same-chapter") << "REV 3:14-22" << QStringLiteral("Revelation 3:14–22");
        QTest::newRow("cross-chapter") << "REV 3:22-4:1" << QStringLiteral("Revelation 3:22–4:1");
        QTest::newRow("cross-book") << "GEN 1:2-REV 3:14" << QStringLiteral("First-Book 1:2–Revelation 3:14");
        QTest::newRow("hyphenated-book") << "GEN 1:1-2" << QStringLiteral("First-Book 1:1–2");
        QTest::newRow("single-verse") << "REV 3:14" << QStringLiteral("Revelation 3:14");
        QTest::newRow("whole-chapter") << "REV 3" << QStringLiteral("Revelation 3");
    }
    // ── Character selection (TextSelection): motions, objects, copy text
    static QList<TextSelection::Verse> charVerses() {
        return {{16, "For God so loved the world, that he gave his Son.", false},
                {17, "God did not send his Son to condemn. He came to save!", false},
                {18, "Whoever believes is not condemned.", true},
                {19, "", false},
                {20, "Light (came) into the world.", false, true, true},
                {21, "Darkness hated it.", false, true, true}};
    }
    static TextSelection::Position at(int verse, int offset) { return {verse, offset}; }
    void characterMotionsCrossVerseBreaks() {
        using namespace TextSelection;
        const auto v = charVerses();
        // h/l step characters and cross the verse break both ways, with counts.
        QCOMPARE(move(v, at(0, 0), "l"), at(0, 1));
        QCOMPARE(move(v, at(0, 0), "l", 4), at(0, 4));
        QCOMPARE(move(v, at(0, v[0].text.size() - 1), "l"), at(1, 0));
        QCOMPARE(move(v, at(1, 0), "h"), at(0, v[0].text.size() - 1));
        QCOMPARE(move(v, at(1, 1), "h", 3), at(0, v[0].text.size() - 2));
        QCOMPARE(move(v, at(0, 0), "h"), at(0, 0)); // chapter start stops
        QCOMPARE(move(v, at(5, v[5].text.size() - 1), "l", 9), at(5, v[5].text.size() - 1));
        // Empty verses are skipped.
        QCOMPARE(move(v, at(2, v[2].text.size() - 1), "l"), at(4, 0));
        // w b e: keyword runs and punctuation are separate words.
        QCOMPARE(move(v, at(0, 0), "w"), at(0, 4));             // God
        QCOMPARE(move(v, at(0, 0), "w", 3), at(0, 11));         // loved
        QCOMPARE(move(v, at(0, 21), "w"), at(0, 26));           // world → ","
        QCOMPARE(move(v, at(0, 21), "W"), at(0, 28));           // world, → that
        QCOMPARE(move(v, at(0, 4), "e"), at(0, 6));
        QCOMPARE(move(v, at(0, 22), "e"), at(0, 25));           // end of world
        QCOMPARE(move(v, at(0, 22), "E"), at(0, 26));           // end of world,
        QCOMPARE(move(v, at(0, 11), "b"), at(0, 8));
        QCOMPARE(move(v, at(0, 11), "b", 2), at(0, 4));
        QCOMPARE(move(v, at(0, 28), "b"), at(0, 26));
        QCOMPARE(move(v, at(0, 28), "B"), at(0, 21));
        QCOMPARE(move(v, at(0, 11), "ge"), at(0, 9));            // so
        QCOMPARE(move(v, at(0, 28), "ge"), at(0, 26));           // ","
        QCOMPARE(move(v, at(0, 28), "gE"), at(0, 26));
        QCOMPARE(move(v, at(0, 11), "ge", 2), at(0, 6));
        // Word motions cross verse breaks.
        const int son = v[0].text.indexOf("Son");
        QCOMPARE(move(v, at(0, son), "w", 2), at(1, 0));          // Son . → God
        QCOMPARE(move(v, at(0, son), "W"), at(1, 0));
        QCOMPARE(move(v, at(1, 0), "b"), at(0, v[0].text.size() - 1));
        QCOMPARE(move(v, at(1, 0), "ge"), at(0, v[0].text.size() - 1));
        QCOMPARE(move(v, at(0, v[0].text.size() - 1), "e"), at(1, 2));
        // At the chapter edges word motions stop on the last/first character.
        QCOMPARE(move(v, at(5, 9), "w", 50), at(5, v[5].text.size() - 1));
        QCOMPARE(move(v, at(0, 4), "b", 50), at(0, 0));
        // Sentences: ( ) with counts, across verse breaks and paragraph starts.
        QCOMPARE(move(v, at(0, 5), ")"), at(1, 0));
        QCOMPARE(move(v, at(1, 2), ")"), at(1, v[1].text.indexOf("He came")));
        QCOMPARE(move(v, at(0, 5), ")", 3), at(2, 0));
        QCOMPARE(move(v, at(1, 40), "("), at(1, v[1].text.indexOf("He came")));
        QCOMPARE(move(v, at(1, v[1].text.indexOf("He came")), "("), at(1, 0));
        QCOMPARE(move(v, at(1, 40), "(", 2), at(1, 0));
        QCOMPARE(move(v, at(2, 3), "(", 9), at(0, 0));
        QCOMPARE(move(v, at(4, 3), ")", 9), at(5, v[5].text.size() - 1));
        // gg and G ignore counts.
        QCOMPARE(move(v, at(4, 3), "gg", 5), at(0, 0));
        QCOMPARE(move(v, at(0, 3), "G", 2), at(5, v[5].text.size() - 1));
        // Graphemes are one character.
        const QList<Verse> emoji{{1, QStringLiteral("a 😀é b"), false, false}};
        QCOMPARE(move(emoji, at(0, 2), "l"), at(0, 4));
        QCOMPARE(move(emoji, at(0, 4), "l", 2), at(0, 6));
        QCOMPARE(characterEnd(emoji, at(0, 2)), 4);
        QCOMPARE(clamp(v, at(3, 0)), at(4, 0));
    }
    void characterTextObjects() {
        using namespace TextSelection;
        const auto v = charVerses();
        auto object = [&](TextSelection::Position a, TextSelection::Position c, const char *name, int count = 1) { return textObject(v, a, c, name, count); };
        // iw: the word, or the white space run; counts add runs.
        QCOMPARE(object(at(0, 12), at(0, 12), "iw"), qMakePair(at(0, 11), at(0, 15)));
        QCOMPARE(object(at(0, 12), at(0, 12), "iw", 3), qMakePair(at(0, 11), at(0, 19)));
        QCOMPARE(object(at(0, 3), at(0, 3), "iw"), qMakePair(at(0, 3), at(0, 3)));
        // aw: trailing white space, else leading; counts add words.
        QCOMPARE(object(at(0, 12), at(0, 12), "aw"), qMakePair(at(0, 11), at(0, 16)));
        QCOMPARE(object(at(0, 12), at(0, 12), "aw", 2), qMakePair(at(0, 11), at(0, 20)));
        const int son = v[0].text.indexOf("Son");
        QCOMPARE(object(at(0, son), at(0, son), "aw"), qMakePair(at(0, son - 1), at(0, son + 2)));
        // A word object crossing a verse break.
        QCOMPARE(object(at(0, son), at(0, son), "iw", 4), qMakePair(at(0, son), at(1, 2)));
        // is / as: the sentence (as with trailing white space); counts add sentences.
        const int he = v[1].text.indexOf("He came");
        QCOMPARE(object(at(1, 5), at(1, 5), "is"), qMakePair(at(1, 0), at(1, he - 2)));
        QCOMPARE(object(at(1, 5), at(1, 5), "as"), qMakePair(at(1, 0), at(1, he - 1)));
        QCOMPARE(object(at(1, 5), at(1, 5), "is", 2), qMakePair(at(1, 0), at(1, v[1].text.size() - 1)));
        QCOMPARE(object(at(0, 5), at(0, 5), "is", 2), qMakePair(at(0, 0), at(1, he - 2)));
        // With a selection the object extends past the cursor.
        QCOMPARE(object(at(0, 11), at(0, 15), "iw", 2), qMakePair(at(0, 11), at(0, 19)));
        QCOMPARE(object(at(0, 15), at(0, 11), "aw"), qMakePair(at(0, 15), at(0, 8)));
    }
    void characterQuotationEllipsesAndNumbers() {
        using namespace TextSelection;
        const auto v = charVerses();
        const QString a = v[0].text, b = v[1].text;
        // Plain text is the selection as is; never ellipses or numbers.
        QCOMPARE(text(v, at(0, 4), at(0, 16), false), QString("God so loved"));
        QCOMPARE(text(v, at(0, 17), at(1, 3), false), a.mid(17) + " God");
        // A whole single verse: no ellipsis, no number.
        QCOMPARE(text(v, at(0, 0), at(0, a.size()), true, "John 3:16"), QStringLiteral("“") + a + QStringLiteral("”\n\n— John 3:16"));
        // Partial single verse: ellipses at both cuts, punctuation trimmed at them.
        QCOMPARE(text(v, at(0, 4), at(0, 27), true, "John 3:16"), QStringLiteral("“… God so loved the world …”\n\n— John 3:16"));
        QCOMPARE(text(v, at(0, 0), at(0, 16), true, "R"), QStringLiteral("“For God so loved …”\n\n— R"));
        QCOMPARE(text(v, at(0, 26), at(0, a.size()), true, "R"), QStringLiteral("“… that he gave his Son.”\n\n— R"));
        // Across verses: the first verse's number only when its beginning is selected.
        QCOMPARE(text(v, at(0, 21), at(1, 7), true, "John 3:16–17"),
                 QStringLiteral("“… world, that he gave his Son. ¹⁷ God did …”\n\n— John 3:16–17"));
        QCOMPARE(text(v, at(0, 0), at(1, b.size()), true, "John 3:16–17"),
                 QStringLiteral("“¹⁶ ") + a + QStringLiteral(" ¹⁷ ") + b + QStringLiteral("”\n\n— John 3:16–17"));
        QCOMPARE(text(v, at(0, 0), at(1, 3), true, "R"), QStringLiteral("“¹⁶ ") + a + QStringLiteral(" ¹⁷ God …”\n\n— R"));
        // Paragraph and poetry joins follow the source.
        const int he = b.indexOf("He came");
        QCOMPARE(text(v, at(1, he), at(2, 8), false), b.mid(he) + "\n\nWhoever ");
        QCOMPARE(text(v, at(4, 0), at(5, 8), true, "R"), QStringLiteral("“²⁰ ") + v[4].text + QStringLiteral("\n²¹ Darkness …”\n\n— R"));
        // A poem line has a line of its own, after prose too, as prose after
        // one has; a verse whose lines begin in prose runs on from prose.
        const QList<TextSelection::Verse> poem = {{1, "Prose.", false}, {2, "A poem line.", false, true, true},
            {3, "More prose.", false}, {4, "Prose, then\na poem line.", false, false, true}, {5, "Last.", false}};
        QCOMPARE(text(poem, at(0, 0), at(4, 5), false), QStringLiteral("Prose.\nA poem line.\nMore prose. Prose, then\na poem line.\nLast."));
        // An end at a verse's start or a start at its end touches nothing there.
        QCOMPARE(touched(v, at(0, a.size()), at(2, 0)), qMakePair(1, 1));
        QCOMPARE(text(v, at(0, a.size()), at(2, 0), true, "R"), QStringLiteral("“") + b + QStringLiteral("”\n\n— R"));
        QVERIFY(text(v, at(0, 3), at(0, 3), true, "R").isEmpty());
        // Every ellipsis has a space toward the words and none toward the quotation marks.
        for (const auto &range : QList<QPair<TextSelection::Position, TextSelection::Position>>{{at(0, 4), at(0, 27)}, {at(0, 21), at(1, 7)}, {at(0, 9), at(2, 5)}, {at(1, 2), at(1, 9)}}) {
            const auto quote = text(v, range.first, range.second, true, "R");
            QVERIFY2(!quote.contains(QStringLiteral("“ ")) && !quote.contains(QStringLiteral(" ”")), qPrintable(quote));
            const auto body = quote.mid(1, quote.indexOf(QStringLiteral("”")) - 1);
            for (int i = body.indexOf(QChar(0x2026)); i >= 0; i = body.indexOf(QChar(0x2026), i + 1)) {
                if (i > 0) QVERIFY2(body.at(i - 1) == ' ', qPrintable(quote));
                if (i + 1 < body.size()) QVERIFY2(body.at(i + 1) == ' ', qPrintable(quote));
            }
            QVERIFY(!text(v, range.first, range.second, false).contains(QChar(0x2026)));
        }
    }
    void characterCopyOnTheChapter() {
        install(net); QVERIFY(bible->navigate("GEN 1"));
        const auto rows = bible->verses();
        const QString one = rows[0].toMap()["text"].toString(), two = rows[1].toMap()["text"].toString();
        const QString quote = bible->copyCharacters(1, 9, 2, 8);
        QCOMPARE(QGuiApplication::clipboard()->text(), quote);
        QVERIFY2(quote.startsWith(QStringLiteral("“… ")), qPrintable(quote));
        QVERIFY2(quote.contains(QStringLiteral(" ² ") + two.left(8).trimmed() + QStringLiteral(" …”")), qPrintable(quote));
        QVERIFY2(quote.endsWith(QStringLiteral(" 1:1–2")), qPrintable(quote));
        QCOMPARE(bible->copyCharacters(1, 9, 2, 8, true), one.mid(9) + ' ' + two.left(8));
        QVERIFY(bible->characterText(1, 0, 1, one.size()).startsWith(QStringLiteral("“") + one + QStringLiteral("”\n\n— ")));
        QVERIFY(bible->characterText(1, 0, 1, one.size()).endsWith(" 1:1"));
        QCOMPARE(bible->textMotion("w", 1, 0).value("offset").toInt(), one.indexOf(' ') + 1);
        QCOMPARE(bible->textMotion("l", 1, one.size() - 1).value("verse").toInt(), 2);
        QCOMPARE(bible->textMotion("G", 1, 0, 7).value("verse").toInt(), rows.last().toMap()["number"].toInt());
        const auto word = bible->textObject("iw", 1, 1, 1, 1);
        QCOMPARE(word["anchor"].toMap()["offset"].toInt(), 0);
        QCOMPARE(word["cursor"].toMap()["offset"].toInt(), one.indexOf(' ') - 1);
        QVERIFY(bible->copyCharacters(1, 3, 1, 3).isEmpty()); QVERIFY(!bible->error().isEmpty());
    }
    void quotationReferenceTypography() {
        QFETCH(QString,input); QFETCH(QString,label);
        const QString path = fixtures.filePath("quotation-engnet_usfm.zip");
        QVERIFY(AoFixture::write(path, AoFixture::complete("engnet", {
            AoFixture::book("GEN", 1, {AoFixture::chapter(1, {AoFixture::verse(1, {"First-word."}), AoFixture::verse(2, {"Last-word."})})}, "First-Book"),
            AoFixture::book("REV", 2, {AoFixture::chapter(3, {AoFixture::verse(14, {"First-word."}), AoFixture::verse(15, {"Middle-word."}), AoFixture::verse(22, {"Last-word."})}), AoFixture::chapter(4, {AoFixture::verse(1, {"Next-word."})})})
        })));
        install(path); QVERIFY2(bible->navigate(input),qPrintable(bible->error()));
        const QString quotation = bible->copySelection();
        QVERIFY2(quotation.endsWith(QStringLiteral("”\n\n— ")+label),qPrintable(quotation));
        QCOMPARE(QGuiApplication::clipboard()->text(),quotation);
        QVERIFY(quotation.contains("-word."));
        const QString plain = bible->copySelection(true);
        QVERIFY(plain.contains("-word.")); QVERIFY(!plain.contains(label));
        if (input.contains(':')) {
            // Copying a verse inside the range must use the same full-range label.
            QCOMPARE(bible->copyVerse(bible->selectedEnd()),quotation);
            QCOMPARE(QGuiApplication::clipboard()->text(),quotation);
        }
    }
    void chapterOpeningHeadingsSurviveReload() {
        const QString path = fixtures.filePath("headings-engnet_usfm.zip");
        QVERIFY(AoFixture::write(path, AoFixture::complete("engnet", {AoFixture::book("REV", 1, {
            AoFixture::chapter(3, {QJsonObject{{"type", "heading"}, {"content", QJsonArray{"To the Church in Sardis"}}}, AoFixture::verse(1, {"First."}),
                QJsonObject{{"type", "heading"}, {"content", QJsonArray{"To the Church in Philadelphia"}}}, AoFixture::verse(7, {"Later."})}),
            AoFixture::chapter(4, {QJsonObject{{"type", "heading"}, {"content", QJsonArray{"Revelation 4"}}}, AoFixture::verse(1, {"Next chapter."})}),
            AoFixture::chapter(5, {AoFixture::verse(1, {"No source heading."})})})})));
        install(path);
        for (int pass = 0; pass < 2; ++pass) {
            QVERIFY(bible->navigate("REV 3"));
            QCOMPARE(bible->verses().first().toMap()["heading"].toString(),QString("To the Church in Sardis"));
            QCOMPARE(bible->verses().last().toMap()["heading"].toString(),QString("To the Church in Philadelphia"));
            QVERIFY(bible->navigate("REV 4"));
            QCOMPARE(bible->verses().first().toMap()["heading"].toString(),QString("Revelation 4"));
            QVERIFY(bible->navigate("REV 5"));
            QVERIFY(bible->verses().first().toMap()["heading"].toString().isEmpty());
            if (!pass) {
                bible.reset(); bible = std::make_unique<BibleBackend>();
                QVERIFY2(bible->ready(),qPrintable(bible->error()));
            }
        }
    }
    void copyPreservesSourceLayoutAndUnicode() {
        const QString path = fixtures.filePath("copy-engojb_usfm.zip");
        QVERIFY(AoFixture::write(path, AoFixture::complete("engojb", {AoFixture::book("GEN", 1, {
            AoFixture::chapter(1, {AoFixture::verse(1, {QStringLiteral("Shalom   café 😀 é.")}), AoFixture::verse(2, {"Same\t paragraph."}),
                QJsonObject{{"type", "line_break"}}, AoFixture::verse(3, {"New paragraph."}), QJsonObject{{"type", "line_break"}},
                AoFixture::verse(4, {QJsonObject{{"text", "First   poetry line"}, {"poem", 1}}, QJsonObject{{"text", "Second line"}, {"poem", 2}}}),
                AoFixture::verse(5, {QJsonObject{{"text", "Poetry continuation."}, {"poem", 2}}}), QJsonObject{{"type", "line_break"}}, AoFixture::verse(6, {"Last paragraph."})}),
            AoFixture::chapter(2, {AoFixture::verse(1, {"Next chapter."})})}, "Bereishis")})));
        install(path);
        QVERIFY(bible->navigate("GEN 1"));
        const auto rows = bible->verses(); QCOMPARE(rows.size(),6);
        QStringList text;
        for (const auto &row : rows) text << row.toMap()["text"].toString();
        // The importer normalizes lexical whitespace; stored source is the copy
        // authority, including its internal poetry newline and Unicode units.
        QVERIFY(text[0].contains(QStringLiteral("café 😀 é")));
        QVERIFY(text[3].contains('\n'));
        QVERIFY(rows[2].toMap()["paragraphStart"].toBool());
        QVERIFY(rows[3].toMap()["poetry"].toBool());
        QVERIFY(rows[4].toMap()["poetry"].toBool());
        // Each line's poetry level (schema 5); prose lines are 0.
        QCOMPARE(rows[3].toMap()["indents"].toList(),QVariantList({1,2}));
        QCOMPARE(rows[4].toMap()["indents"].toList(),QVariantList({2}));
        QCOMPARE(rows[5].toMap()["indents"].toList(),QVariantList({0}));
        QVERIFY(!rows[4].toMap()["paragraphStart"].toBool());
        QCOMPARE(bible->copyVerse(4,true),text[3]);
        const QString expected = text[0]+' '+text[1]+"\n\n"+text[2]+"\n\n"+text[3]+'\n'+text[4]+"\n\n"+text[5];
        bible->selectVerses(1,6);
        QCOMPARE(bible->copySelection(true),expected);
        const QString quotation = QStringLiteral("“¹ ")+text[0]+QStringLiteral(" ² ")+text[1]
            +QStringLiteral("\n\n³ ")+text[2]+QStringLiteral("\n\n⁴ ")+text[3]
            +QStringLiteral("\n⁵ ")+text[4]+QStringLiteral("\n\n⁶ ")+text[5]
            +QStringLiteral("”\n\n— ")+bible->selectionReference();
        QCOMPARE(bible->copySelection(),quotation);
        bible->setReadingMode("block"); bible->setFocusMode(true); bible->setShowVerseNumbers(false);
        QCOMPARE(bible->copySelection(true),expected); QCOMPARE(bible->copySelection(),quotation);
        bible->moveSelection(1,true);
        const auto next = bible->verses().first().toMap()["text"].toString();
        QCOMPARE(bible->copySelection(true),expected+"\n\n"+next);
        QVERIFY(bible->copySelection().endsWith(QStringLiteral("\n\n¹ ")+next+QStringLiteral("”\n\n— ")+bible->selectionReference()));
        // The Desk quotes it laid out the same way, verses numbered.
        const QString spanning = bible->selectionReference(), spanningQuote = bible->copySelection();
        QCOMPARE(bible->appendToDesk(),spanning);
        const QString deskQuotation = deskQuote(spanning);
        QVERIFY(deskQuotation.startsWith("> " + spanningQuote.mid(1,spanningQuote.indexOf('\n') - 1)));
        QCOMPARE(bible->deskPage(),deskQuotation + "\n");
        // A fixture with a paragraph before every verse must copy paragraph breaks.
        const QString paragraphs = fixtures.filePath("copy-engnet_usfm.zip");
        QVERIFY(AoFixture::write(paragraphs, AoFixture::complete("engnet", {AoFixture::book("GEN", 1, {AoFixture::chapter(1, {AoFixture::verse(1, {"One."}), QJsonObject{{"type", "line_break"}}, AoFixture::verse(2, {"Two."})})})})));
        install(paragraphs); QVERIFY(bible->navigate("GEN 1:1-2"));
        QCOMPARE(bible->copySelection(true),QString("One.\n\nTwo."));
        // The page keeps its text whatever is installed since.
        QCOMPARE(bible->deskPage(),deskQuotation + "\n");
    }
    void schemaFourMigrationPreservesArchivesAndRollsBack() {
        const QString path = bible->dataDirectory()+"/personal.sqlite";
        bible.reset();
        const auto cleanup = qScopeGuard([] { QSqlDatabase::removeDatabase("migration"); });
        auto db = QSqlDatabase::addDatabase("QSQLITE","migration"); db.setDatabaseName(path); QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(schemaEight(q)); QVERIFY(q.exec("DROP TABLE annotations"));
        QVERIFY(q.exec("DROP TABLE desk")); // Schema 4 stores predate the Desk, History, and the ribbon.
        QVERIFY(q.exec("DROP TABLE history")); QVERIFY(q.exec("DROP TABLE recent_queries")); QVERIFY(q.exec("DROP TABLE ribbon"));
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
        QVERIFY(q.exec("PRAGMA user_version")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),11); q.finish();
        // The archived bookmark became the Bookmarked tag, in its own version.
        QVERIFY(q.exec("SELECT t.name,a.version_id FROM tag_assignments a JOIN tags t ON t.id=a.tag_id")); QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(),QString("Bookmarked")); QCOMPARE(q.value(1).toString(),QString("net-2.1"));
    }
    void schemaFiveDeskMigrationRollsBack() {
        install(net); QVERIFY(bible->navigate("John 3:16")); bible->setNoteText("Pre-desk note");
        const QString path = bible->dataDirectory()+"/personal.sqlite";
        bible.reset();
        const auto cleanup = qScopeGuard([] { QSqlDatabase::removeDatabase("desk-migration"); });
        auto db = QSqlDatabase::addDatabase("QSQLITE","desk-migration"); db.setDatabaseName(path); QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(schemaEight(q));
        QVERIFY(q.exec("DROP TABLE desk")); QVERIFY(q.exec("DROP TABLE history")); QVERIFY(q.exec("DROP TABLE recent_queries"));
        QVERIFY(q.exec("DROP TABLE ribbon")); QVERIFY(q.exec("PRAGMA user_version=5"));
        // A conflicting object makes the schema 6 step fail; nothing may change.
        QVERIFY(q.exec("CREATE VIEW desk AS SELECT 1 AS id"));
        bible = std::make_unique<BibleBackend>(); QVERIFY(!bible->error().isEmpty()); QVERIFY(bible->deskPage().isEmpty()); bible.reset();
        QVERIFY(q.exec("PRAGMA user_version")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),5); q.finish();
        QVERIFY(q.exec("SELECT note FROM annotations")); QVERIFY(q.next()); QCOMPARE(q.value(0).toString(),QString("Pre-desk note")); q.finish();
        QVERIFY(q.exec("DROP VIEW desk"));
        bible = std::make_unique<BibleBackend>(); QVERIFY2(bible->error().isEmpty(),qPrintable(bible->error()));
        QVERIFY(bible->ready()); QCOMPARE(bible->noteText(),QString("Pre-desk note"));
        QVERIFY(q.exec("PRAGMA user_version")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),11); q.finish();
        QVERIFY(bible->deskPage().isEmpty()); QCOMPARE(bible->appendToDesk(),QString("John 3:16"));
        QCOMPARE(bible->deskPage(),deskQuote("John 3:16") + "\n");
    }
    void schemaSixHistoryMigrationRollsBack() {
        install(net); QVERIFY(bible->navigate("John 3:16")); bible->setNoteText("Pre-history note");
        const QString quotation = deskQuote("John 3:16"), version = bible->versionId();
        const QString path = bible->dataDirectory()+"/personal.sqlite";
        bible.reset();
        const auto cleanup = qScopeGuard([] { QSqlDatabase::removeDatabase("history-migration"); });
        auto db = QSqlDatabase::addDatabase("QSQLITE","history-migration"); db.setDatabaseName(path); QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(schemaEight(q)); QVERIFY(deskRow(q,1,version,43,3,16,16));
        QVERIFY(q.exec("DROP TABLE history")); QVERIFY(q.exec("DROP TABLE recent_queries")); QVERIFY(q.exec("DROP TABLE ribbon"));
        QVERIFY(q.exec("PRAGMA user_version=6"));
        // recent_queries is created after history: a conflict there must roll back both.
        QVERIFY(q.exec("CREATE VIEW recent_queries AS SELECT 'x' AS query"));
        bible = std::make_unique<BibleBackend>(); QVERIFY(!bible->error().isEmpty()); QVERIFY(bible->history().isEmpty()); bible.reset();
        QVERIFY(q.exec("PRAGMA user_version")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),6); q.finish();
        QVERIFY(q.exec("SELECT COUNT(*) FROM sqlite_master WHERE name='history'")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),0); q.finish();
        QVERIFY(q.exec("SELECT note FROM annotations")); QVERIFY(q.next()); QCOMPARE(q.value(0).toString(),QString("Pre-history note")); q.finish();
        QVERIFY(q.exec("DROP VIEW recent_queries"));
        bible = std::make_unique<BibleBackend>(); QVERIFY2(bible->error().isEmpty(),qPrintable(bible->error()));
        QVERIFY(bible->ready()); QCOMPARE(bible->noteText(),QString("Pre-history note")); QCOMPARE(bible->deskPage(),quotation + "\n");
        QVERIFY(q.exec("PRAGMA user_version")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),11); q.finish();
        QVERIFY(bible->historyEnabled()); QVERIFY(bible->history().isEmpty()); QVERIFY(bible->recentQueries().isEmpty());
        QVERIFY(bible->recordHistory()); QCOMPARE(bible->history().size(),1);
    }
    void schemaSevenRibbonMigrationRollsBack() {
        install(net); QVERIFY(bible->navigate("John 3:16")); QVERIFY(bible->recordHistory());
        const QString path = bible->dataDirectory()+"/personal.sqlite";
        bible.reset();
        const auto cleanup = qScopeGuard([] { QSqlDatabase::removeDatabase("ribbon-migration"); });
        auto db = QSqlDatabase::addDatabase("QSQLITE","ribbon-migration"); db.setDatabaseName(path); QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(schemaEight(q)); QVERIFY(q.exec("DROP TABLE ribbon")); QVERIFY(q.exec("PRAGMA user_version=7"));
        QVERIFY(q.exec("CREATE VIEW ribbon AS SELECT 1 AS id"));
        bible = std::make_unique<BibleBackend>(); QVERIFY(!bible->error().isEmpty()); QVERIFY(bible->ribbon().isEmpty()); bible.reset();
        QVERIFY(q.exec("PRAGMA user_version")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),7); q.finish();
        QVERIFY(q.exec("DROP VIEW ribbon"));
        bible = std::make_unique<BibleBackend>(); QVERIFY2(bible->error().isEmpty(),qPrintable(bible->error()));
        QVERIFY(q.exec("PRAGMA user_version")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),11); q.finish();
        QCOMPARE(bible->history().size(),1); QVERIFY(bible->ribbon().isEmpty()); QVERIFY(bible->ribbonLocation().isEmpty());
        QVERIFY(bible->placeRibbon()); QCOMPARE(bible->ribbon()["reference"].toString(),QString("John 3:16"));
    }
    void schemaEightBookmarksBecomeTheBookmarkedTag() {
        install(net); const QString version = bible->versionId();
        QVERIFY(bible->navigate("John 3:16")); bible->setNoteText("Kept note"); const auto noted = bible->tagScope();
        QVERIFY(bible->navigate("John 3:17")); QVERIFY(bible->toggleHighlight()); const auto highlighted = bible->tagScope();
        QVERIFY(bible->navigate("John 3:20")); const auto tagged = bible->tagScope();
        // An equivalent name already in use is reused, spelling and all.
        const QString id = bible->createAndAssignTag(tagged,"bookmarked"); QVERIFY(!id.isEmpty());
        const QString path = bible->dataDirectory()+"/personal.sqlite";
        bible.reset();
        const auto cleanup = qScopeGuard([] { QSqlDatabase::removeDatabase("bookmark-migration"); });
        auto db = QSqlDatabase::addDatabase("QSQLITE","bookmark-migration"); db.setDatabaseName(path); QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(schemaEight(q));
        QVERIFY(q.exec("UPDATE annotations SET bookmark=1 WHERE start=16"));
        q.prepare("INSERT INTO annotations VALUES(43,3,18,19,1,0,'',43,3,?)"); q.addBindValue(version); QVERIFY(q.exec());
        // A failing step leaves schema 8 exactly as it was.
        QVERIFY(q.exec("CREATE VIEW annotations_v8 AS SELECT 1"));
        bible = std::make_unique<BibleBackend>(); QVERIFY(!bible->error().isEmpty()); bible.reset();
        QVERIFY(q.exec("PRAGMA user_version")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),8); q.finish();
        QVERIFY(q.exec("SELECT COUNT(*) FROM annotations WHERE bookmark")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),2); q.finish();
        QVERIFY(q.exec("SELECT COUNT(*) FROM tag_assignments")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),1); q.finish();
        QVERIFY(q.exec("DROP VIEW annotations_v8"));
        bible = std::make_unique<BibleBackend>(); QVERIFY2(bible->error().isEmpty(),qPrintable(bible->error()));
        QVERIFY(q.exec("PRAGMA user_version")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),11); q.finish();
        QCOMPARE(bible->tags().size(),1); QCOMPARE(bible->tags().first().toMap()["name"].toString(),QString("bookmarked"));
        auto bookmarkedAt = [&](QVariantMap scope) {
            const auto tags = bible->tagsForScope(scope);
            return tags.size() == 1 && tags.first().toMap()["id"].toString() == id;
        };
        auto bare = noted; bare["start"] = 18; bare["end"] = 19;
        QVERIFY(bookmarkedAt(noted)); QVERIFY(bookmarkedAt(bare)); QVERIFY(bookmarkedAt(tagged));
        QVERIFY(bible->tagsForScope(highlighted).isEmpty());
        // The bookmark-only row is gone; the note and the highlight stay, their times unknown.
        QVERIFY(q.exec("SELECT start,highlight,note,created,updated FROM annotations ORDER BY start")); QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(),16); QVERIFY(!q.value(1).toBool()); QCOMPARE(q.value(2).toString(),QString("Kept note"));
        QVERIFY(q.value(3).toString().isEmpty()); QVERIFY(q.value(4).toString().isEmpty()); QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(),17); QVERIFY(q.value(1).toBool()); QVERIFY(!q.next()); q.finish();
        QVERIFY(q.exec("SELECT COUNT(*) FROM pragma_table_info('annotations') WHERE name='bookmark'")); QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(),0); q.finish();
        QVERIFY(bible->openTaggedPassage(noted)); QCOMPARE(bible->noteText(),QString("Kept note"));
        // New marks are timed.
        bible->setNoteText("Kept note, edited");
        QVERIFY(q.exec("SELECT created,updated FROM annotations WHERE start=16")); QVERIFY(q.next());
        QVERIFY(q.value(0).toString().isEmpty()); QVERIFY(QDateTime::fromString(q.value(1).toString(),Qt::ISODateWithMs).isValid()); q.finish();
        QVERIFY(bible->setTag(highlighted,id,true));
        q.prepare("SELECT created FROM tag_assignments WHERE start=17"); QVERIFY(q.exec()); QVERIFY(q.next());
        QVERIFY(QDateTime::fromString(q.value(0).toString(),Qt::ISODateWithMs).isValid()); q.finish();
    }
    void schemaNineDeskBecomesItsPage() {
        install(net); const QString version = bible->versionId();
        const QString john = deskQuote("John 3:16"), genesis = deskQuote("GEN 1:4-6");
        QVERIFY(bible->navigate("John 3:17")); bible->setNoteText("Kept note");
        const QString path = bible->dataDirectory()+"/personal.sqlite";
        bible.reset();
        const auto cleanup = qScopeGuard([] { QSqlDatabase::removeDatabase("desk-page-migration"); });
        auto db = QSqlDatabase::addDatabase("QSQLITE","desk-page-migration"); db.setDatabaseName(path); QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(schemaNine(q));
        // In Desk order (positions), not insertion order; a version no longer
        // installed keeps its passage's reference.
        QVERIFY(deskRow(q,2,version,1,1,4,6)); QVERIFY(deskRow(q,1,version,43,3,16,16)); QVERIFY(deskRow(q,3,"eng-gone",19,23,1,2));
        // A failing step leaves schema 9 exactly as it was.
        QVERIFY(q.exec("CREATE VIEW desk_page AS SELECT 1 AS id"));
        bible = std::make_unique<BibleBackend>(); QVERIFY(!bible->error().isEmpty()); bible.reset();
        QVERIFY(q.exec("PRAGMA user_version")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),9); q.finish();
        QVERIFY(q.exec("SELECT COUNT(*) FROM desk")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),3); q.finish();
        QVERIFY(q.exec("DROP VIEW desk_page"));
        bible = std::make_unique<BibleBackend>(); QVERIFY2(bible->error().isEmpty(),qPrintable(bible->error()));
        QVERIFY(q.exec("PRAGMA user_version")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),11); q.finish();
        QVERIFY(q.exec("SELECT COUNT(*) FROM sqlite_master WHERE name='desk'")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),0); q.finish();
        QCOMPARE(bible->deskPage(),john + "\n\n" + genesis + QStringLiteral("\n\n> \u2014 Psalms 23:1–2 (eng-gone)\n"));
        QVERIFY(bible->navigate("John 3:17")); QCOMPARE(bible->noteText(),QString("Kept note"));
    }
    // Schema 11 takes book IDs past 80 (DAG 999:999 in dynamicBooksAndSourceOrder
    // proves one), keeping every mark, the ribbon, and History's ids.
    void schemaTenWidensTheBookRange() {
        install(net); QVERIFY(bible->navigate("John 3:16")); bible->setNoteText("Kept note");
        QVERIFY(bible->recordHistory()); QVERIFY(bible->placeRibbon());
        const QString path = bible->dataDirectory()+"/personal.sqlite";
        bible.reset();
        const auto cleanup = qScopeGuard([] { QSqlDatabase::removeDatabase("book-range-migration"); });
        auto db = QSqlDatabase::addDatabase("QSQLITE","book-range-migration"); db.setDatabaseName(path); QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(schemaTen(q));
        // History has used ids beyond its rows; none may be used again.
        QVERIFY(q.exec("UPDATE sqlite_sequence SET seq=50 WHERE name='history'")); QCOMPARE(q.numRowsAffected(),1);
        auto bounds = [&] {
            QStringList found;
            for (const QString table : {"annotations","history","ribbon"}) {
                q.prepare("SELECT sql FROM sqlite_master WHERE name=?"); q.addBindValue(table);
                if (!q.exec() || !q.next()) return QStringList{"missing " + table};
                found << (q.value(0).toString().contains("book INTEGER NOT NULL CHECK(book BETWEEN 1 AND 80)") ? "80" : q.value(0).toString().contains("CHECK(book BETWEEN 1 AND 999)") && q.value(0).toString().contains("CHECK(endBook BETWEEN 1 AND 999)") ? "999" : "other");
            }
            q.finish();
            return found;
        };
        QCOMPARE(bounds(),QStringList({"80","80","80"}));
        // A failing step leaves schema 10 exactly as it was.
        QVERIFY(q.exec("CREATE VIEW ribbon_v10 AS SELECT 1 AS id"));
        bible = std::make_unique<BibleBackend>(); QVERIFY(!bible->error().isEmpty()); bible.reset();
        QVERIFY(q.exec("PRAGMA user_version")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),10); q.finish();
        QCOMPARE(bounds(),QStringList({"80","80","80"}));
        QVERIFY(q.exec("DROP VIEW ribbon_v10"));
        bible = std::make_unique<BibleBackend>(); QVERIFY2(bible->error().isEmpty(),qPrintable(bible->error()));
        QVERIFY(q.exec("PRAGMA user_version")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),11); q.finish();
        QCOMPARE(bounds(),QStringList({"999","999","999"}));
        QVERIFY(q.exec("SELECT count(*) FROM sqlite_master WHERE name LIKE '%_v10'")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),0); q.finish();
        QVERIFY(bible->navigate("John 3:16")); QCOMPARE(bible->noteText(),QString("Kept note"));
        QCOMPARE(bible->history().size(),1); QCOMPARE(bible->ribbon()["reference"].toString(),QString("John 3:16"));
        QVERIFY(bible->navigate("John 3:17")); QVERIFY(bible->recordHistory());
        QCOMPARE(bible->history().first().toMap()["id"].toInt(),51);
    }
    // The command line's passages and --translation: nothing moves the
    // reading place or changes the version you read.
    void readPassageLeavesTheReaderAlone() {
        install(kjv); install(net); QVERIFY(bible->navigate("John 3:16"));
        const QString verse = bible->copySelection(), plain = bible->copySelection(true);
        QVERIFY(bible->navigate("John 3:16-17")); const QString range = bible->copySelection();
        QVERIFY(bible->navigate("GEN 1:2")); const QString place = bible->selectionReference(), reading = bible->versionId();
        QSignalSpy navigation(bible.get(),&BibleBackend::navigationChanged);
        const auto passage = bible->readPassage("John 3:16");
        QCOMPARE(passage.value("quotation").toString(),verse); QCOMPARE(passage.value("plain").toString(),plain);
        QCOMPARE(passage.value("reference").toString(),QString("John 3:16")); QCOMPARE(passage.value("versionId").toString(),reading);
        QCOMPARE(passage.value("abbreviation").toString(),bible->versionAbbreviation());
        const auto verses = passage.value("verses").toList();
        QCOMPARE(verses.size(),1); QCOMPARE(verses.first().toMap().value("verse").toInt(),16);
        QCOMPARE(verses.first().toMap().value("book").toString(),QString("John"));
        QCOMPARE(bible->readPassage("jn316").value("quotation").toString(),verse); // Compact, as the palette reads it.
        QVERIFY(bible->readPassage("jn316",true).contains("error")); // Strict: exact forms only.
        QCOMPARE(bible->readPassage("John 3:16-17").value("quotation").toString(),range);
        const auto chapter = bible->readPassage("John 3");
        QVERIFY(chapter.value("quotation").toString().endsWith(QStringLiteral("\u2014 John 3"))); QCOMPARE(chapter.value("verses").toList().size(),36);
        QVERIFY(bible->readPassage("Nowhere at all").contains("error"));
        QCOMPARE(navigation.count(),0); QCOMPARE(bible->selectionReference(),place);
        // Another version, by ID or abbreviation, read in this backend alone.
        QCOMPARE(bible->versionIdFor("kjv+"),QString("eng-kjv")); QCOMPARE(bible->versionIdFor(" ENG-KJV"),QString("eng-kjv"));
        QVERIFY(bible->versionIdFor("xyz").isEmpty());
        QCOMPARE(bible->searchEditions(false).size(),1); QCOMPARE(bible->searchEditions(true).size(),2);
        QVERIFY2(bible->readVersion("eng-kjv"),qPrintable(bible->error())); QCOMPARE(bible->versionId(),QString("eng-kjv"));
        QCOMPARE(bible->readPassage("jn316").value("versionId").toString(),QString("eng-kjv"));
        QCOMPARE(bible->searchEditions(true).first().versionId,QString("eng-kjv"));
        bible.reset(); bible = std::make_unique<BibleBackend>();
        QCOMPARE(bible->versionId(),reading); QCOMPARE(bible->selectionReference(),place);
    }
    void ribbonPlacesStaysAndCrossesVersions() {
        // Nothing to place it in before a version is installed.
        QVERIFY(!bible->placeRibbon()); QVERIFY(!bible->error().isEmpty()); QVERIFY(bible->ribbon().isEmpty());
        install(net); BibleBackend peer;
        QSignalSpy changed(&peer,&BibleBackend::ribbonChanged);
        // A chapter with its reading position.
        QVERIFY(bible->navigate("MRK 7")); bible->setReadingPosition(0.4); QVERIFY(bible->placeRibbon());
        QCOMPARE(changed.count(),1);
        auto ribbon = peer.ribbon();
        QCOMPARE(ribbon["reference"].toString(),QString("Mark 7")); QCOMPARE(ribbon["chapterReference"].toString(),QString("Mark 7"));
        QCOMPARE(ribbon["start"].toInt(),0); QCOMPARE(ribbon["position"].toDouble(),0.4);
        QVERIFY(QDateTime::fromString(ribbon["placedAt"].toString(),Qt::ISODateWithMs).isValid());
        // It stays where it was placed while the reader moves on.
        bible->nextChapter(); QVERIFY(peer.navigate("GEN 1:3"));
        QCOMPARE(bible->ribbon()["reference"].toString(),QString("Mark 7"));
        auto location = peer.ribbonLocation();
        QCOMPARE(location["versionId"].toString(),QString("engnet")); QVERIFY(!location.contains("placedAt"));
        QVERIFY(peer.openLocation(location)); QCOMPARE(peer.passage(),QString("Mark 7"));
        QCOMPARE(peer.selectedStart(),0); QCOMPARE(peer.readingPosition(),0.4);
        // A verse range, kept across restarts.
        QVERIFY(bible->navigate("John 3:16-17")); QVERIFY(bible->placeRibbon()); QCOMPARE(changed.count(),2);
        QCOMPARE(peer.ribbon()["reference"].toString(),QString("John 3:16–17"));
        QCOMPARE(peer.ribbon()["chapterReference"].toString(),QString("John 3"));
        bible.reset(); bible = std::make_unique<BibleBackend>();
        QCOMPARE(bible->ribbon()["reference"].toString(),QString("John 3:16–17"));
        // Version-independent: another version opens it in itself, with its own names.
        install(ojb); QCOMPARE(bible->versionId(),QString("engojb"));
        location = bible->ribbonLocation(); QCOMPARE(location["versionId"].toString(),QString("engojb"));
        QVERIFY(bible->openLocation(location)); QCOMPARE(bible->selectedStart(),16); QCOMPARE(bible->selectedEnd(),17);
        QCOMPARE(bible->ribbon()["versionId"].toString(),QString("engnet"));
        // A place this version lacks (MRK 7:16 is KJV's alone) opens where it was placed.
        install(kjv); QVERIFY(bible->navigate("MRK 7:16")); QVERIFY(bible->placeRibbon());
        QVERIFY(bible->switchVersion("engnet")); location = bible->ribbonLocation();
        QCOMPARE(location["versionId"].toString(),QString("eng-kjv")); QCOMPARE(location["start"].toInt(),16);
        // ...and with that version gone, its chapter here.
        QVERIFY(bible->removeVersion("eng-kjv",false));
        location = bible->ribbonLocation();
        QCOMPARE(location["versionId"].toString(),bible->versionId()); QCOMPARE(location["start"].toInt(),0);
        QCOMPARE(location["chapter"].toInt(),7); QVERIFY(bible->openLocation(location));
        // History and backups leave it alone.
        QVERIFY(bible->clearHistory()); QVERIFY(!bible->ribbon().isEmpty());
        // Removed, it is gone for every window and stays gone, until placed again.
        BibleBackend other; QSignalSpy otherChanged(&other,&BibleBackend::ribbonChanged);
        QVERIFY(bible->removeRibbon()); QVERIFY(bible->error().isEmpty());
        QVERIFY(bible->ribbon().isEmpty()); QVERIFY(bible->ribbonLocation().isEmpty());
        QCOMPARE(otherChanged.count(),1); QVERIFY(other.ribbon().isEmpty());
        bible->nextChapter(); QVERIFY(bible->ribbon().isEmpty());
        bible.reset(); bible = std::make_unique<BibleBackend>(); QVERIFY(bible->ribbon().isEmpty());
        QVERIFY(bible->removeRibbon()); QVERIFY(bible->ribbon().isEmpty()); // Nothing to remove is no error.
        QVERIFY(bible->placeRibbon()); QVERIFY(!other.ribbon().isEmpty());
    }
    void historyUpsertOrderCapAndRemoval() {
        install(net);
        QSignalSpy changed(bible.get(),&BibleBackend::historyChanged);
        QVERIFY(bible->navigate("GEN 1:2")); bible->setReadingPosition(0.25); QVERIFY(bible->recordHistory());
        QVERIFY(bible->navigate("John 3")); QVERIFY(!bible->tagScope().size());
        const auto chapter = bible->currentLocation();
        QCOMPARE(chapter["start"].toInt(),0); QCOMPARE(chapter["end"].toInt(),0);
        QCOMPARE(chapter["endChapter"].toInt(),3); QVERIFY(bible->recordHistory());
        QVERIFY(bible->navigate("John 3:16-17")); QVERIFY(bible->recordHistory());
        QCOMPARE(changed.count(),3);
        auto refs = [this] { QStringList r; for (const auto &row : bible->history()) r << row.toMap()["reference"].toString(); return r; };
        QCOMPARE(refs(),QStringList({"John 3:16–17","John 3","Genesis 1:2"}));
        auto top = bible->history().first().toMap();
        QCOMPARE(top["versionAbbreviation"].toString(),bible->versionAbbreviation()); QVERIFY(top["available"].toBool());
        QVERIFY(QDateTime::fromString(top["visitedAt"].toString(),Qt::ISODateWithMs).isValid());
        // A chapter row knows its verses (to copy or add the whole chapter).
        const auto chapterRow = bible->history()[1].toMap();
        QVERIFY(chapterRow["available"].toBool()); QCOMPARE(chapterRow["firstVerse"].toInt(),1); QVERIFY(chapterRow["lastVerse"].toInt() > 1);
        // A repeat visit moves to the top without duplicating, refreshing the position.
        auto genesis = bible->history().last().toMap(); QCOMPARE(genesis["position"].toDouble(),0.25);
        genesis["position"] = 0.5; QVERIFY(bible->recordHistory(genesis));
        QCOMPARE(refs(),QStringList({"Genesis 1:2","John 3:16–17","John 3"}));
        QCOMPARE(bible->history().first().toMap()["position"].toDouble(),0.5);
        QCOMPARE(bible->history().first().toMap()["id"],genesis["id"]);
        // Restoring a location brings back the range and position.
        QVERIFY(bible->openLocation(genesis)); QCOMPARE(bible->selectionReference(),QString("Genesis 1:2"));
        QCOMPARE(bible->readingPosition(),0.5);
        QVERIFY(bible->openLocation(chapter)); QCOMPARE(bible->selectedStart(),0); QCOMPARE(bible->passage(),QString("John 3"));
        // Invalid locations are rejected.
        auto broken = genesis; broken["book"] = int(BibleBooks::canon().size()) + 1; QVERIFY(!bible->recordHistory(broken));
        broken = genesis; broken["position"] = 2; QVERIFY(!bible->recordHistory(broken));
        QCOMPARE(bible->history().size(),3); bible->clearError();
        // Removing one entry.
        QVERIFY(bible->removeHistoryItem(genesis["id"].toInt())); QCOMPARE(refs(),QStringList({"John 3:16–17","John 3"}));
        QVERIFY(!bible->removeHistoryItem(genesis["id"].toInt())); bible->clearError();
        // The cap keeps the newest HistoryLimit entries.
        QVariantMap location = bible->currentLocation();
        for (int i = 0; i < BibleBackend::HistoryLimit + 5; ++i) {
            location["start"] = location["end"] = 1 + i % 900;
            location["chapter"] = location["endChapter"] = 1 + i / 900 + 10;
            QVERIFY(bible->recordHistory(location));
        }
        QCOMPARE(bible->history().size(),BibleBackend::HistoryLimit);
        QCOMPARE(bible->history().first().toMap()["start"].toInt(),1 + (BibleBackend::HistoryLimit + 4) % 900);
        QVERIFY(!refs().contains("John 3"));
        // Persistent across restarts.
        const auto saved = bible->history();
        bible.reset(); bible = std::make_unique<BibleBackend>(); QCOMPARE(bible->history(),saved);
    }
    void historyPeersDisableAndRecentQueries() {
        install(net); BibleBackend peer;
        QSignalSpy peerChanged(&peer,&BibleBackend::historyChanged);
        QVERIFY(bible->navigate("John 3:16")); QVERIFY(bible->recordHistory());
        QCOMPARE(peerChanged.count(),1); QCOMPARE(peer.history(),bible->history());
        QVERIFY(peer.navigate("GEN 1:1")); QVERIFY(peer.recordHistory()); QCOMPARE(bible->history().size(),2);
        // Recent queries: newest first, deduplicated, capped.
        for (const QString query : {"light","love","light","grace","faith","hope","peace"}) QVERIFY(bible->recordQuery(query));
        QVERIFY(!bible->recordQuery("   "));
        QCOMPARE(bible->recentQueries(),QStringList({"peace","hope","faith","grace","light"}));
        QCOMPARE(peer.recentQueries(),bible->recentQueries());
        // Clear removes both lists everywhere.
        QVERIFY(peer.clearHistory()); QVERIFY(bible->history().isEmpty()); QVERIFY(bible->recentQueries().isEmpty());
        QVERIFY(bible->recordHistory()); QVERIFY(bible->recordQuery("light"));
        // Disabling clears and stops recording in every window; persisted.
        QVERIFY(bible->setHistoryEnabled(false));
        QVERIFY(!bible->historyEnabled()); QVERIFY(!peer.historyEnabled());
        QVERIFY(peer.history().isEmpty()); QVERIFY(peer.recentQueries().isEmpty());
        QVERIFY(!peer.recordHistory()); QVERIFY(!bible->recordQuery("love")); QVERIFY(bible->error().isEmpty());
        QVERIFY(bible->history().isEmpty()); QVERIFY(bible->recentQueries().isEmpty());
        BibleBackend later; QVERIFY(!later.historyEnabled());
        QVERIFY(peer.setHistoryEnabled(true)); QVERIFY(bible->historyEnabled()); QVERIFY(later.historyEnabled());
        QVERIFY(bible->recordHistory()); QCOMPARE(later.history().size(),1);
    }
    void historyIsNotBackedUp() {
        install(net); QVERIFY(bible->navigate("John 3:16")); QVERIFY(!bible->appendToDesk().isEmpty());
        const auto before = QUrl::fromLocalFile(session->filePath("before.json")); QVERIFY(bible->exportBackup(before));
        QVERIFY(bible->recordHistory()); QVERIFY(bible->recordQuery("light"));
        const auto after = QUrl::fromLocalFile(session->filePath("after.json")); QVERIFY(bible->exportBackup(after));
        auto read = [](const QUrl &url) { QFile f(url.toLocalFile()); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); };
        const auto a = read(before), b = read(after);
        auto withoutStamp = [](const QByteArray &bytes) { auto o = QJsonDocument::fromJson(bytes).object(); o.remove("created"); return o; };
        QVERIFY(!a.isEmpty()); QCOMPARE(withoutStamp(b),withoutStamp(a)); // Only the export time differs.
        const auto root = QJsonDocument::fromJson(b).object();
        QCOMPARE(root["version"].toInt(),7); QVERIFY(!root.contains("history")); QVERIFY(!b.contains("light"));
        // Restoring a backup leaves History alone.
        QVERIFY2(bible->importBackup(after),qPrintable(bible->error())); QCOMPARE(bible->history().size(),1);
        QCOMPARE(bible->recentQueries(),QStringList({"light"}));
    }
    void deskPageAppendEditUndoAndPersistence() {
        install(net);
        QVERIFY(bible->appendToDesk().isEmpty()); QVERIFY(!bible->error().isEmpty()); // No selection yet.
        QCOMPARE(bible->deskPage(),QString());
        QSignalSpy changed(bible.get(),&BibleBackend::deskChanged);
        // A verse is quoted as its text; a passage of several, verses numbered.
        QVERIFY(bible->navigate("John 3:16")); const QString john = bible->copySelection(true);
        QCOMPARE(bible->appendToDesk(),QString("John 3:16")); QCOMPARE(changed.count(),1);
        const QString first = "> " + john + QStringLiteral("\n>\n> \u2014 John 3:16 (NET)\n");
        QCOMPARE(bible->deskPage(),first); QCOMPARE(deskQuote("John 3:16") + "\n",first);
        // Another passage goes at the end, a blank line apart, without navigating.
        QVERIFY(bible->navigate("GEN 1:4-6")); const auto genesis = bible->tagScope(); QVERIFY(bible->navigate("John 3:1"));
        QCOMPARE(bible->appendToDesk(genesis),QString("Genesis 1:4–6"));
        QCOMPARE(bible->selectionReference(),QString("John 3:1"));
        const QString both = first + "\n" + deskQuote("GEN 1:4-6") + "\n";
        QCOMPARE(bible->deskPage(),both); QCOMPARE(changed.count(),2);
        QVERIFY(both.contains(QStringLiteral("> \u2074 ")) && both.contains(QStringLiteral("\n>\n> \u2014 Genesis 1:4–6 (NET)\n")));
        // u takes the last addition off, and redo puts it back.
        QCOMPARE(bible->undoLabel(),QString("addition of Genesis 1:4–6 to the Desk"));
        QVERIFY(bible->undo()); QCOMPARE(bible->deskPage(),first);
        QVERIFY(bible->redo()); QCOMPARE(bible->deskPage(),both);
        // The page saves only over the text its editor started from.
        QVERIFY(!bible->setDeskPage("Mine",first)); QVERIFY(bible->error().contains("changed in another window"));
        QCOMPARE(bible->deskPage(),both);
        QVERIFY(bible->setDeskPage("My thoughts",both)); QCOMPARE(bible->deskPage(),QString("My thoughts"));
        const int saves = changed.count();
        QVERIFY(bible->setDeskPage("My thoughts","My thoughts")); QCOMPARE(changed.count(),saves); // Unchanged: nothing written.
        // Written on since, the addition stays: undo says why and moves past it.
        QVERIFY(!bible->undo()); QVERIFY(bible->error().contains("Desk has changed")); QCOMPARE(bible->deskPage(),QString("My thoughts"));
        // An addition after the reader's own words starts a new paragraph.
        QVERIFY(bible->navigate("John 3:16")); QVERIFY(!bible->appendToDesk().isEmpty());
        QCOMPARE(bible->deskPage(),"My thoughts\n\n" + first);
        // Control characters and pages past 4 MiB are refused.
        const QString kept = bible->deskPage();
        QVERIFY(!bible->setDeskPage(QString("Bad\u0001"),kept)); QVERIFY(!bible->setDeskPage(QString(4 * 1024 * 1024 + 1,'x'),kept));
        QCOMPARE(bible->deskPage(),kept);
        bible.reset(); bible = std::make_unique<BibleBackend>(); QVERIFY2(bible->ready(),qPrintable(bible->error()));
        QCOMPARE(bible->deskPage(),kept);
    }
    void deleteNoteScopeKeepsMarksAndTags() {
        install(net); BibleBackend peer;
        QVERIFY(bible->navigate("MRK 7:15")); bible->moveSelection(2,true);
        const auto scope = bible->tagScope();
        bible->setNoteText("A long thought"); QVERIFY(bible->toggleHighlight());
        QVERIFY(!bible->createAndAssignTag(scope,"Kept").isEmpty());
        // A note on an overlapping, different range survives.
        bible->selectVerses(15,15); bible->setNoteText("Other range");
        bible->selectVerses(15,17);
        QSignalSpy peerChanged(&peer,&BibleBackend::collectionChanged);
        QVERIFY2(bible->deleteNoteScope(scope),qPrintable(bible->error()));
        QVERIFY(bible->noteText().isEmpty());
        QVERIFY(bible->selectionHighlighted());
        QCOMPARE(bible->tagsForScope(scope).size(),1);
        QVERIFY(peerChanged.count() > 0);
        QVariantMap row;
        for (const auto &v : peer.collection()) if (v.toMap()["reference"] == scope["reference"]) row = v.toMap();
        QVERIFY(row["highlight"].toBool()); QVERIFY(row["note"].toString().isEmpty());
        bible->selectVerses(15,15); QCOMPARE(bible->noteText(),QString("Other range"));
        // No note left on the exact range: nothing to delete.
        QVERIFY(!bible->deleteNoteScope(scope)); QVERIFY(!bible->error().isEmpty());
        // A row with only a note is removed entirely.
        const auto single = bible->tagScope();
        QVERIFY2(bible->deleteNoteScope(single),qPrintable(bible->error()));
        for (const auto &v : bible->collection()) QVERIFY(v.toMap()["reference"] != single["reference"]);
        QCOMPARE(bible->collection().size(),1);
    }
    void undoRedoAnnotationChanges() {
        install(net); BibleBackend peer;
        QVERIFY(!bible->canUndo()); QVERIFY(!bible->canRedo());
        QVERIFY(!bible->undo()); QVERIFY(!bible->error().isEmpty()); bible->clearError();
        QSignalSpy changed(bible.get(),&BibleBackend::undoChanged);
        // Toggling a flag can split rows: undo puts back the exact rows.
        QVERIFY(bible->navigate("John 3:16-18")); QVERIFY(bible->toggleHighlight());
        QCOMPARE(bible->undoLabel(),QString("highlight on John 3:16–18")); QVERIFY(changed.count() > 0);
        bible->selectVerses(17,17); QVERIFY(bible->toggleHighlight());
        QCOMPARE(bible->undoLabel(),QString("highlight removal on John 3:17")); QCOMPARE(bible->collection().size(),2);
        QVERIFY2(bible->undo(),qPrintable(bible->error()));
        QCOMPARE(bible->collection().size(),1); QVERIFY(bible->selectionHighlighted());
        QCOMPARE(peer.collection(),bible->collection()); QVERIFY(!peer.canUndo()); // Per window.
        QCOMPARE(bible->redoLabel(),QString("highlight removal on John 3:17"));
        QVERIFY(bible->undo()); QVERIFY(bible->collection().isEmpty()); QVERIFY(!bible->canUndo());
        QVERIFY(bible->redo()); QVERIFY(bible->redo()); QCOMPARE(bible->collection().size(),2); QVERIFY(!bible->canRedo());
        // A note's saves are one step until its group closes.
        bible->selectVerses(16,16); const auto scope = bible->tagScope();
        bible->setNoteText("F"); bible->setNoteText("Fi"); bible->setNoteText("First");
        QCOMPARE(bible->undoLabel(),QString("note on John 3:16"));
        bible->closeUndoGroup(); bible->setNoteText("First, then more");
        QVERIFY(bible->undo()); QCOMPARE(bible->noteText(),QString("First"));
        QVERIFY(bible->undo()); QVERIFY(bible->noteText().isEmpty()); QVERIFY(bible->selectionHighlighted());
        QCOMPARE(bible->undoLabel(),QString("highlight removal on John 3:17")); QVERIFY(bible->canRedo());
        // Typing back to the saved text leaves no step, but any change clears redo.
        bible->setNoteText("Gone"); bible->setNoteText("");
        QCOMPARE(bible->undoLabel(),QString("highlight removal on John 3:17")); QVERIFY(!bible->canRedo());
        QVERIFY(bible->toggleHighlight()); QCOMPARE(bible->undoLabel(),QString("highlight removal on John 3:16"));
        // A tag the step created goes with its undo, and comes back (same ID) with redo.
        const QString grace = bible->createAndAssignTag(scope,"Grace"); QVERIFY(!grace.isEmpty());
        QCOMPARE(bible->undoLabel(),QString("tag Grace on John 3:16"));
        QVERIFY(bible->undo()); QVERIFY(bible->tagsForScope(scope).isEmpty()); QVERIFY(bible->tags().isEmpty());
        QVERIFY(bible->redo()); QCOMPARE(bible->tagsForScope(scope).value(0).toMap()["id"].toString(),grace);
        const QString hope = bible->createAndAssignTag(scope,"Hope"); QVERIFY(!hope.isEmpty());
        QVERIFY(bible->setTags(scope,{grace,hope},false)); QVERIFY(bible->tagsForScope(scope).isEmpty());
        QCOMPARE(bible->undoLabel(),QString("removal of 2 tags from John 3:16"));
        QVERIFY(bible->undo()); QCOMPARE(bible->tagsForScope(scope).size(),2);
        QVERIFY(bible->setTag(scope,hope,false)); QCOMPARE(bible->undoLabel(),QString("removal of Hope from John 3:16"));
        QVERIFY(bible->undo());
        // Deleting restores marks, note and tags exactly, here and in the peer.
        bible->setNoteText("Kept"); const auto saved = bible->annotationSnapshot(scope);
        QVERIFY(bible->deleteAnnotationScope(scope)); QCOMPARE(bible->undoLabel(),QString("deletion of John 3:16"));
        QVERIFY(!bible->annotationSnapshot(scope).value("ok").toBool());
        QVERIFY(bible->undo()); QCOMPARE(bible->annotationSnapshot(scope),saved);
        QCOMPARE(bible->noteText(),QString("Kept")); QCOMPARE(peer.collection(),bible->collection());
        QVERIFY(bible->deleteNoteScope(scope)); QCOMPARE(bible->undoLabel(),QString("note deletion on John 3:16"));
        QVERIFY(bible->undo()); QCOMPARE(bible->annotationSnapshot(scope),saved);
        // Steps live in memory only.
        bible.reset(); bible = std::make_unique<BibleBackend>(); QVERIFY(!bible->canUndo());
    }
    void highlightsPaintJoinAndErase() {
        install(net);
        const auto highlights = [&]() {
            QStringList out;
            for (const auto &row : bible->collection()) if (row.toMap()["highlight"].toBool()) out << row.toMap()["reference"].toString();
            return out;
        };
        QVERIFY(bible->navigate("John 3:16-17")); bible->setNoteText("Kept on its range"); QVERIFY(bible->toggleHighlight());
        QCOMPARE(highlights(),QStringList{QString::fromUtf8("John 3:16–17")});
        // Painting beside a highlight joins it; the note keeps its own range.
        QVERIFY(bible->navigate("John 3:18-19")); QVERIFY(bible->toggleHighlight());
        QCOMPARE(highlights(),QStringList{QString::fromUtf8("John 3:16–19")});
        QCOMPARE(bible->undoLabel(),QString::fromUtf8("highlight on John 3:18–19"));
        // Erasing inside leaves what lies outside.
        QVERIFY(bible->navigate("John 3:17-18")); QVERIFY(bible->selectionHighlighted()); QVERIFY(bible->toggleHighlight());
        QCOMPARE(highlights(),(QStringList{"John 3:16","John 3:19"}));
        QVERIFY(!bible->selectionHighlighted());
        QCOMPARE(bible->undoLabel(),QString::fromUtf8("highlight removal on John 3:17–18"));
        // Painting over both joins them, and what lies between, into one.
        QVERIFY(bible->navigate("John 3:15-20")); QVERIFY(bible->toggleHighlight());
        QCOMPARE(highlights(),QStringList{QString::fromUtf8("John 3:15–20")});
        // Partly painted: x paints the rest, never erases.
        QVERIFY(bible->navigate("John 3:20-22")); QVERIFY(!bible->selectionHighlighted()); QVERIFY(bible->toggleHighlight());
        QCOMPARE(highlights(),QStringList{QString::fromUtf8("John 3:15–22")});
        // Each stroke is one undo step, the rows put back exactly.
        QVERIFY(bible->undo()); QCOMPARE(highlights(),QStringList{QString::fromUtf8("John 3:15–20")});
        QVERIFY(bible->undo()); QCOMPARE(highlights(),(QStringList{"John 3:16","John 3:19"}));
        QVERIFY(bible->redo()); QCOMPARE(highlights(),QStringList{QString::fromUtf8("John 3:15–20")});
        // The note never moved.
        QVERIFY(bible->navigate("John 3:16-17")); QCOMPARE(bible->noteText(),QString("Kept on its range"));
        // Verses next to each other join across a chapter's end.
        QVERIFY(bible->navigate("John 3:36")); QVERIFY(bible->toggleHighlight());
        QVERIFY(bible->navigate("John 4:1")); QVERIFY(bible->toggleHighlight());
        QVERIFY(highlights().contains(QString::fromUtf8("John 3:36–4:1")));
        // Erasing the whole of a highlight leaves nothing behind.
        QVERIFY(bible->navigate("John 3:36-4:1")); QVERIFY(bible->toggleHighlight());
        QVERIFY(!highlights().contains(QString::fromUtf8("John 3:36–4:1")));
        QVERIFY(!bible->navigate("John 4:1") || !bible->selectionHighlighted());
    }
    void marksAcrossVersionsAndScopeNotes() {
        install(net); BibleBackend peer; QSignalSpy peerMarks(&peer,&BibleBackend::marksChanged);
        QVERIFY(bible->navigate("John 3:16-17")); bible->setNoteText("Net note"); QVERIFY(bible->toggleHighlight());
        QVERIFY(bible->navigate("John 3:3")); const auto tagged = bible->tagScope();
        QVERIFY(!bible->createAndAssignTag(tagged,"Born").isEmpty()); QVERIFY(!bible->createAndAssignTag(tagged,"Again").isEmpty());
        QVERIFY(bible->navigate("Genesis 1:1")); bible->setNoteText("In the beginning");
        install(kjv); QVERIFY(bible->navigate("John 3:16")); const auto kjvScope = bible->tagScope();
        bible->setNoteText("Kjv note");
        // One row per exact range, every version, in passage order; the
        // reading version (KJV) first among equal places.
        auto marks = bible->marks();
        QStringList seen;
        for (const auto &value : marks) {
            const auto mark = value.toMap();
            seen << mark["versionAbbreviation"].toString() + " " + mark["reference"].toString();
        }
        QCOMPARE(seen.size(),4);
        QCOMPARE(seen[0].section(' ',1),QString("Genesis 1:1"));
        QCOMPARE(seen[1].section(' ',1),QString("John 3:3"));
        QVERIFY(seen[2].endsWith(QString("John 3:16"))); QVERIFY(seen[3].endsWith(QString::fromUtf8("John 3:16–17")));
        auto net316 = marks[3].toMap();
        QVERIFY(!net316["active"].toBool()); QCOMPARE(net316["versionId"].toString(),QString("engnet"));
        QVERIFY(net316["highlight"].toBool()); QCOMPARE(net316["note"].toString(),QString("Net note"));
        QVERIFY(QDateTime::fromString(net316["created"].toString(),Qt::ISODateWithMs).isValid());
        QVERIFY(!net316["excerpt"].toString().isEmpty());
        QVERIFY(marks[2].toMap()["active"].toBool());
        const auto born = marks[1].toMap();
        QCOMPARE(born["tags"].toList().size(),2);
        QCOMPARE(born["tags"].toList().first().toMap()["name"].toString(),QString("Again")); // Name order.
        QVERIFY(born["note"].toString().isEmpty()); QVERIFY(born["created"].toString().isEmpty());
        // Notes in place, in another version than the reading one.
        QCOMPARE(bible->savedNote(net316),QString("Net note"));
        peerMarks.clear();
        QVERIFY2(bible->setScopeNote(net316,"Net note, longer","Net note"),qPrintable(bible->error()));
        QCOMPARE(bible->savedNote(net316),QString("Net note, longer"));
        QCOMPARE(bible->versionId(),QString("eng-kjv")); QCOMPARE(bible->noteText(),QString("Kjv note"));
        QVERIFY(peerMarks.count() > 0);
        // Saves continue one undo step; a stale baseline is refused, unchanged.
        QVERIFY(bible->setScopeNote(net316,"Net note, longer still","Net note, longer"));
        QVERIFY(!bible->setScopeNote(net316,"Lost","Net note"));
        QVERIFY(bible->error().contains("another window")); bible->clearError();
        QCOMPARE(bible->savedNote(net316),QString("Net note, longer still"));
        QCOMPARE(bible->undoLabel(),QString::fromUtf8("note on John 3:16–17"));
        bible->closeUndoGroup();
        QVERIFY(bible->undo()); QCOMPARE(bible->savedNote(net316),QString("Net note"));
        // A tags-only range gets a note; emptied again, only its tags stay.
        QVERIFY(bible->setScopeNote(born,"Born again",""));
        QCOMPARE(bible->savedNote(born),QString("Born again"));
        QVERIFY(bible->setScopeNote(born,"","Born again"));
        QVERIFY(bible->savedNote(born).isEmpty()); QCOMPARE(bible->tagsForScope(born).size(),2);
        // The reading version's own note updates the selection's note too.
        QVERIFY(bible->setScopeNote(kjvScope,"Kjv note, edited","Kjv note"));
        QCOMPARE(bible->noteText(),QString("Kjv note, edited"));
        // Removed versions' marks are hidden; invalid scopes are refused.
        QVERIFY(bible->removeVersion("engnet",false));
        QCOMPARE(bible->marks().size(),1);
        auto bad = kjvScope; bad["end"] = 99; QVERIFY(!bible->setScopeNote(bad,"x",""));
    }
    // gm, gn, gt: the reading version's marks in Bible order, one stop per
    // range (a note and a tag on it), from the selection, else from the
    // chapter's start; gi: the note last written.
    void nearMarksAndTheLastNote() {
        install(net);
        QVERIFY(bible->navigate("John 3:1"));
        QCOMPARE(bible->nearMark(1).value("reason").toString(),QString("No mark after John 3:1"));
        QCOMPARE(bible->lastNote().value("reason").toString(),"No notes in " + bible->versionAbbreviation() + " yet");
        const auto tick = [] { const auto now = QDateTime::currentMSecsSinceEpoch(); while (QDateTime::currentMSecsSinceEpoch() == now) {} };
        QVERIFY(bible->navigate("Genesis 1:3")); QVERIFY(bible->toggleHighlight());
        QVERIFY(bible->navigate("John 3:16")); QVERIFY(!bible->createAndAssignTag(bible->tagScope(),"Loved").isEmpty());
        QVERIFY(bible->navigate("John 3:16-17")); bible->setNoteText("So loved");
        QVERIFY(!bible->createAndAssignTag(bible->tagScope(),"Gospel").isEmpty());
        tick(); QVERIFY(bible->navigate("John 3:36-4:2")); bible->setNoteText("Across");
        install(kjv); QVERIFY(bible->navigate("John 3:20")); bible->setNoteText("Another version");
        QVERIFY(bible->switchVersion("engnet"));
        // Each mark selected in turn, until there is none.
        const auto walk = [&](const QString &from, int direction, const QString &kind) {
            QStringList seen;
            if (!bible->navigate(from)) return QStringList{bible->error()};
            for (auto mark = bible->nearMark(direction,kind); seen.size() < 9; mark = bible->nearMark(direction,kind)) {
                if (mark.contains("reason")) { seen << mark.value("reason").toString(); break; }
                seen << mark.value("reference").toString();
                if (!bible->restoreSelection(mark)) return seen << bible->error();
            }
            return seen;
        };
        QCOMPARE(walk("Genesis 1:1",1,""),(QStringList{"Genesis 1:3","John 3:16",QString::fromUtf8("John 3:16–17"),
            QString::fromUtf8("John 3:36–4:2"),QString::fromUtf8("No mark after John 3:36–4:2")}));
        QCOMPARE(walk("John 4:5",-1,"note"),(QStringList{QString::fromUtf8("John 3:36–4:2"),QString::fromUtf8("John 3:16–17"),
            QString::fromUtf8("No note before John 3:16–17")}));
        QCOMPARE(walk("Genesis 1:1",1,"tag"),(QStringList{"John 3:16",QString::fromUtf8("John 3:16–17"),
            QString::fromUtf8("No tagged passage after John 3:16–17")}));
        // Inside a mark, back goes to its start.
        QCOMPARE(walk("John 3:17",-1,"").first(),QString::fromUtf8("John 3:16–17"));
        // Nothing selected: from the chapter's start.
        QVERIFY(bible->navigate("John 3:30")); bible->clearSelection();
        QCOMPARE(bible->nearMark(1).value("reference").toString(),QString("John 3:16"));
        QCOMPARE(bible->nearMark(-1).value("reference").toString(),QString("Genesis 1:3"));
        QVERIFY(bible->navigate("John 4:30")); bible->clearSelection();
        QCOMPARE(bible->nearMark(1,"tag").value("reason").toString(),QString("No tagged passage in or after John 4"));
        // The last note: this version's, and an earlier one written again.
        auto last = bible->lastNote();
        QCOMPARE(last.value("reference").toString(),QString::fromUtf8("John 3:36–4:2"));
        QCOMPARE(last.value("versionId").toString(),QString("engnet"));
        tick(); QVERIFY(bible->navigate("John 3:16-17")); bible->setNoteText("So loved, again");
        last = bible->lastNote();
        QCOMPARE(last.value("reference").toString(),QString::fromUtf8("John 3:16–17"));
        QCOMPARE(last.value("start").toInt(),16); QCOMPARE(last.value("end").toInt(),17);
    }
    // Chapters shown beside the one read come with their marks, as the
    // chapter read has them; the reader stays where it is.
    void chapterRowsCarryTheirMarks() {
        install(net);
        QVERIFY(bible->navigate("John 3:16-17")); QVERIFY(bible->toggleHighlight()); bible->setNoteText("Loved");
        QVERIFY(bible->navigate("John 3:36-4:2")); bible->setNoteText("Across");
        QVERIFY(bible->navigate("John 4:1")); QVERIFY(!bible->createAndAssignTag(bible->tagScope(),"Well").isEmpty());
        QVERIFY(bible->navigate("Genesis 1:1"));
        const auto read = bible->chapterRows({QVariantMap{{"book",43},{"chapter",3}},QVariantMap{{"book",43},{"chapter",4}},
                                             QVariantMap{{"book",43},{"chapter",99}}});
        QCOMPARE(read.size(),3);
        QVERIFY(read[2].toList().isEmpty());
        const auto row = [&](int chapter, int verse) {
            for (const auto &value : read[chapter - 3].toList()) if (value.toMap()["number"].toInt() == verse) return value.toMap();
            return QVariantMap();
        };
        const auto loved = row(3,16);
        QVERIFY(loved["highlighted"].toBool()); QVERIFY(loved["hasNote"].toBool());
        QCOMPARE(loved["noteRanges"].toList().first().toMap()["note"].toString(),QString("Loved"));
        QCOMPARE(loved["highlightRanges"].toList().size(),1); QVERIFY(loved["highlightStart"].toBool());
        QVERIFY(!row(3,15)["highlighted"].toBool()); QVERIFY(row(3,15)["noteRanges"].toList().isEmpty());
        const auto across = row(3,36)["noteRanges"].toList().first().toMap();
        QCOMPARE(across["reference"].toString(),QString::fromUtf8("John 3:36–4:2")); QVERIFY(across["continuesAfter"].toBool());
        const auto end = row(4,2)["noteRanges"].toList().first().toMap();
        QVERIFY(end["noteEnd"].toBool()); QVERIFY(end["continuesBefore"].toBool());
        QVERIFY(row(4,1)["hasTags"].toBool());
        QCOMPARE(row(4,1)["tagRanges"].toList().first().toMap()["tags"].toList().first().toMap()["name"].toString(),QString("Well"));
        QVERIFY(!row(4,3)["hasTags"].toBool());
        QCOMPARE(bible->book(),1); QCOMPARE(bible->chapter(),1); QCOMPARE(bible->selectionReference(),QString("Genesis 1:1"));
        // The chapter read's rows as JSON (QML's copy of them) carry the same values, marks included.
        QVERIFY(bible->navigate("John 3:16"));
        QCOMPARE(QJsonDocument::fromJson(bible->versesJson().toUtf8()).array(), QJsonArray::fromVariantList(bible->verses()));
        const auto copied = QJsonDocument::fromJson(bible->versesJson().toUtf8()).array().at(15).toObject();
        QCOMPARE(copied["number"].toInt(),16); QVERIFY(copied["highlighted"].toBool());
        QCOMPARE(copied["noteRanges"].toArray().first().toObject()["note"].toString(),QString("Loved"));
    }
    void undoAcrossWindowsAndVersions() {
        install(net); BibleBackend peer;
        QVERIFY(bible->navigate("John 3:16")); QVERIFY(peer.navigate("John 3:16"));
        QVERIFY(bible->toggleHighlight()); QVERIFY(bible->navigate("John 3:18")); QVERIFY(bible->toggleHighlight());
        // The peer changes 3:18: that step is dropped, the one before still applies.
        QVERIFY(peer.navigate("John 3:18")); peer.setNoteText("Peer note");
        QVERIFY(!bible->undo()); QVERIFY2(bible->error().contains("another window"),qPrintable(bible->error()));
        QVERIFY(bible->selectionHighlighted()); QCOMPARE(bible->undoLabel(),QString("highlight on John 3:16"));
        QVERIFY(bible->undo()); QVERIFY(!bible->canUndo());
        QVERIFY(peer.navigate("John 3:16")); QVERIFY(!peer.selectionHighlighted());
        QVERIFY(peer.undo()); QVERIFY(peer.navigate("John 3:18")); QVERIFY(peer.noteText().isEmpty());
        // Another version's step applies there, without switching to it.
        install(kjv); QVERIFY(bible->navigate("John 3:16")); QVERIFY(bible->toggleHighlight());
        QVERIFY(bible->switchVersion("engnet")); QVERIFY(bible->undo()); QCOMPARE(bible->versionId(),QString("engnet"));
        QVERIFY(bible->switchVersion("eng-kjv")); QVERIFY(bible->navigate("John 3:16")); QVERIFY(!bible->selectionHighlighted());
        QVERIFY(bible->redo()); QVERIFY(bible->selectionHighlighted());
        // Removing a version drops its steps.
        QVERIFY(bible->removeVersion("eng-kjv",true)); QVERIFY(bible->canUndo());
        QVERIFY(!bible->undo()); QVERIFY(!bible->canUndo()); QVERIFY(!bible->canRedo());
        // Deleting a tag (Manage tags) clears this window's steps.
        QVERIFY(bible->navigate("John 3:16")); QVERIFY(bible->toggleHighlight());
        const QString id = bible->createAndAssignTag(bible->tagScope(),"Passing"); QVERIFY(!id.isEmpty());
        QVERIFY(bible->deleteTag(id)); QVERIFY(!bible->canUndo());
    }
    void nextChapterIsReadAhead() {
        install(net);
        QVERIFY(bible->navigate("John 3:16"));
        const int reads = bible->chapterReads();
        // While idle, the next chapter is read ahead...
        QTRY_COMPARE(bible->chapterReads(), reads + 1);
        // ...so turning to it reads nothing, and reads the one after.
        bible->nextChapter(); QCOMPARE(bible->passage(), QString("John 4"));
        QCOMPARE(bible->chapterReads(), reads + 1);
        QVERIFY(bible->verses().size() > 0); QVERIFY(bible->error().isEmpty());
        QTRY_COMPARE(bible->chapterReads(), reads + 2);
        // A version switch never serves another generation's read.
        install(kjv); QVERIFY(bible->navigate("John 4"));
        QVERIFY(bible->verses().first().toMap()["text"].toString().contains("JHN 4:1"));
    }
    void compareVersionsFollowsEachNumbering() {
        install(net); install(kjv);
        // KJV+ has MRK 7:16; NET omits it.
        QVERIFY(bible->navigate("MRK 7:15-16"));
        auto rows = bible->compareVersions();
        QCOMPARE(rows.size(), 2);
        QMap<QString,QVariantMap> by;
        for (const auto &row : rows) by[row.toMap()["versionId"].toString()] = row.toMap();
        QVERIFY(by["eng-kjv"]["active"].toBool()); QVERIFY(by["eng-kjv"]["available"].toBool());
        QCOMPARE(by["eng-kjv"]["text"].toString(), bible->copySelection(true));
        QCOMPARE(by["eng-kjv"]["reference"].toString(), QString("Mark 7:15–16"));
        QVERIFY(!by["engnet"]["available"].toBool()); QVERIFY(by["engnet"]["text"].toString().isEmpty());
        // A shared passage, given as a scope, reads in each version.
        QVERIFY(bible->navigate("GEN 1:2"));
        for (const auto &row : bible->compareVersions(bible->tagScope())) {
            QVERIFY(row.toMap()["available"].toBool());
            QVERIFY(row.toMap()["text"].toString().contains("GEN 1:2"));
        }
        bible->clearSelection(); QVERIFY(bible->compareVersions().isEmpty());
    }
    void deskPageAcrossVersionsAndWindows() {
        install(net); BibleBackend peer; QVERIFY(peer.deskPage().isEmpty());
        QSignalSpy peerChanged(&peer,&BibleBackend::deskChanged);
        const QString genesis = deskQuote("GEN 1:4"); QVERIFY(!bible->appendToDesk().isEmpty());
        QCOMPARE(peerChanged.count(),1); QCOMPARE(peer.deskPage(),genesis + "\n");
        // Another version's passage is quoted from it, under its abbreviation.
        install(kjv); const QString tobit = deskQuote("TOB 1:2-3"), kjvAbbreviation = bible->versionAbbreviation();
        const auto scope = bible->tagScope();
        QVERIFY(tobit.endsWith(" (" + kjvAbbreviation + ")"));
        QVERIFY(bible->switchVersion("engnet"));
        QCOMPARE(peer.appendToDesk(scope),scope["reference"].toString());
        QCOMPARE(bible->deskPage(),genesis + "\n\n" + tobit + "\n"); QCOMPARE(peer.deskPage(),bible->deskPage());
        // A window's draft of the page saves before another window adds to it.
        int flushes = 0;
        bible->setFlushCallback([&] { ++flushes; return bible->setDeskPage("Draft",bible->deskPage()); });
        QVERIFY(bible->navigate("John 3:16")); QVERIFY(!peer.appendToDesk(bible->tagScope()).isEmpty());
        QCOMPARE(flushes,1); QVERIFY(peer.deskPage().startsWith("Draft\n\n> "));
        // A draft that cannot save cancels the addition.
        const QString page = peer.deskPage();
        bible->setFlushCallback([] { return false; });
        QVERIFY(peer.appendToDesk(bible->tagScope()).isEmpty()); QVERIFY(!peer.error().isEmpty()); QCOMPARE(peer.deskPage(),page);
        bible->setFlushCallback({});
        // Removing a version leaves the page's words; its passages cannot be added again.
        QVERIFY(bible->removeVersion("eng-kjv",true)); QCOMPARE(peer.deskPage(),page); QCOMPARE(bible->deskPage(),page);
        QVERIFY(bible->appendToDesk(scope).isEmpty()); QVERIFY(!bible->error().isEmpty()); QCOMPARE(bible->deskPage(),page);
    }
    void deskBackupRoundTrip() {
        install(net);
        const QString genesis = deskQuote("GEN 1:2"); QVERIFY(!bible->appendToDesk().isEmpty());
        QVERIFY(bible->setDeskPage(bible->deskPage() + "\nMy own line.\n",bible->deskPage()));
        const QString saved = bible->deskPage();
        const auto backup = QUrl::fromLocalFile(session->filePath("desk.json")); QVERIFY(bible->exportBackup(backup));
        QFile file(backup.toLocalFile()); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto root = QJsonDocument::fromJson(file.readAll()).object(); file.close();
        QCOMPARE(root["version"].toInt(),7); QCOMPARE(root["deskPage"].toString(),saved); QVERIFY(!root.contains("desk"));
        // Restoring replaces the page, in every window.
        QVERIFY(bible->setDeskPage("Something else",saved));
        BibleBackend peer;
        QVERIFY2(bible->importBackup(backup),qPrintable(bible->error()));
        QCOMPARE(bible->deskPage(),saved); QCOMPARE(peer.deskPage(),saved);
        // Format 6 held the Desk's passages: they come back as the page's quotations, in order.
        auto item = [](int book, int chapter, int start, int end) {
            return QJsonObject{{"book",book},{"chapter",chapter},{"start",start},{"endBook",book},{"endChapter",chapter},{"end",end},
                               {"created","2026-01-01T00:00:00.000Z"}};
        };
        auto withVersion = [](QJsonObject object) { object["versionId"] = "engnet"; return object; };
        const QString john = deskQuote("John 3:16"), genesisRange = deskQuote("GEN 1:1-2");
        auto six = root; six["version"] = 6; six.remove("deskPage");
        six["desk"] = QJsonArray{withVersion(item(43,3,16,16)),withVersion(item(1,1,1,2))};
        const auto sixFile = QUrl::fromLocalFile(session->filePath("desk-v6.json"));
        QVERIFY(write(sixFile.toLocalFile(),QJsonDocument(six).toJson()));
        QVERIFY2(bible->importBackup(sixFile),qPrintable(bible->error()));
        QCOMPARE(bible->deskPage(),john + "\n\n" + genesisRange + "\n"); QCOMPARE(peer.deskPage(),bible->deskPage());
        // Format 5 held one version's: they join the page's end.
        QVERIFY(bible->setDeskPage("Kept",bible->deskPage()));
        auto five = legacyBackup(root,"engnet"); five["desk"] = QJsonArray{item(43,3,16,16)};
        const auto fiveFile = QUrl::fromLocalFile(session->filePath("desk-v5.json"));
        QVERIFY(write(fiveFile.toLocalFile(),QJsonDocument(five).toJson()));
        QVERIFY2(bible->importBackup(fiveFile),qPrintable(bible->error()));
        QCOMPARE(bible->deskPage(),"Kept\n\n" + john + "\n");
        // Older formats restore without touching it.
        auto four = legacyBackup(root,"engnet"); four["version"] = 4; four.remove("desk");
        const auto fourFile = QUrl::fromLocalFile(session->filePath("desk-v4.json"));
        QVERIFY(write(fourFile.toLocalFile(),QJsonDocument(four).toJson()));
        const QString before = bible->deskPage();
        QVERIFY2(bible->importBackup(fourFile),qPrintable(bible->error())); QCOMPARE(bible->deskPage(),before);
        // An invalid Desk refuses the whole restore.
        for (const QString kind : {"page-type","page-control","page-size","duplicate","invalid","missing-created","other-version"}) {
            auto broken = kind.startsWith("page") ? root : six;
            if (kind == "page-type") broken["deskPage"] = 3;
            if (kind == "page-control") broken["deskPage"] = QString("Bad\u0001");
            if (kind == "page-size") broken["deskPage"] = QString(4 * 1024 * 1024 + 1,'x');
            if (!kind.startsWith("page")) {
                auto items = broken["desk"].toArray(); auto a = items.first().toObject();
                if (kind == "invalid") a["start"] = 999;
                if (kind == "missing-created") a.remove("created");
                if (kind == "other-version") a["versionId"] = "eng-kjv"; // Not installed, nor in this backup.
                items[0] = a; if (kind == "duplicate") items.append(a); broken["desk"] = items;
            }
            const auto path = QUrl::fromLocalFile(session->filePath("broken-desk.json"));
            QVERIFY(write(path.toLocalFile(),QJsonDocument(broken).toJson()));
            QVERIFY2(!bible->importBackup(path),qPrintable(kind)); QCOMPARE(bible->deskPage(),before);
        }
    }
    void backupValidationAndTransactionalFailures() {
        install(net); QVERIFY(bible->navigate("John 3:16-18")); bible->setNoteText("Keep this note"); bible->setReadingPosition(0.73);
        const auto backup = QUrl::fromLocalFile(session->filePath("backup.json")); QVERIFY(bible->exportBackup(backup));
        QFile source(backup.toLocalFile()); QVERIFY(source.open(QIODevice::ReadOnly));
        const auto document = QJsonDocument::fromJson(source.readAll());
        const auto original = bible->collection();
        QCOMPARE(document.object().value("version").toInt(),7);
        // Every check, in this format and in the format earlier versions wrote.
        for (const bool legacy : {false,true})
        for (const QString kind : {"signature","version","fractional","unknown-key","out-of-range","omitted","duplicate","reading","time"}) {
            if (legacy && kind == "time") continue;
            auto root = legacy ? legacyBackup(document.object(),"engnet") : document.object();
            auto entry = legacy ? root : root["versions"].toArray().first().toObject();
            auto rows = entry["annotations"].toArray(); auto a = rows.first().toObject();
            if (kind == "signature") root["format"] = "unknown-backup";
            if (kind == "version") root["version"] = legacy ? 2 : 8;
            if (kind == "fractional") a["start"] = 16.5;
            if (kind == "unknown-key") a["unexpected"] = true;
            if (kind == "out-of-range") a["chapter"] = 1000;
            if (kind == "omitted") { a["book"] = 41; a["chapter"] = 7; a["start"] = 16; }
            if (kind == "reading") { auto r = entry["reading"].toObject(); r["position"] = 2; entry["reading"] = r; }
            if (kind == "time") a["created"] = "yesterday";
            rows[0] = a; if (kind == "duplicate") rows.append(a); entry["annotations"] = rows;
            if (legacy) { entry["format"] = root["format"]; entry["version"] = root["version"]; root = entry; }
            else { auto versions = root["versions"].toArray(); versions[0] = entry; root["versions"] = versions; }
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
        QVERIFY(q.exec("CREATE TRIGGER reject_notes_update BEFORE UPDATE ON annotations BEGIN SELECT RAISE(ABORT,'injected disk failure'); END"));
        bible->setNoteText("Must fail"); QVERIFY(bible->error().contains("injected disk failure"));
        QVERIFY(!bible->importBackup(backup)); QCOMPARE(bible->collection(),original); QCOMPARE(bible->noteText(),QString("Keep this note"));
        QVERIFY(q.exec("DROP TRIGGER reject_notes"));
        QVERIFY(q.exec("DROP TRIGGER reject_notes_update"));
        // Deferred constraint tests a COMMIT failure after successful write/readback.
        QVERIFY(q.exec("PRAGMA foreign_keys=ON"));
        QVERIFY(q.exec("CREATE TABLE parent(id INTEGER PRIMARY KEY)"));
        QVERIFY(q.exec("CREATE TABLE failure(id INTEGER REFERENCES parent(id) DEFERRABLE INITIALLY DEFERRED)"));
        QVERIFY(q.exec("CREATE TRIGGER fail_commit AFTER INSERT ON annotations BEGIN INSERT INTO failure VALUES(1); END"));
        QVERIFY(q.exec("CREATE TRIGGER fail_commit_update AFTER UPDATE ON annotations BEGIN INSERT INTO failure VALUES(1); END"));
        bible->setNoteText("Commit must fail"); QVERIFY(bible->error().contains("Cannot commit annotations")); QCOMPARE(bible->collection(),original);
        QVERIFY(q.exec("DROP TRIGGER fail_commit"));
        QVERIFY(q.exec("DROP TRIGGER fail_commit_update"));
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
        bible->importAo(QUrl::fromLocalFile(net)); QVERIFY(bible->busy()); QVERIFY(peer.busy());
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
        bible->importAo(QUrl::fromLocalFile(kjv)); QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(),30000);
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
    // A new export or backup is yours alone (0600) from the start; a file it
    // replaces keeps its permissions; your own symlink is followed, and one
    // another user owns (as one left in /tmp could be) is refused unwritten.
    void exportsArePrivateAndFollowOnlyYourLinks() {
        install(net); QVERIFY(bible->navigate("John 3:16")); bible->setNoteText("Private");
        const auto mode = [](const QString &path) {
            struct stat info;
            return ::stat(QFile::encodeName(path).constData(),&info) == 0 ? int(info.st_mode & 07777) : -1;
        };
        const QString fresh = session->filePath("fresh.json");
        QVERIFY(bible->exportBackup(QUrl::fromLocalFile(fresh))); QCOMPARE(mode(fresh),0600);
        QVERIFY(bible->exportNotes(QUrl::fromLocalFile(session->path()))); QCOMPARE(mode(session->filePath("omaword-notes.md")),0600);
        const QString shared = session->filePath("shared.json");
        { QFile file(shared); QVERIFY(file.open(QIODevice::WriteOnly)); }
        QVERIFY(QFile::setPermissions(shared,QFile::ReadOwner|QFile::WriteOwner|QFile::ReadGroup|QFile::ReadOther));
        QVERIFY(bible->exportBackup(QUrl::fromLocalFile(shared))); QCOMPARE(mode(shared),0644); QVERIFY(QFileInfo(shared).size() > 0);
        const QString target = session->filePath("target.json"), link = session->filePath("link.json");
        QVERIFY(QFile::link(target,link));
        QVERIFY(bible->exportBackup(QUrl::fromLocalFile(link)));
        QVERIFY(QFileInfo(link).isSymLink()); QVERIFY(QFileInfo(target).size() > 0); QCOMPARE(mode(target),0600);
        // Another user's link: one of root's in /etc, whose target you could not write anyway.
        QString foreign;
        for (const auto &entry : QDir("/etc").entryInfoList(QDir::AllEntries|QDir::System|QDir::NoDotAndDotDot)) {
            struct stat info;
            if (entry.isSymLink() && ::lstat(QFile::encodeName(entry.filePath()).constData(),&info) == 0 && info.st_uid != ::getuid()
                && !QFileInfo(QFileInfo(entry.symLinkTarget()).absolutePath()).isWritable()) { foreign = entry.filePath(); break; }
        }
        if (foreign.isEmpty()) QSKIP("No symlink another user owns in /etc (as when the tests run as root).");
        QVERIFY(!bible->exportBackup(QUrl::fromLocalFile(foreign)));
        QVERIFY2(bible->error().contains("symlink another user owns"), qPrintable(foreign + ": " + bible->error()));
    }
    void invalidImportPreservesTarget() {
        const auto target = session->filePath("target.sqlite"), invalid = session->filePath("bad.zip");
        QVERIFY(write(target,"Original target")); QVERIFY(write(invalid,"Not a ZIP"));
        const auto result = AoImporter::importFile(invalid,target); QVERIFY(!result.ok); QVERIFY(!result.error.isEmpty());
        QFile file(target); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(),QByteArray("Original target"));
        install(net); const auto path = installedPath(); bible->importAo(QUrl::fromLocalFile(invalid));
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
        QString error; QVERIFY(!AoImporter::validateDatabase(path,&error)); QVERIFY(!error.isEmpty());
    }
    void unchangedGenerationsOpenWithoutRevalidation() {
        install(net);
        const QString path = installedPath();
        const QString record = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/validated-bibles.json";
        auto identity = [](const QString &file) {
            struct stat info;
            if (::stat(QFile::encodeName(file).constData(),&info) != 0) return QString();
            return QStringLiteral("%1 %2 %3 %4.%5 %6.%7").arg(info.st_dev).arg(info.st_ino).arg(info.st_size)
                .arg(info.st_mtim.tv_sec).arg(info.st_mtim.tv_nsec).arg(info.st_ctim.tv_sec).arg(info.st_ctim.tv_nsec);
        };
        auto recorded = [&] {
            QFile file(record);
            return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
        };
        auto setRecorded = [&](const QJsonObject &object) {
            QSaveFile file(record);
            return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(object).toJson()) > 0 && file.commit();
        };
        // The import's open validated the generation and recorded its file identity.
        QCOMPARE(recorded().value(path).toString(),identity(path));
        bible.reset();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","edited"); db.setDatabaseName(path); QVERIFY(db.open());
            QSqlQuery q(db); QVERIFY(q.exec("UPDATE verses SET text=text||' changed' WHERE book=1 AND chapter=1 AND number=1"));
        }
        QSqlDatabase::removeDatabase("edited");
        // An edit in place changes the identity: the next open validates again and refuses it.
        QVERIFY(recorded().value(path).toString() != identity(path));
        bible = std::make_unique<BibleBackend>();
        QVERIFY(!bible->ready()); QVERIFY2(bible->error().contains("digest"),qPrintable(bible->error()));
        // An identity matching the record is trusted as already validated.
        bible.reset();
        auto object = recorded(); object.insert(path,identity(path)); QVERIFY(setRecorded(object));
        bible = std::make_unique<BibleBackend>();
        QVERIFY2(bible->ready() && bible->error().isEmpty(),qPrintable(bible->error()));
    }
    void textSchemaFootnotesAndReplacement() {
        const QString path = fixtures.filePath("study.json"); QVERIFY(study(path));
        install(path);
        QCOMPARE(bible->versionId(), QString("study")); QCOMPARE(bible->textSchema(), 7); QVERIFY(bible->hasWordsOfJesus());
        QCOMPARE(versionRow(bible.get(),"study")["revision"].toString(), QString());
        QCOMPARE(versionRow(bible.get(),"study")["textSchema"].toInt(), 7);
        QVERIFY(bible->navigate("Genesis 1"));
        const auto notes = bible->footnotes();
        QCOMPARE(notes.size(), 2);
        QCOMPARE(notes[0].toMap()["verse"].toInt(), 1); QCOMPARE(notes[0].toMap()["text"].toString(), QString("1:1 First note"));
        QCOMPARE(notes[0].toMap()["offset"].toInt(), QString("In the beginning.").size());
        QCOMPARE(notes[1].toMap()["caller"].toString(), QString("b"));
        QCOMPARE(bible->verses()[0].toMap()["footnotes"].toList().size(), 1);
        QVERIFY(bible->navigate("John 3:16"));
        const auto row = bible->verses()[0].toMap();
        const auto spans = row["wordsOfJesus"].toList(); QCOMPARE(spans.size(), 1);
        const QString text = row["text"].toString();
        QCOMPARE(text.mid(spans[0].toMap()["start"].toInt(), spans[0].toMap()["end"].toInt() - spans[0].toMap()["start"].toInt()), QString("Truly I say"));
        bible->selectVerses(16,16); bible->setNoteText("Kept across re-import"); QVERIFY(bible->error().isEmpty());
        const QString file = installedPath();
        // Re-import pins the edition: another edition's file is refused.
        bible->importAo(QUrl::fromLocalFile(net), "study");
        QVERIFY(!bible->busy()); QVERIFY(bible->error().contains("not")); QCOMPARE(bible->versions().size(), 1);
        bible->clearError();
        // Re-import through the ordinary install/replace flow keeps annotations;
        // the versions list reports the AO revision now installed.
        const QString revised = fixtures.filePath("study-revised.json");
        {
            QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
            auto root = QJsonDocument::fromJson(file.readAll()).object(); auto translation = root["translation"].toObject();
            translation["sha256"] = "revision-2"; root["translation"] = translation; QVERIFY(AoFixture::write(revised, root));
        }
        bible->importAo(QUrl::fromLocalFile(revised), "study");
        QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(),60000); QVERIFY2(bible->error().isEmpty(), qPrintable(bible->error()));
        QCOMPARE(bible->textSchema(), 7);
        QCOMPARE(versionRow(bible.get(),"study")["revision"].toString(), QString("revision-2"));
        QVERIFY(installedPath() != file); QVERIFY(!QFile::exists(file));
        QVERIFY(bible->navigate("John 3:16")); bible->selectVerses(16,16);
        QCOMPARE(bible->noteText(), QString("Kept across re-import"));
    }
    void crossReferencesComeFromOpenBible() {
        const QString path = fixtures.filePath("study.json"); QVERIFY(study(path));
        install(path);
        QVERIFY(bible->crossReferences().isEmpty()); // No verse selected.
        // The source's own come first, in source order; OpenBible.info's follow,
        // by votes, without the ranges the source already gave.
        auto own = [](const QVariantList &rows) {
            QStringList references; bool community = false; int votes = std::numeric_limits<int>::max();
            for (const auto &value : rows) {
                const auto row = value.toMap();
                if (row["openBible"].toBool()) {
                    community = true;
                    if (row["votes"].toInt() > votes || row["votes"].toInt() <= 0) return QStringList({"votes out of order"});
                    votes = row["votes"].toInt();
                } else if (community) return QStringList({"own after OpenBible"});
                else references << row["reference"].toString();
            }
            return references;
        };
        auto all = [](const QVariantList &rows) {
            QStringList references;
            for (const auto &value : rows) references << value.toMap()["reference"].toString();
            return references;
        };
        QVERIFY(bible->navigate("Genesis 1:1"));
        // The source lists John 1:1-3 twice ("Jn" and "John"), and so does
        // OpenBible (heavily voted): one row, the source's.
        auto rows = bible->crossReferences();
        QVERIFY(own(rows).isEmpty());
        QCOMPARE(all(rows).count("John 1:1\u20133"), 1);
        auto row = rows[0].toMap();
        QVERIFY(row["openBible"].toBool()); QVERIFY(row["votes"].toInt() > 0);
        QCOMPARE(row["book"].toInt(), 43); QCOMPARE(row["start"].toInt(), 1); QCOMPARE(row["end"].toInt(), 3);
        QVERIFY(row["excerpt"].toString().startsWith("Word. Same."));
        QCOMPARE(row["versionId"].toString(), QString("study"));
        // Source order, not canonical order: a bare number continues the
        // previous chapter; unknown books are skipped.
        QVERIFY(bible->navigate("Genesis 1:2"));
        QVERIFY(own(bible->crossReferences()).isEmpty());
        // A range unions its verses in verse order; the passage itself is never listed.
        QVERIFY(bible->navigate("Genesis 1:1-2"));
        rows = bible->crossReferences();
        QVERIFY(own(rows).isEmpty());
        QVERIFY(!all(rows).contains("Genesis 1:1")); QVERIFY(!all(rows).contains("Genesis 1:2"));
        // A verse without \\x of its own still has OpenBible.info's (KJV verse
        // numbers): John 1:1 points back to Genesis 1:1.
        QVERIFY(bible->navigate("John 1:1"));
        rows = bible->crossReferences();
        QVERIFY(own(rows).isEmpty()); QVERIFY(!rows.isEmpty());
        QVERIFY(all(rows).contains("Genesis 1:1"));
        for (const auto &value : rows) QVERIFY(value.toMap()["openBible"].toBool() && value.toMap()["votes"].toInt() > 0);
        // Targets absent from this version are hidden: John 3:16's are all absent here.
        QVERIFY(bible->navigate("John 3:16")); QVERIFY(bible->crossReferences().isEmpty());
        // Another version's scope is not answered from the active text.
        auto scope = bible->tagScope(); scope["versionId"] = "elsewhere";
        QVERIFY(bible->crossReferences(scope).isEmpty());
    }
    void optionalSourceArchives_data() {
        QTest::addColumn<QString>("id"); QTest::addColumn<QString>("path");
        const auto directory = qEnvironmentVariable("OMAWORD_TEST_AO_DIR");
        for (const QString id : {"eng_net","eng_kjv","eng_kja","eng_ojb","eng_ylt","eng_asv","BSB"}) {
            const QString path = directory.isEmpty() ? QString() : QDir(directory).filePath(id+"_complete.json");
            QTest::newRow(qPrintable(id)) << id << path;
        }
    }
    void optionalSourceArchives() {
        QFETCH(QString,id); QFETCH(QString,path);
        if (path.isEmpty()) QSKIP("Set OMAWORD_TEST_AO_DIR for optional source checks.");
        QVERIFY2(QFileInfo::exists(path),qPrintable(path)); install(path); QCOMPARE(bible->versionId(),id);
        QString error; QVERIFY2(AoImporter::validateDatabase(installedPath(),&error),qPrintable(error));
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
