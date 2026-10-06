#include "testenvironment.h"
#include <QtTest/QTest>
#include <QApplication>
#include <QDir>
#include <QImage>
#include <QJSValue>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTextDocument>
#include <archive.h>
#include <archive_entry.h>
#include "biblebackend.h"
#include "desktop.h"
#include "soundeffects.h"
#include "versioncatalog.h"

// Packaged Main, real backend transactions, two miniature original editions.
class RangesUiTest : public QObject
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
    QObject *editor = nullptr;
    QStringList warnings;
    QString net, kjv;

    static bool edition(const QString &path, const QString &id) {
        auto zip = archive_write_new(); archive_write_set_format_zip(zip);
        if (archive_write_open_filename(zip, QFile::encodeName(path).constData()) != ARCHIVE_OK) { archive_write_free(zip); return false; }
        QByteArray data = "\\id JHN Original range UI fixture\n\\h John\n\\toc1 John\n\\toc2 John\n";
        for (int chapter = 3; chapter <= 4; ++chapter) {
            data += QString("\\c %1\n\\p\n").arg(chapter).toUtf8();
            for (int verse = 1; verse <= 12; ++verse)
                data += QString("\\v %1 Original range fixture %2:%1 with enough text for reading and selection.\n").arg(verse).arg(chapter).toUtf8();
        }
        auto entry = archive_entry_new();
        const auto name = ("01-JHN" + id + ".usfm").toUtf8();
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
    // Hint rows are rich text (KeyNotation.hint); compare their plain reading.
    static QString plainHint(const QString &rich) {
        QTextDocument document; document.setHtml(rich);
        return document.toPlainText().replace(QChar(0x00a0), QChar(' '));
    }
    QQuickItem *item(const QString &name) const { return qobject_cast<QQuickItem *>(object(name)); }
    QVariant prop(const char *name) const { return value(editor->property(name)); }
    void key(Qt::Key key, Qt::KeyboardModifiers mods = Qt::NoModifier) { QTest::keyClick(window, key, mods); QCoreApplication::processEvents(); }
    void call(QObject *target, const char *method) { QVERIFY(QMetaObject::invokeMethod(target, method)); QCoreApplication::processEvents(); }
    void click(const QString &name) {
        auto target = item(name); QVERIFY2(target, qPrintable(name));
        QVERIFY2(target->isVisible() && target->isEnabled(), qPrintable(name));
        const auto point = target->mapToScene(QPointF(target->width()/2, target->height()/2)).toPoint();
        QVERIFY2(QRect(QPoint(), window->size()).contains(point), qPrintable(name));
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point); QCoreApplication::processEvents();
    }
    void install(const QString &path) {
        bible->importUsfm(QUrl::fromLocalFile(path)); QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(), 30000);
        QVERIFY2(bible->ready() && bible->error().isEmpty(), qPrintable(bible->error()));
    }
    void navigate(const QString &ref) {
        QVERIFY(bible->navigate(ref)); call(window, "readerFocus");
        QTRY_VERIFY(!window->property("restoringPosition").toBool());
    }
    void open(const QVariant &scope, const QString &kind = {}) {
        QVERIFY(QMetaObject::invokeMethod(window, "openRangeEditor", Q_ARG(QVariant, scope), Q_ARG(QVariant, kind)));
        QTRY_VERIFY(prop("visibleModal").toBool() || prop("active").toBool());
        QTest::qWait(60);
    }
    void begin() {
        QTest::qWait(60); // Polish the component form after replacing the range list.
        if (!prop("active").toBool()) click("rangeContinue");
        QTRY_VERIFY(prop("active").toBool()); QTRY_VERIFY(!prop("visibleModal").toBool());
        QTRY_VERIFY(window->property("readerKeys").toBool()); call(window, "readerFocus");
    }
    void sql(const QString &statement) {
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "range-ui-sql");
            db.setDatabaseName(bible->dataDirectory() + "/personal.sqlite"); QVERIFY(db.open());
            QSqlQuery q(db); QVERIFY2(q.exec(statement), qPrintable(statement));
        }
        QSqlDatabase::removeDatabase("range-ui-sql");
    }
    void capture(const QString &stage) {
        const auto directory = qEnvironmentVariable("OMAWORD_RANGE_CHOOSER_SCREENSHOTS");
        if (directory.isEmpty()) return;
        QVERIFY(QDir(directory).exists());
        const auto image = window->grabWindow();
        QVERIFY(!image.isNull());
        QVERIFY(image.save(QDir(directory).filePath(QString("rangechooser-%1-%2.png")
            .arg(QString::fromLatin1(QTest::currentDataTag()), stage))));
    }
    static qreal top(QQuickItem *target) { return target->mapToScene(QPointF()).y(); }
    static qreal bottom(QQuickItem *target) { return target->mapToScene(QPointF(0, target->height())).y(); }
    void chooserBounds(bool components) {
        auto title = item("rangeChooserTitle"), reference = item("rangeChooserReference");
        auto description = item("rangeChooserDescription"), scroll = item("rangeChoiceScroll");
        auto actions = item("rangeChooserActions"), rule = item("rangeChooserRule"), footer = item("rangeChooserKeys");
        QVERIFY(title && reference && description && scroll && actions && rule && footer);
        const auto gap = 12 * prop("uiScale").toDouble();
        QVERIFY(top(title) >= 16 + 24 * prop("uiScale").toDouble() - 1);
        QVERIFY(qAbs(top(reference) - bottom(title) - gap) <= 1);
        QCOMPARE(description->isVisible(), !components);
        if (!components) {
            QCOMPARE(description->property("text").toString(), QString("Several annotations overlap this passage and may have different boundaries. Choose the exact range you want to change."));
            QCOMPARE(description->property("textFormat").toInt(), 0);
            QVERIFY(qAbs(top(description) - bottom(reference) - gap) <= 1);
        }
        QVERIFY(qAbs(top(scroll) - bottom(components ? reference : description) - gap) <= 1);
        QVERIFY2(scroll->height() > 35, qPrintable(QString("list height %1").arg(scroll->height())));
        QVERIFY(qAbs(top(actions) - bottom(scroll) - gap) <= 1);
        QVERIFY(qAbs(top(rule) - bottom(actions) - gap) <= 1);
        QVERIFY(qAbs(top(footer) - bottom(rule) - gap) <= 1);
        QVERIFY(bottom(footer) <= window->height() - 16 - 24 * prop("uiScale").toDouble() + 1);
        QCOMPARE(footer->property("color"), desktop->property("muted"));
        QVERIFY(actions->width() <= scroll->width());
        for (const auto &name : {"rangeChooserCancel", "rangeChooseAnother", "rangeContinue"}) {
            auto button = item(name); QVERIFY(button);
            if (!button->isVisible()) continue;
            QVERIFY(button->height() >= button->implicitHeight());
            QVERIFY(button->x() + button->width() <= actions->width() + 1);
        }
    }
    void inlineHint(const QString &name, const QString &text) {
        auto row = item(name), hint = item(name + "Hint"); QVERIFY(row && hint);
        QVERIFY(hint->isVisible()); QCOMPARE(hint->property("text").toString(), text);
        auto label = row->property("contentItem").value<QQuickItem *>(); QVERIFY(label);
        const auto hintLeft = hint->mapToItem(row, QPointF()).x();
        const auto labelRight = label->mapToItem(row, QPointF(label->width() - label->property("rightPadding").toReal(), 0)).x();
        QVERIFY(hintLeft >= labelRight + 11 * prop("uiScale").toDouble());
        QVERIFY(qAbs(hint->mapToItem(row, QPointF(hint->width(), 0)).x()
            - (row->width() - row->property("rightPadding").toReal())) <= 1);
        QVERIFY(row->height() >= label->implicitHeight() + row->property("topPadding").toReal() + row->property("bottomPadding").toReal());
    }
