#include <QtTest/QTest>
#include "biblebackend.h"
#include "epubimporter.h"
#include <QClipboard>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <archive.h>
#include <archive_entry.h>

class BackendTest : public QObject
{
    Q_OBJECT
    QTemporaryDir root;
    QString epub;
    std::unique_ptr<BibleBackend> bible;
    QString file(const QString &name) const { return root.filePath(name); }
    static bool write(const QString &path, const QByteArray &bytes) {
        QSaveFile f(path); return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size() && f.commit();
    }
    static bool zip(const QString &path, const QMap<QString,QByteArray> &entries) {
        archive *a = archive_write_new();
        archive_write_set_format_zip(a);
        bool ok = archive_write_open_filename(a,QFile::encodeName(path).constData()) == ARCHIVE_OK;
        for (auto it = entries.cbegin(); ok && it != entries.cend(); ++it) {
            archive_entry *entry = archive_entry_new();
            archive_entry_set_pathname(entry,it.key().toUtf8().constData());
            archive_entry_set_size(entry,it.value().size());
            archive_entry_set_filetype(entry,AE_IFREG); archive_entry_set_perm(entry,0600);
            ok = archive_write_header(a,entry) == ARCHIVE_OK
                && archive_write_data(a,it.value().constData(),it.value().size()) == it.value().size();
            archive_entry_free(entry);
        }
        ok = archive_write_close(a) == ARCHIVE_OK && ok;
        archive_write_free(a); return ok;
    }
private slots:
    void initTestCase() {
        QVERIFY(root.isValid());
        qputenv("XDG_DATA_HOME",root.path().toUtf8());
        QCoreApplication::setOrganizationName("OmabibleTests");
        QCoreApplication::setApplicationName("omabible-test");
        epub = qEnvironmentVariable("OMABIBLE_TEST_EPUB");
        bible = std::make_unique<BibleBackend>();
        QVERIFY2(bible->error().isEmpty(), qPrintable(bible->error()));
        QVERIFY(!bible->ready());
        QCOMPARE(bible->books().size(),66);
        QVERIFY(!bible->navigate("John 3"));
        QVERIFY(!bible->error().isEmpty());
        bible->clearError();
    }
    void aliases_data() {
        QTest::addColumn<QString>("name"); QTest::addColumn<int>("id");
        QTest::newRow("compact") << "1Jn" << 62;
        QTest::newRow("roman") << "II Corinthians" << 47;
        QTest::newRow("ordinal") << "Third John" << 64;
        QTest::newRow("song") << "Song of Songs" << 22;
        QTest::newRow("punctuation") << "1. Cor." << 46;
        QTest::newRow("psalm") << "Psalm" << 19;
        QTest::newRow("ambiguous") << "J" << 0;
        QTest::newRow("injection") << "John;DROP TABLE" << 0;
    }
    void aliases() { QFETCH(QString,name); QFETCH(int,id); QCOMPARE(EpubImporter::bookId(name),id); }
    void invalidArchivePreservesTarget() {
        const auto target = file("untouched.sqlite");
        QVERIFY(write(target,"original target"));
        QVERIFY(write(file("bad.epub"),"not an EPUB"));
        const auto result = EpubImporter::importFile(file("bad.epub"),target);
        QVERIFY(!result.ok); QVERIFY(!result.error.isEmpty());
        QFile original(target); QVERIFY(original.open(QIODevice::ReadOnly)); QCOMPARE(original.readAll(),QByteArray("original target"));
    }
    void incompleteAndUnsafeEpub() {
        const auto target = file("untouched.sqlite");
        QMap<QString,QByteArray> entries = {
            {"mimetype","application/epub+zip"},
            {"META-INF/container.xml","<container><rootfiles><rootfile full-path=\"OEBPS/content.opf\"/></rootfiles></container>"},
            {"OEBPS/content.opf","<package><metadata><title>NET Bible 2.1</title><identifier>NETBIBLE21</identifier></metadata><manifest><item id=\"chapter\" href=\"chapter.xhtml\"/></manifest><spine><itemref idref=\"chapter\"/></spine></package>"},
            {"OEBPS/chapter.xhtml","<html><head><title>NET Bible 2.1 Genesis 1</title></head><body><p class=\"bodytext\"><span class=\"verse\">1:1</span>Synthetic fixture only.</p></body></html>"}
        };
        QVERIFY(zip(file("incomplete.epub"),entries));
        auto result = EpubImporter::importFile(file("incomplete.epub"),target);
        QVERIFY(!result.ok); QVERIFY2(result.error.contains("1,189"),qPrintable(result.error));
        entries["OEBPS/chapter.xhtml"] = "<html><head><title>NET Bible 2.1 Genesis 1</title></head><body><p class=\"bodytext\"><span class=\"verse\">1:1</span>Broken XML</body>";
        QVERIFY(zip(file("invalid-xml.epub"),entries));
        result = EpubImporter::importFile(file("invalid-xml.epub"),target);
        QVERIFY(!result.ok); QVERIFY2(result.error.contains("Invalid XML"),qPrintable(result.error));
        entries["../escape.xhtml"] = "unsafe";
        QVERIFY(zip(file("unsafe.epub"),entries));
        result = EpubImporter::importFile(file("unsafe.epub"),target);
        QVERIFY(!result.ok); QVERIFY2(result.error.contains("unsafe"),qPrintable(result.error));
        QFile original(target); QVERIFY(original.open(QIODevice::ReadOnly)); QCOMPARE(original.readAll(),QByteArray("original target"));
        QVERIFY(!QFileInfo::exists(file("escape.xhtml")));
    }
    void importRealEpub() {
        if (epub.isEmpty()) QSKIP("Set OMABIBLE_TEST_EPUB to the user's NETBIBLE21.epub to run integration tests.");
        QSignalSpy ready(bible.get(),&BibleBackend::readyChanged);
        bible->importEpub(QUrl::fromLocalFile(epub));
        QVERIFY(bible->busy());
        QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(),120000);
        QVERIFY2(bible->ready(),qPrintable(bible->error()));
        QCOMPARE(ready.count(),1); QCOMPARE(bible->importProgress(),1.0);
        QCOMPARE(bible->verses().size(),31);
        QString validationError;
        QVERIFY2(EpubImporter::validateDatabase(bible->dataDirectory()+"/bible.sqlite",&validationError),qPrintable(validationError));
        int chapters = 0, verses = 0;
        for (int b = 1; b <= 66; ++b) {
            for (int c = 1; c <= EpubImporter::canon()[b-1].chapters; ++c) {
                bible->openChapter(b,c); ++chapters; verses += bible->verses().size();
                for (const auto &row : bible->verses()) {
                    const auto map = row.toMap();
                    QVERIFY(!map.value("text").toString().isEmpty());
                    QVERIFY(!map.value("html").toString().contains("<sup"));
                    QVERIFY(!map.value("html").toString().contains("href="));
                }
            }
        }
        QCOMPARE(chapters,1189); QCOMPARE(verses,31087);
        // Cross-file chapter boundaries must be reconstructed from verse markers.
        QVERIFY(bible->navigate("Acts 7:60"));
        QCOMPARE(bible->verses().last().toMap().value("number").toInt(),60);
        QVERIFY(bible->navigate("John 7:53"));
        QCOMPARE(bible->verses().last().toMap().value("number").toInt(),53);
        QVERIFY(bible->navigate("3 John 1:15"));
        QVERIFY(bible->navigate("Genesis 1:27"));
        const auto poetry = bible->verses()[26].toMap();
        QVERIFY(poetry.value("poetry").toBool());
        QVERIFY(poetry.value("text").toString().contains('\n'));
        QVERIFY(poetry.value("html").toString().contains("<br/>"));
        QVERIFY(bible->verses().first().toMap().value("heading").toString().size()>0);
    }
    void boundedImportOutput_data() {
        QTest::addColumn<QString>("kind");
        for (const QString kind : {"xml-depth","format-depth","verse-output","document-output","global-output"})
            QTest::newRow(qPrintable(kind)) << kind;
    }
    void boundedImportOutput() {
        QFETCH(QString,kind);
        QMap<QString,QByteArray> entries{{"mimetype","application/epub+zip"},
            {"META-INF/container.xml","<container><rootfile full-path=\"content.opf\"/></container>"}};
        QByteArray manifest, spine;
        const int documents = kind == "global-output" ? 150 : 1;
        for (int i = 0; i < documents; ++i) {
            const int book = kind == "global-output" || kind == "document-output" ? 19 : 1;
            const int chapter = i+1;
            QByteArray body;
            const int maximum = EpubImporter::chapterMaximum(book,chapter);
            const int verses = kind == "global-output" || kind == "document-output" ? maximum : 1;
            for (int v = 1; v <= verses; ++v) {
                body += "<span class=\"verse\">" + QByteArray::number(chapter) + ':' + QByteArray::number(v) + "</span>";
                if (kind == "xml-depth") body += QByteArray("<span>").repeated(65) + "x" + QByteArray("</span>").repeated(65);
                if (kind == "format-depth") body += QByteArray("<b>").repeated(17) + "x" + QByteArray("</b>").repeated(17);
                if (kind == "verse-output") body += QByteArray(10000,'&').replace("&","&amp;");
                if (kind == "document-output") body += QByteArray(8000,'x');
                if (kind == "global-output") body += QByteArray(std::min(8000,120000/maximum),'x');
            }
            // Psalm 119 provides enough legal verse markers to exceed a document budget.
            if (kind == "document-output") {
                body.clear();
                for (int v = 1; v <= 176; ++v) body += "<span class=\"verse\">119:" + QByteArray::number(v) + "</span>" + QByteArray(8000,'x');
            }
            const QByteArray name = "chapter" + QByteArray::number(i) + ".xhtml";
            entries[QString::fromLatin1(name)] = "<html><head><title>NET Bible 2.1 " + QByteArray(EpubImporter::canon()[book-1].name)
                + ' ' + QByteArray::number(kind == "document-output" ? 119 : chapter)
                + "</title></head><body><p class=\"bodytext\">" + body + "</p></body></html>";
            manifest += "<item id=\"" + name + "\" href=\"" + name + "\"/>";
            spine += "<itemref idref=\"" + name + "\"/>";
        }
        entries["content.opf"] = "<package><metadata><title>NET Bible 2.1</title><identifier>NETBIBLE21</identifier></metadata><manifest>"
            + manifest + "</manifest><spine>" + spine + "</spine></package>";
        QVERIFY(zip(file("bounded.epub"),entries));
        QVERIFY(write(file("bounded.sqlite"),"unchanged"));
        const auto result = EpubImporter::importFile(file("bounded.epub"),file("bounded.sqlite"));
        QVERIFY(!result.ok); QVERIFY2(result.error.contains("limit exceeded"),qPrintable(result.error));
        QFile target(file("bounded.sqlite")); QVERIFY(target.open(QIODevice::ReadOnly)); QCOMPARE(target.readAll(),QByteArray("unchanged"));
    }
    void chapterMetadataAndCompensatingCorruption() {
        int chapters = 0, total = 0;
        for (int b = 1; b <= 66; ++b) {
            int count = 0;
            for (int c = 1; c <= EpubImporter::canon()[b-1].chapters; ++c) {
                QVERIFY(EpubImporter::chapterMaximum(b,c)>0); ++chapters;
                count += EpubImporter::chapterVerseCount(b,c);
            }
            QCOMPARE(count,EpubImporter::canon()[b-1].verses); total += count;
        }
        QCOMPARE(chapters,1189); QCOMPARE(total,31087);
        if (!bible->ready()) QSKIP("Corruption fixture requires real EPUB");
        QVERIFY(QFile::copy(bible->dataDirectory()+"/bible.sqlite",file("compensated.sqlite")));
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","compensated"); db.setDatabaseName(file("compensated.sqlite")); QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("UPDATE verses SET chapter=2,number=26 WHERE book=1 AND chapter=1 AND number=31"));
            QVERIFY(q.exec("SELECT COUNT(*) FROM verses WHERE book=1")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),1533);
        }
        QSqlDatabase::removeDatabase("compensated");
        QString error; QVERIFY(!EpubImporter::validateDatabase(file("compensated.sqlite"),&error));
        QVERIFY2(error.contains("Incomplete chapter: Genesis 1"),qPrintable(error));
    }
    void references_data() {
        QTest::addColumn<QString>("input"); QTest::addColumn<QString>("expected");
        QTest::newRow("range") << "Jn 3:16–18" << "John 3:16–18";
        QTest::newRow("compact") << "1Jn3:16" << "1 John 3:16";
        QTest::newRow("roman") << "II Cor. 5:17" << "2 Corinthians 5:17";
        QTest::newRow("same-chapter") << "John 3:16-3:18" << "John 3:16–18";
        QTest::newRow("single-book") << "Jude 5" << "Jude 1:5";
        QTest::newRow("single-range") << "Jude 5–7" << "Jude 1:5–7";
        QTest::newRow("song") << "Song of Songs 2:1" << "Song of Solomon 2:1";
        QTest::newRow("omitted-inside") << "Mark 9:43-47" << "Mark 9:43–47";
        QTest::newRow("bare-book") << "Genesis" << "Genesis 1";
    }
    void references() {
        if (!bible->ready()) QSKIP("Requires real EPUB");
        QFETCH(QString,input); QFETCH(QString,expected);
        QVERIFY2(bible->navigate(input),qPrintable(bible->error())); QCOMPARE(bible->selectionReference(),expected);
    }
    void invalidReferences() {
        if (!bible->ready()) QSKIP("Requires real EPUB");
        QVERIFY(bible->navigate("John 3:16"));
        const QStringList invalid = {"John 0","John 22","John 3:0","John 3:99","John 3:18-16", "John 3:16-4:2",
            "Matthew 17:21","Acts 8:37","Acts 24:7","Mark 9:44","John 5:4","4 John 1", "John 3:16 garbage", "", "J 1", "Jude 0", "Jude 0-0"};
        for (const auto &input : invalid) {
            QVERIFY2(!bible->navigate(input),qPrintable(input)); QCOMPARE(bible->selectionReference(),QString("John 3:16"));
        }
        QVERIFY(bible->navigate("3:17")); QCOMPARE(bible->selectionReference(),QString("John 3:17"));
    }
    void selectionAndRangeAnnotations() {
        if (!bible->ready()) QSKIP("Requires real EPUB");
        QVERIFY(bible->navigate("John 3:16-18"));
        bible->setNoteText("# A personal note\n\nUnicode: café — שלום\n");
        QCOMPARE(bible->noteText(),QString("# A personal note\n\nUnicode: café — שלום\n"));
        bible->toggleHighlight(); bible->toggleBookmark();
        QVERIFY(bible->selectionHighlighted()); QVERIFY(bible->selectionBookmarked());
        bible->selectVerses(17,17);
        QVERIFY(bible->noteText().isEmpty()); // exact-range note, never accidentally overwrite an overlapping note
        QVERIFY(bible->selectionHighlighted()); bible->toggleHighlight();
        QVERIFY(!bible->selectionHighlighted()); QVERIFY(bible->selectionBookmarked());
        QVERIFY(bible->verses()[15].toMap().value("highlighted").toBool());
        QVERIFY(!bible->verses()[16].toMap().value("highlighted").toBool());
        QVERIFY(bible->verses()[17].toMap().value("highlighted").toBool());
        bible->selectVerses(16,18);
        QVERIFY(!bible->selectionHighlighted()); QVERIFY(!bible->noteText().isEmpty());
        bible->toggleHighlight(); QVERIFY(bible->selectionHighlighted());
        bible->toggleHighlight(); QVERIFY(!bible->selectionHighlighted());
        for (int i = 15; i < 18; ++i) QVERIFY(!bible->verses()[i].toMap().value("highlighted").toBool());
        bible->selectVerses(18,16); QCOMPARE(bible->selectedStart(),16); QCOMPARE(bible->selectedEnd(),18);
        bible->moveSelection(1,true); QCOMPARE(bible->selectedStart(),17); QCOMPARE(bible->selectedEnd(),18);
        bible->moveSelection(2,true); QCOMPARE(bible->selectedStart(),18); QCOMPARE(bible->selectedEnd(),19);
        QVERIFY(bible->navigate("Mark 9:43")); bible->moveSelection(1); QCOMPARE(bible->selectedStart(),45);
        bible->moveSelection(1,true); QCOMPARE(bible->selectedEnd(),47);
        bible->toggleHighlight(); QVERIFY(bible->selectionHighlighted());
        bible->selectVerses(45,45); bible->toggleHighlight(); QVERIFY(!bible->selectionHighlighted());
        bible->selectVerses(47,47); QVERIFY(bible->selectionHighlighted());
    }
    void synchronousPersistenceAndReuse() {
        if (!bible->ready()) QSKIP("Requires real EPUB");
        QVERIFY(bible->navigate("John 3:16-18")); bible->setReadingPosition(0.42);
        const QString note = bible->noteText();
        QVERIFY(!note.isEmpty());
        bible.reset(); bible = std::make_unique<BibleBackend>();
        QVERIFY(bible->ready()); QVERIFY(!bible->busy()); QCOMPARE(bible->selectionReference(),QString("John 3:16–18"));
        QCOMPARE(bible->readingPosition(),0.42); QCOMPARE(bible->noteText(),note); QVERIFY(bible->selectionBookmarked());
        const QString copied = bible->copySelection();
        QCOMPARE(QGuiApplication::clipboard()->text(),copied);
        QVERIFY(copied.startsWith("John 3:16–18 (NET 2.1)\n16 "));
        QVERIFY(!copied.contains("\n19 "));
        bible->clearSelection(); QVERIFY(bible->noteText().isEmpty());
        QVERIFY(bible->copySelection().contains("\n36 "));
    }
    void backupRoundTripAndValidation() {
        if (!bible->ready()) QSKIP("Requires real EPUB");
        QVERIFY(bible->navigate("John 3:16-18")); bible->setReadingPosition(0.73);
        const auto original = bible->collection(); const QString note = bible->noteText();
        QVERIFY(bible->exportBackup(QUrl::fromLocalFile(file("backup.json"))));
        QFile source(file("backup.json")); QVERIFY(source.open(QIODevice::ReadOnly));
        const auto document = QJsonDocument::fromJson(source.readAll());
        QVERIFY(document.isObject());
        const auto rootObject = document.object();
        QByteArray duplicateFields = document.toJson();
        duplicateFields.replace("\"version\": 1", "\"version\": 2, \"ver\\u0073ion\": 1");
        QVERIFY(write(file("duplicate-fields.json"),duplicateFields));
        QVERIFY(!bible->importBackup(QUrl::fromLocalFile(file("duplicate-fields.json"))));
        QVERIFY(bible->error().contains("duplicate JSON"));
        const QStringList corruptions = {"version", "fractional", "wrong-type", "duplicate", "unknown-key", "out-of-range", "omitted", "empty", "reading", "note-type"};
        for (const auto &kind : corruptions) {
            auto object = rootObject;
            auto rows = object["annotations"].toArray(); auto a = rows.first().toObject();
            if (kind == "version") object["version"] = 2;
            if (kind == "fractional") a["start"] = 16.5;
            if (kind == "wrong-type") a["bookmark"] = 1;
            if (kind == "unknown-key") a["extra"] = "unexpected";
            if (kind == "out-of-range") a["chapter"] = 151;
            if (kind == "omitted") { a["book"] = 43; a["chapter"] = 5; a["start"] = 4; a["end"] = 4; }
            if (kind == "empty") { a["bookmark"] = false; a["highlight"] = false; a["note"] = ""; }
            if (kind == "note-type") a["note"] = 42;
            if (kind == "reading") { auto r = object["reading"].toObject(); r["position"] = 2; object["reading"] = r; }
            rows[0] = a;
            if (kind == "duplicate") rows.append(a);
            object["annotations"] = rows;
            QVERIFY(write(file("invalid.json"),QJsonDocument(object).toJson()));
            QVERIFY2(!bible->importBackup(QUrl::fromLocalFile(file("invalid.json"))),qPrintable(kind));
            QCOMPARE(bible->collection(),original); QCOMPARE(bible->noteText(),note); QCOMPARE(bible->readingPosition(),0.73);
        }
        bible->setNoteText("Changed after backup"); QVERIFY(bible->navigate("Genesis 1"));
        QVERIFY2(bible->importBackup(QUrl::fromLocalFile(file("backup.json"))),qPrintable(bible->error()));
        QCOMPARE(bible->collection(),original); QCOMPARE(bible->noteText(),note); QCOMPARE(bible->readingPosition(),0.73);
        QVERIFY(bible->exportNotes(QUrl::fromLocalFile(root.path())));
        QFile markdown(file("omabible-notes.md")); QVERIFY(markdown.open(QIODevice::ReadOnly));
        const auto text = QString::fromUtf8(markdown.readAll()); QVERIFY(text.contains(note)); QVERIFY(text.contains("John 3:16–18"));
        QVERIFY(!bible->exportBackup(QUrl::fromLocalFile(bible->dataDirectory()+"/personal.sqlite")));
        QVERIFY(QFile::link(bible->dataDirectory()+"/personal.sqlite",file("database-alias.json")));
        QVERIFY(!bible->exportBackup(QUrl::fromLocalFile(file("database-alias.json"))));
    }
    void restoreAndAutosaveRollbackOnSqlFailure() {
        if (!bible->ready()) QSKIP("Requires real EPUB");
        const auto before = bible->collection(); const auto note = bible->noteText();
        const auto selection = bible->selectionReference();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","test-failure");
            db.setDatabaseName(bible->dataDirectory()+"/personal.sqlite"); QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TRIGGER test_reject BEFORE INSERT ON annotations BEGIN SELECT RAISE(ABORT,'injected disk failure'); END"));
            bible->setNoteText("This write must fail");
            QVERIFY(bible->error().contains("injected disk failure"));
            QCOMPARE(bible->noteText(),note);
            QVERIFY(!bible->importBackup(QUrl::fromLocalFile(file("backup.json"))));
            QCOMPARE(bible->collection(),before); QCOMPARE(bible->selectionReference(),selection);
            QVERIFY(q.exec("DROP TRIGGER test_reject"));
            db.close();
        }
        QSqlDatabase::removeDatabase("test-failure");
        bible->refreshCollection(); QCOMPARE(bible->collection(),before); QCOMPARE(bible->noteText(),note);
    }
    void exportMarkdownProtectsDatabases() {
        if (!bible->ready()) QSKIP("Requires real EPUB");
        QTemporaryDir directory;
        const QString destination = directory.filePath("omabible-notes.md");
        const auto before = bible->collection();
        for (const QString suffix : {"personal.sqlite","bible.sqlite","personal.sqlite-wal","personal.sqlite-shm","bible.sqlite-journal"}) {
            const QString path = bible->dataDirectory()+'/'+suffix;
            QFile source(path); QByteArray original;
            const bool exists = source.exists();
            if (exists) { QVERIFY(source.open(QIODevice::ReadOnly)); original = source.readAll(); source.close(); }
            QVERIFY(QFile::link(path,destination));
            QVERIFY(!bible->exportNotes(QUrl::fromLocalFile(directory.path())));
            QVERIFY(bible->error().contains("live database"));
            QCOMPARE(QFile::exists(path),exists);
            if (exists) { QVERIFY(source.open(QIODevice::ReadOnly)); QCOMPARE(source.readAll(),original); }
            QVERIFY(QFile::remove(destination));
        }
        bible->refreshCollection(); QCOMPARE(bible->collection(),before);
    }
    void navigationAndRestoreReadFailureAreAtomic() {
        if (!bible->ready()) QSKIP("Requires real EPUB");
        QVERIFY(bible->navigate("Genesis 1"));
        QVERIFY(bible->exportBackup(QUrl::fromLocalFile(file("read-failure.json"))));
        QVERIFY(bible->navigate("John 3:16-18"));
        bible->setReadingPosition(0.61); bible->setNoteText("Keep this on failed restore");
        const auto before = bible->collection(), verses = bible->verses();
        const auto note = bible->noteText(), selection = bible->selectionReference();
        QSignalSpy navigation(bible.get(),&BibleBackend::navigationChanged);
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","read-failure"); db.setDatabaseName(bible->dataDirectory()+"/bible.sqlite"); QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("ALTER TABLE verses RENAME TO saved_verses"));
            // Endpoints remain queryable; loading verse bodies fails independently.
            QVERIFY(q.exec("CREATE VIEW verses AS SELECT book,chapter,number,CASE WHEN book=1 THEN abs(-9223372036854775808) ELSE text END AS text,html,heading,poetry FROM saved_verses"));
            QVERIFY(!bible->navigate("Genesis 1"));
            QVERIFY(!bible->error().isEmpty());
            QVERIFY(!bible->importBackup(QUrl::fromLocalFile(file("read-failure.json"))));
            QVERIFY(!bible->error().isEmpty());
            QCOMPARE(bible->selectionReference(),selection); QCOMPARE(bible->verses(),verses);
            QCOMPARE(bible->collection(),before); QCOMPARE(bible->noteText(),note); QCOMPARE(bible->readingPosition(),0.61);
            QCOMPARE(navigation.count(),0);
            QVERIFY(q.exec("DROP VIEW verses")); QVERIFY(q.exec("ALTER TABLE saved_verses RENAME TO verses"));
        }
        QSqlDatabase::removeDatabase("read-failure");
        bible.reset(); bible = std::make_unique<BibleBackend>();
        QVERIFY2(bible->ready(),qPrintable(bible->error())); QCOMPARE(bible->selectionReference(),selection);
        QCOMPARE(bible->collection(),before); QCOMPARE(bible->noteText(),note); QCOMPARE(bible->readingPosition(),0.61);
    }
    void successfulReimportPreservesAnnotations() {
        if (!bible->ready()) QSKIP("Requires real EPUB");
        const auto before = bible->collection(); const auto selection = bible->selectionReference();
        bible->importEpub(QUrl::fromLocalFile(epub));
        QVERIFY(bible->busy()); QVERIFY(bible->ready());
        bible->setNoteText("Saved while the import worker was running");
        QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(),120000);
        QVERIFY2(bible->ready(),qPrintable(bible->error()));
        QCOMPARE(bible->noteText(),QString("Saved while the import worker was running"));
        QCOMPARE(bible->selectionReference(),selection); QCOMPARE(bible->collection().size(),before.size());
    }
    void restoreCollectionReadFailureRollsBack() {
        if (!bible->ready()) QSKIP("Requires real EPUB");
        const auto before = bible->collection(), verses = bible->verses();
        const auto note = bible->noteText(), selection = bible->selectionReference();
        const double position = bible->readingPosition();
        QSignalSpy navigation(bible.get(),&BibleBackend::navigationChanged);
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","collection-read-failure");
            db.setDatabaseName(bible->dataDirectory()+"/personal.sqlite"); QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("ALTER TABLE annotations RENAME TO saved_annotations"));
            QVERIFY(q.exec("CREATE TABLE fault_gate(active INTEGER)"));
            QVERIFY(q.exec("INSERT INTO fault_gate VALUES(0)"));
            QVERIFY(q.exec("CREATE VIEW annotations AS SELECT book,chapter,start,end,bookmark,highlight,CASE WHEN (SELECT active FROM fault_gate)=1 THEN abs(-9223372036854775808) ELSE note END AS note FROM saved_annotations"));
            QVERIFY(q.exec("CREATE TRIGGER delete_annotations INSTEAD OF DELETE ON annotations BEGIN DELETE FROM saved_annotations WHERE book=OLD.book AND chapter=OLD.chapter AND start=OLD.start AND end=OLD.end; END"));
            QVERIFY(q.exec("CREATE TRIGGER insert_annotations INSTEAD OF INSERT ON annotations BEGIN INSERT INTO saved_annotations VALUES(NEW.book,NEW.chapter,NEW.start,NEW.end,NEW.bookmark,NEW.highlight,NEW.note); UPDATE fault_gate SET active=1; END"));
            QVERIFY(!bible->importBackup(QUrl::fromLocalFile(file("backup.json"))));
            QVERIFY2(bible->error().contains("Cannot read your annotations"),qPrintable(bible->error()));
            QCOMPARE(bible->collection(),before); QCOMPARE(bible->verses(),verses);
            QCOMPARE(bible->noteText(),note); QCOMPARE(bible->selectionReference(),selection);
            QCOMPARE(bible->readingPosition(),position); QCOMPARE(navigation.count(),0);
            QVERIFY(q.exec("DROP VIEW annotations"));
            QVERIFY(q.exec("DROP TABLE fault_gate"));
            QVERIFY(q.exec("ALTER TABLE saved_annotations RENAME TO annotations"));
        }
        QSqlDatabase::removeDatabase("collection-read-failure");
        bible.reset(); bible = std::make_unique<BibleBackend>();
        QVERIFY(bible->ready()); QCOMPARE(bible->collection(),before); QCOMPARE(bible->noteText(),note);
        QCOMPARE(bible->selectionReference(),selection); QCOMPARE(bible->readingPosition(),position);
    }
    void failedReimportPreservesData() {
        if (!bible->ready()) QSKIP("Requires real EPUB");
        const auto rows = bible->collection(); const QString selection = bible->selectionReference();
        bible->importEpub(QUrl::fromLocalFile(file("bad.epub")));
        QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(),10000);
        QVERIFY(bible->ready()); QVERIFY(!bible->error().isEmpty());
        QCOMPARE(bible->collection(),rows); QCOMPARE(bible->selectionReference(),selection);
        QVERIFY(bible->navigate("Revelation 22:21"));
        bible->nextChapter(); QCOMPARE(bible->passage(),QString("Revelation 22"));
        QVERIFY(bible->navigate("Genesis 1")); bible->previousChapter(); QCOMPARE(bible->passage(),QString("Genesis 1"));
    }
};

QTEST_MAIN(BackendTest)
#include "tst_backend.moc"
