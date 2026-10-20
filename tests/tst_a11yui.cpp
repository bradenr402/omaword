#include "testenvironment.h"
#include <QtTest/QTest>
#include <QtQuickTest/quicktest.h>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QFile>
#include <QDir>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QJSValue>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStyleHints>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTemporaryDir>
#include <QUuid>
#include <functional>
#include "aofixture.h"
#include "biblebackend.h"
#include "blockdocument.h"
#include "desktop.h"
#include "soundeffects.h"
#include <tuple>

class MockCatalog : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList entries MEMBER entries NOTIFY changed)
    Q_PROPERTY(bool busy MEMBER busy NOTIFY changed)
    Q_PROPERTY(double progress MEMBER progress NOTIFY changed)
    Q_PROPERTY(QString error MEMBER error NOTIFY changed)
public:
    QVariantList entries;
    bool busy = false;
    double progress = 0;
    QString error;
    QStringList requests;
    Q_INVOKABLE void download(const QString &id) { requests << id; busy = true; emit changed(); }
    Q_INVOKABLE void cancel() { busy = false; emit changed(); }
    Q_INVOKABLE void clearError() { error.clear(); emit changed(); }
signals:
    void changed();
    void downloadFinished(const QUrl &url);
};

// Keyboard and mouse access across the app: buttons, clickable key hints,
// row menus, scroll keys, outside clicks, and the Phase 2 reader features.
// Every feature is exercised by key and by pointer.
class A11yUiTest : public QObject
{
    Q_OBJECT
    QTemporaryDir fixtures;
    std::unique_ptr<QTemporaryDir> session;
    std::unique_ptr<BibleBackend> bible;
    std::unique_ptr<Desktop> desktop;
    std::unique_ptr<SoundEffects> sounds;
    std::unique_ptr<MockCatalog> catalog;
    std::unique_ptr<QQmlApplicationEngine> engine;
    QQuickWindow *window = nullptr;
    QStringList warnings;
    QString source;

    QString installedFile() const {
        QString path;
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "access-installed");
            db.setDatabaseName(bible->dataDirectory() + "/personal.sqlite");
            if (db.open()) {
                QSqlQuery q(db); q.prepare("SELECT path FROM installed WHERE version_id=?"); q.addBindValue(bible->versionId());
                if (q.exec() && q.next()) path = bible->dataDirectory() + '/' + q.value(0).toString();
            }
        }
        QSqlDatabase::removeDatabase("access-installed");
        return path;
    }
    static bool edition(const QString &path) {
        auto book = [](const char *code, const char *name, int chapterCount, int verses) {
            QJsonArray chapters;
            for (int c = 1; c <= chapterCount; ++c) {
                QJsonArray content{QJsonObject{{"type", "heading"}, {"content", QJsonArray{"Access fixture heading"}}}}, notes;
                for (int v = 1; v <= verses; ++v) {
                    if (v == 20) content.append(QJsonObject{{"type", "heading"}, {"content", QJsonArray{"A later heading"}}});
                    QJsonArray pieces{QString("Original access text %1:%2 with enough ordinary words to wrap across a reading line.").arg(c).arg(v)};
                    if (QString(code) == "JHN" && c == 1 && v == 1) pieces = {"In the beginning was the Word. Original words follow."};
                    if (QString(code) == "JHN" && c == 3 && (v == 3 || v == 16)) {
                        pieces = {v == 3 ? "Jesus answered," : "For God so loved", QJsonObject{{"noteId", v}}, v == 3 ? " truly unless one is born again." : " the world that he gave his Son."};
                        notes.append(QJsonObject{{"noteId", v}, {"caller", v == 3 ? "+" : "a"}, {"text", v == 3 ? "3:3 Or from above" : "Greek agapao"}});
                    }
                    content.append(AoFixture::verse(v, pieces));
                }
                chapters.append(AoFixture::chapter(c, content, notes));
            }
            return AoFixture::book(code, QString(code) == "GEN" ? 1 : 2, chapters, name);
        };
        return AoFixture::write(path, AoFixture::complete("access", {book("GEN", "Genesis", 2, 31), book("JHN", "John", 3, 36)}));
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
    QVariant call(const char *method, const QVariant &a = {}, const QVariant &b = {}) {
        QVariant result; bool ok;
        if (!b.isValid() && !a.isValid()) ok = QMetaObject::invokeMethod(window, method, Q_RETURN_ARG(QVariant, result));
        else if (!b.isValid()) ok = QMetaObject::invokeMethod(window, method, Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, a));
        else ok = QMetaObject::invokeMethod(window, method, Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, a), Q_ARG(QVariant, b));
        if (!ok) qWarning() << "invoke failed" << method;
        QCoreApplication::processEvents();
        return value(result);
    }
    void run(const QString &command) { call("runCommand", command); }
    void key(Qt::Key key, Qt::KeyboardModifiers mods = Qt::NoModifier) { QTest::keyClick(window, key, mods); QCoreApplication::processEvents(); }
    void type(const QString &text) { for (const auto c : text) QTest::keyClick(window, c.toLatin1()); QCoreApplication::processEvents(); }
    QPoint center(QQuickItem *target) const { return target->mapToScene(QPointF(target->width() / 2, target->height() / 2)).toPoint(); }
    void click(QQuickItem *target, Qt::MouseButton button = Qt::LeftButton, Qt::KeyboardModifiers mods = {}) {
        QVERIFY(target); QTest::mouseClick(window, button, mods, center(target)); QCoreApplication::processEvents();
    }
    void click(const QString &name, Qt::MouseButton button = Qt::LeftButton) {
        auto target = item(name); QVERIFY2(target, qPrintable(name)); QVERIFY2(target->isVisible(), qPrintable(name)); click(target, button);
    }
    void clickAt(const QPoint &point, Qt::MouseButton button = Qt::LeftButton) { QTest::mouseClick(window, button, {}, point); QCoreApplication::processEvents(); }
    void install(const QString &file) {
        bible->importAo(QUrl::fromLocalFile(file)); QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(), 30000);
        QVERIFY2(bible->ready() && bible->error().isEmpty(), qPrintable(bible->error()));
    }
    void navigate(const QString &reference) {
        QVERIFY(bible->navigate(reference)); call("readerFocus"); QTRY_VERIFY(!prop("restoringPosition").toBool());
    }
    void setMode(const QString &mode) {
        bible->setReadingMode(mode); QTRY_COMPARE(prop("continuousReader").toBool(), mode != "verse");
        QTest::qWait(50); QCoreApplication::processEvents();
    }
    QString focusName() const { return window->activeFocusItem() ? window->activeFocusItem()->objectName() : QString(); }
    bool focusWithin(const QString &name) const {
        for (auto at = window->activeFocusItem(); at; at = at->parentItem()) if (at->objectName() == name) return true;
        return false;
    }
    bool readerFocused() const { return window->activeFocusItem() == item("readerSurface"); }
    // esc in Notes hands the keyboard back to the reader, Notes still open;
    // esc there closes it.
    // esc in a panel closes the Margin, the reader taking the keyboard.
    void escapeNotes() {
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("notesDrawer")); QTRY_VERIFY(readerFocused());
    }
    bool opened(const QString &name) const { auto o = object(name); return o && o->property("opened").toBool(); }
    bool shown(const QString &name) const { auto o = object(name); return o && o->property("visible").toBool(); }
    // A side panel in the Margin (marginMode): on its mode and done sliding
    // in; still showing it (its exit included).
    bool panelOpened(const QString &mode) const { return prop("marginMode").toString() == mode && opened("notesDrawer"); }
    bool panelShown(const QString &mode) const { return prop("marginMode").toString() == mode && shown("notesDrawer"); }
    void closeMargin() { escapeNotes(); }
    QObject *menu() const { return object("contextMenu"); }
    QStringList menuIds() const {
        QStringList ids;
        for (const auto &entry : value(menu()->property("layout")).toList()) {
            const auto map = entry.toMap();
            ids << (map.value("separator").toBool() ? QString("|") : map.value("id").toString());
        }
        return ids;
    }
    QString menuShortcut(const QString &id) const {
        for (const auto &entry : value(menu()->property("layout")).toList()) if (entry.toMap().value("id") == id) return entry.toMap().value("shortcut").toString();
        return "?";
    }
    bool menuEnabled(const QString &id) const {
        for (const auto &entry : value(menu()->property("layout")).toList()) if (entry.toMap().value("id") == id) return entry.toMap().value("enabled").toBool();
        return false;
    }
    void chooseMenu(const QString &id) {
        QTRY_VERIFY(menu()->property("opened").toBool());
        QTRY_VERIFY(item("contextMenuContent") && item("contextMenuContent")->height() > 0);
        auto row = item("contextMenuItem:" + id); QVERIFY2(row, qPrintable(id)); QVERIFY(row->isVisible());
        // Under load a reused menu may still be laying out its rows: wait until it settles.
        QPointF last(-1, -1);
        QTRY_VERIFY([&] { QTest::qWait(20); const auto at = row->mapToScene(QPointF()); const bool settled = at == last; last = at; return settled; }());
        click(row); QTRY_VERIFY(!menu()->property("visible").toBool());
    }
    QString lastSound() const { return sounds->property("lastSound").toString(); }
    int playCount() const { return sounds->property("playCount").toInt(); }
    QString toast() const { return object("toast")->property("text").toString(); }
    QObject *toolTipOf(QQuickItem *owner) const {
        for (auto child : owner->findChildren<QObject *>())
            if (QByteArray(child->metaObject()->className()).startsWith("AppToolTip")) return child;
        return nullptr;
    }
    QPoint versePoint(int verse) {
        auto body = item("verseBody" + QString::number(verse)); if (!body) return {};
        return body->mapToScene(QPointF(30, 8)).toPoint();
    }

    void wheel(QQuickItem *target, int delta) {
        const QPointF local(target->width() / 2, target->height() / 2);
        const QPointF scene = target->mapToScene(local);
        QWheelEvent event(scene, scene, QPoint(), QPoint(0, delta), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QGuiApplication::sendEvent(window, &event); QCoreApplication::processEvents();
    }
    qreal contentY(const QString &scroll) const {
        auto view = object(scroll); if (!view) return -1;
        auto flick = view->property("contentItem").value<QObject *>();
        return flick ? flick->property("contentY").toReal() : view->property("contentY").toReal();
    }
    qreal travel(const QString &scroll) const {
        auto view = object(scroll); auto flick = view->property("contentItem").value<QObject *>(); if (!flick) flick = view;
        return flick->property("contentHeight").toReal() - flick->property("height").toReal();
    }

    // A verse point on screen, outside the popup `name` and clear of the footer.
    QPoint outsidePoint(const QString &name, int avoidVerse = 3) {
        auto popup = object(name);
        auto frame = qobject_cast<QQuickItem *>(popup->property("background").value<QObject *>());
        const QRectF area = frame ? frame->mapRectToScene(frame->boundingRect()) : QRectF();
        for (int verse = 1; verse <= 36; ++verse) {
            if (verse == avoidVerse) continue;
            auto body = item("verseBody" + QString::number(verse)); if (!body) continue;
            // Any line of the verse: a popup may leave only part of one clear.
            for (const qreal y : {qreal(8), body->height() - 8})
                for (const QPointF local : {QPointF(30, y), QPointF(body->width() / 2, y), QPointF(body->width() - 30, y)}) {
                    const QPoint point = body->mapToScene(local).toPoint();
                    if (point.y() > 60 && point.y() < window->height() - 100 && !area.adjusted(-8, -8, 8, 8).contains(point)) return point;
                }
        }
        return {};
    }

    // Character selection (v) state, as {verse, offset} maps.
    QVariantMap charCursor() const { return prop("charCursor").toMap(); }
    QVariantMap charAnchor() const { return prop("charAnchor").toMap(); }
    bool charAt(const QVariantMap &at, int verse, int offset) const { return at.value("verse").toInt() == verse && at.value("offset").toInt() == offset; }
    QString verseText(int number) const {
        for (const auto &row : bible->verses()) if (row.toMap()["number"].toInt() == number) return row.toMap()["text"].toString();
        return {};
    }
    void keys(const QString &text) {
        for (const auto c : text) {
            const char ch = c.toLatin1();
            Qt::KeyboardModifiers mods = (QChar(ch).isUpper() || QString("()$^").contains(c)) ? Qt::ShiftModifier : Qt::NoModifier;
            QTest::keyClick(window, ch, mods);
        }
        QCoreApplication::processEvents();
    }
    void drag(const QPoint &from, const QPoint &to) {
        QTest::mousePress(window, Qt::LeftButton, {}, from);
        for (int i = 1; i <= 6; ++i) QTest::mouseMove(window, from + (to - from) * i / 6);
        QTest::mouseRelease(window, Qt::LeftButton, {}, to); QCoreApplication::processEvents();
    }
    // The scene point of a source character (verse, offset) in any layout.
    QPoint charPoint(int verse, int offset) {
        QVariant result;
        const QVariant at = QVariantMap{{"verse", verse}, {"offset", offset}};
        QMetaObject::invokeMethod(window, "charRect", Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, at));
        const auto r = value(result).toMap(); if (r.isEmpty()) return {};
        return item("readerColumn")->mapToScene(QPointF(r["x"].toReal() + 2, r["y"].toReal() + r["height"].toReal() / 2)).toPoint();
    }

    // Visual checks: with OMAWORD_A11Y_SHOTS set to a directory, save the window.
    void shot(const QString &name) {
        const auto directory = qEnvironmentVariable("OMAWORD_A11Y_SHOTS");
        if (directory.isEmpty()) return;
        QCoreApplication::processEvents(); QTest::qWait(30);
        QVERIFY(window->grabWindow().save(directory + "/" + name + ".png"));
    }

    // Clicks a hint part once it is clickable and its row has settled (a Flow
    // lays parts out on the next polish after a row appears or changes).
    void clickHint(const QString &name) {
        QTRY_VERIFY2(item(name) && item(name)->property("clickable").toBool(), qPrintable(name));
        QPointF last(-1, -1);
        QTRY_VERIFY([&] { QTest::qWait(20); const auto at = item(name)->mapToScene(QPointF()); const bool settled = at == last; last = at; return settled; }());
        click(item(name));
    }

public slots:
    // Web links opened from a note (QDesktopServices handler).
    void openUrl(const QUrl &url) { openedUrls << url; }
private:
    QList<QUrl> openedUrls;
