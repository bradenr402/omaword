#include "testenvironment.h"
#include "scripturesearch.h"
#include "biblebackend.h"
#include <QtTest/QTest>
#include <QElapsedTimer>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <archive.h>
#include <archive_entry.h>

using namespace ScriptureSearch;
class SearchTest : public QObject
{
    Q_OBJECT
    static Chapter chapter(const QStringList &texts) {
        Chapter c{80,999,"Source Book",{}};
        int number = 990;
        for (const auto &text : texts) c.verses.append({number++,text});
        return c;
    }
    static bool archiveFile(const QString &path, const QByteArray &body, const QByteArray &id = "engnet") {
        archive *a = archive_write_new();
        const auto cleanup = qScopeGuard([&] { archive_write_free(a); });
        if (archive_write_set_format_zip(a) != ARCHIVE_OK
            || archive_write_open_filename(a,QFile::encodeName(path).constData()) != ARCHIVE_OK) return false;
        archive_entry *entry = archive_entry_new();
        const auto cleanupEntry = qScopeGuard([&] { archive_entry_free(entry); });
        archive_entry_set_pathname(entry,("01-GEN"+id+".usfm").constData());
        archive_entry_set_size(entry,body.size()); archive_entry_set_filetype(entry,AE_IFREG); archive_entry_set_perm(entry,0600);
        return archive_write_header(a,entry) == ARCHIVE_OK
            && archive_write_data(a,body.constData(),body.size()) == body.size() && archive_write_close(a) == ARCHIVE_OK;
    }
    static QString sourceSegment(const Chapter &c, const QVariant &segment) {
        const auto map = segment.toMap();
        for (const auto &verse : c.verses) if (verse.number == map["verse"].toInt())
            return verse.text.mid(map["start"].toInt(),map["length"].toInt());
        return {};
    }
private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("OmaWordTests");
        QCoreApplication::setApplicationName("omaword-search-test");
    }
    void literalUnicodeOffsets() {
        const auto ascii = chapter({"ONE\r\n\t  Two", "THREE"});
        const auto asciiRows = find(ascii,"one two three").matches;
        QCOMPARE(asciiRows.size(),1);
        const auto asciiSegments = asciiRows.first().toMap()["segments"].toList();
        QCOMPARE(asciiSegments.size(),2);
        QCOMPARE(sourceSegment(ascii,asciiSegments[0]),QString("ONE\r\n\t  Two"));
        QCOMPARE(sourceSegment(ascii,asciiSegments[1]),QString("THREE"));
        const auto identity = find(chapter({"ONE TWO"}),"two").matches.first().toMap()["segments"].toList().first().toMap();
        QCOMPARE(identity["start"].toInt(),4); QCOMPARE(identity["length"].toInt(),3);
        const auto c = chapter({QString::fromUtf8("😀 Straße  é\t\nLIGHT café, ﬃ İ 𐐀 end"),"Next   verse"});
        for (const auto &pair : QList<QPair<QString,QString>>{
                 {"STRASSE","Straße"},{"ss","ß"},{"e","é"},{"😀","😀"},{"𐐨","𐐀"},
                 {"FFI","ﬃ"},{"i̇","İ"},{"é light","é\t\nLIGHT"},{"café,","café,"}}) {
            const auto result = find(c,pair.first);
            QVERIFY(result.error.isEmpty()); QVERIFY2(!result.matches.isEmpty(),qPrintable(pair.first));
            bool matched = false;
            for (const auto &row : result.matches) {
                const auto segments = row.toMap()["segments"].toList();
                if (segments.size() == 1 && sourceSegment(c,segments.first()) == pair.second) matched = true;
            }
            QVERIFY2(matched,qPrintable(pair.first));
        }
        QVERIFY(find(c,"cafe").matches.isEmpty());
        QVERIFY(find(c,"café.").matches.isEmpty());
        QVERIFY(find(c,"é light").matches.isEmpty()); // No canonical normalization.
        const auto cross = find(c,"END\n next").matches;
        QCOMPARE(cross.size(),1);
        const auto map = cross.first().toMap(); QCOMPARE(map["start"].toInt(),990); QCOMPARE(map["end"].toInt(),991);
        QCOMPARE(map["book"].toInt(),80); QCOMPARE(map["chapter"].toInt(),999);
        const auto segments = map["segments"].toList(), display = map["displaySegments"].toList();
        QCOMPARE(segments.size(),2); QCOMPARE(display.size(),2);
        for (int i = 0; i < segments.size(); ++i) {
            const auto d = display[i].toMap();
            QCOMPARE(map["text"].toString().mid(d["start"].toInt(),d["length"].toInt()),sourceSegment(c,segments[i]));
        }
        // An isolated UTF-16 half cannot produce a half-emoji source fragment.
        const auto half = find(c,QString(QChar(0xd83d))).matches;
        QCOMPARE(half.size(),1); QCOMPARE(sourceSegment(c,half.first().toMap()["segments"].toList().first()),QString("😀"));
        const auto graphemes = chapter({"👩‍👩‍👧‍👦 ᾈ ẞ"});
        for (const auto &pair : QList<QPair<QString,QString>>{{"👧","👩‍👩‍👧‍👦"},{"ἀι","ᾈ"},{"ss","ẞ"}}) {
            const auto rows = find(graphemes,pair.first).matches;
            QCOMPARE(rows.size(),1);
            QCOMPARE(sourceSegment(graphemes,rows.first().toMap()["segments"].toList().first()),pair.second);
        }
    }
    void parserAndAndSemantics() {
        const auto c = chapter({"alpha alpha alphabet BETA beta", "alpha", "beta", "say \"hello\" \\ world"});
        const auto matches = find(c,"AlPhA BETA",true).matches;
        QCOMPARE(matches.size(),1); QCOMPARE(matches.first().toMap()["start"].toInt(),990);
        QCOMPARE(matches.first().toMap()["segments"].toList().size(),5);
        QCOMPARE(find(c,"alpha alpha",true).matches.size(),2);
        QCOMPARE(find(c,"alpha beta",false).matches.size(),1); // literal crosses adjacent verses
        QCOMPARE(find(c,"\"say \\\"hello\\\"\"",true).matches.size(),1);
        QCOMPARE(find(c,"\\\\ world",true).matches.size(),1);
        QVERIFY(!find(c,"\"unfinished",true).error.isEmpty());
        QVERIFY(find(c,"\"unfinished").error.isEmpty());
        QVERIFY(!find(c,QString(4097,'x'),true).error.isEmpty());
        QVERIFY(find(c,QString(4096,'x'),true).error.isEmpty());
        QVERIFY(find(c," \t\n ").matches.isEmpty());
        QVERIFY(find(c,"\"\"",true).matches.isEmpty());
        const auto overlap = find(chapter({"banana"}),"ana nana",true).matches;
        QCOMPARE(overlap.size(),1);
        const auto segment = overlap.first().toMap()["segments"].toList();
        QCOMPARE(segment.size(),1); QCOMPARE(segment.first().toMap()["start"].toInt(),1);
        QCOMPARE(segment.first().toMap()["length"].toInt(),5);
        const auto folds = find(chapter({"ßß"}),"s",true).matches.first().toMap()["segments"].toList();
        QCOMPARE(folds.size(),1); QCOMPARE(folds.first().toMap()["length"].toInt(),2);
        QCOMPARE(find(chapter({"ßß"}),"s").matches.size(),2);
    }
    void chapterFindPatterns() {
        const auto c = chapter({"For God so loved the world", "God is love. Loved ones", "a\\b and 123 SHOUT"});
        auto count = [&](const QString &pattern) { return findPattern(c,pattern).matches.size(); };
        // Smart case: all-lowercase ignores case; any uppercase letter matches it.
        QCOMPARE(count("loved"),2); QCOMPARE(count("Loved"),1); QCOMPARE(count("god"),2);
        // \c forces any case, \C exact case; \c wins over \C.
        QCOMPARE(count("Loved\\c"),2); QCOMPARE(count("\\Cgod"),0); QCOMPARE(count("\\c\\Cgod"),2);
        // Escapes are not uppercase letters for smart case: \S still ignores case.
        QCOMPARE(count("\\Shout"),1); QCOMPARE(count("\\d+"),1);
        // An escaped backslash before c is a literal backslash and c.
        QCOMPARE(count("a\\\\b"),1);
        // Regular expressions, within a verse: alternation, classes, anchors.
        QCOMPARE(count("^god"),1); QCOMPARE(count("world|ones"),2); QCOMPARE(count("lov(ed|e)"),3);
        QCOMPARE(count("world god"),0); // never across verses
        const auto row = findPattern(c,"lo\\w+d").matches.first().toMap();
        QCOMPARE(row["start"].toInt(),990); QCOMPARE(row["end"].toInt(),990);
        QCOMPARE(sourceSegment(c,row["segments"].toList().first()),QString("loved"));
        // Empty matches are skipped; an invalid pattern is an error, not a crash.
        QCOMPARE(count("x*"),0);
        const auto invalid = findPattern(c,"lov(e");
        QCOMPARE(invalid.error,QString("invalid pattern")); QVERIFY(invalid.matches.isEmpty());
        QVERIFY(findPattern(c,"[").matches.isEmpty());
        QVERIFY(findPattern(c,"").error.isEmpty() && findPattern(c,"").matches.isEmpty());
        QVERIFY(!findPattern(c,QString(4097,'x')).error.isEmpty());
    }
    void phraseMinimalCovers() {
        auto c = chapter({"outside", "red green", "blue inside", "outside"});
        auto rows = find(c,"\"green blue\" inside",true).matches;
        QCOMPARE(rows.size(),1); QCOMPARE(rows.first().toMap()["start"].toInt(),991); QCOMPARE(rows.first().toMap()["end"].toInt(),992);
        QVERIFY(find(c,"\"green blue\" outside",true).matches.isEmpty());
        c = chapter({"a", "a", "b", "a", "b"});
        rows = find(c,"\"a\" \"b\"",true).matches;
        QCOMPARE(rows.size(),3);
        for (int i = 0; i < rows.size(); ++i) {
            QCOMPARE(rows[i].toMap()["start"].toInt(),991+i);
            QCOMPARE(rows[i].toMap()["end"].toInt(),992+i);
        }
        QCOMPARE(find(chapter({"a b", "a b"}),"\"a\" \"b\"",true).matches.size(),2);
        // Nested phrases and overlapping occurrences require no redundant cover.
        rows = find(chapter({"red", "blue red", "blue"}),"\"red blue\" \"blue\"",true).matches;
        QCOMPARE(rows.size(),2);
        QCOMPARE(rows[0].toMap()["start"].toInt(),990); QCOMPARE(rows[0].toMap()["end"].toInt(),991);
        QCOMPARE(rows[1].toMap()["start"].toInt(),991); QCOMPARE(rows[1].toMap()["end"].toInt(),992);
        // Source adjacency, including omitted verse IDs, is authoritative.
        c = {80,999,"Extra",{{1,"left"},{999,"right"}}};
        rows = find(c,"\"left right\"",true).matches;
        QCOMPARE(rows.size(),1); QCOMPARE(rows.first().toMap()["end"].toInt(),999);
    }
    void asynchronousBackendAndGeneration() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        qputenv("XDG_DATA_HOME",directory.path().toUtf8());
        const QString path = directory.filePath("engnet_usfm.zip");
        QByteArray body = "\\id GEN Search Bible\n\\c 1\n\\s1 headingOnly\n\\p\n";
        for (int i = 1; i <= 230; ++i) body += "\\v " + QByteArray::number(i) + " mercy light repeated repeated.\\f + \\ft noteOnly\\f*\n";
        body += "\\c 2\n\\p\n\\v 1 second chapter.\n";
        QVERIFY(archiveFile(path,body));
        BibleBackend backend;
        backend.importUsfm(QUrl::fromLocalFile(path));
        QTRY_VERIFY_WITH_TIMEOUT(!backend.busy(),10000);
        QVERIFY2(backend.ready(),qPrintable(backend.error()));
        const QString generation = backend.textGeneration(); QVERIFY(!generation.isEmpty());
        BibleBackend peer; QCOMPARE(peer.textGeneration(),generation);
        QSignalSpy searchChanged(&backend,&BibleBackend::searchChanged), navigation(&backend,&BibleBackend::navigationChanged);
        const auto before = backend.verses();
        const auto reference = backend.selectionReference();
        QCOMPARE(backend.findInChapter("repeated",true)["matches"].toList().size(),230);
        QCOMPARE(backend.findInChapter("repeated")["matches"].toList().size(),460);
        QCOMPARE(backend.verses(),before); QCOMPARE(backend.selectionReference(),reference); QCOMPARE(navigation.size(),0);
        backend.searchScripture("mercy"); QVERIFY(backend.searchBusy());
        QTRY_VERIFY_WITH_TIMEOUT(!backend.searchBusy(),10000);
        QVERIFY2(backend.searchError().isEmpty(),qPrintable(backend.searchError()));
        QCOMPARE(backend.searchResultCount(),230); QCOMPARE(backend.searchResults().size(),100);
        backend.loadMoreSearchResults(); QCOMPARE(backend.searchResults().size(),200);
        backend.loadMoreSearchResults(); QCOMPARE(backend.searchResults().size(),230);
        backend.loadMoreSearchResults(); QCOMPARE(backend.searchResults().size(),230);
        const auto results = backend.searchResults();
        for (int i = 0; i < results.size(); ++i) {
            const auto row = results[i].toMap(); QCOMPARE(row["start"].toInt(),i+1);
            QCOMPARE(row["versionId"].toString(),backend.versionId());
            QCOMPARE(row["versionAbbreviation"].toString(),backend.versionAbbreviation());
        }
        backend.searchScripture("\"unfinished"); backend.searchScripture("light");
        QTRY_VERIFY_WITH_TIMEOUT(!backend.searchBusy(),10000);
        QVERIFY(backend.searchError().isEmpty()); QCOMPARE(backend.searchResultCount(),230);
        backend.searchScripture("mercy"); backend.cancelScriptureSearch();
        QTest::qWait(30); QVERIFY(!backend.searchBusy()); QCOMPARE(backend.searchResultCount(),0);
        for (const QString query : {"headingOnly","noteOnly","\"repeated. second\""}) {
            backend.searchScripture(query); QTRY_VERIFY_WITH_TIMEOUT(!backend.searchBusy(),10000);
            QCOMPARE(backend.searchResultCount(),0);
        }
        backend.searchScripture("\"unfinished"); QTRY_VERIFY_WITH_TIMEOUT(!backend.searchBusy(),10000);
        QVERIFY(!backend.searchError().isEmpty());
        backend.searchScripture(QString(4097,'x')); QTRY_VERIFY_WITH_TIMEOUT(!backend.searchBusy(),10000);
        QVERIFY(backend.searchError().contains("4096"));
        peer.searchScripture("mercy"); QTRY_VERIFY_WITH_TIMEOUT(!peer.searchBusy(),10000);
        QCOMPARE(peer.searchResultCount(),230);
        QSignalSpy peerChanged(&peer,&BibleBackend::searchChanged), versionChanged(&peer,&BibleBackend::versionChanged);
        backend.searchScripture("mercy");
        body.replace("mercy","grace"); QVERIFY(archiveFile(path,body));
        backend.importUsfm(QUrl::fromLocalFile(path)); QTRY_VERIFY_WITH_TIMEOUT(!backend.busy(),10000);
        QVERIFY2(backend.error().isEmpty(),qPrintable(backend.error()));
        QVERIFY(backend.textGeneration() != generation); QCOMPARE(peer.textGeneration(),backend.textGeneration());
        QVERIFY(!versionChanged.isEmpty()); QVERIFY(!peerChanged.isEmpty());
        QCOMPARE(backend.searchResultCount(),0); QCOMPARE(peer.searchResultCount(),0);
        QVERIFY(backend.searchError().isEmpty()); QVERIFY(peer.searchError().isEmpty());
        peer.searchScripture("grace"); QTRY_VERIFY_WITH_TIMEOUT(!peer.searchBusy(),10000); QCOMPARE(peer.searchResultCount(),230);
        const QString replacementGeneration = backend.textGeneration();
        const QString otherPath = directory.filePath("eng-kjv_usfm.zip");
        QVERIFY(archiveFile(otherPath,body,"eng-kjv"));
        backend.importUsfm(QUrl::fromLocalFile(otherPath)); QTRY_VERIFY_WITH_TIMEOUT(!backend.busy(),10000);
        QCOMPARE(backend.versionId(),QString("eng-kjv"));
        QCOMPARE(peer.versionId(),QString("engnet")); QCOMPARE(peer.searchResultCount(),230);
        backend.searchScripture("\"unfinished");
        QVERIFY(backend.switchVersion("engnet")); QCOMPARE(backend.textGeneration(),replacementGeneration);
        QTest::qWait(30); QVERIFY(backend.searchError().isEmpty()); QCOMPARE(backend.searchResultCount(),0);
        QVERIFY(backend.removeVersion("eng-kjv",false));
        peer.searchScripture("\"unfinished");
        QVERIFY(backend.removeVersion(backend.versionId(),false));
        QTest::qWait(30); QVERIFY(peer.textGeneration().isEmpty()); QVERIFY(peer.searchError().isEmpty());
        QCOMPARE(peer.searchResultCount(),0); QVERIFY(!peer.searchBusy()); QVERIFY(!searchChanged.isEmpty());
    }
    void fullBiblePerformanceAndBibleOrder() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("synthetic.sqlite"), connection = "search-fixture";
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE",connection); db.setDatabaseName(path); QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TABLE metadata(key TEXT,value TEXT)"));
            QVERIFY(q.exec("INSERT INTO metadata VALUES('version_id','synthetic')"));
            QVERIFY(q.exec("CREATE TABLE books(id INTEGER,ordinal INTEGER,name TEXT)"));
            QVERIFY(q.exec("INSERT INTO books VALUES(80,1,'First'),(1,2,'Second')"));
            QVERIFY(q.exec("CREATE TABLE verses(book INTEGER,chapter INTEGER,number INTEGER,text TEXT,PRIMARY KEY(book,chapter,number))"));
            QVERIFY(db.transaction()); q.prepare("INSERT INTO verses VALUES(?,?,?,?)");
            for (int book : {80,1}) for (int chapter = 1; chapter <= 150; ++chapter) for (int verse = 1; verse <= 100; ++verse) {
                q.bindValue(0,book); q.bindValue(1,chapter); q.bindValue(2,verse);
                q.bindValue(3,"The light brings mercy and peace to all the people, and mercy remains. "); QVERIFY(q.exec());
            }
            QVERIFY(db.commit());
            QVERIFY(q.exec("UPDATE verses SET text='mercys peaceful' WHERE book=80 AND chapter=1 AND number=1"));
            QVERIFY(q.exec("UPDATE verses SET text='mercy distant distant peace Straße é 😀 “smart”' WHERE book=80 AND chapter=1 AND number=2"));
            QVERIFY(q.exec("UPDATE verses SET text='mercy peace' WHERE book=80 AND chapter=1 AND number=3"));
        }
        QSqlDatabase::removeDatabase(connection);
        QElapsedTimer timer; timer.start();
        auto result = search(path,"synthetic","SYN","mercy peace");
        const auto elapsed = timer.elapsed();
        QVERIFY2(result.error.isEmpty(),qPrintable(result.error)); QCOMPARE(result.matches.size(),30000);
        // Bible order: books.ordinal (book 80 first here), then chapter and
        // source verse keys; partial words ("mercys peaceful") are matches too.
        QCOMPARE(result.matches.first().toMap()["book"].toInt(),80); QCOMPARE(result.matches.first().toMap()["start"].toInt(),1);
        QCOMPARE(result.matches[1].toMap()["start"].toInt(),2); QCOMPARE(result.matches[2].toMap()["start"].toInt(),3);
        QCOMPARE(result.matches[100].toMap()["chapter"].toInt(),2); QCOMPARE(result.matches[100].toMap()["start"].toInt(),1);
        QCOMPARE(result.matches[14999].toMap()["book"].toInt(),80);
        QCOMPARE(result.matches[15000].toMap()["book"].toInt(),1);
        QCOMPARE(result.matches.last().toMap()["chapter"].toInt(),150); QCOMPARE(result.matches.last().toMap()["start"].toInt(),100);
        QVERIFY(!result.matches.first().toMap().contains("score"));
        qInfo("Synthetic 30,000 verses: AND search %lld ms; %lld results",elapsed,qint64(result.matches.size()));
        timer.restart();
        int heartbeats = 0;
        QTimer heartbeat; heartbeat.setInterval(5);
        connect(&heartbeat,&QTimer::timeout,this,[&] { ++heartbeats; }); heartbeat.start();
        QFutureWatcher<Result> watcher;
        watcher.setFuture(QtConcurrent::run([path] { return search(path,"synthetic","SYN","\"remains. the light\""); }));
        QTRY_VERIFY_WITH_TIMEOUT(watcher.isFinished(),15000);
        heartbeat.stop(); QVERIFY(heartbeats > 5);
        result = watcher.result();
        QVERIFY(result.error.isEmpty()); QCOMPARE(result.matches.size(),29697);
        qInfo("Synthetic 30,000 verses: cross-verse phrase %lld ms; %lld results",timer.elapsed(),qint64(result.matches.size()));
        for (const QString query : {"mercy peace","\"remains. the light\"","distant peace","absent"}) {
            for (int repeat = 0; repeat < 3; ++repeat) {
                timer.restart();
                const auto repeated = search(path,"synthetic","SYN",query);
                QVERIFY(repeated.error.isEmpty());
                QCOMPARE(repeated.matches.size(),query == "mercy peace" ? 30000 : query.startsWith('"') ? 29697 : query == "distant peace" ? 1 : 0);
                qInfo("Synthetic 30,000 verses: warm [%s] run %d: %lld ms",qPrintable(query),repeat+1,timer.elapsed());
            }
        }
        const auto unicode = search(path,"synthetic","SYN","STRASSE");
        QCOMPARE(unicode.matches.size(),1);
        const auto unicodeRow = unicode.matches.first().toMap();
        const auto unicodeSegment = unicodeRow["displaySegments"].toList().first().toMap();
        QCOMPARE(unicodeRow["text"].toString().mid(unicodeSegment["start"].toInt(),unicodeSegment["length"].toInt()),QString("Straße"));
        timer.restart();
        auto cancel = std::make_shared<std::atomic_bool>(true);
        QVERIFY(search(path,"synthetic","SYN","mercy",cancel).matches.isEmpty());
        QVERIFY(timer.elapsed() < 1000);
        cancel->store(false); timer.restart();
        watcher.setFuture(QtConcurrent::run([path,cancel] { return search(path,"synthetic","SYN","mercy",cancel); }));
        QTimer::singleShot(10,this,[cancel] { cancel->store(true); });
        QTRY_VERIFY_WITH_TIMEOUT(watcher.isFinished(),5000);
        QVERIFY(watcher.result().matches.isEmpty()); QVERIFY(watcher.result().error.isEmpty());
        qInfo("Synthetic 30,000 verses: warm worker cancellation observed within %lld ms (test polling included)",timer.elapsed());
        const QString coldPath = directory.filePath("cold-generation.sqlite");
        QVERIFY(QFile::copy(path,coldPath)); cancel->store(false); timer.restart();
        watcher.setFuture(QtConcurrent::run([coldPath,cancel] { return search(coldPath,"synthetic","SYN","mercy",cancel); }));
        QTimer::singleShot(10,this,[cancel] { cancel->store(true); });
        QTRY_VERIFY_WITH_TIMEOUT(watcher.isFinished(),5000);
        QVERIFY(watcher.result().matches.isEmpty()); QVERIFY(watcher.result().error.isEmpty());
        qInfo("Synthetic 30,000 verses: cold worker cancellation observed within %lld ms (test polling included)",timer.elapsed());
        QVERIFY(!search(directory.filePath("missing.sqlite"),"synthetic","SYN","mercy").error.isEmpty());
        QVERIFY(!search(path,"wrong","SYN","mercy").error.isEmpty());
        QVERIFY(QFile::remove(path));
        QVERIFY(!search(path,"synthetic","SYN","mercy").error.isEmpty()); // Cached generation must not revive an unlinked file.
    }
    // Search across editions: one FTS5 index (in the cache directory) of every
    // chapter's normalized text narrows the chapters to scan; matching stays
    // exactly the scan's.
    void indexedSearchAcrossEditions() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString index = directory.filePath("cache/search-index.sqlite");
        using Row = std::tuple<int,int,int,QString>;
        auto edition = [&](const QString &id, const QList<Row> &rows) {
            const QString path = directory.filePath(id + ".sqlite"), connection = "index-" + id;
            QFile::remove(path);
            {
                auto db = QSqlDatabase::addDatabase("QSQLITE",connection); db.setDatabaseName(path); db.open();
                QSqlQuery q(db);
                q.exec("CREATE TABLE metadata(key TEXT,value TEXT)");
                q.exec("INSERT INTO metadata VALUES('version_id','" + id + "')");
                q.exec("CREATE TABLE books(id INTEGER,ordinal INTEGER,name TEXT)");
                q.exec("INSERT INTO books VALUES(1,1,'Genesis'),(2,2,'Exodus')");
                q.exec("CREATE TABLE verses(book INTEGER,chapter INTEGER,number INTEGER,text TEXT,PRIMARY KEY(book,chapter,number))");
                q.prepare("INSERT INTO verses VALUES(?,?,?,?)");
                for (const auto &[book,chapter,verse,text] : rows) {
                    q.addBindValue(book); q.addBindValue(chapter); q.addBindValue(verse); q.addBindValue(text); q.exec();
                }
            }
            QSqlDatabase::removeDatabase(connection);
            return path;
        };
        const QString a = edition("alpha", {{1,1,1,"In the beginning God created"},{1,1,2,"the heaven and the earth."},
                                            {1,2,1,"Thus the heavens were finished."},{2,1,1,"A Straße of light, by God."}});
        const QString b = edition("beta", {{1,1,1,"At first God made"},{1,1,2,"the sky and the land."}});
        const QVector<Edition> both{{a,"alpha","A"},{b,"beta","B"}};
        auto references = [](const Result &result) {
            QStringList list;
            for (const auto &row : result.matches) list << row.toMap()["versionId"].toString() + " " + row.toMap()["reference"].toString();
            return list;
        };
        // Every edition in the given order, each in Bible order, tagged with its version.
        auto found = searchEditions(both,"god",index);
        QVERIFY2(found.error.isEmpty(),qPrintable(found.error));
        QCOMPARE(references(found),QStringList({"alpha Genesis 1:1","alpha Exodus 1:1","beta Genesis 1:1"}));
        QCOMPARE(found.matches.first().toMap()["versionAbbreviation"].toString(),QString("A"));
        QCOMPARE(found.matches.first().toMap()["bookName"].toString(),QString("Genesis"));
        QCOMPARE(indexedEditions(index),QStringList({"alpha","beta"}));
        // From the index now: a phrase across verses, full folding, AND within a
        // verse, and terms too short to index (scanned everywhere).
        QCOMPARE(references(searchEditions(both,"\"created the heaven\"",index)),QStringList({"alpha Genesis 1:1–2"}));
        QCOMPARE(references(searchEditions(both,"strasse",index)),QStringList({"alpha Exodus 1:1"}));
        QCOMPARE(references(searchEditions(both,"heaven earth",index)),QStringList({"alpha Genesis 1:2"}));
        QCOMPARE(references(searchEditions(both,"sky la",index)),QStringList({"beta Genesis 1:2"}));
        QCOMPARE(searchEditions(both,"nowhere",index).matches.size(),0);
        // A third edition evicts one prepared text; the index still serves it.
        const QString c = edition("gamma", {{1,1,1,"Gamma light"}});
        QCOMPARE(searchEditions({{c,"gamma","C"}},"light",index).matches.size(),1);
        QVERIFY(!cachedVersions().contains("alpha"));
        QCOMPARE(references(searchEditions(both,"heavens",index)),QStringList({"alpha Genesis 2:1"}));
        // A changed file is indexed again.
        QTest::qWait(20);
        edition("beta", {{1,1,1,"At first God made the heavens"}});
        QCOMPARE(references(searchEditions(both,"heavens",index)),QStringList({"alpha Genesis 2:1","beta Genesis 1:1"}));
        // Editions no longer installed leave the index.
        QCOMPARE(searchEditions({{a,"alpha","A"}},"god",index,{"alpha","beta"}).matches.size(),2);
        QCOMPARE(indexedEditions(index),QStringList({"alpha","beta"}));
        QCOMPARE(searchEditions({{a,"alpha","A"}},"god",index,{"alpha"}).matches.size(),2);
        QCOMPARE(indexedEditions(index),QStringList({"alpha"}));
        // An index file it cannot read is started again.
        { QFile broken(index); QVERIFY(broken.open(QIODevice::WriteOnly | QIODevice::Truncate)); broken.write("not a database"); }
        QFile::remove(index + "-wal"); QFile::remove(index + "-shm");
        QCOMPARE(references(searchEditions(both,"god",index)),QStringList({"alpha Genesis 1:1","alpha Exodus 1:1","beta Genesis 1:1"}));
        QCOMPARE(indexedEditions(index),QStringList({"alpha","beta"}));
        // A query error comes back as before.
        QVERIFY(!searchEditions(both,"\"unfinished",index).error.isEmpty());
        // A regular expression, in slashes or as the syntax asked for: matched
        // within each verse, a row per verse with every match, smart case as in
        // chapter find; indexed or not, every chapter is read.
        auto spans = [](const Result &result, int row) {
            QStringList list;
            const auto map = result.matches[row].toMap();
            for (const auto &segment : map["displaySegments"].toList())
                list << map["text"].toString().mid(segment.toMap()["start"].toInt(),segment.toMap()["length"].toInt());
            return list;
        };
        const auto heavens = searchEditions(both,"/heavens?\\b/",index);
        QVERIFY2(heavens.error.isEmpty(),qPrintable(heavens.error));
        QCOMPARE(references(heavens),QStringList({"alpha Genesis 1:2","alpha Genesis 2:1","beta Genesis 1:1"}));
        QCOMPARE(spans(heavens,0),QStringList({"heaven"}));
        QCOMPARE(heavens.matches.first().toMap()["segments"].toList().first().toMap()["verse"].toInt(),2);
        QCOMPARE(references(searchEditions(both,"th(e|us)\\b",index,{},{},Syntax::Pattern)).size(),4);
        const auto every = searchEditions(both," /the/ ",index);
        QCOMPARE(spans(every,0),QStringList({"the"})); // Genesis 1:1: "In the beginning"
        QCOMPARE(spans(every,1),QStringList({"the","the"}));
        // Smart case: an uppercase letter makes it exact; \c ignores case again.
        QCOMPARE(references(searchEditions(both,"/God/",index)).size(),3);
        QCOMPARE(references(searchEditions(both,"/^In/",index)),QStringList({"alpha Genesis 1:1"}));
        QCOMPARE(references(searchEditions(both,"/^in/",index)),QStringList({"alpha Genesis 1:1"}));
        QCOMPARE(references(searchEditions(both,"/^IN/",index)).size(),0);
        QCOMPARE(references(searchEditions(both,"/^IN\\c/",index)),QStringList({"alpha Genesis 1:1"}));
        // Never across verses: "created the" ends one verse and starts the next.
        QCOMPARE(searchEditions(both,"/created the/",index).matches.size(),0);
        // Empty matches are skipped; a bad pattern says why; bare slashes are words.
        QCOMPARE(searchEditions(both,"/x*/",index).matches.size(),0);
        const auto broken = searchEditions(both,"/(god/",index);
        QVERIFY(broken.error.startsWith("Invalid regular expression: "));
        QCOMPARE(searchEditions(both,"//",index).error,QString());
    }
    void twoVersionsStayCached() {
        // Two windows searching different versions keep both prepared texts;
        // a third version evicts the least recently searched.
        QTemporaryDir directory; QVERIFY(directory.isValid());
        auto edition = [&](const QString &id) {
            const QString path = directory.filePath(id + ".sqlite"), connection = "cache-" + id;
            {
                auto db = QSqlDatabase::addDatabase("QSQLITE",connection); db.setDatabaseName(path); db.open();
                QSqlQuery q(db);
                q.exec("CREATE TABLE metadata(key TEXT,value TEXT)");
                q.exec("INSERT INTO metadata VALUES('version_id','" + id + "')");
                q.exec("CREATE TABLE books(id INTEGER,ordinal INTEGER,name TEXT)");
                q.exec("INSERT INTO books VALUES(1,1,'Genesis')");
                q.exec("CREATE TABLE verses(book INTEGER,chapter INTEGER,number INTEGER,text TEXT,PRIMARY KEY(book,chapter,number))");
                q.exec("INSERT INTO verses VALUES(1,1,1,'In the beginning " + id + "')");
            }
            QSqlDatabase::removeDatabase(connection);
            return path;
        };
        const QString a = edition("alpha"), b = edition("beta"), c = edition("gamma");
        QCOMPARE(search(a,"alpha","A","beginning").matches.size(),1);
        QCOMPARE(search(b,"beta","B","beginning").matches.size(),1);
        QCOMPARE(cachedVersions(),QStringList({"beta","alpha"}));
        QCOMPARE(search(a,"alpha","A","alpha").matches.size(),1); // A hit moves it to the front.
        QCOMPARE(cachedVersions(),QStringList({"alpha","beta"}));
        QCOMPARE(search(c,"gamma","C","gamma").matches.size(),1);
        QCOMPARE(cachedVersions(),QStringList({"gamma","alpha"}));
    }
};
QTEST_GUILESS_MAIN(SearchTest)
#include "tst_search.moc"
