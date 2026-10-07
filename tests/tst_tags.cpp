#include "testenvironment.h"
#include <QtTest/QTest>
#include "biblebackend.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <archive.h>
#include <archive_entry.h>

class TagsTest : public QObject
{
    Q_OBJECT
    QTemporaryDir fixtures;
    std::unique_ptr<QTemporaryDir> session;
    std::unique_ptr<BibleBackend> bible;
    QString net, kjv;
    static bool write(const QString &path, const QByteArray &bytes) {
        QSaveFile f(path); return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size() && f.commit();
    }
    static bool fixture(const QString &path, const QString &id,
                        const QStringList &codes = {"GEN","MAL","TOB","MAT"},
                        int verseCount = 4, bool ambiguousNames = false) {
        archive *a = archive_write_new();
        const auto guard = qScopeGuard([&] { archive_write_free(a); });
        if (archive_write_set_format_zip(a) != ARCHIVE_OK || archive_write_open_filename(a,QFile::encodeName(path)) != ARCHIVE_OK) return false;
        for (int i = 0; i < codes.size(); ++i) {
            const QByteArray filename = QString("%1-%2%3.usfm").arg(i+1,2,10,QChar('0')).arg(codes[i],id).toUtf8();
            QString text = "\\id " + codes[i] + " Original Tag Test Bible\n";
            if (ambiguousNames && codes[i] == "GEN") text += "\\toc2 John\n";
            for (int c = 1; c <= 2; ++c) {
                text += QString("\\c %1\n\\p\n").arg(c);
                for (int v = 1; v <= verseCount; ++v) text += QString("\\v %1 Original %2 %3:%1 edition %4 words.\n").arg(v).arg(codes[i]).arg(c).arg(id);
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
    QSqlDatabase store() const {
        for (const auto &name : QSqlDatabase::connectionNames()) {
            auto db = QSqlDatabase::database(name);
            if (db.databaseName() == bible->dataDirectory()+"/personal.sqlite") return db;
        }
        return {};
    }
    static QJsonObject json(const QString &path) {
        QFile f(path); if (!f.open(QIODevice::ReadOnly)) return {};
        return QJsonDocument::fromJson(f.readAll()).object();
    }
    int count(const QString &id) const {
        for (const auto &v : bible->tags()) if (v.toMap().value("id").toString() == id) return v.toMap().value("count").toInt();
        return -1;
    }
private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("OmaWordTests");
        QCoreApplication::setApplicationName("omaword-tags-test");
        QVERIFY(fixtures.isValid());
        net = fixtures.filePath("engnet_usfm.zip"); kjv = fixtures.filePath("eng-kjv_usfm.zip");
        QVERIFY(fixture(net,"engnet")); QVERIFY(fixture(kjv,"eng-kjv"));
    }
    void init() {
        session = std::make_unique<QTemporaryDir>(); QVERIFY(session->isValid());
        qputenv("XDG_DATA_HOME",session->path().toUtf8());
        bible = std::make_unique<BibleBackend>(); QVERIFY2(bible->error().isEmpty(),qPrintable(bible->error()));
    }
    void cleanup() { bible.reset(); session.reset(); }
    void namesAndDurability() {
        QVERIFY(!bible->toggleHighlight());
        const QString name = QString::fromUtf8("  Café / [Hope] ✨ 👩‍💻  ");
        const QString id = bible->createTag(name); QVERIFY2(!id.isEmpty(),qPrintable(bible->error()));
        QCOMPARE(bible->createTag(QString::fromUtf8("CAFÉ / [HOPE] ✨ 👩‍💻")),id);
        QCOMPARE(bible->tags().first().toMap().value("name").toString(),name.trimmed().normalized(QString::NormalizationForm_C));
        const QString sharp = bible->createTag(QString::fromUtf8("Straße"));
        QCOMPARE(bible->createTag("STRASSE"),sharp);
        QCOMPARE(bible->createTag(QString::fromUtf8("STRAẞE")),sharp);
        QCOMPARE(bible->createTag(QString::fromUtf8("ﬃ")),bible->createTag("FFI"));
        QCOMPARE(bible->createTag(QString::fromUtf8("ᾈ")),bible->createTag(QString::fromUtf8("ἀι")));
        QCOMPARE(bible->createTag(QString::fromUtf8("İ")),bible->createTag(QString::fromUtf8("i̇")));
        for (const auto &bad : QStringList{"", "  ", "\tbad", "bad\n", "x\ry", QString(QChar(0x7f)), QString(QChar(0x85)), QString(QChar(0x2028))})
            QVERIFY2(bible->createTag(bad).isEmpty(),qPrintable(bad));
        const QString internal = bible->createTag("two  spaces"); QVERIFY(!internal.isEmpty());
        QVERIFY(internal != bible->createTag("two spaces"));
        QVERIFY(!bible->renameTag(internal,"STRASSE"));
        QVERIFY(bible->renameTag(id,"Renamed | <tag>"));
        bible.reset(); bible = std::make_unique<BibleBackend>();
        QCOMPARE(bible->createTag("renamed | <TAG>"),id); QCOMPARE(count(id),0);
        BibleBackend emptyPeer; QSignalSpy changed(&emptyPeer,&BibleBackend::tagsChanged);
        QVERIFY(bible->renameTag(id,"Every window")); QCOMPARE(changed.count(),1);
        QCOMPARE(emptyPeer.tags(),bible->tags());
    }
    void exactOverlapSavedAndIndicators() {
        install(net); QVERIFY(bible->navigate("GEN 1:1-3"));
        const auto scope = bible->tagScope();
        const auto id = bible->createAndAssignTag(scope,"Hope"); QVERIFY2(!id.isEmpty(),qPrintable(bible->error()));
        QCOMPARE(bible->selectionTags().size(),1); QCOMPARE(bible->collection().size(),1);
        QVERIFY(!bible->collection().first().toMap().value("highlight").toBool());
        QCOMPARE(bible->collection().first().toMap().value("versionId").toString(),bible->versionId());
        QVERIFY(bible->setTag(scope,id,true)); QCOMPARE(count(id),1);
        QVERIFY(bible->navigate("GEN 1:2")); QVERIFY(bible->selectionTags().isEmpty());
        QCOMPARE(bible->overlappingTags().size(),1);
        auto verse = bible->verses()[1].toMap(); QVERIFY(verse.value("hasTags").toBool());
        auto rail = verse.value("tagRanges").toList().first().toMap();
        QCOMPARE(rail.value("reference"),scope.value("reference")); QVERIFY(rail.value("continuesBefore").toBool());
        QVERIFY(rail.value("continuesAfter").toBool()); QVERIFY(!rail.value("tagStart").toBool());
        QVERIFY(bible->verses()[0].toMap().value("tagRanges").toList().first().toMap().value("tagStart").toBool());
        QVERIFY(!bible->verses()[3].toMap().value("hasTags").toBool());
        bible->setNoteText("Keep overlapping note"); QVERIFY(bible->toggleHighlight());
        QVERIFY(bible->openTaggedPassage(scope)); QVERIFY(bible->toggleHighlight()); bible->setNoteText("Exact note");
        QCOMPARE(bible->collection().size(),2);
        QVERIFY(bible->deleteAnnotation(scope.value("reference").toString(),bible->versionId()));
        QCOMPARE(count(id),0); QCOMPARE(bible->collection().size(),1);
        QVERIFY(bible->navigate("GEN 1:2")); QCOMPARE(bible->noteText(),QString("Keep overlapping note"));
        QVERIFY(bible->selectionHighlighted());
        QVERIFY(bible->setTag(scope,id,true));
        QVERIFY(bible->deleteAnnotation(scope.value("reference").toString())); // tags only
        QCOMPARE(count(id),0);
        auto bad = scope; bad["end"] = 99; QVERIFY(!bible->setTag(bad,id,true));
        bad = scope; bad["start"] = "1"; QVERIFY(!bible->setTag(bad,id,true));
        bad = scope; bad["book"] = 40; QVERIFY(!bible->openTaggedPassage(bad));
        bible->clearSelection(); QVERIFY(bible->tagScope().isEmpty()); QVERIFY(!bible->toggleHighlight());
    }
    void sourceOrderVersionsRelatedAndPeers() {
        install(net); QVERIFY(bible->navigate("TOB 2:4")); bible->moveSelection(2,true);
        const auto cross = bible->tagScope(); QCOMPARE(cross.value("book").toInt(),67); QCOMPARE(cross.value("endBook").toInt(),40);
        const QString hope = bible->createAndAssignTag(cross,"Hope"); QVERIFY(!hope.isEmpty());
        const QString love = bible->createAndAssignTag(cross,"Love"); QVERIFY(!love.isEmpty());
        QVERIFY(bible->navigate("MAT 1:2")); const auto overlap = bible->tagScope(); QVERIFY(bible->setTag(overlap,hope,true));
        QVERIFY(bible->navigate("MAL 1:1")); const auto early = bible->tagScope(); QVERIFY(bible->setTag(early,hope,true));
        BibleBackend peer; QVERIFY(peer.ready()); QVERIFY(peer.navigate("GEN 2:3"));
        const auto peerScope = peer.tagScope();
        install(kjv); QVERIFY(bible->navigate("TOB 1:1")); const auto other = bible->tagScope();
        QVERIFY(bible->setTag(other,hope,true));
        QSignalSpy changed(&peer,&BibleBackend::tagsChanged);
        QVERIFY(bible->renameTag(hope,"HOPE!")); QVERIFY(changed.count() > 0);
        QCOMPARE(peer.tagScope(),peerScope);
        const auto before = bible->tagScope(); const auto rowsBefore = bible->verses();
        QSignalSpy navigation(bible.get(),&BibleBackend::navigationChanged);
        auto rows = bible->taggedPassages({hope,love}); QCOMPARE(rows.size(),4);
        QCOMPARE(rows[0].toMap().value("sharedCount").toInt(),2);
        QCOMPARE(rows[0].toMap().value("reference"),cross.value("reference"));
        QCOMPARE(rows[1].toMap().value("book").toInt(),39); QCOMPARE(rows[2].toMap().value("book").toInt(),67);
        QCOMPARE(rows[3].toMap().value("book").toInt(),40);
        QVERIFY(rows[0].toMap().value("excerpt").toString().contains("TOB"));
        QVERIFY(rows[0].toMap().value("excerpt").toString().contains("MAT"));
        QCOMPARE(bible->taggedPassages({hope,love},true).size(),1);
        QVERIFY(bible->taggedPassages({}).isEmpty()); QVERIFY(bible->taggedPassages({hope,"missing"},true).isEmpty());
        QCOMPARE(bible->relatedPassages(cross,{hope},false,false).size(),1);
        QCOMPARE(bible->relatedPassages(cross,{hope},false,true).size(),2);
        QCOMPARE(bible->tagScope(),before); QCOMPARE(bible->verses(),rowsBefore); QCOMPARE(navigation.count(),0);
        auto misleading = cross; misleading["reference"] = "Genesis 1:1";
        QVERIFY(bible->openTaggedPassage(misleading)); QCOMPARE(bible->tagScope(),cross);
        QCOMPARE(peer.tagScope(),peerScope);
        QVERIFY(bible->verses()[0].toMap().value("hasTags").toBool());
        auto rail = bible->verses()[0].toMap().value("tagRanges").toList().first().toMap();
        QVERIFY(rail.value("continuesBefore").toBool());
        QVERIFY(bible->mergeTags(love,hope)); QCOMPARE(count(love),-1); QCOMPARE(count(hope),4);
        QCOMPARE(peer.tagsForScope(cross).size(),1);
        QVERIFY(peer.setTag(cross,hope,false)); QVERIFY(bible->selectionTags().isEmpty());
        QCOMPARE(count(hope),3);
        QVERIFY(bible->deleteTag(hope)); QVERIFY(peer.tags().isEmpty()); QVERIFY(bible->collection().isEmpty());
    }
    void removalKeepDeleteAndStaleScopes() {
        install(net); QVERIFY(bible->navigate("GEN 1:2")); const auto scope = bible->tagScope();
        const QString id = bible->createAndAssignTag(scope,"Retained"); QVERIFY(!id.isEmpty());
        bible->setNoteText("Personal note"); bible->setFontSize(23);
        install(kjv); QVERIFY(bible->setTag(scope,id,true)); // frozen scope writes original version
        QCOMPARE(bible->tagsForScope(scope).size(),1); QVERIFY(bible->selectionTags().isEmpty());
        QVERIFY(bible->removeVersion("engnet",false)); QCOMPARE(count(id),0);
        QVERIFY(!bible->setTag(scope,id,true)); QVERIFY(!bible->openTaggedPassage(scope));
        QVERIFY(bible->taggedPassages({id}).isEmpty());
        install(net); QCOMPARE(count(id),1); QVERIFY(bible->openTaggedPassage(scope)); QCOMPARE(bible->noteText(),QString("Personal note"));
        QVERIFY(bible->removeVersion("engnet",true)); QCOMPARE(count(id),0);
        install(net); QVERIFY(bible->tagsForScope(scope).isEmpty()); QVERIFY(bible->openTaggedPassage(scope));
        QVERIFY(bible->noteText().isEmpty()); QCOMPARE(bible->fontSize(),23); QCOMPARE(count(id),0);
    }
    void backupsScopedLegacyAndMarkdown() {
        install(net); QVERIFY(bible->navigate("GEN 1:1-2")); const auto scope = bible->tagScope();
        const QString id = bible->createAndAssignTag(scope,"[Hope] <&> | *"); QVERIFY(!id.isEmpty());
        bible->setNoteText("Saved note"); QVERIFY(bible->toggleHighlight());
        const QString unused = bible->createTag("Unused"); QVERIFY(!unused.isEmpty());
        const auto backup = QUrl::fromLocalFile(session->filePath("backup.json")); QVERIFY(bible->exportBackup(backup));
        auto root = json(backup.toLocalFile()); QCOMPARE(root.value("version").toInt(),5);
        QCOMPARE(root.value("tags").toArray().size(),1); QCOMPARE(root.value("tagAssignments").toArray().size(),1);
        auto legacy = root; legacy["version"] = 3; legacy.remove("tags"); legacy.remove("tagAssignments"); legacy.remove("desk");
        const auto old = QUrl::fromLocalFile(session->filePath("legacy.json")); QVERIFY(write(old.toLocalFile(),QJsonDocument(legacy).toJson()));
        install(kjv); QVERIFY(bible->navigate("TOB 1:3")); const auto other = bible->tagScope();
        QVERIFY(bible->setTag(other,id,true)); QVERIFY(bible->renameTag(id,"Renamed elsewhere"));
        QVERIFY(bible->openTaggedPassage(scope)); bible->setNoteText("Changed");
        const QString extra = bible->createAndAssignTag(scope,"Extra"); QVERIFY(!extra.isEmpty());
        BibleBackend peer;
        QVERIFY(bible->importBackup(old)); QCOMPARE(bible->tagsForScope(scope).size(),2); QCOMPARE(bible->noteText(),QString("Saved note"));
        QVERIFY2(bible->importBackup(backup),qPrintable(bible->error())); QCOMPARE(bible->tagsForScope(scope).size(),1);
        const auto restored = bible->tagsForScope(scope).first().toMap(); QCOMPARE(restored.value("name").toString(),QString("[Hope] <&> | *"));
        QVERIFY(restored.value("id").toString() != id);
        QCOMPARE(bible->tagsForScope(other).first().toMap().value("name").toString(),QString("Renamed elsewhere"));
        QCOMPARE(count(extra),0); QCOMPARE(count(unused),0); QCOMPARE(peer.selectionTags(),bible->selectionTags());
        QVERIFY(bible->exportNotes(QUrl::fromLocalFile(session->path())));
        QFile md(session->filePath("omaword-notes.md")); QVERIFY(md.open(QIODevice::ReadOnly)); const auto text = md.readAll();
        QVERIFY(text.contains("Tags: \\[Hope\\] &lt;&amp;&gt; \\| \\*"));
        QVERIFY(text.contains("Saved note"));
        auto invalid = root; auto assignments = invalid.value("tagAssignments").toArray(); assignments.append(assignments.first()); invalid["tagAssignments"] = assignments;
        const auto bad = QUrl::fromLocalFile(session->filePath("bad.json")); QVERIFY(write(bad.toLocalFile(),QJsonDocument(invalid).toJson()));
        const auto collection = bible->collection(); QVERIFY(!bible->importBackup(bad)); QCOMPARE(bible->collection(),collection);
        QVERIFY(!bible->exportBackup(QUrl::fromLocalFile(bible->dataDirectory()+"/personal.sqlite")));
    }
    void legacyBookmarksRestoreAsTheBookmarkedTag() {
        install(net); QVERIFY(bible->navigate("GEN 1:1-2")); const auto noted = bible->tagScope();
        bible->setNoteText("Noted");
        QVERIFY(bible->navigate("GEN 1:3")); const auto bare = bible->tagScope();
        QVERIFY(bible->navigate("GEN 2:1")); const auto elsewhere = bible->tagScope();
        const auto backup = QUrl::fromLocalFile(session->filePath("backup.json")); QVERIFY(bible->exportBackup(backup));
        auto root = json(backup.toLocalFile());
        // Older versions kept bookmarks as a flag: one on the noted range, one alone.
        auto annotations = root.value("annotations").toArray();
        auto withNote = annotations.first().toObject(); QCOMPARE(withNote.value("bookmark").toBool(),false);
        withNote["bookmark"] = true; annotations[0] = withNote;
        auto alone = withNote; alone["note"] = ""; alone["start"] = 3; alone["end"] = 3; annotations.append(alone);
        root["annotations"] = annotations;
        const auto bookmarked = [&](const QVariantMap &scope) {
            const auto tags = bible->tagsForScope(scope);
            return std::any_of(tags.cbegin(),tags.cend(),[](const QVariant &tag) { return tag.toMap().value("name") == "Bookmarked"; });
        };
        const auto restore = [&](int version) {
            auto legacy = root; legacy["version"] = version;
            if (version == 3) { legacy.remove("tags"); legacy.remove("tagAssignments"); legacy.remove("desk"); }
            const auto file = QUrl::fromLocalFile(session->filePath(QString("legacy%1.json").arg(version)));
            return write(file.toLocalFile(),QJsonDocument(legacy).toJson()) && bible->importBackup(file);
        };
        QVERIFY2(restore(5),qPrintable(bible->error()));
        QVERIFY(bookmarked(noted)); QVERIFY(bookmarked(bare)); QCOMPARE(bible->tags().size(),1);
        QVERIFY(bible->openTaggedPassage(noted)); QCOMPARE(bible->noteText(),QString("Noted"));
        // The bookmark alone leaves no annotation row behind, only its tag.
        QSqlQuery q(store()); QVERIFY(q.exec("SELECT COUNT(*) FROM annotations")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(),1); q.finish();
        // Format 3 had no tags: it replaces only Bookmarked among them.
        const QString other = bible->createAndAssignTag(elsewhere,"Other"); QVERIFY(!other.isEmpty());
        QVERIFY(bible->setTag(elsewhere,bible->tagsForScope(bare).first().toMap().value("id").toString(),true));
        QVERIFY2(restore(3),qPrintable(bible->error()));
        QVERIFY(bookmarked(noted)); QVERIFY(bookmarked(bare)); QVERIFY(!bookmarked(elsewhere));
        QCOMPARE(bible->tagsForScope(elsewhere).size(),1);
        // Backups now carry them as the tag.
        QVERIFY(bible->exportBackup(backup)); const auto again = json(backup.toLocalFile());
        for (const auto &value : again.value("annotations").toArray()) QVERIFY(!value.toObject().value("bookmark").toBool());
        QCOMPARE(again.value("tagAssignments").toArray().size(),3);
    }
    void rollbackAndPublication() {
        install(net); QVERIFY(bible->navigate("GEN 1:1")); const auto scope = bible->tagScope();
        const QString id = bible->createAndAssignTag(scope,"Existing"); QVERIFY(!id.isEmpty());
        bible->setNoteText("Preserve me");
        // Capture the writer connection before peers exist: foreign_keys is
        // connection-local, so an arbitrary peer cannot inject COMMIT failure.
        QSqlQuery q(store());
        BibleBackend peer; const auto oldTags = bible->tags(), oldCollection = bible->collection();
        QSignalSpy local(bible.get(),&BibleBackend::tagsChanged), remote(&peer,&BibleBackend::tagsChanged);
        QVERIFY(q.exec("CREATE TRIGGER reject_tags BEFORE INSERT ON tag_assignments BEGIN SELECT RAISE(ABORT,'assignment failure'); END"));
        QVERIFY(bible->createAndAssignTag(scope,"Must roll back").isEmpty()); QCOMPARE(bible->tags(),oldTags);
        QCOMPARE(local.count(),0); QCOMPARE(remote.count(),0); QCOMPARE(peer.collection(),oldCollection);
        QVERIFY(q.exec("DROP TRIGGER reject_tags"));
        QVERIFY(q.exec("CREATE TRIGGER ignore_tags BEFORE INSERT ON tag_assignments BEGIN SELECT RAISE(IGNORE); END"));
        auto different = scope; different["start"] = 2; different["end"] = 2;
        QVERIFY(!bible->setTag(different,id,true)); QCOMPARE(local.count(),0);
        QVERIFY(q.exec("DROP TRIGGER ignore_tags"));
        QVERIFY(q.exec("PRAGMA foreign_keys=ON"));
        QVERIFY(q.exec("CREATE TABLE parent(id INTEGER PRIMARY KEY)"));
        QVERIFY(q.exec("CREATE TABLE failure(id INTEGER REFERENCES parent(id) DEFERRABLE INITIALLY DEFERRED)"));
        QVERIFY(q.exec("CREATE TRIGGER fail_commit AFTER INSERT ON tag_assignments BEGIN INSERT INTO failure VALUES(1); END"));
        QVERIFY(bible->createAndAssignTag(different,"Commit must roll back").isEmpty());
        QVERIFY(bible->error().contains("Cannot commit tags")); QCOMPARE(bible->tags(),oldTags);
        QCOMPARE(local.count(),0); QCOMPARE(remote.count(),0);
        QVERIFY(q.exec("DROP TRIGGER fail_commit"));
        QVERIFY(q.exec("CREATE TRIGGER reject_delete BEFORE DELETE ON tag_assignments BEGIN SELECT RAISE(ABORT,'deletion failure'); END"));
        QVERIFY(!bible->deleteAnnotation(scope.value("reference").toString())); QCOMPARE(bible->collection(),oldCollection);
        QVERIFY(!bible->deleteTag(id)); QCOMPARE(bible->tags(),oldTags); QCOMPARE(remote.count(),0);
        QVERIFY(q.exec("DROP TRIGGER reject_delete"));
        const QString target = bible->createTag("Target"); QVERIFY(!target.isEmpty()); local.clear(); remote.clear();
        QVERIFY(q.exec("CREATE TRIGGER reject_merge BEFORE DELETE ON tags BEGIN SELECT RAISE(ABORT,'merge failure'); END"));
        QVERIFY(!bible->mergeTags(id,target)); QCOMPARE(count(id),1); QCOMPARE(count(target),0); QCOMPARE(local.count(),0);
        QVERIFY(q.exec("DROP TRIGGER reject_merge"));
        const auto backup = QUrl::fromLocalFile(session->filePath("rollback.json")); QVERIFY(bible->exportBackup(backup));
        QVERIFY(bible->setTag(scope,target,true)); bible->setNoteText("Newer note");
        const auto saved = bible->collection(); local.clear(); remote.clear();
        QVERIFY(q.exec("CREATE TRIGGER reject_restore BEFORE INSERT ON annotations BEGIN SELECT RAISE(ABORT,'restore failure'); END"));
        QVERIFY(!bible->importBackup(backup)); QCOMPARE(bible->collection(),saved); QCOMPARE(count(target),1);
        QCOMPARE(peer.collection(),saved); QCOMPARE(remote.count(),0);
        QVERIFY(!bible->toggleHighlight()); QVERIFY(!bible->selectionHighlighted());
        QVERIFY(q.exec("DROP TRIGGER reject_restore"));
        QVERIFY(q.exec("CREATE TRIGGER ignore_marks BEFORE INSERT ON annotations BEGIN SELECT RAISE(IGNORE); END"));
        QVERIFY(!bible->toggleHighlight()); QCOMPARE(bible->collection(),saved);
        QVERIFY(q.exec("DROP TRIGGER ignore_marks"));
        QVERIFY(q.exec("CREATE TRIGGER reject_reading BEFORE UPDATE ON reading BEGIN SELECT RAISE(ABORT,'reading failure'); END"));
        QVERIFY(!bible->openTaggedPassage(different)); QCOMPARE(bible->tagScope(),scope);
        QVERIFY(q.exec("DROP TRIGGER reject_reading"));
        peer.setFlushCallback([] { return false; }); QVERIFY(!bible->importBackup(backup)); QCOMPARE(bible->collection(),saved);
    }
    void crossVersionOpenRollbackAndRestoreCapture() {
        install(net); QVERIFY(bible->navigate("TOB 2:4")); bible->moveSelection(1,true);
        const auto source = bible->tagScope(); const auto id = bible->createAndAssignTag(source,"Frozen"); QVERIFY(!id.isEmpty());
        const auto backup = QUrl::fromLocalFile(session->filePath("capture.json")); QVERIFY(bible->exportBackup(backup));
        install(kjv); QVERIFY(bible->navigate("GEN 2:2")); const auto original = bible->tagScope();
        QSqlQuery q(store());
        QVERIFY(q.exec("CREATE TRIGGER reject_library BEFORE UPDATE ON library BEGIN SELECT RAISE(ABORT,'open failure'); END"));
        QSignalSpy navigation(bible.get(),&BibleBackend::navigationChanged);
        QVERIFY(!bible->openTaggedPassage(source)); QCOMPARE(bible->tagScope(),original); QCOMPARE(navigation.count(),0);
        QVERIFY(q.exec("DROP TRIGGER reject_library"));
        QVERIFY(bible->openTaggedPassage(source)); QCOMPARE(bible->tagScope(),source);
        bible->setFlushCallback([&] { return bible->switchVersion("eng-kjv"); });
        QVERIFY(!bible->importBackup(backup)); QVERIFY(bible->error().contains("version changed"));
        QCOMPARE(count(id),1); QVERIFY(bible->tagsForScope(original).isEmpty());
        bible->setFlushCallback({});
        // A clean restore retains portable IDs when there is no global collision.
        QVERIFY(bible->openTaggedPassage(source)); QVERIFY(bible->deleteTag(id));
        QVERIFY(bible->importBackup(backup)); QCOMPARE(bible->selectionTags().first().toMap().value("id").toString(),id);
    }
    void savedDeletionUsesEndpointsDespiteAmbiguousNames() {
        const QString path = session->filePath("engnet_usfm.zip");
        QVERIFY(fixture(path,"engnet",{"GEN","JHN"},4,true)); install(path);
        QVERIFY(bible->navigate("GEN 1:2")); const auto genesis = bible->tagScope();
        QCOMPARE(genesis.value("book").toInt(),1); QCOMPARE(genesis.value("reference").toString(),QString("John 1:2"));
        const QString tag = bible->createAndAssignTag(genesis,"Shared name"); QVERIFY(!tag.isEmpty());
        bible->setNoteText("Genesis note"); QVERIFY(bible->toggleHighlight());
        QVERIFY(bible->navigate("JHN 1:2")); const auto john = bible->tagScope();
        QCOMPARE(john.value("book").toInt(),43); QCOMPARE(john.value("reference"),genesis.value("reference"));
        QVERIFY(bible->setTag(john,tag,true)); bible->setNoteText("John note");
        const auto savedGenesis = bible->collection().first().toMap();
        QCOMPARE(savedGenesis.value("book").toInt(),1);
        BibleBackend peer; const auto before = bible->collection();
        QSignalSpy changed(&peer,&BibleBackend::tagsChanged);
        for (const auto &field : QStringList{"versionId","book","chapter","start","endBook","endChapter","end"}) {
            auto invalid = savedGenesis; invalid.remove(field);
            QVERIFY(!bible->deleteAnnotationScope(invalid));
        }
        auto invalid = savedGenesis; invalid["end"] = 99; QVERIFY(!bible->deleteAnnotationScope(invalid));
        invalid = savedGenesis; invalid["start"] = "2"; QVERIFY(!bible->deleteAnnotationScope(invalid));
        invalid = savedGenesis; invalid["start"] = 2.5; QVERIFY(!bible->deleteAnnotationScope(invalid));
        invalid = savedGenesis; invalid["versionId"] = "missing"; QVERIFY(!bible->deleteAnnotationScope(invalid));
        QCOMPARE(bible->collection(),before); QCOMPARE(changed.count(),0);
        QSqlQuery q(store());
        QVERIFY(q.exec("CREATE TRIGGER reject_scope_delete BEFORE DELETE ON tag_assignments BEGIN SELECT RAISE(ABORT,'tag delete failure'); END"));
        QVERIFY(!bible->deleteAnnotationScope(savedGenesis)); QCOMPARE(bible->collection(),before);
        QCOMPARE(peer.collection(),before); QCOMPARE(changed.count(),0);
        QVERIFY(q.exec("DROP TRIGGER reject_scope_delete"));
        // Frozen Saved scope remains meaningful after switching editions.
        install(kjv); const auto current = bible->tagScope();
        QSignalSpy navigation(bible.get(),&BibleBackend::navigationChanged);
        QVERIFY(bible->deleteAnnotationScope(savedGenesis)); QCOMPARE(bible->tagScope(),current); QCOMPARE(navigation.count(),0);
        QCOMPARE(count(tag),1); QVERIFY(bible->tagsForScope(genesis).isEmpty()); QCOMPARE(bible->tagsForScope(john).size(),1);
        QCOMPARE(peer.collection().size(),1); QCOMPARE(peer.collection().first().toMap().value("book").toInt(),43);
        QCOMPARE(peer.noteText(),QString("John note"));
        QVERIFY(bible->openTaggedPassage(genesis)); QVERIFY(bible->noteText().isEmpty());
        QVERIFY(!bible->selectionHighlighted());
        QVERIFY(!bible->deleteAnnotationScope(savedGenesis));
        QVERIFY(bible->openTaggedPassage(john)); QCOMPARE(bible->noteText(),QString("John note"));
        QVERIFY(bible->deleteAnnotation("JHN 1:2",bible->versionId())); QCOMPARE(count(tag),0);
        QVERIFY(peer.collection().isEmpty());
        QVERIFY(bible->setTag(genesis,tag,true)); // tags-only exact deletion, bogus label ignored
        auto mislabeled = genesis; mislabeled["reference"] = "Not a reference";
        QVERIFY(bible->deleteAnnotationScope(mislabeled)); QCOMPARE(count(tag),0);
    }
    void reimportRejectsInvalidRetainedTags_data() {
        QTest::addColumn<bool>("reversed");
        QTest::newRow("tag-only-verse-removed") << false;
        QTest::newRow("crossbook-source-order-reversed") << true;
    }
    void reimportRejectsInvalidRetainedTags() {
        QFETCH(bool,reversed);
        const QString path = session->filePath("engnet_usfm.zip");
        const QStringList codes = reversed ? QStringList{"GEN","TOB","MAT"} : QStringList{"GEN"};
        QVERIFY(fixture(path,"engnet",codes)); install(path);
        QVERIFY(bible->navigate(reversed ? "TOB 2:4" : "GEN 1:4"));
        if (reversed) bible->moveSelection(1,true);
        const auto scope = bible->tagScope();
        if (reversed) { QCOMPARE(scope.value("book").toInt(),67); QCOMPARE(scope.value("endBook").toInt(),40); }
        const QString tag = bible->createAndAssignTag(scope,"Retained tag-only range"); QVERIFY(!tag.isEmpty());
        BibleBackend peer; const auto peerScope = peer.tagScope();
        const auto collection = bible->collection(), tags = bible->tags(), versions = bible->versions(), verses = bible->verses();
        auto registry = [&] {
            QSqlQuery q(store()); QVariantList rows;
            if (!q.exec("SELECT version_id,path,last_used FROM installed ORDER BY version_id")) return rows;
            while (q.next()) rows.append(QVariantList{q.value(0),q.value(1),q.value(2)});
            if (!q.exec("SELECT active,use_counter FROM library WHERE id=1")) return QVariantList();
            while (q.next()) rows.append(QVariantList{q.value(0),q.value(1)});
            return rows;
        };
        const auto oldRegistry = registry(); QVERIFY(!oldRegistry.isEmpty());
        const auto oldFiles = QDir(bible->dataDirectory()).entryList({"bible-*.sqlite"},QDir::Files); QCOMPARE(oldFiles.size(),1);
        QFile original(QDir(bible->dataDirectory()).filePath(oldFiles.first())); QVERIFY(original.open(QIODevice::ReadOnly));
        const QByteArray originalBytes = original.readAll(); original.close();
        QSignalSpy committed(bible.get(),&BibleBackend::sharedCommitted), changed(&peer,&BibleBackend::tagsChanged);
        QSignalSpy navigation(bible.get(),&BibleBackend::navigationChanged);
        QVERIFY(fixture(path,"engnet",reversed ? QStringList{"GEN","MAT","TOB"} : codes,reversed ? 4 : 3));
        bible->importUsfm(QUrl::fromLocalFile(path)); QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(),10000);
        QVERIFY2(bible->error().contains("retained tagged passage"),qPrintable(bible->error()));
        QCOMPARE(registry(),oldRegistry); QCOMPARE(QDir(bible->dataDirectory()).entryList({"bible-*.sqlite"},QDir::Files),oldFiles);
        QVERIFY(original.open(QIODevice::ReadOnly)); QCOMPARE(original.readAll(),originalBytes); original.close();
        QCOMPARE(bible->versions(),versions); QCOMPARE(bible->collection(),collection); QCOMPARE(bible->tags(),tags);
        QCOMPARE(bible->tagScope(),scope); QCOMPARE(bible->verses(),verses); QCOMPARE(count(tag),1);
        QCOMPARE(peer.tagScope(),peerScope); QCOMPARE(peer.collection(),collection);
        QCOMPARE(committed.count(),0); QCOMPARE(changed.count(),0); QCOMPARE(navigation.count(),0);
        const auto backup = QUrl::fromLocalFile(session->filePath("after-rejected-import.json"));
        QVERIFY(bible->exportBackup(backup)); QCOMPARE(json(backup.toLocalFile()).value("tagAssignments").toArray().size(),1);
        QVERIFY(bible->setTag(scope,tag,false)); QCOMPARE(count(tag),0);
        QVERIFY2(bible->importBackup(backup),qPrintable(bible->error())); QCOMPARE(count(tag),1); QCOMPARE(bible->collection(),collection);
        // Compatible reimport is still permitted and preserves the same scope.
        QVERIFY(fixture(path,"engnet",codes)); install(path); QCOMPARE(count(tag),1);
        QVERIFY(bible->openTaggedPassage(scope)); QCOMPARE(bible->tagScope(),scope);
    }
};

QTEST_GUILESS_MAIN(TagsTest)
#include "tst_tags.moc"
