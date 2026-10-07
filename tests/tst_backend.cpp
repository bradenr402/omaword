#include "testenvironment.h"
#include <QtTest/QTest>
#include "biblebackend.h"
#include "biblebooks.h"
#include "textselection.h"
#include <QClipboard>
#include <QCryptographicHash>
#include <QSqlRecord>
#include <QUuid>
#include <QDateTime>
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
    static QMap<QString,QByteArray> studyEntries() {
        return {{"01-GENstudy.usfm", "\\id GEN Study Test Bible\n\\c 1\n\\p\n\\v 1 In the beginning.\\f + \\fr 1:1 \\ft First note\\f*\\x - \\xo 1:1 \\xt Jn 1:1-3; John 1:1-3\\x*\n"
                    "\\v 2 Formless.\\x - \\xo 1:2 \\xt Jn 1:3; Mt 1:2-3, 1; Zz 9:9\\x*\n\\v 3 Light.\\f b \\ft Second note\\f*\n\\c 2\n\\p\n\\v 1 Finished.\n"},
                {"02-MATstudy.usfm", "\\id MAT Study Test Bible\n\\c 1\n\\p\n\\v 1 Book.\n\\v 2 Abraham.\n\\v 3 Judah.\n"},
                {"03-JHNstudy.usfm", "\\id JHN Study Test Bible\n\\c 1\n\\p\n\\v 1 Word.\n\\v 2 Same.\n\\v 3 Made.\n"
                    "\\c 3\n\\p\n\\v 16 For God so loved. He said, \\wj Truly I say\\wj* again.\n"}};
    }
    // Rewrites a schema 4 generation as the schema 3 layout older builds wrote.
    static bool downgradeToSchema3(const QString &path) {
        const QString name = QUuid::createUuid().toString();
        bool ok = false;
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name); db.setDatabaseName(path);
            if (db.open()) {
                QSqlQuery q(db);
                ok = q.exec("DROP TABLE notes") && q.exec("DROP TABLE words_of_jesus")
                    && q.exec("DELETE FROM metadata WHERE key IN ('note_count','words_of_jesus')")
                    && q.exec("UPDATE metadata SET value='3' WHERE key='schema'")
                    && q.exec("UPDATE verses SET html=replace(replace(html,'<span class=\"wj\">',''),'</span>','')");
                QCryptographicHash hash(QCryptographicHash::Sha256);
                auto field = [&](const QByteArray &v) { hash.addData(QByteArray::number(v.size())); hash.addData(":"); hash.addData(v); };
                for (const QString &sql : {QString("SELECT key,value FROM metadata WHERE key<>'content_digest' ORDER BY key"),
                         QString("SELECT id,code,name,abbreviation,ordinal FROM books ORDER BY ordinal"),
                         QString("SELECT book,chapter FROM source_chapters ORDER BY book,chapter"),
                         QString("SELECT book,name FROM book_aliases ORDER BY book,name"),
                         QString("SELECT book,chapter,number,retained FROM source_keys ORDER BY book,chapter,number"),
                         QString("SELECT book,chapter,number,text,html,heading,poetry,paragraphStart FROM verses ORDER BY book,chapter,number")}) {
                    field(sql.toUtf8()); ok = ok && q.exec(sql);
                    while (q.next()) for (int i = 0; i < q.record().count(); ++i) field(q.value(i).toString().toUtf8());
                }
                q.prepare("UPDATE metadata SET value=? WHERE key='content_digest'"); q.addBindValue(QString::fromLatin1(hash.result().toHex()));
                ok = ok && q.exec();
                db.close();
            }
        }
        QSqlDatabase::removeDatabase(name);
        return ok;
    }
    static QVariantMap versionRow(BibleBackend *b, const QString &id) {
        for (const auto &row : b->versions()) if (row.toMap()["id"] == id) return row.toMap();
        return {};
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
        QVERIFY(q.exec("PRAGMA user_version")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),7); q.finish();
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
        return {{16, "For God so loved the world, that he gave his Son.", false, false},
                {17, "God did not send his Son to condemn. He came to save!", false, false},
                {18, "Whoever believes is not condemned.", false, true},
                {19, "", false, false},
                {20, "Light (came) into the world.", true, false},
                {21, "Darkness hated it.", true, false}};
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
        QCOMPARE(text(v, at(0, 0), at(0, a.size()), true, "John 3:16"), QStringLiteral("“") + a + QStringLiteral("”\n\n—John 3:16"));
        // Partial single verse: ellipses at both cuts, punctuation trimmed at them.
        QCOMPARE(text(v, at(0, 4), at(0, 27), true, "John 3:16"), QStringLiteral("“… God so loved the world …”\n\n—John 3:16"));
        QCOMPARE(text(v, at(0, 0), at(0, 16), true, "R"), QStringLiteral("“For God so loved …”\n\n—R"));
        QCOMPARE(text(v, at(0, 26), at(0, a.size()), true, "R"), QStringLiteral("“… that he gave his Son.”\n\n—R"));
        // Across verses: the first verse's number only when its beginning is selected.
        QCOMPARE(text(v, at(0, 21), at(1, 7), true, "John 3:16–17"),
                 QStringLiteral("“… world, that he gave his Son. ¹⁷ God did …”\n\n—John 3:16–17"));
        QCOMPARE(text(v, at(0, 0), at(1, b.size()), true, "John 3:16–17"),
                 QStringLiteral("“¹⁶ ") + a + QStringLiteral(" ¹⁷ ") + b + QStringLiteral("”\n\n—John 3:16–17"));
        QCOMPARE(text(v, at(0, 0), at(1, 3), true, "R"), QStringLiteral("“¹⁶ ") + a + QStringLiteral(" ¹⁷ God …”\n\n—R"));
        // Paragraph and poetry joins follow the source.
        const int he = b.indexOf("He came");
        QCOMPARE(text(v, at(1, he), at(2, 8), false), b.mid(he) + "\n\nWhoever ");
        QCOMPARE(text(v, at(4, 0), at(5, 8), true, "R"), QStringLiteral("“²⁰ ") + v[4].text + QStringLiteral("\n²¹ Darkness …”\n\n—R"));
        // An end at a verse's start or a start at its end touches nothing there.
        QCOMPARE(touched(v, at(0, a.size()), at(2, 0)), qMakePair(1, 1));
        QCOMPARE(text(v, at(0, a.size()), at(2, 0), true, "R"), QStringLiteral("“") + b + QStringLiteral("”\n\n—R"));
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
        QVERIFY(bible->characterText(1, 0, 1, one.size()).startsWith(QStringLiteral("“") + one + QStringLiteral("”\n\n—")));
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
        QVERIFY(zip(path,{
            {"01-GENengnet.usfm","\\id GEN Copy Bible\n\\toc2 First-Book\n\\c 1\n\\p\n\\v 1 First-word.\n\\v 2 Last-word.\n"},
            {"02-REVengnet.usfm","\\id REV Copy Bible\n\\c 3\n\\p\n\\v 14 First-word.\n\\v 15 Middle-word.\n\\v 22 Last-word.\n\\c 4\n\\p\n\\v 1 Next-word.\n"}
        }));
        install(path); QVERIFY2(bible->navigate(input),qPrintable(bible->error()));
        const QString quotation = bible->copySelection();
        QVERIFY2(quotation.endsWith(QStringLiteral("”\n\n—")+label),qPrintable(quotation));
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
        QVERIFY(zip(path,{{"01-REVengnet.usfm",
            "\\id REV Heading Bible\n\\is1 Introduction\n\\ip Introductory text.\n"
            "\\c 3\n\\s1 To the Church in Sardis\n\\p\n\\v 1 First.\n"
            "\\s1 To the Church in Philadelphia\n\\p\n\\v 7 Later.\n"
            "\\c 4\n\\s1 Revelation 4\n\\p\n\\v 1 Next chapter.\n"
            "\\c 5\n\\p\n\\v 1 No source heading.\n"}}));
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
        QVERIFY(zip(path,{{"01-GENengojb.usfm",QStringLiteral(
            "\\id GEN Copy Bible\n\\toc1 Bereshis\n\\toc2 Bereishis\n\\c 1\n\\p\n"
            "\\v 1 Shalom   café 😀 é.\n"
            "\\v 2 Same\t paragraph.\n\\p\n"
            "\\v 3 New paragraph.\n\\q1\n"
            "\\v 4 First   poetry line\n\\q2 Second line\n"
            "\\v 5 Poetry continuation.\n\\p\n"
            "\\v 6 Last paragraph.\n\\c 2\n\\p\n"
            "\\v 1 Next chapter.\n").toUtf8()}}));
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
        QVERIFY(!rows[4].toMap()["paragraphStart"].toBool());
        QCOMPARE(bible->copyVerse(4,true),text[3]);
        const QString expected = text[0]+' '+text[1]+"\n\n"+text[2]+"\n\n"+text[3]+'\n'+text[4]+"\n\n"+text[5];
        bible->selectVerses(1,6);
        QCOMPARE(bible->copySelection(true),expected);
        const QString quotation = QStringLiteral("“¹ ")+text[0]+QStringLiteral(" ² ")+text[1]
            +QStringLiteral("\n\n³ ")+text[2]+QStringLiteral("\n\n⁴ ")+text[3]
            +QStringLiteral("\n⁵ ")+text[4]+QStringLiteral("\n\n⁶ ")+text[5]
            +QStringLiteral("”\n\n—")+bible->selectionReference();
        QCOMPARE(bible->copySelection(),quotation);
        bible->setReadingMode("block"); bible->setFocusMode(true); bible->setShowVerseNumbers(false);
        QCOMPARE(bible->copySelection(true),expected); QCOMPARE(bible->copySelection(),quotation);
        bible->moveSelection(1,true);
        const auto next = bible->verses().first().toMap()["text"].toString();
        QCOMPARE(bible->copySelection(true),expected+"\n\n"+next);
        QVERIFY(bible->copySelection().endsWith(QStringLiteral("\n\n¹ ")+next+QStringLiteral("”\n\n—")+bible->selectionReference()));
        const QString spanning = bible->copySelection(true), spanningQuote = bible->copySelection();
        QVERIFY(bible->addToDesk()); QCOMPARE(bible->desk().first().toMap()["text"].toString(),spanning);
        // A fixture with a paragraph before every verse must copy paragraph breaks.
        const QString paragraphs = fixtures.filePath("copy-engnet_usfm.zip");
        QVERIFY(zip(paragraphs,{{"01-GENengnet.usfm","\\id GEN Paragraph Bible\n\\c 1\n\\p\n\\v 1 One.\n\\p\n\\v 2 Two.\n"}}));
        install(paragraphs); QVERIFY(bible->navigate("GEN 1:1-2"));
        QCOMPARE(bible->copySelection(true),QString("One.\n\nTwo."));
        // Desk text of an inactive version is read from its own generation.
        const auto item = bible->desk().first().toMap();
        QCOMPARE(item["versionId"].toString(),QString("engojb")); QCOMPARE(item["text"].toString(),spanning);
        QCOMPARE(bible->copyDeskItem(item["id"].toInt()),spanningQuote);
    }
    void schemaFourMigrationPreservesArchivesAndRollsBack() {
        const QString path = bible->dataDirectory()+"/personal.sqlite";
        bible.reset();
        const auto cleanup = qScopeGuard([] { QSqlDatabase::removeDatabase("migration"); });
        auto db = QSqlDatabase::addDatabase("QSQLITE","migration"); db.setDatabaseName(path); QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec("DROP TABLE desk")); // Schema 4 stores predate the Desk and History.
        QVERIFY(q.exec("DROP TABLE history")); QVERIFY(q.exec("DROP TABLE recent_queries"));
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
        QVERIFY(q.exec("PRAGMA user_version")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),7);
    }
    void schemaFiveDeskMigrationRollsBack() {
        install(net); QVERIFY(bible->navigate("John 3:16")); bible->setNoteText("Pre-desk note");
        const QString path = bible->dataDirectory()+"/personal.sqlite";
        bible.reset();
        const auto cleanup = qScopeGuard([] { QSqlDatabase::removeDatabase("desk-migration"); });
        auto db = QSqlDatabase::addDatabase("QSQLITE","desk-migration"); db.setDatabaseName(path); QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec("DROP TABLE desk")); QVERIFY(q.exec("DROP TABLE history")); QVERIFY(q.exec("DROP TABLE recent_queries"));
        QVERIFY(q.exec("PRAGMA user_version=5"));
        // A conflicting object makes the schema 6 step fail; nothing may change.
        QVERIFY(q.exec("CREATE VIEW desk AS SELECT 1 AS id"));
        bible = std::make_unique<BibleBackend>(); QVERIFY(!bible->error().isEmpty()); QVERIFY(bible->desk().isEmpty()); bible.reset();
        QVERIFY(q.exec("PRAGMA user_version")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),5); q.finish();
        QVERIFY(q.exec("SELECT note FROM annotations")); QVERIFY(q.next()); QCOMPARE(q.value(0).toString(),QString("Pre-desk note")); q.finish();
        QVERIFY(q.exec("DROP VIEW desk"));
        bible = std::make_unique<BibleBackend>(); QVERIFY2(bible->error().isEmpty(),qPrintable(bible->error()));
        QVERIFY(bible->ready()); QCOMPARE(bible->noteText(),QString("Pre-desk note"));
        QVERIFY(q.exec("PRAGMA user_version")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),7); q.finish();
        QVERIFY(bible->desk().isEmpty()); QVERIFY(bible->addToDesk()); QCOMPARE(bible->desk().size(),1);
    }
    void schemaSixHistoryMigrationRollsBack() {
        install(net); QVERIFY(bible->navigate("John 3:16")); bible->setNoteText("Pre-history note");
        QVERIFY(bible->addToDesk());
        const QString path = bible->dataDirectory()+"/personal.sqlite";
        bible.reset();
        const auto cleanup = qScopeGuard([] { QSqlDatabase::removeDatabase("history-migration"); });
        auto db = QSqlDatabase::addDatabase("QSQLITE","history-migration"); db.setDatabaseName(path); QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec("DROP TABLE history")); QVERIFY(q.exec("DROP TABLE recent_queries"));
        QVERIFY(q.exec("PRAGMA user_version=6"));
        // recent_queries is created after history: a conflict there must roll back both.
        QVERIFY(q.exec("CREATE VIEW recent_queries AS SELECT 'x' AS query"));
        bible = std::make_unique<BibleBackend>(); QVERIFY(!bible->error().isEmpty()); QVERIFY(bible->history().isEmpty()); bible.reset();
        QVERIFY(q.exec("PRAGMA user_version")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),6); q.finish();
        QVERIFY(q.exec("SELECT COUNT(*) FROM sqlite_master WHERE name='history'")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),0); q.finish();
        QVERIFY(q.exec("SELECT note FROM annotations")); QVERIFY(q.next()); QCOMPARE(q.value(0).toString(),QString("Pre-history note")); q.finish();
        QVERIFY(q.exec("DROP VIEW recent_queries"));
        bible = std::make_unique<BibleBackend>(); QVERIFY2(bible->error().isEmpty(),qPrintable(bible->error()));
        QVERIFY(bible->ready()); QCOMPARE(bible->noteText(),QString("Pre-history note")); QCOMPARE(bible->desk().size(),1);
        QVERIFY(q.exec("PRAGMA user_version")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),7); q.finish();
        QVERIFY(bible->historyEnabled()); QVERIFY(bible->history().isEmpty()); QVERIFY(bible->recentQueries().isEmpty());
        QVERIFY(bible->recordHistory()); QCOMPARE(bible->history().size(),1);
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
        QVERIFY(!top["excerpt"].toString().isEmpty()); QVERIFY(top["excerpt"].toString().size() <= 160);
        QVERIFY(QDateTime::fromString(top["visitedAt"].toString(),Qt::ISODateWithMs).isValid());
        QVERIFY(!bible->history()[1].toMap()["excerpt"].toString().isEmpty());
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
        auto broken = genesis; broken["book"] = 81; QVERIFY(!bible->recordHistory(broken));
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
        install(net); QVERIFY(bible->navigate("John 3:16")); QVERIFY(bible->addToDesk());
        const auto before = QUrl::fromLocalFile(session->filePath("before.json")); QVERIFY(bible->exportBackup(before));
        QVERIFY(bible->recordHistory()); QVERIFY(bible->recordQuery("light"));
        const auto after = QUrl::fromLocalFile(session->filePath("after.json")); QVERIFY(bible->exportBackup(after));
        auto read = [](const QUrl &url) { QFile f(url.toLocalFile()); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); };
        const auto a = read(before), b = read(after);
        auto withoutStamp = [](const QByteArray &bytes) { auto o = QJsonDocument::fromJson(bytes).object(); o.remove("created"); return o; };
        QVERIFY(!a.isEmpty()); QCOMPARE(withoutStamp(b),withoutStamp(a)); // Only the export time differs.
        const auto root = QJsonDocument::fromJson(b).object();
        QCOMPARE(root["version"].toInt(),5); QVERIFY(!root.contains("history")); QVERIFY(!b.contains("light"));
        // Restoring a backup leaves History alone.
        QVERIFY2(bible->importBackup(after),qPrintable(bible->error())); QCOMPARE(bible->history().size(),1);
        QCOMPARE(bible->recentQueries(),QStringList({"light"}));
    }
    void deskOrderToggleCleanAndPersistence() {
        install(net);
        QVERIFY(!bible->addToDesk()); QVERIFY(!bible->error().isEmpty()); // No selection yet.
        QSignalSpy changed(bible.get(),&BibleBackend::deskChanged);
        QVERIFY(bible->navigate("John 3:16-18")); QVERIFY(!bible->currentInDesk());
        const auto john = bible->tagScope(); const QString johnText = bible->copySelection(true), johnQuote = bible->copySelection();
        QVERIFY(bible->addToDesk()); QCOMPARE(changed.count(),1); QVERIFY(bible->currentInDesk());
        QVERIFY(bible->addToDesk(john)); QCOMPARE(changed.count(),1); QCOMPARE(bible->desk().size(),1); // Duplicate no-op.
        QVERIFY(bible->navigate("GEN 1:4-7")); QVERIFY(!bible->currentInDesk());
        QVERIFY(bible->toggleDesk()); QVERIFY(bible->currentInDesk());
        QVERIFY(bible->navigate("MAT 1:2")); QVERIFY(bible->addToDesk());
        auto desk = bible->desk(); QCOMPARE(desk.size(),3);
        QStringList refs; for (const auto &row : desk) refs << row.toMap()["reference"].toString();
        QCOMPARE(refs,QStringList({"John 3:16–18","Genesis 1:4–7","Matthew 1:2"}));
        const auto first = desk.first().toMap();
        for (const QString key : {"id","versionId","versionAbbreviation","reference","book","chapter","start","endBook","endChapter","end","created","text","available"})
            QVERIFY2(first.contains(key),qPrintable(key));
        QCOMPARE(first["versionId"].toString(),QString("engnet")); QVERIFY(first["available"].toBool());
        QCOMPARE(first["text"].toString(),johnText); QVERIFY(bible->inDesk(john));
        QCOMPARE(bible->copyDeskItem(first["id"].toInt()),johnQuote);
        QCOMPARE(QGuiApplication::clipboard()->text(),johnQuote);
        QVERIFY(bible->navigate("GEN 1:4-7")); QVERIFY(bible->toggleDesk()); QVERIFY(!bible->currentInDesk());
        QCOMPARE(bible->desk().size(),2);
        QVERIFY(bible->toggleDesk()); QCOMPARE(bible->desk().last().toMap()["reference"].toString(),QString("Genesis 1:4–7"));
        QVERIFY(bible->removeFromDesk(first["id"].toInt())); QVERIFY(!bible->inDesk(john));
        QVERIFY(!bible->removeFromDesk(first["id"].toInt())); QVERIFY(!bible->error().isEmpty());
        QVERIFY(!bible->openDeskItem(first["id"].toInt())); QVERIFY(bible->copyDeskItem(first["id"].toInt()).isEmpty());
        const auto before = bible->desk(); QCOMPARE(before.size(),2);
        bible.reset(); bible = std::make_unique<BibleBackend>(); QVERIFY2(bible->ready(),qPrintable(bible->error()));
        QCOMPARE(bible->desk(),before);
        QVERIFY(bible->openDeskItem(before.first().toMap()["id"].toInt()));
        QCOMPARE(bible->selectionReference(),QString("Matthew 1:2"));
        QVERIFY(bible->cleanDesk()); QVERIFY(bible->desk().isEmpty());
        bible.reset(); bible = std::make_unique<BibleBackend>(); QVERIFY(bible->desk().isEmpty());
    }
    void deleteNoteScopeKeepsMarksAndTags() {
        install(net); BibleBackend peer;
        QVERIFY(bible->navigate("MRK 7:15")); bible->moveSelection(2,true);
        const auto scope = bible->tagScope();
        bible->setNoteText("A long thought"); QVERIFY(bible->toggleBookmark()); QVERIFY(bible->toggleHighlight());
        QVERIFY(!bible->createAndAssignTag(scope,"Kept").isEmpty());
        // A note on an overlapping, different range survives.
        bible->selectVerses(15,15); bible->setNoteText("Other range");
        bible->selectVerses(15,17);
        QSignalSpy peerChanged(&peer,&BibleBackend::collectionChanged);
        QVERIFY2(bible->deleteNoteScope(scope),qPrintable(bible->error()));
        QVERIFY(bible->noteText().isEmpty());
        QVERIFY(bible->selectionBookmarked()); QVERIFY(bible->selectionHighlighted());
        QCOMPARE(bible->tagsForScope(scope).size(),1);
        QVERIFY(peerChanged.count() > 0);
        QVariantMap row;
        for (const auto &v : peer.collection()) if (v.toMap()["reference"] == scope["reference"]) row = v.toMap();
        QVERIFY(row["bookmark"].toBool()); QVERIFY(row["highlight"].toBool()); QVERIFY(row["note"].toString().isEmpty());
        bible->selectVerses(15,15); QCOMPARE(bible->noteText(),QString("Other range"));
        // No note left on the exact range: nothing to delete.
        QVERIFY(!bible->deleteNoteScope(scope)); QVERIFY(!bible->error().isEmpty());
        // A row with only a note is removed entirely.
        const auto single = bible->tagScope();
        QVERIFY2(bible->deleteNoteScope(single),qPrintable(bible->error()));
        for (const auto &v : bible->collection()) QVERIFY(v.toMap()["reference"] != single["reference"]);
        QCOMPARE(bible->collection().size(),1);
    }
    void deskCrossVersionPeersAndRemoval() {
        install(net); BibleBackend peer; QVERIFY(peer.desk().isEmpty());
        QSignalSpy peerChanged(&peer,&BibleBackend::deskChanged);
        QVERIFY(bible->navigate("GEN 1:4-6")); QVERIFY(bible->addToDesk());
        QCOMPARE(peerChanged.count(),1); QCOMPARE(peer.desk(),bible->desk());
        install(kjv); QVERIFY(bible->navigate("TOB 1:2-3")); const auto tobit = bible->tagScope();
        const QString text = bible->copySelection(true), quote = bible->copySelection();
        QVERIFY(peer.addToDesk(tobit)); QCOMPARE(bible->desk(),peer.desk()); QVERIFY(bible->currentInDesk());
        QVERIFY(bible->switchVersion("engnet"));
        auto item = bible->desk().last().toMap();
        QCOMPARE(item["versionId"].toString(),QString("eng-kjv")); QVERIFY(item["available"].toBool());
        QCOMPARE(item["text"].toString(),text); QCOMPARE(bible->copyDeskItem(item["id"].toInt()),quote);
        QCOMPARE(item["reference"].toString(),tobit["reference"].toString());
        QVERIFY(bible->openDeskItem(item["id"].toInt())); QCOMPARE(bible->versionId(),QString("eng-kjv"));
        QCOMPARE(bible->selectionReference(),tobit["reference"].toString()); QVERIFY(bible->currentInDesk());
        QCOMPARE(bible->deskCountForVersion("eng-kjv"),1); QCOMPARE(bible->deskCountForVersion("engnet"),1);
        // Keeping annotations keeps Desk items; they are unavailable until reinstalled.
        QVERIFY(bible->removeVersion("eng-kjv",false));
        item = peer.desk().last().toMap(); QVERIFY(!item["available"].toBool()); QVERIFY(item["text"].toString().isEmpty());
        QCOMPARE(item["reference"].toString(),QString("Tobit 1:2–3"));
        QVERIFY(!bible->openDeskItem(item["id"].toInt())); QCOMPARE(bible->deskCountForVersion("eng-kjv"),1);
        install(kjv); QVERIFY(peer.desk().last().toMap()["available"].toBool());
        QVERIFY(bible->removeVersion("eng-kjv",true));
        QCOMPARE(bible->deskCountForVersion("eng-kjv"),0); QCOMPARE(peer.desk().size(),1);
        QCOMPARE(peer.desk().first().toMap()["versionId"].toString(),QString("engnet"));
        QVERIFY(peer.cleanDesk()); QVERIFY(bible->desk().isEmpty());
    }
    void deskBackupRoundTrip() {
        install(net);
        QVERIFY(bible->navigate("GEN 1:2")); QVERIFY(bible->addToDesk());
        QVERIFY(bible->navigate("John 3:16")); QVERIFY(bible->addToDesk());
        const auto saved = bible->desk();
        const auto backup = QUrl::fromLocalFile(session->filePath("desk.json")); QVERIFY(bible->exportBackup(backup));
        QFile file(backup.toLocalFile()); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto root = QJsonDocument::fromJson(file.readAll()).object(); file.close();
        QCOMPARE(root["version"].toInt(),5); QCOMPARE(root["desk"].toArray().size(),2);
        QVERIFY(bible->cleanDesk()); QVERIFY(bible->navigate("MAT 1:1")); QVERIFY(bible->addToDesk());
        BibleBackend peer;
        QVERIFY2(bible->importBackup(backup),qPrintable(bible->error()));
        QCOMPARE(peer.desk(),bible->desk());
        QStringList refs; for (const auto &row : bible->desk()) refs << row.toMap()["reference"].toString();
        QCOMPARE(refs,QStringList({"Genesis 1:2","John 3:16"})); // Version-scoped replacement.
        QCOMPARE(bible->desk().first().toMap()["created"],saved.first().toMap()["created"]);
        // Older formats restore without touching the Desk.
        auto legacy = root; legacy["version"] = 4; legacy.remove("desk");
        const auto old = QUrl::fromLocalFile(session->filePath("desk-v4.json"));
        QVERIFY(write(old.toLocalFile(),QJsonDocument(legacy).toJson()));
        QVERIFY(bible->cleanDesk()); QVERIFY2(bible->importBackup(old),qPrintable(bible->error())); QVERIFY(bible->desk().isEmpty());
        for (const QString kind : {"duplicate","invalid","missing-created"}) {
            auto broken = root; auto items = broken["desk"].toArray(); auto a = items.first().toObject();
            if (kind == "invalid") a["start"] = 999;
            if (kind == "missing-created") a.remove("created");
            items[0] = a; if (kind == "duplicate") items.append(a); broken["desk"] = items;
            const auto path = QUrl::fromLocalFile(session->filePath("broken-desk.json"));
            QVERIFY(write(path.toLocalFile(),QJsonDocument(broken).toJson()));
            QVERIFY2(!bible->importBackup(path),qPrintable(kind)); QVERIFY(bible->desk().isEmpty());
        }
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
    void textSchemaFootnotesAndReimportFlag() {
        const QString path = fixtures.filePath("study_usfm.zip"); QVERIFY(zip(path, studyEntries()));
        install(path);
        QCOMPARE(bible->versionId(), QString("study")); QCOMPARE(bible->textSchema(), 4); QVERIFY(bible->hasWordsOfJesus());
        QVERIFY(!versionRow(bible.get(),"study")["needsReimport"].toBool());
        QCOMPARE(versionRow(bible.get(),"study")["textSchema"].toInt(), 4);
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
        // An older generation stays readable and is flagged for re-import.
        const QString file = installedPath();
        bible.reset(); QVERIFY(downgradeToSchema3(file)); bible = std::make_unique<BibleBackend>();
        QVERIFY2(bible->ready(), qPrintable(bible->error()));
        QCOMPARE(bible->textSchema(), 3); QVERIFY(!bible->hasWordsOfJesus()); QVERIFY(!bible->hasCrossReferences()); QVERIFY(bible->footnotes().isEmpty());
        QVERIFY(versionRow(bible.get(),"study")["needsReimport"].toBool());
        QVERIFY(bible->navigate("John 3:16")); QVERIFY(bible->verses()[0].toMap()["wordsOfJesus"].toList().isEmpty());
        // Re-import pins the edition: another edition's file is refused.
        bible->importUsfm(QUrl::fromLocalFile(net), "study");
        QVERIFY(!bible->busy()); QVERIFY(bible->error().contains("not")); QCOMPARE(bible->versions().size(), 1);
        bible->clearError();
        // Re-import through the ordinary install/replace flow keeps annotations.
        bible->importUsfm(QUrl::fromLocalFile(path), "study");
        QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(),60000); QVERIFY2(bible->error().isEmpty(), qPrintable(bible->error()));
        QCOMPARE(bible->textSchema(), 4); QVERIFY(!versionRow(bible.get(),"study")["needsReimport"].toBool());
        QVERIFY(installedPath() != file); QVERIFY(!QFile::exists(file));
        QVERIFY(bible->navigate("John 3:16")); bible->selectVerses(16,16);
        QCOMPARE(bible->noteText(), QString("Kept across re-import"));
    }
    void crossReferencesComeFromTheSourceInOrder() {
        const QString path = fixtures.filePath("study_usfm.zip"); QVERIFY(zip(path, studyEntries()));
        install(path);
        QVERIFY(bible->hasCrossReferences());
        QVERIFY(bible->crossReferences().isEmpty()); // No verse selected.
        QVERIFY(bible->navigate("Genesis 1:1"));
        // The source lists John 1:1-3 twice ("Jn" and "John"): one row.
        auto rows = bible->crossReferences();
        QCOMPARE(rows.size(), 1);
        auto row = rows[0].toMap();
        QCOMPARE(row["reference"].toString(), QString("John 1:1\u20133"));
        QVERIFY(!row.contains("votes")); QVERIFY(!row.contains("openBible"));
        QCOMPARE(row["book"].toInt(), 43); QCOMPARE(row["start"].toInt(), 1); QCOMPARE(row["end"].toInt(), 3);
        QVERIFY(row["excerpt"].toString().startsWith("Word. Same."));
        QCOMPARE(row["versionId"].toString(), QString("study"));
        // Source order, not canonical order: a bare number continues the
        // previous chapter; unknown books are skipped.
        QVERIFY(bible->navigate("Genesis 1:2"));
        QStringList references;
        for (const auto &value : bible->crossReferences()) references << value.toMap()["reference"].toString();
        QCOMPARE(references, QStringList({"John 1:3", "Matthew 1:2\u20133", "Matthew 1:1"}));
        // A range unions its verses in verse order; the passage itself is never listed.
        QVERIFY(bible->navigate("Genesis 1:1-2"));
        references.clear();
        for (const auto &value : bible->crossReferences()) references << value.toMap()["reference"].toString();
        QCOMPARE(references, QStringList({"John 1:1\u20133", "John 1:3", "Matthew 1:2\u20133", "Matthew 1:1"}));
        // Verses without \\x have none: nothing is bundled.
        QVERIFY(bible->navigate("John 3:16")); QVERIFY(bible->crossReferences().isEmpty());
        // Another version's scope is not answered from the active text.
        auto scope = bible->tagScope(); scope["versionId"] = "elsewhere";
        QVERIFY(bible->crossReferences(scope).isEmpty());
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
            // OJB's own \\x "Ps 33:6,9" lists both verses.
            QVERIFY(bible->navigate("Genesis 1:3"));
            bool psalm6 = false, psalm9 = false;
            for (const auto &value : bible->crossReferences()) {
                const auto row = value.toMap();
                if (row["book"].toInt() == 19 && row["chapter"].toInt() == 33 && row["start"].toInt() == 6) psalm6 = true;
                if (row["book"].toInt() == 19 && row["chapter"].toInt() == 33 && row["start"].toInt() == 9) psalm9 = true;
            }
            QVERIFY(psalm6); QVERIFY(psalm9);
        }
        if (id == "eng-kjv2006") {
            QVERIFY(bible->navigate("Matthew 1")); QVERIFY(!bible->footnotes().isEmpty() || bible->hasWordsOfJesus());
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
