#include "testenvironment.h"
#include <QtTest/QTest>
#include "biblebackend.h"
#include <QFile>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <archive.h>
#include <archive_entry.h>

class RangesTest : public QObject
{
    Q_OBJECT
    QTemporaryDir fixtures;
    std::unique_ptr<QTemporaryDir> session;
    std::unique_ptr<BibleBackend> bible;
    QString net, kjv;
    static bool fixture(const QString &path, const QString &id) {
        archive *a = archive_write_new();
        const auto guard = qScopeGuard([&] { archive_write_free(a); });
        if (archive_write_set_format_zip(a) != ARCHIVE_OK || archive_write_open_filename(a,QFile::encodeName(path)) != ARCHIVE_OK) return false;
        // Deliberately noncanonical source order; omitted verses and chapter 2.
        const QStringList codes{"ESG","GEN","TOB"};
        for (int i = 0; i < codes.size(); ++i) {
            const QByteArray filename = QString("%1-%2%3.usfm").arg(i+1,2,10,QChar('0')).arg(codes[i],id).toUtf8();
            QString text = "\\id " + codes[i] + " Original Range Test Bible\n";
            for (int c : {1,3}) {
                text += QString("\\c %1\n\\p\n").arg(c);
                for (int v : {1,3,4,6}) text += QString("\\v %1 Original %2 %3:%1 edition %4 words.\n").arg(v).arg(codes[i]).arg(c).arg(id);
            }
            const QByteArray bytes = text.toUtf8();
            archive_entry *entry = archive_entry_new();
            const auto entryGuard = qScopeGuard([&] { archive_entry_free(entry); });
            archive_entry_set_pathname(entry,filename); archive_entry_set_size(entry,bytes.size());
            archive_entry_set_filetype(entry,AE_IFREG); archive_entry_set_perm(entry,0600);
            if (archive_write_header(a,entry) != ARCHIVE_OK || archive_write_data(a,bytes.data(),bytes.size()) != bytes.size()) return false;
        }
        return archive_write_close(a) == ARCHIVE_OK;
    }
    void install(const QString &path) {
        bible->importUsfm(QUrl::fromLocalFile(path));
        QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(),10000);
        QVERIFY2(bible->ready() && bible->error().isEmpty(),qPrintable(bible->error()));
    }
    QVariantMap scope(const QString &reference) {
        if (!bible->navigate(reference)) { QTest::qFail(qPrintable(bible->error()),__FILE__,__LINE__); return {}; }
        return bible->tagScope();
    }
    QSqlDatabase store() const {
        for (const auto &name : QSqlDatabase::connectionNames()) {
            auto db = QSqlDatabase::database(name);
            if (db.databaseName() == bible->dataDirectory()+"/personal.sqlite") return db;
        }
        return {};
    }
    void seed(const QVariantMap &s, bool highlight, const QString &note = {}) {
        // Seed exact overlapping highlights without toggleHighlight's coverage subtraction.
        QSqlQuery q(store());
        q.prepare("INSERT INTO annotations(version_id,book,chapter,start,endBook,endChapter,end,highlight,note) VALUES(?,?,?,?,?,?,?,?,?)");
        for (const auto &key : {"versionId","book","chapter","start","endBook","endChapter","end"}) q.addBindValue(s.value(key));
        q.addBindValue(highlight); q.addBindValue(note.isNull() ? QString("") : note);
        QVERIFY2(q.exec(),qPrintable(q.lastError().text()));
        bible->refreshCollection();
    }
    static QVariantMap components(bool highlight = true, bool note = true, QVariantList tags = {}) {
        return {{"highlight",highlight},{"note",note},{"tags",tags}};
    }
    static QVariantList tagIds(const QVariantMap &snapshot) {
        QVariantList ids;
        for (const auto &tag : snapshot.value("tags").toList()) ids.append(tag.toMap().value("id"));
        return ids;
    }
