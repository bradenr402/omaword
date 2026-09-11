#include <QtTest/QTest>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include "biblebackend.h"
#include "desktop.h"
#include "epubimporter.h"

// Run: QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software ./tst_ui
// Optional: OMABIBLE_TEST_EPUB=/path/to/NETBIBLE21.epub. Without it a full,
// canonically numbered synthetic database is generated; no Bible text is bundled.
class UiTest : public QObject
{
    Q_OBJECT
    QTemporaryDir fixtures;
    std::unique_ptr<QTemporaryDir> session;
    std::unique_ptr<BibleBackend> bible;
    std::unique_ptr<Desktop> desktop;
    std::unique_ptr<QQmlApplicationEngine> engine;
    QQuickWindow *window = nullptr;
    QStringList qmlWarnings;
    QString fixture;

    static QQuickItem *visualItem(QQuickItem *root, const QString &name) {
        if (root->objectName() == name) return root;
        for (auto child : root->childItems())
            if (auto found = visualItem(child,name)) return found;
        return nullptr;
    }
    QObject *object(const char *name) const {
        if (!window) return nullptr;
        const auto found = window->findChild<QObject *>(QString::fromUtf8(name));
        return found ? found : visualItem(window->contentItem(),QString::fromUtf8(name));
    }
    QQuickItem *item(const char *name) const { return qobject_cast<QQuickItem *>(object(name)); }
    QVariant prop(const char *name, const char *property) const {
        const auto obj = object(name);
        return obj ? obj->property(property) : QVariant();
    }
    void key(Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QTest::keyClick(window, key, modifiers);
        QCoreApplication::processEvents();
    }
    void type(const QString &text) {
        // QWindow has keyClick, but no keyClicks overload. Let Qt translate each
        // Latin-1 key as it would a real keyboard, including shifted punctuation.
        for (const QChar c : text) {
            if (c == '\n') key(Qt::Key_Return);
            else QTest::keyClick(window, c.toLatin1());
        }
        QCoreApplication::processEvents();
    }
    void click(QQuickItem *target, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QVERIFY(target);
        QVERIFY(target->isVisible());
        const QPoint point = target->mapToScene(QPointF(target->width() / 2, qMin(target->height() / 2, 16.0))).toPoint();
        QVERIFY2(QRect(QPoint(0, 0), window->size()).contains(point), qPrintable(QString("Click outside window: %1,%2").arg(point.x()).arg(point.y())));
        QTest::mouseClick(window, Qt::LeftButton, modifiers, point);
        QCoreApplication::processEvents();
    }
    void reference(const QString &text) {
        key(Qt::Key_K, Qt::ControlModifier);
        QTRY_VERIFY(prop("passagePalette", "opened").toBool());
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type(text);
        QCOMPARE(prop("referenceInput", "text").toString(), text);
        key(Qt::Key_Return);
        QTRY_VERIFY(!prop("passagePalette", "visible").toBool());
        QTRY_VERIFY(!window->property("restoringPosition").toBool());
    }
    void capture(const QString &name) {
        const QString directory = qEnvironmentVariable("OMABIBLE_UI_SCREENSHOTS");
        if (directory.isEmpty()) return;
        QVERIFY(QDir(directory).exists());
        QSignalSpy frames(window,&QQuickWindow::frameSwapped);
        window->update();
        QTRY_VERIFY(frames.count() > 0);
        QTest::qWait(150);
        QVERIFY(window->grabWindow().save(QDir(directory).filePath(name + ".png")));
    }
    bool makeSyntheticBible() {
        bool ok = true;
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "ui-fixture");
            db.setDatabaseName(fixture);
            if (!db.open()) return false;
            QSqlQuery q(db);
            ok = q.exec("CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL)")
                && q.exec("INSERT INTO metadata VALUES('edition','NET Bible 2.1'),('schema','1')")
                && q.exec("CREATE TABLE verses(book INTEGER,chapter INTEGER,number INTEGER,text TEXT,html TEXT,heading TEXT,poetry INTEGER,PRIMARY KEY(book,chapter,number))")
                && db.transaction();
            q.prepare("INSERT INTO verses VALUES(?,?,?,?,?,?,?)");
            for (int b = 1; ok && b <= 66; ++b) {
                for (int c = 1; ok && c <= EpubImporter::canon()[b-1].chapters; ++c) {
                    for (int v = 1; ok && v <= EpubImporter::chapterMaximum(b,c); ++v) {
                        if (EpubImporter::omittedVerse(b,c,v)) continue;
                        const QString text = QString("Synthetic reader fixture %1:%2. This is original test text for checking comfortable wrapping, verse selection, and annotation workflows. A second sentence gives the reading surface enough height to exercise scrolling and restoration.").arg(c).arg(v);
                        q.bindValue(0,b); q.bindValue(1,c); q.bindValue(2,v);
                        q.bindValue(3,text); q.bindValue(4,text);
                        q.bindValue(5,v == 1 ? QString("Reader integration fixture") : QString(""));
                        q.bindValue(6,0);
                        ok = q.exec();
                    }
                }
            }
            ok = db.commit() && ok;
            if (!ok) qWarning() << q.lastError();
        }
        QSqlDatabase::removeDatabase("ui-fixture");
        QString error;
        return ok && EpubImporter::validateDatabase(fixture, &error);
    }
    void loadUi() {
        bible = std::make_unique<BibleBackend>();
        desktop = std::make_unique<Desktop>();
        engine = std::make_unique<QQmlApplicationEngine>();
        connect(engine.get(), &QQmlEngine::warnings, this, [this](const QList<QQmlError> &errors) {
            for (const auto &error : errors) qmlWarnings.append(error.toString());
        });
        engine->rootContext()->setContextProperty("bible", bible.get());
        engine->rootContext()->setContextProperty("desktop", desktop.get());
        engine->load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY2(!engine->rootObjects().isEmpty(), qPrintable(qmlWarnings.join('\n')));
        window = qobject_cast<QQuickWindow *>(engine->rootObjects().first());
        QVERIFY(window);
        window->show();
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QTRY_VERIFY(window->isActive());
        QTRY_VERIFY(!window->property("restoringPosition").toBool());
        QVERIFY(item("reading"));
    }
    void unloadUi() {
        if (window) window->close();
        engine.reset(); window = nullptr;
        desktop.reset(); bible.reset();
    }
    QVariantMap saved(int verse) const {
        for (const auto &row : bible->collection()) {
            const auto map = row.toMap();
            if (map.value("book").toInt() == 43 && map.value("chapter").toInt() == 3
                && map.value("start").toInt() == verse && map.value("end").toInt() == verse) return map;
        }
        return {};
    }