private slots:
    void initTestCase() {
        net = fixtures.filePath("engnet.zip"); kjv = fixtures.filePath("eng-kjv.zip");
        QVERIFY(edition(net, "engnet")); QVERIFY(edition(kjv, "eng-kjv"));
    }
    void init() {
        session = std::make_unique<QTemporaryDir>(); qputenv("XDG_DATA_HOME", session->path().toUtf8());
        bible = std::make_unique<BibleBackend>(); install(net); bible->setReadingMode("verse");
        desktop = std::make_unique<Desktop>(); sounds = std::make_unique<SoundEffects>(nullptr, session->filePath("sound.ini"), false); catalog = std::make_unique<VersionCatalog>();
        engine = std::make_unique<QQmlApplicationEngine>(); warnings.clear();
        connect(engine.get(), &QQmlEngine::warnings, this, [this](const QList<QQmlError> &errors) { for (const auto &e : errors) warnings << e.toString(); });
        engine->rootContext()->setContextProperty("bible", bible.get());
        engine->rootContext()->setContextProperty("desktop", desktop.get());
        engine->rootContext()->setContextProperty("sounds", sounds.get());
        engine->rootContext()->setContextProperty("catalog", catalog.get());
        engine->load(QUrl("qrc:/qml/Main.qml")); QVERIFY2(!engine->rootObjects().isEmpty(), qPrintable(warnings.join('\n')));
        window = qobject_cast<QQuickWindow *>(engine->rootObjects().first()); QVERIFY(window);
        editor = object("rangeEditor"); QVERIFY(editor);
        window->requestActivate(); QVERIFY(QTest::qWaitForWindowExposed(window)); QTRY_VERIFY(window->isActive());
        navigate("John 3:2-4");
    }
    void cleanup() {
        if (editor && prop("sessionOpen").toBool()) call(editor, "cancel");
        engine.reset(); window = nullptr; editor = nullptr; catalog.reset(); sounds.reset(); desktop.reset(); bible.reset(); session.reset();
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
    }
    void componentSubsets_data() {
        QTest::addColumn<QString>("kind");
        for (const auto &kind : {"", "bookmark", "highlight", "note", "tag"}) QTest::newRow(*kind ? kind : "all") << QString(kind);
    }
    void componentSubsets() {
        QFETCH(QString, kind);
        QVERIFY(bible->toggleBookmark()); QVERIFY(bible->toggleHighlight()); bible->setNoteText("Source note");
        const auto source = bible->tagScope();
        const auto one = bible->createAndAssignTag(source, "One"), two = bible->createAndAssignTag(source, "Two");
        QVERIFY(!one.isEmpty() && !two.isEmpty());
        open(source, kind);
        QVERIFY(prop("visibleModal").toBool()); QVERIFY(!prop("active").toBool());
        QCOMPARE(prop("presentKinds").toList().size(), 4);
        if (kind == "tag") click("rangeTag:" + two); // Move a selected tag subset.
        begin();
        QVERIFY(!window->property("verseMode").toBool());
        QVERIFY(item("rangeEditStatus")->isVisible()); QVERIFY(!item("versionControl")->isEnabled());
        const auto unchanged = bible->annotationSnapshot(source);
        QSignalSpy commits(bible.get(), &BibleBackend::tagsChanged);
        key(Qt::Key_2); key(Qt::Key_J); QCOMPARE(bible->selectedEnd(), 6); QCOMPARE(bible->selectedStart(), 2);
        const auto destination = bible->tagScope();
        key(Qt::Key_M); key(Qt::Key_N); key(Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(bible->annotationSnapshot(source), unchanged); QVERIFY(!object("notesDrawer")->property("visible").toBool());
        QSignalSpy newWindow(window, SIGNAL(newWindowRequested())); key(Qt::Key_N, Qt::ControlModifier); QCOMPARE(newWindow.count(), 1);
        commits.clear();
        key(Qt::Key_Return); QTRY_VERIFY(!prop("sessionOpen").toBool());
        QVERIFY2(bible->error().isEmpty(), qPrintable(bible->error())); QTRY_COMPARE(commits.count(), 1);
        const auto old = bible->annotationSnapshot(source), moved = bible->annotationSnapshot(destination);
        for (const auto &flag : {"bookmark", "highlight"}) {
            const bool chosen = kind.isEmpty() || kind == flag;
            QCOMPARE(moved.value(flag).toBool(), chosen); QCOMPARE(old.value(flag).toBool(), !chosen);
        }
        QCOMPARE(moved.value("note").toString(), kind.isEmpty() || kind == "note" ? QString("Source note") : QString());
        QCOMPARE(bible->tagsForScope(source).size(), kind.isEmpty() ? 0 : kind == "tag" ? 1 : 2);
        QCOMPARE(bible->tagsForScope(destination).size(), kind.isEmpty() ? 2 : kind == "tag" ? 1 : 0);
        QVERIFY(!window->property("verseMode").toBool()); key(Qt::Key_J); QCOMPARE(bible->selectedStart(), bible->selectedEnd());
    }
    void overlapChooserCancelAndNoop() {
        QVERIFY(bible->toggleBookmark()); const auto first = bible->tagScope();
        navigate("John 3:3-5"); QVERIFY(bible->toggleHighlight()); const auto second = bible->tagScope();
        navigate("John 3:3"); const auto original = bible->tagScope();
        const auto collection = bible->collection(); QSignalSpy commits(bible.get(), &BibleBackend::sharedCommitted);
        key(Qt::Key_E, Qt::ShiftModifier); QTRY_VERIFY(prop("visibleModal").toBool()); QTest::qWait(60);
        QCOMPARE(prop("ranges").toList().size(), 2); QVERIFY(prop("source").isNull());
        QCOMPARE(plainHint(item("rangeChooserKeys")->property("text").toString()), QString::fromUtf8("↑ ↓ choose    enter select    esc cancel"));
        click("rangeChoice:" + second.value("reference").toString());
        QVERIFY(prop("active").toBool()); QVERIFY(!prop("visibleModal").toBool()); begin();
        QCOMPARE(bible->tagScope(), second); key(Qt::Key_J); key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("sessionOpen").toBool()); QCOMPARE(bible->tagScope(), original); QCOMPARE(bible->collection(), collection); QCOMPARE(commits.count(), 0);
        open(first); begin(); key(Qt::Key_Return); QTRY_VERIFY(!prop("sessionOpen").toBool()); QCOMPARE(commits.count(), 0);
    }
    void swapAcrossChapterAndRestoreDirection() {
        navigate("John 3:11-4:2"); QVERIFY(bible->toggleBookmark()); const auto source = bible->tagScope();
        navigate("John 3:6-8"); bible->selectVerses(8, 6); window->setProperty("verseMode", true);
        const auto original = bible->tagScope();
        bible->setReadingPosition(0.25);
        open(source); begin(); QCOMPARE(bible->chapter(), 4); QCOMPARE(bible->cursorVerse(), 2);
        key(Qt::Key_O); QCOMPARE(bible->chapter(), 3); QCOMPARE(bible->cursorVerse(), 11); QCOMPARE(prop("endpoint").toString(), QString("start"));
        key(Qt::Key_K); QCOMPARE(bible->tagScope().value("start").toInt(), 10); QCOMPARE(bible->tagScope().value("endChapter").toInt(), 4);
        key(Qt::Key_O); key(Qt::Key_K, Qt::ShiftModifier); QCOMPARE(bible->cursorVerse(), 1);
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("sessionOpen").toBool());
        QCOMPARE(bible->tagScope(), original); QCOMPARE(bible->cursorVerse(), 6); QVERIFY(window->property("verseMode").toBool());
        QVERIFY(bible->annotationSnapshot(source).value("bookmark").toBool());
    }
    void noteCollision_data() {
        QTest::addColumn<bool>("combine"); QTest::newRow("cancel") << false; QTest::newRow("combine") << true;
    }
    void noteCollision() {
        QFETCH(bool, combine);
        bible->setNoteText("Source <b>literal</b>"); const auto source = bible->tagScope();
        navigate("John 3:2-5"); bible->setNoteText("Destination note"); const auto destination = bible->tagScope();
        open(source, "note"); begin(); key(Qt::Key_J); key(Qt::Key_Return);
        QTRY_VERIFY(object("rangeNoteMerge")->property("visible").toBool());
        QCOMPARE(item("rangeSourceNote")->property("text").toString(), QString("Source <b>literal</b>"));
        QCOMPARE(item("rangeDestinationNote")->property("text").toString(), QString("Destination note"));
        const auto selected = bible->tagScope();
        key(Qt::Key_J); key(Qt::Key_K); QCOMPARE(bible->tagScope(), selected);
        QVERIFY(object("rangeNoteMerge")->property("visible").toBool());
        click(combine ? "rangeMergeCombine" : "rangeMergeCancel"); QTRY_VERIFY(!prop("sessionOpen").toBool());
        QCOMPARE(bible->annotationSnapshot(destination).value("note").toString(), combine ? QString("Destination note\n\nSource <b>literal</b>") : QString("Destination note"));
        QCOMPARE(bible->annotationSnapshot(source).value("note").toString(), combine ? QString() : QString("Source <b>literal</b>"));
    }
    void staleSourceAndFailedSaveStayActive() {
        bible->setNoteText("Initial"); const auto source = bible->tagScope();
        open(source); begin(); key(Qt::Key_J);
        BibleBackend peer; QVERIFY(peer.openTaggedPassage(source)); peer.setNoteText("Peer revision");
        key(Qt::Key_Return); QVERIFY(prop("active").toBool()); QVERIFY(!prop("message").toString().isEmpty());
        QCOMPARE(bible->annotationSnapshot(source).value("note").toString(), QString("Peer revision"));
        key(Qt::Key_Escape); open(source); begin(); key(Qt::Key_J);
        const auto destination = bible->tagScope();
        sql("CREATE TRIGGER fail_range BEFORE INSERT ON annotations BEGIN SELECT RAISE(ABORT,'range fixture failure'); END");
        key(Qt::Key_Return); QVERIFY(prop("active").toBool()); QCOMPARE(bible->tagScope(), destination);
        QVERIFY(prop("message").toString().contains("range fixture failure"));
        QCOMPARE(bible->annotationSnapshot(source).value("note").toString(), QString("Peer revision"));
        sql("DROP TRIGGER fail_range"); key(Qt::Key_Return); QTRY_VERIFY(!prop("active").toBool());
    }
    void staleMergeDoesNotOverwritePeer() {
        bible->setNoteText("Source"); const auto source = bible->tagScope();
        navigate("John 3:2-5"); bible->setNoteText("Destination"); const auto destination = bible->tagScope();
        open(source); begin(); key(Qt::Key_J); key(Qt::Key_Return); QTRY_VERIFY(prop("visibleModal").toBool());
        BibleBackend peer; QVERIFY(peer.openTaggedPassage(destination)); peer.setNoteText("New destination");
        click("rangeMergeCombine"); QTRY_VERIFY(!prop("visibleModal").toBool()); QVERIFY(prop("active").toBool());
        QCOMPARE(bible->annotationSnapshot(destination).value("note").toString(), QString("New destination"));
        QCOMPARE(bible->annotationSnapshot(source).value("note").toString(), QString("Source"));
    }
    void cancelAfterVersionChangeEndsSafely() {
        QVERIFY(bible->toggleBookmark()); const auto source = bible->tagScope();
        install(kjv); navigate("John 4:4-6"); bible->selectVerses(6, 4); const auto original = bible->tagScope();
        open(source, "bookmark"); begin(); QCOMPARE(bible->versionId(), QString("engnet"));
        key(Qt::Key_J); key(Qt::Key_Escape); QTRY_VERIFY(!prop("sessionOpen").toBool());
        QCOMPARE(bible->versionId(), QString("engnet"));
        QCOMPARE(prop("original").toMap().value("scope").toMap(), original);
        QVERIFY(item("toast")->property("text").toString().contains("no longer active"));
        QVERIFY(bible->annotationSnapshot(source).value("bookmark").toBool());
    }
    void notesEndCursorAndTagsTabNavigation() {
        bible->setNoteText("Existing note"); key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        QCOMPARE(item("noteEditor")->property("cursorPosition").toInt(), 13);
        key(Qt::Key_Left); key(Qt::Key_Left); const auto cursor = item("noteEditor")->property("cursorPosition").toInt();
        QTest::keyClick(window, 'z'); QCOMPARE(item("noteEditor")->property("cursorPosition").toInt(), cursor + 1);
        call(window, "syncNote"); QCOMPARE(item("noteEditor")->property("cursorPosition").toInt(), cursor + 1);
        key(Qt::Key_Escape); QTRY_VERIFY(!object("notesDrawer")->property("visible").toBool());
        const auto source = bible->tagScope(); QVERIFY(!bible->createAndAssignTag(source, "Tab test").isEmpty());
        key(Qt::Key_A, Qt::ShiftModifier); QTRY_VERIFY(object("savedDrawer")->property("opened").toBool());
        key(Qt::Key_H); QCOMPARE(window->property("collectionTab").toInt(), 3);
        QTRY_VERIFY(item("collectionList")->hasActiveFocus());
        key(Qt::Key_J); QCOMPARE(object("collectionList")->property("currentIndex").toInt(), 0);
        key(Qt::Key_Left); QCOMPARE(window->property("collectionTab").toInt(), 2);
        key(Qt::Key_Right); QCOMPARE(window->property("collectionTab").toInt(), 3);
        key(Qt::Key_L); QCOMPARE(window->property("collectionTab").toInt(), 0);
        key(Qt::Key_H); QTest::qWait(80);
        QVERIFY(!item("summaryEditRange")); key(Qt::Key_E, Qt::ShiftModifier); begin();
        QVERIFY(prop("components").toMap().value("tags").toList().size() == 1); QVERIFY(!prop("components").toMap().value("note").toBool());
        key(Qt::Key_Escape); key(Qt::Key_T); QTRY_VERIFY(item("tagSearch")->hasActiveFocus());
        QTest::keyClick(window, 'h'); QTest::keyClick(window, 'l'); QCOMPARE(item("tagSearch")->property("text").toString(), QString("hl"));
    }
    void focusedAnnotationRow() {
        QVERIFY(bible->toggleBookmark()); const auto source = bible->tagScope();
        navigate("John 3:8"); QVERIFY(bible->toggleBookmark());
        key(Qt::Key_A, Qt::ShiftModifier); QTRY_VERIFY(object("savedDrawer")->property("opened").toBool());
        // Bookmarks are listed in source order; the first row is the John 3:2 source.
        QTRY_VERIFY(item("collectionList")->hasActiveFocus());
        QCOMPARE(object("collectionList")->property("currentIndex").toInt(), 0);
        key(Qt::Key_E, Qt::ShiftModifier); QTRY_VERIFY(prop("active").toBool());
        QVERIFY(!prop("visibleModal").toBool());
        QCOMPARE(prop("source").toMap().value("scope").toMap().value("start").toInt(), 2);
        begin(); key(Qt::Key_Escape); QCOMPARE(bible->cursorVerse(), 8);
    }
    void chooserEmptyAndBulkMotion() {
        QVERIFY(bible->toggleHighlight()); QVERIFY(bible->toggleBookmark()); const auto scope = bible->tagScope();
        open(scope, "highlight"); click("rangeComponent:highlight"); QVERIFY(!item("rangeContinue")->isEnabled());
        key(Qt::Key_J); QCOMPARE(bible->tagScope(), scope);
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("sessionOpen").toBool());
        open(scope, "highlight"); begin();
        key(Qt::Key_D, Qt::ControlModifier); QCOMPARE(bible->cursorVerse(), 9); QCOMPARE(bible->tagScope().value("start").toInt(), 2);
        key(Qt::Key_PageDown); QCOMPARE(bible->chapter(), 3); QCOMPARE(bible->cursorVerse(), 12);
        key(Qt::Key_PageDown); QCOMPARE(bible->chapter(), 4); QCOMPARE(bible->cursorVerse(), 1);
        key(Qt::Key_PageUp); QCOMPARE(bible->chapter(), 3); QCOMPARE(bible->cursorVerse(), 12);
        key(Qt::Key_O); key(Qt::Key_Down); QCOMPARE(bible->tagScope().value("start").toInt(), 3);
        key(Qt::Key_Escape); QCOMPARE(bible->tagScope(), scope);
        bible->setNoteText("Everything");
        const auto tag = bible->createAndAssignTag(scope, "<b>Literal tag</b>"); QVERIFY(!tag.isEmpty());
        open(scope, "highlight");
        QVERIFY(!prop("components").toMap().value("bookmark").toBool());
        click("rangeSelectAll");
        const auto all = prop("components").toMap();
        QVERIFY(all.value("bookmark").toBool() && all.value("highlight").toBool() && all.value("note").toBool());
        QCOMPARE(all.value("tags").toList(), QVariantList{tag});
        auto label = item("rangeTag:" + tag)->property("contentItem").value<QQuickItem *>(); QVERIFY(label);
        QCOMPARE(label->property("textFormat").toInt(), 0);
        QCOMPARE(label->property("text").toString(), QString("Tag · <b>Literal tag</b>"));
        QCOMPARE(label->property("color"), desktop->property("foreground"));
    }
    void cancelRestoresUnselectedChapterAndPosition() {
        QVERIFY(bible->toggleBookmark()); const auto source = bible->tagScope();
        navigate("John 4");
        QVERIFY(!window->property("hasSelection").toBool());
        auto reading = item("reading"); QVERIFY(reading);
        const double position = 0.35;
        bible->setReadingPosition(position); QVERIFY(QMetaObject::invokeMethod(window, "restorePosition", Q_ARG(QVariant, false)));
        QTRY_VERIFY(!window->property("restoringPosition").toBool());
        open(source); begin(); key(Qt::Key_J); key(Qt::Key_Escape);
        QTRY_VERIFY(!window->property("restoringPosition").toBool());
        QCOMPARE(bible->chapter(), 4); QVERIFY(!window->property("hasSelection").toBool());
        QVERIFY(qAbs(bible->readingPosition() - position) < 0.01);
    }
    void cancelIgnoresUnrelatedPeerDraftVeto() {
        QVERIFY(bible->toggleBookmark()); const auto source = bible->tagScope();
        navigate("John 4:3-5"); bible->selectVerses(5, 3); const auto initial = bible->tagScope();
        window->setProperty("verseMode", true);
        open(source); begin(); key(Qt::Key_J);
        BibleBackend peer; QVERIFY(peer.navigate("John 3:9")); peer.setNoteText("Unrelated saved note");
        const auto annotations = bible->collection();
        int flushes = 0;
        peer.setFlushCallback([&] { ++flushes; return false; });
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("sessionOpen").toBool());
        QCOMPARE(flushes, 0); QCOMPARE(bible->tagScope(), initial); QCOMPARE(bible->cursorVerse(), 3);
        QVERIFY(window->property("verseMode").toBool()); QCOMPARE(bible->collection(), annotations);
        QCOMPARE(peer.noteText(), QString("Unrelated saved note"));
        peer.setFlushCallback({});
        // The chapter-only restore path also bypasses the peer flush callback.
        navigate("John 4"); open(source); begin();
        peer.setFlushCallback([&] { ++flushes; return false; });
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("sessionOpen").toBool());
        QCOMPARE(flushes, 0); QCOMPARE(bible->chapter(), 4); QCOMPARE(bible->selectedStart(), 0);
        peer.setFlushCallback({});
    }
    void cancelReadingFailureKeepsSnapshotForRetry() {
        QVERIFY(bible->toggleBookmark()); const auto source = bible->tagScope();
        navigate("John 4:2-6"); bible->selectVerses(6, 2); const auto initial = bible->tagScope();
        open(source); begin(); key(Qt::Key_J); const auto destination = bible->tagScope();
        const auto original = prop("original"); const auto annotations = bible->collection();
        sql("CREATE TRIGGER fail_reading BEFORE UPDATE ON reading BEGIN SELECT RAISE(ABORT,'reading fixture failure'); END");
        key(Qt::Key_Escape); QVERIFY(prop("sessionOpen").toBool());
        QCOMPARE(prop("original"), original); QCOMPARE(bible->tagScope(), destination);
        QVERIFY(prop("message").toString().contains("esc to retry"));
        QVERIFY(!window->close()); QVERIFY(prop("sessionOpen").toBool());
        sql("DROP TRIGGER fail_reading"); key(Qt::Key_Escape); QTRY_VERIFY(!prop("sessionOpen").toBool());
        QCOMPARE(bible->tagScope(), initial); QCOMPARE(bible->cursorVerse(), 2); QCOMPARE(bible->collection(), annotations);
        auto wrongVersion = initial; wrongVersion["versionId"] = "missing";
        QVERIFY(!bible->restoreSelection(wrongVersion, false, 0)); QCOMPARE(bible->tagScope(), initial);
        QVERIFY(!bible->restoreSelection(initial, false, 2)); QCOMPARE(bible->tagScope(), initial);
    }
    void peerRemovesLastVersionEndsSession() {
        QVERIFY(bible->toggleBookmark()); const auto source = bible->tagScope();
        open(source); begin(); key(Qt::Key_J);
        BibleBackend peer;
        QVERIFY(peer.removeVersion(bible->versionId(), false));
        QTRY_VERIFY(!bible->ready()); QTRY_VERIFY(!prop("sessionOpen").toBool());
        QVERIFY(!prop("visibleModal").toBool());
        QVERIFY(item("toast")->property("text").toString().contains("source version was removed"));
        QVERIFY(QMetaObject::invokeMethod(window, "runCommand", Q_ARG(QVariant, "import")));
        QTRY_VERIFY(object("importGuide")->property("visible").toBool());
        key(Qt::Key_Escape);
    }
    void singletonEndpointTracksMotionAndSwap() {
        navigate("John 3:12"); QVERIFY(bible->toggleBookmark());
        open(bible->tagScope()); begin(); QCOMPARE(prop("endpoint").toString(), QString("end"));
        key(Qt::Key_K); QCOMPARE(prop("endpoint").toString(), QString("start"));
        key(Qt::Key_J); QCOMPARE(prop("endpoint").toString(), QString("end"));
        key(Qt::Key_O); QCOMPARE(prop("endpoint").toString(), QString("start"));
        key(Qt::Key_O); QCOMPARE(prop("endpoint").toString(), QString("end"));
        key(Qt::Key_J); QCOMPARE(bible->chapter(), 4); QCOMPARE(prop("endpoint").toString(), QString("end"));
        key(Qt::Key_O); QCOMPARE(bible->chapter(), 3); QCOMPARE(prop("endpoint").toString(), QString("start"));
        key(Qt::Key_J); QCOMPARE(bible->chapter(), 4); QCOMPARE(prop("endpoint").toString(), QString("start"));
        key(Qt::Key_J); QCOMPARE(prop("endpoint").toString(), QString("end"));
        key(Qt::Key_K); key(Qt::Key_K); QCOMPARE(prop("endpoint").toString(), QString("start"));
        key(Qt::Key_Escape);
    }
    void singleTypeSkipsChooser_data() {
        QTest::addColumn<QString>("kind");
        for (const auto &kind : {"bookmark", "highlight", "note", "tags"}) QTest::newRow(kind) << QString(kind);
    }
    void singleTypeSkipsChooser() {
        QFETCH(QString, kind);
        if (kind == "bookmark") QVERIFY(bible->toggleBookmark());
        if (kind == "highlight") QVERIFY(bible->toggleHighlight());
        if (kind == "note") bible->setNoteText("Only note");
        const auto source = bible->tagScope();
        if (kind == "tags") {
            QVERIFY(!bible->createAndAssignTag(source, "First").isEmpty());
            QVERIFY(!bible->createAndAssignTag(source, "Second").isEmpty());
        }
        const auto snapshot = bible->annotationSnapshot(source);
        QSignalSpy commits(bible.get(), &BibleBackend::sharedCommitted);
        open(source, "unrelated"); // One present type wins even over an absent context.
        QVERIFY(prop("active").toBool()); QVERIFY(!prop("visibleModal").toBool());
        QCOMPARE(prop("presentKinds").toList(), QVariantList{kind});
        QCOMPARE(bible->annotationSnapshot(source), snapshot); QCOMPARE(commits.count(), 0);
        if (kind == "tags") QCOMPARE(prop("components").toMap().value("tags").toList().size(), 2);
        else QVERIFY(prop("components").toMap().value(kind).toBool());
        key(Qt::Key_J); const auto destination = bible->tagScope();
        key(Qt::Key_Return); QTRY_VERIFY(!prop("sessionOpen").toBool());
        const auto moved = bible->annotationSnapshot(destination), old = bible->annotationSnapshot(source);
        if (kind == "tags") {
            QCOMPARE(bible->tagsForScope(destination).size(), 2); QVERIFY(bible->tagsForScope(source).isEmpty());
        } else {
            QCOMPARE(moved.value(kind), snapshot.value(kind));
            QVERIFY(kind == "note" ? old.value(kind).toString().isEmpty() : !old.value(kind).toBool());
        }
    }
    void chooserKeyboardAndRollback() {
        QVERIFY(bible->toggleBookmark()); QVERIFY(bible->toggleHighlight()); bible->setNoteText("Original note");
        const auto source = bible->tagScope();
        const auto one = bible->createAndAssignTag(source, "One"), two = bible->createAndAssignTag(source, "Two");
        QVERIFY(!one.isEmpty() && !two.isEmpty());
        const auto snapshot = bible->annotationSnapshot(source);
        const auto collection = bible->collection();
        navigate("John 4:3-6"); bible->selectVerses(6, 3); window->setProperty("verseMode", true);
        const auto initial = bible->tagScope();
        open(source, "bookmark");
        QVERIFY(prop("visibleModal").toBool()); QVERIFY(!prop("active").toBool());
        const auto original = prop("original"), frozenSource = prop("source");
        QCOMPARE(item("rangeSelectAll")->property("text").toString(), QString("Select all components"));
        key(Qt::Key_A); QVERIFY(prop("allSelected").toBool());
        QCOMPARE(item("rangeSelectAll")->property("text").toString(), QString("Deselect all components"));
        QCOMPARE(plainHint(item("rangeChooserKeys")->property("text").toString()), QString("m bookmark    x highlight    t tags    n note    ↑ ↓ choose    space toggle    a deselect all    enter continue    esc cancel"));
        key(Qt::Key_A); QVERIFY(!prop("hasComponents").toBool()); QVERIFY(!item("rangeContinue")->isEnabled());
        key(Qt::Key_A); QVERIFY(prop("allSelected").toBool());
        click("rangeSelectAll"); QVERIFY(!prop("hasComponents").toBool()); QVERIFY(!item("rangeContinue")->isEnabled());
        key(Qt::Key_Return); QVERIFY(!prop("active").toBool()); QVERIFY(prop("visibleModal").toBool());
        const QStringList rows = {"rangeComponent:bookmark", "rangeComponent:highlight", "rangeComponent:note", "rangeTag:" + one, "rangeTag:" + two};
        for (int i = 0; i < rows.size(); ++i) {
            QTRY_VERIFY(item(rows[i])->hasActiveFocus());
            key(Qt::Key_Space);
            for (int j = 0; j < rows.size(); ++j) QCOMPARE(item(rows[j])->property("checked").toBool(), j <= i);
            if (i + 1 < rows.size()) key(i % 2 ? Qt::Key_Down : Qt::Key_J);
        }
        key(Qt::Key_Up); QVERIFY(item(rows[3])->hasActiveFocus());
        key(Qt::Key_K); QVERIFY(item(rows[2])->hasActiveFocus());
        key(Qt::Key_J); QVERIFY(item(rows[3])->hasActiveFocus());
        key(Qt::Key_Space); QCOMPARE(prop("components").toMap().value("tags").toList(), QVariantList{two});
        key(Qt::Key_T); QCOMPARE(prop("components").toMap().value("tags").toList().size(), 2);
        key(Qt::Key_T); QVERIFY(prop("components").toMap().value("tags").toList().isEmpty());
        key(Qt::Key_M); key(Qt::Key_X); key(Qt::Key_N); QVERIFY(!prop("hasComponents").toBool());
        key(Qt::Key_N); key(Qt::Key_X); key(Qt::Key_M); key(Qt::Key_T); QVERIFY(prop("allSelected").toBool());
        key(Qt::Key_Right); QVERIFY(item(rows[4])->hasActiveFocus());
        key(Qt::Key_Left); QVERIFY(item(rows[3])->hasActiveFocus());
        const auto selected = prop("components");
        key(Qt::Key_J); key(Qt::Key_Z); QCOMPARE(prop("components"), selected); QCOMPARE(bible->tagScope(), initial);
        QCOMPARE(prop("source"), frozenSource); QCOMPARE(prop("original"), original);
        QCOMPARE(bible->annotationSnapshot(source), snapshot); QCOMPARE(bible->collection(), collection);
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("sessionOpen").toBool());
        QCOMPARE(bible->tagScope(), initial); QCOMPARE(bible->cursorVerse(), 3); QVERIFY(window->property("verseMode").toBool());
        QCOMPARE(bible->annotationSnapshot(source), snapshot); QCOMPARE(bible->collection(), collection);
    }
    void chooserPresentOnlyAndEnter_data() {
        QTest::addColumn<bool>("cancelFocus");
        QTest::newRow("checkbox-return") << false; QTest::newRow("cancel-enter") << true;
    }
    void chooserPresentOnlyAndEnter() {
        QFETCH(bool, cancelFocus);
        QVERIFY(bible->toggleHighlight()); bible->setNoteText("Note"); const auto source = bible->tagScope();
        const auto snapshot = bible->annotationSnapshot(source);
        open(source, "highlight"); QVERIFY(prop("visibleModal").toBool());
        QVERIFY(!item("rangeComponent:bookmark")); QCOMPARE(prop("componentRows").toList().size(), 2);
        QCOMPARE(plainHint(item("rangeChooserKeys")->property("text").toString()), QString("x highlight    n note    ↑ ↓ choose    space toggle    a select all    enter continue    esc cancel"));
        inlineHint("rangeComponent:highlight", "x"); inlineHint("rangeComponent:note", "n"); inlineHint("rangeSelectAll", "a");
        QVERIFY(!item("rangeComponent:bookmarkHint"));
        QVERIFY(!item("rangeComponent:tagsHint"));
        const auto selected = prop("components");
        key(Qt::Key_M); key(Qt::Key_T); key(Qt::Key_Z); QCOMPARE(prop("components"), selected);
        auto target = item(cancelFocus ? "rangeChooserCancel" : "rangeComponent:highlight"); QVERIFY(target); target->forceActiveFocus();
        key(cancelFocus ? Qt::Key_Enter : Qt::Key_Return);
        if (cancelFocus) {
            // A focused button owns enter: Cancel ends the session untouched.
            QTRY_VERIFY(!prop("sessionOpen").toBool());
            QCOMPARE(bible->annotationSnapshot(source), snapshot);
            return;
        }
        QTRY_VERIFY(prop("active").toBool()); QVERIFY(!prop("visibleModal").toBool());
        QCOMPARE(prop("components"), selected); QCOMPARE(bible->annotationSnapshot(source), snapshot);
        QTRY_VERIFY(window->property("readerKeys").toBool());
        key(Qt::Key_J); QCOMPARE(bible->selectedEnd(), 5);
        key(Qt::Key_Escape); QCOMPARE(bible->tagScope(), source); QCOMPARE(bible->annotationSnapshot(source), snapshot);
    }
    void chooserAbsentHighlightAndNote() {
        QVERIFY(bible->toggleBookmark()); const auto source = bible->tagScope();
        const auto tag = bible->createAndAssignTag(source, "Only tag"); QVERIFY(!tag.isEmpty());
        open(source); QVERIFY(prop("visibleModal").toBool());
        QVERIFY(!item("rangeComponent:highlight")); QVERIFY(!item("rangeComponent:note"));
        QVERIFY(item("rangeComponent:bookmark")); QVERIFY(item("rangeTag:" + tag));
        QCOMPARE(prop("componentRows").toList().size(), 2);
        QCOMPARE(plainHint(item("rangeChooserKeys")->property("text").toString()), QString("m bookmark    t tags    ↑ ↓ choose    space toggle    a deselect all    enter continue    esc cancel"));
        inlineHint("rangeComponent:bookmark", "m"); inlineHint("rangeTag:" + tag, "t · all tags");
        QVERIFY(!item("rangeComponent:highlightHint")); QVERIFY(!item("rangeComponent:noteHint"));
        const auto selected = prop("components"); key(Qt::Key_X); key(Qt::Key_N); QCOMPARE(prop("components"), selected);
        click("rangeChooserCancel"); QVERIFY(!prop("sessionOpen").toBool()); QCOMPARE(bible->tagScope(), source);
    }
    void sourceChooserKeyboardThenSkip_data() {
        QTest::addColumn<bool>("mixed");
        QTest::newRow("only-tags-skip") << false; QTest::newRow("mixed-choose-components") << true;
    }
    void sourceChooserKeyboardThenSkip() {
        QFETCH(bool, mixed);
        QVERIFY(bible->toggleBookmark()); const auto first = bible->tagScope();
        navigate("John 3:3-5"); const auto second = bible->tagScope();
        QVERIFY(!bible->createAndAssignTag(second, "One").isEmpty());
        QVERIFY(!bible->createAndAssignTag(second, "Two").isEmpty());
        if (mixed) QVERIFY(bible->toggleHighlight());
        navigate("John 3:3"); const auto initial = bible->tagScope(); const auto collection = bible->collection();
        key(Qt::Key_E, Qt::ShiftModifier); QTRY_VERIFY(prop("visibleModal").toBool()); QTest::qWait(60);
        QCOMPARE(prop("ranges").toList().size(), 2); QVERIFY(prop("source").isNull()); QVERIFY(!prop("active").toBool());
        QCOMPARE(plainHint(item("rangeChooserKeys")->property("text").toString()), QString::fromUtf8("↑ ↓ choose    enter select    esc cancel"));
        auto firstRow = item("rangeChoice:" + first.value("reference").toString()); QVERIFY(firstRow); QVERIFY(firstRow->hasActiveFocus());
        key(Qt::Key_Down); auto secondRow = item("rangeChoice:" + second.value("reference").toString()); QVERIFY(secondRow); QVERIFY(secondRow->hasActiveFocus());
        key(Qt::Key_K); QVERIFY(firstRow->hasActiveFocus());
        key(Qt::Key_K); QVERIFY(secondRow->hasActiveFocus());
        key(Qt::Key_J); QVERIFY(firstRow->hasActiveFocus());
        key(Qt::Key_J); QVERIFY(secondRow->hasActiveFocus());
        key(Qt::Key_Return);
        if (mixed) {
            QVERIFY(prop("visibleModal").toBool()); QVERIFY(!prop("active").toBool());
            QCOMPARE(prop("source").toMap().value("scope").toMap(), second);
            QCOMPARE(prop("presentKinds").toList().size(), 2);
            QTRY_VERIFY(item("rangeComponent:highlight") && item("rangeComponent:highlight")->hasActiveFocus());
            key(Qt::Key_Return);
        }
        QTRY_VERIFY(prop("active").toBool()); QVERIFY(!prop("visibleModal").toBool());
        QCOMPARE(bible->tagScope(), second); QCOMPARE(prop("components").toMap().value("tags").toList().size(), 2);
        QCOMPARE(bible->collection(), collection); key(Qt::Key_Escape); QCOMPARE(bible->tagScope(), initial);
    }
    void singleTypeBeginFailureShowsChooser() {
        QVERIFY(bible->toggleBookmark()); const auto source = bible->tagScope();
        navigate("John 4:3-6"); const auto initial = bible->tagScope(); const auto collection = bible->collection();
        sql("CREATE TRIGGER fail_reading BEFORE UPDATE ON reading BEGIN SELECT RAISE(ABORT,'begin fixture failure'); END");
        open(source); QVERIFY(!prop("active").toBool()); QVERIFY(prop("visibleModal").toBool());
        QVERIFY(prop("message").toString().contains("begin fixture failure"));
        const auto snapshot = prop("source");
        QTest::qWait(100); QVERIFY(!prop("active").toBool()); QCOMPARE(prop("source"), snapshot); QCOMPARE(bible->collection(), collection);
        sql("DROP TRIGGER fail_reading"); key(Qt::Key_Return); QTRY_VERIFY(prop("active").toBool());
        QVERIFY(prop("message").toString().isEmpty());
        key(Qt::Key_Escape); QCOMPARE(bible->tagScope(), initial);
        auto invalid = source; invalid["end"] = 99;
        open(invalid); QVERIFY(prop("visibleModal").toBool()); QVERIFY(!prop("active").toBool());
        QVERIFY(prop("source").isNull()); QVERIFY(!prop("message").toString().isEmpty());
        QTest::qWait(100); QVERIFY(prop("visibleModal").toBool()); QVERIFY(!prop("active").toBool());
        QCOMPARE(bible->tagScope(), initial); QCOMPARE(bible->collection(), collection);
        key(Qt::Key_Escape); QVERIFY(!prop("sessionOpen").toBool());
    }
    void chooserGeometry_data() {
        QTest::addColumn<double>("scale"); QTest::addColumn<QSize>("size"); QTest::addColumn<int>("rangeCount");
        QTest::newRow("normal-two") << 1.0 << QSize(1000, 760) << 2;
        QTest::newRow("normal-many") << 1.0 << QSize(1000, 760) << 8;
        QTest::newRow("scaled-narrow-two") << 1.8 << QSize(640, 480) << 2;
        QTest::newRow("scaled-narrow-many") << 1.8 << QSize(640, 480) << 8;
    }
    void chooserGeometry() {
        QFETCH(double, scale); QFETCH(QSize, size); QFETCH(int, rangeCount);
        desktop->setProperty("textScale", scale); window->setMinimumSize(QSize(0, 0)); window->resize(size);
        QVERIFY(bible->toggleBookmark()); QVERIFY(bible->toggleHighlight()); bible->setNoteText("Note");
        const auto source = bible->tagScope();
        for (int i = 0; i < 8; ++i) QVERIFY(!bible->createAndAssignTag(source, QString("Tag %1 with a very long descriptive name that wraps across multiple lines <b>literally</b>").arg(i)).isEmpty());
        QVariantList ranges;
        for (int i = 0; i < rangeCount; ++i) {
            navigate(QString("John 3:2-%1").arg(4 + i));
            if (i) { QVERIFY(bible->toggleBookmark()); QVERIFY(bible->toggleHighlight()); }
            auto scope = bible->tagScope();
            // Keep real overlapping boundaries, stress the presentation with long references.
            scope["reference"] = scope.value("reference").toString() + " · A long edition and passage reference with <b>literal</b> text";
            ranges << scope;
        }
        navigate("John 3:3");
        key(Qt::Key_E, Qt::ShiftModifier); QTRY_VERIFY(prop("visibleModal").toBool());
        QCOMPARE(prop("ranges").toList().size(), rangeCount);
        QVERIFY(editor->setProperty("ranges", ranges));
        QTest::qWait(100);
        QVERIFY(QMetaObject::invokeMethod(object("rangeChooser"), "focusRow", Q_ARG(QVariant, 0)));
        QTest::qWait(60);
        QCOMPARE(window->size(), size); chooserBounds(false); capture("source");
        auto scroll = item("rangeChoiceScroll");
        for (int i = 0; i < ranges.size(); ++i) {
            auto row = item("rangeChoice:" + ranges[i].toMap().value("reference").toString()); QVERIFY(row);
            QCOMPARE(row->height(), row->implicitHeight());
            auto label = row->property("contentItem").value<QQuickItem *>(); QVERIFY(label);
            QCOMPARE(label->property("textFormat").toInt(), 0);
            QCOMPARE(label->property("horizontalAlignment").toInt(), int(Qt::AlignLeft));
            QVERIFY(row->height() >= label->implicitHeight() + row->property("topPadding").toReal() + row->property("bottomPadding").toReal());
            if (i == 0) QVERIFY(qAbs(top(row) - top(scroll)) <= 1);
            else {
                auto previous = item("rangeChoice:" + ranges[i-1].toMap().value("reference").toString());
                QVERIFY(qAbs(top(row) - bottom(previous) - 12 * prop("uiScale").toDouble()) <= 1);
            }
        }
        key(Qt::Key_K); QTest::qWait(60);
        auto lastSource = item("rangeChoice:" + ranges.last().toMap().value("reference").toString());
        QVERIFY(lastSource->hasActiveFocus()); QVERIFY(bottom(lastSource) <= bottom(scroll) + 1);
        key(Qt::Key_J); key(Qt::Key_Return); QTest::qWait(100);
        QVERIFY(!prop("source").isNull());
        auto snapshot = prop("source").toMap(), scope = snapshot.value("scope").toMap();
        scope["reference"] = ranges.first().toMap().value("reference"); snapshot["scope"] = scope;
        QVERIFY(editor->setProperty("source", snapshot)); QTest::qWait(100);
        QVERIFY(QMetaObject::invokeMethod(object("rangeChooser"), "focusRow", Q_ARG(QVariant, 0)));
        QTest::qWait(60);
        chooserBounds(true); capture("components");
        inlineHint("rangeSelectAll", "a");
        inlineHint("rangeComponent:bookmark", "m"); inlineHint("rangeComponent:highlight", "x"); inlineHint("rangeComponent:note", "n");
        auto bookmark = item("rangeComponent:bookmark");
        QVERIFY(top(bookmark) >= top(scroll) - 1); QVERIFY(bottom(bookmark) <= bottom(scroll) + 1);
        QVERIFY(top(item("rangeSelectAll")) <= top(scroll) + 1);
        const auto rows = prop("componentRows").toList();
        QQuickItem *previous = item("rangeSelectAll");
        for (const auto &entry : rows) {
            const auto row = entry.toMap();
            const auto name = row.value("kind") == "tags" ? "rangeTag:" + row.value("id").toString() : "rangeComponent:" + row.value("kind").toString();
            auto target = item(name); QVERIFY(target);
            QVERIFY(qAbs(top(target) - bottom(previous) - 12 * prop("uiScale").toDouble()) <= 1);
            if (row.value("kind") == "tags") inlineHint(name, "t · all tags");
            previous = target;
        }
        for (int i = 1; i < rows.size(); ++i) key(Qt::Key_J);
        QTest::qWait(60);
        const auto last = item("rangeTag:" + rows.last().toMap().value("id").toString()); QVERIFY(last && last->hasActiveFocus());
        QVERIFY(bottom(last) <= bottom(scroll) + 1);
        key(Qt::Key_Space); QVERIFY(!last->property("checked").toBool());
        click("rangeChooseAnother"); QTest::qWait(100);
        QVERIFY(prop("source").isNull()); chooserBounds(false); capture("source-back");
        auto first = item("rangeChoice:" + ranges.first().toMap().value("reference").toString());
        QVERIFY(first && first->hasActiveFocus()); QVERIFY(qAbs(top(first) - top(scroll)) <= 1);
        key(Qt::Key_Return); QTest::qWait(100); chooserBounds(true);
        key(Qt::Key_Escape); QVERIFY(!prop("sessionOpen").toBool());
    }
    // Visual checks: with OMAWORD_SHOTS set to a directory, save the window.
    void shot(const QString &name) {
        const auto directory = qEnvironmentVariable("OMAWORD_SHOTS");
        if (directory.isEmpty()) return;
        QCoreApplication::processEvents(); QTest::qWait(40);
        QVERIFY(window->grabWindow().save(directory + "/" + name + ".png"));
    }
    // The status bar's title never runs into its key hints, at any width or scale.
    void editStatusLayout_data() {
        QTest::addColumn<int>("width"); QTest::addColumn<double>("scale");
        for (int width : {360, 520, 760, 1200}) for (double scale : {1.0, 1.5, 2.0})
            QTest::newRow(qPrintable(QString("%1@%2").arg(width).arg(scale))) << width << scale;
    }
    void editStatusLayout() {
        QFETCH(int, width); QFETCH(double, scale);
        QVERIFY(bible->toggleBookmark()); const auto source = bible->tagScope();
        open(source); begin();
        desktop->setProperty("textScale", scale); window->resize(width, 700);
        QTRY_COMPARE(window->width(), width); QTest::qWait(80);
        auto title = item("rangeEditTitle"), hints = item("rangeEditHints"), status = item("rangeEditStatus");
        QVERIFY(title && hints && status && status->isVisible());
        auto check = [&](const QString &tag) {
            QTest::qWait(40);
            const auto t = title->mapRectToScene(QRectF(0, 0, title->width(), title->height()));
            QVERIFY2(t.right() <= status->mapToScene(QPointF(status->width(), 0)).x() + 0.5, qPrintable(tag));
            // The title shows whole or elided, never clipped by the hints.
            QVERIFY2(title->property("truncated").toBool() || title->width() + 0.5 >= title->property("implicitWidth").toDouble(), qPrintable(tag));
            for (auto part : hints->childItems()) {
                if (!part->isVisible() || part->width() <= 0) continue;
                const auto r = part->mapRectToScene(QRectF(0, 0, part->width(), part->height()));
                QVERIFY2(!r.intersects(t), qPrintable(tag + " " + part->objectName()));
                // A gap on the title's line, or the hints on their own lines below.
                if (r.top() < t.bottom() - 1) QVERIFY2(r.left() >= t.right() + 8 * scale - 0.5, qPrintable(tag + " gap " + part->objectName()));
                QVERIFY2(r.right() <= status->mapToScene(QPointF(status->width(), 0)).x() + 0.5, qPrintable(tag + " right " + part->objectName()));
            }
            // Everything stays inside the bar.
            QVERIFY2(hints->mapToScene(QPointF(0, hints->height())).y() <= status->mapToScene(QPointF(0, status->height())).y() + 0.5, qPrintable(tag));
        };
        shot(QString("rangestatus-%1-%2-end").arg(width).arg(scale));
        check("end");
        QCOMPARE(prop("endpoint").toString(), QString("end"));
        // Keyboard swap, then a mouse click on the swap hint.
        key(Qt::Key_O); QTRY_COMPARE(prop("endpoint").toString(), QString("start"));
        shot(QString("rangestatus-%1-%2-start").arg(width).arg(scale));
        check("start");
        QTRY_VERIFY(item("rangeEditHintsPart:swap")->property("clickable").toBool());
        click("rangeEditHintsPart:swap"); QTRY_COMPARE(prop("endpoint").toString(), QString("end"));
        check("clicked");
    }
    // The moving endpoint verse is marked apart from the range's tint by a
    // prominent caret flag (no underline): the start's at the top-left of its
    // first line, the end's at the bottom-right of its last line.
    void activeEndpointMarker_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("verse") << QString("verse"); QTest::newRow("paragraph") << QString("paragraph"); QTest::newRow("block") << QString("block");
    }
    void activeEndpointMarker() {
        QFETCH(QString, mode);
        bible->setReadingMode(mode); QTest::qWait(150);
        navigate("John 3:2-4");
        QVERIFY(bible->toggleHighlight()); const auto source = bible->tagScope();
        QVERIFY(!item("rangeEndpointMarker")->isVisible());
        open(source); begin();
        auto marker = item("rangeEndpointMarker"), caret = item("rangeEndpointCaret");
        QTRY_VERIFY(marker->isVisible()); QVERIFY(caret->isVisible());
        auto geometry = [&] { return value(window->property("rangeEndpointGeometry")).toMap(); };
        auto caretX = [&] { return caret->mapToScene(QPointF(caret->width() / 2, 0)).x(); };
        // End: verse 4, caret at the right end of its last line.
        QTRY_COMPARE(geometry().value("verse").toInt(), 4);
        QCOMPARE(geometry().value("endpoint").toString(), QString("end"));
        QVERIFY(!item("rangeEndpointUnderline0"));
        const double k = prop("readerScale").toDouble();
        QVERIFY(caret->width() >= 4 * k - 1); QVERIFY(item("rangeEndpointFlag")->width() >= 12 * k - 1);
        const auto rects = geometry().value("rects").toList(); QVERIFY(!rects.isEmpty());
        const auto last = rects.last().toMap();
        QVERIFY(qAbs(caret->mapToItem(item("readerColumn"), QPointF(caret->width() / 2, 0)).x() - (last["x"].toDouble() + last["width"].toDouble())) <= 2);
        // Bottom-right: spans the last line and hangs below it, its flag at the bottom.
        const auto column = item("readerColumn"), flag = item("rangeEndpointFlag");
        QVERIFY(qAbs(caret->mapToItem(column, QPointF(0, 0)).y() - last["y"].toDouble()) <= 1);
        QVERIFY(caret->mapToItem(column, QPointF(0, caret->height())).y() > last["y"].toDouble() + last["height"].toDouble());
        QVERIFY(qAbs(flag->mapToItem(column, QPointF(0, flag->height())).y() - caret->mapToItem(column, QPointF(0, caret->height())).y()) <= 1);
        QVERIFY(flag->mapToItem(column, QPointF(flag->width(), 0)).x() <= caret->mapToItem(column, QPointF(caret->width(), 0)).x() + 1);
        shot("endpoint-" + mode + "-end");
        // Keyboard: j moves the end; the marker follows.
        key(Qt::Key_J); QTRY_COMPARE(geometry().value("verse").toInt(), 5);
        // o swaps: the start (verse 2) is marked at its left edge.
        key(Qt::Key_O); QTRY_COMPARE(geometry().value("endpoint").toString(), QString("start"));
        QTRY_COMPARE(geometry().value("verse").toInt(), 2);
        const auto first = geometry().value("rects").toList().first().toMap();
        QVERIFY(qAbs(caret->mapToItem(item("readerColumn"), QPointF(caret->width() / 2, 0)).x() - first["x"].toDouble()) <= 2);
        // Top-left: rises above the first line, its flag at the top.
        QVERIFY(caret->mapToItem(column, QPointF(0, 0)).y() < first["y"].toDouble());
        QVERIFY(qAbs(caret->mapToItem(column, QPointF(0, caret->height())).y() - (first["y"].toDouble() + first["height"].toDouble())) <= 1);
        QVERIFY(qAbs(flag->mapToItem(column, QPointF(0, 0)).y() - caret->mapToItem(column, QPointF(0, 0)).y()) <= 1);
        const double startX = caretX();
        shot("endpoint-" + mode + "-start");
        // Mouse: the swap hint swaps back to the end.
        QTRY_VERIFY(item("rangeEditHintsPart:swap")->property("clickable").toBool());
        click("rangeEditHintsPart:swap"); QTRY_COMPARE(geometry().value("endpoint").toString(), QString("end"));
        QTRY_VERIFY(caretX() != startX);
        // Text size: the marker follows the new layout.
        key(Qt::Key_Equal, Qt::ControlModifier); QTest::qWait(200);
        QTRY_COMPARE(geometry().value("verse").toInt(), 5);
        const auto grown = geometry().value("rects").toList().last().toMap();
        QVERIFY(qAbs(caret->mapToItem(item("readerColumn"), QPointF(caret->width() / 2, 0)).x() - (grown["x"].toDouble() + grown["width"].toDouble())) <= 2);
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("sessionOpen").toBool());
        QTRY_VERIFY(!item("rangeEndpointMarker")->isVisible());
    }
    void markerEntrypoints_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("verse") << QString("verse"); QTest::newRow("block") << QString("block"); QTest::newRow("paragraph") << QString("paragraph");
    }
    void markerEntrypoints() {
        QFETCH(QString, mode); bible->setNoteText("Marker note"); const auto scope = bible->tagScope();
        bible->setReadingMode(mode); QTest::qWait(150);
        QVERIFY(QMetaObject::invokeMethod(window, "scrollToVerse", Q_ARG(QVariant, 2), Q_ARG(QVariant, true)));
        QTest::qWait(80);
        QTRY_VERIFY(!window->findChild<QObject *>("readerSlide")->property("running").toBool());
        const auto marker = mode == "verse" ? QString("verseNoteMarker2") : "blockInlineMarker:note:open:" + scope.value("reference").toString();
        QTRY_VERIFY(item(marker));
        auto target = item(marker); const auto point = target->mapToScene(QPointF(target->width()/2, qMin(12.0, target->height()/2))).toPoint();
        QVERIFY(QRect(QPoint(), window->size()).contains(point));
        QTest::mouseClick(window, Qt::LeftButton, Qt::ShiftModifier, point);
        QTRY_VERIFY(prop("active").toBool()); QVERIFY(!prop("visibleModal").toBool());
        QVERIFY(prop("components").toMap().value("note").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("sessionOpen").toBool());
        // Every layout's marker opens the shared context menu with Edit range.
        QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, point);
        QTRY_VERIFY(object("contextMenu")->property("opened").toBool());
        QCOMPARE(object("contextMenu")->property("target").toString(), QString("marker"));
        QTest::qWait(50); // let the popup settle its final position
        click("contextMenuItem:editRange"); QTRY_VERIFY(prop("active").toBool()); QVERIFY(!prop("visibleModal").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("sessionOpen").toBool());
        click(marker); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        QCOMPARE(item("noteEditor")->property("cursorPosition").toInt(), 11);
    }
};

int main(int argc, char **argv) {
    QTemporaryDir environment;
    for (const auto name : {"HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME", "XDG_RUNTIME_DIR"}) {
        const auto path = environment.filePath(name); QDir().mkpath(path);
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner); qputenv(name, path.toUtf8());
    }
    qputenv("QT_QPA_PLATFORM", "offscreen"); qputenv("QT_QUICK_BACKEND", "software");
    QApplication app(argc, argv); app.setOrganizationName("OmaWordTests"); app.setApplicationName("omaword-rangesui-test");
    QQuickStyle::setStyle("Basic"); RangesUiTest test; return QTest::qExec(&test, argc, argv);
}
#include "tst_rangesui.moc"
