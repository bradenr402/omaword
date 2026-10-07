#include "testenvironment.h"
#include <QtTest/QTest>
#include <cmath>
#include <utility>
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
#include "versioncatalog.h"

class ContextMenuTest : public QObject
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
        // Let the reader settle (slide and late layout) before measuring positions.
        double y = -1;
        QTRY_VERIFY(!object("readerSlide")->property("running").toBool()
            && std::exchange(y, item("reading")->property("contentY").toDouble()) == item("reading")->property("contentY").toDouble());
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
    bool menuOpen() const { return menu()->property("visible").toBool(); }
    QString target() const { return menu()->property("target").toString(); }
    // Entry ids with "|" for each divider.
    QStringList ids() const {
        QStringList out;
        for (const auto &entry : value(menu()->property("layout")).toList()) {
            const auto map = entry.toMap();
            out << (map.value("separator").toBool() ? QString("|") : map.value("id").toString());
        }
        return out;
    }
    QVariantMap entry(const QString &id) const {
        for (const auto &e : value(menu()->property("layout")).toList()) if (e.toMap().value("id") == id) return e.toMap();
        return {};
    }
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
    void rightClick(const QPoint &point) {
        QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, point); QCoreApplication::processEvents();
    }
    void rightClick(QQuickItem *target) {
        QVERIFY(target); QVERIFY(target->isVisible());
        rightClick(target->mapToScene(QPointF(target->width() / 2, qMin(target->height() / 2, 12.0))).toPoint());
    }
    void pressEntry(const QString &id) {
        // Rows are positioned on the next polish after opening.
        QTRY_VERIFY(item("contextMenuContent") && item("contextMenuContent")->height() > 0);
        QTest::qWait(50);
        auto row = item("contextMenuItem:" + id); QVERIFY2(row, qPrintable(id)); QVERIFY(row->isVisible());
        QTest::mouseClick(window, Qt::LeftButton, {}, row->mapToScene(QPointF(row->width() / 2, row->height() / 2)).toPoint());
        QCoreApplication::processEvents();
    }
    void clickEntry(const QString &id) {
        QTRY_VERIFY(item("contextMenuContent") && item("contextMenuContent")->height() > 0);
        QTest::qWait(50); // let the popup settle its final position
        auto row = item("contextMenuItem:" + id); QVERIFY2(row, qPrintable(id)); QVERIFY(row->isVisible());
        QTest::mouseClick(window, Qt::LeftButton, {}, row->mapToScene(QPointF(row->width() / 2, row->height() / 2)).toPoint());
        QCoreApplication::processEvents();
        QTRY_VERIFY(!menuOpen());
    }
    void escape() { key(Qt::Key_Escape); QTRY_VERIFY(!menuOpen()); }
    void layouts() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("verse") << QString("verse");
        QTest::newRow("paragraph") << QString("paragraph");
        QTest::newRow("block") << QString("block");
    }
    QString markerName(const QString &kind, int verse, const QString &reference) const {
        if (!prop("continuousReader").toBool())
            return (kind == "bookmark" ? QString("bookmarkMarker") : kind == "note" ? QString("verseNoteMarker") : QString("verseTagMarker")) + QString::number(verse);
        return "blockInlineMarker:" + kind + ":open:" + reference;
    }
    const QStringList passageIds = {"copy", "copyPlain", "|", "bookmark", "highlight", "note", "tags", "|", "desk", "|", "related", "chapterFind"};
    const QStringList markerIds = {"open", "|", "editRange", "remove", "|", "copy", "copyPlain", "|", "desk"};
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
        engine = std::make_unique<QQmlApplicationEngine>(); warnings.clear();
        connect(engine.get(), &QQmlEngine::warnings, this, [this](const QList<QQmlError> &errors) { for (const auto &error : errors) warnings << error.toString(); });
        engine->rootContext()->setContextProperty("bible", bible.get());
        engine->rootContext()->setContextProperty("desktop", desktop.get());
        engine->rootContext()->setContextProperty("sounds", sounds.get());
        engine->rootContext()->setContextProperty("catalog", catalog.get());
        engine->load(QUrl::fromLocalFile(QFINDTESTDATA("../qml/Main.qml")));
        QVERIFY2(!engine->rootObjects().isEmpty(), qPrintable(warnings.join('\n')));
        window = qobject_cast<QQuickWindow *>(engine->rootObjects().first()); QVERIFY(window);
        // Save notes on every change, as the assertions below expect (the app waits for a pause).
        window->setProperty("noteSaveDelay",0);
        window->requestActivate(); QVERIFY(QTest::qWaitForWindowExposed(window)); QTRY_VERIFY(window->isActive());
        navigate("John 1:1");
    }
    void cleanup() {
        engine.reset(); window = nullptr; catalog.reset(); sounds.reset(); desktop.reset(); bible.reset(); session.reset();
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
    }

    void passageMenuGroupsAndShortcuts_data() { layouts(); }
    void passageMenuGroupsAndShortcuts() {
        QFETCH(QString, mode);
        setLayout(mode);
        rightClick(versePoint(3));
        QTRY_VERIFY(menuOpen());
        QCOMPARE(target(), QString("passage"));
        QCOMPARE(bible->selectionReference(), QString("John 1:3"));
        QCOMPARE(ids(), passageIds);
        QVERIFY(prop("modalOpen").toBool()); QVERIFY(!prop("readerKeys").toBool());
        const QHash<QString, QString> shortcuts = {{"copy", "y"}, {"copyPlain", "shift Y"}, {"bookmark", "m"}, {"highlight", "x"},
            {"note", "w"}, {"tags", "t"}, {"desk", "d"}, {"related", "shift S"}, {"chapterFind", "/"}};
        for (auto it = shortcuts.cbegin(); it != shortcuts.cend(); ++it) {
            QCOMPARE(entry(it.key()).value("shortcut").toString(), it.value());
            auto label = item("contextMenuShortcut:" + it.key());
            QVERIFY(label && label->isVisible());
            QCOMPARE(label->property("text").toString(), it.value());
            QCOMPARE(label->property("color").value<QColor>(), desktop->property("muted").value<QColor>());
            QCOMPARE(label->property("horizontalAlignment").toInt(), int(Qt::AlignRight));
            // Right-aligned against the row's right edge.
            auto row = item("contextMenuItem:" + it.key());
            QVERIFY(label->mapToItem(row, QPointF(label->width(), 0)).x() > row->width() - 20);
        }
        QCOMPARE(entry("bookmark").value("text").toString(), QString("Bookmark"));
        QCOMPARE(entry("highlight").value("text").toString(), QString("Highlight"));
        QCOMPARE(entry("desk").value("text").toString(), QString("Add to Desk"));
        QCOMPARE(entry("note").value("text").toString(), QString("Write note"));
        // Dividers are thin border-colored rules between the four groups.
        int dividers = 0;
        for (int i = 0; i < passageIds.size(); ++i) {
            if (passageIds[i] != "|") continue;
            ++dividers;
            auto rule = item("contextMenuRule" + QString::number(i));
            QVERIFY(rule && rule->isVisible());
            QCOMPARE(rule->height(), 1.0);
            QCOMPARE(rule->property("color").value<QColor>(), desktop->property("border").value<QColor>());
        }
        QCOMPARE(dividers, 3);
        escape();
        QTRY_VERIFY(item("readerSurface")->hasActiveFocus());
        QCOMPARE(bible->selectionReference(), QString("John 1:3"));
    }
    void selectionRuleInsideAndOutside_data() { layouts(); }
    void selectionRuleInsideAndOutside() {
        QFETCH(QString, mode);
        setLayout(mode);
        // Visual range 2–4, then right-click inside: the range is kept.
        navigate("John 1:2"); key(Qt::Key_V, Qt::ShiftModifier); key(Qt::Key_J); key(Qt::Key_J);
        QCOMPARE(bible->selectedStart(), 2); QCOMPARE(bible->selectedEnd(), 4);
        rightClick(versePoint(3));
        QTRY_VERIFY(menuOpen());
        QCOMPARE(bible->selectedStart(), 2); QCOMPARE(bible->selectedEnd(), 4);
        escape();
        QCOMPARE(bible->selectedStart(), 2); QCOMPARE(bible->selectedEnd(), 4);
        // Outside the range: that verse becomes the selection first.
        rightClick(versePoint(6));
        QTRY_VERIFY(menuOpen());
        QCOMPARE(bible->selectedStart(), 6); QCOMPARE(bible->selectedEnd(), 6);
        QCOMPARE(bible->selectionReference(), QString("John 1:6"));
        escape();
    }
    void middleClickCopiesQuotationWithoutPaste_data() { layouts(); }
    void middleClickCopiesQuotationWithoutPaste() {
        QFETCH(QString, mode);
        setLayout(mode);
        bible->selectVerses(3, 3); const auto expected = bible->copySelection(); bible->selectVerses(1, 1);
        QVERIFY(expected.startsWith(QChar(0x201c))); QVERIFY(expected.contains("John 1:3"));
        const auto before = prop("continuousReader").toBool() ? item("blockBody")->property("text").toString()
                                                              : item("verseBody3")->property("text").toString();
        auto clipboard = QGuiApplication::clipboard();
        if (clipboard->supportsSelection()) clipboard->setText("PRIMARY PASTE", QClipboard::Selection);
        clipboard->setText("replace me");
        QTest::mouseClick(window, Qt::MiddleButton, {}, versePoint(3)); QCoreApplication::processEvents();
        QCOMPARE(clipboard->text(), expected);
        QCOMPARE(object("toast")->property("text").toString(), QString("John 1:3 copied"));
        QVERIFY(!menuOpen());
        QCOMPARE(bible->selectionReference(), QString("John 1:3"));
        // Read-only reader text never receives a PRIMARY paste.
        const auto after = prop("continuousReader").toBool() ? item("blockBody")->property("text").toString()
                                                             : item("verseBody3")->property("text").toString();
        QCOMPARE(after, before);
        QVERIFY(!after.contains("PRIMARY PASTE"));
        QVERIFY(bible->noteText().isEmpty());
        QVERIFY(!prop("notesOpen").toBool());
        QVERIFY(bible->collection().isEmpty());
    }
    void keyboardOpensNearCursor_data() { layouts(); }
    void keyboardOpensNearCursor() {
        QFETCH(QString, mode);
        setLayout(mode);
        navigate("John 1:5");
        for (const auto &combo : QList<QPair<Qt::Key, Qt::KeyboardModifiers>>{{Qt::Key_F10, Qt::ShiftModifier}, {Qt::Key_Menu, Qt::NoModifier}}) {
            key(combo.first, combo.second);
            QTRY_VERIFY(menuOpen());
            QCOMPARE(target(), QString("passage"));
            QCOMPARE(ids(), passageIds);
            QCOMPARE(bible->selectionReference(), QString("John 1:5"));
            const auto rect = verseViewportRect(5);
            const auto verseTop = item("reading")->mapToScene(rect.topLeft()).y();
            const auto menuY = menu()->property("y").toDouble();
            QVERIFY2(menuY >= verseTop - 4 && menuY <= verseTop + rect.height() + 80,
                     qPrintable(QString("menu y %1 verse top %2").arg(menuY).arg(verseTop)));
            // The same key closes it again; focus returns to the reader.
            key(combo.first, combo.second);
            QTRY_VERIFY(!menuOpen());
            QTRY_VERIFY(item("readerSurface")->hasActiveFocus());
        }
    }
    // The current row's highlight is inset equally on the left and right at
    // every scale, whether it is reached by mouse or by key.
    void highlightInsetsAreSymmetric_data() {
        QTest::addColumn<double>("scale");
        for (double scale : {1.0, 1.1, 1.25, 1.33, 1.5, 1.75}) QTest::newRow(qPrintable(QString::number(scale))) << scale;
    }
    void highlightInsetsAreSymmetric() {
        QFETCH(double, scale);
        desktop->setProperty("textScale", scale); QCoreApplication::processEvents();
        auto check = [&](int index) {
            auto highlight = item("contextMenuHighlight" + QString::number(index));
            auto background = item("contextMenuBackground");
            QVERIFY(highlight && highlight->isVisible() && background);
            const auto rect = highlight->mapRectToItem(background, highlight->boundingRect());
            const double left = rect.left(), right = background->width() - rect.right();
            QVERIFY2(qAbs(left - right) < 0.01, qPrintable(QString("%1 vs %2").arg(left).arg(right)));
            QVERIFY(left > 0);
            // Whole pixels in the scene, so nothing rounds unevenly.
            const auto scene = highlight->mapToScene(QPointF());
            QCOMPARE(scene.x(), std::round(scene.x()));
            QCOMPARE(highlight->width(), std::round(highlight->width()));
        };
        // Keyboard-opened, then moved by key.
        key(Qt::Key_Menu); QTRY_VERIFY(menuOpen());
        check(0); key(Qt::Key_Down); check(1);
        escape();
        // Mouse-opened at a fractional-looking point, hovered row.
        rightClick(versePoint(3)); QTRY_VERIFY(menuOpen());
        auto row = item("contextMenuItem:bookmark");
        QTest::mouseMove(window, row->mapToScene(QPointF(row->width() / 2, row->height() / 2)).toPoint());
        QTRY_COMPARE(menu()->property("currentIndex").toInt(), passageIds.indexOf("bookmark"));
        check(passageIds.indexOf("bookmark"));
        // Visual check: OMAWORD_SHOTS saves the menu, magnified.
        if (const auto directory = qEnvironmentVariable("OMAWORD_SHOTS"); !directory.isEmpty()) {
            QTest::qWait(30);
            auto background = item("contextMenuBackground");
            const auto rect = background->mapRectToScene(background->boundingRect()).toAlignedRect().adjusted(-2, -2, 2, 2);
            const auto image = window->grabWindow().copy(rect);
            QVERIFY(image.scaled(image.size() * 3, Qt::KeepAspectRatio, Qt::FastTransformation)
                .save(directory + "/menu-inset-" + QString::number(scale) + ".png"));
        }
        escape();
    }
    void keyboardNavigationAndEscape() {
        QTest::mouseMove(window, QPoint(window->width() / 2 + 200, window->height() - 120));
        key(Qt::Key_Menu); QTRY_VERIFY(menuOpen());
        QCOMPARE(menu()->property("currentIndex").toInt(), 0);
        key(Qt::Key_Down); QCOMPARE(menu()->property("currentIndex").toInt(), 1);
        key(Qt::Key_Down); QCOMPARE(menu()->property("currentIndex").toInt(), 3); // skips divider
        key(Qt::Key_End); QCOMPARE(menu()->property("currentIndex").toInt(), passageIds.size() - 1);
        key(Qt::Key_Down); QCOMPARE(menu()->property("currentIndex").toInt(), 0); // wraps
        key(Qt::Key_Up); QCOMPARE(menu()->property("currentIndex").toInt(), passageIds.size() - 1);
        key(Qt::Key_Home); QCOMPARE(menu()->property("currentIndex").toInt(), 0);
        QVERIFY(item("contextMenuHighlight0")->isVisible());
        // Reader keys never act behind the menu.
        key(Qt::Key_J); key(Qt::Key_M); key(Qt::Key_D);
        QVERIFY(menuOpen()); QCOMPARE(bible->selectionReference(), QString("John 1:1"));
        QVERIFY(bible->collection().isEmpty()); QVERIFY(bible->desk().isEmpty());
        escape();
        QTRY_VERIFY(item("readerSurface")->hasActiveFocus());
        QVERIFY(bible->collection().isEmpty());
        // Enter runs the highlighted item: plain text copy.
        QGuiApplication::clipboard()->setText("replace me");
        key(Qt::Key_F10, Qt::ShiftModifier); QTRY_VERIFY(menuOpen());
        key(Qt::Key_Down); key(Qt::Key_Return);
        QTRY_VERIFY(!menuOpen());
        QCOMPARE(QGuiApplication::clipboard()->text(), bible->copySelection(true));
        QVERIFY(!QGuiApplication::clipboard()->text().contains(QChar(0x201c)));
        QTRY_VERIFY2(item("readerSurface")->hasActiveFocus(), qPrintable(QDebug::toString(window->activeFocusItem()) + QString(" active=%1").arg(window->isActive())));
        // Outside click closes without acting.
        key(Qt::Key_Menu); QTRY_VERIFY(menuOpen());
        // Away from the menu and from the drawers' edge-drag margins.
        QTest::mouseClick(window, Qt::LeftButton, {}, QPoint(window->width() / 2 + 200, window->height() / 2 + 150));
        QTRY_VERIFY(!menuOpen());
        QVERIFY(bible->collection().isEmpty());
    }
    void passageActionsReuseCommands() {
        rightClick(versePoint(2)); QTRY_VERIFY(menuOpen());
        QGuiApplication::clipboard()->setText("replace me");
        clickEntry("copy");
        QCOMPARE(QGuiApplication::clipboard()->text(), bible->copySelection());
        QVERIFY(object("toast")->property("text").toString().contains("John 1:2 copied"));
        rightClick(versePoint(2)); QTRY_VERIFY(menuOpen());
        clickEntry("bookmark");
        QVERIFY(bible->selectionBookmarked());
        rightClick(versePoint(2)); QTRY_VERIFY(menuOpen());
        QCOMPARE(entry("bookmark").value("text").toString(), QString("Remove bookmark"));
        clickEntry("desk");
        QCOMPARE(bible->desk().size(), 1);
        QVERIFY(bible->currentInDesk());
        rightClick(versePoint(2)); QTRY_VERIFY(menuOpen());
        QCOMPARE(entry("desk").value("text").toString(), QString("Remove from Desk"));
        clickEntry("highlight");
        QVERIFY(bible->selectionHighlighted());
        // A Visual range completes after an action, exactly like the key.
        navigate("John 1:4"); key(Qt::Key_V, Qt::ShiftModifier); key(Qt::Key_J);
        QVERIFY(prop("verseMode").toBool());
        rightClick(versePoint(5)); QTRY_VERIFY(menuOpen());
        QCOMPARE(bible->selectedStart(), 4); QCOMPARE(bible->selectedEnd(), 5);
        clickEntry("bookmark");
        QVERIFY(!prop("verseMode").toBool());
        QVERIFY(std::any_of(bible->collection().cbegin(), bible->collection().cend(), [](const QVariant &row) {
            return row.toMap().value("reference").toString() == QString::fromUtf8("John 1:4–5") && row.toMap().value("bookmark").toBool();
        }));
        // Write note opens the notes editor; Find in chapter opens the prompt.
        rightClick(versePoint(7)); QTRY_VERIFY(menuOpen());
        clickEntry("note");
        QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("notesOpen").toBool());
        call("readerFocus");
        rightClick(versePoint(7)); QTRY_VERIFY(menuOpen());
        clickEntry("chapterFind");
        QTRY_VERIFY(prop("findPromptOpen").toBool());
        key(Qt::Key_Escape);
    }
    void markerMenusInEveryLayout_data() { layouts(); }
    void markerMenusInEveryLayout() {
        QFETCH(QString, mode);
        bible->selectVerses(2, 3); QVERIFY(bible->toggleBookmark());
        const auto reference = bible->selectionReference();
        bible->selectVerses(1, 1);
        setLayout(mode);
        const auto name = markerName("bookmark", 2, reference);
        QTRY_VERIFY(item(name) && item(name)->isVisible());
        rightClick(item(name)); QTRY_VERIFY(menuOpen());
        QCOMPARE(target(), QString("marker"));
        QCOMPARE(ids(), markerIds);
        // shift E edits the selection's range (here none): the key is not shown.
        QCOMPARE(entry("editRange").value("shortcut").toString(), QString());
        QCOMPARE(entry("remove").value("text").toString(), QString("Remove bookmark"));
        QCOMPARE(entry("desk").value("text").toString(), QString("Add to Desk"));
        // Copy passage copies the marker's range without moving the reader.
        QGuiApplication::clipboard()->setText("replace me");
        clickEntry("copy");
        QVERIFY2(QGuiApplication::clipboard()->text().contains(reference), qPrintable(QGuiApplication::clipboard()->text() + " / " + object("toast")->property("text").toString() + " / " + bible->error()));
        QVERIFY(QGuiApplication::clipboard()->text().startsWith(QChar(0x201c)));
        QCOMPARE(bible->selectionReference(), QString("John 1:1"));
        rightClick(item(name)); QTRY_VERIFY(menuOpen());
        clickEntry("desk");
        QCOMPARE(bible->desk().size(), 1);
        QCOMPARE(bible->desk().first().toMap().value("reference").toString(), reference);
        rightClick(item(name)); QTRY_VERIFY(menuOpen());
        QCOMPARE(entry("desk").value("text").toString(), QString("Remove from Desk"));
        clickEntry("editRange");
        QTRY_VERIFY(prop("rangeEditing").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("rangeEditing").toBool());
        call("readerFocus");
        QTRY_VERIFY(item(name) && item(name)->isVisible());
        rightClick(item(name)); QTRY_VERIFY(menuOpen());
        clickEntry("open");
        QTRY_COMPARE(bible->selectionReference(), reference);
        rightClick(item(name)); QTRY_VERIFY(menuOpen());
        // With the marker's range selected, shift E edits it: the key shows.
        QCOMPARE(entry("editRange").value("shortcut").toString(), QString("shift E"));
        clickEntry("remove");
        QTRY_VERIFY(bible->collection().isEmpty());
    }
    void deleteNoteKeepsMarks_data() { layouts(); }
    void deleteNoteKeepsMarks() {
        QFETCH(QString, mode);
        bible->selectVerses(2, 3);
        bible->setNoteText("A long note worth confirming"); QVERIFY(bible->toggleBookmark()); QVERIFY(bible->toggleHighlight());
        const auto scope = bible->tagScope(); const auto reference = bible->selectionReference();
        QVERIFY(!bible->createAndAssignTag(scope, "Kept").isEmpty());
        bible->selectVerses(1, 1); call("syncNote");
        setLayout(mode);
        const auto name = markerName("note", 2, reference);
        QTRY_VERIFY(item(name) && item(name)->isVisible());
        rightClick(item(name)); QTRY_VERIFY(menuOpen());
        QCOMPARE(entry("remove").value("text").toString(), QString("Delete note…"));
        clickEntry("remove");
        const auto confirm = object("deleteConfirmation");
        QTRY_VERIFY(confirm->property("opened").toBool());
        QCOMPARE(object("deleteTitle")->property("text").toString(), QString("Delete note?"));
        QVERIFY(object("deleteExplanation")->property("text").toString().contains("deletes the note"));
        QTRY_VERIFY(item("deleteConfirm"));
        QTest::mouseClick(window, Qt::LeftButton, {}, item("deleteConfirm")->mapToScene(QPointF(4, 4)).toPoint());
        QTRY_VERIFY(!confirm->property("visible").toBool());
        QCOMPARE(bible->collection().size(), 1);
        const auto row = bible->collection().first().toMap();
        QCOMPARE(row.value("reference").toString(), reference);
        QVERIFY(row.value("bookmark").toBool()); QVERIFY(row.value("highlight").toBool());
        QVERIFY(row.value("note").toString().isEmpty());
        QCOMPARE(bible->tagsForScope(scope).size(), 1);
        QTRY_VERIFY(!item(name) || !item(name)->isVisible());
        QVERIFY(object("toast")->property("text").toString().contains("note deleted"));
    }
    void deskAnnotationAndSearchMenus() {
        navigate("John 1:1"); key(Qt::Key_D); QCOMPARE(bible->desk().size(), 1);
        openDesk();
        rightClick(item("deskText0")); QTRY_VERIFY(menuOpen());
        QCOMPARE(target(), QString("desk"));
        QCOMPARE(ids(), (QStringList{"open", "copy", "copyPlain", "desk"}));
        QGuiApplication::clipboard()->setText("replace me");
        clickEntry("copy");
        QVERIFY(QGuiApplication::clipboard()->text().contains("John 1:1"));
        QVERIFY(deskOpen()); QTRY_VERIFY(item("deskList")->hasActiveFocus());
        key(Qt::Key_F10, Qt::ShiftModifier); QTRY_VERIFY(menuOpen());
        QCOMPARE(target(), QString("desk"));
        clickEntry("desk");
        QTRY_VERIFY(bible->desk().isEmpty());
        key(Qt::Key_Escape); QTRY_VERIFY(!object("deskDrawer")->property("visible").toBool());
        // Annotations panel row.
        navigate("John 1:2"); key(Qt::Key_M);
        key(Qt::Key_A, Qt::ShiftModifier); QTRY_VERIFY(object("savedDrawer")->property("opened").toBool());
        const auto saved = "savedEntry" + bible->collection().first().toMap().value("reference").toString();
        QTRY_VERIFY(item(saved) && item(saved)->isVisible());
        rightClick(item(saved)); QTRY_VERIFY(menuOpen());
        QCOMPARE(target(), QString("annotation"));
        QCOMPARE(ids(), (QStringList{"open", "copy", "copyPlain", "desk", "|", "editRange", "delete"}));
        clickEntry("desk");
        QVERIFY2(bible->desk().size() == 1, qPrintable(object("toast")->property("text").toString() + " / " + bible->error() + " / " + QString::number(menu()->property("x").toDouble()) + "," + QString::number(menu()->property("y").toDouble())));
        rightClick(item(saved)); QTRY_VERIFY(menuOpen());
        clickEntry("delete");
        QTRY_VERIFY(object("deleteConfirmation")->property("opened").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!object("deleteConfirmation")->property("visible").toBool());
        QCOMPARE(bible->collection().size(), 1);
        rightClick(item(saved)); QTRY_VERIFY(menuOpen());
        clickEntry("editRange");
        QTRY_VERIFY(prop("rangeEditing").toBool());
        QTRY_VERIFY(!object("savedDrawer")->property("visible").toBool());
        QTRY_VERIFY(!prop("modalOpen").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("rangeEditing").toBool());
        // Bible search result in the palette.
        call("readerFocus");
        search("alpha");
        QTRY_VERIFY(object("paletteResults")->property("currentItem").value<QQuickItem *>());
        auto row = object("paletteResults")->property("currentItem").value<QQuickItem *>();
        rightClick(row); QTRY_VERIFY(menuOpen());
        QCOMPARE(target(), QString("search"));
        QCOMPARE(ids(), (QStringList{"open", "copy", "copyPlain", "desk"}));
        QGuiApplication::clipboard()->setText("replace me");
        clickEntry("copy");
        QVERIFY(QGuiApplication::clipboard()->text().contains("John"));
        QVERIFY(object("passagePalette")->property("opened").toBool());
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        // Results come in Bible order: John 1:2 (already on the Desk) first, then John 1:8.
        key(Qt::Key_Down); QTRY_COMPARE(object("paletteResults")->property("currentIndex").toInt(), 1);
        rightClick(object("paletteResults")->property("currentItem").value<QQuickItem *>()); QTRY_VERIFY(menuOpen());
        const int before = bible->desk().size();
        clickEntry("desk");
        QCOMPARE(bible->desk().size(), before + 1);
        QCOMPARE(bible->desk().last().toMap()["reference"].toString(), QString("John 1:8"));
        rightClick(object("paletteResults")->property("currentItem").value<QQuickItem *>()); QTRY_VERIFY(menuOpen());
        clickEntry("open");
        QTRY_VERIFY(!object("passagePalette")->property("visible").toBool());
    }
    void disabledItemsAndTheme() {
        auto background = item("contextMenuBackground");
        QVERIFY(!background || !background->isVisible());
        rightClick(versePoint(2)); QTRY_VERIFY(menuOpen());
        background = item("contextMenuBackground"); QVERIFY(background);
        QCOMPARE(background->property("color").value<QColor>(), desktop->property("surface").value<QColor>());
        QCOMPARE(background->property("border").value<QObject *>()->property("color").value<QColor>(), desktop->property("border").value<QColor>());
        QCOMPARE(background->property("radius").toDouble(), qMin(desktop->cornerRadius(), qMin(background->width(), background->height()) / 2));
        QCOMPARE(item("contextMenuLabel:copy")->property("color").value<QColor>(), desktop->property("foreground").value<QColor>());
        QCOMPARE(item("contextMenuLabel:copy")->property("font").value<QFont>().family(), desktop->property("fontFamily").toString());
        escape();
        // Live theme and rounded corners: swap in a theme object.
        QQmlComponent component(engine.get());
        component.setData("import QtQuick\nQtObject { property color surface: '#102030'; property color foreground: '#f0e0d0'; "
                          "property color muted: '#808080'; property color accent: '#ff8800'; property color border: '#405060'; "
                          "property real cornerRadius: 6; property real textScale: 1; property string fontFamily: 'DejaVu Sans' }", QUrl());
        std::unique_ptr<QObject> theme(component.create());
        QVERIFY2(theme, qPrintable(component.errorString()));
        QVERIFY(menu()->setProperty("theme", QVariant::fromValue(theme.get())));
        rightClick(versePoint(2)); QTRY_VERIFY(menuOpen());
        QCOMPARE(background->property("color").value<QColor>(), QColor("#102030"));
        QCOMPARE(background->property("radius").toDouble(), 6.0);
        QCOMPARE(item("contextMenuLabel:copy")->property("color").value<QColor>(), QColor("#f0e0d0"));
        QCOMPARE(item("contextMenuShortcut:copy")->property("color").value<QColor>(), QColor("#808080"));
        escape();
        QVERIFY(menu()->setProperty("theme", QVariant::fromValue(static_cast<QObject *>(desktop.get()))));
        // During range editing the menu offers the session's actions, then the copies.
        bible->selectVerses(2, 2); QVERIFY(bible->toggleBookmark()); call("readerFocus");
        key(Qt::Key_E, Qt::ShiftModifier); QTRY_VERIFY(prop("rangeEditing").toBool());
        rightClick(versePoint(2)); QTRY_VERIFY(menuOpen());
        QCOMPARE(target(), QString("rangeEdit"));
        QVERIFY(entry("copy").value("enabled").toBool());
        QVERIFY(entry("saveRange").value("enabled").toBool());
        escape();
        QVERIFY(prop("rangeEditing").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("rangeEditing").toBool());
        // Disabled items are muted and skipped; pressing one runs nothing.
        const QJSValue entries = engine->evaluate("[{ id: 'first', text: 'First' }, { separator: true }, { id: 'off', text: 'Off', enabled: false }]");
        QVERIFY(QMetaObject::invokeMethod(window, "showContextMenu", Q_ARG(QVariant, QString("test")), Q_ARG(QVariant, QVariant::fromValue(entries)),
            Q_ARG(QVariant, 200), Q_ARG(QVariant, 200)));
        QTRY_VERIFY(menuOpen());
        QVERIFY(!entry("off").value("enabled").toBool());
        QCOMPARE(item("contextMenuLabel:off")->property("color").value<QColor>(), desktop->property("muted").value<QColor>());
        key(Qt::Key_End); QCOMPARE(menu()->property("currentIndex").toInt(), 0);
        pressEntry("off");
        QVERIFY(menuOpen()); // disabled: nothing runs
        escape();
    }
};

int main(int argc, char **argv) {
    QQuickStyle::setStyle("Basic"); QApplication app(argc, argv);
    ContextMenuTest test; return QTest::qExec(&test, argc, argv);
}
#include "tst_contextmenu.moc"