private slots:
    void initTestCase() {
        QVERIFY(fixtures.isValid());
        fixture = fixtures.filePath("bible.sqlite");
        const QString epub = qEnvironmentVariable("OMABIBLE_TEST_EPUB");
        if (epub.isEmpty()) {
            QVERIFY(makeSyntheticBible());
        } else {
            const auto imported = EpubImporter::importFile(epub, fixture);
            QVERIFY2(imported.ok, qPrintable(imported.error));
        }
    }
    void init() {
        session = std::make_unique<QTemporaryDir>();
        QVERIFY(session->isValid());
        qputenv("XDG_DATA_HOME", session->filePath("data").toUtf8());
        const auto data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QVERIFY(QDir().mkpath(data));
        const QString testName = QString::fromLatin1(QTest::currentTestFunction());
        if (testName != "emptyAndImportError" && testName != "asyncFirstImportLayout")
            QVERIFY(QFile::copy(fixture, data + "/bible.sqlite"));
        qmlWarnings.clear();
        loadUi();
    }
    void cleanup() {
        unloadUi();
        QVERIFY2(qmlWarnings.isEmpty(), qPrintable(qmlWarnings.join('\n')));
        session.reset();
    }
    void emptyAndImportError() {
        QVERIFY(!bible->ready());
        QVERIFY(item("setupScroll")->isVisible());
        QVERIFY(!item("readerSurface")->isVisible());
        window->resize(640,480);
        key(Qt::Key_K, Qt::ControlModifier);
        QTRY_VERIFY(prop("passagePalette", "opened").toBool());
        QVERIFY(prop("referenceInput", "text").toString().startsWith('>'));
        key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("passagePalette", "visible").toBool());
        const auto invalidPath = session->filePath("invalid.epub");
        QFile invalid(invalidPath);
        QVERIFY(invalid.open(QIODevice::WriteOnly));
        invalid.write("Invalid EPUB test fixture"); invalid.close();
        bible->importEpub(QUrl::fromLocalFile(invalidPath));
        QVERIFY(bible->busy());
        QVERIFY(prop("busyPopup","visible").toBool());
        QTRY_VERIFY(!bible->busy());
        QVERIFY(!bible->error().isEmpty());
        QVERIFY(item("errorMessage")->isVisible());
        QCOMPARE(prop("errorMessage","text").toString(),bible->error());
        QVERIFY(!bible->ready());
        QVERIFY(item("setupScroll")->isVisible());
    }
    void asyncFirstImportLayout() {
        const QString epub = qEnvironmentVariable("OMABIBLE_TEST_EPUB");
        if (epub.isEmpty()) QSKIP("Set OMABIBLE_TEST_EPUB to exercise asynchronous first import into the loaded UI.");
        QVERIFY(!bible->ready());
        QVERIFY(item("setupScroll")->isVisible());
        QVERIFY(!item("readerSurface")->isVisible());
        QVERIFY(desktop->setProperty("fontFamily",QString("VictorMono Nerd Font Mono")));
        QVERIFY(desktop->setProperty("textScale",1.0));
        window->resize(1267,694);
        QSignalSpy ready(bible.get(),&BibleBackend::readyChanged);
        QSignalSpy frames(window,&QQuickWindow::frameSwapped);
        bible->importEpub(QUrl::fromLocalFile(epub));
        QVERIFY(bible->busy());
        QVERIFY(prop("busyPopup","visible").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(),120000);
        QVERIFY2(bible->ready(),qPrintable(bible->error()));
        QCOMPARE(ready.count(),1);
        QTRY_VERIFY(!prop("busyPopup","visible").toBool());
        QTRY_VERIFY(item("readerSurface")->isVisible());
        QVERIFY(!item("setupScroll")->isVisible());
        reference("John 1");
        for (const QSize size : {QSize(1267,694),QSize(640,480),QSize(1120,820)}) {
            window->resize(size);
            const auto previousFrames = frames.count();
            window->update();
            QTRY_VERIFY(frames.count() > previousFrames);
            QTest::qWait(100);
            QVERIFY(item("verseNumber1"));
            QVERIFY(item("verseNumber2"));
            QVERIFY(item("verseHeading1"));
            QVERIFY(item("verseHeading1")->height() > 36);
            QCOMPARE(item("verseHeading2")->height(),0.0);
            const auto headingHeight = item("verseHeading1")->height();
            // Hiding an ancestor must not collapse the measured heading and
            // re-enter its implicit-size binding when it becomes visible again.
            item("readerSurface")->setVisible(false);
            QCoreApplication::processEvents();
            QCOMPARE(item("verseHeading1")->height(),headingHeight);
            item("readerSurface")->setVisible(true);
            QCoreApplication::processEvents();
            QCOMPARE(item("verseHeading1")->height(),headingHeight);
            QVERIFY(item("verseNumber1")->mapToItem(item("reading"),QPointF()).y() > 0);
            QVERIFY(item("verseNumber2")->mapToItem(item("reading"),QPointF()).y()
                > item("verseNumber1")->mapToItem(item("reading"),QPointF()).y());
        }
        QVERIFY2(qmlWarnings.isEmpty(),qPrintable(qmlWarnings.join('\n')));
    }
    void referenceAliasesAndBrowsing() {
        reference("John3:16");
        QCOMPARE(bible->book(),43); QCOMPARE(bible->chapter(),3); QCOMPARE(bible->selectedStart(),16);
        QVERIFY(prop("reading", "contentY").toDouble() > 0);
        reference("1 Cor 13:4-7");
        QCOMPARE(bible->book(),46); QCOMPARE(bible->chapter(),13);
        QCOMPARE(bible->selectedStart(),4); QCOMPARE(bible->selectedEnd(),7);
        reference("Ps 23");
        QCOMPARE(bible->book(),19); QCOMPARE(bible->chapter(),23);
        key(Qt::Key_K, Qt::ControlModifier);
        QTRY_VERIFY(prop("passagePalette", "opened").toBool());
        type("John"); key(Qt::Key_Return);
        QTRY_COMPARE(window->property("paletteBook").toInt(),43);
        type("3"); key(Qt::Key_Return);
        QTRY_VERIFY(!prop("passagePalette", "visible").toBool());
        QCOMPARE(bible->book(),43); QCOMPARE(bible->chapter(),3);
        QCOMPARE(bible->selectedStart(),0);
    }
    void invalidReferenceKeepsPassage() {
        reference("John 3:16");
        key(Qt::Key_K, Qt::ControlModifier);
        QTRY_VERIFY(prop("passagePalette", "opened").toBool());
        type("John 300:999"); key(Qt::Key_Return);
        QVERIFY(bible->error().isEmpty());
        QVERIFY(prop("referencePreview", "text").toString().startsWith("Invalid or incomplete reference"));
        QVERIFY(prop("passagePalette", "visible").toBool());
        QCOMPARE(bible->chapter(),3); QCOMPARE(bible->selectedStart(),16);
        key(Qt::Key_A, Qt::ControlModifier); type("Jn 4:2"); key(Qt::Key_Return);
        QTRY_VERIFY(!prop("passagePalette", "visible").toBool());
        QVERIFY(bible->error().isEmpty()); QCOMPARE(bible->chapter(),4);
    }
    void referencePreviewIsPure() {
        reference("John 3:16");
        bible->setNoteText("Keep this exact note");
        bible->toggleBookmark();
        QTest::qWait(450);
        const auto position = bible->readingPosition();
        const auto collection = bible->collection();
        QSignalSpy navigation(bible.get(),&BibleBackend::navigationChanged);
        QSignalSpy selection(bible.get(),&BibleBackend::selectionChanged);
        QSignalSpy errors(bible.get(),&BibleBackend::errorChanged);
        key(Qt::Key_K,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        for (const auto &query : {QString("rev4"),QString("John 3:"),QString("John 300:999"),QString("4:2")}) {
            key(Qt::Key_A,Qt::ControlModifier); type(query);
            QVERIFY(item("referencePreview")->isVisible());
            const auto preview = prop("referencePreview","text").toString();
            if (query == "rev4") QCOMPARE(preview,QString("Revelation 4"));
            else if (query == "4:2") QCOMPARE(preview,QString("John 4:2"));
            else {
                QVERIFY(preview.startsWith("Invalid or incomplete reference"));
                key(Qt::Key_Return);
                QVERIFY(prop("passagePalette","visible").toBool());
            }
            QCOMPARE(bible->selectionReference(),QString("John 3:16"));
            QCOMPARE(bible->noteText(),QString("Keep this exact note"));
            QCOMPARE(bible->readingPosition(),position);
            QCOMPARE(bible->collection(),collection);
            QVERIFY(bible->error().isEmpty());
        }
        QCOMPARE(navigation.count(),0); QCOMPARE(selection.count(),0); QCOMPARE(errors.count(),0);
        key(Qt::Key_A,Qt::ControlModifier); type("rev4"); key(Qt::Key_Return);
        QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        QCOMPARE(bible->passage(),QString("Revelation 4"));
    }
    void rightArrowBrowsingAndEditing() {
        reference("John 3:16");
        key(Qt::Key_K,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        key(Qt::Key_Down); key(Qt::Key_Right);
        QCOMPARE(window->property("paletteBook").toInt(),2);
        QCOMPARE(window->property("paletteBookName").toString(),QString("Exodus"));
        QCOMPARE(bible->selectionReference(),QString("John 3:16"));
        type("3");
        QCOMPARE(prop("referenceInput","text").toString(),QString("3"));
        QCOMPARE(prop("referencePreview","text").toString(),QString("Exodus 3"));
        key(Qt::Key_Return);
        QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        QCOMPARE(bible->passage(),QString("Exodus 3"));
        key(Qt::Key_K,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("John");
        key(Qt::Key_Home); key(Qt::Key_Right);
        QCOMPARE(prop("referenceInput","cursorPosition").toInt(),1);
        QCOMPARE(window->property("paletteBook").toInt(),0);
        key(Qt::Key_End); key(Qt::Key_Right);
        QCOMPARE(window->property("paletteBook").toInt(),43);
        key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        key(Qt::Key_K,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("1 John 3:16");
        key(Qt::Key_Home); key(Qt::Key_Right);
        QCOMPARE(prop("referenceInput","cursorPosition").toInt(),1);
        key(Qt::Key_End); key(Qt::Key_Right);
        QCOMPARE(window->property("paletteBook").toInt(),0);
        QCOMPARE(prop("referenceInput","text").toString(),QString("1 John 3:16"));
        QCOMPARE(bible->passage(),QString("Exodus 3"));
        key(Qt::Key_A,Qt::ControlModifier); type("1 John");
        key(Qt::Key_Right);
        QCOMPARE(window->property("paletteBook").toInt(),62);
    }
    void normalVisualAndShiftedSelection() {
        reference("John 3:16");
        for (auto down : {Qt::Key_J,Qt::Key_Down}) {
            key(down);
            QCOMPARE(bible->selectedStart(),17); QCOMPARE(bible->selectedEnd(),17);
            QVERIFY(!window->property("verseMode").toBool());
            key(down == Qt::Key_J ? Qt::Key_K : Qt::Key_Up);
            QCOMPARE(bible->cursorVerse(),16);
        }
        key(Qt::Key_V);
        QVERIFY(window->property("verseMode").toBool());
        key(Qt::Key_J); key(Qt::Key_Down);
        QCOMPARE(bible->selectedStart(),16); QCOMPARE(bible->selectedEnd(),18);
        key(Qt::Key_K);
        QCOMPARE(bible->selectedStart(),16); QCOMPARE(bible->selectedEnd(),17);
        key(Qt::Key_Up); key(Qt::Key_Up);
        QCOMPARE(bible->selectedStart(),15); QCOMPARE(bible->selectedEnd(),16);
        QCOMPARE(bible->cursorVerse(),15);
        key(Qt::Key_V);
        QVERIFY(!window->property("verseMode").toBool());
        QCOMPARE(bible->selectedStart(),15); QCOMPARE(bible->selectedEnd(),15);
        for (auto shifted : {Qt::Key_J,Qt::Key_K,Qt::Key_Down,Qt::Key_Up}) {
            reference("John 3:16");
            key(shifted,Qt::ShiftModifier);
            QVERIFY(window->property("verseMode").toBool());
            const bool down = shifted == Qt::Key_J || shifted == Qt::Key_Down;
            QCOMPARE(bible->cursorVerse(),down ? 17 : 15);
            QCOMPARE(bible->selectedStart(),down ? 16 : 15);
            QCOMPARE(bible->selectedEnd(),down ? 17 : 16);
            key(Qt::Key_Escape);
            QVERIFY(!window->property("verseMode").toBool());
            QCOMPARE(bible->selectedStart(),bible->cursorVerse());
            QCOMPARE(bible->selectedEnd(),bible->cursorVerse());
        }
        reference("John 3:16-18");
        key(Qt::Key_V); key(Qt::Key_K);
        QCOMPARE(bible->selectedStart(),17); QCOMPARE(bible->selectedEnd(),18);
        reference("John 3");
        key(Qt::Key_V); key(Qt::Key_J);
        QCOMPARE(bible->selectedStart(),1); QCOMPARE(bible->selectedEnd(),2);
    }
    void palettePreviewMatchesAcceptance() {
        reference("Genesis 1:1");
        QSignalSpy navigation(bible.get(),&BibleBackend::navigationChanged);
        key(Qt::Key_K,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("John"); key(Qt::Key_Return);
        QCOMPARE(window->property("paletteBook").toInt(),43);
        type("3:16");
        QCOMPARE(prop("referencePreview","text").toString(),QString("John 3:16"));
        QCOMPARE(navigation.count(),0);
        QCOMPARE(bible->selectionReference(),QString("Genesis 1:1"));
        key(Qt::Key_Return);
        QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        QCOMPARE(bible->selectionReference(),QString("John 3:16"));
        navigation.clear();
        key(Qt::Key_K,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("Psalms"); key(Qt::Key_Right);
        QCOMPARE(window->property("paletteBook").toInt(),19);
        type("1");
        QCOMPARE(prop("referencePreview","text").toString(),QString("Psalms 1"));
        key(Qt::Key_Down);
        QCOMPARE(prop("referencePreview","text").toString(),QString("Psalms 10"));
        QCOMPARE(navigation.count(),0);
        QCOMPARE(bible->selectionReference(),QString("John 3:16"));
        key(Qt::Key_Return);
        QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        QCOMPARE(bible->book(),19); QCOMPARE(bible->chapter(),10);
        key(Qt::Key_K,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("Psalms"); key(Qt::Key_Return); type("1"); key(Qt::Key_Down);
        QCOMPARE(prop("referencePreview","text").toString(),QString("Psalms 10"));
        // Editing resets the list override and restores the typed destination.
        type(":2");
        QCOMPARE(prop("referencePreview","text").toString(),QString("Psalms 1:2"));
        // Arrows with no matching list entry must not discard a valid typed reference.
        key(Qt::Key_Down);
        QCOMPARE(prop("referencePreview","text").toString(),QString("Psalms 1:2"));
        key(Qt::Key_Return);
        QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        QCOMPARE(bible->chapter(),1); QCOMPARE(bible->cursorVerse(),2);
    }
    void failedCopyDoesNotNotifySuccess() {
        reference("John 3:36-4:1");
        const auto verses = bible->verses();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","ui-copy-failure");
            db.setDatabaseName(bible->dataDirectory() + "/bible.sqlite");
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec("ALTER TABLE verses RENAME TO saved_verses"));
            QVERIFY(query.exec("CREATE VIEW verses AS SELECT book,chapter,number,CASE WHEN book=43 AND chapter=4 THEN abs(-9223372036854775808) ELSE text END AS text,html,heading,poetry FROM saved_verses"));
            QApplication::clipboard()->setText("Preserve clipboard on partial read failure");
            key(Qt::Key_Y);
            QVERIFY(!bible->error().isEmpty());
            QVERIFY(item("errorMessage")->isVisible());
            QCOMPARE(QApplication::clipboard()->text(),QString("Preserve clipboard on partial read failure"));
            QVERIFY(!item("toast")->isVisible());
            QVERIFY(!prop("toast","text").toString().contains("copied"));
            QCOMPARE(bible->verses(),verses);
            QVERIFY(query.exec("DROP VIEW verses"));
            QVERIFY(query.exec("ALTER TABLE saved_verses RENAME TO verses"));
        }
        QSqlDatabase::removeDatabase("ui-copy-failure");
        key(Qt::Key_Y);
        QVERIFY(bible->error().isEmpty());
        QVERIFY(item("toast")->isVisible());
        QCOMPARE(prop("toast","text").toString(),bible->selectionReference() + " copied");
        QVERIFY(QApplication::clipboard()->text().contains(bible->selectionReference()));
    }
    void chapterBoundarySelection_data() {
        QTest::addColumn<bool>("forward");
        QTest::newRow("next-first") << true;
        QTest::newRow("previous-last") << false;
    }
    void chapterBoundarySelection() {
        QFETCH(bool,forward);
        const QString origin = forward ? "John 3:36" : "John 4:1";
        const auto direction = forward ? Qt::Key_J : Qt::Key_K;
        const auto reverse = forward ? Qt::Key_K : Qt::Key_J;
        reference(origin);
        key(direction);
        QCOMPARE(bible->chapter(),forward ? 4 : 3);
        QCOMPARE(bible->cursorVerse(),forward ? 1 : 36);
        QCOMPARE(bible->selectedStart(),bible->selectedEnd());
        QVERIFY(!bible->crossChapterSelection());
        const auto cursorVisible = [&] {
            auto verse = item(qPrintable("verseNumber" + QString::number(bible->cursorVerse())));
            if (!verse) return false;
            const double y = verse->mapToItem(item("reading"),QPointF()).y();
            return y >= 0 && y < item("reading")->height();
        };
        QTRY_VERIFY(cursorVisible());
        reference(origin);
        key(direction,Qt::ShiftModifier);
        QVERIFY(bible->crossChapterSelection());
        const QString span = QString::fromUtf8("John 3:36–4:1");
        QCOMPARE(bible->selectionReference(),span);
        QTRY_VERIFY(cursorVisible());
        // Reverse movement contracts toward the canonical anchor, not the
        // visible chapter intersection, and keeps the active endpoint in view.
        key(reverse);
        QCOMPARE(bible->selectionReference(),origin);
        key(direction); key(direction);
        QCOMPARE(bible->cursorVerse(),forward ? 2 : 35);
        QTRY_VERIFY(cursorVisible());
        key(reverse);
        QCOMPARE(bible->selectionReference(),span);
        QTRY_VERIFY(cursorVisible());
        key(Qt::Key_X); key(Qt::Key_M);
        QVERIFY(bible->selectionHighlighted()); QVERIFY(bible->selectionBookmarked());
        key(Qt::Key_Y);
        const QString copied = QApplication::clipboard()->text();
        QVERIFY(copied.contains(span));
        const auto lines = copied.split('\n');
        QCOMPARE(lines.size(),3);
        QVERIFY(lines[1].startsWith("John 3:36 ")); QVERIFY(lines[1].size() > 20);
        QVERIFY(lines[2].startsWith("John 4:1 ")); QVERIFY(lines[2].size() > 20);
        key(Qt::Key_N);
        QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        const QString note = "An exact cross-chapter note j k h l x v";
        type(note);
        QCOMPARE(bible->noteText(),note);
        key(Qt::Key_Escape);
        QVERIFY(window->property("verseMode").toBool());
        key(Qt::Key_Escape);
        QVERIFY(!window->property("verseMode").toBool());
        QVERIFY(window->property("notesOpen").toBool());
        QVERIFY(!bible->crossChapterSelection());
        QCOMPARE(bible->selectedStart(),bible->cursorVerse());
        QCOMPARE(bible->selectedEnd(),bible->cursorVerse());
        QCOMPARE(prop("noteEditor","text").toString(),QString());
        // Reopening from storage and then the drawer must recover the entire
        // range, its exact note, and flags on both sides of the boundary.
        unloadUi(); loadUi();
        QCOMPARE(bible->collection().size(),1);
        QCOMPARE(bible->collection().first().toMap().value("reference").toString(),span);
        key(Qt::Key_L,Qt::ControlModifier);
        QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        const QByteArray name = ("savedEntry" + span).toUtf8();
        QTRY_VERIFY(item(name.constData()));
        click(item(name.constData()));
        QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
        QCOMPARE(bible->selectionReference(),span);
        QVERIFY(bible->crossChapterSelection());
        QVERIFY(bible->selectionHighlighted()); QVERIFY(bible->selectionBookmarked());
        QCOMPARE(prop("noteEditor","text").toString(),note);
        key(Qt::Key_Y); QCOMPARE(QApplication::clipboard()->text(),copied);
        reference("John 3:36"); QVERIFY(bible->selectionHighlighted()); QVERIFY(bible->noteText().isEmpty());
        reference("John 4:1"); QVERIFY(bible->selectionHighlighted()); QVERIFY(bible->noteText().isEmpty());
    }
    void mouseAnchorAfterCrossChapterMovement() {
        reference("John 4:1");
        key(Qt::Key_K,Qt::ShiftModifier);
        QVERIFY(bible->crossChapterSelection());
        QTRY_VERIFY(item("verseNumber35"));
        click(item("verseNumber35"),Qt::ShiftModifier);
        QCOMPARE(bible->selectionReference(),QString::fromUtf8("John 3:35–36"));
        QCOMPARE(bible->cursorVerse(),35);
        QVERIFY(!bible->crossChapterSelection());
        key(Qt::Key_J);
        QCOMPARE(bible->selectionReference(),QString("John 3:36"));
    }
    void annotationKeysAndNoteIsolation() {
        reference("John 3:16");
        key(Qt::Key_J); QCOMPARE(bible->selectedStart(),17);
        key(Qt::Key_K); QCOMPARE(bible->selectedStart(),16);
        key(Qt::Key_X); QVERIFY(bible->selectionHighlighted());
        key(Qt::Key_M); QVERIFY(bible->selectionBookmarked());
        key(Qt::Key_N, Qt::ControlModifier);
        QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        const QString first = "# John 3\nj k h l x m v y g are note text, not reader commands.";
        type(first);
        QCOMPARE(prop("noteEditor", "text").toString(),first);
        QCOMPARE(bible->noteText(),first);
        QCOMPARE(bible->selectedStart(),16);
        QVERIFY(bible->selectionHighlighted()); QVERIFY(bible->selectionBookmarked());
        QVERIFY(!window->property("verseMode").toBool());
        // Standard undo must stay inside the editor too.
        type("!"); key(Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(!bible->noteText().endsWith('!'));
        key(Qt::Key_A, Qt::ControlModifier); type(first);
        key(Qt::Key_Escape);
        QTRY_VERIFY(!item("noteEditor")->hasActiveFocus());
        key(Qt::Key_J); QCOMPARE(bible->selectedStart(),17);
        QCOMPARE(prop("noteEditor", "text").toString(),QString());
        key(Qt::Key_N, Qt::ControlModifier);
        QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        const QString second = "A different note for the next verse.";
        type(second);
        // Navigation while editing must synchronously finish the correct note.
        key(Qt::Key_K, Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("John3:16"); key(Qt::Key_Return);
        QTRY_VERIFY(!prop("passagePalette", "visible").toBool());
        QCOMPARE(bible->noteText(),first);
        QCOMPARE(prop("noteEditor", "text").toString(),first);
        bible->refreshCollection();
        QCOMPARE(saved(16).value("note").toString(),first);
        QCOMPARE(saved(17).value("note").toString(),second);
        click(item("previewButton"));
        QVERIFY(window->property("notePreview").toBool());
        QVERIFY(item("notePreview")->isVisible());
        QVERIFY(prop("notePreview","readOnly").toBool());
        // TextEdit serializes parsed Markdown, so compare rendered plain text,
        // not the normalized Markdown returned by its text property.
        QString preview;
        QVERIFY(QMetaObject::invokeMethod(object("notePreview"),"getText",Q_RETURN_ARG(QString,preview),
            Q_ARG(int,0),Q_ARG(int,prop("notePreview","length").toInt())));
        QVERIFY(preview.contains("John 3"));
        QVERIFY(preview.contains("j k h l x m v y g are note text"));
        QCOMPARE(bible->noteText(),first);
        // Recreate both C++ and QML objects to prove the notes reached storage.
        unloadUi(); loadUi();
        QCOMPARE(bible->noteText(),first);
        QCOMPARE(saved(17).value("note").toString(),second);
    }
    void clickRangeAndCollections() {
        reference("John 3:16");
        click(item("verseNumber16"));
        QCOMPARE(bible->selectedStart(),16);
        click(item("verseNumber17"),Qt::ShiftModifier);
        QCOMPARE(bible->selectedStart(),16); QCOMPARE(bible->selectedEnd(),17);
        key(Qt::Key_H,Qt::ControlModifier); QVERIFY(bible->selectionHighlighted());
        key(Qt::Key_D,Qt::ControlModifier); QVERIFY(bible->selectionBookmarked());
        key(Qt::Key_L,Qt::ControlModifier);
        QTRY_VERIFY(prop("savedDrawer", "opened").toBool());
        QTRY_COMPARE(prop("collectionList", "count").toInt(),1);
        const QByteArray entryName = ("savedEntry" + bible->selectionReference()).toUtf8();
        QTRY_VERIFY(item(entryName.constData()));
        click(item(entryName.constData()));
        QTRY_VERIFY(!prop("savedDrawer", "visible").toBool());
        QCOMPARE(bible->selectedStart(),16); QCOMPARE(bible->selectedEnd(),17);
    }
    void normalScrollingAndChapterNavigation() {
        reference("John 3");
        const double before = prop("reading","contentY").toDouble();
        key(Qt::Key_PageDown);
        QVERIFY(prop("reading","contentY").toDouble() > before);
        QCOMPARE(bible->selectedStart(),0);
        key(Qt::Key_F,Qt::ControlModifier);
        const double paged = prop("reading","contentY").toDouble();
        key(Qt::Key_B,Qt::ControlModifier);
        QVERIFY(prop("reading","contentY").toDouble() < paged);
        key(Qt::Key_Right,Qt::AltModifier);
        QTRY_COMPARE(bible->chapter(),4);
        key(Qt::Key_Left,Qt::AltModifier);
        QTRY_COMPARE(bible->chapter(),3);
        key(Qt::Key_BracketRight); QCOMPARE(bible->chapter(),4);
        key(Qt::Key_BracketLeft); QCOMPARE(bible->chapter(),3);
        key(Qt::Key_L); QCOMPARE(bible->chapter(),4);
        key(Qt::Key_H); QCOMPARE(bible->chapter(),3);
        key(Qt::Key_G); QCOMPARE(bible->cursorVerse(),1);
        key(Qt::Key_G,Qt::ShiftModifier); QCOMPARE(bible->cursorVerse(),36);
        QCOMPARE(bible->selectedStart(),36); QCOMPARE(bible->selectedEnd(),36);
    }
    void positionSurvivesRestart() {
        reference("Psalm 119");
        key(Qt::Key_PageDown); key(Qt::Key_PageDown);
        QTRY_VERIFY(bible->readingPosition() > 0);
        const double position = bible->readingPosition();
        unloadUi(); loadUi();
        QCOMPARE(bible->book(),19); QCOMPARE(bible->chapter(),119);
        const double travel = prop("reading","contentHeight").toDouble() - item("reading")->height();
        QVERIFY(travel > 0);
        QVERIFY(qAbs(prop("reading","contentY").toDouble() / travel - position) < 0.01);
    }
    void positionWithSelectionSurvivesRestart() {
        reference("Psalm 119:16");
        key(Qt::Key_PageDown); key(Qt::Key_PageDown);
        QTest::qWait(450);
        const double position = bible->readingPosition();
        QVERIFY(position > 0);
        unloadUi(); loadUi();
        QCOMPARE(bible->selectedStart(),16);
        const double travel = prop("reading","contentHeight").toDouble() - item("reading")->height();
        QVERIFY(travel > 0);
        QVERIFY(qAbs(prop("reading","contentY").toDouble() / travel - position) < 0.01);
    }
    void commandPaletteAndHelp() {
        key(Qt::Key_P,Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        QCOMPARE(prop("referenceInput","text").toString(),QString("> "));
        type("keybindings"); key(Qt::Key_Return);
        QTRY_VERIFY(prop("helpDialog","opened").toBool());
        key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("helpDialog","visible").toBool());
        key(Qt::Key_F1); QTRY_VERIFY(prop("helpDialog","opened").toBool());
        key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("helpDialog","visible").toBool());
        key(Qt::Key_P,Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("about"); key(Qt::Key_Return);
        QTRY_VERIFY(prop("aboutDialog","opened").toBool());
    }
    void searchableKeybindings() {
        reference("John 3:16");
        key(Qt::Key_F1);
        QTRY_VERIFY(prop("helpDialog","opened").toBool());
        QTRY_VERIFY(item("keybindingSearch")->hasActiveFocus());
        QCOMPARE(prop("keybindingSearch","placeholderText").toString(),QString("Keybindings..."));
        const int total = prop("keybindingResults","count").toInt();
        QVERIFY(total > 10);
        capture("help");
        for (const auto &query : {QString("m"),QString("x"),QString("bookmark"),QString("Ctrl+H"),QString("Shift+Down")}) {
            key(Qt::Key_A,Qt::ControlModifier); type(query);
            QTRY_COMPARE(prop("keybindingResults","count").toInt(),1);
            const QString action = query == "m" || query == "bookmark" ? "Bookmark selection"
                : query == "Shift+Down" ? "Enter visual mode and extend" : "Highlight selection";
            QTRY_VERIFY(item(qPrintable("keybinding" + action)));
            QCOMPARE(bible->selectionReference(),QString("John 3:16"));
            QVERIFY(!bible->selectionBookmarked()); QVERIFY(!bible->selectionHighlighted());
            QVERIFY(!window->property("verseMode").toBool());
            QVERIFY(item("keybindingSearch")->hasActiveFocus());
        }
        key(Qt::Key_A,Qt::ControlModifier); type("no such keybinding jkhlnv");
        QTRY_COMPARE(prop("keybindingResults","count").toInt(),0);
        QVERIFY(item("keybindingEmpty")->isVisible());
        QCOMPARE(bible->selectionReference(),QString("John 3:16"));
        QVERIFY(!window->property("notesOpen").toBool());
        key(Qt::Key_A,Qt::ControlModifier); key(Qt::Key_Backspace);
        QTRY_COMPARE(prop("keybindingResults","count").toInt(),total);
        QVERIFY(!item("keybindingEmpty")->isVisible());
        key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("helpDialog","visible").toBool());
        QTRY_VERIFY(item("readerSurface")->hasActiveFocus());
        key(Qt::Key_M); QVERIFY(bible->selectionBookmarked());
        key(Qt::Key_F1);
        QTRY_VERIFY(item("keybindingSearch")->hasActiveFocus());
        QCOMPARE(prop("keybindingSearch","text").toString(),QString());
        window->resize(640,480);
        QVERIFY(desktop->setProperty("textScale",1.8));
        QTest::qWait(150);
        QVERIFY(item("keybindingResults")->height() > 50);
        const auto searchBounds = item("keybindingSearch")->mapRectToScene(item("keybindingSearch")->boundingRect());
        QVERIFY(QRectF(0,0,640,480).contains(searchBounds));
        type("x"); QTRY_COMPARE(prop("keybindingResults","count").toInt(),1);
        capture("help-scaled");
    }
    void bookmarkRangeMarkers() {
        reference("John 3:16-18");
        key(Qt::Key_M);
        reference("John 3:16");
        int glyphs = 0;
        for (int verse = 1; verse <= 36; ++verse) {
            auto glyph = item(qPrintable("bookmarkGlyph" + QString::number(verse)));
            QVERIFY(glyph);
            if (glyph->isVisible()) ++glyphs;
        }
        QCOMPARE(glyphs,1);
        QVERIFY(item("bookmarkGlyph16")->isVisible());
        QVERIFY(!item("bookmarkGlyph17")->isVisible());
        QVERIFY(prop("bookmarkMarker16","starts").toBool());
        QVERIFY(prop("bookmarkMarker18","ends").toBool());
        for (int verse : {16,17}) {
            auto line = item(qPrintable("bookmarkLine" + QString::number(verse)));
            auto next = item(qPrintable("bookmarkLine" + QString::number(verse + 1)));
            const double bottom = line->mapToScene(QPointF(0,line->height())).y();
            QVERIFY(qAbs(bottom - next->mapToScene(QPointF()).y()) < 0.1);
        }
        capture("bookmark");
        click(item("bookmarkMarker16"));
        QCOMPARE(bible->selectionReference(),QString::fromUtf8("John 3:16–18"));
        // An overlapping range gets its own logical start, while shared rows
        // expose both references and a stable click target.
        reference("John 3:17-20"); key(Qt::Key_M);
        reference("John 3:17");
        QVERIFY(item("bookmarkGlyph17")->isVisible());
        const QString hint = prop("bookmarkMarker17","hint").toString();
        QVERIFY(hint.contains(QString::fromUtf8("John 3:16–18")));
        QVERIFY(hint.contains(QString::fromUtf8("John 3:17–20")));
        QCOMPARE(prop("bookmarkMarker17","openReference").toString(),QString::fromUtf8("John 3:17–20"));
        click(item("bookmarkMarker17"));
        QCOMPARE(bible->selectionReference(),QString::fromUtf8("John 3:17–20"));
        reference("John 3:36-4:2"); key(Qt::Key_M);
        reference("John 3:36");
        QVERIFY(item("bookmarkGlyph36")->isVisible());
        QVERIFY(prop("bookmarkMarker36","continuesAfter").toBool());
        reference("John 4:1");
        QVERIFY(item("bookmarkMarker1")->isVisible());
        QVERIFY(prop("bookmarkMarker1","chapterContinuation").toBool());
        QVERIFY(!item("bookmarkGlyph1")->isVisible());
        QCOMPARE(item("bookmarkLine1")->y(),0.0);
        QVERIFY(prop("bookmarkMarker2","ends").toBool());
        QVERIFY(!item("bookmarkMarker3")->isVisible());
        capture("bookmark-continuation");
        click(item("bookmarkMarker1"));
        QCOMPARE(bible->selectionReference(),QString::fromUtf8("John 3:36–4:2"));
    }
    void savedPanelGeometry_data() {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<double>("scale");
        QTest::newRow("wide") << QSize(1120,820) << 1.0;
        QTest::newRow("narrow") << QSize(640,480) << 1.0;
        QTest::newRow("scaled") << QSize(640,640) << 1.8;
        QTest::newRow("scaled-short") << QSize(640,480) << 1.8;
    }
    void savedPanelGeometry() {
        QFETCH(QSize,size);
        QFETCH(double,scale);
        for (int verse = 1; verse <= 12; ++verse) {
            QVERIFY(bible->navigate(QString("John 3:%1").arg(verse)));
            bible->toggleBookmark();
            bible->setNoteText("A saved thought with enough text to wrap comfortably within the entry padding.");
        }
        window->resize(size);
        QVERIFY(desktop->setProperty("textScale",scale));
        key(Qt::Key_L,Qt::ControlModifier);
        QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        QTest::qWait(150);
        auto content = item("savedContent");
        QVERIFY(content);
        QCOMPARE(prop("savedDrawer","padding").toDouble(),24 * scale);
        const auto bounds = content->mapRectToScene(content->boundingRect());
        QCOMPARE(bounds.left(),24 * scale);
        QVERIFY(qAbs(bounds.top() - 24 * scale) < 0.1);
        QCOMPARE(prop("savedDrawer","width").toDouble() - bounds.right(),24 * scale);
        QVERIFY(qAbs(size.height() - bounds.bottom() - 24 * scale) < 0.1);
        auto tabs = item("savedTabs");
        for (int index = 0; index < 4; ++index) {
            auto tab = item(qPrintable("savedTab" + QString::number(index)));
            QVERIFY(tab);
            const auto tabBounds = tab->mapRectToItem(tabs,tab->boundingRect());
            QVERIFY2(tabs->boundingRect().adjusted(-0.5,-0.5,0.5,0.5).contains(tabBounds),
                qPrintable(QString("Tab %1: %2,%3 %4x%5 in %6x%7").arg(index).arg(tabBounds.x()).arg(tabBounds.y())
                    .arg(tabBounds.width()).arg(tabBounds.height()).arg(tabs->width()).arg(tabs->height())));
            const auto label = qvariant_cast<QQuickItem *>(tab->property("contentItem"));
            QVERIFY(label);
            QVERIFY(label->width() >= label->implicitWidth());
        }
        auto list = item("collectionList");
        QVERIFY(list->clip());
        QVERIFY(list->height() > 80);
        auto entry = item("savedEntryJohn 3:1");
        QTRY_VERIFY(entry);
        QVERIFY(entry->clip());
        auto entryContent = item("savedEntryContentJohn 3:1");
        QVERIFY(entryContent);
        const auto inner = entryContent->mapRectToItem(entry,entryContent->boundingRect());
        QVERIFY(qAbs(inner.left() - 12 * scale) < 0.1);
        QVERIFY(qAbs(inner.top() - 12 * scale) < 0.1);
        QVERIFY(qAbs(entry->width() - inner.right() - 12 * scale) < 0.1);
        QVERIFY(qAbs(entry->height() - inner.bottom() - 12 * scale) < 0.1);
        auto bar = item("savedScrollbar");
        QVERIFY(bar);
        QVERIFY(entry->width() + 8 * scale <= bar->mapToItem(list,QPointF()).x() + 0.1);
        capture("saved-" + QString::fromLatin1(QTest::currentDataTag()));
        click(item("savedTab2"));
        QTRY_COMPARE(prop("collectionList","count").toInt(),0);
        click(item("savedTab1"));
        QTRY_COMPARE(prop("collectionList","count").toInt(),12);
        // Scrolling must keep the delegate within the clipped viewport and
        // preserve an unobscured scrollbar gutter.
        QVERIFY(QMetaObject::invokeMethod(list,"positionViewAtEnd"));
        QTRY_VERIFY(prop("collectionList","contentY").toDouble() > 0);
    }
    void restoreConfirmationCancelAndAccept() {
        reference("John 3:16");
        key(Qt::Key_X); key(Qt::Key_M);
        key(Qt::Key_N);
        QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        type("Original backup note n j k h m");
        QCOMPARE(bible->noteText(),QString("Original backup note n j k h m"));
        QCOMPARE(bible->selectedStart(),16);
        const auto backup = QUrl::fromLocalFile(session->filePath("restore-test.json"));
        QVERIFY(bible->exportBackup(backup));
        key(Qt::Key_A,Qt::ControlModifier); type("Current note to preserve on cancel");
        reference("John 4:2");
        const auto current = bible->collection();
        const auto request = [&] {
            return QMetaObject::invokeMethod(window,"requestRestore",Q_ARG(QVariant,QVariant::fromValue(backup)));
        };
        QVERIFY(request());
        QTRY_VERIFY(prop("restoreConfirmation","opened").toBool());
        QTRY_VERIFY(item("restoreCancel")->hasActiveFocus());
        QCOMPARE(prop("restoreBackupPath","text").toString(),backup.toString());
        QVERIFY(window->property("modalOpen").toBool());
        key(Qt::Key_N); key(Qt::Key_H); key(Qt::Key_J);
        key(Qt::Key_Right,Qt::AltModifier);
        key(Qt::Key_K,Qt::ControlModifier);
        QVERIFY(!prop("passagePalette","visible").toBool());
        QCOMPARE(bible->chapter(),4); QCOMPARE(bible->selectedStart(),2);
        QCOMPARE(bible->collection(),current);
        // Return on the initially focused Cancel must not restore.
        key(Qt::Key_Return);
        QTRY_VERIFY(!prop("restoreConfirmation","visible").toBool());
        QCOMPARE(bible->collection(),current); QCOMPARE(bible->chapter(),4);
        QVERIFY(request());
        QTRY_VERIFY(prop("restoreConfirmation","opened").toBool());
        key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("restoreConfirmation","visible").toBool());
        QCOMPARE(bible->collection(),current);
        QVERIFY(request());
        QTRY_VERIFY(prop("restoreConfirmation","opened").toBool());
        key(Qt::Key_Tab);
        QTRY_VERIFY(item("restoreConfirm")->hasActiveFocus());
        key(Qt::Key_Return);
        QTRY_VERIFY(!prop("restoreConfirmation","visible").toBool());
        QCOMPARE(bible->chapter(),3); QCOMPARE(bible->selectedStart(),16);
        QCOMPARE(bible->noteText(),QString("Original backup note n j k h m"));
        QVERIFY(bible->selectionHighlighted()); QVERIFY(bible->selectionBookmarked());
        QCOMPARE(saved(16).value("note").toString(),bible->noteText());
    }
    void narrowLayoutAndSystemScale() {
        reference("John 3:16");
        window->resize(640,480);
        key(Qt::Key_N,Qt::ControlModifier);
        QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        QTRY_VERIFY(item("verseNumber16")->mapToItem(item("reading"),QPointF()).y() >= 0);
        QTRY_VERIFY(item("verseNumber16")->mapToItem(item("reading"),QPointF()).y() < 80);
        QVERIFY(!window->property("wideLayout").toBool());
        auto panel = item("notesPanel");
        QVERIFY(panel->isVisible());
        const QRectF panelBounds(panel->mapToScene(QPointF()),QSizeF(panel->width(),panel->height()));
        QVERIFY(QRectF(0,0,640,480).contains(panelBounds));
        QVERIFY(item("noteEditor")->width() > 100);
        type("Narrow window note jkhm");
        QCOMPARE(bible->noteText(),QString("Narrow window note jkhm"));
        key(Qt::Key_Escape); key(Qt::Key_Escape);
        QTRY_VERIFY(!panel->isVisible());
        // Desktop's public property uses the same NOTIFY route as a live theme.
        QVERIFY(desktop->setProperty("textScale",1.8));
        QTRY_COMPARE(window->property("scale").toDouble(),1.8);
        QCOMPARE(window->property("font").value<QFont>().family(),desktop->property("fontFamily").toString());
        reference("Jn 4:1");
        QCOMPARE(bible->chapter(),4);
        key(Qt::Key_N,Qt::ControlModifier);
        QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        QVERIFY(item("noteEditor")->height() >= 100);
        QVERIFY(prop("notesScroll","contentHeight").toDouble() > item("notesScroll")->height());
        // Focusing the editor scrolls the outer panel into view at large scale.
        QTRY_VERIFY(item("noteEditor")->mapToScene(QPointF()).y() < window->height() - 100);
        type("Large-scale editing remains usable.");
        QCOMPARE(bible->noteText(),QString("Large-scale editing remains usable."));
        window->update(); QTest::qWait(250);
        QVERIFY(window->grabWindow().save(fixtures.filePath("omabible-reader-scaled.png")));
    }
    void screenshot() {
        reference("John 3:16-18");
        key(Qt::Key_H,Qt::ControlModifier);
        key(Qt::Key_D,Qt::ControlModifier);
        key(Qt::Key_N,Qt::ControlModifier);
        QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        type("# A love that gives\nRead the whole conversation.\n\n- Notice the movement from love to life.\n- Return to this passage slowly.");
        key(Qt::Key_Escape);
        QSignalSpy frames(window,&QQuickWindow::frameSwapped);
        window->update();
        QTRY_VERIFY_WITH_TIMEOUT(frames.count() > 0,5000);
        QTest::qWait(250);
        const auto image = window->grabWindow();
        QVERIFY(!image.isNull());
        QCOMPARE(image.size(),window->size() * window->devicePixelRatio());
        QVERIFY(image.save(fixtures.filePath("omabible-reader.png")));
        window->resize(640,480);
        window->update(); QTest::qWait(250);
        QVERIFY(window->grabWindow().save(fixtures.filePath("omabible-reader-narrow.png")));
    }
};

int main(int argc,char **argv)
{
    QTemporaryDir environment;
    if (!environment.isValid()) return 1;
    // Set before QApplication, fontconfig, Desktop, or QStandardPaths cache state.
    for (const auto name : {"HOME","XDG_DATA_HOME","XDG_CONFIG_HOME","XDG_CACHE_HOME","XDG_STATE_HOME","XDG_RUNTIME_DIR"}) {
        const auto path = environment.filePath(QString::fromLatin1(name));
        QDir().mkpath(path);
        QFile::setPermissions(path,QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        qputenv(name,path.toUtf8());
    }
    qputenv("QT_QPA_PLATFORM","offscreen");
    qputenv("QT_QUICK_BACKEND","software");
    qputenv("QT_FORCE_STDERR_LOGGING","1");
    QApplication app(argc,argv);
    app.setOrganizationName("OmabibleUiTests");
    app.setApplicationName("omabible-ui-test");
    QQuickStyle::setStyle("Basic");
    UiTest test;
    return QTest::qExec(&test,argc,argv);
}

#include "tst_ui.moc"
