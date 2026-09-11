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
#include <QSignalBlocker>
#include <QScopeGuard>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <limits>
#include <archive.h>
#include <archive_entry.h>
#include <unistd.h>

class BackendTest : public QObject
{
    Q_OBJECT
    QTemporaryDir root;
    QString epub;
    std::unique_ptr<BibleBackend> bible;
    QString file(const QString &name) const { return root.filePath(name); }
    static QString biblePath(BibleBackend *backend) {
        for (const auto &name : QSqlDatabase::connectionNames()) {
            auto db = QSqlDatabase::database(name);
            if (db.databaseName() != backend->dataDirectory()+"/personal.sqlite") continue;
            QSqlQuery q(db); q.prepare("SELECT path FROM installed WHERE version_id=?"); q.addBindValue(backend->versionId());
            if (q.exec() && q.next()) return backend->dataDirectory()+'/'+q.value(0).toString();
        }
        return {};
    }
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
        QCoreApplication::setOrganizationName("OmaWordTests");
        QCoreApplication::setApplicationName("omaword-test");
        epub = qEnvironmentVariable("OMAWORD_TEST_EPUB");
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
    void durablePreferences() {
        const QString application = QCoreApplication::applicationName();
        const auto restoreName = qScopeGuard([application] { QCoreApplication::setApplicationName(application); });
        QCoreApplication::setApplicationName("preferences-test");
        auto prefs = std::make_unique<BibleBackend>();
        QVERIFY2(prefs->error().isEmpty(),qPrintable(prefs->error()));
        QVERIFY(prefs->showVerseNumbers()); QVERIFY(prefs->showHeadings()); QVERIFY(prefs->versePicker());
        QCOMPARE(prefs->lineHeight(),1.5); QVERIFY(!prefs->focusMode()); QVERIFY(!prefs->blockMode());
        QCOMPARE(prefs->readingMode(),QString("paragraph")); QCOMPARE(prefs->fontSize(),18);
        QVERIFY(prefs->chapterVerses(1,1).isEmpty());
        const QString path = prefs->dataDirectory()+"/personal.sqlite";
        // QML can both assign the properties and invoke each setter.
        for (const QString name : {"showVerseNumbers", "showHeadings", "versePicker", "lineHeight", "focusMode", "blockMode", "readingMode", "fontSize"}) {
            const auto property = prefs->metaObject()->property(prefs->metaObject()->indexOfProperty(qPrintable(name)));
            QVERIFY(property.isReadable()); QVERIFY(property.isWritable());
            QCOMPARE(property.notifySignal().name(),QByteArray("preferencesChanged"));
        }
        QSignalSpy changed(prefs.get(),&BibleBackend::preferencesChanged);
        QVERIFY(QMetaObject::invokeMethod(prefs.get(),"setShowVerseNumbers",Q_ARG(bool,false)));
        QVERIFY(prefs->setProperty("showHeadings",false));
        QVERIFY(QMetaObject::invokeMethod(prefs.get(),"setVersePicker",Q_ARG(bool,false)));
        QVERIFY(QMetaObject::invokeMethod(prefs.get(),"setFocusMode",Q_ARG(bool,true)));
        QVERIFY(QMetaObject::invokeMethod(prefs.get(),"setLineHeight",Q_ARG(double,2.26)));
        QVERIFY(QMetaObject::invokeMethod(prefs.get(),"setBlockMode",Q_ARG(bool,true)));
        QCOMPARE(prefs->lineHeight(),2.3); QCOMPARE(changed.count(),6);
        prefs->setLineHeight(2.3); prefs->setShowHeadings(false); prefs->setBlockMode(true); QCOMPARE(changed.count(),6);
        prefs->setLineHeight(-100); QCOMPARE(prefs->lineHeight(),1.0);
        prefs->setLineHeight(100); QCOMPARE(prefs->lineHeight(),3.0);
        const int count = changed.count();
        for (double value : {std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
            prefs->setLineHeight(value); QVERIFY(prefs->error().contains("finite"));
            QCOMPARE(prefs->lineHeight(),3.0); QCOMPARE(changed.count(),count);
        }
        prefs.reset(); prefs = std::make_unique<BibleBackend>();
        QVERIFY(prefs->error().isEmpty()); QVERIFY(!prefs->showVerseNumbers()); QVERIFY(!prefs->showHeadings());
        QVERIFY(!prefs->versePicker()); QVERIFY(prefs->focusMode()); QCOMPARE(prefs->lineHeight(),3.0);
        QVERIFY(prefs->blockMode());
        // Invalid stored types, booleans, bounds and off-step heights use defaults.
        prefs.reset();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","preferences-test"); db.setDatabaseName(path); QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("UPDATE preferences SET value=CASE key WHEN 'showVerseNumbers' THEN 'false' WHEN 'showHeadings' THEN 2 WHEN 'versePicker' THEN -1 WHEN 'focusMode' THEN 0.5 ELSE 99 END"));
            for (const QString value : {"99", "0.9", "1.55", "'1.7'", "'NaN'", "1e999"}) {
                QVERIFY(q.exec("UPDATE preferences SET value="+value+" WHERE key='lineHeight'"));
                BibleBackend invalid;
                QVERIFY2(invalid.error().isEmpty(),qPrintable(invalid.error()));
                QVERIFY(invalid.showVerseNumbers()); QVERIFY(invalid.showHeadings()); QVERIFY(invalid.versePicker());
                QVERIFY(!invalid.focusMode()); QVERIFY(!invalid.blockMode()); QCOMPARE(invalid.lineHeight(),1.5);
            }
            // Existing schema-2 installs acquire the additive table on next open.
            QVERIFY(q.exec("DROP TABLE preferences"));
            QVERIFY(q.exec("PRAGMA user_version")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),4); q.finish();
        }
        QSqlDatabase::removeDatabase("preferences-test");
        prefs = std::make_unique<BibleBackend>();
        QVERIFY(prefs->error().isEmpty()); QCOMPARE(prefs->lineHeight(),1.5);
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","preferences-test"); db.setDatabaseName(path); QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("SELECT COUNT(*) FROM preferences")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),8); q.finish();
            QVERIFY(q.exec("CREATE TRIGGER reject_preferences BEFORE UPDATE ON preferences BEGIN SELECT RAISE(ABORT,'preference disk failure'); END"));
            QSignalSpy changes(prefs.get(),&BibleBackend::preferencesChanged);
            prefs->setShowVerseNumbers(false); prefs->setShowHeadings(false); prefs->setVersePicker(false);
            prefs->setFocusMode(true); prefs->setLineHeight(2); prefs->setBlockMode(true);
            QVERIFY(prefs->error().contains("preference disk failure")); QCOMPARE(changes.count(),0);
            QVERIFY(prefs->showVerseNumbers()); QVERIFY(prefs->showHeadings()); QVERIFY(prefs->versePicker());
            QVERIFY(!prefs->focusMode()); QVERIFY(!prefs->blockMode()); QCOMPARE(prefs->lineHeight(),1.5);
            QVERIFY(q.exec("DROP TRIGGER reject_preferences"));
            // A deferred constraint fails at COMMIT, after the write/readback succeed.
            for (const auto &name : QSqlDatabase::connectionNames()) {
                auto connection = QSqlDatabase::database(name);
                if (connection.databaseName() == path) { QSqlQuery pragma(connection); QVERIFY(pragma.exec("PRAGMA foreign_keys=ON")); }
            }
            QVERIFY(q.exec("CREATE TABLE preference_parent(id INTEGER PRIMARY KEY)"));
            QVERIFY(q.exec("CREATE TABLE preference_failure(id INTEGER REFERENCES preference_parent(id) DEFERRABLE INITIALLY DEFERRED)"));
            QVERIFY(q.exec("CREATE TRIGGER fail_preference_commit AFTER UPDATE ON preferences BEGIN INSERT INTO preference_failure VALUES(1); END"));
            prefs->setLineHeight(2.1);
            prefs->setBlockMode(true);
            QVERIFY2(prefs->error().contains("Cannot commit your preferences"),qPrintable(prefs->error()));
            QCOMPARE(prefs->lineHeight(),1.5); QCOMPARE(changes.count(),0);
            QVERIFY(!prefs->blockMode());
            QVERIFY(q.exec("SELECT value FROM preferences WHERE key='blockMode'")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),0); q.finish();
            QVERIFY(q.exec("SELECT value FROM preferences WHERE key='lineHeight'")); QVERIFY(q.next()); QCOMPARE(q.value(0).toDouble(),1.5); q.finish();
            QVERIFY(q.exec("SELECT COUNT(*) FROM preference_failure")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),0); q.finish();
            QVERIFY(q.exec("DROP TRIGGER fail_preference_commit"));
            prefs->setLineHeight(2.1); QCOMPARE(prefs->lineHeight(),2.1); QCOMPARE(changes.count(),1);
            QVERIFY(prefs->setProperty("blockMode",true)); QCOMPARE(changes.count(),2);
        }
        QSqlDatabase::removeDatabase("preferences-test");
        prefs.reset(); prefs = std::make_unique<BibleBackend>();
        QCOMPARE(prefs->lineHeight(),2.1); QVERIFY(prefs->blockMode()); QVERIFY(prefs->error().isEmpty());
        prefs->setBlockMode(false);
        prefs.reset(); prefs = std::make_unique<BibleBackend>(); QVERIFY(!prefs->blockMode());
        QCOMPARE(prefs->readingMode(),QString("verse"));
        prefs->setReadingMode("paragraph");
        prefs->setFontSize(-100); QCOMPARE(prefs->fontSize(),8);
        prefs->setFontSize(100); QCOMPARE(prefs->fontSize(),40);
        prefs->setFontSize(23);
        QSignalSpy finalChanges(prefs.get(),&BibleBackend::preferencesChanged);
        prefs->setFontSize(23); prefs->setReadingMode("paragraph");
        prefs->setReadingMode("unknown"); QCOMPARE(finalChanges.count(),0);
        QCOMPARE(prefs->readingMode(),QString("paragraph"));
        prefs.reset(); prefs = std::make_unique<BibleBackend>();
        QCOMPARE(prefs->fontSize(),23); QCOMPARE(prefs->readingMode(),QString("paragraph"));
        prefs.reset();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","preferences-migration"); db.setDatabaseName(path); QVERIFY(db.open());
            QSqlQuery q(db);
            for (int old : {1,0}) {
                QVERIFY(q.exec("DELETE FROM preferences WHERE key='readingMode'"));
                QVERIFY(q.exec(QString("UPDATE preferences SET value=%1 WHERE key='blockMode'").arg(old)));
                { BibleBackend migrated; QCOMPARE(migrated.readingMode(),old ? QString("block") : QString("paragraph")); }
                QVERIFY(q.exec("SELECT value FROM preferences WHERE key='readingMode'")); QVERIFY(q.next());
                QCOMPARE(q.value(0).toString(),old ? QString("block") : QString("paragraph")); q.finish();
            }
            QVERIFY(q.exec("UPDATE preferences SET value=8.5 WHERE key='fontSize'"));
            { BibleBackend invalid; QCOMPARE(invalid.fontSize(),18); }
        }
        QSqlDatabase::removeDatabase("preferences-migration");
    }
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
        if (epub.isEmpty()) QSKIP("Set OMAWORD_TEST_EPUB to the user's NETBIBLE21.epub to run integration tests.");
        QSignalSpy ready(bible.get(),&BibleBackend::readyChanged);
        bible->importEpub(QUrl::fromLocalFile(epub));
        QVERIFY(bible->busy());
        QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(),120000);
        QVERIFY2(bible->ready(),qPrintable(bible->error()));
        QCOMPARE(ready.count(),1); QCOMPARE(bible->importProgress(),1.0);
        QCOMPARE(bible->verses().size(),31);
        QVERIFY(bible->hasParagraphs());
        QVERIFY(bible->verses()[5].toMap()["paragraphStart"].toBool());
        QVERIFY(!bible->verses()[6].toMap()["paragraphStart"].toBool());
        QString validationError;
        QVERIFY2(EpubImporter::validateDatabase(biblePath(bible.get()),&validationError),qPrintable(validationError));
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
        QVERIFY(QFile::copy(biblePath(bible.get()),file("compensated.sqlite")));
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
        QVERIFY(copied.startsWith("“¹⁶ ")); QVERIFY(copied.endsWith("”\n\n—John 3:16–18"));
        QVERIFY(!copied.contains("¹⁹ "));
        bible->clearSelection(); QVERIFY(bible->noteText().isEmpty());
        const auto chapterCopy = bible->copySelection();
        QVERIFY(chapterCopy.contains("³⁶ ")); QVERIFY(chapterCopy.endsWith("”\n\n—John 3"));
    }
    void backupRoundTripAndValidation() {
        if (!bible->ready()) QSKIP("Requires real EPUB");
        QVERIFY(bible->navigate("John 3:16-18")); bible->setReadingPosition(0.73);
        if (bible->collection().isEmpty()) bible->setNoteText("Backup validation fixture");
        const auto original = bible->collection(); const QString note = bible->noteText();
        QVERIFY(bible->exportBackup(QUrl::fromLocalFile(file("backup.json"))));
        bible->setShowVerseNumbers(false); bible->setShowHeadings(false); bible->setVersePicker(false);
        bible->setLineHeight(1.9); bible->setFocusMode(true); bible->setBlockMode(true);
        QSignalSpy preferences(bible.get(),&BibleBackend::preferencesChanged);
        QFile source(file("backup.json")); QVERIFY(source.open(QIODevice::ReadOnly));
        const auto document = QJsonDocument::fromJson(source.readAll());
        QVERIFY(document.isObject());
        const auto rootObject = document.object();
        QCOMPARE(rootObject.value("format").toString(), QString("omaword-backup"));
        // The current signature accepts the supported annotation schemas.
        {
            for (int version = 1; version <= 3; ++version) {
                auto legacy = rootObject;
                legacy["format"] = "omaword-backup";
                legacy["version"] = version;
                if (version < 3) legacy.remove("versionId");
                if (version == 1) {
                    auto reading = legacy["reading"].toObject();
                    for (const QString key : {"anchorBook", "anchorChapter", "cursorBook", "cursorChapter", "cursorVerse"}) reading.remove(key);
                    legacy["reading"] = reading;
                    auto rows = legacy["annotations"].toArray();
                    for (int i = 0; i < rows.size(); ++i) {
                        auto row = rows[i].toObject(); row.remove("endBook"); row.remove("endChapter"); rows[i] = row;
                    }
                    legacy["annotations"] = rows;
                }
                QVERIFY(write(file("brand-legacy.json"), QJsonDocument(legacy).toJson()));
                QVERIFY2(bible->importBackup(QUrl::fromLocalFile(file("brand-legacy.json"))), qPrintable(bible->error()));
            }
        }
        QByteArray duplicateFields = document.toJson();
        duplicateFields.replace("\"version\": 3", "\"version\": 1, \"ver\\u0073ion\": 3");
        QVERIFY(write(file("duplicate-fields.json"),duplicateFields));
        QVERIFY(!bible->importBackup(QUrl::fromLocalFile(file("duplicate-fields.json"))));
        QVERIFY(bible->error().contains("duplicate JSON"));
        const QStringList corruptions = {"format", "version", "fractional", "wrong-type", "duplicate", "unknown-key", "out-of-range", "omitted", "empty", "reading", "note-type"};
        for (const auto &kind : corruptions) {
            auto object = rootObject;
            auto rows = object["annotations"].toArray(); auto a = rows.first().toObject();
            if (kind == "version") object["version"] = 4;
            if (kind == "format") object["format"] = "unknown-backup";
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
        QVERIFY(!bible->showVerseNumbers()); QVERIFY(!bible->showHeadings()); QVERIFY(!bible->versePicker());
        QCOMPARE(bible->lineHeight(),1.9); QVERIFY(bible->focusMode()); QVERIFY(bible->blockMode()); QCOMPARE(preferences.count(),0);
        QCOMPARE(bible->collection(),original); QCOMPARE(bible->noteText(),note); QCOMPARE(bible->readingPosition(),0.73);
        QVERIFY(bible->exportNotes(QUrl::fromLocalFile(root.path())));
        QFile markdown(file("omaword-notes.md")); QVERIFY(markdown.open(QIODevice::ReadOnly));
        const auto text = QString::fromUtf8(markdown.readAll()); QVERIFY(text.contains(note)); QVERIFY(text.contains("John 3:16–18"));
        QVERIFY(text.startsWith("# OmaWord notes\n"));
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
        const QString destination = directory.filePath("omaword-notes.md");
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
            auto db = QSqlDatabase::addDatabase("QSQLITE","read-failure"); db.setDatabaseName(biblePath(bible.get())); QVERIFY(db.open());
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
            QVERIFY(q.exec("CREATE VIEW annotations AS SELECT book,chapter,start,end,bookmark,highlight,CASE WHEN (SELECT active FROM fault_gate)=1 THEN abs(-9223372036854775808) ELSE note END AS note,endBook,endChapter,version_id FROM saved_annotations"));
            QVERIFY(q.exec("CREATE TRIGGER delete_annotations INSTEAD OF DELETE ON annotations BEGIN DELETE FROM saved_annotations WHERE version_id=OLD.version_id AND book=OLD.book AND chapter=OLD.chapter AND start=OLD.start AND end=OLD.end AND endBook=OLD.endBook AND endChapter=OLD.endChapter; END"));
            QVERIFY(q.exec("CREATE TRIGGER insert_annotations INSTEAD OF INSERT ON annotations BEGIN INSERT INTO saved_annotations VALUES(NEW.book,NEW.chapter,NEW.start,NEW.end,NEW.bookmark,NEW.highlight,NEW.note,NEW.endBook,NEW.endChapter,NEW.version_id); UPDATE fault_gate SET active=1; END"));
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
    void countedMovementWithoutSelection() {
        if (!bible->ready()) QSKIP("Requires real EPUB");
        const auto check = [this](const QString &chapter, int delta, const QString &expected, bool extend = false) {
            QVERIFY(bible->navigate(chapter));
            QCOMPARE(bible->cursorVerse(),0);
            bible->moveSelection(delta,extend);
            QCOMPARE(bible->selectionReference(),expected);
            QCOMPARE(bible->selectedStart(),bible->selectedEnd());
            QVERIFY(!bible->crossChapterSelection());
        };
        check("John 3",1,"John 3:1");
        check("John 3",-1,"John 3:36");
        check("John 3",4,"John 3:4");
        check("John 3",-4,"John 3:33");
        check("John 3",38,"John 4:2");
        check("John 3",-38,"John 2:24");
        check("Matthew 17",21,"Matthew 17:22"); // Omitted verse 21.
        check("Matthew 17",-7,"Matthew 17:20");
        check("John 3",4,"John 3:4",true);
        check("John 3",0,"John 3");
        check("John 3",std::numeric_limits<int>::min(),"Genesis 1:1");
        check("John 3",std::numeric_limits<int>::max(),"Revelation 22:21");
    }
    void pickerAndContextCopy() {
        if (!bible->ready()) QSKIP("Requires real EPUB");
        QVERIFY(bible->navigate("John 3:16-18")); bible->setReadingPosition(0.38);
        const auto selection = bible->selectionReference(), note = bible->noteText();
        const auto verses = bible->verses(), collection = bible->collection();
        QVERIFY(!bible->navigate("invalid"));
        const auto error = bible->error(), status = bible->status();
        QSignalSpy navigation(bible.get(),&BibleBackend::navigationChanged);
        QSignalSpy changed(bible.get(),&BibleBackend::selectionChanged);
        QSignalSpy annotations(bible.get(),&BibleBackend::annotationChanged);
        QSignalSpy errors(bible.get(),&BibleBackend::errorChanged);
        const auto picker = bible->chapterVerses(41,9);
        QCOMPARE(picker.size(),48);
        QList<int> numbers;
        for (const auto &row : picker) {
            const auto map = row.toMap(); numbers << map["number"].toInt();
            QVERIFY(!map["text"].toString().isEmpty());
        }
        QVERIFY(numbers.contains(43)); QVERIFY(numbers.contains(45)); QVERIFY(numbers.contains(47));
        QVERIFY(!numbers.contains(44)); QVERIFY(!numbers.contains(46));
        QCOMPARE(numbers.first(),1); QCOMPARE(numbers.last(),50);
        for (const auto pair : {qMakePair(0,1),qMakePair(67,1),qMakePair(43,0),qMakePair(43,22)})
            QVERIFY(bible->chapterVerses(pair.first,pair.second).isEmpty());
        QCOMPARE(bible->error(),error); QCOMPARE(bible->status(),status); QCOMPARE(errors.count(),0);
        QCOMPARE(bible->selectionReference(),selection); QCOMPARE(bible->noteText(),note);
        QCOMPARE(navigation.count(),0); QCOMPARE(changed.count(),0); QCOMPARE(annotations.count(),0);
        const QString normal = bible->copySelection();
        QStringList texts, numbered;
        const QStringList superscripts{"¹⁶", "¹⁷", "¹⁸"};
        for (int i = 15; i < 18; ++i) {
            texts << verses[i].toMap()["text"].toString().simplified();
            numbered << superscripts[i-15]+' '+texts.last();
        }
        QCOMPARE(normal,QString("“")+numbered.join(' ')+"”\n\n—John 3:16–18");
        QCOMPARE(bible->copySelection(true),texts.join(' ')); QCOMPARE(changed.count(),0);
        // A clipboard observer must see both the new focus and its persisted state.
        bool focusedBeforeClipboard = false, persistedBeforeClipboard = false;
        const auto observer = connect(QGuiApplication::clipboard(),&QClipboard::dataChanged,this,[&] {
            focusedBeforeClipboard = bible->cursorVerse() == 17 && bible->selectedStart() == 17 && bible->selectedEnd() == 17;
            for (const auto &name : QSqlDatabase::connectionNames()) {
                auto db = QSqlDatabase::database(name);
                if (db.databaseName() != bible->dataDirectory()+"/personal.sqlite") continue;
                QSqlQuery q(db);
                persistedBeforeClipboard = q.exec("SELECT start,end FROM reading WHERE id=1") && q.next()
                    && q.value(0).toInt() == 17 && q.value(1).toInt() == 17;
            }
        });
        QCOMPARE(bible->copyVerse(17),normal);
        disconnect(observer); QVERIFY(focusedBeforeClipboard); QVERIFY(persistedBeforeClipboard);
        QCOMPARE(bible->selectionReference(),QString("John 3:17")); QCOMPARE(changed.count(),1);
        QVERIFY(bible->navigate(selection)); bible->setReadingPosition(0.38);
        QCOMPARE(bible->copyVerse(16,true),texts.join(' ')); QCOMPARE(bible->cursorVerse(),16);
        QCOMPARE(bible->copyVerse(20,true),verses[19].toMap()["text"].toString().simplified());
        QCOMPARE(bible->copyVerse(20),QString("“²⁰ ")+verses[19].toMap()["text"].toString().simplified()+"”\n\n—John 3:20");
        bible->copyText("<b>Plain selection</b>\nשלום");
        QCOMPARE(QGuiApplication::clipboard()->text(),QString("<b>Plain selection</b>\nשלום"));
        QVERIFY(bible->copyVerse(0).isEmpty()); QVERIFY(bible->copyVerse(37,true).isEmpty());
        QCOMPARE(QGuiApplication::clipboard()->text(),QString("<b>Plain selection</b>\nשלום"));
        QCOMPARE(bible->selectionReference(),QString("John 3:20"));
        QCOMPARE(bible->verses(),verses); QCOMPARE(bible->collection(),collection); QCOMPARE(bible->readingPosition(),0.38);
        bible.reset(); bible = std::make_unique<BibleBackend>();
        QCOMPARE(bible->selectionReference(),QString("John 3:20")); QCOMPARE(bible->cursorVerse(),20); QCOMPARE(bible->readingPosition(),0.38);
        QVERIFY(bible->navigate("Genesis 1:27"));
        const QString poetry = bible->verses()[26].toMap()["text"].toString(); QVERIFY(poetry.contains('\n'));
        QCOMPARE(bible->copyVerse(27),QString("“²⁷ ")+poetry.simplified()+"”\n\n—Genesis 1:27");
        bible->clearSelection();
        QCOMPARE(bible->copyVerse(27,true),poetry.simplified());
        QCOMPARE(bible->cursorVerse(),27);
        QVERIFY(bible->navigate("Mark 9:43-47"));
        const QString copied = bible->copyVerse(45);
        QVERIFY(copied.endsWith("”\n\n—Mark 9:43–47"));
        QVERIFY(copied.contains("⁴³ ")); QVERIFY(copied.contains("⁴⁵ ")); QVERIFY(copied.contains("⁴⁷ "));
        QVERIFY(!copied.contains("⁴⁴ ")); QVERIFY(!copied.contains("⁴⁶ ")); QCOMPARE(bible->cursorVerse(),45);
        QVERIFY(bible->copyVerse(44,true).isEmpty()); QCOMPARE(QGuiApplication::clipboard()->text(),copied);
        for (const QString ref : {"John 3:35-4:2", "Malachi 4:5-Matthew 1:3"}) {
            QVERIFY(bible->navigate(ref));
            const int b = bible->book(), c = bible->chapter(), end = bible->cursorVerse();
            const int previousBook = b == 40 ? 39 : b, previousChapter = b == 40 ? 4 : 3;
            const int start = b == 40 ? 5 : 35;
            QStringList expected;
            for (const auto &row : bible->chapterVerses(previousBook,previousChapter))
                if (row.toMap()["number"].toInt() >= start) expected << row.toMap()["text"].toString().simplified();
            for (const auto &row : bible->chapterVerses(b,c))
                if (row.toMap()["number"].toInt() <= end) expected << row.toMap()["text"].toString().simplified();
            const auto reference = bible->selectionReference();
            const auto formatted = bible->copySelection();
            QVERIFY(formatted.endsWith("”\n\n—"+reference)); QVERIFY(!formatted.contains("NET"));
            QCOMPARE(bible->copySelection(true),expected.join(' '));
            QCOMPARE(bible->copyVerse(1,true),expected.join(' '));
            QCOMPARE(QGuiApplication::clipboard()->text(),expected.join(' ')); QCOMPARE(bible->cursorVerse(),1);
            QVERIFY(!bible->crossChapterSelection());
            QVERIFY(bible->navigate(ref)); QCOMPARE(bible->copyVerse(1),formatted);
            QVERIFY(bible->navigate(ref));
            const auto outside = bible->copyVerse(end+1);
            QVERIFY(outside.endsWith("”\n\n—"+bible->passage()+":"+QString::number(end+1)));
            QCOMPARE(bible->cursorVerse(),end+1);
        }
        QVERIFY(bible->navigate("Psalm 119:108-109"));
        const auto digits = bible->copySelection(); QVERIFY(digits.startsWith("“¹⁰⁸ ")); QVERIFY(digits.contains(" ¹⁰⁹ "));
        // Backwards canonical ranges use the same old range before focusing a click.
        QVERIFY(bible->navigate("Matthew 1:3")); bible->moveSelection(-4,true);
        const auto reverse = bible->copySelection(); QCOMPARE(bible->copyVerse(6),reverse);
        QCOMPARE(bible->selectionReference(),QString("Malachi 4:6"));
    }
    void boundedMovement_data() {
        QTest::addColumn<int>("delta");
        for (int delta : {-10,-5,5,10}) QTest::newRow(qPrintable(QString::number(delta))) << delta;
    }
    void boundedMovement() {
        if (!bible->ready()) QSKIP("Requires real EPUB");
        QFETCH(int,delta);
        QVERIFY(bible->navigate("John 3:18"));
        bible->setReadingPosition(0.41); bible->moveByVerses(delta);
        QCOMPARE(bible->cursorVerse(),18+delta); QCOMPARE(bible->readingPosition(),0.41);
        const int start = delta > 0 ? 34 : 3, boundary = delta > 0 ? 36 : 1;
        QVERIFY(bible->navigate(QString("John 3:%1").arg(start)));
        bible->moveByVerses(delta,true); QCOMPARE(bible->cursorVerse(),boundary); QCOMPARE(bible->chapter(),3);
        QVERIFY(!bible->crossChapterSelection());
        bible->moveByVerses(delta,true);
        QCOMPARE(bible->chapter(),delta > 0 ? 4 : 2); QCOMPARE(bible->cursorVerse(),delta > 0 ? 1 : 25);
        QVERIFY(bible->crossChapterSelection());
        bible->moveByVerses(-delta,true); // Boundary reversal crosses just one verse back.
        QCOMPARE(bible->chapter(),3); QCOMPARE(bible->cursorVerse(),boundary);
        bible->moveByVerses(-delta,true);
        QCOMPARE(bible->cursorVerse(),boundary-delta);
        QCOMPARE(delta > 0 ? bible->selectedEnd() : bible->selectedStart(),start); // Original visual anchor.
        bible->openChapter(43,3);
        bible->moveByVerses(delta,true);
        QCOMPARE(bible->chapter(),3); QCOMPARE(bible->selectedStart(),1);
        QCOMPARE(bible->cursorVerse(),delta > 0 ? 1+delta : 1);
        bible->moveByVerses(0,true); QCOMPARE(bible->cursorVerse(),delta > 0 ? 1+delta : 1);
        QVERIFY(bible->navigate(delta > 0 ? "Malachi 4:5" : "Matthew 1:2"));
        bible->moveByVerses(delta); QCOMPARE(bible->cursorVerse(),delta > 0 ? 6 : 1);
        bible->moveByVerses(delta); QCOMPARE(bible->book(),delta > 0 ? 40 : 39); QCOMPARE(bible->cursorVerse(),delta > 0 ? 1 : 6);
        QVERIFY(bible->navigate(delta > 0 ? "Mark 9:40" : "Mark 9:50"));
        bible->moveByVerses(delta);
        QCOMPARE(bible->cursorVerse(),delta > 0 ? (delta == 5 ? 47 : 50) : (delta == -5 ? 43 : 38));
        QVERIFY(bible->navigate(delta > 0 ? "Revelation 22:20" : "Genesis 1:2"));
        bible->moveByVerses(delta); bible->moveByVerses(delta);
        QCOMPARE(bible->selectionReference(),delta > 0 ? QString("Revelation 22:21") : QString("Genesis 1:1"));
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
        QVERIFY(copied.startsWith("“⁵ ")); QVERIFY(copied.endsWith("”\n\n—Malachi 4:5–Matthew 1:3"));
        for (const QString number : {"⁶", "¹", "²", "³"}) QVERIFY(copied.contains(' '+number+' '));
        QVERIFY(!copied.contains("⁴ ")); QCOMPARE(copied.count('\n'),2);
        QVERIFY(bible->exportNotes(QUrl::fromLocalFile(root.path())));
        QFile markdown(file("omaword-notes.md")); QVERIFY(markdown.open(QIODevice::ReadOnly));
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
        QVERIFY(omittedCopy.contains("⁴⁵ ")); QVERIFY(omittedCopy.contains("² "));
        QVERIFY(!omittedCopy.contains("⁴⁴ ")); QVERIFY(!omittedCopy.contains("⁴⁶ "));
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
            bible->moveByVerses(10,true); QCOMPARE(bible->verses(),verses); QCOMPARE(bible->cursorVerse(),5);
            bible->clearSelection(); QCOMPARE(bible->cursorVerse(),5);
            bible->copyText("Keep clipboard on persistence failure");
            QVERIFY(bible->copyVerse(6).isEmpty()); QVERIFY(bible->copyVerse(4,true).isEmpty());
            QVERIFY(bible->error().contains("reading failure"));
            QCOMPARE(QGuiApplication::clipboard()->text(),QString("Keep clipboard on persistence failure"));
            QCOMPARE(bible->cursorVerse(),5); QCOMPARE(bible->readingPosition(),0.48);
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
            db.setDatabaseName(biblePath(bible.get())); QVERIFY(db.open()); QSqlQuery q(db);
            QVERIFY(q.exec("ALTER TABLE verses RENAME TO saved_verses"));
            QVERIFY(q.exec("CREATE VIEW verses AS SELECT book,chapter,number,CASE WHEN book=40 THEN abs(-9223372036854775808) ELSE text END AS text,html,heading,poetry FROM saved_verses"));
            QGuiApplication::clipboard()->setText("Keep clipboard on partial read failure");
            QVERIFY(bible->copySelection().isEmpty()); QVERIFY(!bible->error().isEmpty());
            const auto selection = bible->selectionReference();
            QSignalSpy changes(bible.get(),&BibleBackend::selectionChanged);
            QVERIFY(bible->copyVerse(5,true).isEmpty()); QVERIFY(bible->copyVerse(6).isEmpty());
            const auto error = bible->error();
            QVERIFY(bible->chapterVerses(40,1).isEmpty()); QCOMPARE(bible->error(),error);
            QCOMPARE(bible->selectionReference(),selection); QCOMPARE(bible->noteText(),note);
            QCOMPARE(bible->readingPosition(),0.48); QCOMPARE(changes.count(),0);
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
    void logicalHighlightMetadataAndExactDeletion() {
        if (!bible->ready()) QSKIP("Requires real EPUB");
        const auto original = bible->collection();
        QVERIFY(bible->navigate("Luke 1:2-4")); bible->toggleHighlight(); bible->toggleBookmark(); bible->setNoteText("Delete this entire row");
        QVERIFY(bible->navigate("Luke 1:5-6")); bible->toggleHighlight();
        auto left = bible->verses()[3].toMap(), right = bible->verses()[4].toMap();
        QVERIFY(left["highlightEnd"].toBool()); QVERIFY(!left["highlightContinuesAfter"].toBool());
        QVERIFY(right["highlightStart"].toBool()); QVERIFY(!right["highlightContinuesBefore"].toBool());
        QCOMPARE(left["highlightRanges"].toList().size(),1); QCOMPARE(right["highlightRanges"].toList().size(),1);
        QCOMPARE(left["highlightRanges"].toList().first().toMap()["reference"].toString(),QString("Luke 1:2–4"));
        QCOMPARE(right["highlightRanges"].toList().first().toMap()["reference"].toString(),QString("Luke 1:5–6"));
        QVERIFY(bible->navigate("Luke 1:3-7")); bible->toggleHighlight(); bible->setNoteText("Preserve overlapping note");
        left = bible->verses()[3].toMap();
        QCOMPARE(left["highlightRanges"].toList().size(),2);
        QVERIFY(left["highlightEnd"].toBool()); QVERIFY(left["highlightContinuesAfter"].toBool());
        for (const auto &range : left["highlightRanges"].toList()) {
            const auto map = range.toMap();
            QCOMPARE(map["highlightEnd"].toBool(),map["reference"] == "Luke 1:2–4");
            QCOMPARE(map["highlightContinuesAfter"].toBool(),map["reference"] == "Luke 1:3–7");
        }
        QVERIFY(bible->navigate("Luke 1:80-2:2")); bible->toggleHighlight();
        auto map = bible->verses().first().toMap();
        QVERIFY(!map["highlightStart"].toBool()); QVERIFY(map["highlightContinuesBefore"].toBool());
        QVERIFY(map["highlightContinuesAfter"].toBool());
        map = bible->verses()[1].toMap(); QVERIFY(map["highlightEnd"].toBool());
        QVERIFY(!map["highlightContinuesAfter"].toBool());
        QVERIFY(bible->navigate("Luke 1:80")); map = bible->verses().last().toMap();
        QVERIFY(map["highlightStart"].toBool()); QVERIFY(map["highlightContinuesAfter"].toBool());
        QVERIFY(!map["highlightContinuesBefore"].toBool());
        QVERIFY(bible->navigate("Luke 1:3-7")); bible->setReadingPosition(0.46);
        const auto before = bible->collection(), verses = bible->verses();
        const auto selection = bible->selectionReference(), note = bible->noteText();
        QSignalSpy navigation(bible.get(),&BibleBackend::navigationChanged);
        QSignalSpy selected(bible.get(),&BibleBackend::selectionChanged);
        QSignalSpy collection(bible.get(),&BibleBackend::collectionChanged);
        // Confirmation/cancellation belongs to the UI: merely resolving a target is pure.
        QVERIFY(bible->resolveReference("Luke 1:2-4")["valid"].toBool());
        QCOMPARE(bible->collection(),before); QCOMPARE(bible->verses(),verses); QCOMPARE(collection.count(),0);
        for (const QString missing : {"Luke 1:2-3", "Luke 1:1-7", "Luke 1", "invalid"}) {
            QVERIFY(!bible->deleteAnnotation(missing)); QVERIFY(!bible->error().isEmpty());
            QCOMPARE(bible->collection(),before); QCOMPARE(bible->verses(),verses);
        }
        QVERIFY(bible->deleteAnnotation("Lk1:2-1:4")); QVERIFY(bible->error().isEmpty());
        auto expected = before;
        expected.removeIf([](const QVariant &row) { return row.toMap()["reference"] == "Luke 1:2–4"; });
        QCOMPARE(bible->collection(),expected); QCOMPARE(collection.count(),1);
        QCOMPARE(bible->selectionReference(),selection); QCOMPARE(bible->noteText(),note);
        QCOMPARE(bible->readingPosition(),0.46); QCOMPARE(navigation.count(),0); QCOMPARE(selected.count(),0);
        map = bible->verses()[1].toMap(); QVERIFY(!map["highlighted"].toBool()); QVERIFY(!map["bookmarked"].toBool());
        QVERIFY(!map["hasNote"].toBool()); QVERIFY(map["highlightRanges"].toList().isEmpty());
        map = bible->verses()[3].toMap(); QVERIFY(map["highlighted"].toBool()); QVERIFY(map["hasNote"].toBool());
        QCOMPARE(map["highlightRanges"].toList().size(),1); QVERIFY(!map["highlightEnd"].toBool());
        QVERIFY(!bible->deleteAnnotation("Luke 1:2-4"));
        bible.reset(); bible = std::make_unique<BibleBackend>();
        QVERIFY(bible->ready()); QCOMPARE(bible->collection(),expected); QCOMPARE(bible->noteText(),note);
        QCOMPARE(bible->selectionReference(),selection); QCOMPARE(bible->readingPosition(),0.46);
        QVERIFY(bible->deleteAnnotation("Luke 1:3-7")); QVERIFY(bible->noteText().isEmpty());
        QVERIFY(!bible->selectionHighlighted());
        QVERIFY(bible->deleteAnnotation("Luke 1:5-6")); QVERIFY(bible->deleteAnnotation("Luke 1:80-2:2"));
        QVERIFY(bible->navigate("Obadiah 1:21-Jonah 1:2")); bible->toggleBookmark(); bible->toggleHighlight(); bible->setNoteText("Cross-book deletion");
        QVERIFY(bible->navigate("Luke 1:3-7"));
        QVERIFY(bible->deleteAnnotation("Obadiah 1:21–Jonah 1:2"));
        QCOMPARE(bible->selectionReference(),selection); QCOMPARE(bible->collection(),original);
    }
    void noteSaveRejectsStaleBaseline_data() {
        QTest::addColumn<QString>("original");
        QTest::addColumn<QString>("replacement");
        QTest::newRow("changed-note") << QString("Original") << QString("Peer revision");
        QTest::newRow("deleted-note") << QString("Original") << QString("");
        QTest::newRow("created-note") << QString("") << QString("Peer revision");
    }
    void noteSaveRejectsStaleBaseline() {
        if (!bible->ready()) QSKIP("Requires real EPUB");
        QFETCH(QString,original); QFETCH(QString,replacement);
        QVERIFY(bible->navigate("John 3:16")); bible->setNoteText(original);
        QCOMPARE(bible->noteText(),original);
        BibleBackend peer;
        QVERIFY(peer.navigate("John 3:16"));
        {
            // Simulate the notification delay used by the shared draft flush.
            QSignalBlocker deferred(&peer);
            peer.setNoteText(replacement);
        }
        QCOMPARE(peer.noteText(),replacement); QCOMPARE(bible->noteText(),original);
        const auto before = bible->collection();
        QSignalSpy commits(bible.get(),&BibleBackend::sharedCommitted);
        bible->setNoteText("Local draft");
        QVERIFY(bible->error().contains("changed in another window"));
        QCOMPARE(commits.count(),0); QCOMPARE(bible->noteText(),original);
        QCOMPARE(bible->collection(),before);
        peer.refreshCollection(); QCOMPARE(peer.noteText(),replacement);
        // Refresh plus explicit retry is allowed; annotation flags still merge.
        bible->refreshCollection(); QCOMPARE(bible->noteText(),replacement);
        peer.toggleHighlight(); const bool highlighted = peer.selectionHighlighted();
        bible->setNoteText("Local draft");
        QVERIFY2(bible->error().isEmpty(),qPrintable(bible->error()));
        QCOMPARE(commits.count(),1); QCOMPARE(peer.noteText(),QString("Local draft"));
        QCOMPARE(bible->selectionHighlighted(),highlighted);
    }
    void deletionExpectedVersionGuard() {
        if (!bible->ready()) QSKIP("Requires real EPUB");
        QVERIFY(bible->navigate("John 3:16")); bible->setNoteText("Keep the active edition's note");
        const auto before = bible->collection();
        QSignalSpy commits(bible.get(),&BibleBackend::sharedCommitted);
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","delete-version-guard");
            db.setDatabaseName(bible->dataDirectory()+"/personal.sqlite"); QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TRIGGER reject_version_delete BEFORE DELETE ON annotations BEGIN SELECT RAISE(ABORT,'unexpected delete'); END"));
            QVERIFY(!bible->deleteAnnotation("John 3:16","another-edition"));
            QVERIFY(bible->error().contains("version changed"));
            QCOMPARE(bible->collection(),before); QCOMPARE(commits.count(),0);
            QVERIFY(!bible->deleteAnnotation("invalid reference","another-edition"));
            QVERIFY(bible->error().contains("version changed"));
            QVERIFY(q.exec("SELECT note FROM annotations WHERE book=43 AND chapter=3 AND start=16 AND end=16"));
            QVERIFY(q.next()); QCOMPARE(q.value(0).toString(),QString("Keep the active edition's note")); q.finish();
            QVERIFY(q.exec("DROP TRIGGER reject_version_delete"));
        }
        QSqlDatabase::removeDatabase("delete-version-guard");
        QVERIFY(bible->deleteAnnotation("John 3:16",bible->versionId()));
        QCOMPARE(commits.count(),1); QVERIFY(bible->noteText().isEmpty());
        bible->setNoteText("One-argument callers still work");
        QVERIFY(bible->deleteAnnotation("John 3:16")); QVERIFY(bible->noteText().isEmpty());
    }
    void deletionRollbackErrors() {
        if (!bible->ready()) QSKIP("Requires real EPUB");
        QVERIFY(bible->navigate("Luke 3:2-4")); bible->toggleBookmark(); bible->toggleHighlight(); bible->setNoteText("Survive deletion errors");
        const auto before = bible->collection(), verses = bible->verses();
        const auto selection = bible->selectionReference(), note = bible->noteText();
        bible->setReadingPosition(0.57);
        QSignalSpy changes(bible.get(),&BibleBackend::collectionChanged);
        QSignalSpy navigation(bible.get(),&BibleBackend::navigationChanged);
        const QString path = bible->dataDirectory()+"/personal.sqlite";
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","delete-failure"); db.setDatabaseName(path); QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TRIGGER reject_deletion BEFORE DELETE ON annotations BEGIN SELECT RAISE(ABORT,'delete disk failure'); END"));
            QVERIFY(!bible->deleteAnnotation(selection)); QVERIFY(bible->error().contains("delete disk failure"));
            QCOMPARE(bible->collection(),before); QCOMPARE(bible->verses(),verses);
            QVERIFY(q.exec("DROP TRIGGER reject_deletion"));
            // A late readback error must restore the deleted row and the trigger's changes.
            QVERIFY(q.exec("CREATE TRIGGER fail_delete_readback AFTER DELETE ON annotations BEGIN UPDATE annotations SET endBook=66,endChapter=150; END"));
            QVERIFY(!bible->deleteAnnotation(selection)); QVERIFY2(bible->error().contains("Cannot read your annotations"),qPrintable(bible->error()));
            QCOMPARE(bible->collection(),before); QCOMPARE(bible->verses(),verses);
            QVERIFY(q.exec("DROP TRIGGER fail_delete_readback"));
            for (const auto &name : QSqlDatabase::connectionNames()) {
                auto connection = QSqlDatabase::database(name);
                if (connection.databaseName() == path) { QSqlQuery pragma(connection); QVERIFY(pragma.exec("PRAGMA foreign_keys=ON")); }
            }
            QVERIFY(q.exec("CREATE TABLE delete_parent(id INTEGER PRIMARY KEY)"));
            QVERIFY(q.exec("CREATE TABLE delete_failure(id INTEGER REFERENCES delete_parent(id) DEFERRABLE INITIALLY DEFERRED)"));
            QVERIFY(q.exec("CREATE TRIGGER fail_delete_commit AFTER DELETE ON annotations BEGIN INSERT INTO delete_failure VALUES(1); END"));
            QVERIFY(!bible->deleteAnnotation(selection)); QVERIFY2(bible->error().contains("Cannot commit annotation deletion"),qPrintable(bible->error()));
            QCOMPARE(bible->collection(),before); QCOMPARE(bible->verses(),verses);
            QVERIFY(q.exec("SELECT COUNT(*) FROM delete_failure")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),0); q.finish();
            QVERIFY(q.exec("DROP TRIGGER fail_delete_commit"));
            QVERIFY(q.exec("SELECT bookmark,highlight,note FROM annotations WHERE book=42 AND chapter=3 AND start=2 AND end=4"));
            QVERIFY(q.next()); QVERIFY(q.value(0).toBool()); QVERIFY(q.value(1).toBool()); QCOMPARE(q.value(2).toString(),note); q.finish();
        }
        QSqlDatabase::removeDatabase("delete-failure");
        QCOMPARE(changes.count(),0); QCOMPARE(navigation.count(),0);
        QCOMPARE(bible->selectionReference(),selection); QCOMPARE(bible->noteText(),note); QCOMPARE(bible->readingPosition(),0.57);
        bible.reset(); bible = std::make_unique<BibleBackend>();
        QVERIFY(bible->ready()); QCOMPARE(bible->collection(),before); QCOMPARE(bible->verses(),verses);
        QCOMPARE(bible->noteText(),note); QCOMPARE(bible->selectionReference(),selection); QCOMPARE(bible->readingPosition(),0.57);
        QVERIFY(bible->deleteAnnotation(selection)); QVERIFY(bible->noteText().isEmpty());
        QVERIFY(!bible->selectionBookmarked()); QVERIFY(!bible->selectionHighlighted());
    }
    void corruptLegacyRecovery_data() {
        QTest::addColumn<int>("schema");
        QTest::newRow("v1") << 1;
        QTest::newRow("v2") << 2;
    }
    void corruptLegacyRecovery() {
        if (epub.isEmpty()) QSKIP("Requires OMAWORD_TEST_EPUB.");
        QFETCH(int,schema);
        const QString application = QCoreApplication::applicationName();
        const auto restoreName = qScopeGuard([application] { QCoreApplication::setApplicationName(application); });
        QCoreApplication::setApplicationName(QString("corrupt-legacy-%1").arg(schema));
        const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QVERIFY(QDir().mkpath(directory));
        QVERIFY(write(directory+"/bible.sqlite","malformed legacy SQLite text"));
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","corrupt-legacy"); db.setDatabaseName(directory+"/personal.sqlite"); QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TABLE annotations(book INTEGER,chapter INTEGER,start INTEGER,end INTEGER,bookmark INTEGER,highlight INTEGER,note TEXT,PRIMARY KEY(book,chapter,start,end)) WITHOUT ROWID"));
            QVERIFY(q.exec("INSERT INTO annotations VALUES(43,3,16,18,1,1,'Preserved legacy NET note')"));
            QVERIFY(q.exec("CREATE TABLE reading(id INTEGER PRIMARY KEY,book INTEGER,chapter INTEGER,start INTEGER,end INTEGER,position REAL)"));
            QVERIFY(q.exec("INSERT INTO reading VALUES(1,43,3,16,18,0.63)"));
            QVERIFY(q.exec("CREATE TABLE preferences(key TEXT PRIMARY KEY,value NOT NULL) WITHOUT ROWID"));
            QVERIFY(q.exec("INSERT INTO preferences VALUES('lineHeight',2.3)"));
            if (schema == 2) {
                QVERIFY(q.exec("ALTER TABLE annotations ADD COLUMN endBook INTEGER NOT NULL DEFAULT 43"));
                QVERIFY(q.exec("ALTER TABLE annotations ADD COLUMN endChapter INTEGER NOT NULL DEFAULT 3"));
                for (const QString &sql : {QString("anchorBook INTEGER NOT NULL DEFAULT 43"),QString("anchorChapter INTEGER NOT NULL DEFAULT 3"),
                    QString("cursorBook INTEGER NOT NULL DEFAULT 43"),QString("cursorChapter INTEGER NOT NULL DEFAULT 3")})
                    QVERIFY(q.exec("ALTER TABLE reading ADD COLUMN "+sql));
            }
            QVERIFY(q.exec(QString("PRAGMA user_version=%1").arg(schema)));
        }
        QSqlDatabase::removeDatabase("corrupt-legacy");
        auto backend = std::make_unique<BibleBackend>();
        QVERIFY(!backend->ready()); QVERIFY(!backend->error().isEmpty()); QVERIFY(backend->versions().isEmpty());
        QCOMPARE(backend->lineHeight(),2.3);
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","corrupt-legacy"); db.setDatabaseName(directory+"/personal.sqlite"); QVERIFY(db.open()); QSqlQuery q(db);
            QVERIFY(q.exec("PRAGMA user_version")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),4); q.finish();
            QVERIFY(q.exec("SELECT version_id,note FROM annotations")); QVERIFY(q.next());
            QCOMPARE(q.value(0).toString(),QString("net-2.1")); QCOMPARE(q.value(1).toString(),QString("Preserved legacy NET note")); q.finish();
            QVERIFY(q.exec("SELECT COUNT(*) FROM installed")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),0);
        }
        QSqlDatabase::removeDatabase("corrupt-legacy");
        QSignalSpy busy(backend.get(),&BibleBackend::busyChanged);
        backend->importEpub(QUrl::fromLocalFile(epub)); QVERIFY(backend->busy());
        QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(),240000);
        QCOMPARE(busy.count(),2); QVERIFY2(backend->ready(),qPrintable(backend->error())); QVERIFY(backend->error().isEmpty());
        QVERIFY(backend->navigate("John 3:16-18")); QCOMPARE(backend->noteText(),QString("Preserved legacy NET note"));
        QVERIFY(backend->selectionBookmarked()); QVERIFY(backend->selectionHighlighted()); QCOMPARE(backend->lineHeight(),2.3);
        backend.reset(); backend = std::make_unique<BibleBackend>();
        QVERIFY(backend->ready()); QCOMPARE(backend->noteText(),QString("Preserved legacy NET note"));
    }
    void unsafeRemovalTargets() {
        const QString kjv = qEnvironmentVariable("OMAWORD_TEST_KJV");
        if (!bible->ready() || kjv.isEmpty()) QSKIP("Requires both EPUB fixtures.");
        const QString application = QCoreApplication::applicationName();
        const auto restoreName = qScopeGuard([application] { QCoreApplication::setApplicationName(application); });
        QCoreApplication::setApplicationName("unsafe-removal-test");
        BibleBackend backend;
        backend.importEpub(QUrl::fromLocalFile(epub)); QTRY_VERIFY_WITH_TIMEOUT(!backend.busy(),240000);
        QVERIFY(backend.ready()); QVERIFY(backend.navigate("John 3:16")); backend.setNoteText("NET stays");
        const QString netPath = biblePath(&backend);
        backend.importEpub(QUrl::fromLocalFile(kjv)); QTRY_VERIFY_WITH_TIMEOUT(!backend.busy(),240000);
        QCOMPARE(backend.versionId(),QString("kjv-2006")); backend.setNoteText("KJV stays");
        const QString kjvPath = biblePath(&backend);
        QVERIFY(backend.switchVersion("net-2.1"));
        const QString directory = backend.dataDirectory(), personal = directory+"/personal.sqlite";
        const QString alias = directory+"/bible-12345678-1234-1234-1234-123456789abc.sqlite";
        const auto versions = backend.versions(), annotations = backend.collection(), verses = backend.verses();
        const auto selection = backend.selectionReference();
        QSignalSpy changes(&backend,&BibleBackend::versionsChanged);
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","unsafe-removal"); db.setDatabaseName(personal); QVERIFY(db.open()); QSqlQuery q(db);
            const QStringList cases{"personal","wal","legacy-wrong-id","non-generation","symlink","hardlink","text-symlink","text-hardlink","wrong-edition-copy","sidecar-symlink","sidecar-hardlink"};
            for (const auto &kind : cases) {
                QString target = alias;
                if (kind == "personal") target = personal;
                else if (kind == "wal") target = personal+"-wal";
                else if (kind == "legacy-wrong-id") target = directory+"/bible.sqlite";
                else if (kind == "non-generation") target = directory+"/bible-random.sqlite";
                else if (kind == "symlink" || kind == "text-symlink") QVERIFY(QFile::link(kind == "symlink" ? personal : netPath,alias));
                else if (kind == "hardlink" || kind == "text-hardlink")
                    QCOMPARE(::link(QFile::encodeName(kind == "hardlink" ? personal : netPath).constData(),QFile::encodeName(alias).constData()),0);
                else if (kind == "wrong-edition-copy") QVERIFY(QFile::copy(netPath,alias));
                else {
                    QVERIFY(QFile::copy(kjvPath,alias));
                    if (kind == "sidecar-symlink") QVERIFY(QFile::link(personal+"-wal",alias+"-wal"));
                    else QCOMPARE(::link(QFile::encodeName(personal+"-wal").constData(),QFile::encodeName(alias+"-wal").constData()),0);
                }
                q.prepare("UPDATE installed SET path=? WHERE version_id='kjv-2006'"); q.addBindValue(QFileInfo(target).fileName()); QVERIFY(q.exec());
                // Capture after intentional registry corruption, before either
                // removal. SQL reads may update SHM reader marks, but must not
                // change the database/WAL or unlink any live sidecar.
                QMap<QString,QByteArray> bytes;
                for (const auto &path : {personal,personal+"-wal",netPath,kjvPath}) {
                    QFile f(path); QVERIFY(f.open(QIODevice::ReadOnly)); bytes.insert(path,f.readAll());
                }
                for (bool deleteNotes : {false,true}) {
                    QVERIFY2(!backend.removeVersion("kjv-2006",deleteNotes),qPrintable(kind));
                    QVERIFY(!backend.error().isEmpty()); QCOMPARE(backend.versions(),versions); QCOMPARE(backend.collection(),annotations);
                    QVERIFY(QFileInfo::exists(personal+"-shm"));
                    QCOMPARE(backend.verses(),verses); QCOMPARE(backend.selectionReference(),selection); QCOMPARE(backend.noteText(),QString("NET stays"));
                    for (auto it = bytes.cbegin(); it != bytes.cend(); ++it) {
                        QFile f(it.key()); QVERIFY(f.open(QIODevice::ReadOnly)); QCOMPARE(f.readAll(),it.value());
                    }
                }
                // Reimport must also reject unsafe cleanup of an old generation.
                if (kind == "personal") {
                    backend.importEpub(QUrl::fromLocalFile(kjv)); QTRY_VERIFY_WITH_TIMEOUT(!backend.busy(),240000);
                    QVERIFY(backend.error().contains("Unsafe Bible removal target")); QCOMPARE(backend.versions(),versions);
                    QCOMPARE(backend.noteText(),QString("NET stays")); QVERIFY(QFileInfo::exists(personal));
                }
                q.prepare("UPDATE installed SET path=? WHERE version_id='kjv-2006'"); q.addBindValue(QFileInfo(kjvPath).fileName()); QVERIFY(q.exec());
                if (QFileInfo(alias+"-wal").exists() || QFileInfo(alias+"-wal").isSymLink()) QVERIFY(QFile::remove(alias+"-wal"));
                if (QFileInfo(alias).exists() || QFileInfo(alias).isSymLink()) QVERIFY(QFile::remove(alias));
            }
            QVERIFY(q.exec("SELECT note FROM annotations WHERE version_id='kjv-2006'")); QVERIFY(q.next()); QCOMPARE(q.value(0).toString(),QString("KJV stays"));
        }
        QSqlDatabase::removeDatabase("unsafe-removal");
        QCOMPARE(changes.count(),0); QVERIFY(backend.switchVersion("kjv-2006")); QCOMPARE(backend.noteText(),QString("KJV stays"));
        QVERIFY(backend.removeVersion("kjv-2006",true)); QVERIFY(backend.ready()); QVERIFY(QFileInfo::exists(personal));
    }
    void multiVersionLibrary() {
        const QString kjv = qEnvironmentVariable("OMAWORD_TEST_KJV");
        if (!bible->ready() || kjv.isEmpty()) QSKIP("Requires OMAWORD_TEST_EPUB and OMAWORD_TEST_KJV.");
        const QString application = QCoreApplication::applicationName();
        const auto restoreName = qScopeGuard([application] { QCoreApplication::setApplicationName(application); });
        QCoreApplication::setApplicationName("multi-version-test");
        auto library = std::make_unique<BibleBackend>();
        library->setLineHeight(2.2); library->setFocusMode(true);
        const QString directory = library->dataDirectory();
        library.reset();
        // Exercise a schema-2 install with the old root text path and exact
        // cross-chapter reference endpoints, plus already-existing global prefs.
        QVERIFY(QFile::copy(biblePath(bible.get()),directory+"/bible.sqlite"));
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","legacy-metadata"); db.setDatabaseName(directory+"/bible.sqlite"); QVERIFY(db.open());
            QSqlQuery q(db);
            // The UI's synthetic fixture and original installs identify NET by
            // edition alone; new metadata fields must remain optional for them.
            QVERIFY(q.exec("DELETE FROM metadata WHERE key IN ('version_id','abbreviation','copyright')"));
        }
        QSqlDatabase::removeDatabase("legacy-metadata");
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","library-fixture"); db.setDatabaseName(directory+"/personal.sqlite"); QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("DROP TABLE annotations")); QVERIFY(q.exec("DROP TABLE reading"));
            QVERIFY(q.exec("DROP TABLE installed")); QVERIFY(q.exec("DROP TABLE library"));
            QVERIFY(q.exec("CREATE TABLE annotations(book INTEGER,chapter INTEGER,start INTEGER,end INTEGER,bookmark INTEGER,highlight INTEGER,note TEXT,endBook INTEGER,endChapter INTEGER,PRIMARY KEY(book,chapter,start,endBook,endChapter,end)) WITHOUT ROWID"));
            QVERIFY(q.exec("INSERT INTO annotations VALUES(43,3,35,2,1,1,'NET-only legacy range',43,4)"));
            QVERIFY(q.exec("CREATE TABLE reading(id INTEGER PRIMARY KEY,book INTEGER,chapter INTEGER,start INTEGER,end INTEGER,position REAL,anchorBook INTEGER,anchorChapter INTEGER,cursorBook INTEGER,cursorChapter INTEGER)"));
            QVERIFY(q.exec("INSERT INTO reading VALUES(1,43,4,35,2,0.4,43,3,43,4)"));
            QVERIFY(q.exec("PRAGMA user_version=2"));
            QVERIFY(q.exec("CREATE TABLE installed(conflict INTEGER)"));
        }
        QSqlDatabase::removeDatabase("library-fixture");
        { BibleBackend failed; QVERIFY(!failed.ready()); QVERIFY(!failed.error().isEmpty()); }
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","library-fixture"); db.setDatabaseName(directory+"/personal.sqlite"); QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("PRAGMA user_version")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),2); q.finish();
            QVERIFY(!q.exec("SELECT version_id FROM annotations"));
            QVERIFY(q.exec("SELECT note FROM annotations")); QVERIFY(q.next()); QCOMPARE(q.value(0).toString(),QString("NET-only legacy range")); q.finish();
            QVERIFY(q.exec("SELECT position FROM reading")); QVERIFY(q.next()); QCOMPARE(q.value(0).toDouble(),0.4); q.finish();
            QVERIFY(q.exec("DROP TABLE installed")); QVERIFY(QFileInfo::exists(directory+"/bible.sqlite"));
        }
        QSqlDatabase::removeDatabase("library-fixture");
        library = std::make_unique<BibleBackend>();
        QVERIFY2(library->ready(),qPrintable(library->error()));
        QCOMPARE(library->versionId(),QString("net-2.1")); QCOMPARE(library->versionAbbreviation(),QString("NET"));
        QCOMPARE(library->edition(),QString("NET Bible 2.1"));
        QCOMPARE(library->versions().size(),1); QCOMPARE(library->versions().first().toMap().size(),4);
        QVERIFY(library->versions().first().toMap()["active"].toBool());
        QCOMPARE(library->selectionReference(),QString("John 3:35–4:2"));
        QCOMPARE(library->noteText(),QString("NET-only legacy range")); QCOMPARE(library->readingPosition(),0.4);
        QCOMPARE(library->lineHeight(),2.2); QVERIFY(library->focusMode());
        for (const QString property : {"edition","versionId","versionAbbreviation","versionCopyright","versions"}) {
            const auto p = library->metaObject()->property(library->metaObject()->indexOfProperty(qPrintable(property)));
            QVERIFY(p.isReadable()); QVERIFY(!p.isConstant());
            QCOMPARE(p.notifySignal().name(),property == "versions" ? QByteArray("versionsChanged") : QByteArray("versionChanged"));
        }
        QSignalSpy versions(library.get(),&BibleBackend::versionsChanged);
        library->importEpub(QUrl::fromLocalFile(kjv)); QVERIFY(library->busy());
        QVERIFY(!library->switchVersion("net-2.1")); QVERIFY(!library->removeVersion("net-2.1",true));
        QTRY_VERIFY_WITH_TIMEOUT(!library->busy(),240000);
        QVERIFY2(library->error().isEmpty(),qPrintable(library->error()));
        QCOMPARE(library->versionId(),QString("kjv-2006")); QCOMPARE(library->versionAbbreviation(),QString("KJV"));
        QVERIFY(!library->versionCopyright().isEmpty()); QCOMPARE(library->versions().size(),2); QCOMPARE(versions.count(),1);
        QCOMPARE(library->selectionReference(),QString("John 3:35–4:2"));
        QVERIFY(library->collection().isEmpty()); QVERIFY(library->noteText().isEmpty());
        QVERIFY(!library->selectionBookmarked()); QVERIFY(!library->selectionHighlighted());
        library->setNoteText("KJV-only range"); library->toggleBookmark(); library->toggleHighlight();
        const auto kjvCollection = library->collection();
        int count = 0;
        for (int b = 1; b <= 66; ++b)
            for (int c = 1; c <= EpubImporter::canon()[b-1].chapters; ++c) count += library->chapterVerses(b,c).size();
        QCOMPARE(count,31102);
        QVERIFY(library->navigate("Mark 9:43")); library->moveSelection(1);
        QCOMPARE(library->selectionReference(),QString("Mark 9:44"));
        QVERIFY(library->navigate("Mark 9:43-47"));
        const QString copied = library->copySelection(); QVERIFY(copied.contains("⁴⁴ ")); QVERIFY(copied.contains("⁴⁶ "));
        library->toggleHighlight(); QVERIFY(library->navigate("Mark 9:44")); library->toggleHighlight();
        QVERIFY(!library->selectionHighlighted()); QVERIFY(library->navigate("Mark 9:45")); QVERIFY(library->selectionHighlighted());
        QVERIFY(library->navigate("Matthew 17:21")); library->setNoteText("KJV verse absent in NET");
        QVERIFY(library->switchVersion("net-2.1")); QCOMPARE(library->passage(),QString("Matthew 17"));
        QCOMPARE(library->cursorVerse(),0); QVERIFY(library->status().contains("endpoint is absent"));
        QCOMPARE(library->collection().size(),1); QVERIFY(library->noteText().isEmpty());
        QVERIFY(!library->navigate("Matthew 17:21"));
        QVERIFY(library->navigate("John 3:35-4:2")); QCOMPARE(library->noteText(),QString("NET-only legacy range"));
        QVERIFY(library->exportBackup(QUrl::fromLocalFile(file("net-scoped.json"))));
        QFile netBackup(file("net-scoped.json")); QVERIFY(netBackup.open(QIODevice::ReadOnly));
        const auto netObject = QJsonDocument::fromJson(netBackup.readAll()).object();
        QCOMPARE(netObject["version"].toInt(),3); QCOMPARE(netObject["versionId"].toString(),QString("net-2.1"));
        auto legacy = netObject; legacy["version"] = 2; legacy.remove("versionId");
        QVERIFY(write(file("legacy-net-v2.json"),QJsonDocument(legacy).toJson()));
        QVERIFY(library->importBackup(QUrl::fromLocalFile(file("legacy-net-v2.json"))));
        QVERIFY(library->navigate("3 John 1:15"));
        QVERIFY(library->switchVersion("kjv-2006")); QCOMPARE(library->selectionReference(),QString("3 John 1"));
        QVERIFY(library->status().contains("endpoint is absent"));
        QVERIFY(!library->navigate("3 John 1:15"));
        QVERIFY(library->navigate("John 3:35-4:2")); QCOMPARE(library->noteText(),QString("KJV-only range"));
        QVERIFY(!library->importBackup(QUrl::fromLocalFile(file("net-scoped.json"))));
        QVERIFY(!library->importBackup(QUrl::fromLocalFile(file("legacy-net-v2.json"))));
        auto legacyV1 = legacy; legacyV1["version"] = 1;
        legacyV1["reading"] = QJsonObject{{"book",43},{"chapter",3},{"start",16},{"end",18},{"position",0.2}};
        legacyV1["annotations"] = QJsonArray{QJsonObject{{"book",43},{"chapter",3},{"start",16},{"end",18},
            {"bookmark",true},{"highlight",false},{"note","Old NET backup"}}};
        QVERIFY(write(file("legacy-net-v1-scoping.json"),QJsonDocument(legacyV1).toJson()));
        QVERIFY(!library->importBackup(QUrl::fromLocalFile(file("legacy-net-v1-scoping.json"))));
        QVERIFY(library->error().contains("different Bible version"));
        // Changing only the display name must not defeat stable-ID scoping.
        auto disguised = netObject; disguised["edition"] = library->edition();
        QVERIFY(write(file("wrong-id.json"),QJsonDocument(disguised).toJson()));
        QVERIFY(!library->importBackup(QUrl::fromLocalFile(file("wrong-id.json"))));
        QCOMPARE(library->noteText(),QString("KJV-only range"));
        QVERIFY(library->navigate("Matthew 17:21"));
        QVERIFY(library->exportBackup(QUrl::fromLocalFile(file("kjv-scoped.json"))));
        QVERIFY(library->exportNotes(QUrl::fromLocalFile(root.path())));
        QFile markdown(file("omaword-notes.md")); QVERIFY(markdown.open(QIODevice::ReadOnly));
        const auto exported = markdown.readAll(); QVERIFY(exported.contains("kjv-2006"));
        QVERIFY(exported.contains("KJV-only range")); QVERIFY(!exported.contains("NET-only legacy range"));
        // All installed text generations, root DBs, dangling sidecars and aliases.
        const QString kjvPath = biblePath(library.get());
        QVERIFY(library->switchVersion("net-2.1")); const QString netPath = biblePath(library.get());
        QVERIFY(library->switchVersion("kjv-2006"));
        QTemporaryDir aliases;
        for (const QString &database : {kjvPath,netPath,directory+"/personal.sqlite"}) {
            for (const QString &suffix : {QString(""),QString("-wal"),QString("-shm"),QString("-journal")}) {
                const QString target = database+suffix;
                QVERIFY(!library->exportBackup(QUrl::fromLocalFile(target)));
                const QString alias = aliases.filePath("omaword-notes.md");
                QVERIFY(QFile::link(target,alias));
                QVERIFY(!library->exportBackup(QUrl::fromLocalFile(alias)));
                QVERIFY(!library->exportNotes(QUrl::fromLocalFile(aliases.path())));
                QVERIFY(QFile::remove(alias));
            }
        }
        QVERIFY(QFile::link(directory,aliases.filePath("data")));
        QVERIFY(!library->exportBackup(QUrl::fromLocalFile(aliases.filePath("data/")+QFileInfo(kjvPath).fileName()+"-journal")));
        library.reset(); library = std::make_unique<BibleBackend>();
        QCOMPARE(library->versionId(),QString("kjv-2006")); QVERIFY(library->navigate("Matthew 17:21"));
        QCOMPARE(library->noteText(),QString("KJV verse absent in NET"));
        // A second backend has a separate key vector; switching it cannot change
        // canonical offsets or annotations in this instance.
        { BibleBackend other; QVERIFY(other.switchVersion("net-2.1"));
          QVERIFY(library->navigate("Mark 9:44")); QCOMPARE(library->cursorVerse(),44); }
        QVERIFY(library->navigate("John 3:35-4:2"));
        QVERIFY(library->removeVersion("kjv-2006",false)); QCOMPARE(library->versionId(),QString("net-2.1"));
        QCOMPARE(library->noteText(),QString("NET-only legacy range")); QCOMPARE(library->versions().size(),1);
        QVERIFY(!QFileInfo::exists(kjvPath)); QVERIFY(!library->switchVersion("kjv-2006"));
        library.reset(); library = std::make_unique<BibleBackend>();
        QCOMPARE(library->versions().size(),1); QCOMPARE(library->collection().size(),1);
        library->importEpub(QUrl::fromLocalFile(kjv)); QTRY_VERIFY_WITH_TIMEOUT(!library->busy(),240000);
        QCOMPARE(library->versionId(),QString("kjv-2006")); QCOMPARE(library->noteText(),QString("KJV-only range"));
        QVERIFY(library->collection().size() > kjvCollection.size());
        QVERIFY(library->importBackup(QUrl::fromLocalFile(file("kjv-scoped.json"))));
        QCOMPARE(library->noteText(),QString("KJV verse absent in NET"));
        // Removing an inactive version must preserve active range and annotations.
        const auto before = library->collection(); const auto selection = library->selectionReference();
        QVERIFY(library->removeVersion("net-2.1",false)); QCOMPARE(library->selectionReference(),selection);
        QCOMPARE(library->collection(),before); QCOMPARE(library->versions().size(),1);
        QVERIFY(library->removeVersion("kjv-2006",false)); QVERIFY(!library->ready());
        QVERIFY(library->versionId().isEmpty()); QVERIFY(library->edition().isEmpty()); QVERIFY(library->versionCopyright().isEmpty());
        QVERIFY(library->versions().isEmpty()); QVERIFY(library->verses().isEmpty()); QVERIFY(library->collection().isEmpty());
        QVERIFY(library->noteText().isEmpty()); QVERIFY(!library->selectionBookmarked());
        library->refreshCollection(); QVERIFY(library->collection().isEmpty());
        QVERIFY(!library->exportBackup(QUrl::fromLocalFile(file("empty.json"))));
        QVERIFY(!library->exportNotes(QUrl::fromLocalFile(root.path())));
        library->setLineHeight(2.4);
        library.reset(); library = std::make_unique<BibleBackend>();
        QVERIFY(!library->ready()); QVERIFY(library->versions().isEmpty()); QVERIFY(library->collection().isEmpty());
        QCOMPARE(library->lineHeight(),2.4); QVERIFY(library->focusMode());
        library->importEpub(QUrl::fromLocalFile(kjv)); QTRY_VERIFY_WITH_TIMEOUT(!library->busy(),240000);
        QVERIFY2(library->ready(),qPrintable(library->error())); QCOMPARE(library->collection(),before);
        QVERIFY(library->navigate("Matthew 17:21")); QCOMPARE(library->noteText(),QString("KJV verse absent in NET"));
        QVERIFY(library->removeVersion("kjv-2006",true)); QVERIFY(!library->ready());
        library->importEpub(QUrl::fromLocalFile(kjv)); QTRY_VERIFY_WITH_TIMEOUT(!library->busy(),240000);
        QVERIFY(library->ready()); QVERIFY(library->collection().isEmpty());
        library->importEpub(QUrl::fromLocalFile(epub)); QTRY_VERIFY_WITH_TIMEOUT(!library->busy(),240000);
        QCOMPARE(library->versionId(),QString("net-2.1")); QCOMPARE(library->collection().size(),1);
        QVERIFY(library->navigate("John 3:35-4:2")); QCOMPARE(library->noteText(),QString("NET-only legacy range"));
        QCOMPARE(library->lineHeight(),2.4);
    }
    void legacyParagraphsAndMru() {
        const QString kjv = qEnvironmentVariable("OMAWORD_TEST_KJV");
        if (!bible->ready() || kjv.isEmpty()) QSKIP("Requires both EPUB fixtures.");
        const QString application = QCoreApplication::applicationName();
        const auto restoreName = qScopeGuard([application] { QCoreApplication::setApplicationName(application); });
        QCoreApplication::setApplicationName("paragraph-mru-test");
        auto backend = std::make_unique<BibleBackend>();
        backend->importEpub(QUrl::fromLocalFile(epub)); QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(),240000);
        QVERIFY2(backend->ready(),qPrintable(backend->error()));
        const QString netPath = biblePath(backend.get());
        backend->importEpub(QUrl::fromLocalFile(kjv)); QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(),240000);
        QCOMPARE(backend->versionId(),QString("kjv-2006"));
        const QString kjvPath = biblePath(backend.get()), store = backend->dataDirectory()+"/personal.sqlite";
        QVERIFY(backend->navigate("Genesis 1:6"));
        QVERIFY(backend->hasParagraphs());
        QVERIFY(backend->verses()[5].toMap()["paragraphStart"].toBool());
        QVERIFY(!backend->verses()[6].toMap()["paragraphStart"].toBool());
        QVERIFY(!backend->copySelection().contains(QChar(0xb6)));
        const QString clean = backend->copySelection(true);
        backend->setFontSize(25); backend->setReadingMode("paragraph");
        backend.reset();
        // Recreate schema-1 storage without access to any original source file.
        // Legacy KJV keeps layout markers, including safe HTML wrappers; NET
        // lacks the paragraph boundary information and must fall back to verses.
        for (const auto &path : {netPath,kjvPath}) {
            {
                auto db = QSqlDatabase::addDatabase("QSQLITE","legacy-paragraphs"); db.setDatabaseName(path); QVERIFY(db.open());
                QSqlQuery q(db);
                if (path == kjvPath) {
                    QVERIFY(q.exec("UPDATE verses SET text='¶ '||text,html='¶ '||html WHERE paragraphStart=1"));
                    QVERIFY(q.exec("UPDATE verses SET html='<i>¶ </i>'||substr(html,3) WHERE book=1 AND chapter=1 AND number=6"));
                    QVERIFY(q.exec("UPDATE verses SET text=text||' Quoted ‘¶ literal’.',html=html||' Quoted ‘¶ literal’.' WHERE book=1 AND chapter=1 AND number=7"));
                }
                QVERIFY(q.exec("ALTER TABLE verses DROP COLUMN paragraphStart"));
                QVERIFY(q.exec("DELETE FROM metadata WHERE key='paragraph_layout'"));
                QVERIFY(q.exec("UPDATE metadata SET value='1' WHERE key='schema'"));
            }
            QSqlDatabase::removeDatabase("legacy-paragraphs");
        }
        QFile original(kjvPath); QVERIFY(original.open(QIODevice::ReadOnly)); const auto bytes = original.readAll(); original.close();
        backend = std::make_unique<BibleBackend>();
        QVERIFY2(backend->ready(),qPrintable(backend->error())); QVERIFY(backend->hasParagraphs());
        QCOMPARE(backend->copySelection(true),clean);
        QVERIFY(!backend->verses()[5].toMap()["html"].toString().contains(QChar(0xb6)));
        QVERIFY(backend->chapterVerses(1,1)[5].toMap()["paragraphStart"].toBool());
        QVERIFY(!backend->chapterVerses(1,1)[5].toMap()["text"].toString().contains(QChar(0xb6)));
        QVERIFY(backend->copyVerse(7,true).contains("‘¶ literal’"));
        QVERIFY(backend->navigate("Revelation 22")); QVERIFY(backend->hasParagraphs()); // edition-wide, not this chapter
        QCOMPARE(backend->versions().first().toMap()["id"].toString(),QString("kjv-2006"));
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","mru-check"); db.setDatabaseName(store); QVERIFY(db.open());
            QSqlQuery q(db);
            auto rank = [&]() -> qint64 {
                if (!q.exec("SELECT use_counter FROM library WHERE id=1") || !q.next()) return -1;
                const auto result = q.value(0).toLongLong(); q.finish(); return result;
            };
            qint64 previous = rank(); QVERIFY(previous >= 2);
            QVERIFY(backend->switchVersion("kjv-2006")); QCOMPARE(rank(),previous);
            QVERIFY(backend->switchVersion("net-2.1")); QVERIFY(!backend->hasParagraphs());
            QCOMPARE(backend->versions().first().toMap()["id"].toString(),QString("net-2.1"));
            QCOMPARE(backend->fontSize(),25); QCOMPARE(backend->readingMode(),QString("paragraph"));
            QVERIFY(rank() > previous); previous = rank();
            QVERIFY(backend->navigate("Genesis 1"));
            for (const auto &row : backend->verses()) QVERIFY(!row.toMap()["paragraphStart"].toBool());
            backend.reset(); backend = std::make_unique<BibleBackend>();
            QCOMPARE(rank(),previous); QCOMPARE(backend->versions().first().toMap()["id"].toString(),QString("net-2.1"));
            const auto versions = backend->versions();
            QVERIFY(q.exec("CREATE TRIGGER reject_rank AFTER UPDATE OF last_used ON installed BEGIN SELECT RAISE(ABORT,'rank failure'); END"));
            QVERIFY(!backend->switchVersion("kjv-2006")); QCOMPARE(rank(),previous);
            QCOMPARE(backend->versions(),versions); QVERIFY(!backend->hasParagraphs());
            QVERIFY(q.exec("DROP TRIGGER reject_rank"));
            QVERIFY(q.exec("CREATE TRIGGER reject_rank BEFORE UPDATE ON reading BEGIN SELECT RAISE(ABORT,'late failure'); END"));
            QVERIFY(!backend->switchVersion("kjv-2006")); QCOMPARE(rank(),previous);
            QCOMPARE(backend->versions(),versions); QVERIFY(!backend->hasParagraphs());
            QVERIFY(q.exec("DROP TRIGGER reject_rank"));
            QVERIFY(backend->switchVersion("kjv-2006")); QVERIFY(backend->hasParagraphs());
            QVERIFY(original.open(QIODevice::ReadOnly)); QCOMPARE(original.readAll(),bytes); original.close();
            previous = rank();
            QVERIFY(backend->removeVersion("kjv-2006",false)); QVERIFY(!backend->hasParagraphs());
            QVERIFY(rank() > previous); previous = rank();
            backend->importEpub(QUrl::fromLocalFile(kjv)); QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(),240000);
            QVERIFY2(backend->error().isEmpty(),qPrintable(backend->error())); QVERIFY(rank() > previous);
            QVERIFY(q.exec("SELECT last_used FROM installed WHERE version_id='kjv-2006'")); QVERIFY(q.next());
            const auto latest = q.value(0).toLongLong(); q.finish(); QCOMPARE(latest,rank());
            QCOMPARE(backend->versions().first().toMap()["id"].toString(),QString("kjv-2006"));
        }
        QSqlDatabase::removeDatabase("mru-check");
    }
    void versionOperationRollback() {
        const QString kjv = qEnvironmentVariable("OMAWORD_TEST_KJV");
        if (!bible->ready() || kjv.isEmpty()) QSKIP("Requires both EPUB fixtures.");
        const QString application = QCoreApplication::applicationName();
        const auto restoreName = qScopeGuard([application] { QCoreApplication::setApplicationName(application); });
        QCoreApplication::setApplicationName("version-errors-test");
        auto library = std::make_unique<BibleBackend>();
        library->importEpub(QUrl::fromLocalFile(epub)); QTRY_VERIFY_WITH_TIMEOUT(!library->busy(),240000);
        QVERIFY2(library->ready(),qPrintable(library->error()));
        QVERIFY(library->navigate("John 3:16-18")); library->setNoteText("Never lose this");
        const QString netPath = biblePath(library.get());
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","new-install-failure"); db.setDatabaseName(library->dataDirectory()+"/personal.sqlite"); QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TRIGGER fail_install BEFORE INSERT ON installed BEGIN SELECT RAISE(ABORT,'new install failure'); END"));
            library->importEpub(QUrl::fromLocalFile(kjv)); QTRY_VERIFY_WITH_TIMEOUT(!library->busy(),240000);
            QVERIFY(library->error().contains("new install failure")); QCOMPARE(library->versionId(),QString("net-2.1"));
            QCOMPARE(library->versions().size(),1); QCOMPARE(library->noteText(),QString("Never lose this"));
            QVERIFY(q.exec("DROP TRIGGER fail_install"));
        }
        QSqlDatabase::removeDatabase("new-install-failure");
        library->importEpub(QUrl::fromLocalFile(kjv)); QTRY_VERIFY_WITH_TIMEOUT(!library->busy(),240000);
        QCOMPARE(library->versionId(),QString("kjv-2006")); library->setNoteText("KJV survives too");
        const QString kjvPath = biblePath(library.get());
        QVERIFY(library->switchVersion("net-2.1"));
        const auto models = library->versions(), collection = library->collection(), verses = library->verses();
        const auto selection = library->selectionReference(), note = library->noteText();
        const QString store = library->dataDirectory()+"/personal.sqlite";
        QSignalSpy changes(library.get(),&BibleBackend::versionChanged);
        QSignalSpy modelChanges(library.get(),&BibleBackend::versionsChanged);
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","version-failure"); db.setDatabaseName(store); QVERIFY(db.open()); QSqlQuery q(db);
            const QStringList triggers{
                "CREATE TRIGGER fail_library BEFORE UPDATE ON library BEGIN SELECT RAISE(ABORT,'library write failure'); END",
                "CREATE TRIGGER fail_library AFTER UPDATE ON library BEGIN UPDATE library SET active='net-2.1'; END",
                "CREATE TRIGGER fail_library BEFORE UPDATE ON reading BEGIN SELECT RAISE(ABORT,'reading write failure'); END"};
            for (const QString &trigger : triggers) {
                QVERIFY(q.exec(trigger));
                QVERIFY(!library->switchVersion("kjv-2006")); QVERIFY(!library->error().isEmpty());
                QVERIFY(!library->removeVersion("net-2.1",true));
                QCOMPARE(library->versions(),models); QCOMPARE(library->collection(),collection); QCOMPARE(library->verses(),verses);
                QCOMPARE(library->selectionReference(),selection); QCOMPARE(library->noteText(),note);
                QCOMPARE(library->versionId(),QString("net-2.1"));
                QVERIFY(QFileInfo::exists(netPath)); QVERIFY(QFileInfo::exists(kjvPath));
                QVERIFY(q.exec("DROP TRIGGER fail_library"));
            }
            QVERIFY(q.exec("CREATE TRIGGER fail_library BEFORE DELETE ON annotations BEGIN SELECT RAISE(ABORT,'annotation delete failure'); END"));
            QVERIFY(!library->removeVersion("net-2.1",true)); QCOMPARE(library->collection(),collection);
            QVERIFY(q.exec("DROP TRIGGER fail_library"));
            QVERIFY(q.exec("CREATE TRIGGER fail_library BEFORE DELETE ON installed BEGIN SELECT RAISE(IGNORE); END"));
            QVERIFY(!library->removeVersion("kjv-2006",false)); QCOMPARE(library->versions(),models);
            QVERIFY(q.exec("DROP TRIGGER fail_library"));
            // An annotation read failure in the target must not publish its
            // connection/offsets or commit removal of the current edition.
            QVERIFY(q.exec("UPDATE annotations SET endChapter=150 WHERE version_id='kjv-2006'"));
            QVERIFY(!library->switchVersion("kjv-2006")); QVERIFY(library->error().contains("Cannot read your annotations"));
            QVERIFY(!library->removeVersion("net-2.1",true)); QCOMPARE(library->collection(),collection);
            QVERIFY(q.exec("UPDATE annotations SET endChapter=3 WHERE version_id='kjv-2006'"));
            for (const auto &name : QSqlDatabase::connectionNames()) {
                auto connection = QSqlDatabase::database(name);
                if (connection.databaseName() == store) { QSqlQuery pragma(connection); QVERIFY(pragma.exec("PRAGMA foreign_keys=ON")); }
            }
            QVERIFY(q.exec("CREATE TABLE library_parent(id INTEGER PRIMARY KEY)"));
            QVERIFY(q.exec("CREATE TABLE library_failure(id INTEGER REFERENCES library_parent(id) DEFERRABLE INITIALLY DEFERRED)"));
            QVERIFY(q.exec("CREATE TRIGGER fail_library AFTER UPDATE ON library BEGIN INSERT INTO library_failure VALUES(1); END"));
            QVERIFY(!library->switchVersion("kjv-2006")); QVERIFY(library->error().contains("Cannot commit library"));
            QVERIFY(!library->removeVersion("net-2.1",true));
            library->importEpub(QUrl::fromLocalFile(kjv)); QTRY_VERIFY_WITH_TIMEOUT(!library->busy(),240000);
            QVERIFY(library->error().contains("Cannot commit library"));
            QCOMPARE(library->versions(),models); QCOMPARE(library->collection(),collection); QCOMPARE(biblePath(library.get()),netPath);
            QVERIFY(QFileInfo::exists(kjvPath));
            QVERIFY(q.exec("SELECT COUNT(*) FROM library_failure")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),0); q.finish();
            QVERIFY(q.exec("DROP TRIGGER fail_library"));
            // Reject both replacing an installed generation and adding a new ID.
            QVERIFY(q.exec("CREATE TRIGGER fail_library BEFORE INSERT ON installed BEGIN SELECT RAISE(ABORT,'install failure'); END"));
            library->importEpub(QUrl::fromLocalFile(epub)); QTRY_VERIFY_WITH_TIMEOUT(!library->busy(),240000);
            QVERIFY(library->error().contains("install failure")); QCOMPARE(biblePath(library.get()),netPath);
            QCOMPARE(library->collection(),collection); QCOMPARE(library->selectionReference(),selection);
            QVERIFY(q.exec("DROP TRIGGER fail_library"));
        }
        QSqlDatabase::removeDatabase("version-failure");
        QVERIFY(!library->switchVersion("../../escape")); QVERIFY(!library->removeVersion("",true));
        // A missing target fails before touching active metadata or removing files.
        QVERIFY(QFile::rename(kjvPath,kjvPath+".held"));
        QVERIFY(!library->switchVersion("kjv-2006")); QVERIFY(!library->removeVersion("net-2.1",true));
        QVERIFY(QFile::rename(kjvPath+".held",kjvPath));
        QCOMPARE(changes.count(),0); QCOMPARE(modelChanges.count(),0);
        QCOMPARE(library->collection(),collection); QCOMPARE(library->verses(),verses);
        library.reset(); library = std::make_unique<BibleBackend>();
        QVERIFY2(library->ready(),qPrintable(library->error())); QCOMPARE(library->versionId(),QString("net-2.1"));
        QCOMPARE(library->collection(),collection); QCOMPARE(library->noteText(),note); QCOMPARE(library->selectionReference(),selection);
        QVERIFY(library->switchVersion("kjv-2006")); QCOMPARE(library->noteText(),QString("KJV survives too"));
        QVERIFY(library->removeVersion("net-2.1",false));
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","empty-failure"); db.setDatabaseName(store); QVERIFY(db.open()); QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TRIGGER fail_empty BEFORE UPDATE ON library BEGIN SELECT RAISE(ABORT,'empty library failure'); END"));
            const auto saved = library->collection(), installed = library->versions();
            QVERIFY(!library->removeVersion("kjv-2006",true)); QVERIFY(library->ready());
            QCOMPARE(library->versionId(),QString("kjv-2006")); QCOMPARE(library->collection(),saved); QCOMPARE(library->versions(),installed);
            QVERIFY(QFileInfo::exists(kjvPath)); QVERIFY(q.exec("DROP TRIGGER fail_empty"));
        }
        QSqlDatabase::removeDatabase("empty-failure");
    }
    void v1MigrationAndBackupCompatibility() {
        const QString application = QCoreApplication::applicationName();
        const auto restoreName = qScopeGuard([application] { QCoreApplication::setApplicationName(application); });
        QCoreApplication::setApplicationName("migration-test");
        const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QVERIFY(QDir().mkpath(directory));
        if (bible->ready()) QVERIFY(QFile::copy(biblePath(bible.get()),directory+"/bible.sqlite"));
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
            QSignalSpy busy(&failed,&BibleBackend::busyChanged);
            failed.importEpub(QUrl::fromLocalFile(epub)); QVERIFY(!failed.busy()); QCOMPARE(busy.count(),0);
            QVERIFY(failed.error().contains("notes database is unavailable"));
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
            QVERIFY(q.exec("PRAGMA user_version")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),4); q.finish();
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
        QJsonObject backup{{"format","omaword-backup"},{"version",1},{"edition","NET Bible 2.1"},
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
        const auto object = QJsonDocument::fromJson(converted.readAll()).object(); QCOMPARE(object["version"].toInt(),3);
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
