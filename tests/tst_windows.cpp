#include "testenvironment.h"
#include <QtTest/QTest>
#include "biblebackend.h"
#include "desktop.h"
#include "windowmanager.h"
#include <QApplication>
#include <QDir>
#include <QJSValue>
#include <QClipboard>
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
#include <QTcpServer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QQmlEngine>
#include <QTimer>
#include <QWindow>
#include "aofixture.h"

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
    // names: a book's own name in this edition (\h, \toc), by code; verses: John 3's count.
    static bool writeStructuredUsfm(const QString &path, const QString &id, const QMap<QString,QString> &names = {}, int verses = 36) {
        QJsonArray books;
        const QStringList codes{"GEN","MRK","JHN","ROM"};
        for (int b = 0; b < codes.size(); ++b) {
            QJsonArray chapters;
            const QList<int> numbers = b == 0 ? QList<int>{1,2} : b == 1 ? QList<int>{7} : b == 2 ? QList<int>{3,4} : QList<int>{8};
            for (int c : numbers) {
                QJsonArray content;
                for (int v = 1; v <= (codes[b] == "JHN" && c == 3 ? verses : 36); ++v)
                    content.append(AoFixture::verse(v, {QString("Original window fixture words %1:%2:%3.").arg(codes[b]).arg(c).arg(v)}));
                chapters.append(AoFixture::chapter(c, content));
            }
            books.append(AoFixture::book(codes[b], b + 1, chapters, names.value(codes[b])));
        }
        auto root = AoFixture::complete(id, books, "Original Window Bible");
        auto translation = root["translation"].toObject();
        if (id == "window-original") translation["shortName"] = "OWB";
        root["translation"] = translation;
        return AoFixture::write(path, root);
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
            seed.importAo(QUrl::fromLocalFile(fixture));
            QTRY_VERIFY_WITH_TIMEOUT(!seed.busy(),30000);
            QVERIFY2(seed.ready(),qPrintable(seed.error()));
        }
        desktop = std::make_unique<Desktop>();
        windows = std::make_unique<WindowManager>(desktop.get());
        windows->newWindow(); windows->newWindow();
        QCOMPARE(windows->count(), 2);
        QVERIFY(windows->backendAt(0)->ready()); QVERIFY(windows->backendAt(1)->ready());
        // Save notes on every change, as the assertions below expect (the app waits for a pause).
        for (int i = 0; i < windows->count(); ++i) windows->windowAt(i)->setProperty("noteSaveDelay",0);
    }
    void cleanup() {
        QCoreApplication::processEvents();
        windows.reset(); desktop.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        session.reset();
    }
    // The interface fetches nothing: a request through the windows' QML
    // engine (an image a label's text names) fails at once, and nothing
    // reaches the address it named; a plain manager does reach it.
    void interfaceNeverGoesOnline() {
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        const QUrl address(QString("http://127.0.0.1:%1/image.png").arg(server.serverPort()));
        auto *engine = qmlEngine(windows->windowAt(0)); QVERIFY(engine);
        QVERIFY(engine->networkAccessManager());
        std::unique_ptr<QNetworkReply> reply(engine->networkAccessManager()->get(QNetworkRequest(address)));
        QTRY_VERIFY(reply->isFinished());
        QVERIFY(reply->error() != QNetworkReply::NoError);
        QVERIFY(!server.hasPendingConnections());
        QNetworkAccessManager plain;
        std::unique_ptr<QNetworkReply> direct(plain.get(QNetworkRequest(address)));
        QVERIFY(server.waitForNewConnection(5000));
        direct->abort();
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
        b->toggleHighlight(); QVERIFY(a->selectionHighlighted());
        QVERIFY(a->deleteAnnotation(selection));
        QVERIFY(b->noteText().isEmpty()); QVERIFY(!b->selectionHighlighted());
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
            QVERIFY(q.exec("CREATE TRIGGER reject_notes BEFORE UPDATE ON annotations BEGIN SELECT RAISE(ABORT,'test save failure'); END"));
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
        a->importAo(QUrl::fromLocalFile(structuredUsfm));
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
        QVERIFY(q.exec("CREATE TRIGGER reject_editor BEFORE UPDATE ON annotations BEGIN SELECT RAISE(ABORT,'editor draft save failure'); END"));
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
        a->importAo(QUrl::fromLocalFile(structuredUsfm));
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
        a->importAo(QUrl::fromLocalFile(structuredUsfm));
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
        a->importAo(QUrl::fromLocalFile(structuredUsfm));
        QVERIFY2(a->busy(),qPrintable(a->error()));
        QTRY_VERIFY_WITH_TIMEOUT(!a->busy(),30000);
        QVERIFY2(a->error().isEmpty(),qPrintable(a->error()));
        const QString id = a->versionId();
        QCOMPARE(id,QString("window-original"));
        QVERIFY(!a->edition().isEmpty());
        QVERIFY(!a->versionAbbreviation().isEmpty());
        QString error;
        QVERIFY2(AoImporter::validateDatabase(installedPath(a),&error),qPrintable(error));
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
        a->importAo(QUrl::fromLocalFile(structuredUsfm));
        QTRY_VERIFY_WITH_TIMEOUT(!a->busy(),30000);
        QVERIFY2(a->error().isEmpty(),qPrintable(a->error()));
        QCOMPARE(a->versionId(),id); QCOMPARE(b->versionId(),id);
        QCOMPARE(b->selectionReference(),QString("Mark 7:16"));
        QCOMPARE(b->noteText(),QString("Source-numbered shared note"));
        QVERIFY(installedPath(b) != obsolete); QVERIFY(!QFile::exists(obsolete));
        QVERIFY2(AoImporter::validateDatabase(installedPath(b),&error),qPrintable(error));
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
            QVERIFY(q.exec("CREATE TRIGGER reject_drafts BEFORE UPDATE ON annotations BEGIN SELECT RAISE(ABORT,'draft disk failure'); END"));
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
                third->importAo(QUrl::fromLocalFile(structuredUsfm));
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
        a->importAo(QUrl::fromLocalFile(kjv)); QVERIFY(a->busy()); QVERIFY(b->busy());
        QVERIFY(!b->removeVersion("engnet",true));
        b->importAo(QUrl::fromLocalFile(net)); QVERIFY(!b->error().isEmpty());
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
        a->importAo(QUrl::fromLocalFile(net));
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
        a->importAo(QUrl::fromLocalFile(net)); QVERIFY(a->busy());
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
    // open --here navigates the window you read, switching it to a
    // translation asked for.
    void hereStaysInTheWindowYouRead() {
        const int count = windows->count();
        SingleInstance::Request here; here.reference = "Romans 8:2"; here.newWindow = false;
        bool done = false;
        windows->openRequest(here,[&](auto result) { QVERIFY2(result.ok,qPrintable(result.error)); done = true; },false);
        QTRY_VERIFY(done); QCOMPARE(windows->count(),count);
        QCOMPARE(windows->backendAt(count - 1)->selectionReference(),QString("Romans 8:2"));
        // --translation: by abbreviation or ID, in any case; the passage as
        // the palette reads it (compact).
        {
            BibleBackend other;
            other.importAo(QUrl::fromLocalFile(structuredUsfm));
            QTRY_VERIFY_WITH_TIMEOUT(!other.busy(),30000);
            QVERIFY2(other.ready(),qPrintable(other.error()));
        }
        auto *bible = windows->backendAt(count - 1);
        const QString first = bible->versionId();
        QTRY_VERIFY(!bible->versionIdFor("owb").isEmpty());
        here.reference = "jn316"; here.translation = "owb"; done = false;
        windows->openRequest(here,[&](auto result) { QVERIFY2(result.ok,qPrintable(result.error)); done = true; },false);
        QTRY_VERIFY(done); QCOMPARE(windows->count(),count);
        QCOMPARE(bible->versionId(),QString("window-original")); QCOMPARE(bible->selectionReference(),QString("John 3:16"));
        here.reference.clear(); here.translation = first.toUpper(); done = false;
        windows->openRequest(here,[&](auto result) { QVERIFY2(result.ok,qPrintable(result.error)); done = true; },false);
        QTRY_VERIFY(done); QCOMPARE(bible->versionId(),first); QCOMPARE(bible->selectionReference(),QString("John 3:16"));
        here.translation = "nothing"; done = false;
        windows->openRequest(here,[&](auto result) { QVERIFY(!result.ok); QVERIFY(result.error.contains("nothing")); done = true; },false);
        QTRY_VERIFY(done); QCOMPARE(bible->versionId(),first);
    }
    // Open beside: a window in another version that follows this one, and
    // that this one follows, until either closes.
    void openBesideFollowsBothWays() {
        {
            BibleBackend other;
            other.importAo(QUrl::fromLocalFile(structuredUsfm));
            QTRY_VERIFY_WITH_TIMEOUT(!other.busy(),30000);
            QVERIFY2(other.ready(),qPrintable(other.error()));
        }
        auto *leader = windows->windowAt(0); auto *bible = windows->backendAt(0);
        QTRY_VERIFY(!bible->versionIdFor("window-original").isEmpty());
        const QString version = bible->versionId();
        { BibleBackend reading; QVERIFY(reading.switchVersion(version)); } // The import made the other the version you read.
        QVERIFY(bible->navigate("John 3:16"));
        QTRY_VERIFY(!leader->property("restoringPosition").toBool());
        QVERIFY(!windows->openBeside(leader,"nothing")); QCOMPARE(windows->count(),2);
        QVERIFY(windows->openBeside(leader,"window-original")); QCOMPARE(windows->count(),3);
        auto *follower = windows->windowAt(2); auto *beside = windows->backendAt(2);
        QCOMPARE(windows->partnerOf(leader),follower); QCOMPARE(windows->partnerOf(follower),leader);
        // It reads the other version where this one is, without making it the version you read.
        QCOMPARE(beside->versionId(),QString("window-original"));
        QTRY_COMPARE(beside->selectionReference(),QString("John 3:16"));
        { BibleBackend fresh; QCOMPARE(fresh.versionId(),version); }
        // Each names the other's version, in its key header too.
        QCOMPARE(leader->property("followPartner").toString(),beside->versionAbbreviation());
        QCOMPARE(follower->property("followPartner").toString(),bible->versionAbbreviation());
        QTRY_VERIFY(windowObject(leader,"followStatus")->property("visible").toBool());
        QCOMPARE(windowObject(leader,"followTitle")->property("text").toString(),"Following " + beside->versionAbbreviation());
        QCOMPARE(windowObject(follower,"followHints")->property("plainText").toString(),QString("stop following"));
        // The leader moves: the follower follows; and the other way.
        QVERIFY(bible->navigate("Romans 8:2"));
        QTRY_COMPARE(beside->selectionReference(),QString("Romans 8:2"));
        QTRY_VERIFY(!follower->property("restoringPosition").toBool());
        QVERIFY(beside->navigate("Genesis 2:3"));
        QTRY_COMPARE(bible->selectionReference(),QString("Genesis 2:3"));
        QCOMPARE(bible->versionId(),version); // Each keeps its version.
        // Scrolling: the follower keeps the leader's top verse at its top.
        QTRY_VERIFY(!leader->property("restoringPosition").toBool() && !follower->property("restoringPosition").toBool());
        QTRY_VERIFY(!leader->property("followApplying").toBool() && !follower->property("followApplying").toBool());
        QVERIFY(QMetaObject::invokeMethod(leader,"setReaderY",Q_ARG(QVariant,600.0)));
        const auto top = [](QWindow *window) {
            QVariant location; QMetaObject::invokeMethod(window,"followLocation",Q_RETURN_ARG(QVariant,location));
            return location.value<QJSValue>().toVariant().toMap().value("topVerse").toInt();
        };
        QVERIFY(top(leader) > 1);
        QTRY_COMPARE(top(follower),top(leader));
        // Closing either unlinks the other.
        follower->close();
        QTRY_COMPARE(windows->count(),2);
        QCOMPARE(windows->partnerOf(leader),nullptr);
        QCOMPARE(leader->property("followPartner").toString(),QString());
        QVERIFY(bible->navigate("John 3:1")); QCOMPARE(bible->selectionReference(),QString("John 3:1"));
    }
    // omaword lookup: a small window of its own, the text typed; enter shows
    // the passage, y copies it, enter again opens it in the reader used last.
    void lookupWindowShowsCopiesAndOpens() {
        const int count = windows->count();
        auto *reader = windows->windowAt(0); auto *bible = windows->backendAt(0);
        QVERIFY(bible->navigate("John 3:2"));
        SingleInstance::Request request; request.lookup = true; request.newWindow = false; request.reference = "rom 8:2";
        bool done = false;
        windows->openRequest(request,[&](auto result) { QVERIFY2(result.ok,qPrintable(result.error)); done = true; },false);
        QTRY_VERIFY(done); QCOMPARE(windows->count(),count); // Not a reader window.
        auto *lookup = windows->lookupWindow(); QVERIFY(lookup);
        QCOMPARE(lookup->title(),QString("Look up · OmaWord"));
        // Resizable, above a size its parts fit in.
        QVERIFY(lookup->maximumSize().width() > lookup->width() && lookup->maximumSize().height() > lookup->height());
        QVERIFY(lookup->minimumSize().width() < lookup->width() && lookup->minimumSize().height() < lookup->height());
        const QSize resized(lookup->width() + 120, lookup->height() + 90);
        QCOMPARE(windowObject(lookup,"lookupQuery")->property("text").toString(),QString("rom 8:2"));
        QTRY_VERIFY(lookup->property("candidates").value<QJSValue>().property("length").toInt() > 0);
        // One version: its name, nothing to choose (tab changes nothing).
        auto *version = windowObject(lookup,"lookupVersion");
        QCOMPARE(version->property("text").toString(),bible->versionAbbreviation());
        QVERIFY(!version->property("choosable").toBool());
        // Only its name: no frame, no glyph, no tooltip.
        QVERIFY(!windowObject(lookup,"lookupVersionFrame")->property("visible").toBool());
        QVERIFY(!windowObject(lookup,"lookupVersionGlyph")->property("visible").toBool());
        QCOMPARE(version->property("tip").toString(),QString());
        QVERIFY(!windowObject(lookup,"lookupHints")->property("plainText").toString().contains("tab"));
        lookup->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(lookup));
        // A pointing hand over the hints a click acts on (enter show, esc
        // close), never over the others (↑ ↓ choose).
        const auto cursorOver = [&](const QString &part) {
            auto *hint = qobject_cast<QQuickItem *>(windowObject(lookup,"lookupHintsPart:" + part));
            if (!hint) return Qt::BlankCursor;
            QTest::mouseMove(lookup, hint->mapToScene(QPointF(hint->width() / 2, hint->height() / 2)).toPoint());
            QCoreApplication::processEvents();
            return lookup->cursor().shape();
        };
        QCOMPARE(cursorOver("show"), Qt::PointingHandCursor);
        QCOMPARE(cursorOver("choose"), Qt::ArrowCursor);
        QCOMPARE(cursorOver("close"), Qt::PointingHandCursor);
        QVERIFY(QMetaObject::invokeMethod(lookup,"show",Q_ARG(QVariant,0)));
        QVERIFY(lookup->property("reading").toBool());
        QTest::keyClick(lookup,Qt::Key_Tab); QCOMPARE(version->property("text").toString(),bible->versionAbbreviation());
        QCOMPARE(windowObject(lookup,"lookupTitle")->property("text").toString(),QString("Romans 8:2"));
        QVERIFY(windowObject(lookup,"lookupText")->property("text").toString().contains("ROM:8:2"));
        QGuiApplication::clipboard()->clear();
        QVERIFY(QMetaObject::invokeMethod(lookup,"copy",Q_ARG(QVariant,false)));
        QVERIFY(QGuiApplication::clipboard()->text().endsWith(QStringLiteral("\u2014 Romans 8:2")));
        QCOMPARE(windowObject(lookup,"lookupStatus")->property("text").toString(),QString("Romans 8:2 copied"));
        QVERIFY(QMetaObject::invokeMethod(lookup,"copy",Q_ARG(QVariant,true)));
        QVERIFY(!QGuiApplication::clipboard()->text().contains(QStringLiteral("\u2014")));
        QCOMPARE(bible->selectionReference(),QString("John 3:2")); // Nothing moved.
        // A second lookup replaces it, afresh, at the size the first was given.
        {
            // (Once the window has heard of it: its size changes before its signals arrive.)
            QSignalSpy widths(lookup, &QWindow::widthChanged), heights(lookup, &QWindow::heightChanged);
            lookup->resize(resized); QTRY_VERIFY(widths.count() && heights.count());
            QCOMPARE(lookup->size(), resized);
        }
        QPointer<QWindow> first(lookup);
        QVERIFY(windows->openLookup("jn316"));
        QTRY_VERIFY(!first); lookup = windows->lookupWindow(); QVERIFY(lookup);
        QTRY_COMPARE(lookup->size(), resized);
        QVERIFY(QMetaObject::invokeMethod(lookup,"show",Q_ARG(QVariant,0)));
        // enter: the passage in the reader used last (here, none was active: the newest), and the lookup closes.
        QVERIFY(QMetaObject::invokeMethod(lookup,"openInReader"));
        QTRY_COMPARE(windows->backendAt(count - 1)->selectionReference(),QString("John 3:16"));
        QTRY_VERIFY(!windows->lookupWindow());
        QCOMPARE(windows->count(),count);
        QCOMPARE(windows->backendAt(count - 1)->versionId(),QString("engnet"));
        Q_UNUSED(reader);
        // With another version installed the version is a choice: tab and
        // shift tab step through them, as its menu chooses, the passage shown
        // following at once; enter opens it in the reader in that version.
        {
            BibleBackend other; other.importAo(QUrl::fromLocalFile(structuredUsfm));
            QTRY_VERIFY_WITH_TIMEOUT(!other.busy(),30000); QVERIFY2(other.ready(),qPrintable(other.error()));
            QVERIFY(other.switchVersion("engnet")); // The import made it the version you read.
        }
        QVERIFY(windows->openLookup("rom 8:2")); lookup = windows->lookupWindow(); QVERIFY(lookup);
        version = windowObject(lookup,"lookupVersion");
        QCOMPARE(version->property("text").toString(),QString("NET")); QVERIFY(version->property("choosable").toBool());
        // As tall as the input beside it, level with it.
        {
            auto *button = qobject_cast<QQuickItem *>(version), *input = qobject_cast<QQuickItem *>(windowObject(lookup,"lookupQuery"));
            QTRY_COMPARE(button->height(), input->height());
            QCOMPARE(button->mapToScene(QPointF()).y(), input->mapToScene(QPointF()).y());
        }
        QVERIFY(windowObject(lookup,"lookupHints")->property("plainText").toString().contains("tab version"));
        const auto shownVersion = [&] {
            const auto shown = lookup->property("shown");
            return (shown.canConvert<QJSValue>() ? shown.value<QJSValue>().toVariant().toMap() : shown.toMap()).value("versionId").toString();
        };
        lookup->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(lookup)); // Hyprland focuses it.
        QVERIFY(QMetaObject::invokeMethod(lookup,"show",Q_ARG(QVariant,0)));
        QTest::keyClick(lookup,Qt::Key_Tab);
        QCOMPARE(version->property("text").toString(),QString("OWB")); QCOMPARE(shownVersion(),QString("window-original"));
        QCOMPARE(windowObject(lookup,"lookupTitle")->property("text").toString(),QString("Romans 8:2"));
        QVERIFY(windowObject(lookup,"lookupText")->property("text").toString().contains("ROM:8:2"));
        QTest::keyClick(lookup,Qt::Key_Backtab,Qt::ShiftModifier);
        QCOMPARE(version->property("text").toString(),QString("NET")); QCOMPARE(shownVersion(),QString("engnet"));
        QCOMPARE(lookup->property("chosenVersion").toString(),QString()); // Back where it began.
        // A button, framed, its glyph two arrows: with two versions a click
        // switches to the other (no list), and a right click does too.
        auto *menu = windowObject(lookup,"lookupVersionMenu");
        QVERIFY(windowObject(lookup,"lookupVersionFrame")->property("visible").toBool());
        QCOMPARE(windowObject(lookup,"lookupVersionGlyph")->property("glyph").toString(),QString("swap"));
        QCOMPARE(version->property("tip").toString(),QString::fromUtf8("Switch to OWB · tab"));
        const auto clickVersion = [&](Qt::MouseButton button) {
            auto *item = qobject_cast<QQuickItem *>(version);
            QTest::mouseClick(lookup,button,{},item->mapToScene(QPointF(item->width() / 2,item->height() / 2)).toPoint());
            QCoreApplication::processEvents();
        };
        clickVersion(Qt::LeftButton); QCOMPARE(shownVersion(),QString("window-original"));
        QVERIFY(!menu->property("visible").toBool());
        QCOMPARE(version->property("tip").toString(),QString::fromUtf8("Switch to NET · tab"));
        clickVersion(Qt::RightButton); QCOMPARE(shownVersion(),QString("engnet"));
        clickVersion(Qt::LeftButton); QCOMPARE(shownVersion(),QString("window-original"));
        QVERIFY(!menu->property("visible").toBool());
        // The list follows too, while choosing.
        QVERIFY(QMetaObject::invokeMethod(lookup,"back"));
        QTest::keyClick(lookup,Qt::Key_Tab); QCOMPARE(version->property("text").toString(),QString("NET"));
        QTest::keyClick(lookup,Qt::Key_Tab); QCOMPARE(version->property("text").toString(),QString("OWB"));
        QCOMPARE(windowObject(lookup,"lookupQuery")->property("text").toString(),QString("rom 8:2"));
        QVERIFY(QMetaObject::invokeMethod(lookup,"show",Q_ARG(QVariant,0))); QCOMPARE(shownVersion(),QString("window-original"));
        QVERIFY(QMetaObject::invokeMethod(lookup,"openInReader"));
        QTRY_COMPARE(windows->backendAt(count - 1)->versionId(),QString("window-original"));
        QTRY_COMPARE(windows->backendAt(count - 1)->selectionReference(),QString("Romans 8:2"));
        QTRY_VERIFY(!windows->lookupWindow());
        // A translation not installed: nothing opens.
        QVERIFY(!windows->openLookup("jn316","nothing")); QVERIFY(!windows->lookupWindow());
    }
    // The lookup window steps through every installed version in turn, and
    // keeps the passage shown by book, chapter, and verse, whatever a
    // version calls the book (OJB: Bereshis, Romiyim) and whatever was typed.
    void lookupCyclesEveryVersionKeepingThePassage() {
        for (const auto &[id, names] : {std::pair{QString("window-original"), QMap<QString,QString>{}},
                 std::pair{QString("renamed"), QMap<QString,QString>{{"GEN", "Bereshis"}, {"ROM", "Romiyim"}}},
                 std::pair{QString("fourth"), QMap<QString,QString>{}}}) {
            const QString path = fixtures.filePath(id + "_usfm.zip");
            QVERIFY(writeStructuredUsfm(path, id, names));
            BibleBackend other; other.importAo(QUrl::fromLocalFile(path));
            QTRY_VERIFY_WITH_TIMEOUT(!other.busy(),30000); QVERIFY2(other.ready(),qPrintable(other.error()));
        }
        { BibleBackend reading; QVERIFY(reading.switchVersion("engnet")); }
        QVERIFY(windows->openLookup("rom 8:2")); auto *lookup = windows->lookupWindow(); QVERIFY(lookup);
        lookup->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(lookup));
        auto *backend = qobject_cast<BibleBackend *>(lookup->property("backend").value<QObject *>()); QVERIFY(backend);
        QStringList order;
        for (const auto &row : lookup->property("versionOrder").value<QJSValue>().toVariant().toList()) order << row.toMap()["id"].toString();
        QCOMPARE(order.size(), 4); QCOMPARE(order.first(), QString("engnet"));
        // tab: each version in turn, back to the first; shift tab the other way.
        QStringList seen;
        for (int i = 0; i < 4; ++i) { QTest::keyClick(lookup, Qt::Key_Tab); seen << backend->versionId(); }
        QCOMPARE(seen, order.mid(1) << order.first());
        seen.clear();
        for (int i = 0; i < 4; ++i) { QTest::keyClick(lookup, Qt::Key_Backtab, Qt::ShiftModifier); seen << backend->versionId(); }
        QCOMPARE(seen, QStringList({order[3], order[2], order[1], order[0]}));
        // With more than two, the version button opens the list of them (its
        // glyph a chevron): under it, its right edge on the button's, the
        // version read marked and current; choosing one reads it.
        auto *version = qobject_cast<QQuickItem *>(windowObject(lookup, "lookupVersion"));
        auto *menu = windowObject(lookup, "lookupVersionMenu");
        QCOMPARE(windowObject(lookup, "lookupVersionGlyph")->property("glyph").toString(), QString("chevron-down"));
        QVERIFY(version->property("tip").toString().startsWith("Choose a version"));
        const QPoint centre = version->mapToScene(QPointF(version->width() / 2, version->height() / 2)).toPoint();
        QTest::mouseClick(lookup, Qt::LeftButton, {}, centre); QTRY_VERIFY(menu->property("visible").toBool());
        QStringList listed;
        for (const auto &entry : menu->property("layout").value<QJSValue>().toVariant().toList()) {
            listed << entry.toMap()["id"].toString();
            QCOMPARE(entry.toMap()["shortcut"].toString(), entry.toMap()["id"] == backend->versionId() ? QString("reading") : QString());
        }
        QCOMPARE(listed, order);
        QCOMPARE(menu->property("currentIndex").toInt(), 0);
        const QPointF corner = version->mapToScene(QPointF(version->width(), version->height()));
        QCOMPARE(menu->property("x").toDouble() + menu->property("width").toDouble(), qRound(corner.x()) * 1.0);
        QVERIFY(menu->property("y").toDouble() >= corner.y());
        // A click on the button again closes it, acting on nothing else.
        QTest::mouseClick(lookup, Qt::LeftButton, {}, centre); QTRY_VERIFY(!menu->property("visible").toBool());
        QCoreApplication::processEvents();
        QVERIFY(!menu->property("visible").toBool()); QCOMPARE(backend->versionId(), order[0]);
        QTest::mouseClick(lookup, Qt::LeftButton, {}, centre); QTRY_VERIFY(menu->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(menu, "triggerId", Q_ARG(QVariant, order[2])));
        QTRY_COMPARE(backend->versionId(), order[2]);
        // Opened again, the version read is the current row.
        QTest::mouseClick(lookup, Qt::LeftButton, {}, centre); QTRY_VERIFY(menu->property("visible").toBool());
        QCOMPARE(menu->property("currentIndex").toInt(), 2);
        QTest::keyClick(lookup, Qt::Key_Escape); QTRY_VERIFY(!menu->property("visible").toBool());
        // A right click reads the next, as tab does, with no list.
        QTest::mouseClick(lookup, Qt::RightButton, {}, centre); QCOMPARE(backend->versionId(), order[3]);
        QVERIFY(!menu->property("visible").toBool());
        QTest::mouseClick(lookup, Qt::RightButton, {}, centre); QCOMPARE(backend->versionId(), order[0]);
        // The passage shown, by its scope.
        QVERIFY(QMetaObject::invokeMethod(lookup, "show", Q_ARG(QVariant, 0)));
        const auto shown = [&] {
            const auto value = lookup->property("shown");
            return value.canConvert<QJSValue>() ? value.value<QJSValue>().toVariant().toMap() : value.toMap();
        };
        QCOMPARE(shown()["reference"].toString(), QString("Romans 8:2"));
        const auto scope = shown()["scope"].toMap();
        QCOMPARE(scope["book"].toInt(), 45); QCOMPARE(scope["chapter"].toInt(), 8); QCOMPARE(scope["start"].toInt(), 2);
        while (backend->versionId() != "renamed") QTest::keyClick(lookup, Qt::Key_Tab);
        QCOMPARE(shown()["reference"].toString(), QString("Romiyim 8:2")); QCOMPARE(shown()["scope"].toMap(), scope);
        QCOMPARE(windowObject(lookup, "lookupTitle")->property("text").toString(), QString("Romiyim 8:2"));
        // Whatever the typed text would match now, the passage stays (the
        // reader typed g11 and chose Genesis 1:1; Genesis 11 matched first).
        lookup->setProperty("candidates", QVariantList());
        windowObject(lookup, "lookupQuery")->setProperty("text", "jo");
        while (backend->versionId() != "engnet") QTest::keyClick(lookup, Qt::Key_Tab);
        QCOMPARE(shown()["reference"].toString(), QString("Romans 8:2")); QCOMPARE(shown()["scope"].toMap(), scope);
        QVERIFY(windowObject(lookup, "lookupStatus")->property("text").toString().isEmpty());
        // A whole chapter keeps its scope too.
        QVERIFY(QMetaObject::invokeMethod(lookup, "back"));
        windowObject(lookup, "lookupQuery")->setProperty("text", "gen 2"); QVERIFY(QMetaObject::invokeMethod(lookup, "refresh"));
        QVERIFY(QMetaObject::invokeMethod(lookup, "show", Q_ARG(QVariant, 0)));
        QCOMPARE(shown()["reference"].toString(), QString("Genesis 2"));
        while (backend->versionId() != "renamed") QTest::keyClick(lookup, Qt::Key_Tab);
        QCOMPARE(shown()["reference"].toString(), QString("Bereshis 2"));
        QCOMPARE(shown()["scope"].toMap()["start"].toInt(), 0); QCOMPARE(shown()["verses"].toList().size(), 36);
        lookup->close(); QTRY_VERIFY(!windows->lookupWindow());
    }
    // Following keeps up frame by frame: each move of one window reaches the
    // other within the same turn of the event loop, scrolling and selecting
    // alike, and a selection in the chapter shown moves without navigating
    // (no reload, no restore), also where the other version lacks the verses.
    void followTracksEveryFrame_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("verse") << "verse"; QTest::newRow("paragraph") << "paragraph"; QTest::newRow("block") << "block";
    }
    void followTracksEveryFrame() {
        QFETCH(QString, mode);
        // The other version ends John 3 at verse 30.
        {
            const QString path = fixtures.filePath("shorter_usfm.zip");
            QVERIFY(writeStructuredUsfm(path, "shorter", {}, 30));
            BibleBackend other; other.importAo(QUrl::fromLocalFile(path));
            QTRY_VERIFY_WITH_TIMEOUT(!other.busy(),30000); QVERIFY2(other.ready(),qPrintable(other.error()));
            QVERIFY(other.switchVersion("engnet"));
        }
        auto *leader = windows->windowAt(0); auto *bible = windows->backendAt(0);
        QTRY_VERIFY(!bible->versionIdFor("shorter").isEmpty());
        bible->setReadingMode(mode);
        QVERIFY(bible->navigate("John 3")); QTRY_VERIFY(!leader->property("restoringPosition").toBool());
        QVERIFY(windows->openBeside(leader, "shorter"));
        auto *follower = windows->windowAt(2); auto *beside = windows->backendAt(2);
        QTRY_COMPARE(beside->passage(), bible->passage());
        QTRY_VERIFY(!follower->property("restoringPosition").toBool() && !follower->property("followApplying").toBool());
        leader->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(leader));
        QVERIFY(QMetaObject::invokeMethod(leader, "readerFocus"));
        // The same place: the same top verse, as far into it (within a pixel or
        // so: a move under a pixel is not made), or as far above the first.
        const auto top = [](QWindow *window) {
            QVariant location; QMetaObject::invokeMethod(window, "followLocation", Q_RETURN_ARG(QVariant, location));
            return location.value<QJSValue>().toVariant().toMap();
        };
        const auto apart = [&](QWindow *a, QWindow *b) {
            const auto x = top(a), y = top(b);
            if (x["topVerse"] == y["topVerse"] && std::abs(x["topFraction"].toDouble() - y["topFraction"].toDouble()) < 0.1
                    && std::abs(x["topAbove"].toDouble() - y["topAbove"].toDouble()) < 1.5) return QString();
            return QString("%1 %2 %3 / %4 %5 %6").arg(x["topVerse"].toInt()).arg(x["topFraction"].toDouble()).arg(x["topAbove"].toDouble())
                .arg(y["topVerse"].toInt()).arg(y["topFraction"].toDouble()).arg(y["topAbove"].toDouble());
        };
        const auto bottom = [](QWindow *window) {
            auto reading = windowObject(window, "reading");
            return reading->property("contentHeight").toDouble() - reading->property("height").toDouble();
        };
        QSignalSpy navigations(beside, &BibleBackend::navigationChanged);
        // Scrolling: each step is matched before the next one, from the
        // chapter title on; past the end of the other's shorter chapter, it
        // waits at its end.
        QVERIFY(bottom(follower) > 100);
        for (double y = 0; y <= bottom(follower); y += 23) {
            QVERIFY(QMetaObject::invokeMethod(leader, "setReaderY", Q_ARG(QVariant, y)));
            QCoreApplication::processEvents();
            QVERIFY2(apart(leader, follower).isEmpty(), qPrintable(QString("at %1: ").arg(y) + apart(leader, follower)));
        }
        QVERIFY(QMetaObject::invokeMethod(leader, "setReaderY", Q_ARG(QVariant, bottom(leader))));
        QCoreApplication::processEvents();
        QCOMPARE(windowObject(follower, "reading")->property("contentY").toDouble(), bottom(follower));
        // Selecting: j moves the other window's selection with it, and its
        // scroll follows the slide frame by frame; nothing navigates.
        bible->selectVerses(5, 5); QCoreApplication::processEvents();
        QCOMPARE(beside->selectionReference(), QString("John 3:5"));
        for (int step = 6; step <= 9; ++step) {
            QTest::keyClick(leader, Qt::Key_J);
            QTRY_COMPARE(beside->selectionReference(), QString("John 3:%1").arg(step));
            QTRY_VERIFY(!leader->property("restoringPosition").toBool() && !windowObject(leader, "readerSlide")->property("running").toBool());
            QCoreApplication::processEvents();
            QVERIFY2(apart(leader, follower).isEmpty(), qPrintable(apart(leader, follower)));
        }
        // Verses the other version lacks: no selection there, the scroll still follows.
        bible->selectVerses(33, 33); QCoreApplication::processEvents();
        QCOMPARE(beside->selectedStart(), 0);
        QVERIFY(QMetaObject::invokeMethod(leader, "setReaderY", Q_ARG(QVariant, bottom(follower) / 2)));
        QCoreApplication::processEvents(); QVERIFY2(apart(leader, follower).isEmpty(), qPrintable(apart(leader, follower)));
        QCOMPARE(navigations.count(), 0);
        QVERIFY(!follower->property("restoringPosition").toBool());
        // Another chapter does navigate there; the other window moving during
        // that restore takes the lead, and the two stay linked.
        QVERIFY(bible->navigate("John 4:2"));
        QTRY_COMPARE(beside->selectionReference(), QString("John 4:2"));
        follower->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(follower));
        QVERIFY(QMetaObject::invokeMethod(follower, "readerFocus"));
        QTest::keyClick(follower, Qt::Key_J);
        QTRY_COMPARE(bible->selectionReference(), QString("John 4:3"));
        QTRY_VERIFY(!follower->property("followApplying").toBool() && !leader->property("followApplying").toBool());
        QTest::keyClick(follower, Qt::Key_J); QTRY_COMPARE(bible->selectionReference(), QString("John 4:4"));
        follower->close(); QTRY_COMPARE(windows->count(), 2);
    }
    // j on a chapter's last verse goes on to the next chapter, k on its
    // first back to the previous one, tapped or held, with a window beside
    // following: the two stay on the same verse and neither pulls the other back.
    void verseStepsCrossChaptersWithAWindowBeside_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("verse") << "verse"; QTest::newRow("paragraph") << "paragraph"; QTest::newRow("block") << "block";
    }
    void verseStepsCrossChaptersWithAWindowBeside() {
        QFETCH(QString, mode);
        BibleBackend other; other.importAo(QUrl::fromLocalFile(structuredUsfm));
        QTRY_VERIFY_WITH_TIMEOUT(!other.busy(),30000); QVERIFY(other.switchVersion("engnet"));
        auto *leader = windows->windowAt(0); auto *bible = windows->backendAt(0);
        bible->setReadingMode(mode);
        QVERIFY(bible->navigate("John 3:36")); QTRY_VERIFY(!leader->property("restoringPosition").toBool());
        QVERIFY(windows->openBeside(leader, "window-original"));
        auto *follower = windows->windowAt(2); auto *beside = windows->backendAt(2);
        QTRY_COMPARE(beside->selectionReference(), QString("John 3:36"));
        QTRY_VERIFY(!follower->property("restoringPosition").toBool());
        leader->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(leader));
        QVERIFY(QMetaObject::invokeMethod(leader, "readerFocus"));
        const auto both = [&](const QString &reference) {
            return bible->selectionReference() == reference && beside->selectionReference() == reference
                && !leader->property("restoringPosition").toBool() && !follower->property("restoringPosition").toBool();
        };
        QTest::keyClick(leader, Qt::Key_J); QTRY_VERIFY(both("John 4:1"));
        QTest::keyClick(leader, Qt::Key_K); QTRY_VERIFY(both("John 3:36"));
        QTest::keyClick(leader, Qt::Key_J); QTest::keyClick(leader, Qt::Key_J); QTRY_VERIFY(both("John 4:2"));
        // Held k from John 4:2 runs back into John 3; released, both rest on one verse there.
        QTest::keyPress(leader, Qt::Key_K); QTest::qWait(700); QTest::keyRelease(leader, Qt::Key_K);
        QTRY_VERIFY(!leader->property("restoringPosition").toBool() && !follower->property("restoringPosition").toBool());
        QTest::qWait(300);
        QCOMPARE(bible->chapter(), 3); QCOMPARE(beside->selectionReference(), bible->selectionReference());
        follower->close(); QTRY_COMPARE(windows->count(), 2);
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
