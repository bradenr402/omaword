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
    QString bodyText(int verse) const {
        auto body = object(qPrintable("verseBody" + QString::number(verse)));
        QString text;
        if (body) QMetaObject::invokeMethod(body,"getText",Q_RETURN_ARG(QString,text),
            Q_ARG(int,0),Q_ARG(int,body->property("length").toInt()));
        return text;
    }
    QPoint textPoint(int verse, int position) const {
        auto body = item(qPrintable("verseBody" + QString::number(verse)));
        QRectF rect;
        if (!body || !QMetaObject::invokeMethod(body,"positionToRectangle",Q_RETURN_ARG(QRectF,rect),Q_ARG(int,position))) return {};
        return body->mapToScene(QPointF(rect.x(),rect.y() + rect.height()/2)).toPoint();
    }
    void drag(const QPoint &from, const QPoint &to) {
        QVERIFY(QRect(QPoint(),window->size()).contains(from));
        QVERIFY(QRect(QPoint(),window->size()).contains(to));
        QTest::mousePress(window,Qt::LeftButton,Qt::NoModifier,from);
        for (int step = 1; step <= 12; ++step)
            QTest::mouseMove(window,from + (to - from) * step / 12,10);
        QTest::mouseRelease(window,Qt::LeftButton,Qt::NoModifier,to);
        QCoreApplication::processEvents();
    }
    void mouseCopy(int verse, Qt::MouseButton button) {
        const auto point = textPoint(verse,4);
        QVERIFY2(QRect(QPoint(),window->size()).contains(point),qPrintable(QString("Copy outside window: %1,%2").arg(point.x()).arg(point.y())));
        QTest::mouseClick(window,button,Qt::NoModifier,point);
        QCoreApplication::processEvents();
    }
    void installDragFixture() {
        // Distinct rich paragraphs exercise document offsets, Unicode, poetry,
        // and the join between delegates independently of the source EPUB.
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","ui-drag-fixture");
            db.setDatabaseName(bible->dataDirectory() + "/bible.sqlite");
            QVERIFY(db.open());
            QSqlQuery query(db);
            query.prepare("UPDATE verses SET text=?,html=?,poetry=?,heading=? WHERE book=43 AND chapter=3 AND number=?");
            const QStringList texts = {QString::fromUtf8("Start café — λόγος 🌿 and finish."),
                QString::fromUtf8("Poetry sings softly.\nA second line — שלום."),
                QString::fromUtf8("Final naïve résumé ends here.")};
            for (int i = 0; i < texts.size(); ++i) {
                query.bindValue(0,texts[i]);
                query.bindValue(1,QString("<span%1>%2</span>").arg(i == 1 ? " style=\"font-style:italic\"" : "",texts[i].toHtmlEscaped().replace('\n',"<br>")));
                query.bindValue(2,i == 1);
                query.bindValue(3,i == 0 ? "A section title" : "");
                query.bindValue(4,i+1);
                QVERIFY(query.exec());
            }
        }
        QSqlDatabase::removeDatabase("ui-drag-fixture");
        reference("John 3:1");
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
        QCOMPARE(prop("referencePreview", "text").toString(),bible->resolveReference("John 300:999").value("error").toString());
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
                QCOMPARE(preview,bible->resolveReference(query).value("error").toString());
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
    void paletteBackKeyboardAndEditing() {
        reference("John 3:16");
        QSignalSpy navigation(bible.get(),&BibleBackend::navigationChanged);
        key(Qt::Key_K,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("John"); key(Qt::Key_Return);
        key(Qt::Key_Up);
        QVERIFY(item("paletteBack")->hasActiveFocus());
        QVERIFY(prop("paletteBack","emphasized").toBool());
        QCOMPARE(prop("paletteResults","currentIndex").toInt(),-1);
        key(Qt::Key_Down);
        QVERIFY(item("referenceInput")->hasActiveFocus());
        QCOMPARE(prop("paletteResults","currentIndex").toInt(),0);
        key(Qt::Key_Up); key(Qt::Key_Return);
        QCOMPARE(window->property("paletteBook").toInt(),0);
        QVERIFY(item("referenceInput")->hasActiveFocus());
        type("John"); key(Qt::Key_Right); key(Qt::Key_Left);
        QCOMPARE(window->property("paletteBook").toInt(),0);
        type("John"); key(Qt::Key_Return);
        type("3:16"); key(Qt::Key_Left);
        QCOMPARE(prop("referenceInput","cursorPosition").toInt(),3);
        QCOMPARE(window->property("paletteBook").toInt(),43);
        key(Qt::Key_A,Qt::ControlModifier); key(Qt::Key_Left);
        QCOMPARE(window->property("paletteBook").toInt(),43);
        QCOMPARE(prop("referenceInput","cursorPosition").toInt(),0);
        key(Qt::Key_Left);
        QCOMPARE(window->property("paletteBook").toInt(),0);
        type("John"); key(Qt::Key_Return); type("1"); key(Qt::Key_Down);
        // Once navigating results, Left backs out even with a query present.
        key(Qt::Key_Left);
        QCOMPARE(window->property("paletteBook").toInt(),0);
        type("John"); key(Qt::Key_Return); key(Qt::Key_Up); key(Qt::Key_Left);
        QCOMPARE(window->property("paletteBook").toInt(),0);
        QCOMPARE(navigation.count(),0);
        QCOMPARE(bible->selectionReference(),QString("John 3:16"));
        key(Qt::Key_A,Qt::ControlModifier); type("not a reference 123");
        QCOMPARE(prop("referencePreview","text").toString(),bible->resolveReference("not a reference 123").value("error").toString());
        QVERIFY(prop("referencePreview","text").toString().startsWith("Reference not recognized"));
    }
    void paletteInlineGeometryAndHints_data() {
        QTest::addColumn<double>("scale");
        QTest::newRow("normal") << 1.0;
        QTest::newRow("scaled") << 1.8;
    }
    void paletteInlineGeometryAndHints() {
        QFETCH(double,scale);
        window->resize(640,480);
        QVERIFY(desktop->setProperty("textScale",scale));
        key(Qt::Key_K,Qt::ControlModifier);
        QTRY_VERIFY(prop("passagePalette","opened").toBool());
        for (const auto &mode : {QString("books"),QString("chapters"),QString("commands")}) {
            if (mode == "chapters") { type("John"); key(Qt::Key_Return); }
            if (mode == "commands") click(item("paletteCommands"));
            QTest::qWait(100);
            auto input = item("referenceInput");
            const auto inputBounds = input->mapRectToScene(QRectF(0,0,input->width(),input->height()));
            QVERIFY(input->width() > 100);
            auto previous = inputBounds;
            for (const auto name : {"paletteBooks","paletteCommands"}) {
                const auto button = item(name);
                const auto bounds = button->mapRectToScene(button->boundingRect());
                QCOMPARE(bounds.height(),inputBounds.height());
                QCOMPARE(bounds.top(),inputBounds.top());
                QVERIFY(bounds.left() > previous.right());
                QVERIFY(QRectF(16,16,608,448).contains(bounds));
                const auto label = qvariant_cast<QQuickItem *>(button->property("contentItem"));
                QVERIFY(label->width() >= label->implicitWidth());
                previous = bounds;
            }
            QVERIFY(QRectF(16,16,608,448).contains(inputBounds));
            QVERIFY(item("paletteResults")->height() >= 40);
            const auto hints = item("paletteHints");
            QVERIFY(QRectF(16,16,608,448).contains(hints->mapRectToScene(hints->boundingRect())));
            const auto text = hints->property("text").toString();
            if (mode == "commands") {
                QCOMPARE(text,QString::fromUtf8("↑ ↓ choose    ↵ run command    Esc close"));
                QVERIFY(!item("paletteBack")->isVisible());
            } else if (mode == "chapters") {
                QCOMPARE(text,QString::fromUtf8("↑ ↓ choose · ↑ first row to back    ← all books    ↵ open / back    Esc close"));
            } else QCOMPARE(text,QString::fromUtf8("↑ ↓ choose    → enter book    ↵ open    Esc close"));
        }
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
        key(Qt::Key_M,Qt::ControlModifier); QVERIFY(bible->selectionBookmarked());
        key(Qt::Key_L,Qt::ControlModifier);
        QTRY_VERIFY(prop("savedDrawer", "opened").toBool());
        QTRY_COMPARE(prop("collectionList", "count").toInt(),1);
        const QByteArray entryName = ("savedEntry" + bible->selectionReference()).toUtf8();
        QTRY_VERIFY(item(entryName.constData()));
        click(item(entryName.constData()));
        QTRY_VERIFY(!prop("savedDrawer", "visible").toBool());
        QCOMPARE(bible->selectedStart(),16); QCOMPARE(bible->selectedEnd(),17);
    }
    void freeTextAcrossVerses_data() {
        QTest::addColumn<bool>("reverse");
        QTest::addColumn<bool>("numbers");
        QTest::newRow("forward") << false << true;
        QTest::newRow("reverse") << true << true;
        QTest::newRow("forward-hidden-numbers") << false << false;
        QTest::newRow("reverse-hidden-numbers") << true << false;
    }
    void freeTextAcrossVerses() {
        QFETCH(bool,reverse); QFETCH(bool,numbers);
        installDragFixture();
        bible->setShowVerseNumbers(numbers);
        bible->setNoteText("Canonical note must survive free selection");
        QCoreApplication::processEvents();
        const auto canonical = bible->selectionReference();
        const auto note = bible->noteText();
        QSignalSpy selection(bible.get(),&BibleBackend::selectionChanged);
        const int start = 6, end = 18;
        const auto expected = bodyText(1).mid(start) + '\n' + bodyText(2) + '\n' + bodyText(3).left(end);
        QVERIFY(expected.contains(QString::fromUtf8("café — λόγος 🌿")));
        QVERIFY(expected.contains(QString::fromUtf8("שלום")));
        const auto first = textPoint(1,start), last = textPoint(3,end);
        drag(reverse ? last : first,reverse ? first : last);
        QCOMPARE(window->property("freeSelectedText").toString(),expected);
        QCOMPARE(prop("verseBody1","selectedText").toString(),bodyText(1).mid(start));
        QCOMPARE(prop("verseBody2","selectedText").toString(),bodyText(2));
        QCOMPARE(prop("verseBody3","selectedText").toString(),bodyText(3).left(end));
        QCOMPARE(bible->selectionReference(),canonical);
        QCOMPARE(bible->noteText(),note);
        QCOMPARE(selection.count(),0);
        QVERIFY(window->activeFocusItem()->objectName().startsWith("verseBody"));
        QApplication::clipboard()->setText("replace me");
        key(Qt::Key_C,Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(),expected);
        // Ctrl+C also works after moving focus out of the TextEdit.
        item("readerSurface")->forceActiveFocus();
        QApplication::clipboard()->setText("replace again");
        key(Qt::Key_C,Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(),expected);
        // Right/middle clicks must not consume the free selection or alter the
        // canonical selection before deciding which passage to copy.
        mouseCopy(2,Qt::RightButton);
        QCOMPARE(QApplication::clipboard()->text(),bible->copyVerse(2));
        QCOMPARE(window->property("freeSelectedText").toString(),expected);
        QCOMPARE(bible->noteText(),note);
        QCOMPARE(selection.count(),0);
        mouseCopy(3,Qt::MiddleButton);
        QCOMPARE(QApplication::clipboard()->text(),bible->copyVerse(3,true));
        QCOMPARE(selection.count(),0);
        QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,textPoint(2,4));
        QCOMPARE(bible->cursorVerse(),2);
        QVERIFY(window->property("freeSelectedText").toString().isEmpty());
    }
    void mouseCopySingleAndCanonicalRange() {
        installDragFixture();
        bible->selectVerses(1,2);
        bible->setNoteText("Range note");
        QTest::qWait(100);
        const auto range = bible->selectionReference();
        const auto text = bible->copySelection();
        QSignalSpy selection(bible.get(),&BibleBackend::selectionChanged);
        for (auto button : {Qt::RightButton,Qt::MiddleButton}) {
            mouseCopy(1,button);
            const auto copied = QApplication::clipboard()->text();
            QCOMPARE(copied,bible->copyVerse(1,button == Qt::MiddleButton));
            if (button == Qt::RightButton) QCOMPARE(copied,text);
            else QVERIFY(copied.contains(QChar(0x201c)) && copied.contains(QChar(0x201d)));
            mouseCopy(3,button);
            const auto single = QApplication::clipboard()->text();
            QCOMPARE(single,bible->copyVerse(3,button == Qt::MiddleButton));
            QVERIFY(single.contains("John 3:3"));
            QVERIFY(!single.contains("John 3:1"));
            QCOMPARE(bible->selectionReference(),range);
            QCOMPARE(bible->noteText(),QString("Range note"));
        }
        QCOMPARE(selection.count(),0);
        // A visible intersection still copies the whole canonical cross-chapter
        // range, including the hidden chapter, without moving its endpoints.
        reference("John 3:36-4:2");
        const auto cross = bible->selectionReference();
        const auto all = bible->copySelection();
        QCOMPARE(bible->chapter(),4);
        QVERIFY(QMetaObject::invokeMethod(window,"scrollToVerse",Q_ARG(QVariant,2),Q_ARG(QVariant,true)));
        QTest::qWait(100);
        selection.clear();
        mouseCopy(2,Qt::RightButton);
        QCOMPARE(QApplication::clipboard()->text(),all);
        mouseCopy(2,Qt::MiddleButton);
        QCOMPARE(QApplication::clipboard()->text(),bible->copyVerse(2,true));
        QCOMPARE(bible->selectionReference(),cross);
        QCOMPARE(selection.count(),0);
    }
    void dragAutoscrollAndOmittedNumbers() {
        reference("Matthew 17:20");
        bible->setShowVerseNumbers(false);
        QVERIFY(!item("verseBody21"));
        const int start = 10, end = 15;
        drag(textPoint(20,start),textPoint(22,end));
        QCOMPARE(window->property("freeSelectedText").toString(),bodyText(20).mid(start) + '\n' + bodyText(22).left(end));
        // The number gutter remains a canonical range target when labels hide.
        click(item("verseNumber20"));
        click(item("verseNumber22"),Qt::ShiftModifier);
        QCOMPARE(bible->selectedStart(),20); QCOMPARE(bible->selectedEnd(),22);
        const auto gutterPoint = [this](int verse) {
            return item(qPrintable("verseNumber" + QString::number(verse)))->mapToScene(QPointF(8,12)).toPoint();
        };
        drag(gutterPoint(22),gutterPoint(20));
        QCOMPARE(bible->selectedStart(),20); QCOMPARE(bible->selectedEnd(),22);
        QCOMPARE(bible->cursorVerse(),20);
        reference("John 3:10");
        const auto before = prop("reading","contentY").toDouble();
        const QPoint origin = textPoint(10,8);
        const QPoint bottom(origin.x(),window->height() - 80);
        QTest::mousePress(window,Qt::LeftButton,Qt::NoModifier,origin);
        QTest::mouseMove(window,bottom,30);
        QTRY_VERIFY(prop("reading","contentY").toDouble() > before + 100);
        const auto selected = window->property("freeSelectedText").toString();
        QVERIFY(selected.count('\n') >= 2);
        // Reverse the same live gesture and let it scroll back upward.
        const double down = prop("reading","contentY").toDouble();
        const QPoint top(origin.x(),5);
        QTest::mouseMove(window,top,30);
        QTRY_VERIFY(prop("reading","contentY").toDouble() < down - 100);
        QTest::mouseRelease(window,Qt::LeftButton,Qt::NoModifier,top);
        QVERIFY(!window->property("draggingText").toBool());
        QCOMPARE(bible->cursorVerse(),10);
        key(Qt::Key_C,Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(),window->property("freeSelectedText").toString());
    }
    void runtimeReadingPreferences() {
        installDragFixture();
        bible->toggleBookmark(); bible->setNoteText("Marker");
        QCoreApplication::processEvents();
        const auto x = item("verseBody1")->x();
        const auto height = item("verseBody2")->height();
        const auto heading = item("verseHeading1")->height();
        QVERIFY(heading > 0);
        QVERIFY(item("headingPrefix1")->isVisible());
        QCOMPARE(prop("headingPrefix1","text").toString(),QString("#"));
        QCOMPARE(prop("chapterTitle","leftPadding").toDouble(),x);
        bible->setShowVerseNumbers(false);
        QVERIFY(!item("verseNumberLabel1")->isVisible());
        QCOMPARE(item("verseBody1")->x(),x);
        QVERIFY(item("bookmarkMarker1")->isVisible());
        bible->setShowHeadings(false);
        QTRY_COMPARE(item("verseHeading1")->height(),0.0);
        QVERIFY(!item("headingPrefix1")->isVisible());
        bible->setLineHeight(2.5);
        QTRY_VERIFY(item("verseBody2")->height() > height * 1.4);
        bible->setShowHeadings(true);
        QTRY_COMPARE(item("verseHeading1")->height(),heading);
        bible->setShowVerseNumbers(true);
        QVERIFY(item("verseNumberLabel1")->isVisible());
        bible->setLineHeight(1.5);
        QTRY_COMPARE(item("verseBody2")->height(),height);
        key(Qt::Key_N,Qt::ControlModifier);
        QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        bible->setFocusMode(true);
        QTRY_VERIFY(item("readerSurface")->hasActiveFocus());
        for (const auto name : {"editionLabel","readingBottomFade","readerScrollbar","notesPanel","modeControl","passageControl","floatingActions","bookmarkMarker1","toast"})
            QVERIFY2(!item(name)->isVisible(),name);
        QCOMPARE(item("reading")->width(),double(window->width()));
        QVERIFY(item("verseBody1")->isVisible());
        QVERIFY(item("chapterTitle")->isVisible());
        QVERIFY(item("verseHeading1")->isVisible());
        for (int i = 0; i < 12; ++i) {
            key(Qt::Key_Tab);
            QVERIFY(!window->activeFocusItem() || window->activeFocusItem()->isVisible());
        }
        key(Qt::Key_K,Qt::ControlModifier);
        QTRY_VERIFY(prop("passagePalette","opened").toBool());
        key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        QVERIFY(bible->focusMode());
        key(Qt::Key_Escape);
        QVERIFY(!bible->focusMode());
        QVERIFY(item("readingBottomFade")->isVisible());
        QVERIFY(item("bookmarkMarker1")->isVisible());
    }
    void readerScrollbarAndHeader() {
        reference("John 3:36");
        key(Qt::Key_J);
        QTRY_COMPARE(bible->chapter(),4);
        QTRY_COMPARE(prop("reading","contentY").toDouble(),0.0);
        QVERIFY(item("chapterTitle")->mapToScene(QPointF()).y() >= 0);
        reference("John 3:1");
        QCOMPARE(prop("reading","contentY").toDouble(),0.0);
        auto bar = item("readerScrollbar");
        auto thumb = item("readerScrollThumb");
        QVERIFY(bar->isVisible());
        QVERIFY(bar->y() + bar->height() < item("modeControl")->y());
        QTest::mouseMove(window,QPoint(2,2));
        QTRY_COMPARE(thumb->width(),2.0);
        QTest::mouseMove(window,bar->mapToScene(QPointF(bar->width()/2,20)).toPoint());
        QTRY_VERIFY(bar->property("hovered").toBool());
        QTRY_COMPARE(thumb->width(),6.0);
        QTest::mouseMove(window,QPoint(2,2));
        key(Qt::Key_Space);
        QTRY_VERIFY(thumb->opacity() >= 0.49);
        QTest::qWait(1400);
        QTRY_VERIFY(thumb->opacity() <= 0.21);
        const auto buttonBackground = qvariant_cast<QQuickItem *>(prop("passageControl","background"));
        QCOMPARE(buttonBackground->property("radius").toDouble(),0.0);
        reference("John 3:36-4:2");
        QCOMPARE(prop("passageControl","text").toString(),bible->selectionReference());
    }
    void floatingControlsAndNotes_data() {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<double>("scale");
        QTest::newRow("wide") << QSize(1120,820) << 1.0;
        QTest::newRow("small") << QSize(640,480) << 1.0;
        QTest::newRow("scaled") << QSize(640,480) << 1.8;
    }
    void floatingControlsAndNotes() {
        QFETCH(QSize,size); QFETCH(double,scale);
        window->resize(size);
        QVERIFY(desktop->setProperty("textScale",scale));
        reference("John 3:16-18");
        QTest::qWait(150);
        QVERIFY(!window->property("header").value<QObject *>());
        QVERIFY(!window->property("footer").value<QObject *>());
        QCOMPARE(item("reading")->height(),double(size.height()));
        QCOMPARE(item("reading")->mapToScene(QPointF()).y(),0.0);
        auto passage = item("passageControl");
        QCOMPARE(prop("passageControl","text").toString(),bible->selectionReference());
        QVERIFY(prop("passageControl","tip").toString().contains(bible->selectionReference()));
        QCOMPARE(passage->x() + passage->width() / 2,size.width() / 2.0);
        QVERIFY(passage->width() >= passage->implicitWidth());
        QVERIFY(item("passageControlLabel")->width() >= item("passageControlLabel")->implicitWidth());
        const auto actions = item("floatingActions");
        QVERIFY(passage->x() + passage->width() < actions->x());
        for (const auto name : {"modeControl","passageControl","savedControl","notesControl","helpControl"}) {
            auto control = item(name);
            QVERIFY(control->isVisible());
            QVERIFY(QRectF(QPointF(),size).contains(control->mapRectToScene(QRectF(0,0,control->width(),control->height()))));
            const auto background = qvariant_cast<QQuickItem *>(control->property("background"));
            QVERIFY(background);
            QCOMPARE(background->property("color").value<QColor>().alpha(),0);
            if (control != passage) QVERIFY(control->property("text").toString().isEmpty());
            control->forceActiveFocus(Qt::TabFocusReason);
            QVERIFY(control->hasActiveFocus());
        }
        key(Qt::Key_Return);
        QTRY_VERIFY(prop("helpDialog","opened").toBool());
        key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("helpDialog","visible").toBool());
        click(item("modeControl")); QVERIFY(window->property("verseMode").toBool());
        click(item("modeControl")); QVERIFY(!window->property("verseMode").toBool());
        capture("omabible-refined-controls-" + QString::fromLatin1(QTest::currentDataTag()));
        click(passage);
        QTRY_VERIFY(prop("passagePalette","opened").toBool());
        capture("omabible-refined-palette-" + QString::fromLatin1(QTest::currentDataTag()));
        key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        click(item("savedControl"));
        QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        QVERIFY(!item("notesControl")->isEnabled());
        capture("omabible-refined-saved-" + QString::fromLatin1(QTest::currentDataTag()));
        key(Qt::Key_L,Qt::ControlModifier);
        QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
        click(item("notesControl"));
        QTRY_VERIFY(item("notesPanel")->isVisible());
        QCOMPARE(prop("notesPanel","color"),desktop->property("background"));
        QVERIFY(prop("noteEditor","placeholderText").toString().isEmpty());
        if (size.width() >= 1080) {
            QCOMPARE(item("reading")->width(),size.width() - prop("notesPanel","width").toDouble());
            QCOMPARE(passage->x() + passage->width() / 2,item("reading")->width() / 2);
        }
        key(Qt::Key_N,Qt::ControlModifier);
        QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        type("20g 12345 remains note text");
        QCOMPARE(bible->noteText(),QString("20g 12345 remains note text"));
        capture("omabible-refined-notes-" + QString::fromLatin1(QTest::currentDataTag()));
        // The outer notes scroller can place its heading above the viewport.
        auto viewport = qvariant_cast<QQuickItem *>(prop("notesScroll","contentItem"));
        QVERIFY(viewport); viewport->setProperty("contentY",0);
        QTest::qWait(100);
        auto close = item("notesClose");
        QTest::mouseMove(window,close->mapToScene(QPointF(close->width()/2,close->height()/2)).toPoint());
        QTRY_VERIFY(close->property("hovered").toBool());
        const auto background = qvariant_cast<QQuickItem *>(close->property("background"));
        QVERIFY(background->property("color").value<QColor>().alpha() > 0);
        click(close);
        QVERIFY(!window->property("notesOpen").toBool());
        QCOMPARE(bible->noteText(),QString("20g 12345 remains note text"));
    }
    void scaledNotesLayout_data() {
        QTest::addColumn<int>("width");
        QTest::addColumn<double>("scale");
        QTest::addColumn<bool>("alongside");
        QTest::newRow("1080-scaled") << 1080 << 1.8 << false;
        QTest::newRow("1120-scaled") << 1120 << 1.8 << false;
        QTest::newRow("1120-triple") << 1120 << 3.0 << false;
        QTest::newRow("1600-scaled") << 1600 << 1.8 << true;
        QTest::newRow("1920-triple") << 1920 << 3.0 << true;
    }
    void scaledNotesLayout() {
        QFETCH(int,width); QFETCH(double,scale); QFETCH(bool,alongside);
        reference("John 3:16");
        window->resize(width,900);
        QVERIFY(desktop->setProperty("textScale",scale));
        key(Qt::Key_N,Qt::ControlModifier);
        QTRY_VERIFY(item("notesPanel")->isVisible());
        QTest::qWait(100);
        QCOMPARE(window->property("wideLayout").toBool(),alongside);
        auto passage = item("passageControl");
        QVERIFY(passage->width() > 0);
        QCOMPARE(passage->isVisible(),alongside);
        QCOMPARE(item("floatingActions")->isVisible(),alongside);
        QCOMPARE(item("modeControl")->isVisible(),alongside);
        QCOMPARE(item("reading")->width(),alongside ? width - item("notesPanel")->width() : width);
        if (alongside) {
            const auto bounds = passage->mapRectToScene(QRectF(0,0,passage->width(),passage->height()));
            auto mode = item("modeControl");
            QVERIFY(bounds.left() > mode->x() + mode->width());
            QVERIFY(bounds.right() < item("floatingActions")->x());
            QVERIFY(item("floatingActions")->x() + item("floatingActions")->width() < item("notesPanel")->x());
        }
        // Switching scale live must restore the full reader and hide controls
        // behind overlay notes, rather than squeeze them into a negative width.
        window->resize(1120,900);
        QVERIFY(desktop->setProperty("textScale",1.0));
        QTRY_VERIFY(window->property("wideLayout").toBool());
        QVERIFY(desktop->setProperty("textScale",3.0));
        QTRY_VERIFY(!window->property("wideLayout").toBool());
        QVERIFY(passage->width() >= 0);
        QVERIFY(!passage->isVisible());
    }
    void countedVerseAndCancellation() {
        reference("John 3:16");
        type("20g"); QCOMPARE(bible->cursorVerse(),20);
        type("12G"); QCOMPARE(bible->cursorVerse(),12);
        QSignalSpy selection(bible.get(),&BibleBackend::selectionChanged);
        QSignalSpy navigation(bible.get(),&BibleBackend::navigationChanged);
        const auto before = bible->selectionReference();
        type("999g"); QCOMPARE(bible->selectionReference(),before);
        type("0g"); QCOMPARE(bible->selectionReference(),before);
        QCOMPARE(selection.count(),0); QCOMPARE(navigation.count(),0);
        QVERIFY(bible->error().isEmpty());
        reference("Matthew 17:20");
        type("21g"); QCOMPARE(bible->cursorVerse(),20);
        QVERIFY(bible->error().isEmpty());
        reference("John 3:16");
        for (const auto action : {Qt::Key_J,Qt::Key_V,Qt::Key_M,Qt::Key_X,Qt::Key_Space,Qt::Key_Z}) {
            type("20"); QCOMPARE(window->property("verseCount").toString(),QString("20"));
            key(action);
            QVERIFY(window->property("verseCount").toString().isEmpty());
        }
        type("20"); key(Qt::Key_Escape);
        QVERIFY(window->property("verseMode").toBool());
        QVERIFY(window->property("verseCount").toString().isEmpty());
        type("20"); key(Qt::Key_K,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        QVERIFY(window->property("verseCount").toString().isEmpty());
        type("12"); QCOMPARE(prop("referenceInput","text").toString(),QString("12"));
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        type("20"); key(Qt::Key_L,Qt::ControlModifier);
        QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        type("12g"); QVERIFY(window->property("verseCount").toString().isEmpty());
        key(Qt::Key_L,Qt::ControlModifier); QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
        type("20"); key(Qt::Key_N);
        QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        QVERIFY(window->property("verseCount").toString().isEmpty());
        type("20g 123"); QCOMPARE(bible->noteText(),QString("20g 123"));
        key(Qt::Key_Escape);
        type("20"); item("passageControl")->forceActiveFocus(Qt::TabFocusReason);
        QVERIFY(window->property("verseCount").toString().isEmpty());
        item("readerSurface")->forceActiveFocus();
        type("20"); QTest::qWait(2100);
        QVERIFY(window->property("verseCount").toString().isEmpty());
        key(Qt::Key_G); QCOMPARE(bible->cursorVerse(),1);
    }
    void fiveAndTenVerseMovement() {
        reference("John 3:34");
        key(Qt::Key_D,Qt::ControlModifier);
        QCOMPARE(bible->selectionReference(),QString("John 4:3"));
        QVERIFY(!bible->selectionBookmarked());
        key(Qt::Key_U,Qt::ControlModifier);
        QCOMPARE(bible->selectionReference(),QString("John 3:34"));
        key(Qt::Key_PageDown); QCOMPARE(bible->selectionReference(),QString("John 4:8"));
        key(Qt::Key_PageUp); QCOMPARE(bible->selectionReference(),QString("John 3:34"));
        key(Qt::Key_V); key(Qt::Key_D,Qt::ControlModifier);
        QCOMPARE(bible->selectionReference(),QString::fromUtf8("John 3:34–4:3"));
        QVERIFY(window->property("verseMode").toBool());
        key(Qt::Key_PageDown); QCOMPARE(bible->cursorVerse(),13);
        QCOMPARE(bible->selectionReference(),QString::fromUtf8("John 3:34–4:13"));
        key(Qt::Key_PageUp); key(Qt::Key_U,Qt::ControlModifier);
        QCOMPARE(bible->selectionReference(),QString("John 3:34"));
        key(Qt::Key_M,Qt::ControlModifier); QVERIFY(bible->selectionBookmarked());
        key(Qt::Key_Home); QCOMPARE(bible->cursorVerse(),1);
        key(Qt::Key_End); QCOMPARE(bible->cursorVerse(),36);
        key(Qt::Key_Right); QCOMPARE(bible->chapter(),4);
        key(Qt::Key_Left); QCOMPARE(bible->chapter(),3);
    }
    void verseMovementContext_data() {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<double>("scale");
        QTest::newRow("large") << QSize(1120,1000) << 1.0;
        QTest::newRow("small") << QSize(640,480) << 1.0;
        QTest::newRow("long-scaled") << QSize(640,480) << 1.8;
    }
    void verseMovementContext() {
        QFETCH(QSize,size); QFETCH(double,scale);
        window->resize(size); QVERIFY(desktop->setProperty("textScale",scale));
        reference("John 3:15");
        for (const auto movement : {Qt::Key_J,Qt::Key_K,Qt::Key_J,Qt::Key_Down,Qt::Key_Up}) {
            key(movement);
            QTest::qWait(100);
            auto body = item(qPrintable("verseBody" + QString::number(bible->cursorVerse())));
            QVERIFY(body);
            const double y = body->mapToItem(item("reading"),QPointF()).y();
            const double available = item("reading")->height() - window->property("floatingSafety").toDouble();
            const double threeLines = window->property("readerLineHeight").toDouble() * 3;
            if (body->height() + 2 * threeLines <= available) {
                QVERIFY2(y >= threeLines - 1,qPrintable(QString("Top context %1 < %2").arg(y).arg(threeLines)));
                QVERIFY(y + body->height() <= available - threeLines + 1);
            } else {
                QVERIFY(y >= 12 * scale - 1);
                QVERIFY(y + qMin(body->height(),window->property("readerLineHeight").toDouble()) < available);
            }
        }
        key(Qt::Key_End); QTest::qWait(150);
        auto last = item("verseBody36");
        QVERIFY(last);
        if (last->height() < item("reading")->height() - window->property("floatingSafety").toDouble())
            QVERIFY(last->mapToItem(item("reading"),QPointF()).y() + last->height()
                <= item("reading")->height() - window->property("floatingSafety").toDouble() + 1);
    }
    void normalScrollingAndChapterNavigation() {
        reference("John 3");
        const double before = prop("reading","contentY").toDouble();
        key(Qt::Key_Space);
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
        key(Qt::Key_Space); key(Qt::Key_Space);
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
        key(Qt::Key_Space); key(Qt::Key_Space);
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
    void savedTabFocusActivation() {
        reference("John 3:1"); bible->toggleBookmark();
        reference("John 3:2"); bible->toggleBookmark(); bible->toggleHighlight();
        reference("John 4:1");
        QSignalSpy navigation(bible.get(),&BibleBackend::navigationChanged);
        const auto tabTo = [this](const char *name) {
            QStringList visited;
            for (int i = 0; i < 40; ++i) {
                key(Qt::Key_Tab);
                if (item(name) && window->activeFocusItem() == item(name)) return true;
                visited << (window->activeFocusItem() ? window->activeFocusItem()->objectName() : "<none>");
            }
            qWarning() << "Tab target" << name << "visited" << visited;
            return false;
        };
        key(Qt::Key_L,Qt::ControlModifier);
        QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        QVERIFY(tabTo("savedClose"));
        key(Qt::Key_Return);
        QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
        QCOMPARE(navigation.count(),0);

        key(Qt::Key_L,Qt::ControlModifier);
        QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        QVERIFY(tabTo("savedTab2")); key(Qt::Key_Enter);
        QCOMPARE(window->property("collectionTab").toInt(),2);
        QCOMPARE(prop("collectionList","count").toInt(),1);
        QCOMPARE(navigation.count(),0);
        QVERIFY(tabTo("savedTab0")); key(Qt::Key_Space);
        QCOMPARE(window->property("collectionTab").toInt(),0);
        QCOMPARE(prop("collectionList","count").toInt(),2);

        for (const auto name : {"savedExport","savedBackup"}) {
            QVERIFY(tabTo(name));
            auto dialog = object(QByteArray(name) == "savedExport" ? "exportDialog" : "backupDialog");
            QVERIFY(dialog);
            // Inspect opening and cancel; never choose a path or write a file.
            key(Qt::Key_Return);
            QTRY_VERIFY(dialog->property("visible").toBool());
            QCOMPARE(navigation.count(),0);
            QVERIFY(QMetaObject::invokeMethod(dialog,"reject"));
            QTRY_VERIFY(!dialog->property("visible").toBool());
            QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
            window->requestActivate();
            QTRY_VERIFY(window->isActive());
            item("readerSurface")->forceActiveFocus();
            key(Qt::Key_L,Qt::ControlModifier);
            QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        }
        for (auto activation : {Qt::Key_Space,Qt::Key_Return,Qt::Key_Enter}) {
            QVERIFY(tabTo("savedEntryJohn 3:2"));
            // Tab focus and list selection are independent; activate the focused row.
            object("collectionList")->setProperty("currentIndex",0);
            QCOMPARE(window->activeFocusItem(),item("savedEntryJohn 3:2"));
            key(activation);
            QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
            QCOMPARE(bible->selectionReference(),QString("John 3:2"));
            key(Qt::Key_L,Qt::ControlModifier);
            QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        }
        QVERIFY(tabTo("savedClose")); key(Qt::Key_J);
        QCOMPARE(window->activeFocusItem(),item("collectionList"));
        key(Qt::Key_Return);
        QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
        QCOMPARE(bible->selectionReference(),QString("John 3:2"));
    }
    void savedKeyboardIsolationAndToggle() {
        for (int verse = 1; verse <= 12; ++verse) {
            QVERIFY(bible->navigate(QString("John 3:%1").arg(verse)));
            bible->toggleBookmark();
            if (verse == 2) bible->toggleHighlight();
            if (verse == 3) bible->setNoteText("Saved keyboard note");
        }
        reference("John 4:2");
        QTest::qWait(450);
        const auto collection = bible->collection();
        const auto position = prop("reading","contentY").toDouble();
        QSignalSpy navigation(bible.get(),&BibleBackend::navigationChanged);
        QSignalSpy selection(bible.get(),&BibleBackend::selectionChanged);
        QApplication::clipboard()->setText("Saved isolation");
        key(Qt::Key_L,Qt::ControlModifier);
        QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        QVERIFY(item("collectionList")->hasActiveFocus());
        QCOMPARE(prop("savedTitle","text").toString(),QString("Saved"));
        QCOMPARE(prop("savedDrawer","edge").toInt(),int(Qt::RightEdge));
        key(Qt::Key_J); QCOMPARE(prop("collectionList","currentIndex").toInt(),1);
        key(Qt::Key_Down); QCOMPARE(prop("collectionList","currentIndex").toInt(),2);
        key(Qt::Key_K); QCOMPARE(prop("collectionList","currentIndex").toInt(),1);
        key(Qt::Key_Up); QCOMPARE(prop("collectionList","currentIndex").toInt(),0);
        for (int i = 0; i < 20; ++i) key(Qt::Key_J);
        QCOMPARE(prop("collectionList","currentIndex").toInt(),11);
        auto current = qvariant_cast<QQuickItem *>(prop("collectionList","currentItem"));
        QVERIFY(current);
        QVERIFY(current->property("highlighted").toBool());
        const auto rowBounds = current->mapRectToItem(item("collectionList"),current->boundingRect());
        QVERIFY(item("collectionList")->boundingRect().adjusted(-1,-1,1,1).contains(rowBounds));
        const auto background = qvariant_cast<QQuickItem *>(current->property("background"));
        QVERIFY(background);
        QVERIFY(background->property("color").value<QColor>().alpha() > 0);
        key(Qt::Key_L); QCOMPARE(window->property("collectionTab").toInt(),1);
        QCOMPARE(prop("collectionList","count").toInt(),12);
        key(Qt::Key_Right); QCOMPARE(window->property("collectionTab").toInt(),2);
        QCOMPARE(prop("collectionList","count").toInt(),1);
        key(Qt::Key_Right); QCOMPARE(window->property("collectionTab").toInt(),3);
        QCOMPARE(prop("collectionList","count").toInt(),1);
        key(Qt::Key_H); QCOMPARE(window->property("collectionTab").toInt(),2);
        key(Qt::Key_Left); QCOMPARE(window->property("collectionTab").toInt(),1);
        for (const auto k : {Qt::Key_M,Qt::Key_X,Qt::Key_N,Qt::Key_V,Qt::Key_Y,Qt::Key_G,
                            Qt::Key_BracketLeft,Qt::Key_BracketRight,Qt::Key_PageDown,Qt::Key_Space}) {
            key(k);
            QVERIFY2(navigation.count() == 0,qPrintable(QString("Saved leaked key %1").arg(int(k))));
        }
        key(Qt::Key_J,Qt::ShiftModifier);
        key(Qt::Key_Right,Qt::AltModifier);
        key(Qt::Key_H,Qt::ControlModifier);
        key(Qt::Key_D,Qt::ControlModifier);
        key(Qt::Key_N,Qt::ControlModifier);
        key(Qt::Key_K,Qt::ControlModifier);
        QCOMPARE(navigation.count(),0); QCOMPARE(selection.count(),0);
        QCOMPARE(bible->collection(),collection);
        QCOMPARE(prop("reading","contentY").toDouble(),position);
        QCOMPARE(QApplication::clipboard()->text(),QString("Saved isolation"));
        QVERIFY(!window->property("verseMode").toBool());
        QVERIFY(!window->property("notesOpen").toBool());
        QVERIFY(!prop("passagePalette","visible").toBool());
        key(Qt::Key_L,Qt::ControlModifier);
        QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
        QVERIFY(item("readerSurface")->hasActiveFocus());
        key(Qt::Key_L,Qt::ControlModifier);
        QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        key(Qt::Key_Right); key(Qt::Key_Return);
        QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
        QCOMPARE(bible->selectionReference(),QString("John 3:2"));
        key(Qt::Key_L,Qt::ControlModifier);
        QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        key(Qt::Key_Right); key(Qt::Key_Enter);
        QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
        QCOMPARE(bible->selectionReference(),QString("John 3:3"));
        QCOMPARE(bible->noteText(),QString("Saved keyboard note"));
        QVERIFY(window->property("notesOpen").toBool());
    }
    void savedEmptyStates() {
        window->resize(640,480);
        QVERIFY(desktop->setProperty("textScale",1.8));
        const QStringList messages = {
            "No saved passages yet.", "No bookmarks yet.", "No highlights yet.", "No notes yet."
        };
        key(Qt::Key_L,Qt::ControlModifier);
        QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        for (int tab = 0; tab < 4; ++tab) {
            QTest::qWait(100);
            QCOMPARE(window->property("collectionTab").toInt(),tab);
            QCOMPARE(prop("collectionList","count").toInt(),0);
            QVERIFY(item("savedEmpty")->isVisible());
            QCOMPARE(prop("savedEmpty","text").toString(),messages[tab]);
            const auto bounds = item("savedEmpty")->mapRectToItem(item("collectionList"),item("savedEmpty")->boundingRect());
            QVERIFY(item("collectionList")->boundingRect().contains(bounds));
            key(Qt::Key_J); key(Qt::Key_K); key(Qt::Key_Return);
            QVERIFY(prop("savedDrawer","opened").toBool());
            key(Qt::Key_Right);
        }
        QCOMPARE(window->property("collectionTab").toInt(),0);
        key(Qt::Key_Left); QCOMPARE(window->property("collectionTab").toInt(),3);
        key(Qt::Key_L,Qt::ControlModifier);
        QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
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
        QCOMPARE(prop("savedDrawer","edge").toInt(),int(Qt::RightEdge));
        QCOMPARE(prop("savedTitle","text").toString(),QString("Saved"));
        QCOMPARE(bounds.left(),size.width() - prop("savedDrawer","width").toDouble() + 24 * scale);
        QVERIFY(qAbs(bounds.top() - 24 * scale) < 0.1);
        QCOMPARE(size.width() - bounds.right(),24 * scale);
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
        QTRY_VERIFY(item("verseNumber16")->mapToItem(item("reading"),QPointF()).y() <= window->property("readerLineHeight").toDouble() * 3 + 1);
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
        key(Qt::Key_M,Qt::ControlModifier);
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
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc,argv);
    app.setOrganizationName("OmabibleUiTests");
    app.setApplicationName("omabible-ui-test");
    QQuickStyle::setStyle("Basic");
    UiTest test;
    return QTest::qExec(&test,argc,argv);
}

#include "tst_ui.moc"