private slots:
    void initTestCase() {
        source = fixtures.filePath("access_usfm.zip"); QVERIFY(edition(source));
    }
    void init() {
        session = std::make_unique<QTemporaryDir>(); qputenv("XDG_DATA_HOME", session->path().toUtf8());
        bible = std::make_unique<BibleBackend>(); install(source); bible->setReadingMode("verse");
        desktop = std::make_unique<Desktop>(); sounds = std::make_unique<SoundEffects>(nullptr, session->filePath("sound.ini"), false);
        sounds->setReduceMotion(true);
        catalog = std::make_unique<MockCatalog>();
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
        window->resize(1200, 820);
        window->requestActivate(); QVERIFY(QTest::qWaitForWindowExposed(window)); QTRY_VERIFY(window->isActive());
        window->setProperty("historyDwellMs", 3600000);
        navigate("John 3:3");
    }
    void cleanup() {
        engine.reset(); window = nullptr; catalog.reset(); sounds.reset(); desktop.reset(); bible.reset(); session.reset();
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
    }

    // ── Phase 1: shared foundations

    void tooltipsShareOneDelay() {
        int seen = 0;
        for (auto child : window->findChildren<QObject *>()) {
            if (!QByteArray(child->metaObject()->className()).startsWith("AppToolTip")) continue;
            QCOMPARE(child->property("delay").toInt(), 600); ++seen;
        }
        QVERIFY(seen > 5);
    }
    void buttonsActivateOnEnterAndOwnSpace() {
        // Keyboard: a focused footer button takes enter and space; space does not resume the palette.
        auto aa = item("textControl"); QVERIFY(aa);
        aa->forceActiveFocus(); QVERIFY(!shown("textPopover"));
        key(Qt::Key_Return); QTRY_VERIFY(shown("textPopover"));
        key(Qt::Key_Enter); QTRY_VERIFY(!shown("textPopover"));
        auto search = item("searchControl"); search->forceActiveFocus();
        key(Qt::Key_Space); QTRY_VERIFY(opened("passagePalette"));
        QCOMPARE(object("referenceInput")->property("text").toString(), QString("/ "));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("passagePalette"));
        // A panel's QuietButton: enter and space both activate it.
        run("desk"); QTRY_VERIFY(item("deskEditor")->hasActiveFocus());
        item("deskPreviewButton")->forceActiveFocus(); key(Qt::Key_Space);
        QTRY_VERIFY(prop("deskPreview").toBool()); QTRY_VERIFY(item("deskPreview")->hasActiveFocus());
        item("deskWriteButton")->forceActiveFocus(); key(Qt::Key_Return);
        QTRY_VERIFY(!prop("deskPreview").toBool()); QTRY_VERIFY(item("deskEditor")->hasActiveFocus());
        escapeNotes();
    }
    void chromeButtonsTakeFocusOnlyByTab() {
        // Mouse: a click never moves the keyboard off the reader.
        QVERIFY(readerFocused());
        auto layout = item("textControl");
        QCOMPARE(layout->property("focusPolicy").toInt(), int(Qt::TabFocus));
        click(layout); QTRY_VERIFY(shown("textPopover")); QVERIFY(readerFocused());
        // Reached by tab, a mouse click hands the keyboard back to the reader.
        layout->forceActiveFocus(); QVERIFY(layout->hasActiveFocus());
        click(layout); QTRY_VERIFY(!shown("textPopover")); QTRY_VERIFY(readerFocused());
        // Keyboard focus shows the tooltip as hover does.
        auto tip = toolTipOf(layout); QVERIFY(tip);
        layout->forceActiveFocus(); QTRY_VERIFY(tip->property("visible").toBool());
        QTest::mouseMove(window, QPoint(window->width() / 2, 200));
        call("readerFocus"); QTRY_VERIFY(!tip->property("visible").toBool());
        // Tab reaches the footer from the reader.
        bool reached = false;
        for (int i = 0; i < 12 && !reached; ++i) { key(Qt::Key_Tab); reached = focusName() == "helpControl" || focusName() == "modeControl" || focusName() == "searchControl"; }
        QVERIFY(reached);
    }
    void hintPartsAreClickableOnlyWhenTheyAct() {
        QVERIFY(bible->navigate("John 3:5")); QVERIFY(!bible->appendToDesk().isEmpty());
        navigate("John 3:3");
        // The Desk's page in Preview: its scroll keys only show; the rest act.
        run("desk"); QTRY_VERIFY(item("deskEditor")->hasActiveFocus());
        key(Qt::Key_V, Qt::AltModifier); QTRY_VERIFY(item("deskPreview")->hasActiveFocus());
        // The row's parts are rebuilt whenever what they can do changes: look
        // them up by name each time, once the layout has settled where the
        // pointer will go.
        QVERIFY(QQuickTest::qWaitForPolish(window));
        auto scroll = [this] { return item("deskHintsPart:scroll"); };
        auto write = [this] { return item("deskHintsPart:write"); };
        auto tint = [this] { return item("deskHintsPart:write:tint"); };
        QVERIFY(scroll() && write());
        QVERIFY(!scroll()->property("clickable").toBool()); QVERIFY(write()->property("clickable").toBool());
        // The row reads as before: the same markup.
        QVERIFY(object("deskHints")->property("text").toString().contains("alt&nbsp;v"));
        // Unhovered parts show no tint; hovering a clickable one tints it under a pointing hand.
        QVERIFY(tint()); QVERIFY(!tint()->isVisible());
        shot("hints-desk-unhovered");
        QTest::mouseMove(window, center(write())); QTRY_VERIFY(tint()->isVisible());
        shot("hints-desk-hover-write");
        const auto areaOf = [](QQuickItem *part) -> QQuickItem * {
            for (auto child : part->childItems()) if (QByteArray(child->metaObject()->className()).contains("MouseArea")) return child;
            return nullptr;
        };
        QVERIFY(areaOf(write())); QCOMPARE(areaOf(write())->cursor().shape(), Qt::PointingHandCursor);
        QTest::mouseMove(window, center(scroll())); QTRY_VERIFY(!tint()->isVisible());
        QVERIFY(!item("deskHintsPart:scroll:tint")->isVisible());
        // A part a click cannot act on never shows the pointing hand (a
        // disabled MouseArea still sets the cursor).
        QVERIFY(areaOf(scroll())); QCOMPARE(areaOf(scroll())->cursor().shape(), Qt::ArrowCursor);
        // Mouse: a part runs what its key does, and the keyboard stays in the Margin.
        click(write()); QTRY_VERIFY(item("deskEditor")->hasActiveFocus()); QVERIFY(!prop("deskPreview").toBool());
        // Keyboard: the key itself still works.
        key(Qt::Key_V, Qt::AltModifier); QTRY_VERIFY(item("deskPreview")->hasActiveFocus());
        clickHint("deskHintsPart:write"); QTRY_VERIFY(item("deskEditor")->hasActiveFocus());
        // On a link the page offers alt enter, which follows it.
        item("deskEditor")->setProperty("cursorPosition", bible->deskPage().lastIndexOf("John 3:5") + 2);
        clickHint("deskHintsPart:open link"); QTRY_COMPARE(bible->selectionReference(), QString("John 3:5"));
        // "to the text" gives the keyboard to the reader, the Desk staying; its
        // hints are then "keys here" alone, which brings it back; "close" closes.
        clickHint("deskHintsPart:to the text"); QTRY_VERIFY(readerFocused()); QVERIFY(opened("notesDrawer"));
        QTRY_COMPARE(item("deskHints")->property("plainText").toString(), QString("ctrl w keys here"));
        clickHint("deskHintsPart:keys here"); QTRY_VERIFY(item("deskEditor")->hasActiveFocus());
        clickHint("deskHintsPart:close"); QTRY_VERIFY(!shown("notesDrawer")); QTRY_VERIFY(readerFocused());
    }
    void everyPanelHintRowIsClickable() {
        // Palette: esc close and enter select.
        run("books"); QTRY_VERIFY(opened("passagePalette"));
        QVERIFY(!item("paletteHintsPart:choose")->property("clickable").toBool());
        clickHint("paletteHintsPart:close"); QTRY_VERIFY(!shown("passagePalette"));
        run("books"); QTRY_VERIFY(opened("passagePalette")); type("john 3:16");
        clickHint("paletteHintsPart:select"); QTRY_VERIFY(!shown("passagePalette"));
        QCOMPARE(bible->selectionReference(), QString("John 3:16"));
        // Help: a disabled (explanation) row cannot be selected by its hint.
        run("help"); QTRY_VERIFY(opened("helpDialog")); type("click");
        QTRY_VERIFY(!item("keybindingHintsPart:select")->property("clickable").toBool());
        clickHint("keybindingHintsPart:close"); QTRY_VERIFY(!shown("helpDialog"));
        // Versions: cancel by its hint.
        run("switchVersion"); QTRY_VERIFY(opened("versionDialog"));
        QVERIFY(item("versionHintsPart:select")->property("clickable").toBool());
        clickHint("versionHintsPart:cancel"); QTRY_VERIFY(!shown("versionDialog"));
        // Reading settings: increase / reset.
        run("settings"); QTRY_VERIFY(opened("settingsDialog"));
        const int size = bible->fontSize();
        clickHint("fontSizeHintsPart:increase"); QCOMPARE(bible->fontSize(), size + 1);
        clickHint("fontSizeHintsPart:reset"); QCOMPARE(bible->fontSize(), 18);
        // The line height row may sit below the dialog's fold: scroll to it.
        if (travel("settingsScroll") > 0) {
            auto flick = object("settingsScroll")->property("contentItem").value<QObject *>(); QVERIFY(flick);
            flick->setProperty("contentY", travel("settingsScroll")); QTRY_VERIFY(contentY("settingsScroll") > 0);
        }
        clickHint("lineHeightHintsPart:increase"); QCOMPARE(bible->lineHeight(), 1.6);
        clickHint("lineHeightHintsPart:reset"); QCOMPARE(bible->lineHeight(), 1.5);
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("settingsDialog"));
        // Annotations: open the current row by its hint.
        QVERIFY(bible->navigate("John 3:9")); bible->selectVerses(9, 9); QVERIFY(bible->toggleHighlight());
        navigate("John 1:1");
        run("annotations"); QTRY_VERIFY(panelOpened("marks"));
        QTRY_VERIFY(item("savedHintsPart:open")->property("clickable").toBool());
        clickHint("savedHintsPart:open"); QTRY_COMPARE(bible->selectionReference(), QString("John 3:9"));
        QVERIFY(panelShown("marks")); // Marks stays beside the passage it opened.
        closeMargin();
        // Confirmations: cancel by the hint, confirm with enter on the focused button.
        QVERIFY(bible->recordHistory()); run("clearHistory"); QTRY_VERIFY(opened("clearHistoryConfirmation"));
        clickHint("clearHistoryHintsPart:cancel"); QTRY_VERIFY(!shown("clearHistoryConfirmation"));
        QCOMPARE(bible->history().size(), 1);
        run("clearHistory"); QTRY_VERIFY(opened("clearHistoryConfirmation"));
        key(Qt::Key_Right); QVERIFY(item("clearHistoryConfirm")->hasActiveFocus());
        clickHint("clearHistoryHintsPart:select"); QTRY_VERIFY(bible->history().isEmpty());
        call("readerFocus");
        // See also and the tag picker.
        run("related"); QTRY_VERIFY(panelOpened("related"));
        QTRY_VERIFY(item("studyHintsPart:filter")); QVERIFY(item("studyHintsPart:filter")->property("clickable").toBool());
        clickHint("studyHintsPart:filter"); QTRY_VERIFY(item("studySearch")->hasActiveFocus());
        key(Qt::Key_Escape); QTRY_VERIFY(item("studyList")->hasActiveFocus());
        clickHint("studyHintsPart:to the text"); QTRY_VERIFY(readerFocused()); QVERIFY(panelShown("related"));
        clickHint("studyHintsPart:keys here"); QTRY_VERIFY(item("studyList")->hasActiveFocus());
        clickHint("studyHintsPart:close"); QTRY_VERIFY(!shown("notesDrawer"));
        run("tags"); QTRY_VERIFY(opened("tagsSurface")); type("Grace");
        clickHint("tagsHintsPart:toggle / create"); QTRY_COMPARE(bible->tags().size(), 1);
        clickHint("tagsHintsPart:close"); QTRY_VERIFY(!shown("tagsSurface"));
    }
    void everyListRowSharesOneMenu() {
        const QStringList shared = {"open", "copy", "copyPlain", "desk"};
        navigate("John 3:3");
        // History, in the palette (keyboard): the menu key opens the row menu
        // with Remove after a divider.
        QVERIFY(bible->recordHistory()); run("history"); QTRY_VERIFY(opened("passagePalette")); QVERIFY(prop("paletteHistory").toBool());
        key(Qt::Key_Menu); QTRY_VERIFY(menu()->property("opened").toBool());
        QCOMPARE(menuIds(), (shared + QStringList{"|", "remove"}));
        key(Qt::Key_Escape); QTRY_VERIFY(!menu()->property("visible").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("passagePalette"));
        // Marks rows: the shared four, then Write note, Edit tags, Move here, Delete, and the undo it offers.
        bible->selectVerses(3, 3); QVERIFY(bible->toggleHighlight());
        run("annotations"); QTRY_VERIFY(panelOpened("marks"));
        key(Qt::Key_Menu); QTRY_VERIFY(menu()->property("opened").toBool());
        QCOMPARE(menuIds(), (shared + QStringList{"|", "note", "tags", "moveHere", "delete", "undo"}));
        key(Qt::Key_Escape); QTRY_VERIFY(!menu()->property("visible").toBool()); closeMargin();
        // See also rows: mouse and keyboard (Edit tags follows the shared four).
        navigate("John 1:1"); run("related"); QTRY_VERIFY(panelOpened("related"));
        QTRY_VERIFY(item("studyRow:0") && item("studyRow:0")->isVisible());
        click(item("studyRow:0"), Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
        QCOMPARE(menuIds(), (shared + QStringList{"|", "tags"}));
        QCOMPARE(menuShortcut("copyPlain"), QString("shift Y")); QCOMPARE(menuShortcut("desk"), QString("d"));
        QGuiApplication::clipboard()->clear(); chooseMenu("copy");
        QTRY_VERIFY(QGuiApplication::clipboard()->text().contains("Genesis 1:1"));
        key(Qt::Key_Menu); QTRY_VERIFY(menu()->property("opened").toBool()); QCOMPARE(menuIds(), (shared + QStringList{"|", "tags"}));
        key(Qt::Key_Escape); QTRY_VERIFY(!menu()->property("visible").toBool()); closeMargin();
    }
    void paletteRecentsAndLinkedNotesHaveRowMenus() {
        // Recent searches: Search again · Copy · Copy plain text (no Desk; no list keys shown).
        QVERIFY(bible->recordQuery("loved"));
        key(Qt::Key_Slash, Qt::ControlModifier); QTRY_VERIFY(opened("passagePalette"));
        QTRY_VERIFY(prop("paletteTextMode").toBool());
        key(Qt::Key_Menu); QTRY_VERIFY(menu()->property("opened").toBool());
        QCOMPARE(menuIds(), (QStringList{"open", "copy", "copyPlain"}));
        QCOMPARE(menuShortcut("copy"), QString());
        QGuiApplication::clipboard()->clear(); chooseMenu("copy");
        QTRY_COMPARE(QGuiApplication::clipboard()->text(), QString("loved"));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("passagePalette"));
        // Recent passages (mouse).
        QVERIFY(bible->navigate("John 2:4")); QVERIFY(bible->recordHistory()); navigate("John 3:3");
        run("books"); QTRY_VERIFY(opened("passagePalette"));
        QQuickItem *row = nullptr;
        QTRY_VERIFY((row = qobject_cast<QQuickItem *>(item("paletteResults")->property("currentItem").value<QObject *>())));
        click(row, Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
        // A recent passage is a History row: the shared four, then Remove.
        QCOMPARE(menuIds(), (QStringList{"open", "copy", "copyPlain", "desk", "|", "remove"}));
        chooseMenu("open"); QTRY_VERIFY(!shown("passagePalette"));
        QCOMPARE(bible->selectionReference(), QString("John 2:4"));
        // Notes → Linked from rows.
        QVERIFY(bible->navigate("John 1:5")); bible->selectVerses(5, 5); bible->setNoteText("See John 3:16 for this.");
        navigate("John 3:16"); bible->selectVerses(16, 16);
        run("notes"); QTRY_VERIFY(opened("notesDrawer"));
        QTRY_VERIFY(item("linkedFromRow0") && item("linkedFromRow0")->isVisible());
        click(item("linkedFromRow0"), Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
        QCOMPARE(menuIds(), (QStringList{"open", "copy", "copyPlain", "desk"}));
        chooseMenu("open"); QTRY_COMPARE(bible->selectionReference(), QString("John 1:5"));
    }
    void scrollablePanesScrollByKey() {
        window->resize(900, 480); QTRY_COMPARE(window->height(), 480); bible->setFontSize(26);
        // About: focus lands on the pane; j k pgdn ctrl d/u home end move it.
        run("about"); QTRY_VERIFY(opened("aboutDialog")); QTRY_VERIFY(item("aboutScroll")->hasActiveFocus());
        QVERIFY(travel("aboutScroll") > 20);
        key(Qt::Key_J); QTRY_VERIFY(contentY("aboutScroll") > 0);
        key(Qt::Key_K); QTRY_COMPARE(contentY("aboutScroll"), 0.0);
        key(Qt::Key_End); QTRY_VERIFY(qAbs(contentY("aboutScroll") - travel("aboutScroll")) < 1);
        key(Qt::Key_Home); QTRY_COMPARE(contentY("aboutScroll"), 0.0);
        key(Qt::Key_D, Qt::ControlModifier); QTRY_VERIFY(contentY("aboutScroll") > 0);
        key(Qt::Key_U, Qt::ControlModifier); QTRY_COMPARE(contentY("aboutScroll"), 0.0);
        key(Qt::Key_PageDown); QTRY_VERIFY(contentY("aboutScroll") > 0);
        // Mouse: the wheel still scrolls it.
        key(Qt::Key_Home); wheel(item("aboutScroll"), -240); QTRY_VERIFY(contentY("aboutScroll") > 0);
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("aboutDialog"));
        // Reading settings: tabbing to a control below the fold scrolls it into view.
        run("settings"); QTRY_VERIFY(opened("settingsDialog"));
        QVERIFY(travel("settingsScroll") > 20);
        auto first = item("settingfocus"); QVERIFY(first); first->forceActiveFocus();
        QTRY_VERIFY(first->mapRectToItem(item("settingsScroll"), first->boundingRect()).top() >= -1);
        key(Qt::Key_Home); QTRY_COMPARE(contentY("settingsScroll"), 0.0);
        key(Qt::Key_PageDown); QTRY_VERIFY(contentY("settingsScroll") > 0);
        key(Qt::Key_Home); QTRY_COMPARE(contentY("settingsScroll"), 0.0);
        auto line = item("lineHeightSlider"); line->forceActiveFocus(); QCoreApplication::processEvents();
        const auto slider = line->mapRectToItem(item("settingsScroll"), line->boundingRect());
        QVERIFY2(slider.top() >= -1 && slider.bottom() <= item("settingsScroll")->height() + 1, qPrintable(QString::number(slider.top())));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("settingsDialog"));
        // Versions removal summary: arrows still choose, the other scroll keys scroll.
        window->resize(640, 480); bible->setFontSize(40);
        run("removeVersion"); QTRY_VERIFY(opened("versionDialog"));
        key(Qt::Key_Return); key(Qt::Key_Return); QTRY_COMPARE(object("versionDialog")->property("step").toInt(), 2);
        QTRY_VERIFY(travel("versionSummaryScroll") > 0);
        key(Qt::Key_End); QTRY_VERIFY(contentY("versionSummaryScroll") > 0);
        key(Qt::Key_Home); QTRY_COMPARE(contentY("versionSummaryScroll"), 0.0);
        QVERIFY(item("versionCancel")->hasActiveFocus()); key(Qt::Key_Right); QVERIFY(item("versionNext")->hasActiveFocus());
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("versionDialog"));
        // Add-version guide: paging keys from the search field scroll the catalog;
        // Details & Attribution takes focus and scrolls by key.
        QVariantList entries;
        for (int i = 0; i < 12; ++i) entries << QVariantMap{{"id", QString("v%1").arg(i)}, {"name", QString("Version %1 with a long descriptive name").arg(i)},
            {"abbreviation", QString("V%1").arg(i)}, {"language", "eng"}, {"copyright", QString("A long copyright notice. ").repeated(40)}, {"detailsUrl", "https://example.invalid"}, {"description", "A fixture edition."}, {"year", "2026"}, {"license", "Public domain"}, {"size", 1500000}, {"features", QVariantList{}}};
        catalog->entries = entries; emit catalog->changed();
        bible->setFontSize(26); window->resize(900, 480); QTRY_COMPARE(window->width(), 900);
        run("import"); QTRY_VERIFY(opened("importGuide")); QTRY_VERIFY(item("catalogSearch")->hasActiveFocus());
        QTRY_VERIFY(object("catalogResults")->property("contentHeight").toDouble() > item("catalogResults")->height() + 20);
        key(Qt::Key_PageDown); QTRY_VERIFY(object("catalogResults")->property("contentY").toDouble() > 0);
        QVERIFY(item("catalogSearch")->hasActiveFocus());
        key(Qt::Key_PageUp); QTRY_VERIFY(qAbs(object("catalogResults")->property("contentY").toDouble() - object("catalogResults")->property("originY").toDouble()) < 1);
        // A card (name, description, facts, buttons) can be taller than this short
        // window's viewport: scroll its Details button into view, then click it.
        auto guideFlick = item("catalogResults");
        auto details = item("catalogDetails:v0");
        const qreal buttonY = details->mapToItem(guideFlick->property("contentItem").value<QQuickItem *>(), QPointF()).y();
        guideFlick->setProperty("contentY", qMax(0.0, buttonY + details->height() - guideFlick->height()));
        QTRY_VERIFY(guideFlick->mapRectFromItem(details, details->boundingRect()).bottom() <= guideFlick->height());
        click(details); QTRY_VERIFY(opened("catalogDetails")); QTRY_VERIFY(item("detailsScroll")->hasActiveFocus());
        QTRY_VERIFY(travel("detailsScroll") > 20);
        key(Qt::Key_End); QTRY_VERIFY(contentY("detailsScroll") > 0);
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("catalogDetails"));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("importGuide"));
        bible->setFontSize(40); window->resize(640, 480); QTRY_COMPARE(window->width(), 640);
        // Backup scope: the explanation scrolls while Choose file… keeps focus.
        run("backup"); QTRY_VERIFY(opened("backupScope"));
        QTRY_VERIFY(travel("backupScopeScroll") > 0);
        key(Qt::Key_J); QTRY_VERIFY(contentY("backupScopeScroll") > 0);
        QVERIFY(item("backupChoose")->hasActiveFocus());
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("backupScope"));
        // The Notes preview takes focus and scrolls.
        bible->setFontSize(18); window->resize(1200, 820); QTRY_COMPARE(window->height(), 820);
        QString note; for (int i = 0; i < 80; ++i) note += QString("Line %1 of a long note.\n\n").arg(i);
        bible->selectVerses(3, 3); bible->setNoteText(note);
        run("notes"); QTRY_VERIFY(opened("notesDrawer"));
        click("previewButton"); QTRY_VERIFY(item("notePreview")->hasActiveFocus());
        QTRY_VERIFY(travel("notePreviewScroll") > 100);
        key(Qt::Key_End); QTRY_VERIFY(qAbs(contentY("notePreviewScroll") - travel("notePreviewScroll")) < 1);
        key(Qt::Key_K); QTRY_VERIFY(contentY("notePreviewScroll") < travel("notePreviewScroll"));
        escapeNotes();
    }
    void outlineScrollsByKey() {
        // The chapter outline: reached by tab; j scrolls it instead of moving the reader.
        bible->setChapterOutline(true); navigate("John 3:3");
        QTRY_VERIFY(item("chapterOutline")->isVisible());
        bool reached = false;
        for (int i = 0; i < 30 && !reached; ++i) { key(Qt::Key_Tab); reached = item("chapterOutline")->hasActiveFocus(); }
        QVERIFY(reached); QVERIFY(item("outlineFocus")->isVisible());
        QVERIFY(!prop("readerKeys").toBool());
        key(Qt::Key_J); QCOMPARE(bible->cursorVerse(), 3);
        key(Qt::Key_Escape); QTRY_VERIFY(readerFocused());
        key(Qt::Key_J); QCOMPARE(bible->cursorVerse(), 4);
        // Mouse: clicking an outline entry still jumps.
        click(item("outlineEntry:20")); QTRY_COMPARE(bible->cursorVerse(), 20);
    }
    void rejectedKeysAndRowsSoundTheError() {
        bible->clearSelection(); call("readerFocus"); QTRY_VERIFY(!prop("hasSelection").toBool());
        // Keyboard: a reader key that cannot act now (highlight with nothing
        // selected) sounds the error and says why.
        int before = playCount(); key(Qt::Key_X);
        QCOMPARE(playCount(), before + 1); QCOMPARE(lastSound(), QString("error")); QVERIFY(bible->collection().isEmpty());
        QTRY_COMPARE(object("toast")->property("text").toString(), QString("Select a verse first"));
        // A disabled command row: enter sounds, the palette stays.
        run("commands"); QTRY_VERIFY(opened("passagePalette")); type("highlight selection");
        QTRY_COMPARE(item("paletteResults")->property("count").toInt(), 1);
        object("toast")->setProperty("text", QString());
        before = playCount(); key(Qt::Key_Return);
        QCOMPARE(lastSound(), QString("error")); QVERIFY(playCount() > before); QVERIFY(opened("passagePalette"));
        QCOMPARE(object("toast")->property("text").toString(), QString("Select a verse first"));
        // Mouse: a click on a disabled row runs nothing and sounds the error.
        before = playCount();
        click(qobject_cast<QQuickItem *>(item("paletteResults")->property("currentItem").value<QObject *>()));
        QCOMPARE(playCount(), before + 1); QCOMPARE(lastSound(), QString("error")); QVERIFY(opened("passagePalette"));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("passagePalette"));
        // A disabled keybinding row.
        run("help"); QTRY_VERIFY(opened("helpDialog")); type("highlight selection");
        before = playCount(); key(Qt::Key_Return);
        QCOMPARE(lastSound(), QString("error")); QVERIFY(playCount() > before); QVERIFY(opened("helpDialog"));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("helpDialog"));
        // An empty list: y has no row to copy.
        run("annotations"); QTRY_VERIFY(panelOpened("marks"));
        before = playCount(); key(Qt::Key_Y); QCOMPARE(lastSound(), QString("error")); QVERIFY(playCount() > before);
        closeMargin();
        // Character selection: a key it does not have says how to leave.
        bible->selectVerses(3, 3); call("readerFocus"); key(Qt::Key_V); QTRY_VERIFY(prop("charMode").toBool());
        before = playCount(); key(Qt::Key_Q);
        QCOMPARE(playCount(), before + 1); QCOMPARE(lastSound(), QString("error"));
        QVERIFY(object("toast")->property("text").toString().startsWith("Not a key in character selection"));
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("charMode").toBool());
    }
    void outsideClicksOnlyClose() {
        bible->selectVerses(3, 3); call("readerFocus");
        auto check = [&](const QString &popup, std::function<void()> open) {
            open(); QTRY_VERIFY2(opened(popup), qPrintable(popup));
            const QPoint point = outsidePoint(popup); QVERIFY2(!point.isNull(), qPrintable(popup));
            const int before = playCount();
            clickAt(point); QTRY_VERIFY2(!shown(popup), qPrintable(popup));
            QVERIFY2(bible->selectedStart() == 3 && bible->selectedEnd() == 3, qPrintable(popup + ": " + bible->selectionReference()));
            QVERIFY2(bible->collection().isEmpty() && bible->deskPage().isEmpty(), qPrintable(popup));
            QVERIFY(playCount() >= before);
            QTest::qWait(30);
        };
        check("passagePalette", [&] { run("books"); });
        check("helpDialog", [&] { run("help"); });
        check("settingsDialog", [&] { run("settings"); });
        check("versionDialog", [&] { run("switchVersion"); });
        check("aboutDialog", [&] { run("about"); });
        // Marks and See also sit beside the text, as Notes does: a click on the
        // text reaches the reader, and the panel stays.
        for (const auto &[command, mode] : {std::pair{"annotations", "marks"}, std::pair{"related", "related"}}) {
            run(command); QTRY_VERIFY2(panelOpened(mode), mode);
            clickAt(outsidePoint("notesDrawer"));
            QTRY_VERIFY2(readerFocused(), mode); QVERIFY2(panelOpened(mode), mode); QVERIFY(bible->selectedStart() != 3);
            key(Qt::Key_Escape); QTRY_VERIFY(!prop("selectionBarShown").toBool());
            key(Qt::Key_Escape); QTRY_VERIFY(!shown("notesDrawer"));
            bible->selectVerses(3, 3); call("readerFocus");
        }
        // Notes sits beside the text, never over it: a click on the text
        // reaches the reader (the verse clicked is selected, the keyboard
        // comes back) and Notes stays open, following the selection.
        run("notes"); QTRY_VERIFY(opened("notesDrawer")); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        clickAt(outsidePoint("notesDrawer"));
        QTRY_VERIFY(readerFocused()); QVERIFY(opened("notesDrawer"));
        QVERIFY(bible->selectedStart() != 3); QCOMPARE(prop("loadedNoteReference").toString(), bible->selectionReference());
        // esc puts the mouse's selection bar away first, then closes Notes.
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("selectionBarShown").toBool()); QVERIFY(opened("notesDrawer"));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("notesDrawer"));
        bible->selectVerses(3, 3); call("readerFocus");
        // The picker nearly fills the window: give it room above to click outside.
        window->resize(1200, 1100); QTRY_COMPARE(window->height(), 1100); call("setReaderY", 0);
        check("tagsSurface", [&] { run("tags"); });
        window->resize(1200, 820); QTRY_COMPARE(window->height(), 820);
        // Context menu: a left click outside only closes it.
        key(Qt::Key_Menu); QTRY_VERIFY(menu()->property("opened").toBool());
        clickAt(outsidePoint("contextMenu")); QTRY_VERIFY(!menu()->property("visible").toBool());
        QCOMPARE(bible->selectedStart(), 3);
        // ...while a right click elsewhere closes it and opens the new menu there at once.
        QTRY_VERIFY(prop("readerKeys").toBool()); QTRY_VERIFY(readerFocused());
        key(Qt::Key_Menu); QTRY_VERIFY(menu()->property("opened").toBool());
        const QPoint there = versePoint(2);
        clickAt(there, Qt::RightButton);
        QTRY_VERIFY(menu()->property("opened").toBool());
        QCOMPARE(bible->selectedStart(), 2);
        QVERIFY(qAbs(menu()->property("x").toReal() - there.x()) < 2);
        key(Qt::Key_Escape); QTRY_VERIFY(!menu()->property("visible").toBool());
    }
    // ── Phase 2: critical and high

    void searchPaletteLoadsMoreByKeyAndHint() {
        auto results = item("paletteResults");
        auto search = [&] {
            key(Qt::Key_Slash, Qt::ControlModifier); QTRY_VERIFY(opened("passagePalette"));
            type("access"); QTRY_VERIFY(prop("paletteSearchCurrent").toBool()); QTRY_VERIFY(!bible->searchBusy());
            QTRY_COMPARE(results->property("count").toInt(), 100); QCOMPARE(bible->searchResultCount(), 167);
        };
        search();
        // The hint row offers ↓ more while a page is left.
        QVERIFY(item("paletteHintsPart:more") && item("paletteHintsPart:more")->property("clickable").toBool());
        // Keyboard: end reaches the last loaded row, end again loads the next page.
        key(Qt::Key_Down); key(Qt::Key_End); QCOMPARE(results->property("currentIndex").toInt(), 99);
        key(Qt::Key_End); QTRY_COMPARE(results->property("count").toInt(), 167);
        QCOMPARE(results->property("currentIndex").toInt(), 100);
        QTRY_VERIFY(!item("paletteHintsPart:more"));
        key(Qt::Key_Home); QCOMPARE(results->property("currentIndex").toInt(), 0);
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("passagePalette"));
        // ↓ past the last row and pgdn load too.
        run("books"); key(Qt::Key_Escape); QTRY_VERIFY(!shown("passagePalette"));
        search();
        key(Qt::Key_Down); key(Qt::Key_End); key(Qt::Key_Down);
        QTRY_COMPARE(results->property("count").toInt(), 167); QCOMPARE(results->property("currentIndex").toInt(), 100);
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("passagePalette"));
        run("books"); key(Qt::Key_Escape); QTRY_VERIFY(!shown("passagePalette"));
        search();
        // pgdn pages through the loaded rows; paging past the last loads the next page.
        int pages = 0;
        for (; pages < 200 && results->property("count").toInt() == 100; ++pages) {
            key(Qt::Key_PageDown); QCoreApplication::processEvents();
        }
        QTRY_COMPARE(results->property("count").toInt(), 167);
        QVERIFY(pages > 1); QVERIFY(results->property("currentIndex").toInt() > 99);
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("passagePalette"));
        // Mouse: the "↓ more" hint loads the next page.
        run("books"); key(Qt::Key_Escape); QTRY_VERIFY(!shown("passagePalette"));
        search();
        clickHint("paletteHintsPart:more"); QTRY_COMPARE(results->property("count").toInt(), 167);
    }
    void reimportFromVersionsByKeyAndButton() {
        const QString id = bible->versionId();
        catalog->entries = {QVariantMap{{"id", id}, {"name", "Access fixture"}, {"abbreviation", "OAF"}, {"copyright", "Original"}, {"detailsUrl", "https://example.invalid"}, {"description", "A fixture edition."}, {"year", "2026"}, {"license", "Public domain"}, {"size", 1500000}, {"features", QVariantList{}}, {"sha256", "revision-2"}}};
        emit catalog->changed();
        // Keyboard: ctrl shift I re-imports the highlighted row; the hint names the key.
        run("switchVersion"); QTRY_VERIFY(opened("versionDialog"));
        QTRY_VERIFY(item("versionHintsPart:re-import"));
        QVERIFY(object("versionHints")->property("text").toString().contains("ctrl&nbsp;shift&nbsp;I"));
        key(Qt::Key_I, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(catalog->requests, QStringList{id}); QTRY_VERIFY(opened("importGuide"));
        catalog->cancel(); key(Qt::Key_Escape); QTRY_VERIFY(!shown("importGuide"));
        // Keyboard: the Re-import button, reached by focus, takes enter.
        catalog->requests.clear();
        run("switchVersion"); QTRY_VERIFY(opened("versionDialog"));
        auto button = item("versionReimport:" + id); QTRY_VERIFY(button && button->isVisible());
        button->forceActiveFocus(); key(Qt::Key_Return);
        QTRY_COMPARE(catalog->requests, QStringList{id});
        catalog->cancel(); key(Qt::Key_Escape); QTRY_VERIFY(!shown("importGuide"));
        // Mouse: the hint part re-imports too.
        catalog->requests.clear();
        run("switchVersion"); QTRY_VERIFY(opened("versionDialog"));
        QTRY_VERIFY(item("versionHintsPart:re-import") && item("versionHintsPart:re-import")->property("clickable").toBool());
        QTest::qWait(50);
        clickHint("versionHintsPart:re-import"); QTRY_COMPARE(catalog->requests, QStringList{id});
        catalog->cancel(); key(Qt::Key_Escape); QTRY_VERIFY(!shown("importGuide"));
    }
    void escapeFinallyClearsTheErrorBanner() {
        // Keyboard: esc first drops a pending count, then clears the banner.
        QVERIFY(!bible->navigate("Nowhere 99")); QVERIFY(!bible->error().isEmpty());
        QTRY_VERIFY(item("errorBanner")->isVisible());
        type("3"); QCOMPARE(prop("verseCount").toString(), QString("3"));
        key(Qt::Key_Escape); QCOMPARE(prop("verseCount").toString(), QString()); QVERIFY(!bible->error().isEmpty());
        key(Qt::Key_Escape); QVERIFY(bible->error().isEmpty()); QTRY_VERIFY(!item("errorBanner")->isVisible());
        // Mouse: Dismiss clears it; its tooltip names esc.
        QVERIFY(!bible->navigate("Nowhere 99")); QTRY_VERIFY(item("errorBanner")->isVisible());
        auto dismiss = item("errorDismiss"); QTRY_VERIFY(dismiss->width() > 0); QTest::qWait(50);
        QCOMPARE(toolTipOf(dismiss)->property("text").toString(), QString::fromUtf8("Dismiss · esc"));
        click(dismiss); QVERIFY(bible->error().isEmpty()); QVERIFY(readerFocused());
    }
    void passageMenuOpensTheCursorsRangesAndMarkersSkipTab() {
        bible->selectVerses(5, 5); QVERIFY(!bible->createAndAssignTag(bible->tagScope(), "Five").isEmpty());
        bible->setNoteText("A note on five");
        bible->selectVerses(4, 6); QVERIFY(!bible->createAndAssignTag(bible->tagScope(), "Five").isEmpty());
        navigate("John 3:5"); bible->selectVerses(5, 5); call("readerFocus");
        // The verse rails are mouse only: tab never lands on them.
        for (int i = 0; i < 25; ++i) {
            key(Qt::Key_Tab);
            QVERIFY2(!focusName().contains("Marker"), qPrintable(focusName()));
        }
        call("readerFocus");
        // Keyboard: the menu adds Open note and Open tags (two overlap: a chooser).
        key(Qt::Key_Menu); QTRY_VERIFY(menu()->property("opened").toBool());
        QCOMPARE(menuIds().mid(menuIds().size() - 3), (QStringList{"|", "openNote", "openTags"}));
        menuKeyTo("openNote"); QTRY_VERIFY(opened("notesDrawer"));
        QCOMPARE(bible->selectionReference(), QString("John 3:5"));
        escapeNotes();
        // Mouse: right-click, Open tags, then pick the wider range in the
        // chooser: its tags open in place.
        clickAt(versePoint(5), Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
        chooseMenu("openTags");
        QTRY_VERIFY(object("verseTagMenu")->property("opened").toBool());
        auto wide = item("verseTagAction:John 3:4–6"); QTRY_VERIFY(wide && wide->isVisible());
        click(wide); QTRY_VERIFY(object("tagsUi")->property("scopePinned").toBool());
        QCOMPARE(bible->selectionReference(), QString("John 3:5"));
        key(Qt::Key_Escape); QTRY_VERIFY(!object("tagsUi")->property("scopePinned").toBool());
    }
    void notesTabReachesLinkedFrom() {
        QVERIFY(bible->navigate("John 1:5")); bible->selectVerses(5, 5); bible->setNoteText("See John 3:16 here.");
        QVERIFY(bible->navigate("John 1:7")); bible->selectVerses(7, 7); bible->setNoteText("Also John 3:16.");
        navigate("John 3:16"); bible->selectVerses(16, 16); call("readerFocus");
        key(Qt::Key_W); QTRY_VERIFY(opened("notesDrawer")); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        // No Highlight or Copy buttons in the panel.
        QVERIFY(!item("notesCopy"));
        // Tab types a tab; shift tab takes it back; the editor keeps the keyboard.
        key(Qt::Key_Tab); QCOMPARE(item("noteEditor")->property("text").toString(), QString("\t"));
        QVERIFY(item("noteEditor")->hasActiveFocus());
        key(Qt::Key_Tab, Qt::ShiftModifier); QCOMPARE(item("noteEditor")->property("text").toString(), QString());
        QVERIFY(item("noteEditor")->hasActiveFocus());
        // The editor hint names ctrl ↓; it moves to the list and never types.
        QTRY_VERIFY(item("notesHintsPart:linked from"));
        QVERIFY(object("notesHints")->property("plainText").toString().contains(QStringLiteral("ctrl ↓ linked from")));
        key(Qt::Key_Down, Qt::ControlModifier); QTRY_VERIFY(item("linkedFrom")->hasActiveFocus());
        QCOMPARE(item("noteEditor")->property("text").toString(), QString());
        QVERIFY(item("linkedHints")->isVisible());
        QCOMPARE(item("linkedFromRowBackground0")->property("color").value<QColor>().alphaF() > 0, true);
        key(Qt::Key_J); QCOMPARE(item("linkedFrom")->property("currentIndex").toInt(), 1);
        key(Qt::Key_K); QCOMPARE(item("linkedFrom")->property("currentIndex").toInt(), 0);
        // esc returns to the note (Notes stays open); ctrl ↑ too.
        key(Qt::Key_Escape); QTRY_VERIFY(item("noteEditor")->hasActiveFocus()); QVERIFY(opened("notesDrawer"));
        key(Qt::Key_Down, Qt::ControlModifier); QTRY_VERIFY(item("linkedFrom")->hasActiveFocus());
        key(Qt::Key_Up, Qt::ControlModifier); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        // Enter opens the linking note; Notes stays open on it.
        key(Qt::Key_Down, Qt::ControlModifier); QTRY_VERIFY(item("linkedFrom")->hasActiveFocus());
        key(Qt::Key_Return); QTRY_COMPARE(bible->selectionReference(), QString("John 1:5"));
        QVERIFY(opened("notesDrawer"));
        // [[ suggestions: tab accepts the suggestion instead of typing.
        QVERIFY(QMetaObject::invokeMethod(window, "closeNotes")); QTRY_VERIFY(!shown("notesDrawer"));
        navigate("John 3:16"); bible->selectVerses(16, 16); key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        type("[[jn 3:1"); QTRY_VERIFY(prop("linkSuggesting").toBool());
        key(Qt::Key_Tab); QTRY_VERIFY(!prop("linkSuggesting").toBool());
        QVERIFY(item("noteEditor")->hasActiveFocus());
        QVERIFY(item("noteEditor")->property("text").toString().startsWith("John 3:1"));
        QVERIFY(!item("noteEditor")->property("text").toString().contains('\t'));
        // Mouse: the editor's hint, the list's open hint, and a click on a row.
        item("noteEditor")->setProperty("text", QString());
        clickHint("notesHintsPart:linked from"); QTRY_VERIFY(item("linkedFrom")->hasActiveFocus());
        key(Qt::Key_J); clickHint("linkedHintsPart:open"); QTRY_COMPARE(bible->selectionReference(), QString("John 1:7"));
        navigate("John 3:16"); bible->selectVerses(16, 16); QTRY_VERIFY(item("linkedFromRow0") && item("linkedFromRow0")->isVisible());
        QTest::qWait(50); click(item("linkedFromRow0")); QTRY_COMPARE(bible->selectionReference(), QString("John 1:5"));
    }
    // While typing a note, the palette and dialog launchers (Ctrl, F1) work
    // again: each saves the draft and opens beside Notes, which stays; closing
    // it returns to the note. Plain keys (? included) still type. A draft that
    // cannot save keeps Notes as it is, and nothing opens.
    void notesEditorLaunchers() {
        struct Launcher { Qt::Key key; Qt::KeyboardModifiers modifiers; const char *surface; };
        const QList<Launcher> launchers = {
            {Qt::Key_K, Qt::ControlModifier, "passagePalette"}, {Qt::Key_Slash, Qt::ControlModifier, "passagePalette"},
            {Qt::Key_Space, Qt::ControlModifier, "passagePalette"}, {Qt::Key_Comma, Qt::ControlModifier, "settingsDialog"},
            {Qt::Key_F1, Qt::NoModifier, "helpDialog"}, {Qt::Key_T, Qt::ControlModifier, "tagsSurface"}};
        int n = 0;
        for (const auto &launcher : launchers) {
            const QString tag = QString("%1 %2").arg(launcher.surface).arg(n);
            navigate("John 3:16"); bible->selectVerses(16, 16); call("readerFocus");
            key(Qt::Key_W); QTRY_VERIFY2(item("noteEditor")->hasActiveFocus(), qPrintable(tag));
            item("noteEditor")->setProperty("text", QString());
            const QString draft = QString("Draft %1").arg(++n);
            type(draft.toLower());
            key(launcher.key, launcher.modifiers);
            QTRY_VERIFY2(shown(launcher.surface), qPrintable(tag));
            QVERIFY2(shown("notesDrawer"), qPrintable(tag));
            QCOMPARE(bible->noteText(), draft.toLower());
            key(Qt::Key_Escape); QTRY_VERIFY2(!shown(launcher.surface), qPrintable(tag));
            if (shown("tagsSurface")) { key(Qt::Key_Escape); QTRY_VERIFY(!shown("tagsSurface")); }
            QTRY_VERIFY2(item("noteEditor")->hasActiveFocus(), qPrintable(tag));
            key(Qt::Key_Escape); QTRY_VERIFY(readerFocused());
        }
        // Plain keys type: ? stays in the note; no Keybindings.
        key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        item("noteEditor")->setProperty("text", QString());
        QTest::keyClick(window, '?'); QTest::qWait(60);
        QVERIFY(!shown("helpDialog")); QCOMPARE(item("noteEditor")->property("text").toString(), QString("?"));
        QVERIFY(shown("notesDrawer"));
        // A draft that cannot save (changed in another window): Notes stays, nothing opens.
        window->setProperty("noteConflict", true); item("noteEditor")->setProperty("text", QString("unsaved draft")); QTest::qWait(60);
        QTRY_VERIFY(item("noteEditor")->hasActiveFocus()); QVERIFY(window->property("noteConflict").toBool());
        key(Qt::Key_K, Qt::ControlModifier); QTest::qWait(120);
        QVERIFY(!shown("passagePalette")); QVERIFY(shown("notesDrawer"));
        key(Qt::Key_S, Qt::ControlModifier); QTRY_VERIFY(!window->property("noteConflict").toBool());
        QVERIFY(QMetaObject::invokeMethod(window, "closeNotes")); QTRY_VERIFY(!shown("notesDrawer"));
    }
    // Notes in Write mode keeps the editor focused: after clicks on the panel's
    // chrome, after its menus and dialogs close, and after Preview → Write.
    void notesEditorKeepsTheKeyboard() {
        navigate("John 3:16"); bible->selectVerses(16, 16); key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        auto editor = item("noteEditor");
        // A click on the panel background, its title, and the Write button.
        auto panel = item("notesPanel");
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, panel->mapToScene(QPointF(panel->width() - 6, 6)).toPoint());
        QTRY_VERIFY(editor->hasActiveFocus());
        click(item("writeButton")); QTRY_VERIFY(editor->hasActiveFocus());
        // Preview, then back to Write by click and by key.
        click(item("previewButton")); QTRY_VERIFY(prop("notePreview").toBool()); QVERIFY(!editor->hasActiveFocus());
        click(item("writeButton")); QTRY_VERIFY(editor->hasActiveFocus());
        key(Qt::Key_V, Qt::AltModifier); QTRY_VERIFY(prop("notePreview").toBool());
        key(Qt::Key_V, Qt::AltModifier); QTRY_VERIFY(!prop("notePreview").toBool()); QTRY_VERIFY(editor->hasActiveFocus());
        // The editor's own menu, and a dialog over Notes.
        QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, editor->mapToScene(QPointF(20, 10)).toPoint());
        QTRY_VERIFY(menu()->property("opened").toBool()); key(Qt::Key_Escape); QTRY_VERIFY(!menu()->property("visible").toBool());
        QTRY_VERIFY(editor->hasActiveFocus());
        run("help"); QTRY_VERIFY(shown("helpDialog")); key(Qt::Key_Escape); QTRY_VERIFY(!shown("helpDialog"));
        QTRY_VERIFY(editor->hasActiveFocus());
        // A click on a label in the panel too; typing goes on in the editor.
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, panel->mapToScene(QPointF(40, 30)).toPoint());
        QTRY_VERIFY(editor->hasActiveFocus());
        type("kept"); QCOMPARE(bible->noteText(), QString("kept"));
    }
    // Previous / next chapter buttons at the reader's outer edges.
    void chapterEdgeButtons() {
        window->resize(1400, 820); QTRY_COMPARE(window->width(), 1400);
        navigate("John 2:5"); QTest::qWait(80);
        auto previous = item("chapterPreviousButton"), next = item("chapterNextButton");
        QVERIFY(previous && next);
        QTRY_VERIFY(previous->isVisible() && next->isVisible());
        QCOMPARE(previous->opacity(), 1.0); QVERIFY(previous->isEnabled() && next->isEnabled());
        auto rect = [](QQuickItem *target) { return target->mapRectToScene(QRectF(0, 0, target->width(), target->height())); };
        const auto column = rect(item("readerColumn")), surface = rect(item("readerSurface"));
        // Outer edges, vertically centred in the viewport, clear of the text.
        QVERIFY(rect(previous).left() < 12 * prop("controlScale").toDouble() + 1);
        QVERIFY(rect(next).right() > surface.right() - 12 * prop("controlScale").toDouble() - item("readerScrollbar")->width() - 1);
        const double middle = surface.top() + (surface.height() - prop("floatingSafety").toDouble()) / 2;
        QVERIFY(qAbs(rect(previous).center().y() - middle) <= 1); QVERIFY(qAbs(rect(next).center().y() - middle) <= 1);
        QVERIFY(rect(previous).right() <= column.left()); QVERIFY(rect(next).left() >= column.right());
        // Quiet: no background until hovered; tooltips name h / l.
        auto background = [](QQuickItem *button) { return button->property("background").value<QQuickItem *>()->property("color").value<QColor>(); };
        QCOMPARE(background(previous).alpha(), 0);
        QTest::mouseMove(window, center(previous)); QTRY_VERIFY(background(previous).alpha() > 0);
        QTRY_VERIFY(toolTipOf(previous)->property("visible").toBool());
        // The tips name the destination passage and its key.
        QCOMPARE(bible->chapter(), 2);
        QVERIFY2(previous->property("tip").toString().startsWith("John 1 · h"), qPrintable(previous->property("tip").toString()));
        QVERIFY(next->property("tip").toString().startsWith("John 3 · l"));
        shot("chapter-buttons");
        // Mouse: a click steps the chapter; the jump stack (History) is untouched.
        const auto stack = prop("jumpStack").toList(); const int at = prop("jumpIndex").toInt();
        click(next); QTRY_COMPARE(bible->chapter(), 3); QVERIFY(readerFocused());
        click(previous); QTRY_COMPARE(bible->chapter(), 2);
        click(previous); QTRY_COMPARE(bible->chapter(), 1);
        QCOMPARE(prop("jumpStack").toList(), stack); QCOMPARE(prop("jumpIndex").toInt(), at);
        // Bible start and end: disabled.
        // Across books (the fixture has Genesis 1–2 and John 1–3).
        navigate("Genesis 2:1"); QTRY_COMPARE(bible->chapter(), 2);
        QTRY_VERIFY(next->property("tip").toString().startsWith("John 1 · l"));
        QVERIFY(previous->property("tip").toString().startsWith("Genesis 1 · h"));
        navigate("John 1:1"); QTRY_VERIFY(previous->property("tip").toString().startsWith("Genesis 2 · h"));
        navigate("Genesis 1:1"); QTRY_VERIFY(!previous->isEnabled()); QVERIFY(next->isEnabled());
        QCOMPARE(previous->property("tip").toString(), QString("First chapter"));
        QVERIFY(next->property("tip").toString().startsWith("Genesis 2 · l"));
        click(previous); QCOMPARE(bible->book(), 1); QCOMPARE(bible->chapter(), 1);
        navigate("John 3:1"); QTRY_VERIFY(!next->isEnabled()); QVERIFY(previous->isEnabled());
        QCOMPARE(next->property("tip").toString(), QString("Last chapter"));
        // Keyboard: never tab stops (h / l are the keys).
        call("readerFocus");
        for (int i = 0; i < 40; ++i) { key(Qt::Key_Tab); QVERIFY(focusName() != "chapterPreviousButton" && focusName() != "chapterNextButton"); }
        call("readerFocus"); key(Qt::Key_H); QTRY_COMPARE(bible->chapter(), 2);
        // The outline and margin notes keep clear of them.
        bible->setChapterOutline(true); bible->selectVerses(5, 5); bible->setNoteText("A margin note here.");
        bible->setMarginNotes(true); QTest::qWait(200);
        QTRY_VERIFY(item("chapterOutline")->isVisible());
        QVERIFY(!rect(item("chapterOutline")).intersects(rect(previous)));
        if (auto note = item("marginNote:" + bible->selectionReference()); note && note->isVisible()) QVERIFY(!rect(note).intersects(rect(next)));
        QVERIFY(previous->isVisible() && next->isVisible());
        shot("chapter-buttons-margins");
        bible->setMarginNotes(false); bible->setChapterOutline(false);
        // Focus mode: hidden until the pointer is over one; a click right then is ignored.
        QTest::mouseMove(window, QPoint(700, 300)); call("readerFocus");
        key(Qt::Key_F); QVERIFY(bible->focusMode()); QTRY_COMPARE(next->opacity(), 0.0); QVERIFY(!next->isEnabled());
        QTest::mouseMove(window, center(previous)); QTRY_COMPARE(previous->opacity(), 1.0); QVERIFY(previous->isEnabled());
        const int chapter = bible->chapter();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center(previous)); QCoreApplication::processEvents();
        QCOMPARE(bible->chapter(), chapter);
        QTest::qWait(350); click(previous); QTRY_COMPARE(bible->chapter(), chapter - 1);
        QTest::mouseMove(window, QPoint(700, 300)); QTRY_COMPARE(previous->opacity(), 0.0);
        key(Qt::Key_F); QVERIFY(!bible->focusMode());
        // Narrow windows: hidden rather than over the text.
        window->resize(700, 820); QTRY_COMPARE(window->width(), 700);
        QTRY_VERIFY(!previous->isVisible()); QVERIFY(!next->isVisible());
        shot("chapter-buttons-narrow");
        window->resize(1200, 820);
    }
    // Highlights look like a highlighter pen: translucent ink bands about the
    // text's x-height (never the full line), uneven and deterministic, in
    // every layout and the Highlights tab; text and layout never change.
    void highlighterLook() {
        // Omarchy themes (colors.toml under XDG_STATE_HOME): a dark and a light one.
        const QList<QPair<QString, QByteArray>> themes = {
            {"dark", "background = \"#1e1e2e\"\nforeground = \"#cdd6f4\"\naccent = \"#89b4fa\"\ncolor3 = \"#f9e2af\"\n"},
            {"light", "background = \"#faf8f2\"\nforeground = \"#262626\"\naccent = \"#3a6fb0\"\ncolor3 = \"#e0b000\"\n"}};
        // Never the user's live theme: the state directory must be the test's own.
        QVERIFY(TestEnvironment::isolated());
        const QString themeDir = qEnvironmentVariable("XDG_STATE_HOME") + "/omarchy/current/theme";
        QVERIFY(!TestEnvironment::reachesRealUserState(themeDir));
        QVERIFY(QDir().mkpath(themeDir));
        navigate("John 3:2"); bible->selectVerses(2, 4); QVERIFY(bible->toggleHighlight());
        bible->selectVerses(16, 16); QVERIFY(bible->toggleHighlight());
        bible->clearSelection();
        for (const auto &theme : themes) {
            { QFile file(themeDir + "/colors.toml"); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); file.write(theme.second); }
            QVERIFY(QMetaObject::invokeMethod(desktop.get(), "refresh"));
            QTRY_COMPARE(desktop->property("dark").toBool(), theme.first == "dark");
            for (const QString mode : {"verse", "paragraph", "block"}) {
                setMode(mode); navigate("John 3:1"); bible->clearSelection(); call("readerFocus");
                QTest::qWait(250);
                shot("highlight-" + theme.first + "-" + mode);
                if (!qEnvironmentVariableIsSet("OMAWORD_HIGHLIGHT_BEFORE")) {
                    // The band sits about the x-height: taller than it, shorter than a line.
                    const auto band = value(window->property("highlightBand")).toMap();
                    const double line = prop("readerLineHeight").toDouble();
                    QVERIFY2(band["height"].toDouble() > band["xHeight"].toDouble() && band["height"].toDouble() < 0.8 * line,
                             qPrintable(QString("%1 %2 %3").arg(band["height"].toDouble()).arg(band["xHeight"].toDouble()).arg(line)));
                    // Deterministic: a repaint draws the same pixels.
                    // Settle first (fades, slides), comparing the reader column only.
                    const QRect column = item("readerColumn")->mapRectToScene(QRectF(0, 0, item("readerColumn")->width(), 400)).toAlignedRect()
                        .intersected(QRect(QPoint(), window->size()));
                    QImage first = window->grabWindow().copy(column);
                    for (int i = 0; i < 20; ++i) {
                        QTest::qWait(100);
                        const QImage next = window->grabWindow().copy(column);
                        if (next == first) break;
                        first = next;
                    }
                    call("refreshHighlights");
                    QTRY_VERIFY_WITH_TIMEOUT(window->grabWindow().copy(column) == first, 2000);
                }
            }
            setMode("verse");
            run("annotations"); QTRY_VERIFY(panelOpened("marks")); QTest::qWait(200);
            shot("highlight-" + theme.first + "-panel");
            if (!qEnvironmentVariableIsSet("OMAWORD_HIGHLIGHT_BEFORE")) {
                auto mark = item("savedHighlightJohn 3:16"); QVERIFY(mark && mark->isVisible());
                // Behind the row's reference, not a bar at the row's edge.
                auto label = item("savedReferenceJohn 3:16"); QVERIFY(label);
                const auto m = mark->mapRectToScene(QRectF(0, 0, mark->width(), mark->height())), l = label->mapRectToScene(QRectF(0, 0, label->width(), label->height()));
                QVERIFY(m.intersects(l)); QVERIFY(m.width() > 20);
            }
            closeMargin();
        }
        QVERIFY(!TestEnvironment::reachesRealUserState(themeDir));
        QFile::remove(themeDir + "/colors.toml"); QVERIFY(QMetaObject::invokeMethod(desktop.get(), "refresh"));
    }
    void focusModeFooterRevealsInPlace() {
        const QPointF searchAt = item("searchControl")->mapToScene(QPointF());
        const QPointF deskAt = item("deskControl")->mapToScene(QPointF());
        // Keyboard: f enters focus mode; the footer stays with only the focus button.
        key(Qt::Key_F); QVERIFY(bible->focusMode()); QVERIFY(prop("footerQuiet").toBool());
        QTRY_COMPARE(item("searchControl")->opacity(), 0.0);
        QVERIFY(!item("searchControl")->isEnabled()); QVERIFY(!item("deskControl")->isEnabled());
        QCOMPARE(item("focusControl")->opacity(), 1.0); QVERIFY(item("focusControl")->isEnabled());
        QCOMPARE(item("focusControl")->property("selected").toBool(), true);
        QCOMPARE(item("searchControl")->mapToScene(QPointF()), searchAt);
        QCOMPARE(item("deskControl")->mapToScene(QPointF()), deskAt);
        QTest::mouseMove(window, QPoint(window->width() / 2, 200));
        shot("focus-footer-quiet");
        // Tab reaches the focus button and reveals the rest at once, in place.
        bool reached = false;
        for (int i = 0; i < 30 && !reached; ++i) { key(Qt::Key_Tab); reached = focusName() == "focusControl"; }
        QVERIFY(reached); QVERIFY(!prop("footerQuiet").toBool()); QVERIFY(item("searchControl")->isEnabled());
        QVERIFY(!prop("footerGuard").toBool());
        key(Qt::Key_Tab); QCOMPARE(focusName(), QString("textControl")); QVERIFY(!prop("footerQuiet").toBool());
        key(Qt::Key_Tab); QCOMPARE(focusName(), QString("modeControl")); QVERIFY(!prop("footerQuiet").toBool());
        key(Qt::Key_Tab); QCOMPARE(focusName(), QString("helpControl"));
        key(Qt::Key_Tab); QCOMPARE(focusName(), QString("searchControl")); QVERIFY(!prop("footerQuiet").toBool());
        call("readerFocus"); QVERIFY(prop("footerQuiet").toBool());
        // Space on the focused focus button leaves focus mode.
        for (int i = 0; i < 30 && focusName() != "focusControl"; ++i) key(Qt::Key_Tab);
        key(Qt::Key_Space); QVERIFY(!bible->focusMode()); QVERIFY(!prop("footerQuiet").toBool());
        QCOMPARE(item("searchControl")->mapToScene(QPointF()), searchAt);
        // Mouse: the pointer on the footer reveals it at once; a click in the next 300 ms is ignored.
        call("readerFocus"); key(Qt::Key_F); QVERIFY(bible->focusMode());
        QTest::mouseMove(window, center(item("modeControl")));
        QVERIFY(!prop("footerQuiet").toBool()); QVERIFY(prop("footerGuard").toBool());
        click(item("modeControl")); QVERIFY(!prop("verseMode").toBool());
        QTRY_VERIFY(!prop("footerGuard").toBool());
        QTRY_COMPARE(item("modeControl")->opacity(), 1.0);
        shot("focus-footer-revealed");
        click(item("modeControl")); QVERIFY(prop("verseMode").toBool());
        QVERIFY(readerFocused());
        key(Qt::Key_Escape); QVERIFY(!prop("verseMode").toBool());
        QTest::mouseMove(window, QPoint(window->width() / 2, 200)); QTRY_VERIFY(prop("footerQuiet").toBool());
        // The focus button by mouse; the passage menu offers Leave focus mode (f).
        key(Qt::Key_Menu); QTRY_VERIFY(menu()->property("opened").toBool());
        QCOMPARE(menuIds().last(), QString("leaveFocus")); QCOMPARE(menuShortcut("leaveFocus"), QString("f"));
        chooseMenu("leaveFocus"); QVERIFY(!bible->focusMode());
        key(Qt::Key_F); QVERIFY(bible->focusMode());
        // Straight onto the focus button: the reveal is immediate, the guard
        // keeps the new buttons inert, and the focus button itself still acts.
        QTest::mouseMove(window, QPoint(window->width() / 2, 200)); QTRY_VERIFY(prop("footerQuiet").toBool());
        QTest::mouseMove(window, center(item("focusControl"))); QVERIFY(!prop("footerQuiet").toBool());
        QVERIFY(prop("footerGuard").toBool());
        click(item("focusControl")); QVERIFY(!bible->focusMode()); QVERIFY(readerFocused());
        // A cramped footer keeps its folds: the focus button shows only in focus mode there.
        desktop->setProperty("textScale", 1.8); window->resize(640, 480); QTRY_COMPARE(window->width(), 640);
        QTRY_VERIFY(!item("focusControl")->isVisible()); QVERIFY(item("savedControl")->isVisible());
        key(Qt::Key_F); QVERIFY(bible->focusMode()); QTRY_VERIFY(item("focusControl")->isVisible());
        key(Qt::Key_F); QVERIFY(!bible->focusMode()); QTRY_VERIFY(!item("focusControl")->isVisible());
    }
    void chapterFindByClickAndTab() {
        auto find = [&](const QString &query) {
            key(Qt::Key_Slash); QTRY_VERIFY(prop("findPromptOpen").toBool());
            QTRY_VERIFY(item("chapterFindInput")->hasActiveFocus()); type(query); QTest::qWait(220);
        };
        // Mouse: a click in the reader accepts the current match; the click selects nothing.
        find("text 3:2 with");
        const QPoint elsewhere(window->width() / 2, window->height() / 2);
        clickAt(elsewhere); QTRY_VERIFY(!prop("findPromptOpen").toBool());
        QCOMPARE(prop("acceptedSearchQuery").toString(), QString("text 3:2 with"));
        QCOMPARE(bible->selectionReference(), QString("John 3:2"));
        // Nothing matches: the click cancels.
        key(Qt::Key_Escape); find("zzzz");
        clickAt(elsewhere); QTRY_VERIFY(!prop("findPromptOpen").toBool());
        QVERIFY(prop("acceptedSearchQuery").toString().isEmpty()); QCOMPARE(bible->selectionReference(), QString("John 3:2"));
        // The hints are buttons: enter accept, esc cancel.
        find("text 3:5 with"); clickHint("chapterFindHintsPart:accept"); QTRY_VERIFY(!prop("findPromptOpen").toBool());
        QCOMPARE(bible->selectionReference(), QString("John 3:5"));
        find("text 3:9 with"); clickHint("chapterFindHintsPart:cancel"); QTRY_VERIFY(!prop("findPromptOpen").toBool());
        QCOMPARE(bible->selectionReference(), QString("John 3:5"));
        // Keyboard: tab cycles prompt → accept → cancel → prompt (the field keeps
        // the keyboard; the marked hint shows a focus ring); shift tab goes back.
        auto marked = [&] { return object("chapterFind")->property("hintFocus").toInt(); };
        find("text 3:7 with");
        key(Qt::Key_Tab); QCOMPARE(marked(), 0); QVERIFY(item("chapterFindHintsPart:accept:tint")->isVisible());
        key(Qt::Key_Tab); QCOMPARE(marked(), 1); QVERIFY(item("chapterFindHintsPart:cancel:tint")->isVisible());
        QVERIFY(!item("chapterFindHintsPart:accept:tint")->isVisible());
        key(Qt::Key_Tab); QCOMPARE(marked(), -1);
        key(Qt::Key_Tab, Qt::ShiftModifier); QCOMPARE(marked(), 1);
        key(Qt::Key_Return); QTRY_VERIFY(!prop("findPromptOpen").toBool()); QCOMPARE(bible->selectionReference(), QString("John 3:5"));
        find("text 3:7 with"); key(Qt::Key_Tab); key(Qt::Key_Space);
        QTRY_VERIFY(!prop("findPromptOpen").toBool()); QCOMPARE(bible->selectionReference(), QString("John 3:7"));
        QVERIFY(readerFocused());
        // Typing while a hint is marked returns to the field and types.
        find("text 3:9"); key(Qt::Key_Tab); QCOMPARE(marked(), 0); type("x"); QCOMPARE(marked(), -1);
        QCOMPARE(item("chapterFindInput")->property("text").toString(), QString("text 3:9x"));
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("findPromptOpen").toBool());
        // Esc with a hint marked cancels too.
        find("text 3:9 with"); key(Qt::Key_Tab); key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("findPromptOpen").toBool()); QCOMPARE(bible->selectionReference(), QString("John 3:7"));
    }
    void mouseBackAndForwardButtons() {
        auto jumpTo = [&](const QString &reference) { call("jump", reference); QCOMPARE(bible->selectionReference(), reference); };
        jumpTo("John 1:10"); jumpTo("John 2:5");
        const QPoint middle(window->width() / 2, window->height() / 2);
        // Reader: mouse back and forward travel the jumps, like [ and ].
        clickAt(middle, Qt::BackButton); QCOMPARE(bible->selectionReference(), QString("John 1:10"));
        clickAt(middle, Qt::ForwardButton); QCOMPARE(bible->selectionReference(), QString("John 2:5"));
        key(Qt::Key_BracketLeft); QCOMPARE(bible->selectionReference(), QString("John 1:10"));
        key(Qt::Key_BracketRight); QCOMPARE(bible->selectionReference(), QString("John 2:5"));
        // Palette: back returns from a book's chapters to the book list.
        run("books"); QTRY_VERIFY(opened("passagePalette")); type("genesis"); key(Qt::Key_Return);
        QTRY_VERIFY(prop("paletteBook").toInt() > 0);
        clickAt(middle, Qt::BackButton); QCOMPARE(prop("paletteBook").toInt(), 0); QVERIFY(opened("passagePalette"));
        QCOMPARE(bible->selectionReference(), QString("John 2:5"));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("passagePalette"));
        // A side panel: back closes it (and does not travel); the Margin too.
        run("annotations"); QTRY_VERIFY(panelOpened("marks"));
        clickAt(middle, Qt::BackButton); QTRY_VERIFY(!shown("notesDrawer"));
        QCOMPARE(bible->selectionReference(), QString("John 2:5")); QVERIFY(readerFocused());
        run("desk"); QTRY_VERIFY(opened("notesDrawer"));
        clickAt(QPoint(40, window->height() / 2), Qt::BackButton); QTRY_VERIFY(!shown("notesDrawer"));
        QCOMPARE(bible->selectionReference(), QString("John 2:5")); QVERIFY(readerFocused());
        // Keyboard equivalent for the panel: esc.
        run("annotations"); QTRY_VERIFY(panelOpened("marks")); closeMargin();
        // The keybindings list explains the buttons.
        QVariant text; QMetaObject::invokeMethod(window, "keyText", Q_RETURN_ARG(QVariant, text), Q_ARG(QVariant, QString("mouseBack")));
        QCOMPARE(text.toString(), QString("mouse back"));
    }
    void rightClickKeepsMouseSelectedText() {
        for (const QString mode : {"verse", "block"}) {
            setMode(mode); navigate("John 3:3"); bible->clearSelection(); call("readerFocus");
            QQuickItem *body = mode == "verse" ? item("verseBody4") : item("blockBody");
            QVERIFY(body);
            QPoint from, to;
            if (mode == "verse") { from = body->mapToScene(QPointF(10, 8)).toPoint(); to = body->mapToScene(QPointF(200, 8)).toPoint(); }
            else {
                QRectF rect; auto blockBody = item("blockBody");
                QMetaObject::invokeMethod(blockBody, "positionToRectangle", Q_RETURN_ARG(QRectF, rect), Q_ARG(int, window->findChild<BlockDocument *>()->positionFor(4) + 2));
                from = blockBody->mapToScene(rect.center()).toPoint(); to = QPoint(from.x() + 200, from.y());
            }
            // Mouse: drag over text (a character selection), then right-click on it.
            drag(from, to);
            QTRY_VERIFY2(prop("charMode").toBool(), qPrintable(mode));
            const QString selected = prop("freeSelectedText").toString(); QVERIFY(!selected.isEmpty());
            const auto selection = bible->tagScope();
            clickAt((from + to) / 2, Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
            QCOMPARE(menuIds().first(), QString("copySelected")); QCOMPARE(menuShortcut("copySelected"), QString("ctrl c"));
            QCOMPARE(prop("freeSelectedText").toString(), selected); QCOMPARE(bible->tagScope(), selection);
            // Copy selected text is a quotation (ctrl c), which, as y, ends
            // the selection; the plain entry follows.
            QGuiApplication::clipboard()->clear(); chooseMenu("copySelected");
            QTRY_VERIFY(QGuiApplication::clipboard()->text().endsWith(QStringLiteral("”\n\n— John 3:4")));
            QVERIFY(!prop("charMode").toBool()); QCOMPARE(bible->tagScope(), selection);
            // Keyboard: dragged again, the menu key offers it first too; enter copies.
            drag(from, to); QTRY_COMPARE(prop("freeSelectedText").toString(), selected);
            key(Qt::Key_Menu); QTRY_VERIFY(menu()->property("opened").toBool());
            QCOMPARE(menuIds().first(), QString("copySelected"));
            QGuiApplication::clipboard()->clear(); key(Qt::Key_Down); key(Qt::Key_Return);
            QTRY_COMPARE(QGuiApplication::clipboard()->text(), selected);
            QTRY_VERIFY(prop("freeSelectedText").toString().isEmpty());
        }
    }

    // ── Phase 3

    void characterSelectionByKeysInEveryLayout() {
        for (const QString mode : {"verse", "block", "paragraph"}) {
            setMode(mode); navigate("John 3:16"); call("readerFocus");
            const QString v16 = verseText(16), v17 = verseText(17);
            QCOMPARE(v16, QString("For God so loved the world that he gave his Son."));
            key(Qt::Key_V);
            QTRY_VERIFY2(prop("charMode").toBool(), qPrintable(mode));
            QVERIFY(charAt(charAnchor(), 16, 0)); QVERIFY(charAt(charCursor(), 16, 0));
            QCOMPARE(prop("freeSelectedText").toString(), QString("F"));
            QVERIFY(shown("characterStatus")); QCOMPARE(item("modeControl")->property("glyph").toString(), QString("characters"));
            // The selection paints with the layout's native selection.
            QQuickItem *body = mode == "verse" ? item("verseBody16") : item("blockBody");
            QCOMPARE(body->property("selectedText").toString(), QString("F"));
            // Words, ends, counts, swap.
            keys("w"); QVERIFY(charAt(charCursor(), 16, 4)); QCOMPARE(prop("freeSelectedText").toString(), QString("For G"));
            keys("2e"); QVERIFY(charAt(charCursor(), 16, 9));
            QCOMPARE(body->property("selectedText").toString().remove(QChar(0x2060)).remove(QChar(0xFFFC)), QString("For God so"));
            keys("o"); QVERIFY(charAt(charAnchor(), 16, 9)); QVERIFY(charAt(charCursor(), 16, 0));
            keys("o"); QVERIFY(charAt(charCursor(), 16, 9));
            keys("b"); QVERIFY(charAt(charCursor(), 16, 8));
            keys("ge"); QVERIFY(charAt(charCursor(), 16, 6));
            keys("W"); QVERIFY(charAt(charCursor(), 16, 8)); keys("E"); QVERIFY(charAt(charCursor(), 16, 9)); keys("B"); QVERIFY(charAt(charCursor(), 16, 8));
            // Line edges (no counts) follow the visual line: a whole verse row,
            // or a wrapped line of the flowing text.
            auto cursorPoint = [&] { return charPoint(charCursor()["verse"].toInt(), charCursor()["offset"].toInt()); };
            const qreal left = body->mapToScene(QPointF(0, 0)).x();
            keys("$");
            if (mode == "verse") QVERIFY(charAt(charCursor(), 16, v16.size() - 1));
            else {
                // The wrapped line ends on its last ink; the next word is on the next line.
                const QPoint end = cursorPoint(); keys("w"); QVERIFY(cursorPoint().y() > end.y()); keys("ge");
                QCOMPARE(cursorPoint(), end);
                // Where that line ends depends on the column: start again at 16.
                key(Qt::Key_Escape); QTRY_VERIFY(!prop("charMode").toBool());
                key(Qt::Key_V); QTRY_VERIFY(prop("charMode").toBool()); QVERIFY(charAt(charCursor(), 16, 0));
            }
            // h and l cross the verse break; ) and ( step sentences.
            keys(")"); QVERIFY(charAt(charCursor(), 17, 0));
            keys("h"); QVERIFY(charAt(charCursor(), 16, v16.size() - 1));
            keys("l"); QVERIFY(charAt(charCursor(), 17, 0));
            keys("h"); QVERIFY(charAt(charCursor(), 16, v16.size() - 1));
            keys("0");
            if (mode == "verse") QVERIFY(charAt(charCursor(), 16, 0));
            QVERIFY2(cursorPoint().x() - left < 6, qPrintable(mode));
            const auto lineStart = charCursor();
            keys("3l"); keys("^"); QCOMPARE(charCursor(), lineStart);
            keys(")3l("); QVERIFY(charAt(charCursor(), 17, 0)); keys("("); QVERIFY(charAt(charCursor(), 16, 0));
            // A count of 20 (0 joins a count) moves 20 characters.
            keys("20l"); QVERIFY(charAt(charCursor(), 16, 20));
            // j and k move by line, keeping the column, across verses.
            keys("j"); QCOMPARE(charCursor()["verse"].toInt(), 17);
            const int column = charCursor()["offset"].toInt(); QVERIFY2(column > 5, qPrintable(mode + " " + QString::number(column)));
            keys("k"); QVERIFY2(charAt(charCursor(), 16, 20), qPrintable(mode + QString(" %1:%2").arg(charCursor()["verse"].toInt()).arg(charCursor()["offset"].toInt())));
            keys("2j"); QVERIFY(charCursor()["verse"].toInt() >= 17);
            keys("2k"); QVERIFY(charAt(charCursor(), 16, 20));
            // Sentences, chapter edges (counts ignored).
            keys(")"); QVERIFY(charAt(charCursor(), 17, 0));
            keys("("); QVERIFY(charAt(charCursor(), 16, 0));
            keys("5gg"); QVERIFY(charAt(charCursor(), 1, 0));
            keys("3G"); QVERIFY(charAt(charCursor(), 36, verseText(36).size() - 1));
            // An unknown key sounds the error and keeps the selection.
            const int plays = playCount(); keys("q"); QCOMPARE(lastSound(), QString("error")); QVERIFY(playCount() > plays);
            QVERIFY(prop("charMode").toBool());
            // esc leaves without copying.
            QGuiApplication::clipboard()->setText("unchanged");
            key(Qt::Key_Escape); QVERIFY(!prop("charMode").toBool());
            QVERIFY(prop("freeSelectedText").toString().isEmpty()); QCOMPARE(QGuiApplication::clipboard()->text(), QString("unchanged"));
            QVERIFY(readerFocused()); QVERIFY(!shown("characterStatus"));
            // Text objects, then y copies a quotation and leaves.
            navigate("John 3:16"); key(Qt::Key_V); QTRY_VERIFY(prop("charMode").toBool());
            keys("l"); keys("iw"); QVERIFY(charAt(charAnchor(), 16, 0)); QVERIFY(charAt(charCursor(), 16, 2));
            keys("aw"); QVERIFY(charAt(charCursor(), 16, 6));
            keys("y"); QVERIFY(!prop("charMode").toBool());
            QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("“For God …”\n\n— John 3:16"));
            QCOMPARE(lastSound(), QString("chime"));
            // Across a verse break: y quotes with the later verse's number, shift Y is plain.
            key(Qt::Key_V); QTRY_VERIFY(prop("charMode").toBool());
            keys("wo"); keys(")w");
            QVERIFY(charAt(charAnchor(), 16, 4)); QVERIFY(charAt(charCursor(), 17, 9));
            const QString plain = v16.mid(4) + " " + v17.left(10);
            key(Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier); QVERIFY(!prop("charMode").toBool());
            QCOMPARE(QGuiApplication::clipboard()->text(), plain);
            key(Qt::Key_V); keys("wo)w"); QVERIFY(charAt(charCursor(), 17, 9));
            key(Qt::Key_C, Qt::ControlModifier);
            QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("“… God so loved the world that he gave his Son. ¹⁷ Original a …”\n\n— John 3:16–17"));
            key(Qt::Key_V); keys(")h"); keys("Y");
            QCOMPARE(QGuiApplication::clipboard()->text(), v16);
            QVERIFY(warnings.isEmpty());
        }
    }
    void characterSelectionRendersInEveryLayout() {
        for (const QString mode : {"verse", "block", "paragraph"}) {
            setMode(mode); navigate("John 3:16"); call("readerFocus");
            key(Qt::Key_V); QTRY_VERIFY(prop("charMode").toBool());
            keys("w"); keys("o"); keys("$j");
            QTRY_VERIFY(shown("characterStatus"));
            QCOMPARE(charAnchor()["verse"].toInt(), 16);
            QCOMPARE(charCursor()["verse"].toInt(), 17);
            // Verse rows select each touched row; the flowing layouts one body.
            if (mode == "verse") {
                QCOMPARE(item("verseBody16")->property("selectedText").toString().remove(QChar(0x2060)).remove(QChar(0xFFFC)), verseText(16).mid(4));
                QVERIFY(!item("verseBody17")->property("selectedText").toString().isEmpty());
                QVERIFY(item("verseBody18")->property("selectedText").toString().isEmpty());
            } else QVERIFY(item("blockBody")->property("selectedText").toString().contains("God so loved"));
            shot("characters-" + mode);
            key(Qt::Key_Escape);
        }
    }
    // Text size and line height rebuild the layout but keep a character
    // selection: same source positions, same text, reselected natively.
    void characterSelectionSurvivesRelayout() {
        for (const QString mode : {"verse", "block", "paragraph"}) {
            setMode(mode); navigate("John 3:16"); bible->selectVerses(16, 16); call("readerFocus");
            auto native = [&] {
                if (mode != "verse") return item("blockBody")->property("selectedText").toString().remove(QChar(0x2060)).remove(QChar(0xFFFC)).simplified();
                QStringList parts;
                for (int verse = 1; verse <= 40; ++verse) if (auto body = item("verseBody" + QString::number(verse))) {
                    const auto text = body->property("selectedText").toString().remove(QChar(0x2060)).remove(QChar(0xFFFC)).simplified();
                    if (!text.isEmpty()) parts << text;
                }
                return parts.join(' ');
            };
            auto expectKept = [&](const QVariantMap &anchor, const QVariantMap &cursor, const QString &text, const QString &tag) {
                QTest::qWait(150);
                QVERIFY2(prop("charMode").toBool(), qPrintable(mode + " " + tag));
                QCOMPARE(charAnchor(), anchor); QCOMPARE(charCursor(), cursor);
                QCOMPARE(prop("freeSelectedText").toString(), text);
                // Flowing layouts select their verse-number labels too.
                QTRY_COMPARE(native().replace(QRegularExpression("(^|\\s)\\d+(?=\\s)"), "").simplified(), text.simplified());
            };
            // Keyboard: v, a few words across the verse break, then every size key.
            key(Qt::Key_V); QTRY_VERIFY(prop("charMode").toBool());
            keys("w)we");
            const auto anchor = charAnchor(), cursor = charCursor(); const QString text = prop("freeSelectedText").toString();
            QVERIFY2(text.contains(' ') && cursor["verse"].toInt() == 17, qPrintable(text));
            shot("relayout-" + mode + "-before");
            const int size = bible->fontSize(); const double line = bible->lineHeight();
            key(Qt::Key_Equal, Qt::ControlModifier); QCOMPARE(bible->fontSize(), size + 1); expectKept(anchor, cursor, text, "larger");
            shot("relayout-" + mode + "-larger");
            key(Qt::Key_Minus, Qt::ControlModifier | Qt::AltModifier); QVERIFY(bible->lineHeight() < line); expectKept(anchor, cursor, text, "line less");
            key(Qt::Key_Equal, Qt::ControlModifier | Qt::AltModifier); QCOMPARE(bible->lineHeight(), line); expectKept(anchor, cursor, text, "line more");
            key(Qt::Key_Minus, Qt::ControlModifier); QCOMPARE(bible->fontSize(), size); expectKept(anchor, cursor, text, "smaller");
            // Its keys still work afterwards.
            keys("w"); QVERIFY(charCursor() != cursor); key(Qt::Key_Escape); QVERIFY(!prop("charMode").toBool());
            // Mouse: a dragged selection, characters by itself, survives too.
            const QPoint from = charPoint(16, 4), to = charPoint(16, 16);
            drag(from, to); QTRY_VERIFY(prop("charMode").toBool());
            const auto dragAnchor = charAnchor(), dragCursor = charCursor(); const QString dragged = prop("freeSelectedText").toString();
            key(Qt::Key_Equal, Qt::ControlModifier | Qt::AltModifier); expectKept(dragAnchor, dragCursor, dragged, "mouse line more");
            key(Qt::Key_0, Qt::ControlModifier); expectKept(dragAnchor, dragCursor, dragged, "mouse reset size");
            key(Qt::Key_Minus, Qt::ControlModifier | Qt::AltModifier);
            key(Qt::Key_Escape); QVERIFY(!prop("charMode").toBool());
            bible->setFontSize(size); bible->setLineHeight(line);
        }
    }
    // A drag over the text, or a double-clicked word, begins character
    // selection quietly: the text holds still under the pointer, with no
    // toast and no selection bar. Its cursor is where the pointer let go, its
    // motions move that end, and esc returns to the verses it ran over.
    void dragAndDoubleClickSelectCharactersQuietly() {
        for (const QString mode : {"verse", "block", "paragraph"}) {
            setMode(mode); navigate("John 3:16"); bible->clearSelection(); call("readerFocus");
            object("toast")->setProperty("text", QString("before"));
            // Verse 16's first line just under the view's top, where a reveal would move it.
            QVERIFY(QMetaObject::invokeMethod(window, "scrollBy", Q_ARG(QVariant, 2 * prop("readerLineHeight").toDouble())));
            const QPoint start = charPoint(16, 4), end = charPoint(17, 7);
            QVERIFY2(!start.isNull() && !end.isNull(), qPrintable(mode));
            // Within a pixel: the reader rounds a fractional scroll as it resizes.
            const auto still = [&](const QPoint &at) { return (charPoint(16, 4) - at).manhattanLength() <= 1; };
            // Back from verse 17 into verse 16, the cursor near the view's top.
            drag(end, start); QTRY_VERIFY2(prop("charMode").toBool(), qPrintable(mode));
            QVERIFY(still(start)); QCOMPARE(toast(), QString("before"));
            // Still so once laid out with its bar and reselected there.
            QSignalSpy frames(window, &QQuickWindow::frameSwapped);
            for (int frame = 0; frame < 2; ++frame) {
                const int before = frames.count(); window->update();
                QVERIFY(QTest::qWaitFor([&] { return frames.count() > before; }, 1000));
            }
            QVERIFY(!object("readerSlide")->property("running").toBool()); QVERIFY(still(start));
            QVERIFY(!prop("selectionBarShown").toBool());
            QVERIFY(charAt(charCursor(), 16, 4)); QVERIFY(charAt(charAnchor(), 17, 6));
            const QString dragged = prop("freeSelectedText").toString();
            QVERIFY2(dragged.startsWith("God so loved") && dragged.endsWith(verseText(17).left(7)), qPrintable(dragged));
            // A motion moves that end (and, as a key, reveals it).
            keys("b"); QVERIFY(charAt(charCursor(), 16, 0));
            QCOMPARE(prop("freeSelectedText").toString(), "For " + dragged);
            QTRY_VERIFY(!object("readerSlide")->property("running").toBool());
            const QPoint shown = charPoint(16, 4);
            // esc returns to the verses it ran over, the text still.
            key(Qt::Key_Escape); QVERIFY(!prop("charMode").toBool());
            QVERIFY(prop("freeSelectedText").toString().isEmpty());
            QCOMPARE(bible->selectionReference(), QString::fromUtf8("John 3:16–17"));
            QVERIFY(still(shown));
            // A double-clicked word, extended by a word.
            QTest::mouseDClick(window, Qt::LeftButton, {}, charPoint(16, 5));
            QTRY_VERIFY2(prop("charMode").toBool(), qPrintable(mode));
            QCOMPARE(prop("freeSelectedText").toString(), QString("God")); QCOMPARE(toast(), QString("before"));
            keys("e"); QCOMPARE(prop("freeSelectedText").toString(), QString("God so"));
            key(Qt::Key_Escape); QVERIFY(!prop("charMode").toBool());
            QCOMPARE(bible->selectionReference(), QString("John 3:16"));
        }
    }
    void characterSelectionByMouse() {
        for (const QString mode : {"verse", "block"}) {
            setMode(mode); navigate("John 3:16"); bible->clearSelection(); call("readerFocus");
            // Mouse: a drag inside verse 16 is a character selection; ctrl c
            // copies it as a quotation with ellipses, which ends it.
            const QString v16 = verseText(16);
            const QPoint from = charPoint(16, 4), to = charPoint(16, 16);
            QVERIFY(!from.isNull() && !to.isNull());
            drag(from, to); QTRY_VERIFY(prop("charMode").toBool());
            QGuiApplication::clipboard()->clear();
            key(Qt::Key_C, Qt::ControlModifier);
            const QString quote = QGuiApplication::clipboard()->text();
            QVERIFY2(quote.startsWith(QStringLiteral("“… God so loved")) && quote.endsWith(QStringLiteral(" …”\n\n— John 3:16")), qPrintable(mode + ": " + quote));
            QCOMPARE(toast(), QString("Selected text copied as quotation"));
            QVERIFY(!prop("charMode").toBool());
            // ctrl shift C copies its plain text.
            drag(from, to); QTRY_VERIFY(prop("charMode").toBool());
            const QString dragged = prop("freeSelectedText").toString();
            key(Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
            QCOMPARE(QGuiApplication::clipboard()->text(), dragged);
            QVERIFY(dragged.startsWith("God so loved"));
            // The menu copies either way.
            drag(from, to); QTRY_VERIFY(prop("charMode").toBool());
            clickAt((from + to) / 2, Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
            QCOMPARE(menuIds().mid(0, 2), QStringList({"copySelected", "copySelectedPlain"}));
            QCOMPARE(menuShortcut("copySelectedPlain"), QString("ctrl shift C"));
            QGuiApplication::clipboard()->clear(); chooseMenu("copySelected");
            QTRY_VERIFY(QGuiApplication::clipboard()->text().startsWith(QStringLiteral("“… God")));
            // A drag across a verse break, its keys in the reader.
            const QPoint end = charPoint(17, 7);
            drag(from, end); QTRY_VERIFY(prop("charMode").toBool());
            QCOMPARE(charAnchor()["verse"].toInt(), 16); QCOMPARE(charCursor()["verse"].toInt(), 17);
            QVERIFY(readerFocused());
            // Mouse: the hint parts act; quotation copies (the later verse
            // numbered) and leaves.
            QGuiApplication::clipboard()->clear();
            clickHint("characterHintsPart:quotation");
            QTRY_VERIFY(!prop("charMode").toBool());
            QVERIFY2(QGuiApplication::clipboard()->text().contains(QStringLiteral("Son. ¹⁷ Origin")), qPrintable(QGuiApplication::clipboard()->text()));
            QVERIFY(QGuiApplication::clipboard()->text().endsWith(QStringLiteral("— John 3:16–17")));
            // The footer mode button leaves character selection; a click in the text does too.
            key(Qt::Key_V); QTRY_VERIFY(prop("charMode").toBool());
            click("modeControl"); QVERIFY(!prop("charMode").toBool()); QVERIFY(readerFocused());
            key(Qt::Key_V); QTRY_VERIFY(prop("charMode").toBool());
            clickHint("characterHintsPart:exit"); QVERIFY(!prop("charMode").toBool());
            key(Qt::Key_V); QTRY_VERIFY(prop("charMode").toBool());
            const QPoint inside = charPoint(18, 3); QVERIFY2(!inside.isNull(), qPrintable(mode));
            clickAt(inside); QTRY_VERIFY2(!prop("charMode").toBool(), qPrintable(mode + QString(" %1,%2").arg(inside.x()).arg(inside.y())));
            QVERIFY(v16.size() > 0);
        }
    }
    // Visual mode has a header of its keys, made as character selection's
    // is (ModeHeader); its parts act by mouse as the keys do.
    void visualModeHasAKeyHeaderAsCharactersDo() {
        setMode("verse"); navigate("John 3:16"); call("readerFocus");
        QVERIFY(!shown("visualStatus"));
        key(Qt::Key_V, Qt::ShiftModifier); QTRY_VERIFY(prop("verseMode").toBool());
        QVERIFY(shown("visualStatus")); QVERIFY(!shown("characterStatus"));
        QCOMPARE(item("visualTitle")->property("text").toString(), QString("Visual"));
        QCOMPARE(item("visualHints")->property("plainText").toString(),
                 QString("j k extend    o swap    x highlight    w note    y quotation    shift Y plain text    esc exit"));
        // Characters take the same header, theirs in place of Visual's.
        const QByteArray shell = item("visualStatus")->metaObject()->className();
        key(Qt::Key_V); QTRY_VERIFY(prop("charMode").toBool());
        QVERIFY(!shown("visualStatus")); QVERIFY(shown("characterStatus"));
        QCOMPARE(QByteArray(item("characterStatus")->metaObject()->className()), shell);
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("charMode").toBool());
        // Mouse: quotation copies the range and leaves, as y does; exit leaves.
        key(Qt::Key_V, Qt::ShiftModifier); QTRY_VERIFY(prop("verseMode").toBool());
        key(Qt::Key_J); QTRY_COMPARE(bible->selectionReference(), QString("John 3:16–17"));
        QGuiApplication::clipboard()->clear();
        clickHint("visualHintsPart:quotation");
        QTRY_VERIFY(QGuiApplication::clipboard()->text().endsWith(QStringLiteral("— John 3:16–17")));
        QTRY_VERIFY(!prop("verseMode").toBool()); QVERIFY(!shown("visualStatus"));
        key(Qt::Key_V, Qt::ShiftModifier); QTRY_VERIFY(shown("visualStatus"));
        clickHint("visualHintsPart:exit");
        QTRY_VERIFY(!prop("verseMode").toBool()); QVERIFY(!shown("visualStatus"));
    }
    QString tagId(const QString &name) const {
        for (const auto &tag : bible->tags()) if (tag.toMap()["name"].toString() == name) return tag.toMap()["id"].toString();
        return {};
    }
    void manageTagsByKeyAndMouse() {
        const auto scope = bible->tagScope();
        for (const auto &name : {"Alpha", "Beta", "Gamma"}) QVERIFY(!bible->createAndAssignTag(scope, name).isEmpty());
        auto tags = object("tagsUi");
        // ctrl t opens Manage; inside the picker ctrl t switches pages.
        key(Qt::Key_T, Qt::ControlModifier); QTRY_VERIFY(opened("tagsSurface"));
        QCOMPARE(tags->property("page").toString(), QString("manage"));
        key(Qt::Key_T, Qt::ControlModifier); QCOMPARE(tags->property("page").toString(), QString("picker"));
        QVERIFY(object("tagsHints")->property("plainText").toString().contains("ctrl t manage"));
        key(Qt::Key_T, Qt::ControlModifier); QCOMPARE(tags->property("page").toString(), QString("manage"));
        // Enter chooses a tag and focuses its new name.
        QTRY_VERIFY(item("tagSearch")->hasActiveFocus()); type("alp"); key(Qt::Key_Return);
        QTRY_VERIFY(item("tagRenameInput")->hasActiveFocus());
        QCOMPARE(item("tagRenameInput")->property("selectedText").toString(), QString("Alpha"));
        type("aleph"); key(Qt::Key_Return);
        QTRY_VERIFY(item("cancelTagManagement")->hasActiveFocus());
        // enter presses the focused button: the hint says select, not confirm.
        QTRY_VERIFY(object("tagsHints")->property("plainText").toString().contains("enter select"));
        // The confirmation's arrows move between Cancel and Confirm.
        key(Qt::Key_Right); QVERIFY(item("confirmTagManagement")->hasActiveFocus());
        key(Qt::Key_Left); QVERIFY(item("cancelTagManagement")->hasActiveFocus());
        key(Qt::Key_Down); key(Qt::Key_Return);
        QTRY_VERIFY(!tagId("aleph").isEmpty()); QVERIFY(tagId("Alpha").isEmpty());
        // Backspace in the empty filter goes back to the picker.
        QTRY_VERIFY(item("tagSearch")->hasActiveFocus());
        for (int i = 0; i < 3; ++i) key(Qt::Key_Backspace); // "alp" first
        QCOMPARE(tags->property("page").toString(), QString("manage"));
        key(Qt::Key_Backspace); QCOMPARE(tags->property("page").toString(), QString("picker"));
        // Delete asks to delete the current tag; esc cancels.
        key(Qt::Key_T, Qt::ControlModifier); QCOMPARE(tags->property("page").toString(), QString("manage"));
        key(Qt::Key_Down); QTRY_VERIFY(item("tagList")->hasActiveFocus());
        key(Qt::Key_Delete); QTRY_VERIFY(tags->property("confirmation").isValid());
        QCOMPARE(value(tags->property("confirmation")).toMap()["action"].toString(), QString("delete"));
        key(Qt::Key_Escape); QTRY_VERIFY(value(tags->property("confirmation")).isNull() || !value(tags->property("confirmation")).isValid());
        QVERIFY(opened("tagsSurface"));
        // ctrl m picks a merge target with j/k; enter asks to merge.
        key(Qt::Key_Down); key(Qt::Key_Return); QTRY_VERIFY(item("tagRenameInput")->hasActiveFocus());
        const QString source = tags->property("managedId").toString(); QVERIFY(!source.isEmpty());
        key(Qt::Key_M, Qt::ControlModifier); QTRY_VERIFY(item("tagMergeTargets")->hasActiveFocus());
        QCOMPARE(item("tagMergeTargets")->property("currentIndex").toInt(), 0);
        key(Qt::Key_J); QCOMPARE(item("tagMergeTargets")->property("currentIndex").toInt(), 1);
        key(Qt::Key_K); QCOMPARE(item("tagMergeTargets")->property("currentIndex").toInt(), 0);
        key(Qt::Key_Return);
        QTRY_COMPARE(value(tags->property("confirmation")).toMap()["action"].toString(), QString("merge"));
        // Mouse: Confirm merges.
        const int before = bible->tags().size();
        click("confirmTagManagement"); QTRY_COMPARE(bible->tags().size(), before - 1);
        // Mouse: the hint parts switch pages and ask to delete.
        clickHint("tagsHintsPart:back"); QCOMPARE(tags->property("page").toString(), QString("picker"));
        clickHint("tagsHintsPart:manage"); QCOMPARE(tags->property("page").toString(), QString("manage"));
        click(item("tag:" + tagId("Gamma"))); QTRY_VERIFY(item("tagRenameInput")->hasActiveFocus());
        clickHint("tagsHintsPart:merge"); QTRY_VERIFY(item("tagMergeTargets")->hasActiveFocus());
        key(Qt::Key_Escape); QTRY_VERIFY(item("tagList")->hasActiveFocus());
        clickHint("tagsHintsPart:delete…"); QTRY_COMPARE(value(tags->property("confirmation")).toMap()["action"].toString(), QString("delete"));
        click("confirmTagManagement"); QTRY_VERIFY(tagId("Gamma").isEmpty());
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("tagsSurface"));
    }
    void annotationRowsTakeNoFocus() {
        for (const int verse : {5, 7}) { bible->selectVerses(verse, verse); QVERIFY(bible->toggleHighlight()); }
        navigate("John 3:3");
        run("annotations"); QTRY_VERIFY(panelOpened("marks")); QTRY_VERIFY(item("collectionList")->hasActiveFocus());
        auto row = item("savedEntryJohn 3:5"); QVERIFY(row);
        QCOMPARE(row->property("focusPolicy").toInt(), int(Qt::NoFocus));
        // Keyboard: j/k and tab never put a row in focus.
        key(Qt::Key_J); QVERIFY(item("collectionList")->hasActiveFocus());
        QCOMPARE(item("collectionList")->property("currentIndex").toInt(), 1);
        key(Qt::Key_Tab); QVERIFY(!focusName().startsWith("savedEntry"));
        // Mouse: a right click makes the row current without focusing it.
        call("moveSaved", 0); QTRY_VERIFY(item("collectionList")->hasActiveFocus());
        click(row, Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
        QCOMPARE(item("collectionList")->property("currentIndex").toInt(), 0);
        key(Qt::Key_Escape); QTRY_VERIFY(!menu()->property("visible").toBool());
        QTRY_VERIFY(item("collectionList")->hasActiveFocus()); QVERIFY(!row->hasActiveFocus());
        // Mouse: a click opens the row's passage.
        click(item("savedEntryJohn 3:7")); QTRY_COMPARE(bible->selectionReference(), QString("John 3:7"));
    }
    void notesPreviewKeyAndWebLinks() {
        QDesktopServices::setUrlHandler("https", this, "openUrl"); openedUrls.clear();
        run("notes"); QTRY_VERIFY(opened("notesDrawer")); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        auto editor = item("noteEditor");
        editor->setProperty("text", "Read https://example.org/page. Then more.");
        editor->setProperty("cursorPosition", 12);
        // Keyboard: alt enter opens the web address at the cursor.
        QTRY_VERIFY(object("notesHints")->property("plainText").toString().contains("alt enter open link"));
        key(Qt::Key_Return, Qt::AltModifier); QCOMPARE(openedUrls, QList<QUrl>{QUrl("https://example.org/page")});
        // Away from a link it only sounds the boundary.
        editor->setProperty("cursorPosition", 35); key(Qt::Key_Return, Qt::AltModifier);
        QCOMPARE(openedUrls.size(), 1); QCOMPARE(lastSound(), QString("boundary"));
        // alt v switches Write and Preview, from the editor and from the preview.
        QVERIFY(object("notesHints")->property("plainText").toString().contains("alt v preview"));
        key(Qt::Key_V, Qt::AltModifier); QTRY_VERIFY(prop("notePreview").toBool()); QTRY_VERIFY(item("notePreview")->hasActiveFocus());
        QVERIFY(object("notesHints")->property("plainText").toString().contains("alt v write"));
        key(Qt::Key_V, Qt::AltModifier); QTRY_VERIFY(!prop("notePreview").toBool()); QTRY_VERIFY(editor->hasActiveFocus());
        // Not from the reader: alt v there does nothing to Notes.
        call("readerFocus"); key(Qt::Key_V, Qt::AltModifier); QVERIFY(!prop("notePreview").toBool());
        // Mouse: the hint parts switch too, and the buttons' tooltips name the key.
        editor->forceActiveFocus();
        clickHint("notesHintsPart:preview"); QTRY_VERIFY(prop("notePreview").toBool());
        clickHint("notesHintsPart:write"); QTRY_VERIFY(!prop("notePreview").toBool());
        QVERIFY(toolTipOf(item("previewButton"))->property("text").toString().contains("alt v"));
        // Mouse: "to the text" hands the keyboard back to the reader, Notes still
        // open; the panel's Close closes it.
        clickHint("notesHintsPart:to the text"); QTRY_VERIFY(readerFocused()); QVERIFY(opened("notesDrawer"));
        QVERIFY(toolTipOf(item("notesClose"))->property("text").toString().contains("esc"));
        click(item("notesClose")); QTRY_VERIFY(!shown("notesDrawer")); QVERIFY(readerFocused());
        QDesktopServices::unsetUrlHandler("https");
    }
    void addVersionGuideKeys() {
        catalog->entries = {QVariantMap{{"id", "engkjv"}, {"language", "eng"}, {"name", "King James Version"}, {"abbreviation", "KJV"}, {"copyright", "Public domain"}, {"detailsUrl", "https://example.invalid/kjv"}, {"description", "A fixture edition."}},
                            QVariantMap{{"id", "engweb"}, {"language", "eng"}, {"name", "World English Bible"}, {"abbreviation", "WEB"}, {"copyright", "Public domain"}, {"detailsUrl", "https://example.invalid/web"}, {"description", "A fixture edition."}}};
        emit catalog->changed();
        run("import"); QTRY_VERIFY(opened("importGuide")); QTRY_VERIFY(item("catalogSearch")->hasActiveFocus());
        auto guide = object("importGuide");
        QCOMPARE(guide->property("catalogIndex").toInt(), 0); QTRY_VERIFY(item("catalogRowTint:engkjv") && item("catalogRowTint:engkjv")->isVisible());
        // Keyboard: ↑ ↓ choose (stopping at the ends), enter installs.
        key(Qt::Key_Down); QCOMPARE(guide->property("catalogIndex").toInt(), 1); QVERIFY(item("catalogRowTint:engweb")->isVisible());
        key(Qt::Key_Down); QCOMPARE(guide->property("catalogIndex").toInt(), 1); QCOMPARE(lastSound(), QString("boundary"));
        key(Qt::Key_Up); QCOMPARE(guide->property("catalogIndex").toInt(), 0);
        key(Qt::Key_Return); QCOMPARE(catalog->requests, QStringList{"engkjv"});
        catalog->busy = false; emit catalog->changed();
        // shift enter shows Details & Attribution; closing it returns to the search.
        key(Qt::Key_Down); key(Qt::Key_Return, Qt::ShiftModifier); QTRY_VERIFY(opened("catalogDetails"));
        QCOMPARE(value(object("catalogDetails")->property("entry")).toMap()["id"].toString(), QString("engweb"));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("catalogDetails")); QTRY_VERIFY(item("catalogSearch")->hasActiveFocus());
        // Typing filters and starts again at the first match.
        type("web"); QCOMPARE(guide->property("catalogIndex").toInt(), 0);
        QVERIFY(object("importGuideHints")->property("plainText").toString().contains("shift enter details"));
        // ctrl i goes to the ZIP picker.
        key(Qt::Key_I, Qt::ControlModifier); QTRY_VERIFY(!shown("importGuide"));
        QTRY_VERIFY(object("importDialog")->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(object("importDialog"), "close"));
        QTRY_VERIFY(!object("importDialog")->property("visible").toBool());
        // Mouse: a click chooses a row; the hint parts act on it.
        window->requestActivate(); QTRY_COMPARE(QGuiApplication::focusWindow(), window);
        run("import"); QTRY_VERIFY(opened("importGuide"));
        auto row = item("catalogRow:engweb"); shot("add-version-guide"); clickAt(row->mapToScene(QPointF(row->width() - 20, 6)).toPoint());
        QCOMPARE(guide->property("catalogIndex").toInt(), 1); QVERIFY(!shown("catalogDetails"));
        clickHint("importGuideHintsPart:install"); QCOMPARE(catalog->requests, QStringList({"engkjv", "engweb"}));
        catalog->busy = false; emit catalog->changed();
        clickHint("importGuideHintsPart:details"); QTRY_VERIFY(opened("catalogDetails"));
        QTRY_VERIFY(item("detailsScroll")->hasActiveFocus());
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("catalogDetails"));
        clickHint("importGuideHintsPart:close"); QTRY_VERIFY(!shown("importGuide"));
    }
    static QQuickItem *visualPrefix(QQuickItem *root, const QString &prefix) {
        if (root->objectName().startsWith(prefix)) return root;
        for (auto child : root->childItems()) if (auto found = visualPrefix(child, prefix)) return found;
        return nullptr;
    }
    void footnoteMarkersSkipTabAndNameTheKey() {
        for (const QString mode : {"verse", "block"}) {
            setMode(mode); navigate("John 3:3");
            auto marker = visualPrefix(window->contentItem(), mode == "verse" ? "verseFootnoteMark:3." : "blockInlineMarker:footnote:");
            QVERIFY2(marker, qPrintable(mode)); QVERIFY(!marker->activeFocusOnTab());
            auto tip = toolTipOf(marker); QVERIFY(tip);
            QVERIFY2(tip->property("text").toString().endsWith(QStringLiteral("Click or i: peek")), qPrintable(tip->property("text").toString()));
            // Mouse: hovering shows it.
            QTest::mouseMove(window, center(marker)); QTRY_VERIFY(tip->property("visible").toBool());
            QTest::mouseMove(window, QPoint(5, window->height() / 2));
            // Keyboard: tab never stops on a footnote marker.
            call("readerFocus");
            for (int i = 0; i < 14; ++i) {
                key(Qt::Key_Tab);
                QVERIFY2(!focusName().startsWith("verseFootnoteMark") && !focusName().startsWith("blockInlineMarker:footnote"), qPrintable(focusName()));
            }
            call("readerFocus");
            key(Qt::Key_S, Qt::ShiftModifier); QTRY_VERIFY(panelOpened("related"));
            closeMargin();
        }
    }
    void markersHaveLargerHitAreas() {
        for (const QString mode : {"verse", "block"}) {
            setMode(mode); navigate("John 3:3"); bible->clearSelection();
            auto marker = visualPrefix(window->contentItem(), mode == "verse" ? "verseFootnoteMark:3." : "blockInlineMarker:footnote:");
            auto hit = visualPrefix(window->contentItem(), mode == "verse" ? "verseFootnoteHit:3." : "blockInlineHit:footnote:");
            QVERIFY(marker && hit);
            // Taller than the mark (never wider: the next word keeps its clicks), and the text did not move.
            QCOMPARE(hit->width(), marker->width()); QVERIFY(hit->height() > marker->height() + 6);
            // Mouse: a click just above the drawn cell still peeks at the footnote.
            const QPointF edge = marker->mapToScene(QPointF(marker->width() / 2, -3));
            clickAt(edge.toPoint()); QTRY_VERIFY(opened("peekCard"));
            QVERIFY(object("peekBody")->property("text").toString().contains("from above"));
            key(Qt::Key_Escape); QTRY_VERIFY(!shown("peekCard"));
        }
        // Inline annotation markers too (flowing layouts).
        bible->selectVerses(5, 6); bible->setNoteText("Five and six"); navigate("John 3:3");
        setMode("block");
        auto glyph = visualPrefix(window->contentItem(), "blockInlineMarker:note:open");
        auto glyphHit = visualPrefix(window->contentItem(), "blockInlineHit:note:open");
        QVERIFY(glyph && glyphHit); QCOMPARE(glyphHit->width(), glyph->width()); QVERIFY(glyphHit->height() > glyph->height() + 6);
        // Keyboard: out of the tab order, as in the Verse layout (the passage menu opens its ranges).
        QVERIFY(!glyph->property("activeFocusOnTab").toBool());
        // Mouse: a click just below the cell opens it.
        clickAt(glyph->mapToScene(QPointF(glyph->width() / 2, glyph->height() + 3)).toPoint());
        QTRY_COMPARE(bible->selectionReference(), QString("John 3:5–6"));
    }
    void settingsSlidersTakeLeftAndRight() {
        run("settings"); QTRY_VERIFY(opened("settingsDialog"));
        auto font = item("fontSizeSlider"), line = item("lineHeightSlider");
        // Keyboard: → increases, ← decreases; ↑ ↓ step to the other controls.
        // Focused, the slider adds "← → adjust" after the others, which keep their places.
        const QString unfocused = object("fontSizeHints")->property("plainText").toString();
        const auto partX = [&](const char *row, const QString &name) {
            auto part = item(QString("%1Part:%2").arg(row, name)); return part ? part->mapToScene(QPointF()).x() : -1;
        };
        QVERIFY(QQuickTest::qWaitForPolish(window));
        const double decreaseAt = partX("fontSizeHints", "decrease"), resetAt = partX("fontSizeHints", "reset");
        font->forceActiveFocus();
        QTRY_VERIFY(object("fontSizeHints")->property("plainText").toString().contains(QStringLiteral("← → adjust")));
        QVERIFY(object("fontSizeHints")->property("plainText").toString().startsWith(unfocused));
        QVERIFY(object("fontSizeHints")->property("plainText").toString().endsWith(QStringLiteral("← → adjust")));
        QVERIFY(QQuickTest::qWaitForPolish(window));
        QTRY_COMPARE(partX("fontSizeHints", "decrease"), decreaseAt); QTRY_COMPARE(partX("fontSizeHints", "reset"), resetAt);
        key(Qt::Key_Right); QCOMPARE(bible->fontSize(), 19);
        key(Qt::Key_Left); key(Qt::Key_Left); QCOMPARE(bible->fontSize(), 17);
        key(Qt::Key_Right); QCOMPARE(bible->fontSize(), 18);
        key(Qt::Key_Down); QVERIFY(line->hasActiveFocus()); QCOMPARE(bible->fontSize(), 18);
        key(Qt::Key_Up); QVERIFY(font->hasActiveFocus()); QCOMPARE(bible->lineHeight(), 1.5);
        line->forceActiveFocus();
        QTRY_VERIFY(object("lineHeightHints")->property("plainText").toString().contains(QStringLiteral("← → adjust")));
        QVERIFY(!object("fontSizeHints")->property("plainText").toString().contains("adjust"));
        key(Qt::Key_Right); QCOMPARE(bible->lineHeight(), 1.6);
        key(Qt::Key_Left); QCOMPARE(bible->lineHeight(), 1.5);
        // Mouse: dragging along the track still sets the value.
        const QPoint from = font->mapToScene(QPointF(font->width() * 0.3, font->height() / 2)).toPoint();
        const QPoint to = font->mapToScene(QPointF(font->width() * 0.9, font->height() / 2)).toPoint();
        QTest::mousePress(window, Qt::LeftButton, {}, from);
        for (int i = 1; i <= 6; ++i) QTest::mouseMove(window, from + (to - from) * i / 6);
        QTest::mouseRelease(window, Qt::LeftButton, {}, to);
        QTRY_VERIFY(bible->fontSize() > 30);
        key(Qt::Key_0, Qt::ControlModifier); QCOMPARE(bible->fontSize(), 18);
        // Mouse: − left of each slider and + right of it step it, with their
        // keys in the tooltip; at an end, that end's button is disabled. They
        // are not tab stops (the slider's arrows and ctrl - / = do the same).
        struct Step { const char *slider, *decrease, *increase, *less, *more; };
        for (const Step &step : {Step{"fontSizeSlider", "fontSizeDecrease", "fontSizeIncrease", "ctrl -", "ctrl ="},
                 Step{"lineHeightSlider", "lineHeightDecrease", "lineHeightIncrease", "ctrl alt -", "ctrl alt ="}}) {
            auto slider = item(step.slider), less = item(step.decrease), more = item(step.increase);
            QVERIFY2(less && more && less->isVisible() && more->isVisible(), step.slider);
            const auto box = [](QQuickItem *target) { return target->mapRectToScene(QRectF(0, 0, target->width(), target->height())); };
            QVERIFY(box(less).right() <= box(slider).left()); QVERIFY(box(more).left() >= box(slider).right());
            QVERIFY(qAbs(box(less).center().y() - box(slider).center().y()) <= 1);
            QCOMPARE(less->property("tip").toString(), QString("Decrease · ") + step.less);
            QCOMPARE(more->property("tip").toString(), QString("Increase · ") + step.more);
            QCOMPARE(less->property("focusPolicy").toInt(), int(Qt::NoFocus));
            const double before = slider->property("value").toDouble();
            // In view (the dialog scrolls): the slider, in the same row, focused and revealed.
            slider->forceActiveFocus();
            QVERIFY(QMetaObject::invokeMethod(window, "revealFocus", Q_ARG(QVariant, QVariant::fromValue<QObject *>(item("settingsScroll")->property("contentItem").value<QObject *>()))));
            QVERIFY(QQuickTest::qWaitForPolish(window));
            QTRY_VERIFY(QRectF(QPointF(), QSizeF(window->size())).contains(box(more)));
            click(more); QTRY_VERIFY(slider->property("value").toDouble() > before);
            click(less); QTRY_COMPARE(slider->property("value").toDouble(), before);
            QTRY_VERIFY(opened("settingsDialog"));
            slider->setProperty("value", slider->property("to")); QMetaObject::invokeMethod(slider, "moved");
            QTRY_VERIFY(!more->isEnabled()); QVERIFY(less->isEnabled());
            slider->setProperty("value", slider->property("from")); QMetaObject::invokeMethod(slider, "moved");
            QTRY_VERIFY(!less->isEnabled()); QVERIFY(more->isEnabled());
            key(Qt::Key_0, Qt::ControlModifier); key(Qt::Key_0, Qt::ControlModifier | Qt::AltModifier);
            QCOMPARE(bible->fontSize(), 18); QCOMPARE(bible->lineHeight(), 1.5);
        }
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("settingsDialog"));
    }
    void versionsBackspaceStepsBack() {
        run("removeVersion"); QTRY_VERIFY(opened("versionDialog"));
        auto dialog = object("versionDialog");
        // Typing in the search keeps backspace.
        QTRY_VERIFY(focusName() == "versionSearch"); type("o"); key(Qt::Key_Backspace);
        QCOMPARE(dialog->property("step").toInt(), 0); QVERIFY(item("versionSearch")->property("text").toString().isEmpty());
        key(Qt::Key_Return); QTRY_COMPARE(dialog->property("step").toInt(), 1);
        QVERIFY(object("versionHints")->property("plainText").toString().contains("backspace back"));
        key(Qt::Key_Return); QTRY_COMPARE(dialog->property("step").toInt(), 2);
        // Keyboard: backspace steps back a page at a time.
        key(Qt::Key_Backspace); QCOMPARE(dialog->property("step").toInt(), 1);
        key(Qt::Key_Backspace); QCOMPARE(dialog->property("step").toInt(), 0);
        QTRY_VERIFY(focusName() == "versionSearch");
        // Mouse: the hint part steps back too.
        key(Qt::Key_Return); QTRY_COMPARE(dialog->property("step").toInt(), 1);
        clickHint("versionHintsPart:back"); QCOMPARE(dialog->property("step").toInt(), 0);
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("versionDialog"));
    }
    void globalKeysActOverSidePanels() {
        bible->selectVerses(3, 3); QVERIFY(bible->toggleHighlight());
        struct Panel { Qt::Key key; const char *mode; const char *list; };
        const QList<Panel> panels = {{Qt::Key_M, "marks", "collectionList"}, {Qt::Key_S, "related", "studyList"}};
        for (const auto &panel : panels) {
            call("readerFocus"); key(panel.key, Qt::ShiftModifier); QTRY_VERIFY2(panelOpened(panel.mode), panel.mode);
            QTRY_VERIFY2(item(panel.list)->hasActiveFocus(), panel.mode);
            // Keyboard: the palette and a dialog open over the panel and give
            // the keyboard back to it; a reading toggle acts with it open.
            key(Qt::Key_K, Qt::ControlModifier); QTRY_VERIFY2(opened("passagePalette"), panel.mode); QVERIFY2(panelShown(panel.mode), panel.mode);
            key(Qt::Key_Escape); QTRY_VERIFY(!shown("passagePalette")); QTRY_VERIFY2(item(panel.list)->hasActiveFocus(), panel.mode);
            key(Qt::Key_Comma, Qt::ControlModifier); QTRY_VERIFY(opened("settingsDialog")); QVERIFY2(panelShown(panel.mode), panel.mode);
            key(Qt::Key_Escape); QTRY_VERIFY(!shown("settingsDialog")); QTRY_VERIFY2(item(panel.list)->hasActiveFocus(), panel.mode);
            const bool numbers = bible->showVerseNumbers();
            key(Qt::Key_N, Qt::AltModifier); QCOMPARE(bible->showVerseNumbers(), !numbers); QVERIFY2(panelOpened(panel.mode), panel.mode);
            bible->setShowVerseNumbers(numbers);
            closeMargin();
        }
        {
            // ? opens Keybindings over See also; focus mode (f) closes it.
            call("readerFocus"); key(Qt::Key_S, Qt::ShiftModifier); QTRY_VERIFY(panelOpened("related")); QTRY_VERIFY(item("studyList")->hasActiveFocus());
            key(Qt::Key_Question); QTRY_VERIFY(opened("helpDialog")); QVERIFY(panelShown("related"));
            key(Qt::Key_Escape); QTRY_VERIFY(!shown("helpDialog")); QTRY_VERIFY(item("studyList")->hasActiveFocus());
            key(Qt::Key_F); QTRY_VERIFY(bible->focusMode()); QTRY_VERIFY(!panelShown("related"));
            bible->setFocusMode(false);
            // ctrl t opens Manage tags over the panel, which stays.
            call("readerFocus"); key(Qt::Key_S, Qt::ShiftModifier); QTRY_VERIFY(panelOpened("related")); QTest::qWait(30);
            key(Qt::Key_T, Qt::ControlModifier); QTRY_VERIFY(opened("tagsSurface")); QVERIFY(panelShown("related"));
            key(Qt::Key_Escape); QTRY_VERIFY(!shown("tagsSurface"));
            closeMargin();
        }
        // Notes is not modal: from the preview the palette opens over it and
        // returns to it, and a reading toggle acts with it open (from the
        // editor too: notesEditorLaunchers).
        call("readerFocus"); key(Qt::Key_Escape); run("notes"); QTRY_VERIFY(opened("notesDrawer")); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        key(Qt::Key_V, Qt::AltModifier); QTRY_VERIFY(item("notePreview")->hasActiveFocus());
        key(Qt::Key_K, Qt::ControlModifier); QTRY_VERIFY(opened("passagePalette")); QVERIFY(shown("notesDrawer"));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("passagePalette")); QTRY_VERIFY(item("notePreview")->hasActiveFocus());
        const bool numbers = bible->showVerseNumbers();
        key(Qt::Key_N, Qt::AltModifier); QCOMPARE(bible->showVerseNumbers(), !numbers); QVERIFY(opened("notesDrawer"));
        escapeNotes();
        // Nor is the Desk: from the reader, toggles and the palette act over it;
        // from its page, a launcher opens beside it and returns to the page.
        run("desk"); QTRY_VERIFY(item("deskEditor")->hasActiveFocus()); key(Qt::Key_W, Qt::ControlModifier); QTRY_VERIFY(readerFocused());
        key(Qt::Key_N, Qt::AltModifier); QCOMPARE(bible->showVerseNumbers(), numbers); QVERIFY(prop("deskOpen").toBool());
        key(Qt::Key_K, Qt::ControlModifier); QTRY_VERIFY(opened("passagePalette")); QVERIFY(prop("deskOpen").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("passagePalette")); QTRY_VERIFY(readerFocused());
        key(Qt::Key_D, Qt::ShiftModifier); QTRY_VERIFY(item("deskEditor")->hasActiveFocus());
        key(Qt::Key_K, Qt::ControlModifier); QTRY_VERIFY(opened("passagePalette")); QVERIFY(prop("deskOpen").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("passagePalette")); QTRY_VERIFY(item("deskEditor")->hasActiveFocus());
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("notesDrawer"));
        // Mouse: the footer stays live beside an open panel.
        call("readerFocus"); key(Qt::Key_M, Qt::ShiftModifier); QTRY_VERIFY(panelOpened("marks")); QTest::qWait(30);
        click("searchControl"); QTRY_VERIFY(opened("passagePalette")); QVERIFY(panelShown("marks"));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("passagePalette"));
        QVERIFY(QMetaObject::invokeMethod(window, "closeNotes")); QTRY_VERIFY(!shown("notesDrawer"));
    }
    // Keyboard: ↓ through the open menu to `id`, then enter.
    void menuKeyTo(const QString &id) {
        QTRY_VERIFY(menu()->property("opened").toBool());
        for (int i = 0; i < 40; ++i) {
            const auto entries = value(menu()->property("entries")).toList();
            const int at = menu()->property("currentIndex").toInt();
            if (at >= 0 && at < entries.size() && entries[at].toMap()["id"].toString() == id) break;
            key(Qt::Key_Down);
        }
        key(Qt::Key_Return); QTRY_VERIFY(!menu()->property("visible").toBool());
    }
    void passageMenuStepsAndClickableMatchCounter() {
        navigate("John 2:5");
        key(Qt::Key_Menu); QTRY_VERIFY(menu()->property("opened").toBool());
        // Chapter and heading steps live on h/l and {/}, not in the passage menu.
        for (const auto &id : {"previousChapter", "nextChapter", "previousHeading", "nextHeading"}) QVERIFY2(!menuIds().contains(id), id);
        QCOMPARE(menuIds().last(), QString("chapterFind"));
        key(Qt::Key_Escape); QTRY_VERIFY(!menu()->property("visible").toBool());
        clickAt(versePoint(5), Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
        for (const auto &id : {"previousChapter", "nextChapter", "previousHeading", "nextHeading"}) QVERIFY2(!menuIds().contains(id), id);
        key(Qt::Key_Escape); QTRY_VERIFY(!menu()->property("visible").toBool());
        // The match counter: click next, shift click previous, right click for a menu.
        navigate("John 3:1"); key(Qt::Key_Slash); type("access"); key(Qt::Key_Return);
        QTRY_VERIFY(shown("searchMatchCount"));
        auto counter = item("searchMatchCount");
        auto counterMouse = item("searchMatchCountMouse"); QCOMPARE(counterMouse->cursor().shape(), Qt::PointingHandCursor);
        QTest::mouseMove(window, center(counter)); QTRY_VERIFY(toolTipOf(counter)->property("visible").toBool());
        QVERIFY(toolTipOf(counter)->property("text").toString().contains("shift click"));
        QTRY_VERIFY(prop("searchMatchIndex").toInt() >= 0); QTest::qWait(50);
        QTest::mouseMove(window, QPoint(5, window->height() / 2)); QTRY_VERIFY(!toolTipOf(counter)->property("visible").toBool());
        const int first = prop("searchMatchIndex").toInt();
        click(counter);
        QTRY_VERIFY2(prop("searchMatchIndex").toInt() == first + 1, qPrintable(QString("index %1 chapter %2 query %3 matches %4 at %5,%6")
            .arg(prop("searchMatchIndex").toInt()).arg(bible->chapter()).arg(prop("acceptedSearchQuery").toString())
            .arg(value(prop("searchMatches")).toList().size()).arg(center(counter).x()).arg(center(counter).y())));
        click(counter, Qt::LeftButton, Qt::ShiftModifier); QTRY_COMPARE(prop("searchMatchIndex").toInt(), first);
        click(counter, Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
        QCOMPARE(menuIds(), QStringList({"nextMatch", "previousMatch", "|", "clearSearch"}));
        chooseMenu("nextMatch");
        QTRY_VERIFY2(prop("searchMatchIndex").toInt() == first + 1, qPrintable(QString("index %1 chapter %2 query %3 matches %4 cursor %5")
            .arg(prop("searchMatchIndex").toInt()).arg(bible->chapter()).arg(prop("acceptedSearchQuery").toString())
            .arg(value(prop("searchMatches")).toList().size()).arg(bible->cursorVerse())));
        // Keyboard: the same menu by keys.
        click(counter, Qt::RightButton); menuKeyTo("clearSearch");
        QTRY_VERIFY(prop("acceptedSearchQuery").toString().isEmpty()); QVERIFY(!shown("searchMatchCount"));
    }
    void doubleClickNumbersOpenNotesAndTextSelectsWords() {
        for (const QString mode : {"verse", "block", "paragraph"}) {
            setMode(mode); navigate("John 3:16"); call("readerFocus");
            const QString v16 = verseText(16);
            // Mouse: double-clicking a word selects it; Notes stays closed.
            const int loved = v16.indexOf("loved");
            QTest::mouseDClick(window, Qt::LeftButton, {}, charPoint(16, loved + 1)); QTest::qWait(30);
            QTRY_COMPARE(prop("freeSelectedText").toString(), QString("loved"));
            QVERIFY2(!opened("notesDrawer"), qPrintable(mode));
            // ctrl c then copies it as a quotation.
            key(Qt::Key_C, Qt::ControlModifier);
            QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("“… loved …”\n\n— John 3:16"));
            // Mouse: double-clicking the verse number opens Notes on that verse.
            QPoint number;
            if (mode == "verse") number = item("verseNumber17")->mapToScene(QPointF(8, 12)).toPoint();
            else {
                QRectF rect; auto blockBody = item("blockBody");
                QMetaObject::invokeMethod(blockBody, "positionToRectangle", Q_RETURN_ARG(QRectF, rect), Q_ARG(int, window->findChild<BlockDocument *>()->positionFor(17)));
                number = blockBody->mapToScene(QPointF(rect.x() + 3, rect.center().y())).toPoint();
            }
            QTest::mouseDClick(window, Qt::LeftButton, {}, number);
            QTRY_VERIFY2(opened("notesDrawer"), qPrintable(mode)); QCOMPARE(bible->cursorVerse(), 17);
            QVERIFY(prop("freeSelectedText").toString().isEmpty());
            // The keyboard stays in the reader: its esc puts the selection bar
            // away first, then closes Notes.
            QVERIFY(readerFocused()); QVERIFY(shown("selectionBar"));
            key(Qt::Key_Escape); QTRY_VERIFY(!shown("selectionBar")); QVERIFY(shown("notesDrawer"));
            escapeNotes();
            // Keyboard: w writes a note; v and iw select a word.
            call("readerFocus"); key(Qt::Key_W); QTRY_VERIFY(opened("notesDrawer"));
            escapeNotes();
        }
    }
    void seeAlsoTagsByMouseAndKey() {
        bible->selectVerses(7, 7); const QString alpha = bible->createAndAssignTag(bible->tagScope(), "Alpha");
        bible->selectVerses(9, 9); const QString beta = bible->createAndAssignTag(bible->tagScope(), "Beta");
        bible->selectVerses(3, 3); QVERIFY(bible->setTag(bible->tagScope(), alpha, true)); QVERIFY(bible->setTag(bible->tagScope(), beta, true));
        navigate("John 3:3");
        auto tags = object("tagsUi");
        run("related"); QTRY_VERIFY(panelOpened("related"));
        // Mouse: the heading's tags are links (a hand, a tooltip); one keeps only its passages.
        QTRY_VERIFY(item("studySectionTag:Alpha") && item("studySectionTag:Alpha")->isVisible());
        auto link = item("studySectionTag:Alpha");
        QCOMPARE(mouseAreaOf(link)->cursor().shape(), Qt::PointingHandCursor);
        auto tip = toolTipOf(link); QVERIFY(tip);
        QTest::mouseMove(window, center(link) + QPoint(1, 1)); QTest::mouseMove(window, center(link)); QTRY_VERIFY(tip->property("visible").toBool());
        QCOMPARE(tip->property("text").toString(), QString("Only the passages tagged Alpha"));
        click(link); QTRY_COMPARE(tags->property("studyQuery").toString(), QString("#Alpha"));
        QTRY_COMPARE(item("studyList")->property("count").toInt(), 1);
        QVERIFY(item("studyList")->hasActiveFocus());
        // Keyboard: / edits the filter; the hints name the keys.
        QVERIFY(object("studyHints")->property("plainText").toString().contains("/ filter"));
        key(Qt::Key_Slash); QTRY_VERIFY(item("studySearch")->hasActiveFocus());
        key(Qt::Key_A, Qt::ControlModifier); type("#beta"); QTRY_COMPARE(item("studyList")->property("count").toInt(), 1);
        QVERIFY(object("studyHints")->property("plainText").toString().contains("esc leave filter"));
        key(Qt::Key_Escape); QTRY_VERIFY(item("studyList")->hasActiveFocus());
        // t on a row opens the tag picker for its passage.
        QVERIFY(object("studyHints")->property("plainText").toString().contains("t tag"));
        key(Qt::Key_T); QTRY_VERIFY(opened("tagsSurface"));
        QCOMPARE(value(tags->property("editingScope")).toMap()["start"].toInt(), 9);
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("tagsSurface"));
        // A tagged passage's row menu: Edit tags and Move here too.
        click(item("studyRow:0"), Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
        QCOMPARE(menuIds(), QStringList({"open", "copy", "copyPlain", "desk", "|", "tags", "moveHere"}));
        chooseMenu("tags"); QTRY_VERIFY(opened("tagsSurface"));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("tagsSurface"));
    }
    void marginNotesTakeTheMarkerMenu() {
        window->resize(1700, 900); QTRY_COMPARE(window->width(), 1700);
        bible->selectVerses(16, 16); bible->setNoteText("A margin note"); bible->setMarginNotes(true);
        navigate("John 3:14");
        QTRY_VERIFY(item("marginNote:John 3:16") && item("marginNote:John 3:16")->isVisible());
        auto note = item("marginNote:John 3:16");
        // Mouse: right click opens the note marker's menu.
        click(note, Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
        QCOMPARE(menuIds(), QStringList({"open", "|", "moveHere", "remove", "|", "copy", "copyPlain", "|", "desk"}));
        QCOMPARE(value(menu()->property("layout")).toList().at(3).toMap()["text"].toString(), QString("Delete note"));
        chooseMenu("open"); QTRY_VERIFY(opened("notesDrawer")); QCOMPARE(bible->selectionReference(), QString("John 3:16"));
        escapeNotes();
        // Keyboard: the same menu by keys copies the passage.
        click(note, Qt::RightButton); QGuiApplication::clipboard()->clear(); menuKeyTo("copy");
        QTRY_VERIFY(QGuiApplication::clipboard()->text().endsWith(QStringLiteral("— John 3:16")));
        // Left click still opens the note.
        click(note); QTRY_VERIFY(opened("notesDrawer"));
    }
    void finishedMouseSelectionsBecomePrimary() {
        for (const QString mode : {"verse", "block"}) {
            setMode(mode); navigate("John 3:16"); call("readerFocus");
            // Mouse: a drag, when it ends, offers its plain text as PRIMARY.
            drag(charPoint(16, 4), charPoint(16, 16));
            QTRY_VERIFY(!prop("freeSelectedText").toString().isEmpty());
            QCOMPARE(bible->lastPrimarySelection(), prop("freeSelectedText").toString());
            // A double-clicked word too.
            QTest::mouseDClick(window, Qt::LeftButton, {}, charPoint(16, 24));
            QTRY_COMPARE(prop("freeSelectedText").toString(), QString("world"));
            QCOMPARE(bible->lastPrimarySelection(), QString("world"));
            // Keyboard: character selection never touches PRIMARY by itself.
            key(Qt::Key_Escape); key(Qt::Key_V); keys("e"); key(Qt::Key_Escape);
            QCOMPARE(bible->lastPrimarySelection(), QString("world"));
        }
    }
    void listsReserveTheScrollbar() {
        window->resize(1000, 520);
        for (int verse = 1; verse <= 24; ++verse) {
            bible->selectVerses(verse, verse); bible->setNoteText("Long");
            bible->createAndAssignTag(bible->tagScope(), "Long");
        }
        navigate("John 3:3");
        auto check = [&](const QString &list, const QString &row, const QString &bar) {
            QTRY_VERIFY2(item(bar) && item(bar)->isVisible(), qPrintable(bar));
            auto first = item(row); QVERIFY2(first, qPrintable(row));
            const qreal right = first->mapToItem(item(list), QPointF(first->width(), 0)).x();
            QVERIFY2(right <= item(list)->width() - item(bar)->width() + 0.5, qPrintable(list + QString(" %1 / %2").arg(right).arg(item(list)->width())));
        };
        // Keyboard opens each list; rows end where the scrollbar begins.
        key(Qt::Key_M, Qt::ShiftModifier); QTRY_VERIFY(panelOpened("marks"));
        check("collectionList", "savedEntryJohn 3:1", "savedScrollbar");
        // The "here" label stays clear of the scrollbar.
        QTRY_VERIFY(item("savedHereJohn 3:3"));
        auto here = item("savedHereJohn 3:3");
        QTRY_VERIFY(here->mapToItem(item("collectionList"), QPointF(here->width(), 0)).x() <= item("collectionList")->width() - item("savedScrollbar")->width() + 0.5);
        shot("scrollbar-marks");
        // Mouse: wheel scrolling still works and the rows keep their width.
        wheel(item("collectionList"), -480); QTRY_VERIFY(item("collectionList")->property("contentY").toReal() > 0);
        QString row;
        for (int verse = 24; verse >= 1 && row.isEmpty(); --verse)
            if (auto entry = item(QString("savedEntryJohn 3:%1").arg(verse)); entry && entry->isVisible()) row = entry->objectName();
        check("collectionList", row, "savedScrollbar");
        QVERIFY(QMetaObject::invokeMethod(window, "closeNotes")); QTRY_VERIFY(!shown("notesDrawer"));
        QVERIFY(warnings.isEmpty());
    }
    // ── Phase 4: low

    QQuickItem *mouseAreaOf(QQuickItem *owner) const {
        for (auto child : owner->childItems()) if (QByteArray(child->metaObject()->className()).contains("MouseArea")) return child;
        return nullptr;
    }
    void outlineAndMarginNoteTooltipsNameTheirKeys() {
        window->resize(1700, 900); QTRY_COMPARE(window->width(), 1700);
        bible->selectVerses(16, 16); bible->setNoteText("See John 3:5 and https://example.org"); bible->setMarginNotes(true);
        bible->setChapterOutline(true); navigate("John 3:14");
        QTRY_VERIFY(item("chapterOutline")->isVisible());
        // Mouse: hovering the title and an entry names { and } in their tooltips.
        auto title = item("outlineTitle"), entry = item("outlineEntry:20");
        QTest::mouseMove(window, center(title)); QTRY_VERIFY(toolTipOf(title)->property("visible").toBool());
        QVERIFY(toolTipOf(title)->property("text").toString().contains("{"));
        QTest::mouseMove(window, center(entry)); QTRY_VERIFY(toolTipOf(entry)->property("visible").toBool());
        QCOMPARE(toolTipOf(entry)->property("text").toString(), QStringLiteral("Jump to this section · { } step between sections"));
        shot("outline-entry-tooltip");
        // Keyboard: } and { do what the tooltip says.
        call("readerFocus"); key(Qt::Key_BraceRight); QTRY_COMPARE(bible->cursorVerse(), 20);
        key(Qt::Key_BraceLeft); QTRY_VERIFY(bible->cursorVerse() < 20);
        // A margin note names w, and alt enter for its links.
        navigate("John 3:14");
        QTRY_VERIFY(item("marginNote:John 3:16") && item("marginNote:John 3:16")->isVisible());
        auto note = item("marginNote:John 3:16"); auto tip = toolTipOf(note); QVERIFY(tip);
        QVERIFY(tip->property("text").toString().contains(QStringLiteral("Click: open it in Notes · w")));
        QVERIFY(tip->property("text").toString().contains(QStringLiteral("alt enter")));
        QTest::mouseMove(window, note->mapToScene(QPointF(note->width() - 4, note->height() - 3)).toPoint());
        QTest::mouseMove(window, note->mapToScene(QPointF(note->width() - 3, note->height() - 3)).toPoint());
        // Keyboard: w on the passage opens Notes, where alt enter follows the link.
        navigate("John 3:16"); key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        item("noteEditor")->setProperty("cursorPosition", 6);
        key(Qt::Key_Return, Qt::AltModifier); QTRY_COMPARE(bible->selectionReference(), QString("John 3:5"));
    }
    void focusReturnsAfterRestoreConflictAndSetup() {
        // Keyboard: esc cancels the restore file dialog; the reader has the keyboard again.
        run("restore"); QTRY_VERIFY(object("restoreDialog")->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(object("restoreDialog"), "reject"));
        QTRY_VERIFY(!object("restoreDialog")->property("visible").toBool());
        window->requestActivate(); QTRY_COMPARE(QGuiApplication::focusWindow(), window);
        call("restorePanelFocus"); QTRY_VERIFY(readerFocused());
        // From an open panel, the panel gets it back.
        run("annotations"); QTRY_VERIFY(panelOpened("marks"));
        QVERIFY(QMetaObject::invokeMethod(window, "runCommand", Q_ARG(QVariant, "restore")));
        QTRY_VERIFY(object("restoreDialog")->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(object("restoreDialog"), "reject"));
        window->requestActivate(); QTRY_COMPARE(QGuiApplication::focusWindow(), window);
        call("restorePanelFocus");
        QTRY_VERIFY(!readerFocused() && window->activeFocusItem() && (focusWithin("collectionList") || focusWithin("savedSearch") || focusName() == "collectionList"));
        closeMargin();
        // A note conflict puts the keyboard on its choices, unless the editor has it.
        navigate("John 3:16"); key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        item("notesPanel")->forceActiveFocus();
        window->setProperty("noteConflict", true);
        QTRY_VERIFY(item("noteRetry")->hasActiveFocus());
        shot("note-conflict-focus");
        // Pressed by key, the button hides itself: the editor takes the keyboard.
        key(Qt::Key_Return); QTRY_VERIFY(!window->property("noteConflict").toBool());
        QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        // Mouse: Use saved text.
        item("notesPanel")->forceActiveFocus();
        window->setProperty("noteConflict", true); QTRY_VERIFY(item("noteRetry")->hasActiveFocus());
        QTest::qWait(50); click(item("noteUseSaved")); QTRY_VERIFY(!window->property("noteConflict").toBool());
        QVERIFY(QMetaObject::invokeMethod(window, "closeNotes")); QTRY_VERIFY(!shown("notesDrawer"));
        // The setup screen starts on Add version: enter opens the guide; so does a click.
        QVERIFY(bible->removeVersion(bible->versionId(), true)); QTRY_VERIFY(!bible->ready());
        QTRY_VERIFY(item("setupImport")->isVisible()); QTRY_VERIFY(item("setupImport")->hasActiveFocus());
        key(Qt::Key_Return); QTRY_VERIFY(opened("importGuide"));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("importGuide"));
        QTest::qWait(50); click(item("setupImport")); QTRY_VERIFY(opened("importGuide"));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("importGuide"));
    }
    void seeAlsoEscapeClosesItLikeMarks() {
        bible->selectVerses(7, 7); const QString alpha = bible->createAndAssignTag(bible->tagScope(), "Alpha");
        bible->selectVerses(3, 3); QVERIFY(bible->setTag(bible->tagScope(), alpha, true));
        navigate("John 3:3");
        auto tags = object("tagsUi");
        auto openTags = [&] {
            run("related"); QTRY_VERIFY(panelOpened("related")); QTRY_VERIFY(item("studyList")->hasActiveFocus());
        };
        openTags();
        // Keyboard: / filters; esc leaves the field keeping the filter; the next
        // esc closes See also, the reader taking the keyboard.
        key(Qt::Key_Slash); QTRY_VERIFY(item("studySearch")->hasActiveFocus()); type("zz");
        key(Qt::Key_Escape); QTRY_VERIFY(item("studyList")->hasActiveFocus());
        QCOMPARE(tags->property("studyQuery").toString(), QString("zz"));
        QVERIFY(object("studyHints")->property("plainText").toString().endsWith("ctrl w to the text    esc close"));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("notesDrawer")); QTRY_VERIFY(readerFocused());
        // Reopened, the query is gone.
        openTags(); QCOMPARE(tags->property("studyQuery").toString(), QString());
        // ctrl w gives the keyboard to the reader, See also staying (and
        // following it); ctrl w again (or F6) brings it back.
        key(Qt::Key_W, Qt::ControlModifier); QTRY_VERIFY(readerFocused()); QVERIFY(panelShown("related"));
        key(Qt::Key_F6); QTRY_VERIFY(item("studyList")->hasActiveFocus());
        // Mouse: the esc hint closes it too, as does its ×.
        clickHint("studyHintsPart:close"); QTRY_VERIFY(!shown("notesDrawer"));
        openTags(); click(item("studyClose")); QTRY_VERIFY(!shown("notesDrawer"));
    }
    void versionsSearchHomeAndEndMoveTheCaret() {
        run("removeVersion"); QTRY_VERIFY(opened("versionDialog"));
        auto search = item("versionSearch"); QTRY_VERIFY(search->hasActiveFocus());
        type("ac");
        key(Qt::Key_Home); QCOMPARE(search->property("cursorPosition").toInt(), 0);
        key(Qt::Key_End); QCOMPARE(search->property("cursorPosition").toInt(), 2);
        // The list's home/end hint shows only where those keys move the list.
        QVERIFY(!object("versionHints")->property("plainText").toString().contains("jump"));
        item("versionResults")->forceActiveFocus();
        QTRY_VERIFY(object("versionHints")->property("plainText").toString().contains("home end jump"));
        // Mouse: clicking into the field puts the caret where clicked, and home still moves it.
        QTest::mouseClick(window, Qt::LeftButton, {}, search->mapToScene(QPointF(search->width() - 8, search->height() / 2)).toPoint());
        QTRY_VERIFY(search->hasActiveFocus()); QCOMPARE(search->property("cursorPosition").toInt(), 2);
        key(Qt::Key_Home); QCOMPARE(search->property("cursorPosition").toInt(), 0);
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("versionDialog"));
    }
    void helpTitlesSayWhereKeysWork() {
        // Keyboard: search Keybindings for the clarified rows.
        run("help"); QTRY_VERIFY(opened("helpDialog")); type("tags");
        QTRY_VERIFY(item("keybindingEdit the item's tags (Marks, See also)"));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("helpDialog"));
        run("help"); QTRY_VERIFY(opened("helpDialog")); type("filter the list");
        QQuickItem *row = nullptr;
        QTRY_VERIFY((row = item("keybindingFilter the list, #name for a tag (Marks with more than 10 items, See also)")) != nullptr);
        // Mouse: hovering the row names its scope.
        QTest::mouseMove(window, QPoint(1, 1));
        QTRY_VERIFY(!toolTipOf(row)->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(object("keybindingResults"), "positionViewAtIndex", Q_ARG(int, 0), Q_ARG(int, 0)));
        QPoint previous(-1, -1);
        QTRY_VERIFY([&] { QCoreApplication::processEvents(); const auto at = center(row); const bool settled = at == previous; previous = at; return settled; }());
        QTest::mouseMove(window, row->mapToScene(QPointF(row->width() / 2, row->height() - 4)).toPoint()); QTRY_VERIFY(toolTipOf(row)->property("visible").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("helpDialog"));
    }
    void paletteHintsCycleModesWithTab() {
        run("books"); QTRY_VERIFY(opened("passagePalette"));
        QTRY_VERIFY(object("paletteHints")->property("plainText").toString().contains("tab mode"));
        // Mouse: the tab hint steps Passage → Search → Command.
        clickHint("paletteHintsPart:mode"); QTRY_VERIFY(prop("paletteTextMode").toBool());
        clickHint("paletteHintsPart:mode"); QTRY_COMPARE(object("referenceInput")->property("text").toString(), QString("> "));
        // Keyboard: tab continues back to Passage.
        key(Qt::Key_Tab); QTRY_COMPARE(object("referenceInput")->property("text").toString(), QString());
        QVERIFY(!prop("paletteTextMode").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("passagePalette"));
    }
    // Every focus ring in Reading settings stays at least a pixel inside the
    // dialog's clipping view, on both sides, whatever the text size makes of
    // its width (a ring on the clip edge loses its outer pixel on screen).
    void settingsFocusRingsStayInsideTheView_data() {
        QTest::addColumn<int>("fontSize"); QTest::addColumn<double>("scale");
        QTest::newRow("default") << 18 << 1.0;
        QTest::newRow("fractional") << 20 << 1.0;
        QTest::newRow("scaled") << 23 << 1.25;
    }
    void settingsFocusRingsStayInsideTheView() {
        QFETCH(int, fontSize); QFETCH(double, scale);
        bible->setFontSize(fontSize); QVERIFY(desktop->setProperty("textScale", scale));
        run("settings"); QTRY_VERIFY(opened("settingsDialog"));
        QVERIFY(QQuickTest::qWaitForPolish(window));
        auto scroll = item("settingsScroll");
        const QRectF view = scroll->mapRectToScene(QRectF(0, 0, scroll->width(), scroll->height()));
        for (const QString name : {"settingfocus", "settingnumbers", "settingmarginNotes", "readingMode:paragraph", "readingMode:block"}) {
            auto control = item(name); QVERIFY2(control, qPrintable(name));
            const QRectF ring = control->mapRectToScene(QRectF(0, 0, control->width(), control->height()));
            QVERIFY2(ring.left() >= view.left() + 1 && ring.right() <= view.right() - 1,
                qPrintable(QString("%1: %2..%3 in %4..%5").arg(name).arg(ring.left()).arg(ring.right()).arg(view.left()).arg(view.right())));
        }
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("settingsDialog"));
        bible->setFontSize(18); QVERIFY(desktop->setProperty("textScale", 1.0));
    }
    void switchesAndTagButtonsShowFocus() {
        run("settings"); QTRY_VERIFY(opened("settingsDialog"));
        auto first = item("settingfocus"), outline = item("settingfocusFocus"), next = item("settingnumbersFocus");
        QVERIFY(first && outline && next);
        // Keyboard: the focused switch is outlined; tab moves the outline.
        QTRY_VERIFY(first->hasActiveFocus()); QCOMPARE(outline->property("border").value<QObject *>()->property("width").toInt(), 1);
        shot("settings-switch-focus");
        key(Qt::Key_Tab); QTRY_VERIFY(item("settingnumbers")->hasActiveFocus());
        QCOMPARE(outline->property("border").value<QObject *>()->property("width").toInt(), 0);
        QCOMPARE(next->property("border").value<QObject *>()->property("width").toInt(), 1);
        // Mouse: hover tints without an outline.
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("settingsDialog"));
        // TagButtons: focus draws a heavier border than a chosen toggle.
        key(Qt::Key_T); QTRY_VERIFY(opened("tagsSurface"));
        auto manage = item("manageTags"); auto frame = item("manageTags:focus"); QVERIFY(manage && frame);
        QCOMPARE(frame->property("border").value<QObject *>()->property("width").toInt(), 0);
        manage->forceActiveFocus(); QCOMPARE(frame->property("border").value<QObject *>()->property("width").toInt(), 2);
        shot("tag-button-focus");
        // Keyboard: enter on the focused button opens management; a click does too.
        key(Qt::Key_Return); QTRY_COMPARE(object("tagsUi")->property("page").toString(), QString("manage"));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("tagsSurface"));
    }
    void pointingHandsAndHoveredSuggestions() {
        bible->selectVerses(5, 5); bible->setNoteText("Five");
        navigate("John 3:3");
        // Mouse: rails, menu rows, and chips show a pointing hand.
        QTRY_VERIFY(item("verseNoteMarker5Hit"));
        QCOMPARE(item("verseNoteMarker5Hit")->cursor().shape(), Qt::PointingHandCursor);
        clickAt(versePoint(3), Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
        auto row = item("contextMenuItem:copy"); QVERIFY(row);
        QCOMPARE(mouseAreaOf(row)->cursor().shape(), Qt::PointingHandCursor);
        key(Qt::Key_Escape); QTRY_VERIFY(!menu()->property("visible").toBool());
        const QString alpha = bible->createAndAssignTag(bible->tagScope(), "Alpha");
        bible->selectVerses(9, 9); QVERIFY(bible->setTag(bible->tagScope(), alpha, true)); bible->selectVerses(3, 3);
        run("related"); QTRY_VERIFY(panelOpened("related")); QTRY_VERIFY(item("studySectionTag:Alpha"));
        QCOMPARE(mouseAreaOf(item("studySectionTag:Alpha"))->cursor().shape(), Qt::PointingHandCursor);
        QVERIFY(QMetaObject::invokeMethod(window, "closeNotes")); QTRY_VERIFY(!shown("notesDrawer"));
        // [[ suggestions: hovering one chooses it (tint); a click inserts it.
        navigate("John 3:16"); key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        type("[[jn 3:1"); QTRY_VERIFY(prop("linkSuggesting").toBool());
        // This small fixture ranks one passage per query: offer a second one.
        window->setProperty("linkSuggestions", bible->referenceCandidates("jn 3:1", 6) + bible->referenceCandidates("gen 1:2", 6));
        QTRY_VERIFY(item("noteLinkSuggestionMouse1"));
        auto second = item("noteLinkSuggestion1"); auto hover = item("noteLinkSuggestionMouse1");
        QCOMPARE(hover->cursor().shape(), Qt::PointingHandCursor);
        QCOMPARE(prop("linkSuggestionIndex").toInt(), 0);
        QTest::mouseMove(window, center(second) + QPoint(1, 0)); QTest::mouseMove(window, center(second));
        QTRY_COMPARE(prop("linkSuggestionIndex").toInt(), 1);
        QVERIFY(second->property("color").value<QColor>().alpha() > 0);
        shot("link-suggestion-hover");
        // Keyboard: ↑ still moves the choice; enter inserts it.
        key(Qt::Key_Up); QCOMPARE(prop("linkSuggestionIndex").toInt(), 0);
        const QString chosen = value(prop("linkSuggestions")).toList().at(1).toMap()["reference"].toString();
        click(second); QTRY_VERIFY(!prop("linkSuggesting").toBool());
        QVERIFY(item("noteEditor")->property("text").toString().startsWith(chosen));
    }
    void blockVerseNumbersExplainSelecting() {
        setMode("block"); navigate("John 3:16");
        QRectF rect; auto blockBody = item("blockBody");
        QMetaObject::invokeMethod(blockBody, "positionToRectangle", Q_RETURN_ARG(QRectF, rect), Q_ARG(int, window->findChild<BlockDocument *>()->positionFor(17)));
        const QPoint number = blockBody->mapToScene(QPointF(rect.x() + 3, rect.center().y())).toPoint();
        auto tip = object("blockNumberTip"); QVERIFY(tip);
        // Mouse: hovering a verse number shows the tooltip and a pointing hand.
        QTest::mouseMove(window, number + QPoint(1, 0)); QTest::mouseMove(window, number);
        QTRY_VERIFY(tip->property("visible").toBool());
        QCOMPARE(tip->property("text").toString(), QStringLiteral("Select verse 17 · shift click extends · middle click copies"));
        shot("block-number-tooltip");
        // The click selects; shift click extends.
        clickAt(number); QTRY_COMPARE(bible->selectionReference(), QString("John 3:17"));
        // Text away from numbers shows none.
        QTest::mouseMove(window, charPoint(16, 10)); QTRY_VERIFY(!tip->property("visible").toBool());
        // Keyboard: the same selection by j and shift J.
        call("readerFocus"); key(Qt::Key_J, Qt::ShiftModifier); QTRY_COMPARE(bible->selectionReference(), QString("John 3:17–18"));
    }
    void ctrlClickOpensNoteLinks() {
        navigate("John 3:16"); key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        auto editor = item("noteEditor"); editor->setProperty("text", QStringLiteral("Compare John 3:5 here"));
        // Wait for the drawer to settle before taking the link's position.
        QTRY_COMPARE(object("notesDrawer")->property("position").toReal(), 1.0); QTest::qWait(50);
        QRectF rect; QMetaObject::invokeMethod(editor, "positionToRectangle", Q_RETURN_ARG(QRectF, rect), Q_ARG(int, 10));
        const QPoint link = editor->mapToScene(rect.center()).toPoint();
        // Mouse: a plain click only places the caret.
        clickAt(link); QCOMPARE(bible->selectionReference(), QString("John 3:16"));
        const int caret = editor->property("cursorPosition").toInt(); QVERIFY2(caret >= 8 && caret <= 16, qPrintable(QString::number(caret))); // on the link
        // ctrl click opens the passage; Notes stays open. (Apart from the first click: not a double click.)
        QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 50);
        QTest::mouseClick(window, Qt::LeftButton, Qt::ControlModifier, link); QCoreApplication::processEvents();
        QTRY_COMPARE(bible->selectionReference(), QString("John 3:5")); QVERIFY(opened("notesDrawer"));
        // Keyboard: alt enter at the cursor does the same (Notes is now on 3:5's empty note).
        QVERIFY(QMetaObject::invokeMethod(window, "closeNotes")); QTRY_VERIFY(!shown("notesDrawer"));
        navigate("John 3:16"); key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        item("noteEditor")->setProperty("cursorPosition", 10);
        key(Qt::Key_Return, Qt::AltModifier); QTRY_COMPARE(bible->selectionReference(), QString("John 3:5"));
    }
    void chapterTitleOpensItsChapterList() {
        navigate("John 3:1"); call("setReaderY", 0);
        auto title = item("chapterTitleMouse"); QVERIFY(title);
        QTRY_VERIFY(center(title).y() > 20 && center(title).y() < window->height() / 2);
        QCOMPARE(title->cursor().shape(), Qt::PointingHandCursor);
        // Mouse: hovering explains it; a click lists John's chapters with 3 chosen.
        QTest::mouseMove(window, center(title) + QPoint(2, 0)); QTest::mouseMove(window, center(title));
        QTRY_VERIFY(toolTipOf(item("chapterTitle"))->property("visible").toBool());
        click(title); QTRY_VERIFY(opened("passagePalette"));
        QCOMPARE(prop("paletteBook").toInt(), bible->book());
        QCOMPARE(item("paletteChapters")->property("currentIndex").toInt(), 2);
        shot("chapter-title-palette");
        // Keyboard: ← and enter open the previous chapter; backspace goes back to all books.
        key(Qt::Key_Left); key(Qt::Key_Return);
        QTRY_VERIFY(!shown("passagePalette"));
        QCOMPARE(bible->chapter(), 2);
        call("setReaderY", 0); QTRY_VERIFY(center(title).y() > 20 && center(title).y() < window->height() / 2);
        click(item("chapterTitleMouse")); QTRY_VERIFY(opened("passagePalette"));
        key(Qt::Key_Backspace); QTRY_COMPARE(prop("paletteBook").toInt(), 0);
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("passagePalette"));
    }
    // The chapter title scales with the Text size, keeping its proportion to the verse text.
    void chapterTitleScalesWithTextSize() {
        navigate("John 3:1"); call("setReaderY", 0);
        auto title = item("chapterTitle"); QVERIFY(title);
        const int original = bible->fontSize();
        auto size = [&] { return title->property("font").value<QFont>().pixelSize(); };
        auto verseSize = [&] { return item("verseBody1")->property("font").value<QFont>().pixelSize(); };
        bible->setFontSize(18); QTRY_COMPARE(bible->fontSize(), 18);
        const int base = size(); const double ratio = double(base) / verseSize(); const double pad = title->property("bottomPadding").toDouble();
        bible->setFontSize(27); QTRY_VERIFY(size() > base);
        QVERIFY(qAbs(size() - 1.5 * base) <= 1);
        QVERIFY(qAbs(double(size()) / verseSize() - ratio) < 0.06);
        QVERIFY(qAbs(title->property("bottomPadding").toDouble() - 1.5 * pad) <= 1);
        shot("chapter-title-large");
        // Keyboard: ctrl minus shrinks it again.
        key(Qt::Key_Minus, Qt::ControlModifier); QTRY_VERIFY(size() < qRound(1.5 * base));
        bible->setFontSize(original);
    }

    // ── Phase 5: new

    int matchCount() const { return value(prop("searchMatches")).toList().size(); }
    void findInChapterTakesRegularExpressions() {
        navigate("John 3:1");
        auto input = item("chapterFindInput");
        auto find = [&](const QString &pattern) {
            if (!prop("findPromptOpen").toBool()) { key(Qt::Key_Slash); QTRY_VERIFY(input->hasActiveFocus()); }
            input->setProperty("text", QString()); type(pattern); QTest::qWait(220);
        };
        // Keyboard: patterns are regular expressions, smart case.
        find("access text 3:1\\d "); QTRY_COMPARE(matchCount(), 9); // 3:10–19 but 3:16, whose text differs
        find("original access"); const int all = matchCount(); QVERIFY(all >= 30);
        find("ORIGINAL access"); QCOMPARE(matchCount(), 0);
        find("ORIGINAL access\\c"); QCOMPARE(matchCount(), all);
        find("\\Coriginal access"); QCOMPARE(matchCount(), 0);
        find("(beginning|again)"); QTRY_COMPARE(matchCount(), 1); // within John 3: born again
        // An invalid pattern says so, matches nothing, and enter is refused.
        find("access("); QTRY_COMPARE(object("chapterFindCount")->property("text").toString(), QString("invalid pattern"));
        QCOMPARE(matchCount(), 0);
        const int sounds = playCount(); key(Qt::Key_Return);
        QVERIFY(prop("findPromptOpen").toBool()); QCOMPARE(lastSound(), QString("error")); QVERIFY(playCount() > sounds);
        shot("find-invalid-pattern");
        // Accepted: n and N step the regex matches.
        find("text 3:2\\d "); QTRY_COMPARE(matchCount(), 10); key(Qt::Key_Return);
        QTRY_VERIFY(!prop("findPromptOpen").toBool()); const int at = bible->cursorVerse(); QVERIFY(at >= 20);
        key(Qt::Key_N); QTRY_COMPARE(bible->cursorVerse(), at + 1);
        key(Qt::Key_N, Qt::ShiftModifier); QTRY_COMPARE(bible->cursorVerse(), at);
        // Mouse: clicking the reader accepts a valid pattern; with an invalid one it just closes.
        find("access("); QTRY_COMPARE(object("chapterFindCount")->property("text").toString(), QString("invalid pattern"));
        clickAt(outsidePoint("chapterFind", -1)); QTRY_VERIFY(!prop("findPromptOpen").toBool());
        QCOMPARE(prop("acceptedSearchQuery").toString(), QString("text 3:2\\d "));
        find("born ag(ai)n"); QTRY_COMPARE(matchCount(), 1); clickAt(outsidePoint("chapterFind", -1));
        QTRY_VERIFY(!prop("findPromptOpen").toBool()); QCOMPARE(prop("acceptedSearchQuery").toString(), QString("born ag(ai)n"));
    }
    void readingSettingsLayoutAndKeys() {
        run("settings"); QTRY_VERIFY(opened("settingsDialog"));
        QTest::qWait(60);
        // Focus starts at the top.
        QTRY_VERIFY(item("settingfocus")->hasActiveFocus()); QCOMPARE(contentY("settingsScroll"), 0.0);
        // Layout, with the footer's icons beside each choice.
        QCOMPARE(object("readingModeTitle")->property("text").toString(), QString("Layout"));
        for (const QString mode : {"paragraph", "verse", "block"}) {
            auto icon = item("readingModeIcon:" + mode), button = item("readingMode:" + mode);
            QVERIFY(icon && icon->isVisible());
            auto label = qvariant_cast<QQuickItem *>(button->property("contentItem"));
            const qreal textLeft = label->x() + (label->width() - label->implicitWidth()) / 2;
            QVERIFY2(icon->x() + icon->width() <= textLeft && icon->x() >= 0, qPrintable(mode));
        }
        // A rule under the switches, above the layout.
        auto rule = item("settingsSwitchesRule"); QVERIFY(rule && rule->isVisible());
        const qreal ruleY = rule->mapToScene(QPointF()).y();
        QVERIFY(item("settingmarginNotes")->mapToScene(QPointF(0, item("settingmarginNotes")->height())).y() <= ruleY);
        QVERIFY(item("readingModeTitle")->mapToScene(QPointF()).y() >= ruleY);
        // The same rules between Layout and Text size, and Text size and Line height.
        QCOMPARE(object("textSizeTitle")->property("text").toString(), QString("Text size"));
        auto below = [&](const char *name) { auto target = item(name); return target->mapToScene(QPointF(0, target->height())).y(); };
        auto top = [&](const char *name) { return item(name)->mapToScene(QPointF()).y(); };
        for (const auto &[ruleName, above, under] : {std::tuple{"settingsTextSizeRule", "readingMode:block", "textSizeTitle"},
                 std::tuple{"settingsLineHeightRule", "fontSizeHints", "lineHeightTitle"}}) {
            auto fine = item(ruleName); QVERIFY2(fine && fine->isVisible(), ruleName);
            QCOMPARE(fine->height(), rule->height()); QCOMPARE(fine->opacity(), rule->opacity());
            QCOMPARE(fine->property("color"), rule->property("color"));
            QCOMPARE(fine->width(), rule->width());
            QVERIFY2(below(above) <= top(ruleName) && top(ruleName) + fine->height() <= top(under), ruleName);
        }
        // ctrl = is the listed increase key; ctrl + still works.
        const QString hints = object("fontSizeHints")->property("plainText").toString();
        QVERIFY2(hints.contains("ctrl = increase") && !hints.contains("ctrl +"), qPrintable(hints));
        const int size = bible->fontSize();
        key(Qt::Key_Equal, Qt::ControlModifier); QCOMPARE(bible->fontSize(), size + 1);
        key(Qt::Key_Plus, Qt::ControlModifier); QCOMPARE(bible->fontSize(), size + 2);
        key(Qt::Key_0, Qt::ControlModifier);
        shot("reading-settings");
        // Keyboard: tab reaches every control in order and wraps to the top.
        QStringList order;
        for (int i = 0; i < 30; ++i) {
            key(Qt::Key_Tab); const QString name = focusName();
            if (name == "settingfocus") break;
            order << name;
        }
        for (const QString name : {"settingnumbers", "settingmarginNotes", "readingMode:paragraph", "readingMode:verse", "readingMode:block",
                 "fontSizeSlider", "lineHeightSlider"})
            QVERIFY2(order.contains(name), qPrintable(order.join(", ")));
        QVERIFY(order.indexOf("settingmarginNotes") < order.indexOf("readingMode:paragraph"));
        QVERIFY(order.indexOf("readingMode:block") < order.indexOf("fontSizeSlider"));
        // Keyboard: ↓ and j step through the controls in tab's order, ↑ and k
        // back, stopping at the ends; a disabled switch is passed over.
        item("settingfocus")->forceActiveFocus();
        QStringList stepped{focusName()};
        for (int i = 0; i < 30; ++i) {
            key(i % 2 ? Qt::Key_J : Qt::Key_Down);
            if (focusName() == stepped.last()) break;
            stepped << focusName();
        }
        QCOMPARE(stepped.first(), QString("settingfocus")); QCOMPARE(stepped.last(), QString("lineHeightSlider"));
        QCOMPARE(lastSound(), QString("boundary"));
        QCOMPARE(stepped, QStringList(QStringList{"settingfocus"} << order.mid(0, order.indexOf("lineHeightSlider") + 1)));
        for (int i = stepped.size() - 2; i >= 0; --i) { key(i % 2 ? Qt::Key_K : Qt::Key_Up); QCOMPARE(focusName(), stepped[i]); }
        key(Qt::Key_K); QCOMPARE(focusName(), QString("settingfocus")); QCOMPARE(lastSound(), QString("boundary"));
        // Keyboard: a focused layout choice takes enter; a switch takes space.
        item("readingMode:block")->forceActiveFocus(); key(Qt::Key_Return); QCOMPARE(bible->readingMode(), QString("block"));
        item("settingnumbers")->forceActiveFocus(); const bool numbers = bible->showVerseNumbers();
        key(Qt::Key_Space); QCOMPARE(bible->showVerseNumbers(), !numbers);
        // Mouse: a click on each.
        click(item("readingMode:verse")); QCOMPARE(bible->readingMode(), QString("verse"));
        click(item("settingnumbers")); QCOMPARE(bible->showVerseNumbers(), numbers);
        QVERIFY(opened("settingsDialog"));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("settingsDialog"));
    }

    // ── Re-audit fixes (keyboard and mouse)

    qreal readerY() const { return object("reading")->property("contentY").toReal(); }
    void reauditCharacterSelectionKeepsItsMenu() {
        for (const QString mode : {"verse", "block"}) {
            setMode(mode); navigate("John 3:16"); call("readerFocus");
            // Keyboard: the menu key opens the passage menu at the cursor; the selection stays.
            key(Qt::Key_V); QTRY_VERIFY(prop("charMode").toBool()); keys("e");
            QCOMPARE(prop("freeSelectedText").toString(), QString("For"));
            key(Qt::Key_Menu); QTRY_VERIFY(menu()->property("opened").toBool());
            QVERIFY2(prop("charMode").toBool(), qPrintable(mode)); QCOMPARE(menuIds().first(), QString("copySelected"));
            QGuiApplication::clipboard()->clear(); key(Qt::Key_Return);
            QTRY_VERIFY(QGuiApplication::clipboard()->text().contains("For")); QTRY_VERIFY(!prop("charMode").toBool());
            // shift F10 too; esc closes the menu and keeps selecting.
            key(Qt::Key_V); QTRY_VERIFY(prop("charMode").toBool()); keys("e");
            key(Qt::Key_F10, Qt::ShiftModifier); QTRY_VERIFY(menu()->property("opened").toBool());
            key(Qt::Key_Escape); QTRY_VERIFY(!menu()->property("visible").toBool());
            QVERIFY(prop("charMode").toBool()); QVERIFY(readerFocused());
            // Mouse: right click on the selection keeps it; Copy selected plain text copies and ends it.
            clickAt(charPoint(16, 1), Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
            QVERIFY(prop("charMode").toBool()); QCOMPARE(menuIds().first(), QString("copySelected"));
            QGuiApplication::clipboard()->clear(); chooseMenu("copySelectedPlain");
            QTRY_COMPARE(QGuiApplication::clipboard()->text(), QString("For")); QVERIFY(!prop("charMode").toBool());
            // Mouse: right click away from the selection drops it and selects that verse.
            key(Qt::Key_V); QTRY_VERIFY(prop("charMode").toBool());
            clickAt(charPoint(20, 3), Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
            QVERIFY(!prop("charMode").toBool()); QVERIFY(prop("freeSelectedText").toString().isEmpty());
            QCOMPARE(bible->selectionReference(), QString("John 3:20")); QCOMPARE(menuIds().first(), QString("copy"));
            key(Qt::Key_Escape); QTRY_VERIFY(!menu()->property("visible").toBool());
        }
    }
    void reauditCharacterSelectionKeys() {
        setMode("verse"); navigate("John 3:16"); call("readerFocus");
        key(Qt::Key_V); QTRY_VERIFY(prop("charMode").toBool());
        keys("w"); QVERIFY(charAt(charCursor(), 16, 4));
        // home is the 0 motion, never a count digit: after a count it moves to the line start.
        keys("3"); key(Qt::Key_Home); QVERIFY(charAt(charCursor(), 16, 0)); QCOMPARE(prop("verseCount").toString(), QString());
        keys("l"); QVERIFY(charAt(charCursor(), 16, 1));
        // Scroll keys scroll the reader; the selection stays.
        const qreal top = readerY();
        key(Qt::Key_D, Qt::ControlModifier); QTRY_VERIFY(readerY() > top + 20);
        QVERIFY(prop("charMode").toBool()); QVERIFY(charAt(charCursor(), 16, 1));
        key(Qt::Key_PageUp); QTRY_VERIFY(readerY() < top + 20);
        key(Qt::Key_F, Qt::ControlModifier); QTRY_VERIFY(readerY() > top + 20);
        key(Qt::Key_B, Qt::ControlModifier); QTRY_VERIFY(readerY() < top + 20);
        // Unbound keys sound the error instead of nothing.
        for (const auto &press : QList<QPair<Qt::Key, Qt::KeyboardModifiers>>{{Qt::Key_E, Qt::ControlModifier}, {Qt::Key_X, Qt::AltModifier}, {Qt::Key_Tab, Qt::NoModifier}}) {
            const int plays = playCount(); key(press.first, press.second);
            QCOMPARE(playCount(), plays + 1); QCOMPARE(lastSound(), QString("error")); QVERIFY(prop("charMode").toBool());
        }
        key(Qt::Key_Escape); QVERIFY(!prop("charMode").toBool());
    }
    void reauditRightClickDropsDistantMouseSelections() {
        for (const QString mode : {"verse", "block"}) {
            setMode(mode); navigate("John 3:3"); bible->clearSelection(); call("readerFocus");
            drag(charPoint(4, 1), charPoint(4, 12)); QTRY_VERIFY2(!prop("freeSelectedText").toString().isEmpty(), qPrintable(mode));
            // Mouse: right click far from the selected text: it goes, that verse is selected.
            clickAt(charPoint(9, 5), Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
            QVERIFY(prop("freeSelectedText").toString().isEmpty()); QCOMPARE(bible->selectionReference(), QString("John 3:9"));
            QCOMPARE(menuIds().first(), QString("copy"));
            key(Qt::Key_Escape); QTRY_VERIFY(!menu()->property("visible").toBool());
            // On the selection it stays (covered before; here the same line, just past its end).
            drag(charPoint(4, 1), charPoint(4, 12)); QTRY_VERIFY(!prop("freeSelectedText").toString().isEmpty());
            clickAt(charPoint(4, 6), Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
            QCOMPARE(menuIds().first(), QString("copySelected"));
            key(Qt::Key_Escape); QTRY_VERIFY(!menu()->property("visible").toBool());
            key(Qt::Key_Escape);
        }
    }
    void reauditNoteKeybindingsAct() {
        navigate("John 3:16"); key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        // Keyboard: Keybindings → "Suggest a passage" types [[ in the open note.
        run("help"); QTRY_VERIFY(item("keybindingSearch")->hasActiveFocus());
        type("Suggest a passage"); QTRY_VERIFY(item("keybindingSuggest a passage to link (in a note)"));
        QVERIFY(item("keybindingSuggest a passage to link (in a note)")->property("available").toBool());
        key(Qt::Key_Return); QTRY_VERIFY(!shown("helpDialog"));
        QTRY_VERIFY(item("noteEditor")->hasActiveFocus()); QCOMPARE(item("noteEditor")->property("text").toString(), QString("[["));
        // Mouse: the Write/Preview row switches to Preview.
        run("help"); QTRY_VERIFY(item("keybindingSearch")->hasActiveFocus());
        type("Switch a note"); QQuickItem *row = nullptr; QTRY_VERIFY((row = item("keybindingSwitch a note or the Desk between Write and Preview")));
        QTest::qWait(50); click(row); QTRY_VERIFY(!shown("helpDialog")); QTRY_VERIFY(prop("notePreview").toBool());
        // Without Notes the rows are unavailable: activating sounds the error.
        QVERIFY(QMetaObject::invokeMethod(window, "closeNotes")); QTRY_VERIFY(!shown("notesDrawer"));
        for (const char *action : {"noteLinkSuggest", "openNoteLink", "notePreview", "noteLinkedFrom", "noteKeepDraft", "noteUseSaved"}) {
            const int plays = playCount(); run(action); QCOMPARE(playCount(), plays + 1); QCOMPARE(lastSound(), QString("error"));
        }
    }
    void reauditNotesTabChainAndConflictKeys() {
        navigate("John 3:16"); key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        // Tab and shift tab stay in the editor (tab types a tab, shift tab removes it).
        key(Qt::Key_Tab, Qt::ShiftModifier); QVERIFY(item("noteEditor")->hasActiveFocus());
        key(Qt::Key_Tab); QVERIFY(item("noteEditor")->hasActiveFocus());
        QCOMPARE(item("noteEditor")->property("text").toString(), QString("\t"));
        key(Qt::Key_Tab, Qt::ShiftModifier); QCOMPARE(item("noteEditor")->property("text").toString(), QString());
        // Preview by mouse, back to Write by the hint.
        click(item("previewButton")); QTRY_VERIFY(prop("notePreview").toBool());
        clickHint("notesHintsPart:write"); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        item("noteEditor")->forceActiveFocus(); type("draft");
        // A conflict while typing leaves the keyboard in the editor; ctrl s keeps the draft.
        window->setProperty("noteConflict", true); QTest::qWait(60);
        QVERIFY(item("noteEditor")->hasActiveFocus());
        QTRY_VERIFY(object("notesHints")->property("plainText").toString().contains("ctrl s save my draft"));
        QVERIFY(toolTipOf(item("noteRetry"))->property("text").toString().contains("ctrl s"));
        key(Qt::Key_S, Qt::ControlModifier); QTRY_VERIFY(!window->property("noteConflict").toBool());
        QCOMPARE(bible->noteText(), QString("draft")); QVERIFY(item("noteEditor")->hasActiveFocus());
        // ctrl shift S takes the saved text.
        window->setProperty("noteConflict", true); QTest::qWait(60); QVERIFY(item("noteEditor")->hasActiveFocus());
        key(Qt::Key_S, Qt::ControlModifier | Qt::ShiftModifier); QTRY_VERIFY(!window->property("noteConflict").toBool());
        // Mouse: the hint resolves it too.
        window->setProperty("noteConflict", true); QTest::qWait(60);
        clickHint("notesHintsPart:use saved text"); QTRY_VERIFY(!window->property("noteConflict").toBool());
        // Without a conflict ctrl s is an error.
        const int plays = playCount(); key(Qt::Key_S, Qt::ControlModifier); QCOMPARE(playCount(), plays + 1); QCOMPARE(lastSound(), QString("error"));
        // alt v with no selection sounds the error.
        bible->clearSelection(); item("notesPanel")->forceActiveFocus();
        key(Qt::Key_V, Qt::AltModifier); QCOMPARE(lastSound(), QString("error"));
    }
    void reauditLinkedFromListKeysAndMenu() {
        for (int verse = 1; verse <= 14; ++verse) { QVERIFY(bible->navigate(QString("John 1:%1").arg(verse))); bible->selectVerses(verse, verse); bible->setNoteText("See John 3:16."); }
        window->resize(1200, 560); QTRY_COMPARE(window->height(), 560);
        navigate("John 3:16"); bible->selectVerses(16, 16); call("readerFocus");
        key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        key(Qt::Key_Down, Qt::ControlModifier); QTRY_VERIFY(item("linkedFrom")->hasActiveFocus());
        // G: the last row, scrolled into view; g: the first.
        key(Qt::Key_G, Qt::ShiftModifier); QCOMPARE(item("linkedFrom")->property("currentIndex").toInt(), 13);
        auto last = item("linkedFromRow13"); auto view = item("notesScroll");
        QTRY_VERIFY(last->mapRectToItem(view, last->boundingRect()).bottom() <= view->height() + 1);
        // A lone g waits for its second: gg is the first.
        key(Qt::Key_G); QCOMPARE(item("linkedFrom")->property("currentIndex").toInt(), 13);
        key(Qt::Key_G); QCOMPARE(item("linkedFrom")->property("currentIndex").toInt(), 0);
        // A key after a lone g is spent (as in Vim): j moves on only next time.
        key(Qt::Key_J); key(Qt::Key_G); key(Qt::Key_J); QCOMPARE(item("linkedFrom")->property("currentIndex").toInt(), 1);
        key(Qt::Key_J); QCOMPARE(item("linkedFrom")->property("currentIndex").toInt(), 2);
        key(Qt::Key_G); key(Qt::Key_G); QCOMPARE(item("linkedFrom")->property("currentIndex").toInt(), 0);
        auto first = item("linkedFromRow0"); QTRY_VERIFY(first->mapRectToItem(view, first->boundingRect()).top() >= -1);
        // Mouse: right-click a row from the editor: the list takes the keyboard its menu names.
        item("noteEditor")->forceActiveFocus(); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        click(first, Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
        QCOMPARE(menuShortcut("copy"), QString("y"));
        key(Qt::Key_Escape); QTRY_VERIFY(item("linkedFrom")->hasActiveFocus());
    }
    void reauditHiddenButtonsHandOnTheKeyboard() {
        // The error banner's Dismiss, pressed by key, hands the keyboard to the reader.
        QVERIFY(!bible->navigate("Nowhere 99")); QTRY_VERIFY(item("errorBanner")->isVisible());
        auto dismiss = item("errorDismiss"); dismiss->forceActiveFocus(); QVERIFY(dismiss->hasActiveFocus());
        key(Qt::Key_Return); QVERIFY(bible->error().isEmpty()); QTRY_VERIFY(readerFocused());
        // The add-version guide's download Cancel: the search field.
        run("import"); QTRY_VERIFY(opened("importGuide"));
        catalog->busy = true; emit catalog->changed(); QTRY_VERIFY(item("catalogCancel")->isVisible());
        item("catalogCancel")->forceActiveFocus(); key(Qt::Key_Space);
        QTRY_VERIFY(!item("catalogCancel")->isVisible()); QTRY_VERIFY(item("catalogSearch")->hasActiveFocus());
        // The guide's keys work from anywhere in it: from Close, ↓ chooses, ctrl i imports.
        catalog->entries = {QVariantMap{{"id", "engkjv"}, {"language", "eng"}, {"name", "King James Version"}, {"abbreviation", "KJV"}, {"description", "A fixture edition."}},
                            QVariantMap{{"id", "engweb"}, {"language", "eng"}, {"name", "World English Bible"}, {"abbreviation", "WEB"}, {"description", "A fixture edition."}}};
        emit catalog->changed();
        item("importCancel")->forceActiveFocus(); key(Qt::Key_Down);
        QCOMPARE(object("importGuide")->property("catalogIndex").toInt(), 1); QVERIFY(item("importCancel")->hasActiveFocus());
        key(Qt::Key_I, Qt::ControlModifier); QTRY_VERIFY(object("importDialog")->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(object("importDialog"), "close"));
        QTRY_VERIFY(!object("importDialog")->property("visible").toBool());
        // Mouse: hovering a catalog row tints it under a pointing hand.
        window->requestActivate(); QTRY_COMPARE(QGuiApplication::focusWindow(), window);
        run("import"); QTRY_VERIFY(opened("importGuide"));
        auto row = item("catalogRow:engweb"); QTRY_VERIFY(row && row->isVisible());
        QCOMPARE(item("catalogRowMouse:engweb")->cursor().shape(), Qt::PointingHandCursor);
        QTest::mouseMove(window, row->mapToScene(QPointF(10, 6)).toPoint()); QTRY_VERIFY(item("catalogRowTint:engweb")->isVisible());
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("importGuide"));
    }
    void reauditFooterEscapeAndOutlineKeys() {
        // esc on a footer button hands the keyboard back to the reader first: the range stays.
        bible->selectVerses(3, 5); call("readerFocus");
        item("textControl")->forceActiveFocus(); key(Qt::Key_Escape);
        QTRY_VERIFY(readerFocused()); QCOMPARE(bible->selectionReference(), QString("John 3:3–5"));
        // The chapter outline: ↑ ↓ choose an entry, enter jumps, { } step sections.
        bible->setChapterOutline(true); navigate("John 3:3");
        QTRY_VERIFY(item("chapterOutline")->isVisible());
        item("chapterOutline")->forceActiveFocus();
        auto outline = object("chapterOutline");
        QCOMPARE(outline->property("keyIndex").toInt(), outline->property("currentIndex").toInt());
        key(Qt::Key_End); key(Qt::Key_Home);
        while (outline->property("keyIndex").toInt() > 0) key(Qt::Key_Up);
        key(Qt::Key_Up); QCOMPARE(lastSound(), QString("boundary"));
        key(Qt::Key_Down); key(Qt::Key_Down); QCOMPARE(outline->property("keyIndex").toInt(), 2);
        QVERIFY(item("outlineEntryKey:20")->isVisible());
        key(Qt::Key_Return); QTRY_COMPARE(bible->cursorVerse(), 20);
        item("chapterOutline")->forceActiveFocus(); QTest::keyClick(window, '{'); QTRY_VERIFY(bible->cursorVerse() < 20);
    }
    void reauditSeeAlsoRowsAndTooltips() {
        bible->selectVerses(7, 7); const QString alpha = bible->createAndAssignTag(bible->tagScope(), "Alpha");
        bible->selectVerses(3, 3); QVERIFY(bible->setTag(bible->tagScope(), alpha, true));
        navigate("John 3:3");
        auto tags = object("tagsUi");
        run("related"); QTRY_VERIFY(panelOpened("related")); QTRY_VERIFY(item("studySectionTag:Alpha"));
        // Mouse: leaving a heading's tag hides its tooltip.
        auto link = item("studySectionTag:Alpha"); auto tip = toolTipOf(link);
        QTest::mouseMove(window, center(link) + QPoint(1, 1)); QTest::mouseMove(window, center(link)); QTRY_VERIFY(tip->property("visible").toBool());
        QTest::mouseMove(window, QPoint(10, window->height() / 2)); QTRY_VERIFY(!tip->property("visible").toBool());
        // Keyboard: with no row (a filter matching nothing), t opens the tag picker for the passage.
        key(Qt::Key_Slash); QTRY_VERIFY(item("studySearch")->hasActiveFocus()); type("nothing here");
        key(Qt::Key_Escape); QTRY_VERIFY(item("studyList")->hasActiveFocus()); QCOMPARE(item("studyList")->property("count").toInt(), 0);
        key(Qt::Key_T); QTRY_VERIFY(opened("tagsSurface"));
        QCOMPARE(value(tags->property("editingScope")).toMap()["start"].toInt(), 3);
        // Tag rows never take tab.
        auto tagRow = item("tag:" + alpha); QVERIFY(tagRow); QCOMPARE(tagRow->property("focusPolicy").toInt(), int(Qt::NoFocus));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("tagsSurface"));
        closeMargin();
        // See also's rows never take tab either; home and end move the list.
        navigate("John 1:1");
        run("related"); QTRY_VERIFY(panelOpened("related")); QTRY_VERIFY(item("studyRow:0"));
        QCOMPARE(item("studyRow:0")->property("focusPolicy").toInt(), int(Qt::NoFocus));
        QTRY_VERIFY(item("studyList")->hasActiveFocus());
        key(Qt::Key_End); QCOMPARE(item("studyList")->property("currentIndex").toInt(), item("studyList")->property("count").toInt() - 1);
        key(Qt::Key_Home); QCOMPARE(item("studyList")->property("currentIndex").toInt(), 0);
        closeMargin();
    }
    void reauditListsTakeHomeEndAndEnterAnywhere() {
        for (const int verse : {5, 7, 9}) { bible->selectVerses(verse, verse); QVERIFY(bible->toggleHighlight()); }
        navigate("John 3:3");
        run("annotations"); QTRY_VERIFY(panelOpened("marks")); QTRY_VERIFY(item("collectionList")->hasActiveFocus());
        key(Qt::Key_End); QCOMPARE(item("collectionList")->property("currentIndex").toInt(), 2);
        key(Qt::Key_Home); QCOMPARE(item("collectionList")->property("currentIndex").toInt(), 0);
        key(Qt::Key_End);
        // enter opens the current row with the keyboard elsewhere in the panel.
        item("savedContent")->forceActiveFocus(); QVERIFY(!item("collectionList")->hasActiveFocus());
        key(Qt::Key_Return); QTRY_COMPARE(bible->selectionReference(), QString("John 3:9"));
    }
    void reauditPaletteAndKeybindingKeys() {
        // Keybindings: home and end move the search caret, not the list.
        key(Qt::Key_F1); QTRY_VERIFY(item("keybindingSearch")->hasActiveFocus());
        type("verse"); key(Qt::Key_Down); const int row = item("keybindingResults")->property("currentIndex").toInt();
        key(Qt::Key_Home); QCOMPARE(item("keybindingSearch")->property("cursorPosition").toInt(), 0);
        key(Qt::Key_End); QCOMPARE(item("keybindingSearch")->property("cursorPosition").toInt(), 5);
        QCOMPARE(item("keybindingResults")->property("currentIndex").toInt(), row);
        // Rows keep clear of the scrollbar.
        key(Qt::Key_A, Qt::ControlModifier); key(Qt::Key_Backspace);
        auto results = item("keybindingResults"); auto current = results->property("currentItem").value<QQuickItem *>();
        QTRY_VERIFY(item("keybindingScrollbar")->isVisible());
        QCOMPARE(current->width(), results->width() - item("keybindingScrollbar")->width());
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("helpDialog"));
        // A book's chapter grid names its keys: arrows choose, backspace returns
        // to the books; with a number typed ← moves the caret. ↑ on Back is an error.
        run("books"); QTRY_VERIFY(opened("passagePalette"));
        type("John"); key(Qt::Key_Right); QTRY_VERIFY(prop("paletteBook").toInt() > 0);
        QVERIFY(object("paletteHints")->property("plainText").toString().contains("← → ↑ ↓ choose"));
        QVERIFY(object("paletteHints")->property("plainText").toString().contains("backspace books"));
        type("3"); window->setProperty("paletteArrowUsed", false);
        QTRY_VERIFY(!prop("paletteLeftGoesBack").toBool());
        key(Qt::Key_Left); QVERIFY(prop("paletteBook").toInt() > 0);
        item("paletteBack")->forceActiveFocus(); key(Qt::Key_Up); QCOMPARE(lastSound(), QString("error"));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("passagePalette"));
        // Mouse: an unavailable command row reads muted; a click sounds the error.
        bible->clearSelection(); run("commands"); QTRY_VERIFY(opened("passagePalette"));
        type("highlight selection"); QTRY_VERIFY(object("paletteResults")->property("currentItem").value<QQuickItem *>());
        auto entry = object("paletteResults")->property("currentItem").value<QQuickItem *>();
        QVERIFY(!entry->property("available").toBool());
        const int plays = playCount(); QTest::qWait(50); click(entry);
        QCOMPARE(playCount(), plays + 1); QCOMPARE(lastSound(), QString("error")); QVERIFY(shown("passagePalette"));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("passagePalette"));
    }
    void reauditRejectionsSound() {
        // ctrl t during a Manage tags confirmation.
        const auto scope = bible->tagScope(); QVERIFY(!bible->createAndAssignTag(scope, "Alpha").isEmpty());
        auto tags = object("tagsUi");
        key(Qt::Key_T, Qt::ControlModifier); QTRY_VERIFY(opened("tagsSurface"));
        QTRY_VERIFY(item("tagSearch")->hasActiveFocus()); type("alp"); key(Qt::Key_Return);
        QTRY_VERIFY(item("tagRenameInput")->hasActiveFocus()); key(Qt::Key_Escape); QTRY_VERIFY(item("tagList")->hasActiveFocus());
        key(Qt::Key_Delete); QTRY_VERIFY(!value(tags->property("confirmation")).isNull());
        int plays = playCount(); key(Qt::Key_T, Qt::ControlModifier);
        QCOMPARE(playCount(), plays + 1); QCOMPARE(lastSound(), QString("error")); QCOMPARE(tags->property("page").toString(), QString("manage"));
        // Mouse: a click on the scrim cancels the confirmation; the picker stays.
        clickAt(QPoint(4, window->height() / 2)); QTRY_VERIFY(value(tags->property("confirmation")).isNull());
        QVERIFY(opened("tagsSurface")); QVERIFY(QStringLiteral("Alpha") == bible->tags().first().toMap()["name"].toString());
        // Keyboard: the hint's enter selects the focused button (Cancel).
        QTRY_VERIFY(item("tagSearch")->hasActiveFocus()); key(Qt::Key_Down); QTRY_VERIFY(item("tagList")->hasActiveFocus());
        key(Qt::Key_Delete); QTRY_VERIFY(!value(tags->property("confirmation")).isNull());
        QTRY_VERIFY(item("cancelTagManagement")->hasActiveFocus());
        clickHint("tagsHintsPart:select"); QTRY_VERIFY(value(tags->property("confirmation")).isNull()); QCOMPARE(bible->tags().size(), 1);
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("tagsSurface"));
        // A disabled context menu row.
        call("readerFocus");
        QVariantList entries{QVariantMap{{"id", "off"}, {"text", "Off"}, {"enabled", false}}, QVariantMap{{"id", "on"}, {"text", "On"}}};
        QVERIFY(QMetaObject::invokeMethod(menu(), "show", Q_ARG(QVariant, "test"), Q_ARG(QVariant, entries), Q_ARG(QVariant, 200), Q_ARG(QVariant, 200)));
        QTRY_VERIFY(menu()->property("opened").toBool()); QTest::qWait(50);
        plays = playCount(); click(item("contextMenuItem:off"));
        QCOMPARE(playCount(), plays + 1); QCOMPARE(lastSound(), QString("error")); QVERIFY(menu()->property("visible").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!menu()->property("visible").toBool());
        // Chapter find in a narrow window (hints hidden): tab is an error.
        window->resize(520, 700); QTRY_COMPARE(window->width(), 520);
        call("readerFocus"); QTest::keyClick(window, '/'); QTRY_VERIFY(prop("findPromptOpen").toBool());
        QVERIFY(!item("chapterFindHints")->isVisible());
        plays = playCount(); key(Qt::Key_Tab); QCOMPARE(playCount(), plays + 1); QCOMPARE(lastSound(), QString("error"));
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("findPromptOpen").toBool());
    }
    void reauditBackupAndErrorDialogHints() {
        run("backup"); QTRY_VERIFY(opened("backupScope")); QTRY_VERIFY(item("backupChoose")->hasActiveFocus());
        QTRY_VERIFY(object("backupScopeHints")->property("plainText").toString().contains("enter select"));
        key(Qt::Key_Left); QVERIFY(item("backupCancel")->hasActiveFocus());
        key(Qt::Key_Right); QVERIFY(item("backupChoose")->hasActiveFocus());
        clickHint("backupScopeHintsPart:cancel"); QTRY_VERIFY(!shown("backupScope"));
        auto error = object("errorDialog"); error->setProperty("message", "Something failed");
        QVERIFY(QMetaObject::invokeMethod(error, "open")); QTRY_VERIFY(opened("errorDialog"));
        QTRY_VERIFY(object("errorDialogHints")->property("plainText").toString().contains("enter dismiss"));
        clickHint("errorDialogHintsPart:dismiss"); QTRY_VERIFY(!shown("errorDialog")); QTRY_VERIFY(readerFocused());
    }
    void moveHereFromAMarkerMenu() {
        bible->selectVerses(5, 5); bible->setNoteText("Moving note");
        // The verses to move it to are selected first.
        navigate("John 3:3"); bible->selectVerses(6, 7);
        auto marker = item("verseNoteMarker5"); QTRY_VERIFY(marker && marker->isVisible());
        // Mouse: the note's marker menu moves it here.
        click(marker, Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
        QCOMPARE(QString::fromUtf8(value(menu()->property("layout")).toList().at(2).toMap()["text"].toString().toUtf8()), QString::fromUtf8("Move here (John 3:6–7)"));
        chooseMenu("moveHere");
        QTRY_VERIFY(bible->selectionReference() == QString::fromUtf8("John 3:6–7") && bible->noteText() == "Moving note");
        QVERIFY(object("toast")->property("text").toString().startsWith(QString::fromUtf8("John 3:5 moved to John 3:6–7")));
        // u puts it back.
        call("readerFocus"); key(Qt::Key_U); QTRY_VERIFY(bible->noteText().isEmpty());
        bible->selectVerses(5, 5); QCOMPARE(bible->noteText(), QString("Moving note"));
        // With the mark's own verses selected, there is nowhere to move it.
        auto again = item("verseNoteMarker5"); QTRY_VERIFY(again && again->isVisible());
        click(again, Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
        QVERIFY(!menuEnabled("moveHere"));
        key(Qt::Key_Escape); QTRY_VERIFY(!menu()->property("visible").toBool());
    }
    void reauditMarkerMenusChooseOverlappingRanges() {
        bible->selectVerses(5, 6); QVERIFY(!bible->createAndAssignTag(bible->tagScope(), "Overlap").isEmpty());
        bible->selectVerses(5, 7); QVERIFY(!bible->createAndAssignTag(bible->tagScope(), "Overlap").isEmpty());
        navigate("John 3:3");
        auto marker = item("verseTagMarker5"); QTRY_VERIFY(marker && marker->isVisible());
        click(marker, Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
        QVERIFY(menuIds().contains("copyPlain"));
        // Copy plain text…: a second menu asks which range.
        QGuiApplication::clipboard()->clear(); QTest::qWait(60); click(item("contextMenuItem:copyPlain"));
        QTRY_COMPARE(menu()->property("target").toString(), QString("markerChoice")); QTRY_VERIFY(menu()->property("opened").toBool());
        QCOMPARE(menuIds().size(), 2);
        chooseMenu("range:1"); QTRY_VERIFY(!QGuiApplication::clipboard()->text().isEmpty());
        QVERIFY(!QGuiApplication::clipboard()->text().startsWith(QChar(0x201c)));
    }
    void reauditMoreMenus() {
        // Versions rows: Switch · Details & Attribution · Remove….
        run("switchVersion"); QTRY_VERIFY(opened("versionDialog")); QTRY_VERIFY(item("versionEntry0"));
        QTest::qWait(50); click(item("versionEntry0"), Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
        QCOMPARE(menu()->property("target").toString(), QString("version"));
        QCOMPARE(menuIds(), QStringList({"switch", "details", "|", "removeVersion"}));
        QVERIFY(!menuEnabled("switch")); // the current version
        chooseMenu("removeVersion"); QTRY_COMPARE(object("versionDialog")->property("step").toInt(), 1);
        // Mouse back steps back, then closes the dialog.
        clickAt(QPoint(20, 20), Qt::BackButton); QTRY_COMPARE(object("versionDialog")->property("step").toInt(), 0);
        clickAt(QPoint(20, 20), Qt::BackButton); QTRY_VERIFY(!shown("versionDialog"));
        // Mouse back closes Keybindings and Reading settings too.
        key(Qt::Key_F1); QTRY_VERIFY(opened("helpDialog")); clickAt(QPoint(20, 20), Qt::BackButton); QTRY_VERIFY(!shown("helpDialog"));
        run("settings"); QTRY_VERIFY(opened("settingsDialog")); clickAt(QPoint(20, 20), Qt::BackButton); QTRY_VERIFY(!shown("settingsDialog"));
        // Right click on the chapter title (an empty spot for the menu): the passage menu there.
        call("readerFocus"); bible->clearSelection(); object("reading")->setProperty("contentY", 0.0);
        auto title = item("chapterTitle"); QTRY_VERIFY(title && title->isVisible());
        click(title, Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
        QCOMPARE(menu()->property("target").toString(), QString("passage")); QVERIFY(prop("hasSelection").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!menu()->property("visible").toBool());
        // Its hover reads as a link; verse numbers name the middle click.
        QTest::mouseMove(window, center(item("chapterTitleMouse")));
        QTRY_COMPARE(title->property("color").value<QColor>(), desktop->property("accent").value<QColor>());
    }
    void reauditTextFieldsUseTheAppMenu() {
        run("books"); QTRY_VERIFY(opened("passagePalette")); type("john");
        auto input = item("referenceInput");
        click(input, Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
        QCOMPARE(menu()->property("target").toString(), QString("text"));
        QCOMPARE(menuIds(), QStringList({"cut", "copyField", "paste", "selectAll"}));
        QVERIFY(shown("passagePalette"));
        chooseMenu("selectAll"); QTRY_COMPARE(input->property("selectedText").toString(), QString("john"));
        QGuiApplication::clipboard()->clear();
        key(Qt::Key_Menu); QTRY_VERIFY(menu()->property("opened").toBool()); QCOMPARE(menu()->property("target").toString(), QString("text"));
        chooseMenu("copyField"); QTRY_COMPARE(QGuiApplication::clipboard()->text(), QString("john"));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("passagePalette"));
        // The note editor adds Open link and Preview.
        navigate("John 3:16"); key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        type("see John 1:1");
        click(item("noteEditor"), Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
        QCOMPARE(menuIds(), QStringList({"cut", "copyField", "paste", "selectAll", "|", "openLink", "preview"}));
        chooseMenu("preview"); QTRY_VERIFY(prop("notePreview").toBool());
    }
    void reauditContextMenuScrollsInShortWindows() {
        window->resize(1000, 260); QTRY_COMPARE(window->height(), 260);
        call("readerFocus"); key(Qt::Key_Menu); QTRY_VERIFY(menu()->property("opened").toBool());
        auto content = item("contextMenuContent");
        QTRY_VERIFY(menu()->property("height").toReal() <= window->height());
        QTRY_VERIFY(item("contextMenuScrollbar")->isVisible());
        key(Qt::Key_End); QTRY_VERIFY(content->property("contentY").toReal() > 0);
        auto last = item("contextMenuItem:" + menuIds().last());
        const QRectF shown = last->mapRectToItem(content, last->boundingRect());
        QVERIFY(shown.top() >= -1 && shown.bottom() <= content->height() + 1);
        key(Qt::Key_Home); QTRY_COMPARE(content->property("contentY").toReal(), 0.0);
        key(Qt::Key_Escape);
    }
    void reauditFocusFooterHoverAndWheel() {
        key(Qt::Key_F); QVERIFY(bible->focusMode());
        auto zone = item("footerZone"); QVERIFY(zone);
        const QPoint inZone = zone->mapToScene(QPointF(zone->width() / 2 - 200, zone->height() - 4)).toPoint();
        QTest::mouseMove(window, inZone); QTRY_VERIFY(!prop("footerQuiet").toBool());
        // Moving onto a revealed button and back never hides the footer in between.
        for (const char *name : {"textControl", "searchControl", "helpControl"}) {
            QTest::mouseMove(window, center(item(name))); QTest::qWait(30); QVERIFY(!prop("footerQuiet").toBool());
            QTest::mouseMove(window, inZone); QTest::qWait(30); QVERIFY(!prop("footerQuiet").toBool());
        }
        // Leaving hides it after a short delay.
        QTest::mouseMove(window, QPoint(window->width() / 2, 200)); QTRY_VERIFY(prop("footerQuiet").toBool());
        // The wheel over the footer zone scrolls the reader.
        const qreal before = readerY();
        QWheelEvent event(QPointF(inZone), QPointF(inZone), QPoint(), QPoint(0, -240), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QGuiApplication::sendEvent(window, &event); QCoreApplication::processEvents();
        QTRY_VERIFY(readerY() > before);
    }
    void reauditChapterFindChromeClicks() {
        // A settled chapter: a late text load would refresh the search and close the prompt.
        navigate("John 3:1");
        call("readerFocus"); QTest::keyClick(window, '/'); QTRY_VERIFY(prop("findPromptOpen").toBool());
        // Let the typed preview settle, as the other find tests do.
        type("words"); QTest::qWait(220);
        auto find = item("chapterFind");
        // Mouse: a click on the prompt's padding stays in the prompt.
        clickAt(find->mapToScene(QPointF(4, find->height() / 2)).toPoint());
        QVERIFY(prop("findPromptOpen").toBool()); QTRY_VERIFY(item("chapterFindInput")->hasActiveFocus());
        clickAt(center(item("chapterFindCount")));
        QVERIFY(prop("findPromptOpen").toBool());
        // Right click in the field: the app's text menu.
        click(item("chapterFindInput"), Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
        QCOMPARE(menu()->property("target").toString(), QString("text"));
        key(Qt::Key_Escape); QTRY_VERIFY(!menu()->property("visible").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("findPromptOpen").toBool());
    }
    void namesAreActionsKeysAreDescriptions() {
        // Footer buttons: the accessible name says what it does, the
        // description its keys; the tooltip shows both.
        for (const auto &[name, label, action] : QList<std::tuple<QString, QString, QString>>{
                {"searchControl", "Search Bible", "ctrl /"}, {"savedControl", "Marks", "shift M"},
                {"deskControl", "Desk", "shift D"}, {"helpControl", "Keybindings", "?"}}) {
            auto button = item(name); QVERIFY(button);
            QCOMPARE(button->property("label").toString(), label);
            QCOMPARE(button->property("keys").toString(), action);
            QCOMPARE(button->property("tip").toString(), label + " · " + action);
            QObject *accessible = nullptr;
            for (auto *child : button->children())
                if (QByteArray(child->metaObject()->className()) == "QQuickAccessibleAttached") accessible = child;
            QVERIFY(accessible);
            QCOMPARE(accessible->property("name").toString(), label);
            QCOMPARE(accessible->property("description").toString(), action);
        }
        // Errors take the theme's error color; selected text keeps its look (dots underline it).
        QVERIFY(!bible->navigate("Nowhere 99")); QTRY_VERIFY(shown("errorBanner"));
        QVariant tinted;
        QVERIFY(QMetaObject::invokeMethod(window, "tint", Q_RETURN_ARG(QVariant, tinted),
            Q_ARG(QVariant, desktop->property("error")), Q_ARG(QVariant, 0.12)));
        QCOMPARE(object("errorBanner")->property("color").value<QColor>(), tinted.value<QColor>());
        auto body = item("verseBody1") ? item("verseBody1") : item("blockBody"); QVERIFY(body);
        QCOMPARE(body->property("selectionColor").value<QColor>().alpha(), 0);
        QCOMPARE(body->property("selectedTextColor").value<QColor>().alpha(), 0);
        bible->clearError();
    }
};

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QQuickStyle::setStyle("Basic"); QApplication app(argc, argv);
    A11yUiTest test; return QTest::qExec(&test, argc, argv);
}
#include "tst_a11yui.moc"