private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("OmaWordTests");
        QCoreApplication::setApplicationName("omaword-ranges-test");
        QVERIFY(fixtures.isValid());
        net = fixtures.filePath("engnet_usfm.zip"); kjv = fixtures.filePath("eng-kjv_usfm.zip");
        QVERIFY(fixture(net,"engnet")); QVERIFY(fixture(kjv,"eng-kjv"));
    }
    void init() {
        session = std::make_unique<QTemporaryDir>(); QVERIFY(session->isValid());
        qputenv("XDG_DATA_HOME",session->path().toUtf8());
        bible = std::make_unique<BibleBackend>();
        install(net);
    }
    void cleanup() { bible.reset(); session.reset(); }
    void exactSnapshotsAcrossBooks() {
        const auto first = scope("ESG 3:6");
        bible->moveSelection(2,true); const auto cross = bible->tagScope();
        QCOMPARE(cross.value("endBook").toInt(),1);
        QVERIFY(cross.value("book").toInt() > cross.value("endBook").toInt());
        seed(cross,false,"cross-book note");
        const auto before = bible->tagScope(); const auto verses = bible->verses();
        QSignalSpy navigation(bible.get(),&BibleBackend::navigationChanged), errors(bible.get(),&BibleBackend::errorChanged);
        // Identity is the endpoints: a display reference or order changes nothing.
        auto fakeDisplay = cross; fakeDisplay["reference"] = "TOB 1:1"; fakeDisplay["_order"] = -100;
        QCOMPARE(bible->annotationSnapshot(fakeDisplay),bible->annotationSnapshot(cross));
        QCOMPARE(bible->annotationSnapshot(cross).value("note").toString(),QString("cross-book note"));
        QVERIFY(!bible->annotationSnapshot(first).value("ok").toBool()); // coverage is not an exact assignment
        QCOMPARE(bible->tagScope(),before); QCOMPARE(bible->verses(),verses);
        QCOMPARE(navigation.count(),0); QCOMPARE(errors.count(),0);
    }
    void moveAll_data() {
        QTest::addColumn<QString>("from"); QTest::addColumn<QString>("to");
        QTest::newRow("expand") << "GEN 1:3-4" << "GEN 1:1-6";
        QTest::newRow("shrink") << "GEN 1:1-6" << "GEN 1:3-4";
        QTest::newRow("overlap") << "GEN 1:1-4" << "GEN 1:3-6";
        QTest::newRow("disjoint") << "GEN 1:1-3" << "TOB 3:4-6";
        QTest::newRow("cross-book") << "GEN 1:1-3" << "ESG 3:6–GEN 1:3";
        QTest::newRow("cross-source") << "ESG 3:6–GEN 1:3" << "TOB 3:4-6";
    }
    void moveAll() {
        QFETCH(QString,from); QFETCH(QString,to);
        const auto s = scope(from), d = scope(to);
        seed(s,true,"Moved note");
        const QString tag = bible->createAndAssignTag(s,"Move me"); QVERIFY(!tag.isEmpty());
        const auto snapshot = bible->annotationSnapshot(s);
        QVERIFY(snapshot.value("ok").toBool());
        const auto choice = components(true,true,{tag});
        BibleBackend peer; QVERIFY(peer.openTaggedPassage(s));
        const auto before = bible->tagScope(), peerBefore = peer.tagScope();
        bible->setReadingPosition(0.42);
        QSignalSpy navigation(bible.get(),&BibleBackend::navigationChanged), peerNavigation(&peer,&BibleBackend::navigationChanged);
        QSignalSpy local(bible.get(),&BibleBackend::tagsChanged), remote(&peer,&BibleBackend::tagsChanged);
        const auto preview = bible->previewRangeChange(snapshot,d,choice);
        QVERIFY2(preview.value("ok").toBool(),qPrintable(preview.value("error").toString()));
        QVERIFY(!preview.value("needsNoteMerge").toBool());
        QCOMPARE(bible->annotationSnapshot(s),snapshot); QCOMPARE(local.count(),0);
        QVERIFY2(bible->changeAnnotationRange(snapshot,d,choice),qPrintable(bible->error()));
        QVERIFY(!bible->annotationSnapshot(s).value("ok").toBool());
        const auto dest = bible->annotationSnapshot(d);
        for (const auto &key : {"highlight","note","tags"}) QCOMPARE(dest.value(key),snapshot.value(key));
        QCOMPARE(peer.annotationSnapshot(d),dest); QCOMPARE(peer.collection(),bible->collection());
        QVERIFY(local.count() > 0); QVERIFY(remote.count() > 0);
        QVERIFY(peer.noteText().isEmpty()); QVERIFY(peer.selectionTags().isEmpty());
        QCOMPARE(bible->tagScope(),before); QCOMPARE(peer.tagScope(),peerBefore);
        QCOMPARE(bible->readingPosition(),0.42); QCOMPARE(navigation.count(),0); QCOMPARE(peerNavigation.count(),0);
        // One undo step puts both exact ranges back, in every window; redo moves them again.
        QCOMPARE(bible->undoLabel(),"range move to " + d.value("reference").toString());
        QVERIFY2(bible->undo(),qPrintable(bible->error()));
        QCOMPARE(bible->annotationSnapshot(s),snapshot); QCOMPARE(peer.annotationSnapshot(s),snapshot);
        QVERIFY(!bible->annotationSnapshot(d).value("ok").toBool());
        QVERIFY2(bible->redo(),qPrintable(bible->error())); QCOMPARE(bible->annotationSnapshot(d),dest);
        bible.reset(); bible = std::make_unique<BibleBackend>();
        QCOMPARE(bible->annotationSnapshot(d),dest);
    }
    void subsetOverlapAndDestinationUnion() {
        const auto s = scope("GEN 1:1-4"), d = scope("GEN 1:3-6"), overlap = scope("GEN 1:1-6");
        seed(s,true,"source stays"); seed(d,false,"destination stays"); seed(overlap,true,"overlap stays");
        const QString move = bible->createAndAssignTag(s,"Move"), stay = bible->createAndAssignTag(s,"Stay");
        QVERIFY(!move.isEmpty() && !stay.isEmpty()); QVERIFY(bible->setTag(d,move,true));
        const auto snapshot = bible->annotationSnapshot(s), other = bible->annotationSnapshot(overlap);
        const auto choice = components(false,false,{move,move});
        QVERIFY2(bible->changeAnnotationRange(snapshot,d,choice),qPrintable(bible->error()));
        const auto source = bible->annotationSnapshot(s), target = bible->annotationSnapshot(d);
        QVERIFY(source.value("highlight").toBool());
        QCOMPARE(source.value("note").toString(),QString("source stays")); QCOMPARE(tagIds(source),QVariantList{stay});
        QVERIFY(!target.value("highlight").toBool());
        QCOMPARE(target.value("note").toString(),QString("destination stays")); QCOMPARE(tagIds(target),QVariantList{move});
        QCOMPARE(bible->annotationSnapshot(overlap),other);
        QVERIFY(bible->changeAnnotationRange(source,d,components(true,false)));
        QVERIFY(!bible->annotationSnapshot(s).value("highlight").toBool()); QVERIFY(bible->annotationSnapshot(d).value("highlight").toBool());
        QVERIFY(bible->openTaggedPassage(s)); QVERIFY(bible->selectionHighlighted()); // independent overlap remains
        QCOMPARE(bible->annotationSnapshot(overlap),other);
    }
    void tagsOnlyAndExactValidation() {
        const auto s = scope("GEN 1:1-4"), d = scope("TOB 1:1");
        const QString a = bible->createAndAssignTag(s,"A"), b = bible->createAndAssignTag(s,"B"), c = bible->createAndAssignTag(d,"C");
        const auto snapshot = bible->annotationSnapshot(s);
        const auto before = bible->collection();
        for (const auto &bad : {components(true,false),components(false,false,{c}),components(false,false),
                                QVariantMap{{"tags",QVariantList{42}}},QVariantMap{{"highlight","true"}},QVariantMap{{"bookmark",true}},
                                QVariantMap{{"unknown",true}}}) {
            QVERIFY(!bible->previewRangeChange(snapshot,d,bad).value("ok").toBool());
            QVERIFY(!bible->changeAnnotationRange(snapshot,d,bad)); QCOMPARE(bible->collection(),before);
        }
        bible->clearError();
        QVERIFY(bible->changeAnnotationRange(snapshot,d,components(false,false,{a,b})));
        QVERIFY(!bible->annotationSnapshot(s).value("ok").toBool());
        QCOMPARE(tagIds(bible->annotationSnapshot(d)).size(),3);
        QSqlQuery q(store()); QVERIFY(q.exec("SELECT COUNT(*) FROM annotations")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),0);
    }
    void noteConfirmationAndDestinationDrift() {
        const auto s = scope("GEN 1:1"), d = scope("TOB 1:1");
        seed(s,true,"source text"); seed(d,true,"destination text");
        const auto snapshot = bible->annotationSnapshot(s), choice = components(false,true);
        bible->navigate("bogus reference"); const QString previousError = bible->error(); QVERIFY(!previousError.isEmpty());
        QSignalSpy errors(bible.get(),&BibleBackend::errorChanged);
        auto preview = bible->previewRangeChange(snapshot,d,choice);
        QVERIFY(preview.value("ok").toBool()); QVERIFY(preview.value("needsNoteMerge").toBool());
        QCOMPARE(preview.value("sourceNote").toString(),QString("source text"));
        QCOMPARE(preview.value("destinationNote").toString(),QString("destination text"));
        QCOMPARE(preview.value("combinedNote").toString(),QString("destination text\n\nsource text"));
        QVERIFY(preview.value("message").toString().contains("destination note first"));
        QCOMPARE(bible->error(),previousError); QCOMPARE(errors.count(),0);
        const auto before = bible->collection();
        QVERIFY(!bible->changeAnnotationRange(snapshot,d,choice));
        QVERIFY(!bible->changeAnnotationRange(snapshot,d,choice,true));
        QVERIFY(!bible->changeAnnotationRange(snapshot,d,choice,false,preview.value("destinationToken").toString()));
        QCOMPARE(bible->collection(),before);
        BibleBackend peer; QVERIFY(peer.openTaggedPassage(d)); peer.setNoteText("new destination");
        QVERIFY(!bible->changeAnnotationRange(snapshot,d,choice,true,preview.value("destinationToken").toString()));
        QCOMPARE(peer.noteText(),QString("new destination")); QCOMPARE(bible->annotationSnapshot(s),snapshot);
        preview = bible->previewRangeChange(snapshot,d,choice);
        const QString tag = peer.createAndAssignTag(d,"destination changed"); QVERIFY(!tag.isEmpty());
        QVERIFY(!bible->changeAnnotationRange(snapshot,d,choice,true,preview.value("destinationToken").toString()));
        preview = bible->previewRangeChange(snapshot,d,choice);
        QVERIFY(bible->changeAnnotationRange(snapshot,d,choice,true,preview.value("destinationToken").toString()));
        QCOMPARE(peer.noteText(),QString("new destination\n\nsource text"));
        QVERIFY(bible->annotationSnapshot(s).value("note").toString().isEmpty());
        QVERIFY(bible->annotationSnapshot(s).value("highlight").toBool());
        QVERIFY(bible->annotationSnapshot(d).value("highlight").toBool());
    }
    void identicalNotesAndSameRangeNoop() {
        const auto s = scope("GEN 1:1"), d = scope("TOB 1:1");
        seed(s,true,"same note"); seed(d,false,"same note");
        const QString tag = bible->createAndAssignTag(s,"same");
        const auto snapshot = bible->annotationSnapshot(s), choice = components(true,true,{tag});
        BibleBackend peer; int flushes = 0;
        peer.setFlushCallback([&] { ++flushes; return false; });
        QSignalSpy published(&peer,&BibleBackend::tagsChanged);
        const auto before = bible->collection();
        QSqlQuery q(store()); QVERIFY(q.exec("SELECT total_changes()")); QVERIFY(q.next()); const auto changes = q.value(0);
        QVERIFY(bible->changeAnnotationRange(snapshot,s,choice));
        QCOMPARE(flushes,0); QCOMPARE(published.count(),0); QCOMPARE(bible->collection(),before);
        QVERIFY(q.exec("SELECT total_changes()")); QVERIFY(q.next()); QCOMPARE(q.value(0),changes); q.finish();
        peer.setFlushCallback({});
        QVERIFY(!bible->previewRangeChange(snapshot,d,choice).value("needsNoteMerge").toBool());
        QVERIFY(bible->changeAnnotationRange(snapshot,d,choice));
        QCOMPARE(bible->annotationSnapshot(d).value("note").toString(),QString("same note"));
    }
    void sourceDrift_data() {
        QTest::addColumn<QString>("component");
        for (const auto &key : {"note","highlight","tag","rename","delete"}) QTest::newRow(key) << QString(key);
    }
    void sourceDrift() {
        QFETCH(QString,component);
        const auto s = scope("GEN 1:1"), d = scope("TOB 1:1"); seed(s,true,"source");
        const QString tag = bible->createAndAssignTag(s,"original");
        const auto snapshot = bible->annotationSnapshot(s);
        BibleBackend peer; QVERIFY(peer.openTaggedPassage(s));
        if (component == "note") peer.setNoteText("changed");
        else if (component == "highlight") QVERIFY(peer.toggleHighlight());
        else if (component == "tag") QVERIFY(peer.setTag(s,tag,false));
        else if (component == "rename") QVERIFY(peer.renameTag(tag,"renamed"));
        else QVERIFY(peer.deleteAnnotationScope(s));
        const auto before = bible->collection();
        QVERIFY(!bible->previewRangeChange(snapshot,d,components(false,true)).value("ok").toBool());
        QVERIFY(!bible->changeAnnotationRange(snapshot,d,components(false,true)));
        QVERIFY(bible->error().contains("source annotation changed")); QCOMPARE(bible->collection(),before);
    }
    void draftsFlushBeforeValidation() {
        const auto s = scope("GEN 1:1"), d = scope("TOB 1:1"), unrelated = scope("ESG 1:1");
        seed(s,false,"old source"); seed(d,false,"old destination"); seed(unrelated,false,"unrelated");
        auto snapshot = bible->annotationSnapshot(s); const auto choice = components(false,true);
        BibleBackend peer, second; QVERIFY(peer.openTaggedPassage(s)); QVERIFY(second.openTaggedPassage(unrelated));
        int flushed = 0;
        peer.setFlushCallback([&] { ++flushed; peer.setNoteText("draft source"); return peer.error().isEmpty(); });
        second.setFlushCallback([&] { ++flushed; second.setNoteText("other draft"); return second.error().isEmpty(); });
        QVERIFY(!bible->changeAnnotationRange(snapshot,d,choice)); QCOMPARE(flushed,2);
        QCOMPARE(peer.noteText(),QString("draft source")); QCOMPARE(second.noteText(),QString("other draft"));
        QCOMPARE(bible->annotationSnapshot(d).value("note").toString(),QString("old destination"));
        snapshot = bible->annotationSnapshot(s);
        QVERIFY(peer.openTaggedPassage(d)); // also flushes callbacks; reset the draft target below
        auto preview = bible->previewRangeChange(snapshot,d,choice);
        peer.setFlushCallback([&] { peer.setNoteText("draft destination"); return peer.error().isEmpty(); });
        QVERIFY(!bible->changeAnnotationRange(snapshot,d,choice,true,preview.value("destinationToken").toString()));
        QCOMPARE(bible->annotationSnapshot(s),snapshot); QCOMPARE(peer.noteText(),QString("draft destination"));
        peer.setFlushCallback([] { return false; });
        QVERIFY(!bible->changeAnnotationRange(snapshot,d,choice)); QVERIFY(bible->error().contains("unsaved note"));
    }
    void invalidScopesSnapshotsAndVersions() {
        const auto s = scope("GEN 1:1"), d = scope("TOB 1:1"); seed(s,true,"source");
        const auto snapshot = bible->annotationSnapshot(s), choice = components();
        for (const auto &key : {"book","chapter","start","endBook","endChapter","end"}) {
            auto bad = d; bad[key] = 2; // absent book/chapter/verse
            QVERIFY(!bible->previewRangeChange(snapshot,bad,choice).value("ok").toBool());
            bad = d; bad[key] = "1";
            QVERIFY(!bible->previewRangeChange(snapshot,bad,choice).value("ok").toBool());
        }
        auto forged = snapshot; forged["note"] = "forged";
        QVERIFY(!bible->changeAnnotationRange(forged,d,choice));
        forged = snapshot; forged["token"] = QString(10000,'x');
        QVERIFY(!bible->previewRangeChange(forged,d,choice).value("ok").toBool());
        auto bad = d; bad["versionId"] = QString(10000,'x');
        QVERIFY(!bible->previewRangeChange(snapshot,bad,choice).value("ok").toBool());
        bible->clearError(); install(kjv);
        const auto other = scope("GEN 1:1");
        QVERIFY(!bible->changeAnnotationRange(snapshot,other,choice));
        QVERIFY(bible->switchVersion("engnet"));
        QVERIFY(!bible->changeAnnotationRange(snapshot,other,choice));
        bible->clearError(); install(net); // same edition ID, new installed generation
        QVERIFY(!bible->changeAnnotationRange(snapshot,d,choice));
        const auto fresh = bible->annotationSnapshot(s); QVERIFY(fresh.value("ok").toBool());
        BibleBackend peer; QVERIFY(peer.removeVersion("engnet",false));
        QVERIFY(!bible->changeAnnotationRange(fresh,d,choice));
    }
    void rollback_data() {
        QTest::addColumn<QString>("trigger"); QTest::addColumn<bool>("commitFailure");
        QTest::newRow("delete") << "CREATE TRIGGER fail BEFORE DELETE ON annotations BEGIN SELECT RAISE(ABORT,'delete failed'); END" << false;
        QTest::newRow("insert") << "CREATE TRIGGER fail BEFORE INSERT ON annotations BEGIN SELECT RAISE(ABORT,'insert failed'); END" << false;
        QTest::newRow("ignored-insert") << "CREATE TRIGGER fail BEFORE INSERT ON annotations BEGIN SELECT RAISE(IGNORE); END" << false;
        QTest::newRow("tag-delete") << "CREATE TRIGGER fail BEFORE DELETE ON tag_assignments BEGIN SELECT RAISE(ABORT,'tag delete failed'); END" << false;
        QTest::newRow("tag-insert") << "CREATE TRIGGER fail BEFORE INSERT ON tag_assignments BEGIN SELECT RAISE(ABORT,'tag insert failed'); END" << false;
        QTest::newRow("ignored-tag") << "CREATE TRIGGER fail BEFORE INSERT ON tag_assignments BEGIN SELECT RAISE(IGNORE); END" << false;
        QTest::newRow("commit") << "CREATE TRIGGER fail AFTER INSERT ON tag_assignments BEGIN INSERT INTO failure VALUES(1); END" << true;
    }
    void rollback() {
        QFETCH(QString,trigger); QFETCH(bool,commitFailure);
        const auto s = scope("GEN 1:1"), d = scope("TOB 1:1"); seed(s,true,"source");
        const QString tag = bible->createAndAssignTag(s,"rollback");
        const auto snapshot = bible->annotationSnapshot(s), choice = components(true,true,{tag});
        QSqlQuery q(store()); // writer connection, before opening another reader
        if (commitFailure) {
            QVERIFY(q.exec("PRAGMA foreign_keys=ON")); QVERIFY(q.exec("CREATE TABLE parent(id INTEGER PRIMARY KEY)"));
            QVERIFY(q.exec("CREATE TABLE failure(id INTEGER REFERENCES parent(id) DEFERRABLE INITIALLY DEFERRED)"));
        }
        BibleBackend peer; const auto before = bible->collection(), tags = bible->tags();
        QSignalSpy local(bible.get(),&BibleBackend::tagsChanged), remote(&peer,&BibleBackend::tagsChanged);
        QSignalSpy annotations(&peer,&BibleBackend::annotationChanged);
        QVERIFY2(q.exec(trigger),qPrintable(q.lastError().text()));
        QVERIFY(!bible->changeAnnotationRange(snapshot,d,choice)); QVERIFY(!bible->error().isEmpty());
        if (commitFailure) QVERIFY(bible->error().contains("Cannot commit range change"));
        QCOMPARE(bible->annotationSnapshot(s),snapshot); QVERIFY(!bible->annotationSnapshot(d).value("ok").toBool());
        QCOMPARE(bible->collection(),before); QCOMPARE(peer.collection(),before); QCOMPARE(bible->tags(),tags);
        QCOMPARE(local.count(),0); QCOMPARE(remote.count(),0); QCOMPARE(annotations.count(),0);
        QVERIFY(q.exec("DROP TRIGGER fail"));
        QVERIFY2(bible->changeAnnotationRange(snapshot,d,choice),qPrintable(bible->error()));
    }
};

QTEST_MAIN(RangesTest)
#include "tst_ranges.moc"
