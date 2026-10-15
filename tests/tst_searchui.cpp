#include "testenvironment.h"
#include <QtTest/QTest>
#include <QApplication>
#include <QFile>
#include <QJSValue>
#include <QQmlApplicationEngine>
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
#include "aofixture.h"
#include "biblebackend.h"
#include "blockdocument.h"
#include "desktop.h"
#include "soundeffects.h"
#include "versioncatalog.h"

class SearchUiTest : public QObject
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
        return AoFixture::searchEdition(path, id, alternate);
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
        bible->importAo(QUrl::fromLocalFile(file)); QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(), 30000);
        QVERIFY2(bible->ready() && bible->error().isEmpty(), qPrintable(bible->error()));
    }
    // Programmatic scrolls slide (readerSlide); read positions once it lands.
    double settledY() {
        auto slide = object("readerSlide");
        for (int i = 0; slide && slide->property("running").toBool() && i < 200; ++i) QTest::qWait(10);
        return object("reading")->property("contentY").toDouble();
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
private slots:
    void initTestCase() {
        first = fixtures.filePath("engnet.zip"); second = fixtures.filePath("eng-kjv.zip");
        QVERIFY(edition(first, "engnet", false)); QVERIFY(edition(second, "eng-kjv", true));
    }
    void init() {
        session = std::make_unique<QTemporaryDir>(); qputenv("XDG_DATA_HOME", session->path().toUtf8());
        bible = std::make_unique<BibleBackend>(); install(first); bible->setReadingMode("verse");
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
    void soundEffectsCommandTitleFlips() {
        const auto titles = [this] {
            QVariant entries;
            QMetaObject::invokeMethod(window, "paletteEntries", Q_RETURN_ARG(QVariant, entries));
            QStringList result;
            for (const auto &entry : value(entries).toList()) result << entry.toMap().value("title").toString();
            return result;
        };
        QVERIFY(sounds->enabled());
        key(Qt::Key_K, Qt::ControlModifier); QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("> sound");
        QTRY_COMPARE(titles(), QStringList{"Disable sound effects"});
        key(Qt::Key_Return);
        QTRY_VERIFY(!sounds->enabled());
        QVERIFY(!SoundEffects(nullptr, session->filePath("sound.ini"), false).enabled());
        key(Qt::Key_K, Qt::ControlModifier); QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("> sound");
        QTRY_COMPARE(titles(), QStringList{"Enable sound effects"});
        // Another window sharing the instance sees the change live.
        sounds->setEnabled(true);
        QTRY_COMPARE(titles(), QStringList{"Disable sound effects"});
    }
    void palettePrefixEditing() {
        key(Qt::Key_K, Qt::ControlModifier); QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        for (const auto &prefix : {QString("/"), QString(">")}) {
            key(Qt::Key_A, Qt::ControlModifier); type(prefix);
            QCOMPARE(object("referenceInput")->property("text").toString(), prefix + " ");
            QCOMPARE(object("referenceInput")->property("cursorPosition").toInt(), 2);
            // Backspace on the keyboard-inserted empty mode returns to Passage.
            key(Qt::Key_Backspace);
            QCOMPARE(object("referenceInput")->property("text").toString(), QString());
            QVERIFY(object("paletteBooks")->property("emphasized").toBool());
            QVERIFY(item("referenceInput")->hasActiveFocus());
            // Input is live during the fade-in; opened follows when it completes.
            QTRY_VERIFY(object("passagePalette")->property("opened").toBool());
            type(prefix);
            type("alpha beta");
            QCOMPARE(object("referenceInput")->property("text").toString(), prefix + " alpha beta");
            key(Qt::Key_Home); key(Qt::Key_Right); key(Qt::Key_Delete);
            QCOMPARE(object("referenceInput")->property("text").toString(), prefix + "alpha beta");
            QCOMPARE(object("referenceInput")->property("cursorPosition").toInt(), 1);
            key(Qt::Key_End); type(" ");
            QCOMPARE(object("referenceInput")->property("text").toString(), prefix + "alpha beta ");
            // Deleting the last query character must not recreate a separator.
            const QString query = "alpha beta ";
            for (int remaining = query.size() - 1; remaining >= 0; --remaining) {
                key(Qt::Key_Backspace);
                QCOMPARE(object("referenceInput")->property("text").toString(), prefix + query.left(remaining));
                QCOMPARE(object("referenceInput")->property("cursorPosition").toInt(), remaining + 1);
            }
            key(Qt::Key_Backspace);
            QCOMPARE(object("referenceInput")->property("text").toString(), QString());
            QVERIFY(object("paletteBooks")->property("emphasized").toBool());
            type(prefix); type("query");
            key(Qt::Key_Home); key(Qt::Key_Delete);
            QCOMPARE(object("referenceInput")->property("text").toString(), QString(" query"));
            QCOMPARE(object("referenceInput")->property("cursorPosition").toInt(), 0);
            QVERIFY(object("paletteBooks")->property("emphasized").toBool());
            type("x");
            QCOMPARE(object("referenceInput")->property("text").toString(), QString("x query"));
            // Programmatic updates must not trigger the user-edit normalization.
            object("referenceInput")->setProperty("text", prefix);
            QCOMPARE(object("referenceInput")->property("text").toString(), prefix);
        }
    }
    void palettePrefixPaint_data() {
        QTest::addColumn<QString>("prefix");
        QTest::newRow("search") << QString("/");
        QTest::newRow("command") << QString(">");
    }
    void palettePrefixPaint() {
        QFETCH(QString, prefix);
        // Distinct opaque colors expose native foreground ink painted over the overlay.
        const QColor foreground("#40f040"), muted("#e050a0");
        desktop->setProperty("surface", QColor("#101010"));
        desktop->setProperty("foreground", foreground);
        desktop->setProperty("muted", muted);
        key(Qt::Key_K, Qt::ControlModifier); QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        auto input = item("referenceInput");
        auto font = input->property("font").value<QFont>(); font.setPixelSize(32);
        input->setProperty("font", font);
        type(prefix); type("query");
        QCOMPARE(input->property("text").toString(), prefix + " query");
        auto overlay = item("palettePrefix"); QVERIFY(overlay); QVERIFY(overlay->isVisible());
        QQuickItem *label = nullptr;
        for (auto child : overlay->childItems()) if (child->property("text").toString() == prefix) label = child;
        QVERIFY(label);
        QCOMPARE(label->property("color"), desktop->property("muted"));
        QCOMPARE(input->property("color"), desktop->property("foreground"));
        QVERIFY(overlay->parentItem()->z() >= 10);
        QTRY_COMPARE(object("passagePalette")->property("opacity").toDouble(), 1.0); // fade-in done
        QTest::qWait(100);
        const auto image = window->grabWindow(); QVERIFY(!image.isNull());
        const qreal sx = qreal(image.width()) / window->width();
        const qreal sy = qreal(image.height()) / window->height();
        // Sample the input's painted glyph cells, independently of overlay geometry.
        // TextInput cursor coordinates exclude the TextField's padding.
        const qreal left = input->property("leftPadding").toDouble();
        const auto prefixRect = input->mapRectToScene(QRectF(left, 0,
            caret(input, 1).x() - caret(input, 0).x(), input->height()));
        const auto queryRect = input->mapRectToScene(QRectF(left + caret(input, 2).x(), 0,
            caret(input, 7).x() - caret(input, 2).x(), input->height()));
        const auto ink = [&](const QRectF &bounds, const QColor &color) {
            const QRect pixels = QRectF(bounds.x() * sx, bounds.y() * sy,
                bounds.width() * sx, bounds.height() * sy).toAlignedRect().intersected(image.rect());
            int count = 0;
            for (int y = pixels.top(); y <= pixels.bottom(); ++y)
                for (int x = pixels.left(); x <= pixels.right(); ++x) {
                    const auto p = image.pixelColor(x, y);
                    if (qAbs(p.red() - color.red()) <= 16 && qAbs(p.green() - color.green()) <= 16
                            && qAbs(p.blue() - color.blue()) <= 16) ++count;
                }
            return count;
        };
        QVERIFY2(ink(queryRect, foreground) >= 5, "The query must retain normal foreground ink");
        const int mutedPixels = ink(prefixRect, muted), foregroundPixels = ink(prefixRect, foreground);
        QVERIFY2(mutedPixels >= 3, qPrintable(QString("The actual window must paint muted prefix ink; muted=%1, foreground=%2")
            .arg(mutedPixels).arg(foregroundPixels)));
        QCOMPARE(foregroundPixels, 0);
        key(Qt::Key_Home); key(Qt::Key_Delete);
        QCOMPARE(input->property("text").toString(), QString(" query"));
        QVERIFY(!overlay->isVisible());
        type("x"); QCOMPARE(input->property("text").toString(), QString("x query"));
    }
    void paletteTabCycle_data() {
        QTest::addColumn<QString>("focus");
        QTest::addColumn<int>("direction");
        for (const auto &focus : {QString("referenceInput"), QString("paletteResults"), QString("currentItem"),
                QString("paletteBooks"), QString("paletteSearch"), QString("paletteCommands")})
            for (int direction : {1, -1})
                QTest::newRow(qPrintable(focus + (direction > 0 ? "-tab" : "-shift-tab"))) << focus << direction;
    }
    void paletteTabCycle() {
        QFETCH(QString, focus); QFETCH(int, direction);
        key(Qt::Key_K, Qt::ControlModifier); QTRY_VERIFY(object("passagePalette")->property("opened").toBool());
        auto popup = object("passagePalette");
        QSignalSpy hiding(popup, SIGNAL(aboutToHide())), closed(popup, SIGNAL(closed())), visible(popup, SIGNAL(visibleChanged()));
        QVERIFY(hiding.isValid()); QVERIFY(closed.isValid()); QVERIFY(visible.isValid());
        const QStringList modes = {"", "/ ", "> "};
        const QStringList tabs = {"paletteBooks", "paletteSearch", "paletteCommands"};
        int mode = 0;
        for (int step = 0; step < 6; ++step) {
            // Populate Search too, so its result delegate can own focus.
            if (mode == 1) {
                type("alpha"); QTRY_VERIFY(prop("paletteSearchCurrent").toBool()); QTRY_VERIFY(!bible->searchBusy());
            }
            QQuickItem *target = focus == "currentItem"
                ? object("paletteResults")->property("currentItem").value<QQuickItem *>() : item(focus);
            QVERIFY(target); target->forceActiveFocus(); QTRY_VERIFY(target->hasActiveFocus());
            key(Qt::Key_Tab, direction > 0 ? Qt::NoModifier : Qt::ShiftModifier);
            mode = (mode + direction + 3) % 3;
            QCOMPARE(object("referenceInput")->property("text").toString(), modes[mode]);
            QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
            QVERIFY(popup->property("opened").toBool());
            for (int index = 0; index < tabs.size(); ++index)
                QCOMPARE(object(tabs[index])->property("emphasized").toBool(), index == mode);
        }
        QCOMPARE(hiding.count(), 0); QCOMPARE(closed.count(), 0); QCOMPARE(visible.count(), 0);
    }
    void paletteModeCommandsStayOpen_data() {
        QTest::addColumn<QString>("command"); QTest::addColumn<QString>("text"); QTest::addColumn<QString>("tab");
        QTest::newRow("search") << QString("Search Bible") << QString("/ ") << QString("paletteSearch");
        QTest::newRow("passage") << QString("Navigate to a passage") << QString() << QString("paletteBooks");
    }
    void paletteModeCommandsStayOpen() {
        QFETCH(QString, command); QFETCH(QString, text); QFETCH(QString, tab);
        key(Qt::Key_Space, Qt::ControlModifier);
        QTRY_VERIFY(object("passagePalette")->property("opened").toBool());
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        QCOMPARE(object("referenceInput")->property("text").toString(), QString("> "));
        type(command); QTRY_COMPARE(object("paletteResults")->property("count").toInt(), 1);
        auto popup = object("passagePalette");
        QSignalSpy hiding(popup, SIGNAL(aboutToHide())), closed(popup, SIGNAL(closed())), visible(popup, SIGNAL(visibleChanged()));
        QVERIFY(hiding.isValid()); QVERIFY(closed.isValid()); QVERIFY(visible.isValid());
        key(Qt::Key_Return);
        QTRY_COMPARE(object("referenceInput")->property("text").toString(), text);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        QTest::qWait(250); // Include deferred close/reopen and popup transitions.
        QVERIFY(popup->property("opened").toBool());
        QVERIFY(object(tab)->property("emphasized").toBool());
        QCOMPARE(hiding.count(), 0); QCOMPARE(closed.count(), 0); QCOMPARE(visible.count(), 0);
        type("John"); QCOMPARE(object("referenceInput")->property("text").toString(), text + "John");
    }
    void plainSearchResultHighlight() {
        search("bright"); QCOMPARE(bible->searchResultCount(), 2);
        QTRY_VERIFY(object("paletteResults")->property("currentItem").value<QQuickItem *>());
        auto row = object("paletteResults")->property("currentItem").value<QQuickItem *>();
        QTRY_VERIFY(visual(row, "searchHighlightsCanvas"));
        auto canvas = visual(row, "searchHighlightsCanvas");
        auto overlay = canvas->parentItem();
        auto body = overlay->property("body").value<QQuickItem *>();
        QVERIFY(body); QVERIFY(document(body));
        const auto result = bible->searchResults().first().toMap();
        const QString text = result.value("text").toString();
        QVERIFY(text.contains("& <bright>"));
        QCOMPARE(body->property("text").toString(), text);
        QCOMPARE(document(body)->toPlainText(), text);
        QCOMPARE(body->property("textFormat").toInt(), 0); // TextEdit.PlainText
        QVERIFY(body->property("readOnly").toBool());
        QVERIFY(!body->property("activeFocusOnPress").toBool());
        QCOMPARE(body->property("color"), desktop->property("foreground"));
        QVERIFY(overlay->property("outlined").toBool());
        for (auto block = document(body)->begin(); block.isValid(); block = block.next())
            for (auto it = block.begin(); !it.atEnd(); ++it)
                QVERIFY(!it.fragment().charFormat().fontUnderline());
        const auto siblings = body->parentItem()->childItems();
        QVERIFY(siblings.indexOf(overlay) < siblings.indexOf(body));
        const auto segments = value(overlay->property("segments")).toList();
        QVERIFY(!segments.isEmpty());
        const auto segment = segments.first().toMap();
        QCOMPARE(text.mid(segment.value("start").toInt(), segment.value("length").toInt()), QString("bright"));
        QVariant rect;
        QVERIFY(QMetaObject::invokeMethod(overlay, "rectFor", Q_RETURN_ARG(QVariant, rect),
            Q_ARG(QVariant, segment.value("start")), Q_ARG(QVariant, segment.value("length"))));
        QVERIFY(rect.toRectF().width() > 0);
        QTest::qWait(60);
        auto grab = canvas->grabToImage(); QVERIFY(grab);
        QSignalSpy ready(grab.get(), &QQuickItemGrabResult::ready);
        if (grab->image().isNull()) QVERIFY(ready.wait(3000));
        const auto image = grab->image(); QVERIFY(!image.isNull());
        const auto pixel = image.pixelColor(rect.toRectF().center().toPoint());
        const auto accent = desktop->property("accent").value<QColor>();
        QVERIFY(pixel.alpha() > 0);
        QVERIFY(qAbs(pixel.red() - accent.red()) <= 3);
        QVERIFY(qAbs(pixel.green() - accent.green()) <= 3);
        QVERIFY(qAbs(pixel.blue() - accent.blue()) <= 3);
        // A filled rectangle or underline alone must not satisfy the border regression.
        const auto bounds = rect.toRectF();
        const QList<QPointF> edges = {
            QPointF(bounds.center().x(), bounds.top() + 0.5),
            QPointF(bounds.center().x(), bounds.bottom() - 0.5),
            QPointF(bounds.left() + 0.5, bounds.center().y()),
            QPointF(bounds.right() - 0.5, bounds.center().y())
        };
        for (const auto &edge : edges) {
            int alpha = 0;
            // Fractional glyph geometry and the inset hand-drawn pen can spread the
            // stroke over neighbouring pixels.
            for (int dy = -2; dy <= 2; ++dy) for (int dx = -2; dx <= 2; ++dx) {
                const auto point = edge.toPoint() + QPoint(dx, dy);
                if (!image.rect().contains(point)) continue;
                const auto border = image.pixelColor(point);
                if (border.alpha() <= alpha) continue;
                alpha = border.alpha();
                // The pen edge is antialiased; faint pixels carry rounding noise.
                if (alpha < 128) continue;
                QVERIFY(qAbs(border.red() - accent.red()) <= 3);
                QVERIFY(qAbs(border.green() - accent.green()) <= 3);
                QVERIFY(qAbs(border.blue() - accent.blue()) <= 3);
            }
            QVERIFY2(alpha > pixel.alpha() + 50, "Every edge must have stronger accent ink than the fill");
        }
    }
    void searchQueryStatus() {
        QVERIFY(!window->findChild<QQuickItem *>("editionLabel"));
        QVERIFY(!item("searchQueryLabel")->isVisible());
        find("<bright>"); key(Qt::Key_Return);
        QTRY_VERIFY(item("searchQueryLabel")->isVisible());
        QCOMPARE(object("searchQueryLabel")->property("text").toString(), QString("/ <bright>"));
        QCOMPARE(object("searchQueryLabel")->property("textFormat").toInt(), 0);
        for (bool focus : {false, true}) {
            bible->setFocusMode(focus); QCoreApplication::processEvents();
            const auto query = item("searchQueryLabel")->mapRectToScene(item("searchQueryLabel")->boundingRect());
            const auto count = item("searchMatchCount")->mapRectToScene(item("searchMatchCount")->boundingRect());
            QVERIFY(query.left() < window->width() / 4);
            QVERIFY(query.top() > window->height() / 2);
            QVERIFY(query.right() < count.left());
            QVERIFY(qAbs(query.top() - count.top()) <= 1);
        }
        call("readerFocus"); key(Qt::Key_Escape);
        QVERIFY(!item("searchQueryLabel")->isVisible()); QVERIFY(bible->focusMode());
    }
    void alignVerseChords_data() { rendererNavigation_data(); }
    void alignVerseChords() {
        QFETCH(QString, mode); bible->setReadingMode(mode);
        navigate("John 1:35");
        const auto scope = bible->tagScope();
        const double available = item("reading")->height() - prop("floatingSafety").toDouble();
        for (const auto suffix : {Qt::Key_Z, Qt::Key_T, Qt::Key_B}) {
            const double before = settledY();
            key(Qt::Key_Z);
            QCOMPARE(settledY(), before);
            key(suffix); settledY();
            const auto rect = verseViewportRect(35);
            const double expected = suffix == Qt::Key_T ? 0 : suffix == Qt::Key_B
                ? available - rect.height() : (available - rect.height()) / 2;
            QVERIFY2(qAbs(rect.top() - expected) <= 1,
                qPrintable(QString("%1: top %2, expected %3").arg(mode).arg(rect.top()).arg(expected)));
            QCOMPARE(bible->tagScope(), scope); QCOMPARE(bible->cursorVerse(), 35);
            QCOMPARE(bible->readingMode(), mode);
            QVERIFY(!object("tagsUi")->property("visibleModal").toBool());
            QVERIFY(!window->property("relatedOpen").toBool());
        }
        // Single-key actions remain available after the chord has completed.
        key(Qt::Key_L, Qt::AltModifier); QVERIFY(bible->readingMode() != mode);
        key(Qt::Key_T); QTRY_VERIFY(item("tagSearch")->hasActiveFocus());
    }
    void alignmentInputGuardsAndCatalog() {
        navigate("John 1:35");
        const auto scope = bible->tagScope();
        for (const auto &context : {QString("notes"), QString("palette"), QString("find"), QString("keybindings")}) {
            if (context == "notes") key(Qt::Key_W);
            else if (context == "palette") key(Qt::Key_K, Qt::ControlModifier);
            else if (context == "find") key(Qt::Key_Slash);
            else key(Qt::Key_F1);
            auto input = item(context == "notes" ? "noteEditor" : context == "palette" ? "referenceInput"
                : context == "find" ? "chapterFindInput" : "keybindingSearch");
            QTRY_VERIFY2(input->hasActiveFocus(), qPrintable(context));
            const double y = settledY();
            type("zzztzb");
            QCOMPARE(input->property("text").toString(), QString("zzztzb"));
            QCOMPARE(settledY(), y);
            QCOMPARE(bible->tagScope(), scope); QCOMPARE(bible->readingMode(), QString("verse"));
            key(Qt::Key_Escape);
            // The palette's first esc clears its query; Notes' hands the keyboard to the reader.
            if (context == "palette" || context == "notes") key(Qt::Key_Escape);
            if (context == "notes") QTRY_VERIFY(!object("notesDrawer")->property("visible").toBool());
            else if (context == "palette") QTRY_VERIFY(!object("passagePalette")->property("visible").toBool());
            else if (context == "find") QTRY_VERIFY(!prop("findPromptOpen").toBool());
            else QTRY_VERIFY(!object("helpDialog")->property("visible").toBool());
        }
        key(Qt::Key_F1); QTRY_VERIFY(item("keybindingSearch")->hasActiveFocus());
        for (const auto &title : {QString("Center current verse"), QString("Scroll current verse to top"), QString("Scroll current verse to bottom")}) {
            key(Qt::Key_A, Qt::ControlModifier); type(title);
            QTRY_VERIFY(item("keybinding" + title));
            QVERIFY(item("keybinding" + title)->property("available").toBool());
        }
        key(Qt::Key_Return); QTRY_VERIFY(!object("helpDialog")->property("visible").toBool());
        QTRY_VERIFY(!object("readerSlide")->property("running").toBool());
        const auto rect = verseViewportRect(35);
        QVERIFY(qAbs(rect.bottom() - (item("reading")->height() - prop("floatingSafety").toDouble())) <= 1);
        QCOMPARE(bible->tagScope(), scope);
    }
    void firstSourceHeading_data() { rendererNavigation_data(); }
    void firstSourceHeading() {
        QFETCH(QString, mode); bible->setReadingMode(mode); bible->setShowHeadings(true);
        navigate("John 1:1");
        const auto heading = bible->verses().first().toMap().value("heading").toString();
        QCOMPARE(heading, QString("First source heading"));
        if (mode == "verse") {
            QTRY_VERIFY(item("verseHeading1") && item("verseHeading1")->height() > 0);
            QVERIFY(item("verseHeading1")->isVisible());
            QQuickItem *text = nullptr;
            for (auto child : item("verseHeading1")->childItems())
                if (child->property("text").toString().remove(QChar(0x200e)) == heading) text = child;
            QVERIFY(text); QVERIFY(text->isVisible()); QVERIFY(text->height() > 0);
        } else {
            QTRY_VERIFY(blockMapping() && item("blockBody") && document(item("blockBody")));
            QTRY_VERIFY(document(item("blockBody"))->toPlainText().startsWith(heading));
            QVERIFY(blockMapping()->positionFor(1) >= heading.size());
        }
    }
    void sourceOffsetNavigation_data() { rendererNavigation_data(); }
    void sourceOffsetNavigation() {
        QFETCH(QString, mode); bible->setReadingMode(mode); navigate("John 1:1");
        find("<bright>"); key(Qt::Key_Return);
        QCOMPARE(bible->cursorVerse(), 3);
        const auto segments = prop("searchSegments").toList(); QCOMPARE(segments.size(), 1);
        const auto segment = segments.first().toMap();
        const auto source = bible->verses().at(2).toMap().value("text").toString();
        const int start = source.indexOf("<bright>"); QVERIFY(start > 0);
        QCOMPARE(segment.value("start").toInt(), start);
        QCOMPARE(segment.value("length").toInt(), 8);
        QQuickItem *body = item(mode == "verse" ? "verseBody3" : "blockBody");
        QVERIFY(body); QVERIFY(document(body));
        const auto plain = document(body)->toPlainText();
        const int renderedStart = plain.indexOf("<bright>"); QVERIFY(renderedStart >= 0);
        QRectF highlight;
        if (mode == "verse") {
            QVariant rect;
            QVERIFY(QMetaObject::invokeMethod(item("verseSearch3"), "rectFor", Q_RETURN_ARG(QVariant, rect),
                Q_ARG(QVariant, start), Q_ARG(QVariant, 8)));
            highlight = rect.toRectF();
        } else highlight = blockMapping()->searchRect(segment);
        QVERIFY(highlight.width() > 0);
        const auto native = caret(body, renderedStart);
        QVERIFY(qAbs(highlight.x() - native.x()) <= 1);
        QVERIFY(qAbs(highlight.y() - native.y()) <= 1);
        const auto viewport = body->mapRectToItem(item("reading"), highlight);
        QVERIFY(viewport.top() >= 0); QVERIFY(viewport.bottom() <= item("reading")->height());
        const auto scope = bible->tagScope();
        key(Qt::Key_N); QCOMPARE(bible->tagScope(), scope);
        key(Qt::Key_N, Qt::ShiftModifier); QCOMPARE(bible->tagScope(), scope);
        QCOMPARE(document(body)->toPlainText(), plain);
    }
    void bibleQueriesAndResume() {
        const auto original = bible->tagScope();
        search("alpha beta"); QCOMPARE(bible->searchResultCount(), 8); QCOMPARE(bible->tagScope(), original);
        QVERIFY(!item("referencePreview")->isVisible());
        key(Qt::Key_Down); const int row = object("paletteResults")->property("currentIndex").toInt();
        // One esc closes with the query; space resumes it.
        key(Qt::Key_Escape); QTRY_VERIFY(!object("passagePalette")->property("visible").toBool());
        key(Qt::Key_Space); QTRY_COMPARE(object("referenceInput")->property("text").toString(), QString("/ alpha beta"));
        QTRY_VERIFY(prop("paletteSearchCurrent").toBool()); QTRY_VERIFY(!bible->searchBusy());
        QTRY_COMPARE(object("paletteResults")->property("currentIndex").toInt(), row);
        key(Qt::Key_Return); QTRY_VERIFY(!object("passagePalette")->property("visible").toBool());
        QCOMPARE(prop("acceptedSearchQuery").toString(), QString("alpha beta")); QVERIFY(prop("acceptedSearchBibleSyntax").toBool());
        QVERIFY(!prop("searchSegments").toList().isEmpty());
        call("clearSearch"); search("\"bridge begins here\""); QCOMPARE(bible->searchResultCount(), 2);
        key(Qt::Key_Return); QTRY_VERIFY(!object("passagePalette")->property("visible").toBool());
        QCOMPARE(bible->selectedStart(), 2); QCOMPARE(bible->selectedEnd(), 3);
        QCOMPARE(prop("searchSegments").toList().size(), 2);
    }
    void incompleteQuoteAndPagination() {
        search("\"unfinished"); QVERIFY(!bible->searchError().isEmpty()); QVERIFY(bible->error().isEmpty());
        QVERIFY(!object("errorDialog")->property("visible").toBool());
        key(Qt::Key_A, Qt::ControlModifier); type("/common"); QTRY_VERIFY(prop("paletteSearchCurrent").toBool()); QTRY_VERIFY(!bible->searchBusy());
        QCOMPARE(bible->searchResultCount(), 130); QCOMPARE(bible->searchResults().size(), 100);
        bible->loadMoreSearchResults(); QTRY_COMPARE(object("paletteResults")->property("count").toInt(), 130);
        object("paletteResults")->setProperty("currentIndex", 115);
        QVERIFY(QMetaObject::invokeMethod(object("paletteResults"), "positionViewAtIndex", Q_ARG(int, 115), Q_ARG(int, 2)));
        QTRY_VERIFY(object("paletteResults")->property("contentY").toDouble() > 0);
        key(Qt::Key_Escape); QTRY_VERIFY(!object("passagePalette")->property("visible").toBool());
        key(Qt::Key_Space); QTRY_VERIFY(prop("paletteSearchCurrent").toBool()); QTRY_VERIFY(!bible->searchBusy());
        QTRY_COMPARE(object("paletteResults")->property("currentIndex").toInt(), 115);
        QTRY_VERIFY(object("paletteResults")->property("contentY").toDouble() > 0);
        key(Qt::Key_K, Qt::ControlModifier); QCOMPARE(object("referenceInput")->property("text").toString(), QString());
        QVERIFY(!prop("paletteTextMode").toBool());
    }
    void resultsInBibleOrderUnderBooks() {
        // Genesis (120 matches, more than a page), Exodus (2), Matthew (1).
        const QString path = session->filePath("books.json");
        {
            QJsonArray books;
            auto add = [&](const char *code, const char *title, int verses, QList<int> lamps) {
                QJsonArray content;
                for (int number = 1; number <= verses; ++number)
                    content.append(AoFixture::verse(number, {QString("Original line %1%2.").arg(number).arg(lamps.contains(number) || lamps.contains(0) ? " with a lamp" : "")}));
                books.append(AoFixture::book(code, books.size() + 1, {AoFixture::chapter(1, content)}, title));
            };
            add("GEN", "Genesis", 120, {0});
            add("EXO", "Exodus", 10, {3, 7});
            add("MAT", "Matthew", 5, {2});
            QVERIFY(AoFixture::write(path, AoFixture::complete("books", books)));
        }
        install(path); navigate("Matthew 1:1");
        search("lamp");
        QCOMPARE(bible->searchResultCount(), 123); QCOMPARE(bible->searchResults().size(), 100);
        const auto books = bible->searchBooks();
        QCOMPARE(books.size(), 3);
        QCOMPARE(books[0].toMap()["name"].toString(), QString("Genesis")); QCOMPARE(books[0].toMap()["count"].toInt(), 120);
        QCOMPARE(books[1].toMap()["name"].toString(), QString("Exodus")); QCOMPARE(books[1].toMap()["index"].toInt(), 120);
        QCOMPARE(books[2].toMap()["name"].toString(), QString("Matthew")); QCOMPARE(books[2].toMap()["count"].toInt(), 1);
        // Bible order, not the reader's book first; each book's first row is headed.
        QCOMPARE(bible->searchResults()[0].toMap()["reference"].toString(), QString("Genesis 1:1"));
        QCOMPARE(bible->searchResults()[1].toMap()["reference"].toString(), QString("Genesis 1:2"));
        QTRY_VERIFY(item("paletteGroup0") && item("paletteGroup0")->isVisible());
        QCOMPARE(object("paletteGroup0")->property("text").toString(), QString("Genesis · 120"));
        QVERIFY(object("searchResultCount")->property("text").toString().endsWith(" · 3 books"));
        QVERIFY(object("paletteHints")->property("plainText").toString().contains("ctrl ↑ ctrl ↓ books"));
        // ctrl ↓ jumps to the next book, loading the page it is on.
        key(Qt::Key_Down, Qt::ControlModifier);
        QCOMPARE(object("paletteResults")->property("currentIndex").toInt(), 120);
        QVERIFY(bible->searchResults().size() > 120);
        QTRY_VERIFY(item("paletteGroup120") && item("paletteGroup120")->isVisible());
        QCOMPARE(object("paletteGroup120")->property("text").toString(), QString("Exodus · 2"));
        key(Qt::Key_Down); QCOMPARE(object("paletteResults")->property("currentIndex").toInt(), 121);
        // ctrl ↑ goes to this book's first result, then the previous book's.
        key(Qt::Key_Up, Qt::ControlModifier); QCOMPARE(object("paletteResults")->property("currentIndex").toInt(), 120);
        key(Qt::Key_Up, Qt::ControlModifier); QCOMPARE(object("paletteResults")->property("currentIndex").toInt(), 0);
        key(Qt::Key_Down, Qt::ControlModifier); key(Qt::Key_Down, Qt::ControlModifier);
        QCOMPARE(object("paletteResults")->property("currentIndex").toInt(), 122);
        // Past the last book: the boundary sound, nowhere to go.
        key(Qt::Key_Down, Qt::ControlModifier); QCOMPARE(object("paletteResults")->property("currentIndex").toInt(), 122);
        QCOMPARE(sounds->lastSound(), QString("boundary"));
        key(Qt::Key_Return); QTRY_VERIFY(!object("passagePalette")->property("visible").toBool());
        QCOMPARE(bible->selectionReference(), QString("Matthew 1:2"));
    }
    void previewCancelAndVisualNormal() {
        find("alpha"); key(Qt::Key_Return); QTRY_VERIFY(!prop("findPromptOpen").toBool());
        navigate("John 1:35");
        const auto scope = bible->tagScope(); const auto y = settledY();
        const auto height = item("reading")->height();
        find("beta"); QCOMPARE(bible->tagScope(), scope); QCOMPARE(item("reading")->height(), height);
        QVERIFY(settledY() != y);
        key(Qt::Key_Escape); QCOMPARE(bible->tagScope(), scope); QCOMPARE(prop("acceptedSearchQuery").toString(), QString("alpha"));
        QCOMPARE(settledY(), y);
        key(Qt::Key_V, Qt::ShiftModifier); key(Qt::Key_K, Qt::ShiftModifier); QVERIFY(prop("verseMode").toBool()); const int cursor = bible->cursorVerse();
        key(Qt::Key_Slash); QTRY_VERIFY(prop("findPromptOpen").toBool()); QVERIFY(!prop("verseMode").toBool());
        QCOMPARE(bible->selectedStart(), cursor); QCOMPARE(bible->selectedEnd(), cursor);
        QCOMPARE(object("chapterFindInput")->property("selectedText").toString(), QString("alpha"));
        type("unmatched"); QTest::qWait(180); key(Qt::Key_Escape);
        QCOMPARE(bible->cursorVerse(), cursor); QVERIFY(!prop("verseMode").toBool());
    }
    void countedNavigationAndTypingGuards() {
        find("beta"); key(Qt::Key_Return); QCOMPARE(bible->cursorVerse(), 2);
        key(Qt::Key_3); key(Qt::Key_N); QCOMPARE(bible->cursorVerse(), 50); QCOMPARE(prop("verseCount").toString(), QString());
        key(Qt::Key_N); QCOMPARE(bible->cursorVerse(), 50); QCOMPARE(object("toast")->property("text").toString(), QString("No matches after cursor"));
        key(Qt::Key_9); key(Qt::Key_N, Qt::ShiftModifier); QCOMPARE(bible->cursorVerse(), 2);
        QCOMPARE(object("toast")->property("text").toString(), QString("No matches before cursor"));
        key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus()); type("nw/s");
        QCOMPARE(object("noteEditor")->property("text").toString(), QString("nw/s"));
        QVERIFY(!prop("findPromptOpen").toBool()); QVERIFY(!object("passagePalette")->property("visible").toBool());
        // esc in the note closes Notes, keeping the search; from the reader
        // (ctrl w) too, esc closes Notes before it clears the search.
        key(Qt::Key_Escape); QTRY_VERIFY(!object("notesDrawer")->property("visible").toBool());
        QTRY_VERIFY(item("readerSurface")->hasActiveFocus());
        QVERIFY(!prop("acceptedSearchQuery").toString().isEmpty());
        key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        key(Qt::Key_W, Qt::ControlModifier); QTRY_VERIFY(item("readerSurface")->hasActiveFocus());
        key(Qt::Key_Escape); QTRY_VERIFY(!object("notesDrawer")->property("visible").toBool());
        QVERIFY(!prop("acceptedSearchQuery").toString().isEmpty());
        key(Qt::Key_W); QTRY_VERIFY(object("notesDrawer")->property("visible").toBool());
    }
    void chapterFindWithoutForwardMatch_data() {
        QTest::addColumn<QString>("query");
        QTest::addColumn<int>("matches");
        QTest::newRow("all-before-cursor") << QString("beta") << 4;
        QTest::newRow("absent-query") << QString("unmatched") << 0;
        QTest::newRow("empty-query") << QString() << 0;
    }
    void chapterFindWithoutForwardMatch() {
        QFETCH(QString, query); QFETCH(int, matches);
        find("alpha"); key(Qt::Key_Return);
        navigate("John 1:60");
        const auto scope = bible->tagScope();
        const auto acceptedCount = object("searchMatchCount")->property("text");
        const double y = settledY();
        key(Qt::Key_Slash); QTRY_VERIFY(item("chapterFindInput")->hasActiveFocus());
        QCOMPARE(object("chapterFindInput")->property("selectedText").toString(), QString("alpha"));
        key(Qt::Key_Backspace); type(query);
        QTRY_COMPARE(prop("searchMatches").toList().size(), matches);
        QCOMPARE(prop("searchMatchIndex").toInt(), -1);
        QVERIFY(prop("searchActiveMatch").isNull());
        QCOMPARE(prop("searchSegments").toList().size(), matches);
        for (const auto &segment : prop("searchSegments").toList()) QVERIFY(!segment.toMap().value("active").toBool());
        if (matches) QCOMPARE(object("chapterFindCount")->property("text").toString(), QString("No matches after cursor"));
        key(Qt::Key_Return);
        if (matches) {
            QTRY_VERIFY(!prop("findPromptOpen").toBool());
            QCOMPARE(bible->cursorVerse(), 2);
            QCOMPARE(prop("searchMatchIndex").toInt(), 0);
            QCOMPARE(prop("acceptedSearchQuery").toString(), query);
            QCOMPARE(prop("chapterQueryHistory").toString(), query);
            QVERIFY(prop("searchError").toString().isEmpty());
            key(Qt::Key_N, Qt::ShiftModifier);
            QCOMPARE(bible->cursorVerse(), 2);
            QCOMPARE(object("toast")->property("text").toString(), QString("No matches before cursor"));
            key(Qt::Key_9); key(Qt::Key_N); QCOMPARE(bible->cursorVerse(), 50);
            key(Qt::Key_N); QCOMPARE(bible->cursorVerse(), 50);
            QCOMPARE(object("toast")->property("text").toString(), QString("No matches after cursor"));
            return;
        }
        QVERIFY(prop("findPromptOpen").toBool()); QVERIFY(item("chapterFindInput")->hasActiveFocus());
        QCOMPARE(bible->tagScope(), scope); QCOMPARE(bible->cursorVerse(), 60);
        QCOMPARE(settledY(), y);
        QCOMPARE(prop("acceptedSearchQuery").toString(), QString("alpha"));
        QCOMPARE(prop("chapterQueryHistory").toString(), QString("alpha"));
        key(Qt::Key_Escape); QVERIFY(!prop("findPromptOpen").toBool());
        QCOMPARE(bible->tagScope(), scope); QCOMPARE(settledY(), y);
        QCOMPARE(object("searchMatchCount")->property("text"), acceptedCount);
    }
    void chapterFindStartsAtOrAfterCursor_data() {
        QTest::addColumn<int>("cursor"); QTest::addColumn<int>("matchVerse"); QTest::addColumn<int>("nextVerse");
        QTest::newRow("at-cursor") << 8 << 8 << 20;
        QTest::newRow("after-cursor") << 9 << 20 << 50;
    }
    void chapterFindStartsAtOrAfterCursor() {
        QFETCH(int, cursor); QFETCH(int, matchVerse); QFETCH(int, nextVerse);
        navigate(QString("John 1:%1").arg(cursor));
        find("beta"); QCOMPARE(bible->cursorVerse(), cursor);
        QCOMPARE(prop("searchActiveMatch").toMap().value("start").toInt(), matchVerse);
        const int index = prop("searchMatchIndex").toInt(); QVERIFY(index >= 0);
        key(Qt::Key_Return); QTRY_VERIFY(!prop("findPromptOpen").toBool());
        QCOMPARE(bible->cursorVerse(), matchVerse); QCOMPARE(prop("searchMatchIndex").toInt(), index);
        QCOMPARE(prop("searchMatchIdentity"), prop("selectionIdentity"));
        QCOMPARE(prop("searchActiveMatch").toMap().value("start").toInt(), matchVerse);
        QVERIFY(prop("searchSegments").toList().at(index).toMap().value("active").toBool());
        key(Qt::Key_N); QCOMPARE(bible->cursorVerse(), nextVerse);
        QCOMPARE(prop("searchMatchIndex").toInt(), index + 1);
    }
    void acceptedBibleEscapeBeforeFocusMode() {
        search("\"bridge begins here\""); key(Qt::Key_Return);
        QTRY_VERIFY(!object("passagePalette")->property("visible").toBool());
        QVERIFY(prop("acceptedSearchBibleSyntax").toBool()); QVERIFY(prop("searchMatchIndex").toInt() >= 0);
        QCOMPARE(prop("searchMatchIdentity"), prop("selectionIdentity"));
        const auto scope = bible->tagScope(); QCOMPARE(bible->selectedStart(), 2); QCOMPARE(bible->selectedEnd(), 3);
        bible->setFocusMode(true); QTRY_VERIFY(window->property("footerQuiet").toBool()); call("readerFocus");
        QVERIFY(!prop("searchSegments").toList().isEmpty());
        key(Qt::Key_Escape);
        QVERIFY(bible->focusMode()); QVERIFY(prop("acceptedSearchQuery").toString().isEmpty());
        QVERIFY(prop("searchSegments").toList().isEmpty()); QCOMPARE(bible->tagScope(), scope);
        // The accepted multi-verse match collapses to the cursor next, then
        // Escape leaves focus mode.
        key(Qt::Key_Escape); QVERIFY(bible->focusMode()); QCOMPARE(bible->selectedStart(), bible->selectedEnd());
        key(Qt::Key_Escape); QVERIFY(!bible->focusMode());
    }
    // alt a: Bible search in every installed version, the one being read
    // first, each under its own headings; a result elsewhere opens there.
    void searchEveryVersion() {
        install(second); const QString kjv = bible->versionId(), kjvName = bible->versionAbbreviation();
        QVERIFY(bible->switchVersion("engnet")); navigate("John 1:1");
        QVERIFY(!bible->searchAllVersions());
        // "Different words" is only in the other edition.
        search("different words"); QCOMPARE(bible->searchResultCount(), 0);
        QTRY_VERIFY(object("paletteHints")->property("plainText").toString().contains("alt a all versions"));
        key(Qt::Key_A, Qt::AltModifier);
        QTRY_VERIFY(bible->searchAllVersions());
        QCOMPARE(object("toast")->property("text").toString(), QString("Searching every version"));
        QTRY_VERIFY(prop("paletteSearchCurrent").toBool()); QTRY_VERIFY(!bible->searchBusy());
        QTRY_COMPARE(bible->searchResultCount(), 6);
        QTRY_VERIFY(object("paletteHints")->property("plainText").toString().contains("alt a " + bible->versionAbbreviation() + " only"));
        const auto rows = value(object("paletteResults")->property("model")).toList();
        QCOMPARE(rows.first().toMap()["group"].toString(), kjvName + QStringLiteral(" · John · 6"));
        QCOMPARE(rows.first().toMap()["versionId"].toString(), kjv);
        QVERIFY(object("searchResultCount")->property("text").toString().endsWith("1 book"));
        // Both editions: the one being read comes first, under its own headings.
        key(Qt::Key_Escape); QTRY_VERIFY(!object("passagePalette")->property("visible").toBool());
        search("common original fixture 1:2."); QTRY_COMPARE(bible->searchResultCount(), 2);
        QTRY_VERIFY(object("searchResultCount")->property("text").toString().endsWith("2 books in 2 versions"));
        auto both = value(object("paletteResults")->property("model")).toList();
        QCOMPARE(both[0].toMap()["versionId"].toString(), QString("engnet")); QCOMPARE(both[1].toMap()["versionId"].toString(), kjv);
        QVERIFY(both[1].toMap()["firstOfGroup"].toBool());
        // ctrl ↓ steps to the next version's book.
        key(Qt::Key_Down, Qt::ControlModifier); QCOMPARE(object("paletteResults")->property("currentIndex").toInt(), 1);
        // Opening it switches to its version.
        key(Qt::Key_Return); QTRY_VERIFY(!object("passagePalette")->property("visible").toBool());
        QCOMPARE(bible->versionId(), kjv); QCOMPARE(bible->selectionReference(), QString("John 1:2"));
        // The choice is kept; alt a again searches the one being read only.
        { BibleBackend reopened; QVERIFY(reopened.searchAllVersions()); }
        search("different words"); QTRY_COMPARE(bible->searchResultCount(), 6);
        key(Qt::Key_A, Qt::AltModifier); QTRY_VERIFY(!bible->searchAllVersions());
        QTRY_VERIFY(!bible->searchBusy()); QTRY_COMPARE(bible->searchResultCount(), 6);
        QCOMPARE(object("toast")->property("text").toString(), "Searching " + kjvName + " only");
        key(Qt::Key_Escape); QTRY_VERIFY(!object("passagePalette")->property("visible").toBool());
    }
    // A query in slashes is a regular expression, matched within each verse,
    // as in the terminal; anything else is words (there is no switch).
    void searchWithARegularExpression() {
        navigate("John 1:1");
        // alt a acts only on a Bible search in the palette: not in the
        // reader, nor in the palette's other modes.
        install(second); QVERIFY(bible->switchVersion("engnet")); navigate("John 1:1");
        const QString before = object("toast")->property("text").toString();
        call("readerFocus");
        key(Qt::Key_A, Qt::AltModifier); QVERIFY(!bible->searchAllVersions());
        key(Qt::Key_K, Qt::ControlModifier); QTRY_VERIFY(object("passagePalette")->property("opened").toBool());
        key(Qt::Key_A, Qt::AltModifier); QVERIFY(!bible->searchAllVersions());
        QCOMPARE(object("toast")->property("text").toString(), before);
        key(Qt::Key_Escape); QTRY_VERIFY(!object("passagePalette")->property("visible").toBool());
        // Commands has no switch for regular expressions.
        key(Qt::Key_Space, Qt::ControlModifier); QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("regular expression"); QTRY_VERIFY(value(object("paletteResults")->property("model")).toList().isEmpty());
        key(Qt::Key_Escape); QTRY_VERIFY(!object("passagePalette")->property("visible").toBool());
        const auto segments = [&](int row) { return bible->searchResults()[row].toMap()["segments"].toList().size(); };
        // "alpha" as a word: ALPHA in 1:2 (not alphabet), both alphas in 1:8.
        search("/alpha\\b/"); QTRY_COMPARE(bible->searchResultCount(), 8);
        QCOMPARE(bible->searchResults().first().toMap()["reference"].toString(), QString("John 1:2"));
        QCOMPARE(segments(0), 1); QCOMPARE(segments(1), 2);
        QVERIFY(!object("paletteHints")->property("plainText").toString().contains("regex"));
        // Choosing a verse shows the same matches in the reader: a verse each
        // in its chapter, the one chosen current.
        key(Qt::Key_Return); QTRY_VERIFY(!object("passagePalette")->property("visible").toBool());
        QCOMPARE(bible->selectionReference(), QString("John 1:2"));
        QTRY_COMPARE(prop("searchMatches").toList().size(), 4); QCOMPARE(prop("searchMatchIndex").toInt(), 0);
        key(Qt::Key_N); QCOMPARE(bible->cursorVerse(), 8);
        // Without the slashes the same text is words.
        search("alpha\\b"); QCOMPARE(bible->searchResultCount(), 0);
        key(Qt::Key_Escape); QTRY_VERIFY(!object("passagePalette")->property("visible").toBool());
        // A broken pattern says why.
        search("/alpha(/"); QVERIFY2(bible->searchError().startsWith("Invalid regular expression"), qPrintable(bible->searchError()));
        key(Qt::Key_Escape); QTRY_VERIFY(!object("passagePalette")->property("visible").toBool());
    }
    // Words use smart case, as regular expressions do: a word with a capital
    // matches its case, in the results and in the reader after choosing one.
    void wordsUseSmartCase() {
        navigate("John 1:1");
        search("alpha"); QTRY_COMPARE(bible->searchResultCount(), 8);
        key(Qt::Key_Escape); QTRY_VERIFY(!object("passagePalette")->property("visible").toBool());
        search("ALPHA"); QTRY_COMPARE(bible->searchResultCount(), 2);
        key(Qt::Key_Escape); QTRY_VERIFY(!object("passagePalette")->property("visible").toBool());
        search("ALPHA\\c"); QTRY_COMPARE(bible->searchResultCount(), 8);
        key(Qt::Key_Escape); QTRY_VERIFY(!object("passagePalette")->property("visible").toBool());
        // Alpha: 1:8, 1:20, and 1:50 in each chapter, not ALPHA or alphabet.
        search("Alpha"); QTRY_COMPARE(bible->searchResultCount(), 6);
        QCOMPARE(bible->searchResults().first().toMap()["reference"].toString(), QString("John 1:8"));
        QCOMPARE(bible->searchResults().first().toMap()["segments"].toList().size(), 1);
        key(Qt::Key_Return); QTRY_VERIFY(!object("passagePalette")->property("visible").toBool());
        QCOMPARE(bible->selectionReference(), QString("John 1:8"));
        // In the reader, only Alpha is marked: one a verse, never "another alpha".
        QTRY_COMPARE(prop("searchMatches").toList().size(), 3);
        for (const auto &match : prop("searchMatches").toList()) QCOMPARE(match.toMap()["segments"].toList().size(), 1);
    }
    void focusEscapeAndVersionInvalidation() {
        find("alpha"); key(Qt::Key_Return); bible->setFocusMode(true); call("readerFocus");
        QTRY_VERIFY(item("searchMatchCount")->isVisible());
        key(Qt::Key_Slash); QTRY_VERIFY(item("chapterFindInput")->hasActiveFocus()); key(Qt::Key_Escape);
        QVERIFY(bible->focusMode()); QVERIFY(!prop("acceptedSearchQuery").toString().isEmpty());
        key(Qt::Key_Escape); QVERIFY(bible->focusMode()); QVERIFY(prop("searchSegments").toList().isEmpty());
        key(Qt::Key_Escape); QVERIFY(!bible->focusMode());
        find("alpha"); key(Qt::Key_Return); const QString version = bible->versionId();
        navigate("John 1:35"); install(second); QTRY_VERIFY(prop("searchSegments").toList().isEmpty());
        QVERIFY(bible->switchVersion(version)); QTRY_VERIFY(!prop("searchSegments").toList().isEmpty());
        QCOMPARE(bible->cursorVerse(), 35); QCOMPARE(prop("searchMatchIndex").toInt(), -1);
        QCOMPARE(object("searchMatchCount")->property("text").toString(), QString("8 matches"));
    }
    void rendererNavigation_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("verse") << QString("verse");
        QTest::newRow("block") << QString("block");
        QTest::newRow("paragraph") << QString("paragraph");
    }
    void rendererNavigation() {
        QFETCH(QString, mode); bible->setReadingMode(mode);
        QTRY_VERIFY(!prop("restoringPosition").toBool());
        navigate("John 1:35"); find("beta");
        QCOMPARE(bible->cursorVerse(), 35); QCOMPARE(prop("searchActiveMatch").toMap().value("start").toInt(), 50);
        QVERIFY(settledY() > 0);
        key(Qt::Key_Return); QCOMPARE(bible->cursorVerse(), 50);
        key(Qt::Key_N, Qt::ShiftModifier); QCOMPARE(bible->cursorVerse(), 20);
        navigate("John 2:35"); QTRY_VERIFY(!prop("searchSegments").toList().isEmpty());
        QCOMPARE(bible->cursorVerse(), 35); QCOMPARE(prop("searchMatchIndex").toInt(), -1);
        key(Qt::Key_N); QCOMPARE(bible->cursorVerse(), 50);
    }
    void guardsAndLiteralSyntax() {
        // Smart case: an uppercase letter matches case, \c ignores it.
        find("ALPh"); QCOMPARE(prop("searchMatches").toList().size(), 0); key(Qt::Key_Escape);
        find("ALPh\\c"); QVERIFY(prop("searchMatches").toList().size() > 4); key(Qt::Key_Return);
        find("\"alpha\""); QCOMPARE(prop("searchMatches").toList().size(), 0); QVERIFY(prop("searchError").toString().isEmpty());
        key(Qt::Key_Escape);
        search("beta"); const auto original = bible->tagScope(); window->setProperty("noteConflict", true);
        object("noteEditor")->setProperty("text", "Unsaved conflict draft");
        key(Qt::Key_Return); QVERIFY(object("passagePalette")->property("visible").toBool());
        QCOMPARE(bible->tagScope(), original); QCOMPARE(object("referenceInput")->property("text").toString(), QString("/ beta"));
        QCOMPARE(prop("acceptedSearchQuery").toString(), QString("ALPh\\c"));
    }
};

int main(int argc, char **argv) {
    QQuickStyle::setStyle("Basic"); QApplication app(argc, argv);
    SearchUiTest test; return QTest::qExec(&test, argc, argv);
}
#include "tst_searchui.moc"
