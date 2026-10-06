#include "testenvironment.h"
#include <QtTest/QTest>
#include <QElapsedTimer>
#include <QThread>
#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QJSValue>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickStyle>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextFragment>
#include <archive.h>
#include <archive_entry.h>
#include "biblebackend.h"
#include "blockdocument.h"
#include "desktop.h"
#include "soundeffects.h"
#include <cmath>
#include "versioncatalog.h"

class MotionTest : public QObject
{
    Q_OBJECT
    QTemporaryDir fixtures;
    std::unique_ptr<QTemporaryDir> session;
    std::unique_ptr<BibleBackend> bible;
    std::unique_ptr<Desktop> desktop;
    std::unique_ptr<SoundEffects> sounds;
    std::unique_ptr<VersionCatalog> catalog;
    std::unique_ptr<QQmlApplicationEngine> engine;
    QQuickWindow *window = nullptr;
    QStringList warnings;
    QString first, second;
    static bool edition(const QString &path, const QString &id, bool alternate) {
        auto zip = archive_write_new(); archive_write_set_format_zip(zip);
        if (archive_write_open_filename(zip, QFile::encodeName(path).constData()) != ARCHIVE_OK) { archive_write_free(zip); return false; }
        QByteArray data = "\\id JHN Original search UI fixture\n\\h John\n\\toc1 John\n\\toc2 John\n";
        for (int chapter = 1; chapter <= 2; ++chapter) {
            data += QString("\\c %1\n\\s First source heading\n\\p\n").arg(chapter).toUtf8();
            for (int verse = 1; verse <= 65; ++verse) {
                if (verse > 1 && verse % 4 == 1) data += "\\p\n";
                QString text = QString("Common original fixture %1:%2. ").arg(chapter).arg(verse);
                if (verse == 2) text += alternate ? "Changed edition without the word." : "ALPHA and beta with alphabet soup. A bridge begins";
                else if (verse == 3) text = "here across verses. Common original text. Blue & <bright> original text.";
                else if (verse == 8 || verse == 20 || verse == 50) text += alternate ? "Different words." : "Alpha beta and another alpha.";
                else text += QString("Ordinary words for a long scrolling chapter. ").repeated(4);
                data += QString("\\v %1 %2\n").arg(verse).arg(text).toUtf8();
            }
        }
        auto entry = archive_entry_new(); const auto name = ("01-JHN" + id + ".usfm").toUtf8();
        archive_entry_set_pathname(entry, name.constData()); archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0600); archive_entry_set_size(entry, data.size());
        bool ok = archive_write_header(zip, entry) == ARCHIVE_OK && archive_write_data(zip, data.constData(), data.size()) == data.size();
        archive_entry_free(entry); ok = archive_write_close(zip) == ARCHIVE_OK && ok; archive_write_free(zip); return ok;
    }
    static QVariant value(QVariant v) { return v.metaType() == QMetaType::fromType<QJSValue>() ? v.value<QJSValue>().toVariant() : v; }
    static QQuickItem *visual(QQuickItem *root, const QString &name) {
        if (root->objectName() == name) return root;
        for (auto child : root->childItems()) if (auto found = visual(child, name)) return found;
        return nullptr;
    }
    QObject *object(const QString &name) const {
        if (auto found = window->findChild<QObject *>(name)) return found;
        return visual(window->contentItem(), name);
    }
    QQuickItem *item(const QString &name) const { return qobject_cast<QQuickItem *>(object(name)); }
    QVariant prop(const char *name) const { return value(window->property(name)); }
    static QRectF caret(QObject *body, int position) {
        QRectF rect;
        QMetaObject::invokeMethod(body, "positionToRectangle", Q_RETURN_ARG(QRectF, rect), Q_ARG(int, position));
        return rect;
    }
    static QTextDocument *document(QObject *body) {
        const auto quick = body->property("textDocument").value<QQuickTextDocument *>();
        return quick ? quick->textDocument() : nullptr;
    }
    BlockDocument *blockMapping() const { return window->findChild<BlockDocument *>(); }
    QRectF verseViewportRect(int verse) const {
        if (prop("continuousReader").toBool()) {
            auto body = item("blockBody");
            return body->mapRectToItem(item("reading"), caret(body, blockMapping()->positionFor(verse)));
        }
        auto body = item("verseBody" + QString::number(verse));
        return body->mapRectToItem(item("reading"), body->boundingRect());
    }
    void call(const char *method) { QVERIFY(QMetaObject::invokeMethod(window, method)); QCoreApplication::processEvents(); }
    void key(Qt::Key key, Qt::KeyboardModifiers mods = Qt::NoModifier) { QTest::keyClick(window, key, mods); QCoreApplication::processEvents(); }
    void type(const QString &text) { for (const auto c : text) QTest::keyClick(window, c.toLatin1()); QCoreApplication::processEvents(); }
    void install(const QString &file) {
        bible->importUsfm(QUrl::fromLocalFile(file)); QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(), 30000);
        QVERIFY2(bible->ready() && bible->error().isEmpty(), qPrintable(bible->error()));
    }
    void navigate(const QString &reference) {
        QVERIFY(bible->navigate(reference)); call("readerFocus"); QTRY_VERIFY(!prop("restoringPosition").toBool());
    }
    void find(const QString &query) {
        key(Qt::Key_Slash); QTRY_VERIFY(prop("findPromptOpen").toBool());
        QTRY_VERIFY(item("chapterFindInput")->hasActiveFocus()); type(query); QTest::qWait(200);
    }
    void search(const QString &query) {
        key(Qt::Key_Slash, Qt::ControlModifier); QTRY_VERIFY(object("passagePalette")->property("opened").toBool());
        QCOMPARE(object("referenceInput")->property("text").toString(), QString("/ "));
        type(query); QTRY_VERIFY(prop("paletteSearchCurrent").toBool()); QTRY_VERIFY(!bible->searchBusy());
    }
    QVariantList desk() const { return bible->desk(); }
    QString toast() const { return object("toast")->property("text").toString(); }
    bool deskOpen() const { return object("deskDrawer")->property("opened").toBool(); }
    void openDesk() { key(Qt::Key_D, Qt::ShiftModifier); QTRY_VERIFY(deskOpen()); QTRY_VERIFY(item("deskList")->hasActiveFocus()); }
    QString commandTitle(const QString &query) {
        QVariant entries; window->setProperty("verseCount", "");
        object("referenceInput")->setProperty("text", "> " + query);
        QMetaObject::invokeMethod(window, "paletteEntries", Q_RETURN_ARG(QVariant, entries));
        const auto list = value(entries).toList();
        return list.isEmpty() ? QString() : list.first().toMap().value("title").toString();
    }
    QString secondId() const {
        for (const auto &v : bible->versions()) if (v.toMap()["id"].toString() != bible->versionId()) return v.toMap()["id"].toString();
        return {};
    }
    QObject *menu() const { return object("contextMenu"); }
    void setLayout(const QString &mode) {
        bible->setReadingMode(mode);
        QTRY_COMPARE(prop("continuousReader").toBool(), mode != "verse");
        if (mode != "verse") QTRY_VERIFY(blockMapping() && blockMapping()->spans().size() == bible->verses().size());
        QTest::qWait(80);
        call("readerFocus");
    }
    QPoint versePoint(int verse) const {
        if (prop("continuousReader").toBool()) {
            auto body = item("blockBody");
            const auto rect = caret(body, blockMapping()->positionFor(verse) + 4);
            return body->mapToScene(rect.center()).toPoint();
        }
        return item("verseBody" + QString::number(verse))->mapToScene(QPointF(30, 8)).toPoint();
    }
    void layouts() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("verse") << QString("verse");
        QTest::newRow("paragraph") << QString("paragraph");
        QTest::newRow("block") << QString("block");
    }
    QObject *block() const { return object("blockReader"); }
    // Verse-row item (opacity carrier) in the Verse layout.
    QQuickItem *row(int verse) const {
        auto body = item("verseBody" + QString::number(verse));
        for (auto p = body ? body->parentItem() : nullptr; p; p = p->parentItem())
            if (p->metaObject()->indexOfProperty("dimmed") >= 0) return p;
        return nullptr;
    }
    // Dim strength over a verse: 0 undimmed, >0 dimmed, in either layout.
    double dim(int verse) const {
        if (prop("continuousReader").toBool()) {
            QVariant level; QMetaObject::invokeMethod(block(), "dimLevel", Q_RETURN_ARG(QVariant, level), Q_ARG(QVariant, verse));
            return level.toDouble();
        }
        return 1.0 - row(verse)->opacity();
    }
    void settle() { QTest::qWait(400); }
    // Geometry of every verse's start as laid out now.
    QList<QRectF> geometry() const {
        QList<QRectF> rects;
        for (int verse = 1; verse <= 12; ++verse) {
            if (prop("continuousReader").toBool()) rects << caret(item("blockBody"), blockMapping()->positionFor(verse)).translated(item("blockBody")->mapToItem(item("chapterTitle")->parentItem(), QPointF()));
            else { auto body = item("verseBody" + QString::number(verse)); rects << body->mapRectToItem(item("chapterTitle")->parentItem(), body->boundingRect()); }
        }
        return rects;
    }
    QString readerText() const {
        if (prop("continuousReader").toBool()) return item("blockBody")->property("text").toString();
        QString text; for (int verse = 1; verse <= 12; ++verse) text += item("verseBody" + QString::number(verse))->property("text").toString();
        return text;
    }
    void expectPulse(int first, int last) {
        QTRY_VERIFY(prop("pulseActive").toBool());
        QCOMPARE(prop("pulseFirst").toInt(), first); QCOMPARE(prop("pulseLast").toInt(), last);
        if (!prop("continuousReader").toBool()) QTRY_VERIFY(item("jumpPulse" + QString::number(first))->isVisible());
        else QTRY_VERIFY(item("blockMotionBackground")->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(!prop("pulseActive").toBool(), 3000);
        if (!prop("continuousReader").toBool()) QVERIFY(!item("jumpPulse" + QString::number(first))->isVisible());
        else QVERIFY(!item("blockMotionBackground")->isVisible());
    }
    void expectFlash(int first, int last) {
        QTRY_VERIFY(prop("flashActive").toBool());
        QCOMPARE(prop("flashFirst").toInt(), first); QCOMPARE(prop("flashLast").toInt(), last);
        QTRY_VERIFY_WITH_TIMEOUT(!prop("flashActive").toBool(), 3000);
        QCOMPARE(prop("flashLevel").toDouble(), 0.0);
    }
private slots:
    void initTestCase() {
        first = fixtures.filePath("engnet.zip"); second = fixtures.filePath("eng-kjv.zip");
        QVERIFY(edition(first, "engnet", false)); QVERIFY(edition(second, "eng-kjv", true));
    }
    void init() {
        session = std::make_unique<QTemporaryDir>(); qputenv("XDG_DATA_HOME", session->path().toUtf8());
        bible = std::make_unique<BibleBackend>(); install(first); bible->setReadingMode("verse");
        QVERIFY(bible->hasParagraphs());
        desktop = std::make_unique<Desktop>(); sounds = std::make_unique<SoundEffects>(nullptr, session->filePath("sound.ini"), false); catalog = std::make_unique<VersionCatalog>();
        QVERIFY(!sounds->reduceMotion());
        engine = std::make_unique<QQmlApplicationEngine>(); warnings.clear();
        connect(engine.get(), &QQmlEngine::warnings, this, [this](const QList<QQmlError> &errors) { for (const auto &error : errors) warnings << error.toString(); });
        engine->rootContext()->setContextProperty("bible", bible.get());
        engine->rootContext()->setContextProperty("desktop", desktop.get());
        engine->rootContext()->setContextProperty("sounds", sounds.get());
        engine->rootContext()->setContextProperty("catalog", catalog.get());
        engine->load(QUrl::fromLocalFile(QFINDTESTDATA("../qml/Main.qml")));
        QVERIFY2(!engine->rootObjects().isEmpty(), qPrintable(warnings.join('\n')));
        window = qobject_cast<QQuickWindow *>(engine->rootObjects().first()); QVERIFY(window);
        window->requestActivate(); QVERIFY(QTest::qWaitForWindowExposed(window)); QTRY_VERIFY(window->isActive());
        navigate("John 1:1");
    }
    void cleanup() {
        engine.reset(); window = nullptr; catalog.reset(); sounds.reset(); desktop.reset(); bible.reset(); session.reset();
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
    }

    void dimmingOnlyInFocusMode_data() { layouts(); }
    void dimmingOnlyInFocusMode() {
        QFETCH(QString, mode);
        setLayout(mode); navigate("John 1:5");
        for (int verse : {1, 4, 5, 6, 9}) QCOMPARE(dim(verse), 0.0);
        if (mode != "verse") QVERIFY(!item("blockFocusDim")->isVisible());
        key(Qt::Key_F); QTRY_VERIFY(bible->focusMode());
        if (mode != "verse") QTRY_VERIFY(blockMapping() && blockMapping()->spans().size() == bible->verses().size());
        settle();
        QCOMPARE(dim(5), 0.0);
        for (int verse : {1, 4, 6, 9}) QVERIFY2(dim(verse) > 0.5, qPrintable(QString::number(verse)));
        if (mode != "verse") {
            auto veil = item("blockFocusDim");
            QVERIFY(veil->isVisible()); QCOMPARE(veil->opacity(), 1.0);
            // Painted above the text and inert for input.
            QVERIFY(veil->z() > item("blockBody")->z());
            QCOMPARE(veil->acceptedMouseButtons(), Qt::NoButton);
        } else QVERIFY(item("chapterTitle")->opacity() < 1);
        // A visual range keeps the whole range undimmed.
        key(Qt::Key_V, Qt::ShiftModifier); key(Qt::Key_J); settle();
        QCOMPARE(dim(5), 0.0); QCOMPARE(dim(6), 0.0); QVERIFY(dim(7) > 0.5); QVERIFY(dim(4) > 0.5);
        key(Qt::Key_Escape); key(Qt::Key_Escape); QTRY_VERIFY(!bible->focusMode());
        settle();
        for (int verse : {1, 4, 5, 6, 9}) QCOMPARE(dim(verse), 0.0);
        if (mode != "verse") QTRY_VERIFY(!item("blockFocusDim")->isVisible());
        else QCOMPARE(item("chapterTitle")->opacity(), 1.0);
    }
    void escapeCollapsesRangeBeforeLeavingFocus() {
        navigate("John 1:5");
        key(Qt::Key_F); QTRY_VERIFY(bible->focusMode());
        // Visual mode: the first Escape leaves Visual at the cursor.
        key(Qt::Key_V, Qt::ShiftModifier); key(Qt::Key_J); key(Qt::Key_J);
        QVERIFY(prop("verseMode").toBool()); QCOMPARE(bible->selectedStart(), 5); QCOMPARE(bible->selectedEnd(), 7);
        key(Qt::Key_Escape);
        QVERIFY(bible->focusMode()); QVERIFY(!prop("verseMode").toBool());
        QCOMPARE(bible->selectedStart(), 7); QCOMPARE(bible->selectedEnd(), 7); QCOMPARE(bible->cursorVerse(), 7);
        key(Qt::Key_Escape); QVERIFY(!bible->focusMode());
        // A multi-verse selection outside Visual mode collapses first, too.
        key(Qt::Key_F); QTRY_VERIFY(bible->focusMode());
        QVERIFY(bible->navigate("John 1:2-4")); call("readerFocus");
        QVERIFY(!prop("verseMode").toBool()); QCOMPARE(bible->selectedEnd() - bible->selectedStart(), 2);
        key(Qt::Key_Escape);
        QVERIFY(bible->focusMode()); QCOMPARE(bible->selectedStart(), bible->selectedEnd());
        // A collapsed single-verse selection never blocks leaving focus mode.
        key(Qt::Key_Escape); QVERIFY(!bible->focusMode());
        // A pending count is still cleared before the range.
        key(Qt::Key_F); QTRY_VERIFY(bible->focusMode());
        key(Qt::Key_V, Qt::ShiftModifier); key(Qt::Key_J); type("3");
        key(Qt::Key_Escape); QCOMPARE(prop("verseCount").toString(), QString()); QVERIFY(prop("verseMode").toBool());
        key(Qt::Key_Escape); QVERIFY(!prop("verseMode").toBool()); QVERIFY(bible->focusMode());
        key(Qt::Key_Escape); QVERIFY(!bible->focusMode());
    }
    void dimmingKeepsGeometryCopyAndHits_data() { layouts(); }
    void dimmingKeepsGeometryCopyAndHits() {
        QFETCH(QString, mode);
        setLayout(mode);
        key(Qt::Key_F); QTRY_VERIFY(bible->focusMode());
        if (mode != "verse") QTRY_VERIFY(blockMapping() && blockMapping()->spans().size() == bible->verses().size());
        navigate("John 1:2"); settle();
        const auto rects = geometry(); const auto text = readerText();
        QVERIFY(dim(4) > 0.5);
        navigate("John 1:4"); settle();
        QCOMPARE(dim(4), 0.0); QVERIFY(dim(2) > 0.5);
        QCOMPARE(geometry(), rects); QCOMPARE(readerText(), text);
        // Copy output is the backend quotation, unaffected by the paint layer.
        bible->selectVerses(4, 4); const auto expected = bible->copySelection();
        QGuiApplication::clipboard()->clear(); key(Qt::Key_Y);
        QTRY_COMPARE(QGuiApplication::clipboard()->text(), expected);
        // Clicking a dimmed verse still selects it (hit-testing unchanged).
        settle();
        QTest::mouseClick(window, Qt::LeftButton, {}, versePoint(7)); QCoreApplication::processEvents();
        QTRY_COMPARE(bible->cursorVerse(), 7);
        settle(); QCOMPARE(dim(7), 0.0); QVERIFY(dim(4) > 0.5);
        QCOMPARE(geometry(), rects);
    }
    void dimmingInstantOnCursorMove_data() { layouts(); }
    void heldVerseMotionRepeatsWithoutPlatformDelay_data() {
        QTest::addColumn<int>("key"); QTest::addColumn<int>("direction");
        QTest::newRow("j") << int(Qt::Key_J) << 1;
        QTest::newRow("down") << int(Qt::Key_Down) << 1;
        QTest::newRow("k") << int(Qt::Key_K) << -1;
        QTest::newRow("up") << int(Qt::Key_Up) << -1;
    }
    void heldVerseMotionRepeatsWithoutPlatformDelay() {
        QFETCH(int, key); QFETCH(int, direction);
        navigate("John 1:20");
        const int start = bible->cursorVerse();
        // A tap moves exactly one verse.
        QTest::keyClick(window, Qt::Key(key)); QTest::qWait(250);
        QCOMPARE(bible->cursorVerse(), start + direction);
        // Holding (no platform auto-repeat events at all) keeps moving well
        // before a typical ~600 ms platform repeat delay would elapse.
        QTest::keyPress(window, Qt::Key(key));
        QTest::qWait(400);
        const int held = bible->cursorVerse();
        QVERIFY2((held - start) * direction >= 4, qPrintable(QString("moved %1").arg(held - start)));
        // Releasing stops it immediately.
        QTest::keyRelease(window, Qt::Key(key));
        const int released = bible->cursorVerse();
        QTest::qWait(250);
        QCOMPARE(bible->cursorVerse(), released);
    }
    void verseTapDuringStallDoesNotRepeat_data() { heldVerseMotionRepeatsWithoutPlatformDelay_data(); }
    void verseTapDuringStallDoesNotRepeat() {
        // A tap whose press handler (or the frame after it) blocks the GUI
        // thread past the hold delay: the overdue hold timer is dispatched
        // while the release is still in flight from the platform.
        QFETCH(int, key); QFETCH(int, direction);
        navigate("John 1:20"); settle();
        const int start = bible->cursorVerse();
        QTest::keyPress(window, Qt::Key(key));
        QThread::msleep(400);
        // The loop runs briefly (the frame after the stall) before the release.
        QElapsedTimer responsive; responsive.start();
        while (responsive.elapsed() < 40) QCoreApplication::processEvents();
        QTest::keyRelease(window, Qt::Key(key));
        QTest::qWait(300);
        QCOMPARE(bible->cursorVerse(), start + direction);
    }
    void dimmingInstantOnCursorMove() {
        QFETCH(QString, mode);
        setLayout(mode); navigate("John 1:5");
        key(Qt::Key_F); QTRY_VERIFY(bible->focusMode());
        if (mode != "verse") QTRY_VERIFY(blockMapping() && blockMapping()->spans().size() == bible->verses().size());
        settle(); call("readerFocus");
        QTRY_VERIFY_WITH_TIMEOUT(!prop("pulseActive").toBool(), 3000);
        const auto full = 1 - prop("focusDimOpacity").toDouble();
        // j, k, Down, and Up re-dim at once: never a partial (fading) level,
        // no pulse, and no flash.
        const QList<QPair<Qt::Key, int>> moves = {{Qt::Key_J, 6}, {Qt::Key_Down, 7}, {Qt::Key_K, 6}, {Qt::Key_Up, 5}};
        for (const auto &move : moves) {
            const int previous = bible->cursorVerse();
            key(move.first); QCOMPARE(bible->cursorVerse(), move.second);
            QTest::qWait(1);
            for (int sample = 0; sample < 8; ++sample) {
                QCOMPARE(dim(move.second), 0.0);
                QVERIFY(std::abs(dim(previous) - full) < 1e-6);
                QVERIFY(!prop("pulseActive").toBool()); QVERIFY(!prop("flashActive").toBool());
                QTest::qWait(20);
            }
        }
        if (mode == "verse") QCOMPARE(item("chapterTitle")->opacity(), prop("focusDimOpacity").toDouble());
    }
    void pulseAfterJumps() {
        // Palette jump.
        key(Qt::Key_K, Qt::ControlModifier); QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("John 1:9"); key(Qt::Key_Return);
        QTRY_COMPARE(bible->selectionReference(), QString("John 1:9"));
        expectPulse(9, 9);
        // Bible search result.
        navigate("John 1:1");
        search("another alpha"); key(Qt::Key_Return);
        QTRY_VERIFY(!object("passagePalette")->property("visible").toBool());
        expectPulse(bible->selectedStart(), bible->selectedEnd());
        QVERIFY(bible->selectedStart() > 0);
        // n moves to the next chapter match.
        navigate("John 1:1");
        find("alpha"); key(Qt::Key_Return); QTRY_VERIFY(!prop("findPromptOpen").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!prop("pulseActive").toBool(), 3000);
        key(Qt::Key_N);
        expectPulse(bible->selectedStart(), bible->selectedEnd());
        // Counted g.
        navigate("John 1:1"); type("12"); key(Qt::Key_G);
        QTRY_COMPARE(bible->cursorVerse(), 12);
        expectPulse(12, 12);
        // Desk item open.
        navigate("John 1:20"); key(Qt::Key_D); navigate("John 1:1");
        key(Qt::Key_D, Qt::ShiftModifier); QTRY_VERIFY(item("deskList")->hasActiveFocus());
        key(Qt::Key_Return);
        QTRY_COMPARE(bible->cursorVerse(), 20);
        expectPulse(20, 20);
    }
    void pulseInBlockLayout() {
        setLayout("block");
        key(Qt::Key_K, Qt::ControlModifier); QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("John 1:8"); key(Qt::Key_Return);
        QTRY_COMPARE(bible->cursorVerse(), 8);
        expectPulse(8, 8);
    }
    void copyFlash_data() { layouts(); }
    void copyFlash() {
        QFETCH(QString, mode);
        setLayout(mode); navigate("John 1:4");
        key(Qt::Key_Y); expectFlash(4, 4);
        key(Qt::Key_Y, Qt::ShiftModifier); expectFlash(4, 4);
        key(Qt::Key_V, Qt::ShiftModifier); key(Qt::Key_J); key(Qt::Key_C, Qt::ControlModifier); expectFlash(4, 5);
        navigate("John 1:2");
        QTest::mouseClick(window, Qt::MiddleButton, {}, versePoint(6)); QCoreApplication::processEvents();
        expectFlash(6, 6);
        // Passage context menu copy.
        call("openPassageMenuAtCursor"); QTRY_VERIFY(menu()->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(menu(), "triggerId", Q_ARG(QVariant, "copy")));
        QTRY_VERIFY(!menu()->property("visible").toBool());
        expectFlash(bible->cursorVerse(), bible->cursorVerse());
        if (mode == "verse") QVERIFY(!item("copyFlash6")->isVisible());
    }
    void popupsFocusImmediatelyAndReopen() {
        auto ghost = [&](const char *name) { return object(name); };
        // Palette: open at once (no entrance fade), input focused.
        key(Qt::Key_K, Qt::ControlModifier);
        auto palette = object("passagePalette");
        QVERIFY(palette->property("visible").toBool());
        QVERIFY(item("referenceInput")->hasActiveFocus());
        QCOMPARE(palette->property("opacity").toDouble(), 1.0);
        type("John 1:3"); QCOMPARE(object("referenceInput")->property("text").toString(), QString("John 1:3"));
        QTest::qWait(60);
        // Closing: the palette is gone at once (not open, keys back); its ghost
        // fades and shrinks toward the centre without taking input.
        const auto paletteRect = item("referenceInput")->window() ? QRectF(palette->property("x").toDouble(), palette->property("y").toDouble(),
            palette->property("width").toDouble(), palette->property("height").toDouble()) : QRectF();
        key(Qt::Key_Escape);
        QVERIFY(!palette->property("visible").toBool()); QVERIFY(!prop("modalOpen").toBool()); QVERIFY(!prop("dialogOpen").toBool());
        QVERIFY(ghost("modalGhost")->property("running").toBool());
        const auto exit = value(ghost("modalGhost")->property("lastExit")).toMap();
        QVERIFY(qAbs(exit["width"].toDouble() - paletteRect.width()) <= 1 && qAbs(exit["height"].toDouble() - paletteRect.height()) <= 1);
        QVERIFY(qAbs(exit["originX"].toDouble() - (exit["x"].toDouble() + exit["width"].toDouble() / 2)) <= 1);
        const auto before = bible->cursorVerse(); key(Qt::Key_J); QCOMPARE(bible->cursorVerse(), before + 1);
        if (const auto directory = qEnvironmentVariable("OMAWORD_SHOTS"); !directory.isEmpty()) {
            QTest::qWait(30); QVERIFY(window->grabWindow().save(directory + "/palette-exit-ghost.png"));
        }
        QTRY_VERIFY_WITH_TIMEOUT(!ghost("modalGhost")->property("running").toBool(), 1000);
        // Reopen mid-exit: instantly, in every palette mode.
        for (auto mode : {Qt::Key_K, Qt::Key_Slash}) {
            key(mode, Qt::ControlModifier);
            QVERIFY(palette->property("visible").toBool()); QVERIFY(item("referenceInput")->hasActiveFocus());
            QCOMPARE(palette->property("opacity").toDouble(), 1.0);
            QTest::qWait(40);
            key(Qt::Key_Escape); QVERIFY(!palette->property("visible").toBool());
            QVERIFY(ghost("modalGhost")->property("running").toBool());
        }
        key(Qt::Key_K, Qt::ControlModifier); QVERIFY(palette->property("visible").toBool());
        QVERIFY(item("referenceInput")->hasActiveFocus()); QVERIFY(prop("modalOpen").toBool());
        key(Qt::Key_Escape); QVERIFY(!palette->property("visible").toBool());
        // Other modals: Help, Reading settings, Versions, About open at once and close at once.
        for (auto command : {"help", "settings", "switchVersion", "about"}) {
            QVERIFY(QMetaObject::invokeMethod(window, "runCommand", Q_ARG(QVariant, QString(command))));
            const auto name = QString(command) == "help" ? "helpDialog" : QString(command) == "settings" ? "settingsDialog"
                : QString(command) == "about" ? "aboutDialog" : "versionDialog";
            auto dialog = object(name); QVERIFY2(dialog, name);
            QVERIFY2(dialog->property("visible").toBool(), name);
            QCOMPARE(dialog->property("opacity").toDouble(), 1.0);
            QTRY_VERIFY(dialog->property("opened").toBool());
            QTest::qWait(40);
            key(Qt::Key_Escape); QVERIFY2(!dialog->property("visible").toBool(), name);
            QVERIFY2(ghost("modalGhost")->property("running").toBool(), name);
            QVERIFY(!prop("modalOpen").toBool());
            call("readerFocus");
        }
        QTRY_VERIFY_WITH_TIMEOUT(!ghost("modalGhost")->property("running").toBool(), 1000);
        // Desk: list focus while sliding/fading in (the full slide); reopen during close.
        navigate("John 1:2"); key(Qt::Key_D);
        auto desk = object("deskDrawer");
        key(Qt::Key_D, Qt::ShiftModifier);
        QVERIFY(desk->property("visible").toBool()); QVERIFY(!desk->property("opened").toBool());
        QVERIFY(item("deskList")->hasActiveFocus());
        QVERIFY(desk->property("position").toDouble() < 0.5);
        QTRY_VERIFY(desk->property("opened").toBool());
        // Exit is subtler than entrance: a short slide and a quicker fade.
        {
            QElapsedTimer timer; timer.start();
            key(Qt::Key_Escape);
            double lowest = 1;
            while (desk->property("visible").toBool() && timer.elapsed() < 1000) {
                lowest = qMin(lowest, desk->property("position").toDouble());
                QTest::qWait(5);
            }
            QVERIFY(!desk->property("visible").toBool());
            QVERIFY2(lowest >= 0.84, qPrintable(QString::number(lowest)));
            QVERIFY2(timer.elapsed() < 400, qPrintable(QString::number(timer.elapsed())));
        }
        key(Qt::Key_D, Qt::ShiftModifier); QVERIFY(desk->property("position").toDouble() < 0.5);
        QTRY_VERIFY(desk->property("opened").toBool());
        key(Qt::Key_Escape);
        QVERIFY(desk->property("visible").toBool()); QVERIFY(!desk->property("shown").toBool());
        QVERIFY(!prop("modalOpen").toBool());
        key(Qt::Key_D, Qt::ShiftModifier);
        QTRY_VERIFY(desk->property("opened").toBool());
        QVERIFY(desk->property("shown").toBool());
        QTRY_VERIFY(item("deskList")->hasActiveFocus());
        QTest::qWait(300);
        QVERIFY(desk->property("opened").toBool()); QVERIFY(item("deskList")->hasActiveFocus());
        key(Qt::Key_Escape); QTRY_VERIFY(!desk->property("visible").toBool());
        QTRY_VERIFY(item("readerSurface")->hasActiveFocus());
        // Annotations drawer: immediate list focus.
        key(Qt::Key_A, Qt::ShiftModifier);
        QVERIFY(object("savedDrawer")->property("visible").toBool());
        QVERIFY(item("collectionList")->hasActiveFocus());
        QTRY_VERIFY(object("savedDrawer")->property("opened").toBool());
        // While it slides out, reader shortcuts already work (no modal blocking).
        key(Qt::Key_Escape); QVERIFY(object("savedDrawer")->property("visible").toBool());
        QVERIFY(!object("savedDrawer")->property("modal").toBool());
        { const auto verse = bible->cursorVerse(); key(Qt::Key_J); QCOMPARE(bible->cursorVerse(), verse + 1); }
        QTRY_VERIFY(!object("savedDrawer")->property("visible").toBool());
        // Notes: the editor is focused while the drawer fades in.
        key(Qt::Key_W);
        QVERIFY(object("notesDrawer")->property("visible").toBool()); QVERIFY(prop("notesOpen").toBool());
        QVERIFY(item("noteEditor")->hasActiveFocus());
        QTRY_VERIFY(object("notesDrawer")->property("opened").toBool());
        // Side panels animate out as well as in.
        key(Qt::Key_Escape); QVERIFY(object("notesDrawer")->property("visible").toBool());
        QTRY_VERIFY(!object("notesDrawer")->property("visible").toBool());
        call("readerFocus"); key(Qt::Key_H, Qt::ShiftModifier);
        QVERIFY(object("historyDrawer")->property("visible").toBool()); QVERIFY(!object("historyDrawer")->property("opened").toBool());
        QTRY_VERIFY(object("historyDrawer")->property("opened").toBool());
        key(Qt::Key_Escape); QVERIFY(object("historyDrawer")->property("visible").toBool());
        QTRY_VERIFY(!object("historyDrawer")->property("visible").toBool());
        call("readerFocus"); key(Qt::Key_S, Qt::ShiftModifier);
        QTRY_VERIFY(object("relatedDrawer")->property("visible").toBool());
        QVERIFY(object("relatedDrawer")->property("position").toDouble() < 1.0);
        QTRY_VERIFY(object("relatedDrawer")->property("opened").toBool());
        QVERIFY(QMetaObject::invokeMethod(object("relatedDrawer"), "close"));
        QVERIFY(object("relatedDrawer")->property("visible").toBool());
        QTRY_VERIFY(!object("relatedDrawer")->property("visible").toBool());
        call("readerFocus");
        // Chapter find prompt fades; input is immediate; closing releases keys.
        key(Qt::Key_Slash);
        QVERIFY(item("chapterFindInput")->hasActiveFocus());
        QVERIFY(item("chapterFind")->opacity() < 1);
        QTRY_COMPARE(item("chapterFind")->opacity(), 1.0);
        key(Qt::Key_Escape); QVERIFY(!prop("findPromptOpen").toBool());
        QVERIFY(item("chapterFind")->isVisible()); QVERIFY(!item("chapterFind")->isEnabled());
        QTRY_VERIFY(!item("chapterFind")->isVisible());
        // Context menu: opens at once; closes at once, its ghost shrinking
        // toward the anchor point it opened at.
        call("openPassageMenuAtCursor");
        QVERIFY(menu()->property("visible").toBool());
        QCOMPARE(menu()->property("opacity").toDouble(), 1.0);
        QVERIFY(menu()->property("opened").toBool());
        QTRY_VERIFY(item("contextMenuContent")->hasActiveFocus());
        QTest::qWait(40);
        const auto anchor = menu()->property("origin").toPointF();
        key(Qt::Key_Escape); QVERIFY(!menu()->property("visible").toBool());
        QVERIFY(object("menuGhost")->property("running").toBool());
        {
            const auto exit = value(object("menuGhost")->property("lastExit")).toMap();
            QVERIFY(qAbs(exit["originX"].toDouble() - qBound(exit["x"].toDouble(), anchor.x(), exit["x"].toDouble() + exit["width"].toDouble())) <= 1);
            QVERIFY(qAbs(exit["originY"].toDouble() - qBound(exit["y"].toDouble(), anchor.y(), exit["y"].toDouble() + exit["height"].toDouble())) <= 1);
        }
        QTRY_VERIFY_WITH_TIMEOUT(!object("menuGhost")->property("running").toBool(), 1000);
        // Toasts fade in and out.
        QVERIFY(QMetaObject::invokeMethod(window, "notify", Q_ARG(QVariant, QString("Hello")), Q_ARG(QVariant, true)));
        auto toast = item("toastFrame");
        QVERIFY(toast->isVisible()); QVERIFY(toast->opacity() < 1.0);
        QTRY_COMPARE(toast->opacity(), 1.0);
        QVERIFY(QMetaObject::invokeMethod(object("toastTimer"), "stop"));
        QCoreApplication::processEvents();
        QVERIFY(toast->isVisible()); QVERIFY(!toast->property("shown").toBool());
        QTRY_VERIFY(toast->opacity() < 1.0);
        QTRY_VERIFY(!toast->isVisible());
    }
    // A right click elsewhere while a menu is open: the new menu appears at
    // once, as the old one's ghost starts leaving from where it was opened.
    void menuSwitchesWithoutAGap_data() { layouts(); }
    void menuSwitchesWithoutAGap() {
        QFETCH(QString, mode);
        setLayout(mode); navigate("John 1:3");
        QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, versePoint(3)); QCoreApplication::processEvents();
        QVERIFY(menu()->property("visible").toBool()); QCOMPARE(menu()->property("opacity").toDouble(), 1.0);
        const auto firstOrigin = menu()->property("origin").toPointF();
        const double firstX = menu()->property("x").toDouble(), firstY = menu()->property("y").toDouble();
        QTest::qWait(60);
        auto ghost = object("menuGhost"); const int exits = ghost->property("exits").toInt();
        // Mouse: right press on another verse (outside the open menu), one event.
        const QRectF open(firstX, firstY, menu()->property("width").toDouble(), menu()->property("height").toDouble());
        int other = 6;
        while (other < 30 && open.contains(versePoint(other))) ++other;
        QVERIFY(!open.contains(versePoint(other)));
        QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, versePoint(other)); QCoreApplication::processEvents();
        QVERIFY(menu()->property("visible").toBool()); QCOMPARE(menu()->property("opacity").toDouble(), 1.0);
        QCOMPARE(bible->selectionReference(), QString("John 1:%1").arg(other));
        // The old menu's exit started in the same event (under load it may already be over).
        QCOMPARE(ghost->property("exits").toInt(), exits + 1);
        const auto exit = value(ghost->property("lastExit")).toMap();
        QVERIFY(qAbs(exit["x"].toDouble() - firstX) <= 1 && qAbs(exit["y"].toDouble() - firstY) <= 1);
        QVERIFY(qAbs(exit["originX"].toDouble() - firstOrigin.x()) <= 1 && qAbs(exit["originY"].toDouble() - firstOrigin.y()) <= 1);
        QVERIFY(menu()->property("y").toDouble() != firstY || menu()->property("x").toDouble() != firstX);
        // Visual check: OMAWORD_SHOTS saves the switch mid-exit.
        if (const auto directory = qEnvironmentVariable("OMAWORD_SHOTS"); !directory.isEmpty() && mode == "verse") {
            QTest::qWait(30); QVERIFY(window->grabWindow().save(directory + "/menu-switch-ghost.png"));
        }
        // The ghost sits under the new menu and never takes input.
        QVERIFY(ghost->property("z").toDouble() < menu()->property("z").toDouble());
        QVERIFY(!qobject_cast<QQuickItem *>(ghost)->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(!ghost->property("running").toBool(), 1000);
        // Keyboard: the menu key on an open menu closes it; it reopens at once.
        key(Qt::Key_Escape); QVERIFY(!menu()->property("visible").toBool());
        key(Qt::Key_Menu); QVERIFY(menu()->property("visible").toBool());
        QTest::qWait(40);
        const int before = ghost->property("exits").toInt();
        key(Qt::Key_Escape); QVERIFY(!menu()->property("visible").toBool()); QCOMPARE(ghost->property("exits").toInt(), before + 1);
        key(Qt::Key_Menu); QVERIFY(menu()->property("visible").toBool());
        key(Qt::Key_Escape);
    }
    // Hover colors change at once (never animated), with animations on.
    void hoverColorsAreInstant() {
        QVERIFY(!sounds->reduceMotion());
        auto button = item("searchControl");
        auto background = button->property("background").value<QQuickItem *>();
        QCOMPARE(background->property("color").value<QColor>().alpha(), 0);
        QTest::mouseMove(window, button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint());
        QCoreApplication::processEvents();
        QVERIFY(background->property("color").value<QColor>().alpha() > 0);
        QTest::mouseMove(window, QPoint(window->width() / 2, 200)); QCoreApplication::processEvents();
        QCOMPARE(background->property("color").value<QColor>().alpha(), 0);
        call("openPassageMenuAtCursor"); QVERIFY(menu()->property("visible").toBool());
        QTest::qWait(30);
        auto row = item("contextMenuItem:bookmark");
        QTest::mouseMove(window, row->mapToScene(QPointF(row->width() / 2, row->height() / 2)).toPoint());
        QCoreApplication::processEvents();
        auto highlight = item("contextMenuHighlight" + QString::number(menu()->property("currentIndex").toInt()));
        QVERIFY(highlight->isVisible()); QCOMPARE(highlight->opacity(), 1.0);
        key(Qt::Key_Escape);
    }
    void reduceMotionCommand() {
        QCOMPARE(commandTitle("animations"), QString("Disable animations"));
        key(Qt::Key_Escape);
        key(Qt::Key_K, Qt::ControlModifier); QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        object("referenceInput")->setProperty("text", "");
        type("> disable animations"); key(Qt::Key_Return);
        QTRY_VERIFY(sounds->reduceMotion());
        QCOMPARE(object("toast")->property("text").toString(), QString("Animations off"));
        QTRY_VERIFY(!object("passagePalette")->property("visible").toBool());
        QCOMPARE(commandTitle("enable animations"), QString("Enable animations"));
        QVERIFY(SoundEffects(nullptr, session->filePath("sound.ini"), false).reduceMotion());
        QVERIFY(prop("reduceMotion").toBool()); QCOMPARE(prop("fadeMs").toInt(), 0);
        QCOMPARE(item("chapterFind")->property("fadeDuration").toInt(), 0);
        call("readerFocus");
        // Bound to alt m, which toggles it back.
        key(Qt::Key_M, Qt::AltModifier); QVERIFY(!prop("reduceMotion").toBool());
        key(Qt::Key_M, Qt::AltModifier); QVERIFY(prop("reduceMotion").toBool());
        QVariantList actions = prop("actions").toList(); bool found = false;
        for (const auto &a : actions) if (a.toMap()["id"] == "reduceMotion") { found = true; QVERIFY(!a.toMap().contains("keys")); QCOMPARE(a.toMap()["sequences"].toStringList(), QStringList{"Alt+M"}); }
        QVERIFY(found);
        // Jumps and copies produce no pulse/flash objects.
        key(Qt::Key_K, Qt::ControlModifier); QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        QCOMPARE(object("passagePalette")->property("opacity").toDouble(), 1.0);
        type("John 1:9"); key(Qt::Key_Return);
        QTRY_COMPARE(bible->cursorVerse(), 9);
        for (int i = 0; i < 6; ++i) { QTest::qWait(20); QVERIFY(!prop("pulseActive").toBool()); QVERIFY(!item("jumpPulse9")->isVisible()); }
        key(Qt::Key_Y);
        for (int i = 0; i < 6; ++i) { QTest::qWait(20); QVERIFY(!prop("flashActive").toBool()); QVERIFY(!item("copyFlash9")->isVisible()); }
        // Modals and toasts appear at full strength at once.
        call("readerFocus"); QVERIFY(QMetaObject::invokeMethod(window, "runCommand", Q_ARG(QVariant, QString("help"))));
        QTRY_VERIFY(object("helpDialog")->property("opened").toBool());
        QCOMPARE(object("helpDialog")->property("opacity").toDouble(), 1.0);
        key(Qt::Key_Escape); QVERIFY(!object("helpDialog")->property("visible").toBool());
        QVERIFY(!object("modalGhost")->property("running").toBool()); call("readerFocus");
        // Menus: no ghost either.
        call("openPassageMenuAtCursor"); QVERIFY(menu()->property("visible").toBool());
        key(Qt::Key_Escape); QVERIFY(!menu()->property("visible").toBool());
        QVERIFY(!object("menuGhost")->property("running").toBool()); call("readerFocus");
        QVERIFY(QMetaObject::invokeMethod(window, "notify", Q_ARG(QVariant, QString("Hello")), Q_ARG(QVariant, true)));
        QCoreApplication::processEvents(); QTest::qWait(20);
        QCOMPARE(item("toastFrame")->opacity(), 1.0);
        // Drawers open without transitions; focus mode dims instantly.
        key(Qt::Key_D, Qt::ShiftModifier); QVERIFY(object("deskDrawer")->property("visible").toBool());
        QTRY_VERIFY(object("deskDrawer")->property("opened").toBool());
        QCOMPARE(object("deskDrawer")->property("opacity").toDouble(), 1.0);
        key(Qt::Key_Escape); QTRY_VERIFY_WITH_TIMEOUT(!object("deskDrawer")->property("visible").toBool(), 60);
        call("readerFocus");
        key(Qt::Key_F); QTRY_VERIFY(bible->focusMode());
        QTest::qWait(30);
        QCOMPARE(dim(9), 0.0); QVERIFY(std::abs(dim(8) - (1 - prop("focusDimOpacity").toDouble())) < 1e-6);
        key(Qt::Key_Escape); QTRY_VERIFY(!bible->focusMode());
        // Enable animations flips back and persists.
        QCOMPARE(commandTitle("animations"), QString("Enable animations"));
        key(Qt::Key_Escape); call("readerFocus");
        QVERIFY(QMetaObject::invokeMethod(window, "runCommand", Q_ARG(QVariant, "reduceMotion")));
        QVERIFY(!sounds->reduceMotion());
        QCOMPARE(object("toast")->property("text").toString(), QString("Animations on"));
        QVERIFY(!SoundEffects(nullptr, session->filePath("sound.ini"), false).reduceMotion());
        QCOMPARE(commandTitle("animations"), QString("Disable animations"));
    }
};

int main(int argc, char **argv) {
    QQuickStyle::setStyle("Basic"); QApplication app(argc, argv);
    MotionTest test; return QTest::qExec(&test, argc, argv);
}
#include "tst_motion.moc"
