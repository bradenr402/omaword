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
#include <QScopeGuard>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <limits>
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
        QTest::newRow("cross-chapter") << "John3:35-4:2" << "John 3:35–4:2";
        QTest::newRow("cross-book") << "Mal4:6-Matt1:2" << "Malachi 4:6–Matthew 1:2";
    }
    void references() {
        if (!bible->ready()) QSKIP("Requires real EPUB");
        QFETCH(QString,input); QFETCH(QString,expected);
        QVERIFY2(bible->navigate(input),qPrintable(bible->error())); QCOMPARE(bible->selectionReference(),expected);
    }
    void invalidReferences() {
        if (!bible->ready()) QSKIP("Requires real EPUB");
        QVERIFY(bible->navigate("John 3:16"));
        const QStringList invalid = {"John 0","John 22","John 3:0","John 3:99","John 3:18-16", "John 4:2-3:35", "John 3:1-0:2",
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
        duplicateFields.replace("\"version\": 2", "\"version\": 1, \"ver\\u0073ion\": 2");
        QVERIFY(write(file("duplicate-fields.json"),duplicateFields));
        QVERIFY(!bible->importBackup(QUrl::fromLocalFile(file("duplicate-fields.json"))));
        QVERIFY(bible->error().contains("duplicate JSON"));
        const QStringList corruptions = {"version", "fractional", "wrong-type", "duplicate", "unknown-key", "out-of-range", "omitted", "empty", "reading", "note-type"};
        for (const auto &kind : corruptions) {
            auto object = rootObject;
            auto rows = object["annotations"].toArray(); auto a = rows.first().toObject();
            if (kind == "version") object["version"] = 3;
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
            QVERIFY(q.exec("CREATE VIEW annotations AS SELECT book,chapter,start,end,bookmark,highlight,CASE WHEN (SELECT active FROM fault_gate)=1 THEN abs(-9223372036854775808) ELSE note END AS note,endBook,endChapter FROM saved_annotations"));
            QVERIFY(q.exec("CREATE TRIGGER delete_annotations INSTEAD OF DELETE ON annotations BEGIN DELETE FROM saved_annotations WHERE book=OLD.book AND chapter=OLD.chapter AND start=OLD.start AND end=OLD.end; END"));
            QVERIFY(q.exec("CREATE TRIGGER insert_annotations INSTEAD OF INSERT ON annotations BEGIN INSERT INTO saved_annotations VALUES(NEW.book,NEW.chapter,NEW.start,NEW.end,NEW.bookmark,NEW.highlight,NEW.note,NEW.endBook,NEW.endChapter); UPDATE fault_gate SET active=1; END"));
            bible->setNoteText("Readback failure must roll back this note");
            QVERIFY2(bible->error().contains("Cannot read your annotations"),qPrintable(bible->error()));
            QCOMPARE(bible->collection(),before); QCOMPARE(bible->noteText(),note);
            bible->toggleBookmark();
            QVERIFY2(bible->error().contains("Cannot read your annotations"),qPrintable(bible->error()));
            QCOMPARE(bible->collection(),before);
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
    void previewIsPure() {
        if (!bible->ready()) QSKIP("Requires real EPUB");
        QVERIFY(bible->navigate("John 3:35-4:2"));
        bible->setReadingPosition(0.37);
        QVERIFY(!bible->navigate("invalid")); // Preview must preserve even an existing error.
        const auto error = bible->error(), status = bible->status(), selection = bible->selectionReference();
        const auto verses = bible->verses(), collection = bible->collection();
        QSignalSpy navigation(bible.get(),&BibleBackend::navigationChanged);
        QSignalSpy changed(bible.get(),&BibleBackend::selectionChanged);
        QSignalSpy errors(bible.get(),&BibleBackend::errorChanged);
        QSignalSpy passages(bible.get(),&BibleBackend::passageChanged);
        QSignalSpy annotations(bible.get(),&BibleBackend::annotationChanged);
        const QMap<QString,QString> valid{{"Jn3:35-4:2","John 3:35–4:2"},
            {"Mal4:6—Mt1:2","Malachi 4:6–Matthew 1:2"},{"3:16","John 3:16"},
            {"Jude 5-7","Jude 1:5–7"},{"1Jn3:16","1 John 3:16"},
            {"Mark9:43-47","Mark 9:43–47"},{"Genesis","Genesis 1"}};
        for (auto it = valid.cbegin(); it != valid.cend(); ++it) {
            const auto result = bible->resolveReference(it.key());
            QCOMPARE(result.size(),3); QVERIFY2(result["valid"].toBool(),qPrintable(result["error"].toString()));
            QCOMPARE(result["reference"].toString(),it.value()); QVERIFY(result["error"].toString().isEmpty());
        }
        for (const QString input : {"", "John 3:35-0:2", "John 4:2-3:35", "John 5:4", "Mark 9:44-47",
                                   "John 3:35-4:99", "Matthew 1:2-Malachi 4:6", "Genesis-John", "John 3:1-Acts 1"}) {
            const auto result = bible->resolveReference(input);
            QVERIFY2(!result["valid"].toBool(),qPrintable(input));
            QVERIFY(result["reference"].toString().isEmpty()); QVERIFY(!result["error"].toString().isEmpty());
        }
        QCOMPARE(bible->error(),error); QCOMPARE(bible->status(),status); QCOMPARE(bible->selectionReference(),selection);
        QCOMPARE(bible->verses(),verses); QCOMPARE(bible->collection(),collection); QCOMPARE(bible->readingPosition(),0.37);
        QCOMPARE(navigation.count(),0); QCOMPARE(changed.count(),0); QCOMPARE(errors.count(),0);
        QCOMPARE(passages.count(),0); QCOMPARE(annotations.count(),0);
        bible.reset(); bible = std::make_unique<BibleBackend>();
        QVERIFY(bible->ready()); QCOMPARE(bible->selectionReference(),selection); QCOMPARE(bible->readingPosition(),0.37);
        QCOMPARE(bible->cursorVerse(),2); QCOMPARE(bible->chapter(),4);
        for (auto it = valid.cbegin(); it != valid.cend(); ++it) {
            // The relative reference is tested while John is still current.
            if (it.key() == "3:16") continue;
            QVERIFY(bible->navigate(it.key())); QCOMPARE(bible->selectionReference(),it.value());
        }
    }
    void canonicalMovementAndReversal() {
        if (!bible->ready()) QSKIP("Requires real EPUB");
        QVERIFY(bible->navigate("John 3:36"));
        bible->moveSelection(1); QCOMPARE(bible->selectionReference(),QString("John 4:1"));
        QCOMPARE(bible->cursorVerse(),1); QVERIFY(!bible->crossChapterSelection());
        bible->moveSelection(-1); QCOMPARE(bible->selectionReference(),QString("John 3:36"));
        bible->moveSelection(2,true); QCOMPARE(bible->selectionReference(),QString("John 3:36–4:2"));
        QCOMPARE(bible->chapter(),4); QCOMPARE(bible->selectedStart(),1); QCOMPARE(bible->selectedEnd(),2);
        QCOMPARE(bible->cursorVerse(),2); QVERIFY(bible->crossChapterSelection());
        bible->moveSelection(-3,true); QCOMPARE(bible->selectionReference(),QString("John 3:35–36"));
        QCOMPARE(bible->chapter(),3); QCOMPARE(bible->cursorVerse(),35); QVERIFY(!bible->crossChapterSelection());
        bible->moveSelection(1,true); QCOMPARE(bible->selectionReference(),QString("John 3:36"));
        bible->moveSelection(1,true); QVERIFY(bible->crossChapterSelection());
        bible->moveSelection(1); QCOMPARE(bible->selectionReference(),QString("John 4:2"));
        QVERIFY(!bible->crossChapterSelection());
        QVERIFY(bible->navigate("Matthew 1:1"));
        bible->moveSelection(-1); QCOMPARE(bible->selectionReference(),QString("Malachi 4:6"));
        bible->moveSelection(1); QCOMPARE(bible->selectionReference(),QString("Matthew 1:1"));
        bible->moveSelection(-2,true); QCOMPARE(bible->selectionReference(),QString("Malachi 4:5–Matthew 1:1"));
        QCOMPARE(bible->book(),39); QCOMPARE(bible->chapter(),4); QCOMPARE(bible->cursorVerse(),5);
        QCOMPARE(bible->selectedStart(),5); QCOMPARE(bible->selectedEnd(),6);
        bible->setReadingPosition(0.29);
        bible.reset(); bible = std::make_unique<BibleBackend>();
        QVERIFY(bible->ready()); QCOMPARE(bible->selectionReference(),QString("Malachi 4:5–Matthew 1:1"));
        QCOMPARE(bible->cursorVerse(),5); QCOMPARE(bible->readingPosition(),0.29);
        bible->moveSelection(3,true); QCOMPARE(bible->selectionReference(),QString("Matthew 1:1–2"));
        QCOMPARE(bible->cursorVerse(),2); // Original anchor survived restart and reversal.
        QVERIFY(bible->navigate("Mark 9:43"));
        bible->moveSelection(2,true); QCOMPARE(bible->selectionReference(),QString("Mark 9:43–47"));
        bible->moveSelection(-3,true); QCOMPARE(bible->selectionReference(),QString("Mark 9:42–43"));
        bible->moveSelection(std::numeric_limits<int>::min());
        QCOMPARE(bible->selectionReference(),QString("Genesis 1:1"));
        bible->moveSelection(-1,true); QCOMPARE(bible->selectionReference(),QString("Genesis 1:1"));
        bible->moveSelection(std::numeric_limits<int>::max());
        QCOMPARE(bible->selectionReference(),QString("Revelation 22:21"));
        bible->moveSelection(1,true); QCOMPARE(bible->selectionReference(),QString("Revelation 22:21"));
        bible->openChapter(43,4);
        QCOMPARE(bible->cursorVerse(),0); bible->moveSelection(1,true);
        QCOMPARE(bible->selectionReference(),QString("John 4:1"));
        bible->clearSelection(); bible->moveSelection(-1);
        QCOMPARE(bible->selectionReference(),QString("John 4:54"));
        QVERIFY(bible->navigate("John 3:35-4:2"));
        bible->selectVerses(4,2); QCOMPARE(bible->selectionReference(),QString("John 4:2–4"));
        QVERIFY(!bible->crossChapterSelection()); QCOMPARE(bible->cursorVerse(),2);
        QSignalSpy changed(bible.get(),&BibleBackend::selectionChanged);
        bible->clearSelection(); QCOMPARE(changed.count(),1);
        QCOMPARE(bible->selectedStart(),0); QCOMPARE(bible->selectedEnd(),0); QCOMPARE(bible->cursorVerse(),0);
        QCOMPARE(bible->selectionReference(),QString("John 4"));
        bible->clearSelection(); QCOMPARE(changed.count(),1);
    }
    void logicalRangesCopyAndSplitting() {
        if (!bible->ready()) QSKIP("Requires real EPUB");
        QVERIFY(bible->navigate("Malachi 4:5-Matthew 1:3"));
        bible->setNoteText("A note spanning the testament boundary");
        bible->toggleBookmark(); bible->toggleHighlight();
        QVERIFY(bible->selectionBookmarked()); QVERIFY(bible->selectionHighlighted());
        auto map = bible->verses().first().toMap();
        QVERIFY(map["bookmarked"].toBool()); QVERIFY(!map["bookmarkStart"].toBool());
        QVERIFY(map["bookmarkContinuesBefore"].toBool()); QVERIFY(map["bookmarkContinuesAfter"].toBool());
        map = bible->verses()[2].toMap(); QVERIFY(map["bookmarkEnd"].toBool()); QVERIFY(!map["bookmarkContinuesAfter"].toBool());
        const QString copied = bible->copySelection();
        QCOMPARE(QGuiApplication::clipboard()->text(),copied);
        QVERIFY(copied.startsWith("Malachi 4:5–Matthew 1:3 (NET 2.1)\nMalachi 4:5 "));
        for (const QString ref : {"Malachi 4:6", "Matthew 1:1", "Matthew 1:2", "Matthew 1:3"}) QVERIFY(copied.contains('\n'+ref+' '));
        QVERIFY(!copied.contains("\nMatthew 1:4 ")); QVERIFY(!copied.contains("\nMalachi 4:4 "));
        QCOMPARE(copied.count('\n'),5);
        QVERIFY(bible->exportNotes(QUrl::fromLocalFile(root.path())));
        QFile markdown(file("omabible-notes.md")); QVERIFY(markdown.open(QIODevice::ReadOnly));
        const auto text = markdown.readAll(); QVERIFY(text.contains(QString("Malachi 4:5–Matthew 1:3").toUtf8()));
        QVERIFY(text.contains("A note spanning the testament boundary"));
        // Subtract only the bookmark flag; the original note and highlight stay logical.
        QVERIFY(bible->navigate("Malachi 4:6-Matthew 1:1"));
        bible->toggleBookmark(); QVERIFY(!bible->selectionBookmarked()); QVERIFY(bible->selectionHighlighted());
        QVERIFY(bible->noteText().isEmpty());
        QVERIFY(bible->navigate("Malachi 4:5-Matthew 1:3"));
        QVERIFY(!bible->selectionBookmarked()); QVERIFY(bible->selectionHighlighted());
        QCOMPARE(bible->noteText(),QString("A note spanning the testament boundary"));
        bool left = false, right = false, whole = false;
        for (const auto &row : bible->collection()) {
            const auto a = row.toMap();
            if (a["reference"] == "Malachi 4:5") left = a["bookmark"].toBool();
            if (a["reference"] == "Matthew 1:2–3") right = a["bookmark"].toBool();
            if (a["reference"] == "Malachi 4:5–Matthew 1:3") {
                whole = true; QVERIFY(!a["bookmark"].toBool()); QVERIFY(a["highlight"].toBool());
                QCOMPARE(a["book"].toInt(),39); QCOMPARE(a["endBook"].toInt(),40);
                QCOMPARE(a["chapter"].toInt(),4); QCOMPARE(a["endChapter"].toInt(),1);
                QCOMPARE(a["start"].toInt(),5); QCOMPARE(a["end"].toInt(),3);
            }
        }
        QVERIFY(left); QVERIFY(right); QVERIFY(whole);
        QVERIFY(bible->navigate("Malachi 4:6-Matthew 1:1"));
        bible->toggleHighlight(); QVERIFY(!bible->selectionHighlighted());
        QVERIFY(bible->navigate("Malachi 4:5")); QVERIFY(bible->selectionHighlighted());
        QVERIFY(bible->verses()[4].toMap()["bookmarkStart"].toBool());
        QVERIFY(bible->verses()[4].toMap()["bookmarkEnd"].toBool());
        QVERIFY(bible->navigate("Matthew 1:2-3")); QVERIFY(bible->selectionHighlighted());
        QVERIFY(bible->navigate("Malachi 4:5-Matthew 1:3"));
        QVERIFY(!bible->selectionHighlighted()); QCOMPARE(bible->noteText(),QString("A note spanning the testament boundary"));
        bible->toggleBookmark(); // Restore full coverage while retaining overlapping logical ranges.
        QVERIFY(bible->navigate("Matthew 1:2"));
        map = bible->verses()[1].toMap(); QCOMPARE(map["bookmarkRanges"].toList().size(),2);
        QVERIFY(map["bookmarkStart"].toBool()); QVERIFY(map["bookmarkContinuesBefore"].toBool());
        // Every collection reference is parseable and reopens exactly its logical range.
        const auto collection = bible->collection();
        for (const auto &row : collection) {
            const auto a = row.toMap(); QVERIFY(bible->navigate(a["reference"].toString()));
            QCOMPARE(bible->selectionReference(),a["reference"].toString()); QCOMPARE(bible->noteText(),a["note"].toString());
        }
        QVERIFY(bible->navigate("Mark 9:42-10:2")); bible->toggleBookmark();
        QVERIFY(bible->navigate("Mark 9:43-45")); bible->toggleBookmark();
        QVERIFY(bible->navigate("Mark 9:47-10:2")); QVERIFY(bible->selectionBookmarked());
        bool omittedSplit = false;
        for (const auto &row : bible->collection())
            if (row.toMap()["reference"] == "Mark 9:47–10:2") omittedSplit = true;
        QVERIFY(omittedSplit);
        QVERIFY(bible->navigate("Mark 9:43-10:2"));
        const auto omittedCopy = bible->copySelection();
        QVERIFY(omittedCopy.contains("\nMark 9:45 ")); QVERIFY(omittedCopy.contains("\nMark 10:2 "));
        QVERIFY(!omittedCopy.contains("\nMark 9:44 ")); QVERIFY(!omittedCopy.contains("\nMark 9:46 "));
    }
    void crossRangeBackupAndAtomicFailure() {
        if (!bible->ready()) QSKIP("Requires real EPUB");
        QVERIFY(bible->navigate("Matthew 1:3")); bible->moveSelection(-4,true);
        QCOMPARE(bible->selectionReference(),QString("Malachi 4:5–Matthew 1:3"));
        bible->setReadingPosition(0.48);
        const auto collection = bible->collection(), verses = bible->verses();
        const auto note = bible->noteText();
        QVERIFY(bible->exportBackup(QUrl::fromLocalFile(file("cross-backup.json"))));
        QVERIFY(bible->navigate("Genesis 1"));
        QVERIFY(bible->importBackup(QUrl::fromLocalFile(file("cross-backup.json"))));
        QCOMPARE(bible->collection(),collection); QCOMPARE(bible->verses(),verses); QCOMPARE(bible->noteText(),note);
        QCOMPARE(bible->book(),39); QCOMPARE(bible->cursorVerse(),5); QCOMPARE(bible->readingPosition(),0.48);
        bible->moveSelection(5,true); QCOMPARE(bible->selectionReference(),QString("Matthew 1:3–4"));
        QVERIFY(bible->importBackup(QUrl::fromLocalFile(file("cross-backup.json"))));
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","cross-failure");
            db.setDatabaseName(bible->dataDirectory()+"/personal.sqlite"); QVERIFY(db.open()); QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TRIGGER reject_reading BEFORE UPDATE ON reading BEGIN SELECT RAISE(ABORT,'reading failure'); END"));
            QSignalSpy changes(bible.get(),&BibleBackend::selectionChanged);
            bible->moveSelection(5,true); QCOMPARE(bible->verses(),verses); QCOMPARE(bible->cursorVerse(),5);
            bible->clearSelection(); QCOMPARE(bible->cursorVerse(),5);
            QVERIFY(!bible->importBackup(QUrl::fromLocalFile(file("cross-backup.json"))));
            QCOMPARE(bible->collection(),collection); QCOMPARE(bible->noteText(),note); QCOMPARE(changes.count(),0);
            QVERIFY(q.exec("DROP TRIGGER reject_reading"));
            QVERIFY(q.exec("CREATE TRIGGER reject_range BEFORE INSERT ON annotations WHEN NEW.endBook>NEW.book BEGIN SELECT RAISE(ABORT,'range failure'); END"));
            bible->toggleHighlight(); QCOMPARE(bible->collection(),collection); QCOMPARE(bible->verses(),verses);
            bible->setNoteText("must roll back"); QCOMPARE(bible->noteText(),note);
            QVERIFY(bible->error().contains("range failure"));
            QVERIFY(q.exec("DROP TRIGGER reject_range"));
        }
        QSqlDatabase::removeDatabase("cross-failure");
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","copy-failure");
            db.setDatabaseName(bible->dataDirectory()+"/bible.sqlite"); QVERIFY(db.open()); QSqlQuery q(db);
            QVERIFY(q.exec("ALTER TABLE verses RENAME TO saved_verses"));
            QVERIFY(q.exec("CREATE VIEW verses AS SELECT book,chapter,number,CASE WHEN book=40 THEN abs(-9223372036854775808) ELSE text END AS text,html,heading,poetry FROM saved_verses"));
            QGuiApplication::clipboard()->setText("Keep clipboard on partial read failure");
            QVERIFY(bible->copySelection().isEmpty()); QVERIFY(!bible->error().isEmpty());
            QCOMPARE(QGuiApplication::clipboard()->text(),QString("Keep clipboard on partial read failure"));
            QCOMPARE(bible->verses(),verses); QCOMPARE(bible->collection(),collection);
            QVERIFY(q.exec("DROP VIEW verses")); QVERIFY(q.exec("ALTER TABLE saved_verses RENAME TO verses"));
        }
        QSqlDatabase::removeDatabase("copy-failure");
        bible.reset(); bible = std::make_unique<BibleBackend>();
        QVERIFY(bible->ready()); QCOMPARE(bible->collection(),collection); QCOMPARE(bible->noteText(),note);
        QCOMPARE(bible->selectionReference(),QString("Malachi 4:5–Matthew 1:3")); QCOMPARE(bible->cursorVerse(),5);
        QFile source(file("cross-backup.json")); QVERIFY(source.open(QIODevice::ReadOnly));
        const auto original = QJsonDocument::fromJson(source.readAll()).object();
        for (const QString kind : {"reversed", "missing-end", "fractional-end", "wrong-cursor", "half-selection"}) {
            auto object = original;
            auto rows = object["annotations"].toArray(); auto a = rows.first().toObject();
            auto r = object["reading"].toObject();
            if (kind == "reversed") { a["endBook"] = 1; a["endChapter"] = 1; a["end"] = 1; }
            if (kind == "missing-end") a.remove("endBook");
            if (kind == "fractional-end") a["endChapter"] = 1.5;
            if (kind == "wrong-cursor") r["cursorBook"] = 40;
            if (kind == "half-selection") r["start"] = 0;
            rows[0] = a; object["annotations"] = rows; object["reading"] = r;
            QVERIFY(write(file("bad-cross.json"),QJsonDocument(object).toJson()));
            QVERIFY2(!bible->importBackup(QUrl::fromLocalFile(file("bad-cross.json"))),qPrintable(kind));
            QCOMPARE(bible->collection(),collection); QCOMPARE(bible->cursorVerse(),5);
        }
    }
    void v1MigrationAndBackupCompatibility() {
        const QString application = QCoreApplication::applicationName();
        const auto restoreName = qScopeGuard([application] { QCoreApplication::setApplicationName(application); });
        QCoreApplication::setApplicationName("migration-test");
        const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QVERIFY(QDir().mkpath(directory));
        if (bible->ready()) QVERIFY(QFile::copy(bible->dataDirectory()+"/bible.sqlite",directory+"/bible.sqlite"));
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","migration"); db.setDatabaseName(directory+"/personal.sqlite"); QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TABLE annotations(book INTEGER,chapter INTEGER,start INTEGER,end INTEGER,bookmark INTEGER,highlight INTEGER,note TEXT,PRIMARY KEY(book,chapter,start,end)) WITHOUT ROWID"));
            QVERIFY(q.exec("INSERT INTO annotations VALUES(43,3,16,18,1,1,'Legacy note — שלום'),(41,9,43,47,1,0,'Omitted interior')"));
            QVERIFY(q.exec("CREATE TABLE reading(id INTEGER PRIMARY KEY,book INTEGER,chapter INTEGER,start INTEGER,end INTEGER,position REAL)"));
            QVERIFY(q.exec("INSERT INTO reading VALUES(1,43,3,16,18,0.63)"));
            QVERIFY(q.exec("PRAGMA user_version=1"));
            // A late migration conflict must undo table replacement and all prior ALTERs.
            QVERIFY(q.exec("ALTER TABLE reading ADD COLUMN cursorChapter INTEGER"));
        }
        QSqlDatabase::removeDatabase("migration");
        {
            BibleBackend failed;
            QVERIFY(!failed.ready()); QVERIFY(!failed.error().isEmpty());
            QVERIFY2(failed.error().contains("duplicate column"),qPrintable(failed.error()));
        }
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","migration"); db.setDatabaseName(directory+"/personal.sqlite"); QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("PRAGMA user_version")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),1); q.finish();
            QVERIFY(q.exec("SELECT note FROM annotations WHERE book=43")); QVERIFY(q.next()); QCOMPARE(q.value(0).toString(),QString("Legacy note — שלום")); q.finish();
            QVERIFY(q.exec("SELECT COUNT(*) FROM annotations")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),2); q.finish();
            QVERIFY(q.exec("SELECT COUNT(*) FROM sqlite_master WHERE name='annotations_v1'")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),0); q.finish();
            QVERIFY(!q.exec("SELECT anchorBook FROM reading")); // Earlier ALTER rolled back too.
            QVERIFY(q.exec("ALTER TABLE reading DROP COLUMN cursorChapter"));
        }
        QSqlDatabase::removeDatabase("migration");
        auto migrated = std::make_unique<BibleBackend>();
        QVERIFY2(migrated->error().isEmpty(),qPrintable(migrated->error()));
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","migration"); db.setDatabaseName(directory+"/personal.sqlite"); QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("PRAGMA user_version")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),2); q.finish();
            QVERIFY(q.exec("SELECT book,chapter,start,end,endBook,endChapter,note FROM annotations WHERE book=43")); QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(),43); QCOMPARE(q.value(1).toInt(),3); QCOMPARE(q.value(2).toInt(),16);
            QCOMPARE(q.value(3).toInt(),18); QCOMPARE(q.value(4).toInt(),43); QCOMPARE(q.value(5).toInt(),3);
            QCOMPARE(q.value(6).toString(),QString("Legacy note — שלום"));
        }
        QSqlDatabase::removeDatabase("migration");
        if (!migrated->ready()) {
            const auto preview = migrated->resolveReference("John 3:16");
            QVERIFY(!preview["valid"].toBool()); QVERIFY(!preview["error"].toString().isEmpty());
            return;
        }
        QCOMPARE(migrated->selectionReference(),QString("John 3:16–18")); QCOMPARE(migrated->cursorVerse(),18);
        QCOMPARE(migrated->readingPosition(),0.63); QCOMPARE(migrated->noteText(),QString("Legacy note — שלום"));
        QVERIFY(migrated->selectionBookmarked()); QVERIFY(migrated->selectionHighlighted()); QCOMPARE(migrated->collection().size(),2);
        const auto collection = migrated->collection();
        migrated.reset(); migrated = std::make_unique<BibleBackend>(); // v2 startup is idempotent.
        QCOMPARE(migrated->collection(),collection); QCOMPARE(migrated->readingPosition(),0.63);
        QJsonObject backup{{"format","omabible-backup"},{"version",1},{"edition","NET Bible 2.1"},
            {"created","2026-09-10T12:00:00.000Z"},
            {"reading",QJsonObject{{"book",43},{"chapter",3},{"start",16},{"end",18},{"position",0.63}}},
            {"annotations",QJsonArray{
                QJsonObject{{"book",43},{"chapter",3},{"start",16},{"end",18},{"bookmark",true},{"highlight",true},{"note","Legacy note — שלום"}},
                QJsonObject{{"book",41},{"chapter",9},{"start",43},{"end",47},{"bookmark",true},{"highlight",false},{"note","Omitted interior"}}}}};
        QVERIFY(write(file("v1.json"),QJsonDocument(backup).toJson()));
        migrated->setNoteText("Changed");
        QVERIFY2(migrated->importBackup(QUrl::fromLocalFile(file("v1.json"))),qPrintable(migrated->error()));
        QCOMPARE(migrated->collection(),collection); QCOMPARE(migrated->noteText(),QString("Legacy note — שלום"));
        QCOMPARE(migrated->cursorVerse(),18); QCOMPARE(migrated->readingPosition(),0.63);
        QVERIFY(migrated->exportBackup(QUrl::fromLocalFile(file("converted-v2.json"))));
        QFile converted(file("converted-v2.json")); QVERIFY(converted.open(QIODevice::ReadOnly));
        const auto object = QJsonDocument::fromJson(converted.readAll()).object(); QCOMPARE(object["version"].toInt(),2);
        QCOMPARE(object["annotations"].toArray().last().toObject()["endBook"].toInt(),43);
        auto reading = backup["reading"].toObject(); reading["start"] = 0; reading["end"] = 0; backup["reading"] = reading;
        QVERIFY(write(file("v1-none.json"),QJsonDocument(backup).toJson()));
        QVERIFY(migrated->importBackup(QUrl::fromLocalFile(file("v1-none.json"))));
        QCOMPARE(migrated->cursorVerse(),0); QCOMPARE(migrated->selectionReference(),QString("John 3"));
        QVERIFY(migrated->exportBackup(QUrl::fromLocalFile(file("v2-none.json"))));
        QVERIFY(migrated->navigate("Genesis 1:1"));
        QVERIFY(migrated->importBackup(QUrl::fromLocalFile(file("v2-none.json"))));
        QCOMPARE(migrated->cursorVerse(),0); QCOMPARE(migrated->readingPosition(),0.63);
        // v1 still rejects cross-chapter-shaped/reversed ranges, duplicate ranges and unknown keys.
        for (const QString kind : {"reversed", "duplicate", "unknown"}) {
            auto bad = backup; auto rows = bad["annotations"].toArray(); auto a = rows.first().toObject();
            if (kind == "reversed") a["end"] = 2;
            if (kind == "unknown") a["endChapter"] = 4;
            rows[0] = a; if (kind == "duplicate") rows.append(a); bad["annotations"] = rows;
            QVERIFY(write(file("v1-bad.json"),QJsonDocument(bad).toJson()));
            QVERIFY(!migrated->importBackup(QUrl::fromLocalFile(file("v1-bad.json"))));
            QCOMPARE(migrated->collection(),collection); QCOMPARE(migrated->cursorVerse(),0);
        }
    }
};

QTEST_MAIN(BackendTest)
#include "tst_backend.moc"
