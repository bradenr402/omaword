#include <QtTest/QTest>
#include <QApplication>
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
        QVERIFY(!bible->error().isEmpty());
        QVERIFY(prop("passagePalette", "visible").toBool());
        QCOMPARE(bible->chapter(),3); QCOMPARE(bible->selectedStart(),16);
        key(Qt::Key_A, Qt::ControlModifier); type("Jn 4:2"); key(Qt::Key_Return);
        QTRY_VERIFY(!prop("passagePalette", "visible").toBool());
        QVERIFY(bible->error().isEmpty()); QCOMPARE(bible->chapter(),4);
    }
    void annotationKeysAndNoteIsolation() {
        reference("John 3:16");
        key(Qt::Key_V);
        QVERIFY(window->property("verseMode").toBool());
        key(Qt::Key_J); QCOMPARE(bible->selectedStart(),17);
        key(Qt::Key_K); QCOMPARE(bible->selectedStart(),16);
        key(Qt::Key_H); QVERIFY(bible->selectionHighlighted());
        key(Qt::Key_M); QVERIFY(bible->selectionBookmarked());
        key(Qt::Key_N, Qt::ControlModifier);
        QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        const QString first = "# John 3\nj k h m v y g are note text, not reader commands.";
        type(first);
        QCOMPARE(prop("noteEditor", "text").toString(),first);
        QCOMPARE(bible->noteText(),first);
        QCOMPARE(bible->selectedStart(),16);
        QVERIFY(bible->selectionHighlighted()); QVERIFY(bible->selectionBookmarked());
        QVERIFY(window->property("verseMode").toBool());
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
        QVERIFY(preview.contains("j k h m v y g are note text"));
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
        key(Qt::Key_J);
        QVERIFY(prop("reading","contentY").toDouble() > before);
        QCOMPARE(bible->selectedStart(),0);
        key(Qt::Key_Right,Qt::AltModifier);
        QTRY_COMPARE(bible->chapter(),4);
        key(Qt::Key_Left,Qt::AltModifier);
        QTRY_COMPARE(bible->chapter(),3);
        key(Qt::Key_BracketRight); QCOMPARE(bible->chapter(),4);
        key(Qt::Key_BracketLeft); QCOMPARE(bible->chapter(),3);
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
        type("keyboard"); key(Qt::Key_Return);
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
    void restoreConfirmationCancelAndAccept() {
        reference("John 3:16");
        key(Qt::Key_H); key(Qt::Key_M);
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
