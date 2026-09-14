#include <QtTest/QTest>
#include "biblebackend.h"
#include "desktop.h"
#include "windowmanager.h"
#include <QApplication>
#include <QDir>
#include <QPointer>
#include <QProcess>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQuickStyle>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QWindow>
#include <archive.h>
#include <archive_entry.h>

class WindowsTest : public QObject
{
    Q_OBJECT
    QTemporaryDir fixtures;
    std::unique_ptr<QTemporaryDir> session;
    std::unique_ptr<Desktop> desktop;
    std::unique_ptr<WindowManager> windows;
    QString fixture;
    QString structuredUsfm;
    static QObject *visualObject(QQuickItem *item, const QString &name) {
        if (item->objectName() == name) return item;
        for (auto *child : item->childItems())
            if (auto *found = visualObject(child,name)) return found;
        return nullptr;
    }
    static QObject *windowObject(QWindow *window, const QString &name) {
        if (auto *found = window->findChild<QObject *>(name)) return found;
        auto *quick = qobject_cast<QQuickWindow *>(window);
        return quick ? visualObject(quick->contentItem(),name) : nullptr;
    }
    static bool flush(QWindow *window) {
        QVariant saved;
        return QMetaObject::invokeMethod(window,"flushNote",Q_RETURN_ARG(QVariant,saved)) && saved.toBool();
    }
    // Small original USFM content covers only references used by these tests.
    static bool writeStructuredUsfm(const QString &path, const QString &id) {
        QMap<QString,QByteArray> entries;
        const QStringList codes{"GEN","MRK","JHN","ROM"};
        for (int b = 0; b < codes.size(); ++b) {
            QString body = "\\id " + codes[b] + " Original Window Bible\n";
            const QList<int> chapters = b == 0 ? QList<int>{1,2} : b == 1 ? QList<int>{7} : b == 2 ? QList<int>{3,4} : QList<int>{8};
            for (int c : chapters) {
                body += QString("\\c %1\n\\p\n").arg(c);
                for (int v = 1; v <= 36; ++v)
                    body += QString("\\v %1 Original window fixture words %2:%3:%1.\n").arg(v).arg(codes[b]).arg(c);
            }
            entries[QString("%1-%2%3.usfm").arg(b+1,2,10,QChar('0')).arg(codes[b],id)] = body.toUtf8();
        }
        archive *zip = archive_write_new();
        const auto cleanup = qScopeGuard([&] { archive_write_free(zip); });
        if (archive_write_set_format_zip(zip) != ARCHIVE_OK
            || archive_write_open_filename(zip,QFile::encodeName(path).constData()) != ARCHIVE_OK) return false;
        for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
            archive_entry *entry = archive_entry_new();
            const auto cleanupEntry = qScopeGuard([&] { archive_entry_free(entry); });
            archive_entry_set_pathname(entry,it.key().toUtf8().constData());
            archive_entry_set_size(entry,it.value().size());
            archive_entry_set_filetype(entry,AE_IFREG); archive_entry_set_perm(entry,0600);
            if (archive_write_header(zip,entry) != ARCHIVE_OK
                || archive_write_data(zip,it.value().constData(),it.value().size()) != it.value().size()) return false;
        }
        return archive_write_close(zip) == ARCHIVE_OK;
    }
    QString installedPath(BibleBackend *bible) {
        const QString connection = "windows-inspect";
        QString path;
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(bible->dataDirectory() + "/personal.sqlite");
            if (db.open()) {
                QSqlQuery q(db);
                q.prepare("SELECT path FROM installed WHERE version_id=?"); q.addBindValue(bible->versionId());
                if (q.exec() && q.next()) path = bible->dataDirectory() + '/' + q.value(0).toString();
            }
        }
        QSqlDatabase::removeDatabase(connection);
        return path;
    }
private slots:
    void initTestCase() {
        QVERIFY(fixtures.isValid());
        fixture = fixtures.filePath("engnet_usfm.zip");
        structuredUsfm = fixtures.filePath("window-original_usfm.zip");
        QVERIFY(writeStructuredUsfm(fixture,"engnet"));
        QVERIFY(writeStructuredUsfm(structuredUsfm,"window-original"));
    }
    void init() {
        session = std::make_unique<QTemporaryDir>();
        qputenv("XDG_DATA_HOME", session->path().toUtf8());
        const auto data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QVERIFY(QDir().mkpath(data));
        {
            BibleBackend seed;
            seed.importUsfm(QUrl::fromLocalFile(fixture));
            QTRY_VERIFY_WITH_TIMEOUT(!seed.busy(),30000);
            QVERIFY2(seed.ready(),qPrintable(seed.error()));
        }
        desktop = std::make_unique<Desktop>();
        windows = std::make_unique<WindowManager>(desktop.get());
        windows->newWindow(); windows->newWindow();
        QCOMPARE(windows->count(), 2);
        QVERIFY(windows->backendAt(0)->ready()); QVERIFY(windows->backendAt(1)->ready());
    }
    void cleanup() {
        QCoreApplication::processEvents();
        windows.reset(); desktop.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        session.reset();
    }
    void independentReadersAndSharedCommits() {
        auto *a = windows->backendAt(0), *b = windows->backendAt(1);
        QVERIFY(a != b); QVERIFY(windows->windowAt(0) != windows->windowAt(1));
        QVERIFY(windows->windowAt(0)->title() != windows->windowAt(1)->title());
        QVERIFY(a->navigate("John 3:18"));
        a->moveSelection(-1, true); a->setReadingPosition(0.42);
        const auto selection = a->selectionReference();
        QVERIFY(b->navigate("Genesis 2:3"));
        b->setNoteText("Second window note");
        QCOMPARE(a->selectionReference(), selection); QCOMPARE(a->cursorVerse(),17);
        QCOMPARE(a->readingPosition(),0.42); QCOMPARE(a->collection(),b->collection());
        a->setNoteText("First window note");
        QCOMPARE(a->collection().size(),2); QCOMPARE(a->collection(),b->collection());
        QVERIFY(b->navigate("John 3:17-18"));
        QCOMPARE(b->noteText(),QString("First window note"));
        b->toggleBookmark(); QVERIFY(a->selectionBookmarked());
        QVERIFY(a->deleteAnnotation(selection));
        QVERIFY(b->noteText().isEmpty()); QVERIFY(!b->selectionBookmarked());
        a->setFontSize(23); QCOMPARE(b->fontSize(),23);
        b->setReadingMode("verse"); QCOMPARE(a->readingMode(),QString("verse"));
        b->setShowHeadings(false); QVERIFY(!a->showHeadings());
        QCOMPARE(a->selectionReference(),selection); QCOMPARE(a->cursorVerse(),17);
    }
    void failedCommitDoesNotPublish() {
        auto *a = windows->backendAt(0), *b = windows->backendAt(1);
        QVERIFY(a->navigate("John 3:16")); QVERIFY(b->navigate("John 3:16"));
        a->setNoteText("Keep both copies");
        const auto collection = b->collection();
        QSignalSpy commits(a,&BibleBackend::sharedCommitted);
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","windows-failure");
            db.setDatabaseName(a->dataDirectory()+"/personal.sqlite"); QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TRIGGER reject_notes BEFORE DELETE ON annotations BEGIN SELECT RAISE(ABORT,'test save failure'); END"));
            a->setNoteText("Cannot save");
            QVERIFY(a->error().contains("test save failure")); QCOMPARE(commits.count(),0);
            QCOMPARE(b->collection(),collection); QCOMPARE(a->noteText(),b->noteText());
            QVERIFY(q.exec("DROP TRIGGER reject_notes"));
        }
        QSqlDatabase::removeDatabase("windows-failure");
        b->toggleHighlight();
        QCOMPARE(a->noteText(),QString("Keep both copies")); QVERIFY(a->selectionHighlighted());
    }
    void actualWindowDraftConflict_data() {
        QTest::addColumn<bool>("saveDraft");
        QTest::newRow("save-my-draft") << true;
        QTest::newRow("use-saved-text") << false;
    }
    void actualWindowDraftConflict() {
        QFETCH(bool,saveDraft);
        auto *a = windows->backendAt(0), *b = windows->backendAt(1);
        auto *first = windows->windowAt(0), *second = windows->windowAt(1);
        a->importUsfm(QUrl::fromLocalFile(structuredUsfm));
        QVERIFY2(a->busy(),qPrintable(a->error()));
        QTRY_VERIFY_WITH_TIMEOUT(!a->busy(),30000);
        QVERIFY2(a->error().isEmpty(),qPrintable(a->error()));
        const QString id = a->versionId();
        QCOMPARE(id,QString("window-original")); QVERIFY(b->switchVersion(id));
        QVERIFY(a->navigate("John 3:16")); QVERIFY(b->navigate("John 3:16"));
        auto *editorA = windowObject(first,"noteEditor"), *editorB = windowObject(second,"noteEditor");
        QVERIFY(editorA); QVERIFY(editorB);
        QVERIFY(editorA->setProperty("text","Initial shared note"));
        QCOMPARE(editorB->property("text").toString(),QString("Initial shared note"));
        const auto backup = QUrl::fromLocalFile(session->filePath("conflict-backup.json"));
        QVERIFY(a->exportBackup(backup));
        const auto generation = installedPath(a);
        const auto removeConnection = qScopeGuard([] { QSqlDatabase::removeDatabase("windows-editor-failure"); });
        auto db = QSqlDatabase::addDatabase("QSQLITE","windows-editor-failure");
        db.setDatabaseName(a->dataDirectory()+"/personal.sqlite"); QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec("CREATE TRIGGER reject_editor BEFORE DELETE ON annotations BEGIN SELECT RAISE(ABORT,'editor draft save failure'); END"));
        QVERIFY(editorB->setProperty("text","My failed draft"));
        QVERIFY(b->error().contains("editor draft save failure"));
        QCOMPARE(second->property("loadedNoteText").toString(),QString("Initial shared note"));
        QCOMPARE(b->noteText(),QString("Initial shared note"));
        QVERIFY(!flush(second)); QVERIFY(!second->close());
        QCOMPARE(windows->count(),2); QVERIFY(second->isVisible());
        QVERIFY(q.exec("DROP TRIGGER reject_editor"));

        a->toggleHighlight(); // A mark-only commit must not discard a failed draft.
        QCOMPARE(editorB->property("text").toString(),QString("My failed draft"));
        QVERIFY(!second->property("noteConflict").toBool());
        QVERIFY(editorA->setProperty("text","New saved peer revision"));
        QCOMPARE(b->noteText(),QString("New saved peer revision"));
        QCOMPARE(editorB->property("text").toString(),QString("My failed draft"));
        QVERIFY(second->property("noteConflict").toBool());
        QCOMPARE(second->property("loadedVersionId").toString(),id);
        QCOMPARE(second->property("loadedNoteReference").toString(),QString("John 3:16"));
        QVERIFY(windowObject(second,"noteConflictSaved")->property("text").toString().contains("New saved peer revision"));
        QVERIFY(!flush(second));
        // Use the manager's real root callbacks, without replacing them with test lambdas.
        QVERIFY(!a->importBackup(backup));
        QVERIFY(!a->removeVersion(id,true));
        a->importUsfm(QUrl::fromLocalFile(structuredUsfm));
        QVERIFY(!a->busy()); QVERIFY(a->error().contains("unsaved note"));
        QVERIFY(!second->close());
        QCOMPARE(windows->count(),2); QVERIFY(first->isVisible()); QVERIFY(second->isVisible());
        QCOMPARE(installedPath(a),generation); QCOMPARE(installedPath(b),generation);
        QVERIFY(QFile::exists(generation)); QCOMPARE(a->versions().size(),2);
        QCOMPARE(editorB->property("text").toString(),QString("My failed draft"));
        QCOMPARE(editorA->property("text").toString(),QString("New saved peer revision"));

        auto *resolve = windowObject(second,saveDraft ? "noteRetry" : "noteUseSaved");
        QVERIFY(resolve); QVERIFY(QMetaObject::invokeMethod(resolve,"clicked"));
        const QString resolved = saveDraft ? "My failed draft" : "New saved peer revision";
        QCOMPARE(a->noteText(),resolved); QCOMPARE(b->noteText(),resolved);
        QCOMPARE(editorA->property("text").toString(),resolved);
        QCOMPARE(editorB->property("text").toString(),resolved);
        QVERIFY(!second->property("noteConflict").toBool()); QVERIFY(flush(second));
        q.prepare("SELECT note FROM annotations WHERE version_id=?"); q.addBindValue(id);
        QVERIFY(q.exec());
        QVERIFY(q.next()); QCOMPARE(q.value(0).toString(),resolved); q.finish();
        QVERIFY(a->importBackup(backup));
        QCOMPARE(editorB->property("text").toString(),QString("Initial shared note"));
        a->importUsfm(QUrl::fromLocalFile(structuredUsfm));
        QVERIFY2(a->busy(),qPrintable(a->error()));
        QTRY_VERIFY_WITH_TIMEOUT(!a->busy(),30000);
        QVERIFY2(a->error().isEmpty(),qPrintable(a->error()));
        QCOMPARE(b->versionId(),id); QCOMPARE(b->noteText(),QString("Initial shared note"));
        QVERIFY(installedPath(b) != generation); QVERIFY(!QFile::exists(generation));
        QVERIFY(a->removeVersion(id,true));
        QCOMPARE(a->versionId(),QString("engnet")); QCOMPARE(b->versionId(),QString("engnet"));
        QVERIFY(a->ready()); QVERIFY(b->ready());
        QPointer<QWindow> closed(second);
        QVERIFY(second->close()); QTRY_COMPARE(windows->count(),1); QTRY_VERIFY(!closed);
        QVERIFY(first->isVisible());
    }
    void genericStructuredEditionAcrossWindows() {
        auto *a = windows->backendAt(0), *b = windows->backendAt(1);
        QVERIFY(b->navigate("John 3:16"));
        a->importUsfm(QUrl::fromLocalFile(structuredUsfm));
        QVERIFY2(a->busy(),qPrintable(a->error()));
        QTRY_VERIFY_WITH_TIMEOUT(!a->busy(),30000);
        QVERIFY2(a->error().isEmpty(),qPrintable(a->error()));
        const QString id = a->versionId();
        QCOMPARE(id,QString("window-original"));
        QVERIFY(!a->edition().isEmpty());
        QVERIFY(!a->versionAbbreviation().isEmpty());
        QString error;
        QVERIFY2(UsfmImporter::validateDatabase(installedPath(a),&error),qPrintable(error));
        QCOMPARE(b->versionId(),QString("engnet")); QCOMPARE(b->versions().size(),2);
        QCOMPARE(b->selectionReference(),QString("John 3:16"));
        QVERIFY(a->navigate("Mark 7:16")); QVERIFY(b->switchVersion(id));
        QCOMPARE(b->selectionReference(),QString("John 3:16"));
        QVERIFY(b->navigate("Mark 7:16"));
        auto *editor = windowObject(windows->windowAt(0),"noteEditor"); QVERIFY(editor);
        QVERIFY(editor->setProperty("text","Source-numbered shared note"));
        QCOMPARE(b->noteText(),QString("Source-numbered shared note"));
        QCOMPARE(windowObject(windows->windowAt(1),"noteEditor")->property("text").toString(),b->noteText());
        const auto obsolete = installedPath(a);
        a->importUsfm(QUrl::fromLocalFile(structuredUsfm));
        QTRY_VERIFY_WITH_TIMEOUT(!a->busy(),30000);
        QVERIFY2(a->error().isEmpty(),qPrintable(a->error()));
        QCOMPARE(a->versionId(),id); QCOMPARE(b->versionId(),id);
        QCOMPARE(b->selectionReference(),QString("Mark 7:16"));
        QCOMPARE(b->noteText(),QString("Source-numbered shared note"));
        QVERIFY(installedPath(b) != obsolete); QVERIFY(!QFile::exists(obsolete));
        QVERIFY2(UsfmImporter::validateDatabase(installedPath(b),&error),qPrintable(error));
        QVERIFY(a->removeVersion(id,true));
        QCOMPARE(a->versionId(),QString("engnet")); QCOMPARE(b->versionId(),QString("engnet"));
        QVERIFY(a->ready()); QVERIFY(b->ready()); QCOMPARE(b->versions().size(),1);
    }
    void flushVetoRestoreAndRemoval() {
        auto *a = windows->backendAt(0), *b = windows->backendAt(1);
        QVERIFY(a->navigate("John 3:16")); QVERIFY(b->navigate("John 3:18"));
        a->setNoteText("Saved");
        const auto backup = QUrl::fromLocalFile(session->filePath("backup.json"));
        QVERIFY(a->exportBackup(backup));
        const auto path = installedPath(a);
        int calls = 0;
        b->setFlushCallback([&] { ++calls; return false; });
        QVERIFY(!a->removeVersion("engnet",true));
        QVERIFY(!a->importBackup(backup));
        QCOMPARE(calls,2); QVERIFY(QFile::exists(path)); QVERIFY(b->ready());
        b->setFlushCallback([&] { b->setNoteText("Flushed peer draft"); return b->noteText() == "Flushed peer draft"; });
        // Restore refreshes annotations but leaves the peer's cursor/direction alone.
        QVERIFY(a->importBackup(backup)); QCOMPARE(b->cursorVerse(),18);
        QVERIFY(b->noteText().isEmpty()); QCOMPARE(a->collection(),b->collection());
        QVERIFY(a->removeVersion("engnet",false));
        QVERIFY(!a->ready()); QVERIFY(!b->ready()); QVERIFY(b->versions().isEmpty());
        QVERIFY(!QFile::exists(path));
        b->setNoteText("Cannot resurrect removed annotations"); QVERIFY(!b->error().isEmpty());
    }
    void flushDoesNotPublishBeforeEveryDraftIsSaved_data() {
        QTest::addColumn<bool>("import");
        QTest::newRow("third-window-removal") << false;
        QTest::newRow("third-window-import") << true;
    }
    void flushDoesNotPublishBeforeEveryDraftIsSaved() {
        QFETCH(bool,import);
        auto *a = windows->backendAt(0), *b = windows->backendAt(1);
        QVERIFY(a->navigate("John 3:16")); QVERIFY(b->navigate("John 3:16"));
        a->setNoteText("Original");
        windows->newWindow(); QCOMPARE(windows->count(),3);
        auto *third = windows->backendAt(2);
        auto *firstRoot = windows->windowAt(0), *secondRoot = windows->windowAt(1);
        auto *firstEditor = windowObject(firstRoot,"noteEditor");
        auto *secondEditor = windowObject(secondRoot,"noteEditor");
        QVERIFY(firstEditor); QVERIFY(secondEditor);
        const QString generation = installedPath(a);
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","windows-flushed");
            db.setDatabaseName(a->dataDirectory()+"/personal.sqlite"); QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TRIGGER reject_drafts BEFORE DELETE ON annotations BEGIN SELECT RAISE(ABORT,'draft disk failure'); END"));
            QVERIFY(firstEditor->setProperty("text","First draft"));
            QVERIFY(secondEditor->setProperty("text","Second draft"));
            QVERIFY(a->error().contains("draft disk failure")); QVERIFY(b->error().contains("draft disk failure"));
            QCOMPARE(a->noteText(),QString("Original")); QCOMPARE(b->noteText(),QString("Original"));
            QVERIFY(q.exec("DROP TRIGGER reject_drafts"));
            QSignalSpy firstCommits(a,&BibleBackend::sharedCommitted);
            QSignalSpy secondCommits(b,&BibleBackend::sharedCommitted);
            QSignalSpy libraryCommits(third,&BibleBackend::sharedCommitted);
            QString conflict;
            const auto observer = connect(b,&BibleBackend::errorChanged,this,[&] { conflict = b->error(); });
            // Real WindowManager callbacks flush A then B, withholding A's peer
            // notification until B has tried to save against its old baseline.
            if (import) {
                third->importUsfm(QUrl::fromLocalFile(structuredUsfm));
                QVERIFY(!third->busy());
            } else QVERIFY(!third->removeVersion("engnet",true));
            disconnect(observer);
            QVERIFY2(conflict.contains("changed in another window"),qPrintable(conflict));
            QVERIFY(third->error().contains("unsaved note"));
            QCOMPARE(firstCommits.count(),0); QCOMPARE(secondCommits.count(),0);
            QCOMPARE(libraryCommits.count(),1); // Deferred A annotation commit only.
            QCOMPARE(libraryCommits.first().at(0).toString(),QString("engnet"));
            QVERIFY(!libraryCommits.first().at(1).toBool());
            QCOMPARE(firstEditor->property("text").toString(),QString("First draft"));
            QCOMPARE(secondEditor->property("text").toString(),QString("Second draft"));
            QCOMPARE(firstRoot->property("loadedNoteText").toString(),QString("First draft"));
            QCOMPARE(secondRoot->property("loadedNoteText").toString(),QString("Original"));
            QVERIFY(secondRoot->property("noteConflict").toBool());
            QCOMPARE(a->noteText(),QString("First draft")); QCOMPARE(b->noteText(),QString("First draft"));
            QCOMPARE(installedPath(third),generation); QVERIFY(QFile::exists(generation));
            QCOMPARE(third->versions().size(),1); QVERIFY(a->ready()); QVERIFY(b->ready());
            QVERIFY(q.exec("SELECT note FROM annotations WHERE version_id='engnet'"));
            QVERIFY(q.next()); QCOMPARE(q.value(0).toString(),QString("First draft")); q.finish();
            QVERIFY(!flush(secondRoot)); QVERIFY(!secondRoot->close()); QCOMPARE(windows->count(),3);
            // Explicit consent uses the refreshed backend baseline and succeeds.
            auto *retry = windowObject(secondRoot,"noteRetry"); QVERIFY(retry);
            QVERIFY(QMetaObject::invokeMethod(retry,"clicked"));
            QCOMPARE(a->noteText(),QString("Second draft")); QCOMPARE(b->noteText(),QString("Second draft"));
            QCOMPARE(firstEditor->property("text").toString(),QString("Second draft"));
            QVERIFY(!secondRoot->property("noteConflict").toBool()); QVERIFY(flush(secondRoot));
            QVERIFY(third->removeVersion("engnet",false));
            QVERIFY(!a->ready()); QVERIFY(!b->ready()); QVERIFY(!QFile::exists(generation));
            QVERIFY(q.exec("SELECT note FROM annotations WHERE version_id='engnet'"));
            QVERIFY(q.next()); QCOMPARE(q.value(0).toString(),QString("Second draft")); q.finish();
        }
        QSqlDatabase::removeDatabase("windows-flushed");
    }
    void versionReplacementAndFallback() {
        const auto net = fixture, kjv = structuredUsfm;
        auto *a = windows->backendAt(0), *b = windows->backendAt(1);
        QVERIFY(a->navigate("John 3:16")); QVERIFY(b->navigate("John 4:3"));
        b->moveSelection(-1,true); b->setReadingPosition(0.37);
        const auto selection = b->selectionReference();
        a->importUsfm(QUrl::fromLocalFile(kjv)); QVERIFY(a->busy()); QVERIFY(b->busy());
        QVERIFY(!b->removeVersion("engnet",true));
        b->importUsfm(QUrl::fromLocalFile(net)); QVERIFY(!b->error().isEmpty());
        QVERIFY(b->navigate("John 4:3")); // Reading remains usable during import.
        b->moveSelection(-1,true); b->setReadingPosition(0.37);
        QTRY_VERIFY_WITH_TIMEOUT(!a->busy(),60000);
        QCOMPARE(a->versionId(),QString("window-original")); QCOMPARE(b->versionId(),QString("engnet"));
        QCOMPARE(b->versions().size(),2); QCOMPARE(b->selectionReference(),selection);
        QCOMPARE(b->versions().first().toMap()["active"].toBool(),true);
        QCOMPARE(b->versions().first().toMap()["id"].toString(),QString("engnet"));
        QVERIFY(a->switchVersion("engnet"));
        const QString obsolete = installedPath(b);
        double positionBeforeReplacement = 0, positionAfterReplacement = -1;
        b->setFlushCallback([&] {
            QVariant saved;
            if (!QMetaObject::invokeMethod(windows->windowAt(1),"flushNote",Q_RETURN_ARG(QVariant,saved)) || !saved.toBool()) return false;
            positionBeforeReplacement = b->readingPosition();
            return true;
        });
        const auto observer = connect(a,&BibleBackend::sharedCommitted,this,[&](const QString &id, bool library, bool) {
            if (library && id == "engnet") positionAfterReplacement = b->readingPosition();
        });
        a->importUsfm(QUrl::fromLocalFile(net));
        QTRY_VERIFY_WITH_TIMEOUT(!a->busy(),60000);
        QVERIFY2(a->error().isEmpty(),qPrintable(a->error()));
        QVERIFY(installedPath(b) != obsolete); QVERIFY(!QFile::exists(obsolete));
        QCOMPARE(b->selectionReference(),selection); QCOMPARE(b->cursorVerse(),2);
        QCOMPARE(positionAfterReplacement,positionBeforeReplacement);
        disconnect(observer);
        for (const auto &name : QSqlDatabase::connectionNames())
            QVERIFY(QSqlDatabase::database(name).databaseName() != obsolete);
        QVERIFY(a->removeVersion("engnet",true));
        QCOMPARE(b->versionId(),QString("window-original")); QVERIFY(b->ready());
        QCOMPARE(b->selectionReference(),selection); QCOMPARE(b->cursorVerse(),2);
        b->setNoteText("Fallback remains writable"); QCOMPARE(a->collection(),b->collection());
    }
    void closingOneKeepsTheOther() {
        QPointer<QWindow> first(windows->windowAt(0));
        QPointer<BibleBackend> backend(windows->backendAt(0));
        auto *second = windows->windowAt(1);
        QVERIFY(first->close());
        QTRY_COMPARE(windows->count(),1);
        QTRY_VERIFY(!first); QTRY_VERIFY(!backend);
        QCOMPARE(windows->windowAt(0),second); QVERIFY(second->isVisible());
        QVERIFY(windows->backendAt(0)->navigate("Romans 8:1"));
    }
    void importCompletionFlushVeto() {
        const auto net = fixture;
        auto *a = windows->backendAt(0), *b = windows->backendAt(1);
        const auto original = installedPath(a);
        int flushes = 0;
        b->setFlushCallback([&] { return ++flushes == 1; });
        a->importUsfm(QUrl::fromLocalFile(net)); QVERIFY(a->busy());
        QTRY_VERIFY_WITH_TIMEOUT(!a->busy(),60000);
        QCOMPARE(flushes,2); QVERIFY(a->error().contains("unsaved note"));
        QCOMPARE(installedPath(a),original); QCOMPARE(installedPath(b),original);
        QVERIFY(QFile::exists(original)); QVERIFY(a->ready()); QVERIFY(b->ready());
    }
    void rootSignalAndLastWindowExit() {
        QVERIFY(QMetaObject::invokeMethod(windows->windowAt(0),"newWindowRequested"));
        QCOMPARE(windows->count(),3);
        QProcess child;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("XDG_DATA_HOME",session->filePath("exit-child"));
        child.setProcessEnvironment(environment);
        child.start(QCoreApplication::applicationFilePath(),{"--close-all"});
        QVERIFY(child.waitForStarted());
        QTRY_COMPARE_WITH_TIMEOUT(child.state(),QProcess::NotRunning,5000);
        QCOMPARE(child.exitCode(),0);
    }
    void realSecondaryCreatesAnotherWindow() {
        const auto directory = windows->backendAt(0)->dataDirectory();
        SingleInstance primary(directory,"windows-test",session->path());
        QCOMPARE(primary.start(),SingleInstance::Role::Primary);
        primary.setHandler([&](const auto &request, auto reply) { windows->openRequest(request,reply,false); });
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(),{"--secondary",directory,session->path()});
        QVERIFY(child.waitForStarted());
        QTRY_COMPARE_WITH_TIMEOUT(child.state(),QProcess::NotRunning,5000);
        QCOMPARE(child.exitCode(),0); QCOMPARE(windows->count(),3);
        QCOMPARE(windows->backendAt(2)->selectionReference(),QString("Romans 8:1"));
        QCOMPARE(windows->backendAt(0)->book(),1);
        QLockFile lock(directory+"/windows-test.lock"); QVERIFY(!lock.tryLock());
        qint64 pid = 0; QVERIFY(lock.getLockInfo(&pid,nullptr,nullptr));
        QCOMPARE(pid,QCoreApplication::applicationPid());
    }
};

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM","offscreen"); qputenv("QT_QUICK_BACKEND","software");
    qunsetenv("HYPRLAND_INSTANCE_SIGNATURE");
    QApplication app(argc,argv);
    app.setApplicationName("omaword-windows-test");
    QQuickStyle::setStyle("Basic");
    if (app.arguments().value(1) == "--close-all") {
        Desktop desktop;
        WindowManager windows(&desktop);
        windows.newWindow(); windows.newWindow();
        QTimer::singleShot(50,&windows,[&] { windows.windowAt(0)->close(); });
        QTimer::singleShot(150,&windows,[&] {
            if (windows.count() != 1) app.exit(3);
            else windows.windowAt(0)->close();
        });
        QTimer::singleShot(2500,&app,[&] { app.exit(4); });
        return app.exec();
    }
    if (app.arguments().value(1) == "--secondary") {
        SingleInstance client(app.arguments().value(2),"windows-test",app.arguments().value(3));
        if (client.start() != SingleInstance::Role::Secondary) return 2;
        QTimer::singleShot(0,&client,[&] {
            SingleInstance::Request request; request.reference = "Romans 8:1";
            client.forward(request,[&](auto result) { app.exit(result.ok ? 0 : 1); });
        });
        return app.exec();
    }
    app.setQuitOnLastWindowClosed(false);
    WindowsTest test;
    return QTest::qExec(&test,argc,argv);
}
#include "tst_windows.moc"
