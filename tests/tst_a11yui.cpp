#include <QtTest/QTest>
#include <QApplication>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QFile>
#include <QJSValue>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTemporaryDir>
#include <QUuid>
#include <functional>
#include <archive.h>
#include <archive_entry.h>
#include "biblebackend.h"
#include "blockdocument.h"
#include "desktop.h"
#include "soundeffects.h"

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
    // Rewrites a schema 4 generation as the schema 3 layout older builds wrote.
    static bool downgradeToSchema3(const QString &path) {
        const QString name = QUuid::createUuid().toString();
        bool ok = false;
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name); db.setDatabaseName(path);
            if (db.open()) {
                QSqlQuery q(db);
                ok = q.exec("DROP TABLE notes") && q.exec("DROP TABLE words_of_jesus")
                    && q.exec("DELETE FROM metadata WHERE key IN ('note_count','words_of_jesus')")
                    && q.exec("UPDATE metadata SET value='3' WHERE key='schema'")
                    && q.exec("UPDATE verses SET html=replace(replace(html,'<span class=\"wj\">',''),'</span>','')");
                QCryptographicHash hash(QCryptographicHash::Sha256);
                auto field = [&](const QByteArray &v) { hash.addData(QByteArray::number(v.size())); hash.addData(":"); hash.addData(v); };
                for (const QString &sql : {QString("SELECT key,value FROM metadata WHERE key<>'content_digest' ORDER BY key"),
                         QString("SELECT id,code,name,abbreviation,ordinal FROM books ORDER BY ordinal"),
                         QString("SELECT book,chapter FROM source_chapters ORDER BY book,chapter"),
                         QString("SELECT book,name FROM book_aliases ORDER BY book,name"),
                         QString("SELECT book,chapter,number,retained FROM source_keys ORDER BY book,chapter,number"),
                         QString("SELECT book,chapter,number,text,html,heading,poetry,paragraphStart FROM verses ORDER BY book,chapter,number")}) {
                    field(sql.toUtf8()); ok = ok && q.exec(sql);
                    while (q.next()) for (int i = 0; i < q.record().count(); ++i) field(q.value(i).toString().toUtf8());
                }
                q.prepare("UPDATE metadata SET value=? WHERE key='content_digest'"); q.addBindValue(QString::fromLatin1(hash.result().toHex()));
                ok = ok && q.exec();
                db.close();
            }
        }
        QSqlDatabase::removeDatabase(name);
        return ok;
    }
    static bool edition(const QString &path) {
        auto zip = archive_write_new(); archive_write_set_format_zip(zip);
        if (archive_write_open_filename(zip, QFile::encodeName(path).constData()) != ARCHIVE_OK) { archive_write_free(zip); return false; }
        auto book = [](const char *code, const char *name, int chapters, int verses, auto special) {
            QByteArray data = QString("\\id %1 Original access fixture\n\\h %2\n\\toc1 %2\n\\toc2 %2\n").arg(code, name).toUtf8();
            for (int c = 1; c <= chapters; ++c) {
                data += QString("\\c %1\n\\s Access fixture heading\n\\p\n").arg(c).toUtf8();
                for (int v = 1; v <= verses; ++v) {
                    if (v == 20) data += "\\s A later heading\n\\p\n";
                    const QString text = special(c, v);
                    data += QString("\\v %1 %2\n").arg(v).arg(text.isEmpty()
                        ? QString("Original access text %1:%2 with enough ordinary words to wrap across a reading line.").arg(c).arg(v) : text).toUtf8();
                }
            }
            return data;
        };
        const QMap<QString, QByteArray> entries = {
            {"01-GENaccess.usfm", book("GEN", "Genesis", 2, 31, [](int, int) { return QString(); })},
            {"43-JHNaccess.usfm", book("JHN", "John", 3, 36, [](int c, int v) {
                if (c == 1 && v == 1) return QString("In the beginning was the Word.\\x - \\xo 1:1 \\xt Gen 1:1; Jn 1:14; Gen 2:3\\x* Original words follow.");
                if (c == 3 && v == 3) return QString("Jesus answered,\\f + \\fr 3:3 \\ft Or \\fq from above\\f* truly unless one is born again.");
                if (c == 3 && v == 16) return QString("For God so loved\\f a \\ft Greek \\fq agapao\\f* the world that he gave his Son.");
                return QString(); })},
        };
        bool ok = true;
        for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
            auto entry = archive_entry_new(); const auto name = it.key().toUtf8();
            archive_entry_set_pathname(entry, name.constData()); archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_perm(entry, 0600); archive_entry_set_size(entry, it.value().size());
            ok = archive_write_header(zip, entry) == ARCHIVE_OK && archive_write_data(zip, it.value().constData(), it.value().size()) == it.value().size() && ok;
            archive_entry_free(entry);
        }
        ok = archive_write_close(zip) == ARCHIVE_OK && ok; archive_write_free(zip); return ok;
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
        bible->importUsfm(QUrl::fromLocalFile(file)); QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(), 30000);
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
    bool opened(const QString &name) const { auto o = object(name); return o && o->property("opened").toBool(); }
    bool shown(const QString &name) const { auto o = object(name); return o && o->property("visible").toBool(); }
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
            for (const QPointF local : {QPointF(30, 8), QPointF(body->width() / 2, 8), QPointF(body->width() - 30, 8)}) {
                const QPoint point = body->mapToScene(local).toPoint();
                if (point.y() > 60 && point.y() < window->height() - 100 && !area.adjusted(-8, -8, 8, 8).contains(point)) return point;
            }
        }
        return {};
    }

    // Character selection (shift V) state, as {verse, offset} maps.
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
        if (QByteArray(QTest::currentTestFunction()).startsWith("reimport")) {
            // An installed generation from before schema 4: Versions offers Re-import.
            const QString file = installedFile(); bible.reset();
            QVERIFY(downgradeToSchema3(file)); bible = std::make_unique<BibleBackend>();
            QVERIFY2(bible->ready(), qPrintable(bible->error())); QCOMPARE(bible->textSchema(), 3);
        }
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
        auto layout = item("layoutControl"); QVERIFY(layout);
        layout->forceActiveFocus(); QCOMPARE(bible->readingMode(), QString("verse"));
        key(Qt::Key_Return); QCOMPARE(bible->readingMode(), QString("block"));
        key(Qt::Key_Enter); QCOMPARE(bible->readingMode(), QString("paragraph"));
        auto search = item("searchControl"); search->forceActiveFocus();
        key(Qt::Key_Space); QTRY_VERIFY(opened("passagePalette"));
        QCOMPARE(object("referenceInput")->property("text").toString(), QString("/ "));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("passagePalette"));
        // A dialog's QuietButton: enter and space both activate it.
        QVERIFY(bible->navigate("John 3:5")); QVERIFY(bible->addToDesk());
        run("desk"); QTRY_VERIFY(opened("deskDrawer"));
        auto clean = item("deskClean"); clean->forceActiveFocus(); key(Qt::Key_Space);
        QTRY_VERIFY(opened("cleanDeskConfirmation"));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("cleanDeskConfirmation"));
        clean->forceActiveFocus(); key(Qt::Key_Return); QTRY_VERIFY(opened("cleanDeskConfirmation"));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("cleanDeskConfirmation"));
    }
    void chromeButtonsTakeFocusOnlyByTab() {
        // Mouse: a click never moves the keyboard off the reader.
        QVERIFY(readerFocused());
        auto layout = item("layoutControl");
        QCOMPARE(layout->property("focusPolicy").toInt(), int(Qt::TabFocus));
        click(layout); QCOMPARE(bible->readingMode(), QString("block")); QVERIFY(readerFocused());
        // Reached by tab, a mouse click hands the keyboard back to the reader.
        layout->forceActiveFocus(); QVERIFY(layout->hasActiveFocus());
        click(layout); QCOMPARE(bible->readingMode(), QString("paragraph")); QTRY_VERIFY(readerFocused());
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
        QVERIFY(bible->navigate("John 3:5")); QVERIFY(bible->addToDesk());
        QVERIFY(bible->navigate("John 3:7")); QVERIFY(bible->addToDesk());
        navigate("John 3:3");
        run("desk"); QTRY_VERIFY(opened("deskDrawer")); QTRY_VERIFY(item("deskList")->hasActiveFocus());
        auto choose = item("deskHintsPart:choose"), copy = item("deskHintsPart:copy");
        QVERIFY(choose && copy);
        QVERIFY(!choose->property("clickable").toBool()); QVERIFY(copy->property("clickable").toBool());
        // The row reads as before: the same markup.
        QVERIFY(object("deskHints")->property("text").toString().contains("shift&nbsp;delete"));
        // Unhovered parts show no tint; hovering a clickable one tints it under a pointing hand.
        auto tint = item("deskHintsPart:copy:tint"); QVERIFY(tint); QVERIFY(!tint->isVisible());
        shot("hints-desk-unhovered");
        QTest::mouseMove(window, center(copy)); QTRY_VERIFY(tint->isVisible());
        shot("hints-desk-hover-copy");
        QQuickItem *area = nullptr;
        for (auto child : copy->childItems()) if (QByteArray(child->metaObject()->className()).contains("MouseArea")) area = child;
        QVERIFY(area); QCOMPARE(area->cursor().shape(), Qt::PointingHandCursor);
        QTest::mouseMove(window, center(choose)); QTRY_VERIFY(!tint->isVisible());
        QVERIFY(!item("deskHintsPart:choose:tint")->isVisible());
        // Mouse: a part runs what its key does, and the keyboard stays in the list.
        const QString current = bible->desk().at(item("deskList")->property("currentIndex").toInt()).toMap()["reference"].toString();
        QGuiApplication::clipboard()->clear();
        click(copy); QTRY_VERIFY(QGuiApplication::clipboard()->text().contains(current));
        QVERIFY(item("deskList")->hasActiveFocus());
        // Keyboard: the key itself still works.
        QGuiApplication::clipboard()->clear(); key(Qt::Key_Y); QTRY_VERIFY(QGuiApplication::clipboard()->text().contains(current));
        clickHint("deskHintsPart:remove"); QCOMPARE(bible->desk().size(), 1);
        key(Qt::Key_Delete); QCOMPARE(bible->desk().size(), 0);
        // Nothing left to open: those parts stop being clickable.
        QTRY_VERIFY(!item("deskHintsPart:open")->property("clickable").toBool());
        QVERIFY(!item("deskHintsPart:clean")->property("clickable").toBool());
        clickHint("deskHintsPart:close"); QTRY_VERIFY(!shown("deskDrawer"));
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
        clickHint("lineHeightHintsPart:increase"); QCOMPARE(bible->lineHeight(), 1.6);
        clickHint("lineHeightHintsPart:reset"); QCOMPARE(bible->lineHeight(), 1.5);
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("settingsDialog"));
        // Annotations: open the current row by its hint.
        QVERIFY(bible->navigate("John 3:9")); bible->selectVerses(9, 9); QVERIFY(bible->toggleBookmark());
        navigate("John 1:1");
        run("annotations"); QTRY_VERIFY(opened("savedDrawer"));
        QTRY_VERIFY(item("savedHintsPart:open")->property("clickable").toBool());
        clickHint("savedHintsPart:open"); QTRY_VERIFY(!shown("savedDrawer"));
        QCOMPARE(bible->selectionReference(), QString("John 3:9"));
        // Confirmations: cancel by the hint, confirm with enter on the focused button.
        QVERIFY(bible->addToDesk()); run("desk"); QTRY_VERIFY(opened("deskDrawer"));
        key(Qt::Key_Delete, Qt::ShiftModifier); QTRY_VERIFY(opened("cleanDeskConfirmation"));
        clickHint("cleanDeskHintsPart:cancel"); QTRY_VERIFY(!shown("cleanDeskConfirmation"));
        QCOMPARE(bible->desk().size(), 1);
        key(Qt::Key_Delete, Qt::ShiftModifier); QTRY_VERIFY(opened("cleanDeskConfirmation"));
        key(Qt::Key_Right); QVERIFY(item("cleanDeskConfirm")->hasActiveFocus());
        clickHint("cleanDeskHintsPart:select"); QTRY_VERIFY(bible->desk().isEmpty());
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("deskDrawer"));
        // Study and the tag picker.
        run("related"); QTRY_VERIFY(opened("relatedDrawer"));
        QVERIFY(!item("studyHintsPart:tabs")->property("clickable").toBool());
        clickHint("studyHintsPart:close"); QTRY_VERIFY(!shown("relatedDrawer"));
        run("tags"); QTRY_VERIFY(opened("tagsSurface")); type("Grace");
        clickHint("tagsHintsPart:toggle / create"); QTRY_COMPARE(bible->tags().size(), 1);
        clickHint("tagsHintsPart:close"); QTRY_VERIFY(!shown("tagsSurface"));
    }
    void everyListRowSharesOneMenu() {
        const QStringList shared = {"open", "copy", "copyPlain", "desk"};
        // Desk (mouse): Open · Copy · Copy plain text · Remove from Desk.
        QVERIFY(bible->navigate("John 3:5")); QVERIFY(bible->addToDesk()); navigate("John 3:3");
        run("desk"); QTRY_VERIFY(opened("deskDrawer"));
        click(item("deskEntry0"), Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
        QCOMPARE(menuIds(), shared);
        QCOMPARE(menuShortcut("copyPlain"), QString("shift Y")); QCOMPARE(menuShortcut("desk"), QString("d"));
        QGuiApplication::clipboard()->clear(); chooseMenu("copyPlain");
        QTRY_VERIFY(!QGuiApplication::clipboard()->text().isEmpty());
        QVERIFY(!QGuiApplication::clipboard()->text().contains("John 3:5"));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("deskDrawer"));
        // History (keyboard): the menu key opens the row menu with Remove after a divider.
        QVERIFY(bible->recordHistory()); run("history"); QTRY_VERIFY(opened("historyDrawer"));
        key(Qt::Key_Menu); QTRY_VERIFY(menu()->property("opened").toBool());
        QCOMPARE(menuIds(), (shared + QStringList{"|", "remove"}));
        key(Qt::Key_Escape); QTRY_VERIFY(!menu()->property("visible").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("historyDrawer"));
        // Annotations rows: the shared four, then Edit range and Delete.
        bible->selectVerses(3, 3); QVERIFY(bible->toggleBookmark());
        run("annotations"); QTRY_VERIFY(opened("savedDrawer"));
        key(Qt::Key_Menu); QTRY_VERIFY(menu()->property("opened").toBool());
        QCOMPARE(menuIds(), (shared + QStringList{"|", "editRange", "delete"}));
        key(Qt::Key_Escape); key(Qt::Key_Escape); QTRY_VERIFY(!shown("savedDrawer"));
        // Study Cross-references: mouse and keyboard.
        navigate("John 1:1"); run("related"); QTRY_VERIFY(opened("relatedDrawer"));
        QTRY_VERIFY(item("studyRow:0") && item("studyRow:0")->isVisible());
        click(item("studyRow:0"), Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
        QCOMPARE(menuIds(), shared);
        QGuiApplication::clipboard()->clear(); chooseMenu("copy");
        QTRY_VERIFY(QGuiApplication::clipboard()->text().contains("Genesis 1:1"));
        key(Qt::Key_Menu); QTRY_VERIFY(menu()->property("opened").toBool()); QCOMPARE(menuIds(), shared);
        key(Qt::Key_Escape); QTRY_VERIFY(!menu()->property("visible").toBool());
        // Footnotes rows too.
        key(Qt::Key_L); QTRY_COMPARE(object("tagsUi")->property("studyTab").toString(), QString("footnotes"));
        navigate("John 3:3"); run("related"); QTRY_VERIFY(opened("relatedDrawer"));
        QTRY_VERIFY(item("studyRow:0") && item("studyRow:0")->isVisible());
        click(item("studyRow:0"), Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
        QCOMPARE(menuIds(), shared);
        key(Qt::Key_Escape); key(Qt::Key_Escape); QTRY_VERIFY(!shown("relatedDrawer"));
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
        QCOMPARE(menuIds(), (QStringList{"open", "copy", "copyPlain", "desk"}));
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
        key(Qt::Key_J); QTRY_VERIFY(contentY("settingsScroll") > 0);
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
            {"abbreviation", QString("V%1").arg(i)}, {"copyright", QString("A long copyright notice. ").repeated(40)}, {"detailsUrl", "https://example.invalid"}};
        catalog->entries = entries; emit catalog->changed();
        bible->setFontSize(26); window->resize(900, 480); QTRY_COMPARE(window->width(), 900);
        run("import"); QTRY_VERIFY(opened("importGuide")); QTRY_VERIFY(item("catalogSearch")->hasActiveFocus());
        QTRY_VERIFY(travel("importGuideScroll") > 20);
        key(Qt::Key_PageDown); QTRY_VERIFY(contentY("importGuideScroll") > 0);
        QVERIFY(item("catalogSearch")->hasActiveFocus());
        key(Qt::Key_PageUp); QTRY_COMPARE(contentY("importGuideScroll"), 0.0);
        click(item("catalogDetails:v0")); QTRY_VERIFY(opened("catalogDetails")); QTRY_VERIFY(item("detailsScroll")->hasActiveFocus());
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
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("notesDrawer"));
    }
    void outlineAndCombineDialogScrollByKey() {
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
        // Combine notes: the two notes scroll by key.
        auto editor = object("rangeEditor"); auto merge = object("rangeNoteMerge");
        QString note; for (int i = 0; i < 60; ++i) note += QString("Note line %1\n").arg(i);
        editor->setProperty("pending", QVariantMap{{"preview", QVariantMap{{"sourceNote", note}, {"destinationNote", note}}}});
        QVERIFY(QMetaObject::invokeMethod(merge, "open")); QTRY_VERIFY(merge->property("opened").toBool());
        QTRY_VERIFY(travel("rangeMergeScroll") > 20);
        key(Qt::Key_End); QTRY_VERIFY(contentY("rangeMergeScroll") > 0);
        key(Qt::Key_Home); QTRY_COMPARE(contentY("rangeMergeScroll"), 0.0);
        QVERIFY(QMetaObject::invokeMethod(merge, "close")); editor->setProperty("pending", QVariant());
    }
    void rejectedKeysAndRowsSoundTheError() {
        bible->clearSelection(); call("readerFocus"); QTRY_VERIFY(!prop("hasSelection").toBool());
        // Keyboard: a reader key that cannot act now (bookmark with nothing selected).
        int before = playCount(); key(Qt::Key_M);
        QCOMPARE(playCount(), before + 1); QCOMPARE(lastSound(), QString("error")); QVERIFY(bible->collection().isEmpty());
        // A disabled command row: enter sounds, the palette stays.
        run("commands"); QTRY_VERIFY(opened("passagePalette")); type("bookmark");
        QTRY_COMPARE(item("paletteResults")->property("count").toInt(), 1);
        before = playCount(); key(Qt::Key_Return);
        QCOMPARE(lastSound(), QString("error")); QVERIFY(playCount() > before); QVERIFY(opened("passagePalette"));
        // Mouse: a disabled row is skipped, nothing runs.
        before = playCount();
        click(qobject_cast<QQuickItem *>(item("paletteResults")->property("currentItem").value<QObject *>()));
        QCOMPARE(playCount(), before); QVERIFY(opened("passagePalette"));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("passagePalette"));
        // A disabled keybinding row.
        run("help"); QTRY_VERIFY(opened("helpDialog")); type("bookmark selection");
        before = playCount(); key(Qt::Key_Return);
        QCOMPARE(lastSound(), QString("error")); QVERIFY(playCount() > before); QVERIFY(opened("helpDialog"));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("helpDialog"));
        // An empty list: y has no row to copy.
        run("desk"); QTRY_VERIFY(opened("deskDrawer"));
        before = playCount(); key(Qt::Key_Y); QCOMPARE(lastSound(), QString("error")); QVERIFY(playCount() > before);
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("deskDrawer"));
    }
    void outsideClicksOnlyClose() {
        bible->selectVerses(3, 3); call("readerFocus");
        auto check = [&](const QString &popup, std::function<void()> open) {
            open(); QTRY_VERIFY2(opened(popup), qPrintable(popup));
            const QPoint point = outsidePoint(popup); QVERIFY2(!point.isNull(), qPrintable(popup));
            const int before = playCount();
            clickAt(point); QTRY_VERIFY2(!shown(popup), qPrintable(popup));
            QVERIFY2(bible->selectedStart() == 3 && bible->selectedEnd() == 3, qPrintable(popup + ": " + bible->selectionReference()));
            QVERIFY2(bible->collection().isEmpty() && bible->desk().isEmpty(), qPrintable(popup));
            QVERIFY(playCount() >= before);
            QTest::qWait(30);
        };
        check("passagePalette", [&] { run("books"); });
        check("helpDialog", [&] { run("help"); });
        check("settingsDialog", [&] { run("settings"); });
        check("versionDialog", [&] { run("switchVersion"); });
        check("aboutDialog", [&] { run("about"); });
        check("savedDrawer", [&] { run("annotations"); });
        check("deskDrawer", [&] { run("desk"); });
        check("historyDrawer", [&] { run("history"); });
        check("relatedDrawer", [&] { run("related"); });
        check("notesDrawer", [&] { run("notes"); });
        // The picker nearly fills the window: give it room above to click outside.
        window->resize(1200, 1100); QTRY_COMPARE(window->height(), 1100); call("setReaderY", 0);
        check("tagsSurface", [&] { run("tags"); });
        window->resize(1200, 820); QTRY_COMPARE(window->height(), 820);
        // A confirmation over a drawer: the outside click cancels only the confirmation.
        QVERIFY(bible->navigate("John 3:30")); QVERIFY(bible->addToDesk()); navigate("John 3:3"); bible->selectVerses(3, 3);
        run("desk"); QTRY_VERIFY(opened("deskDrawer")); QTRY_VERIFY(item("deskList")->hasActiveFocus());
        key(Qt::Key_Delete, Qt::ShiftModifier);
        clickAt(outsidePoint("cleanDeskConfirmation")); QTRY_VERIFY(!shown("cleanDeskConfirmation"));
        QVERIFY(opened("deskDrawer")); QCOMPARE(bible->desk().size(), 1);
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("deskDrawer"));
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

    void rangeEditingMenuAndHints() {
        bible->selectVerses(5, 5); QVERIFY(bible->toggleBookmark()); call("readerFocus");
        // Keyboard: shift E, j, then the menu key offers Save · Swap · Cancel; enter saves.
        key(Qt::Key_E, Qt::ShiftModifier); QTRY_VERIFY(object("rangeEditor")->property("active").toBool());
        key(Qt::Key_J); QCOMPARE(bible->selectedEnd(), 6);
        // The status bar keeps its one-line height: title and hints share a row.
        QVERIFY2(item("rangeEditStatus")->height() < 100, qPrintable(QString::number(item("rangeEditStatus")->height())));
        QCOMPARE(item("rangeEditHints")->mapToScene(QPointF()).y(), item("rangeEditTitle")->mapToScene(QPointF()).y());
        key(Qt::Key_Menu); QTRY_VERIFY(menu()->property("opened").toBool());
        QCOMPARE(menuIds(), (QStringList{"saveRange", "swapEnds", "cancelRange", "|", "copy", "copyPlain"}));
        QCOMPARE(menuShortcut("saveRange"), QString("enter")); QCOMPARE(menuShortcut("swapEnds"), QString("o"));
        QCOMPARE(menuShortcut("cancelRange"), QString("esc"));
        key(Qt::Key_Return); QTRY_VERIFY(!prop("rangeEditing").toBool());
        QCOMPARE(bible->collection().first().toMap()["end"].toInt(), 6);
        // Mouse: right-click a verse while moving; Swap ends, then Cancel restores.
        navigate("John 3:5"); bible->selectVerses(5, 6); call("readerFocus");
        key(Qt::Key_E, Qt::ShiftModifier); QTRY_VERIFY(object("rangeEditor")->property("active").toBool());
        const int cursor = bible->cursorVerse();
        clickAt(versePoint(7), Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
        QCOMPARE(bible->selectedStart(), 5); QCOMPARE(bible->selectedEnd(), 6);
        chooseMenu("swapEnds"); QVERIFY(bible->cursorVerse() != cursor);
        clickAt(versePoint(7), Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
        chooseMenu("cancelRange"); QTRY_VERIFY(!prop("rangeEditing").toBool());
        // The status bar's hints are clickable: o swap, enter save, esc cancel.
        key(Qt::Key_E, Qt::ShiftModifier); QTRY_VERIFY(object("rangeEditor")->property("active").toBool());
        QVERIFY(item("rangeEditHintsPart:save")->property("clickable").toBool());
        key(Qt::Key_J); clickHint("rangeEditHintsPart:swap"); QCOMPARE(object("rangeEditor")->property("endpoint").toString(), QString("start"));
        clickHint("rangeEditHintsPart:save"); QTRY_VERIFY(!prop("rangeEditing").toBool());
        QCOMPARE(bible->collection().first().toMap()["end"].toInt(), 7);
        key(Qt::Key_E, Qt::ShiftModifier); QTRY_VERIFY(object("rangeEditor")->property("active").toBool());
        clickHint("rangeEditHintsPart:cancel"); QTRY_VERIFY(!prop("rangeEditing").toBool());
    }
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
        catalog->entries = {QVariantMap{{"id", id}, {"name", "Access fixture"}, {"abbreviation", "OAF"}, {"copyright", "Original"}, {"detailsUrl", "https://example.invalid"}}};
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
        bible->selectVerses(5, 5); QVERIFY(bible->toggleBookmark()); bible->setNoteText("A note on five");
        bible->selectVerses(4, 6); QVERIFY(bible->toggleBookmark());
        navigate("John 3:5"); bible->selectVerses(5, 5); call("readerFocus");
        // The verse rails are mouse only: tab never lands on them.
        for (int i = 0; i < 25; ++i) {
            key(Qt::Key_Tab);
            QVERIFY2(!focusName().contains("Marker"), qPrintable(focusName()));
        }
        call("readerFocus");
        // Keyboard: the menu adds Open bookmark (two overlap: a chooser) and Open note.
        key(Qt::Key_Menu); QTRY_VERIFY(menu()->property("opened").toBool());
        QCOMPARE(menuIds().mid(menuIds().size() - 3), (QStringList{"|", "openBookmark", "openNote"}));
        key(Qt::Key_End); key(Qt::Key_Return); QTRY_VERIFY(opened("notesDrawer"));
        QCOMPARE(bible->selectionReference(), QString("John 3:5"));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("notesDrawer"));
        // Mouse: right-click, Open bookmark, then pick the wider range in the chooser.
        clickAt(versePoint(5), Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
        chooseMenu("openBookmark");
        QTRY_VERIFY(object("verseTagMenu")->property("opened").toBool());
        auto wide = item("verseTagAction:John 3:4–6"); QTRY_VERIFY(wide && wide->isVisible());
        click(wide); QTRY_COMPARE(bible->selectionReference(), QString("John 3:4–6"));
    }
    void notesTabReachesLinkedFrom() {
        QVERIFY(bible->navigate("John 1:5")); bible->selectVerses(5, 5); bible->setNoteText("See John 3:16 here.");
        QVERIFY(bible->navigate("John 1:7")); bible->selectVerses(7, 7); bible->setNoteText("Also John 3:16.");
        navigate("John 3:16"); bible->selectVerses(16, 16); call("readerFocus");
        key(Qt::Key_W); QTRY_VERIFY(opened("notesDrawer")); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        // The editor hint names tab; tab moves to the list and never types.
        QTRY_VERIFY(item("notesHintsPart:linked from"));
        key(Qt::Key_Tab); QTRY_VERIFY(item("linkedFrom")->hasActiveFocus());
        QCOMPARE(item("noteEditor")->property("text").toString(), QString());
        QVERIFY(item("linkedHints")->isVisible());
        QCOMPARE(item("linkedFromRowBackground0")->property("color").value<QColor>().alphaF() > 0, true);
        key(Qt::Key_J); QCOMPARE(item("linkedFrom")->property("currentIndex").toInt(), 1);
        key(Qt::Key_K); QCOMPARE(item("linkedFrom")->property("currentIndex").toInt(), 0);
        // esc returns to the note (Notes stays open); shift tab too.
        key(Qt::Key_Escape); QTRY_VERIFY(item("noteEditor")->hasActiveFocus()); QVERIFY(opened("notesDrawer"));
        key(Qt::Key_Tab); QTRY_VERIFY(item("linkedFrom")->hasActiveFocus());
        key(Qt::Key_Tab, Qt::ShiftModifier); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        // Enter opens the linking note; Notes stays open on it.
        key(Qt::Key_Tab); QTRY_VERIFY(item("linkedFrom")->hasActiveFocus());
        key(Qt::Key_Return); QTRY_COMPARE(bible->selectionReference(), QString("John 1:5"));
        QVERIFY(opened("notesDrawer"));
        // [[ suggestions: tab accepts the suggestion instead.
        QVERIFY(QMetaObject::invokeMethod(window, "closeNotes")); QTRY_VERIFY(!shown("notesDrawer"));
        navigate("John 3:16"); bible->selectVerses(16, 16); key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        type("[[jn 3:1"); QTRY_VERIFY(prop("linkSuggesting").toBool());
        key(Qt::Key_Tab); QTRY_VERIFY(!prop("linkSuggesting").toBool());
        QVERIFY(item("noteEditor")->hasActiveFocus());
        QVERIFY(item("noteEditor")->property("text").toString().startsWith("John 3:1"));
        // Mouse: the editor's tab hint and the list's open hint.
        item("noteEditor")->setProperty("text", QString());
        clickHint("notesHintsPart:linked from"); QTRY_VERIFY(item("linkedFrom")->hasActiveFocus());
        key(Qt::Key_J); clickHint("linkedHintsPart:open"); QTRY_COMPARE(bible->selectionReference(), QString("John 1:7"));
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
        key(Qt::Key_Tab); QCOMPARE(focusName(), QString("searchControl")); QVERIFY(!prop("footerQuiet").toBool());
        call("readerFocus"); QVERIFY(prop("footerQuiet").toBool());
        // Space on the focused focus button leaves focus mode.
        for (int i = 0; i < 30 && focusName() != "focusControl"; ++i) key(Qt::Key_Tab);
        key(Qt::Key_Space); QVERIFY(!bible->focusMode()); QVERIFY(!prop("footerQuiet").toBool());
        QCOMPARE(item("searchControl")->mapToScene(QPointF()), searchAt);
        // Mouse: resting on the footer reveals it after 250 ms; a click in the next 300 ms is ignored.
        call("readerFocus"); key(Qt::Key_F); QVERIFY(bible->focusMode());
        QTest::mouseMove(window, center(item("layoutControl")));
        QTest::qWait(120); QVERIFY(prop("footerQuiet").toBool());
        QTRY_VERIFY(!prop("footerQuiet").toBool()); QVERIFY(prop("footerGuard").toBool());
        const QString mode = bible->readingMode();
        click(item("layoutControl")); QCOMPARE(bible->readingMode(), mode);
        QTRY_VERIFY(!prop("footerGuard").toBool());
        QTRY_COMPARE(item("layoutControl")->opacity(), 1.0);
        shot("focus-footer-revealed");
        click(item("layoutControl")); QVERIFY(bible->readingMode() != mode);
        QVERIFY(readerFocused());
        QTest::mouseMove(window, QPoint(window->width() / 2, 200)); QTRY_VERIFY(prop("footerQuiet").toBool());
        // The focus button by mouse; the passage menu offers Leave focus mode (f).
        key(Qt::Key_Menu); QTRY_VERIFY(menu()->property("opened").toBool());
        QCOMPARE(menuIds().last(), QString("leaveFocus")); QCOMPARE(menuShortcut("leaveFocus"), QString("f"));
        chooseMenu("leaveFocus"); QVERIFY(!bible->focusMode());
        key(Qt::Key_F); QVERIFY(bible->focusMode());
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
        // A side panel: back closes it (and does not travel).
        run("desk"); QTRY_VERIFY(opened("deskDrawer"));
        clickAt(middle, Qt::BackButton); QTRY_VERIFY(!shown("deskDrawer"));
        QCOMPARE(bible->selectionReference(), QString("John 2:5")); QVERIFY(readerFocused());
        // Keyboard equivalent for the panel: esc.
        run("desk"); QTRY_VERIFY(opened("deskDrawer")); key(Qt::Key_Escape); QTRY_VERIFY(!shown("deskDrawer"));
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
            // Mouse: drag over text, then right-click on it.
            QTest::mousePress(window, Qt::LeftButton, {}, from);
            for (int i = 1; i <= 5; ++i) QTest::mouseMove(window, from + (to - from) * i / 5);
            QTest::mouseRelease(window, Qt::LeftButton, {}, to);
            QTRY_VERIFY2(!prop("freeSelectedText").toString().isEmpty(), qPrintable(mode));
            const QString selected = prop("freeSelectedText").toString();
            const auto selection = bible->tagScope();
            clickAt((from + to) / 2, Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
            QCOMPARE(menuIds().first(), QString("copySelected")); QCOMPARE(menuShortcut("copySelected"), QString("ctrl c"));
            QCOMPARE(prop("freeSelectedText").toString(), selected); QCOMPARE(bible->tagScope(), selection);
            // Copy selected text is a quotation (ctrl c); the plain entry follows.
            QGuiApplication::clipboard()->clear(); chooseMenu("copySelected");
            QTRY_VERIFY(QGuiApplication::clipboard()->text().endsWith(QStringLiteral("”\n\n—John 3:4")));
            QCOMPARE(prop("freeSelectedText").toString(), selected);
            // Keyboard: the menu key offers it first too; enter copies.
            key(Qt::Key_Menu); QTRY_VERIFY(menu()->property("opened").toBool());
            QCOMPARE(menuIds().first(), QString("copySelected"));
            QGuiApplication::clipboard()->clear(); key(Qt::Key_Down); key(Qt::Key_Return);
            QTRY_COMPARE(QGuiApplication::clipboard()->text(), selected);
            key(Qt::Key_Escape); QTRY_VERIFY(prop("freeSelectedText").toString().isEmpty());
        }
    }

    // ── Phase 3

    void characterSelectionByKeysInEveryLayout() {
        for (const QString mode : {"verse", "block", "paragraph"}) {
            setMode(mode); navigate("John 3:16"); call("readerFocus");
            const QString v16 = verseText(16), v17 = verseText(17);
            QCOMPARE(v16, QString("For God so loved the world that he gave his Son."));
            key(Qt::Key_V, Qt::ShiftModifier);
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
            navigate("John 3:16"); key(Qt::Key_V, Qt::ShiftModifier); QTRY_VERIFY(prop("charMode").toBool());
            keys("l"); keys("iw"); QVERIFY(charAt(charAnchor(), 16, 0)); QVERIFY(charAt(charCursor(), 16, 2));
            keys("aw"); QVERIFY(charAt(charCursor(), 16, 6));
            keys("y"); QVERIFY(!prop("charMode").toBool());
            QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("“For God…”\n\n—John 3:16"));
            QCOMPARE(lastSound(), QString("chime"));
            // Across a verse break: y quotes with the later verse's number, shift Y is plain.
            key(Qt::Key_V, Qt::ShiftModifier); QTRY_VERIFY(prop("charMode").toBool());
            keys("wo"); keys(")w");
            QVERIFY(charAt(charAnchor(), 16, 4)); QVERIFY(charAt(charCursor(), 17, 9));
            const QString plain = v16.mid(4) + " " + v17.left(10);
            key(Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier); QVERIFY(!prop("charMode").toBool());
            QCOMPARE(QGuiApplication::clipboard()->text(), plain);
            key(Qt::Key_V, Qt::ShiftModifier); keys("wo)w"); QVERIFY(charAt(charCursor(), 17, 9));
            key(Qt::Key_C, Qt::ControlModifier);
            QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("“…God so loved the world that he gave his Son. ¹⁷ Original a…”\n\n—John 3:16–17"));
            key(Qt::Key_V, Qt::ShiftModifier); keys(")h"); keys("Y");
            QCOMPARE(QGuiApplication::clipboard()->text(), v16);
            QVERIFY(warnings.isEmpty());
        }
    }
    void characterSelectionRendersInEveryLayout() {
        for (const QString mode : {"verse", "block", "paragraph"}) {
            setMode(mode); navigate("John 3:16"); call("readerFocus");
            key(Qt::Key_V, Qt::ShiftModifier); QTRY_VERIFY(prop("charMode").toBool());
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
    void characterSelectionByMouse() {
        for (const QString mode : {"verse", "block"}) {
            setMode(mode); navigate("John 3:16"); bible->clearSelection(); call("readerFocus");
            // Mouse: a drag inside verse 16 copies a quotation with ellipses on ctrl c.
            const QString v16 = verseText(16);
            const QPoint from = charPoint(16, 4), to = charPoint(16, 16);
            QVERIFY(!from.isNull() && !to.isNull());
            drag(from, to); QTRY_VERIFY(!prop("freeSelectedText").toString().isEmpty());
            QGuiApplication::clipboard()->clear();
            key(Qt::Key_C, Qt::ControlModifier);
            const QString quote = QGuiApplication::clipboard()->text();
            QVERIFY2(quote.startsWith(QStringLiteral("“…God so loved")) && quote.endsWith(QStringLiteral("…”\n\n—John 3:16")), qPrintable(mode + ": " + quote));
            QCOMPARE(toast(), QString("Selected text copied as quotation"));
            key(Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
            QCOMPARE(QGuiApplication::clipboard()->text(), prop("freeSelectedText").toString());
            QVERIFY(QGuiApplication::clipboard()->text().startsWith("God so loved"));
            // The menu copies either way.
            clickAt((from + to) / 2, Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
            QCOMPARE(menuIds().mid(0, 2), QStringList({"copySelected", "copySelectedPlain"}));
            QCOMPARE(menuShortcut("copySelectedPlain"), QString("ctrl shift C"));
            QGuiApplication::clipboard()->clear(); chooseMenu("copySelected");
            QTRY_VERIFY(QGuiApplication::clipboard()->text().startsWith(QStringLiteral("“…God")));
            // A drag across a verse break: the later verse is numbered.
            const QPoint end = charPoint(17, 7);
            drag(from, end); QTRY_VERIFY(!prop("freeSelectedText").toString().isEmpty());
            key(Qt::Key_C, Qt::ControlModifier);
            QVERIFY2(QGuiApplication::clipboard()->text().contains(QStringLiteral("Son. ¹⁷ Origin")), qPrintable(QGuiApplication::clipboard()->text()));
            QVERIFY(QGuiApplication::clipboard()->text().endsWith(QStringLiteral("—John 3:16–17")));
            // shift V turns the dragged text into a character selection.
            key(Qt::Key_V, Qt::ShiftModifier); QTRY_VERIFY(prop("charMode").toBool());
            QCOMPARE(charAnchor()["verse"].toInt(), 16); QCOMPARE(charCursor()["verse"].toInt(), 17);
            QVERIFY(readerFocused());
            // Mouse: the hint parts act; quotation copies and leaves.
            QGuiApplication::clipboard()->clear();
            clickHint("characterHintsPart:quotation");
            QTRY_VERIFY(!prop("charMode").toBool());
            QVERIFY(QGuiApplication::clipboard()->text().endsWith(QStringLiteral("—John 3:16–17")));
            // The footer mode button leaves character selection; a click in the text does too.
            key(Qt::Key_V, Qt::ShiftModifier); QTRY_VERIFY(prop("charMode").toBool());
            click("modeControl"); QVERIFY(!prop("charMode").toBool()); QVERIFY(readerFocused());
            key(Qt::Key_V, Qt::ShiftModifier); QTRY_VERIFY(prop("charMode").toBool());
            clickHint("characterHintsPart:exit"); QVERIFY(!prop("charMode").toBool());
            key(Qt::Key_V, Qt::ShiftModifier); QTRY_VERIFY(prop("charMode").toBool());
            const QPoint inside = charPoint(18, 3); QVERIFY2(!inside.isNull(), qPrintable(mode));
            clickAt(inside); QTRY_VERIFY2(!prop("charMode").toBool(), qPrintable(mode + QString(" %1,%2").arg(inside.x()).arg(inside.y())));
            QVERIFY(v16.size() > 0);
        }
    }
    void rangeEditorBackspaceAndCombineKeys() {
        bible->selectVerses(2, 5); bible->setNoteText("Destination");
        bible->selectVerses(3, 3); QVERIFY(bible->toggleHighlight());
        bible->selectVerses(2, 4); QVERIFY(bible->toggleBookmark()); bible->setNoteText("Source");
        call("readerFocus"); auto editor = object("rangeEditor");
        run("editRange"); QTRY_VERIFY(opened("rangeChooser")); QTest::qWait(60);
        const QString source = QStringLiteral("rangeChoice:John 3:2–4");
        // Keyboard: backspace goes from the components back to the range list.
        click(source); QTRY_VERIFY(editor->property("source").isValid() && !editor->property("source").value<QJSValue>().isNull());
        QTRY_VERIFY(shown("rangeChooseAnother"));
        QVERIFY(object("rangeChooserKeys")->property("plainText").toString().contains("backspace another range"));
        key(Qt::Key_Backspace); QTRY_VERIFY(shown(source)); QVERIFY(!shown("rangeChooseAnother"));
        // Mouse: the hint part does the same.
        click(source); QTRY_VERIFY(shown("rangeChooseAnother"));
        clickHint("rangeChooserKeysPart:another range"); QTRY_VERIFY(shown(source));
        click(source); QTRY_VERIFY(shown("rangeContinue")); key(Qt::Key_Return);
        QTRY_VERIFY(editor->property("active").toBool()); QTRY_VERIFY(prop("readerKeys").toBool()); call("readerFocus");
        // Combine notes: Combine has the keyboard; esc steps back to moving the range.
        key(Qt::Key_J); key(Qt::Key_Return); QTRY_VERIFY(opened("rangeNoteMerge"));
        QTRY_VERIFY(item("rangeMergeCombine")->hasActiveFocus());
        QVERIFY(object("rangeMergeKeys")->property("plainText").toString().contains("enter combine")); shot("range-merge");
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("rangeNoteMerge"));
        QVERIFY(editor->property("active").toBool()); QVERIFY(editor->property("sessionOpen").toBool()); QTRY_VERIFY(readerFocused());
        // ← → move between the buttons.
        key(Qt::Key_Return); QTRY_VERIFY(opened("rangeNoteMerge")); QTRY_VERIFY(item("rangeMergeCombine")->hasActiveFocus());
        key(Qt::Key_Right); QVERIFY(item("rangeMergeBack")->hasActiveFocus());
        key(Qt::Key_Left); QVERIFY(item("rangeMergeCombine")->hasActiveFocus());
        // Mouse: back by its hint, then combine by its hint.
        clickHint("rangeMergeKeysPart:back"); QTRY_VERIFY(!shown("rangeNoteMerge")); QVERIFY(editor->property("sessionOpen").toBool());
        call("readerFocus"); key(Qt::Key_Return); QTRY_VERIFY(opened("rangeNoteMerge"));
        clickHint("rangeMergeKeysPart:combine"); QTRY_VERIFY(!editor->property("sessionOpen").toBool());
        bible->selectVerses(2, 5); QCOMPARE(bible->noteText(), QString("Destination\n\nSource"));
        // Keyboard: enter on the focused Combine button combines too.
        bible->selectVerses(6, 6); QVERIFY(bible->toggleBookmark()); bible->setNoteText("Six");
        bible->selectVerses(6, 7); bible->setNoteText("Seven"); bible->selectVerses(6, 6); call("readerFocus");
        run("editRange"); QTRY_VERIFY(opened("rangeChooser")); QTest::qWait(60);
        click(QStringLiteral("rangeChoice:John 3:6")); QTRY_VERIFY(shown("rangeContinue")); key(Qt::Key_Return);
        QTRY_VERIFY(editor->property("active").toBool()); QTRY_VERIFY(prop("readerKeys").toBool()); call("readerFocus");
        key(Qt::Key_J); key(Qt::Key_Return); QTRY_VERIFY(opened("rangeNoteMerge")); QTRY_VERIFY(item("rangeMergeCombine")->hasActiveFocus());
        key(Qt::Key_Return); QTRY_VERIFY(!editor->property("sessionOpen").toBool());
        bible->selectVerses(6, 7); QCOMPARE(bible->noteText(), QString("Seven\n\nSix"));
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
        for (const int verse : {5, 7}) { bible->selectVerses(verse, verse); QVERIFY(bible->toggleBookmark()); }
        navigate("John 3:3");
        run("annotations"); QTRY_VERIFY(opened("savedDrawer")); QTRY_VERIFY(item("collectionList")->hasActiveFocus());
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
        QDesktopServices::unsetUrlHandler("https");
    }
    void addVersionGuideKeys() {
        catalog->entries = {QVariantMap{{"id", "engkjv"}, {"name", "King James Version"}, {"abbreviation", "KJV"}, {"copyright", "Public domain"}, {"detailsUrl", "https://ebible.org/kjv"}},
                            QVariantMap{{"id", "engweb"}, {"name", "World English Bible"}, {"abbreviation", "WEB"}, {"copyright", "Public domain"}, {"detailsUrl", "https://ebible.org/web"}}};
        emit catalog->changed();
        run("import"); QTRY_VERIFY(opened("importGuide")); QTRY_VERIFY(item("catalogSearch")->hasActiveFocus());
        auto guide = object("importGuide");
        QCOMPARE(guide->property("catalogIndex").toInt(), 0); QVERIFY(item("catalogRowTint:engkjv")->isVisible());
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
        run("import"); QTRY_VERIFY(opened("importGuide"));
        auto row = item("catalogRow:engweb"); shot("add-version-guide"); clickAt(row->mapToScene(QPointF(row->width() - 20, 6)).toPoint());
        QCOMPARE(guide->property("catalogIndex").toInt(), 1); QVERIFY(!shown("catalogDetails"));
        clickHint("importGuideHintsPart:install"); QCOMPARE(catalog->requests, QStringList({"engkjv", "engweb"}));
        catalog->busy = false; emit catalog->changed();
        clickHint("importGuideHintsPart:details"); QTRY_VERIFY(opened("catalogDetails"));
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
            QVERIFY2(tip->property("text").toString().contains(QStringLiteral("shift S → Footnotes")), qPrintable(tip->property("text").toString()));
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
            key(Qt::Key_S, Qt::ShiftModifier); QTRY_VERIFY(opened("relatedDrawer"));
            key(Qt::Key_Escape); QTRY_VERIFY(!shown("relatedDrawer"));
        }
    }
    void markersHaveLargerHitAreas() {
        for (const QString mode : {"verse", "block"}) {
            setMode(mode); navigate("John 3:3"); bible->clearSelection();
            auto marker = visualPrefix(window->contentItem(), mode == "verse" ? "verseFootnoteMark:3." : "blockInlineMarker:footnote:");
            auto hit = visualPrefix(window->contentItem(), mode == "verse" ? "verseFootnoteHit:3." : "blockInlineHit:footnote:");
            QVERIFY(marker && hit);
            // Larger than the mark, and the text did not move.
            QVERIFY(hit->width() > marker->width() + 6); QVERIFY(hit->height() > marker->height() + 6);
            // Mouse: a click just beside the drawn cell still opens the footnote.
            const QPointF edge = marker->mapToScene(QPointF(marker->width() + 3, marker->height() / 2));
            clickAt(edge.toPoint()); QTRY_VERIFY(opened("relatedDrawer"));
            QCOMPARE(bible->selectionReference(), QString("John 3:3"));
            key(Qt::Key_Escape); QTRY_VERIFY(!shown("relatedDrawer"));
        }
        // Inline annotation markers too (flowing layouts).
        bible->selectVerses(5, 6); QVERIFY(bible->toggleBookmark()); navigate("John 3:3");
        setMode("block");
        auto glyph = visualPrefix(window->contentItem(), "blockInlineMarker:bookmark:open");
        auto glyphHit = visualPrefix(window->contentItem(), "blockInlineHit:bookmark:open");
        QVERIFY(glyph && glyphHit); QVERIFY(glyphHit->width() > glyph->width() + 6);
        // Keyboard: the opening marker still takes tab and opens on enter.
        glyph->forceActiveFocus(); key(Qt::Key_Return); QTRY_COMPARE(bible->selectionReference(), QString("John 3:5–6"));
    }
    void settingsSlidersTakeLeftAndRight() {
        run("settings"); QTRY_VERIFY(opened("settingsDialog"));
        auto font = item("fontSizeSlider"), line = item("lineHeightSlider");
        // Keyboard: → and ↑ increase, ← and ↓ decrease.
        font->forceActiveFocus();
        QTRY_VERIFY(object("fontSizeHints")->property("plainText").toString().contains(QStringLiteral("← → adjust")));
        key(Qt::Key_Right); QCOMPARE(bible->fontSize(), 19);
        key(Qt::Key_Left); key(Qt::Key_Left); QCOMPARE(bible->fontSize(), 17);
        key(Qt::Key_Up); QCOMPARE(bible->fontSize(), 18);
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
    void globalKeysLeaveSidePanels() {
        QVERIFY(bible->addToDesk()); bible->selectVerses(3, 3); QVERIFY(bible->toggleBookmark());
        struct Panel { Qt::Key key; const char *drawer; };
        const QList<Panel> panels = {{Qt::Key_D, "deskDrawer"}, {Qt::Key_A, "savedDrawer"}, {Qt::Key_H, "historyDrawer"}, {Qt::Key_S, "relatedDrawer"}};
        for (const auto &panel : panels) {
            auto open = [&] { call("readerFocus"); key(panel.key, Qt::ShiftModifier); QTRY_VERIFY2(opened(panel.drawer), panel.drawer); QTest::qWait(30); };
            // Keyboard: the palette, a dialog, and a reading toggle each close the panel first.
            open(); key(Qt::Key_K, Qt::ControlModifier);
            QTRY_VERIFY2(opened("passagePalette"), panel.drawer); QTRY_VERIFY(!shown(panel.drawer));
            key(Qt::Key_Escape); QTRY_VERIFY(!shown("passagePalette"));
            open(); key(Qt::Key_R, Qt::ControlModifier); QTRY_VERIFY(opened("settingsDialog")); QTRY_VERIFY(!shown(panel.drawer));
            key(Qt::Key_Escape); QTRY_VERIFY(!shown("settingsDialog"));
            open(); const bool numbers = bible->showVerseNumbers();
            key(Qt::Key_N, Qt::AltModifier); QCOMPARE(bible->showVerseNumbers(), !numbers); QTRY_VERIFY(!shown(panel.drawer));
            bible->setShowVerseNumbers(numbers);
        }
        open: {
            call("readerFocus"); key(Qt::Key_D, Qt::ShiftModifier); QTRY_VERIFY(opened("deskDrawer")); QTest::qWait(30);
            key(Qt::Key_Question); QTRY_VERIFY(opened("helpDialog")); QTRY_VERIFY(!shown("deskDrawer"));
            key(Qt::Key_Escape); QTRY_VERIFY(!shown("helpDialog"));
            call("readerFocus"); key(Qt::Key_D, Qt::ShiftModifier); QTRY_VERIFY(opened("deskDrawer")); QTest::qWait(30);
            key(Qt::Key_F); QTRY_VERIFY(bible->focusMode()); QTRY_VERIFY(!shown("deskDrawer"));
            bible->setFocusMode(false);
            // ctrl t opens Manage tags over the panel, which stays.
            call("readerFocus"); key(Qt::Key_D, Qt::ShiftModifier); QTRY_VERIFY(opened("deskDrawer")); QTest::qWait(30);
            key(Qt::Key_T, Qt::ControlModifier); QTRY_VERIFY(opened("tagsSurface")); QVERIFY(shown("deskDrawer"));
            key(Qt::Key_Escape); QTRY_VERIFY(!shown("tagsSurface"));
        }
        // Notes is not modal: from the preview (not while typing) the palette opens
        // over it and returns to it, and a reading toggle acts with it open.
        call("readerFocus"); key(Qt::Key_Escape); run("notes"); QTRY_VERIFY(opened("notesDrawer")); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        key(Qt::Key_K, Qt::ControlModifier); QVERIFY(!shown("passagePalette")); QVERIFY(opened("notesDrawer"));
        key(Qt::Key_V, Qt::AltModifier); QTRY_VERIFY(item("notePreview")->hasActiveFocus());
        key(Qt::Key_K, Qt::ControlModifier); QTRY_VERIFY(opened("passagePalette")); QVERIFY(shown("notesDrawer"));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("passagePalette")); QTRY_VERIFY(item("notePreview")->hasActiveFocus());
        const bool numbers = bible->showVerseNumbers();
        key(Qt::Key_N, Qt::AltModifier); QCOMPARE(bible->showVerseNumbers(), !numbers); QVERIFY(opened("notesDrawer"));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("notesDrawer"));
        // Mouse: a click outside an open panel only closes it (the footer button underneath does not act).
        call("readerFocus"); key(Qt::Key_D, Qt::ShiftModifier); QTRY_VERIFY(opened("deskDrawer")); QTest::qWait(30);
        click("searchControl"); QTRY_VERIFY(!shown("deskDrawer")); QVERIFY(!shown("passagePalette"));
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
        for (const auto &id : {"previousChapter", "nextChapter", "previousHeading", "nextHeading"}) QVERIFY2(menuIds().contains(id), id);
        QCOMPARE(menuShortcut("previousChapter"), QString("h")); QCOMPARE(menuShortcut("nextChapter"), QString("l"));
        QCOMPARE(menuShortcut("previousHeading"), QString("{")); QCOMPARE(menuShortcut("nextHeading"), QString("}"));
        // Keyboard: choose Next chapter in the menu.
        menuKeyTo("nextChapter"); QTRY_COMPARE(bible->chapter(), 3);
        // Mouse: Previous chapter, then Next heading.
        clickAt(versePoint(5), Qt::RightButton); chooseMenu("previousChapter"); QTRY_COMPARE(bible->chapter(), 2);
        navigate("John 2:5");
        clickAt(versePoint(5), Qt::RightButton); chooseMenu("nextHeading"); QTRY_COMPARE(bible->cursorVerse(), 20);
        clickAt(versePoint(20), Qt::RightButton); chooseMenu("previousHeading"); QTRY_VERIFY(bible->cursorVerse() < 20);
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
            QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("“…loved…”\n\n—John 3:16"));
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
            key(Qt::Key_Escape); QTRY_VERIFY(!shown("notesDrawer"));
            // Keyboard: w writes a note; shift V and iw select a word.
            call("readerFocus"); key(Qt::Key_W); QTRY_VERIFY(opened("notesDrawer"));
            key(Qt::Key_Escape); QTRY_VERIFY(!shown("notesDrawer"));
        }
    }
    void studyTagChipsAndCountByMouseAndKey() {
        bible->selectVerses(7, 7); const QString alpha = bible->createAndAssignTag(bible->tagScope(), "Alpha");
        bible->selectVerses(9, 9); const QString beta = bible->createAndAssignTag(bible->tagScope(), "Beta");
        bible->selectVerses(3, 3); QVERIFY(bible->setTag(bible->tagScope(), alpha, true)); QVERIFY(bible->setTag(bible->tagScope(), beta, true));
        navigate("John 3:3");
        auto tags = object("tagsUi");
        QVERIFY(QMetaObject::invokeMethod(tags, "openStudy", Q_ARG(QVariant, "related"), Q_ARG(QVariant, QVariant())));
        QTRY_VERIFY(opened("relatedDrawer")); QTRY_VERIFY(item("studyChip:" + alpha));
        auto chip = item("studyChip:" + alpha);
        // Every chip's tooltip explains the clicks.
        auto tip = toolTipOf(chip); QVERIFY(tip);
        QVERIFY(tip->property("text").toString().contains("shift click: show only this tag"));
        QTest::mouseMove(window, center(chip) + QPoint(1, 1)); QTest::mouseMove(window, center(chip)); QTRY_VERIFY(tip->property("visible").toBool());
        // Mouse: the chip's menu shows only this tag, then all tags again.
        click(chip, Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
        QCOMPARE(menuIds(), QStringList({"soloChip", "allChips", "|", "editTags"}));
        QVERIFY(!menuEnabled("allChips"));
        chooseMenu("soloChip"); QCOMPARE(value(tags->property("onIds")).toStringList(), QStringList{alpha});
        click(chip, Qt::RightButton); chooseMenu("allChips"); QCOMPARE(value(tags->property("onIds")).toStringList().size(), 2);
        // Keyboard: the chip hints use the same words.
        chip->forceActiveFocus();
        QTRY_VERIFY(object("studyHints")->property("plainText").toString().contains("shift space show only this tag"));
        QVERIFY(object("studyHints")->property("plainText").toString().contains("0 show all tags"));
        key(Qt::Key_Space, Qt::ShiftModifier); QCOMPARE(value(tags->property("onIds")).toStringList(), QStringList{alpha});
        key(Qt::Key_0); QCOMPARE(value(tags->property("onIds")).toStringList().size(), 2);
        // The count line opens the passage search; its menu offers All versions and Search.
        auto count = item("studyTagCount"); QVERIFY(count && count->isVisible());
        QCOMPARE(item("studyTagCountMouse")->cursor().shape(), Qt::PointingHandCursor);
        click(count); QTRY_VERIFY(item("studySearch")->hasActiveFocus());
        key(Qt::Key_Escape); QTRY_VERIFY(!item("studySearch")->isVisible());
        QTRY_VERIFY(item("studyTagCount")->isVisible());
        click(item("studyTagCount"), Qt::RightButton); QTRY_VERIFY(menu()->property("opened").toBool());
        QCOMPARE(menuIds(), QStringList({"allVersions", "searchPassages"}));
        QVERIFY(!menuEnabled("allVersions")); // one version installed
        chooseMenu("searchPassages"); QTRY_VERIFY(item("studySearch")->hasActiveFocus());
        key(Qt::Key_Escape);
        // Edit tags from the chip menu opens the tag picker for the passage.
        click(item("studyChip:" + alpha), Qt::RightButton); chooseMenu("editTags"); QTRY_VERIFY(opened("tagsSurface"));
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
        QCOMPARE(menuIds(), QStringList({"open", "|", "editRange", "remove", "|", "copy", "|", "desk"}));
        QCOMPARE(value(menu()->property("layout")).toList().at(3).toMap()["text"].toString(), QString("Delete note…"));
        chooseMenu("open"); QTRY_VERIFY(opened("notesDrawer")); QCOMPARE(bible->selectionReference(), QString("John 3:16"));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("notesDrawer"));
        // Keyboard: the same menu by keys copies the passage.
        click(note, Qt::RightButton); QGuiApplication::clipboard()->clear(); menuKeyTo("copy");
        QTRY_VERIFY(QGuiApplication::clipboard()->text().endsWith(QStringLiteral("—John 3:16")));
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
            key(Qt::Key_Escape); key(Qt::Key_V, Qt::ShiftModifier); keys("e"); key(Qt::Key_Escape);
            QCOMPARE(bible->lastPrimarySelection(), QString("world"));
        }
    }
    void listsReserveTheScrollbar() {
        window->resize(1000, 520);
        for (int verse = 1; verse <= 24; ++verse) {
            bible->selectVerses(verse, verse); QVERIFY(bible->addToDesk()); QVERIFY(bible->toggleBookmark());
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
        key(Qt::Key_D, Qt::ShiftModifier); QTRY_VERIFY(opened("deskDrawer"));
        check("deskList", "deskEntry0", "deskScrollbar"); shot("scrollbar-desk");
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("deskDrawer"));
        key(Qt::Key_A, Qt::ShiftModifier); QTRY_VERIFY(opened("savedDrawer"));
        check("collectionList", "savedEntryJohn 3:1", "savedScrollbar");
        // Tags: the "here" label stays clear of the scrollbar.
        call("filterSaved", 3); QTRY_VERIFY(item("savedHereJohn 3:3"));
        auto here = item("savedHereJohn 3:3");
        QTRY_VERIFY(here->mapToItem(item("collectionList"), QPointF(here->width(), 0)).x() <= item("collectionList")->width() - item("savedScrollbar")->width() + 0.5);
        shot("scrollbar-annotations-tags");
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("savedDrawer"));
        // Mouse: wheel scrolling still works and the rows keep their width.
        call("readerFocus"); key(Qt::Key_D, Qt::ShiftModifier); QTRY_VERIFY(opened("deskDrawer"));
        wheel(item("deskList"), -480); QTRY_VERIFY(contentY("deskList") > 0 || item("deskList")->property("contentY").toReal() > 0);
        check("deskList", "deskEntry0", "deskScrollbar");
        QVERIFY(warnings.isEmpty());
    }
    // ── Phase 4: low

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
        QVERIFY(tip->property("text").toString().contains(QStringLiteral("Click to open it in Notes · w")));
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
        QTRY_VERIFY(!object("restoreDialog")->property("visible").toBool()); QTRY_VERIFY(readerFocused());
        // From an open panel, the panel gets it back.
        run("annotations"); QTRY_VERIFY(opened("savedDrawer"));
        QVERIFY(QMetaObject::invokeMethod(window, "runCommand", Q_ARG(QVariant, "restore")));
        QTRY_VERIFY(object("restoreDialog")->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(object("restoreDialog"), "reject"));
        QTRY_VERIFY(!readerFocused() && window->activeFocusItem() && (focusWithin("collectionList") || focusWithin("savedSearch") || focusName() == "collectionList"));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("savedDrawer"));
        // A note conflict puts the keyboard on its choices.
        navigate("John 3:16"); key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        window->setProperty("noteConflict", true);
        QTRY_VERIFY(item("noteRetry")->hasActiveFocus());
        shot("note-conflict-focus");
        key(Qt::Key_Return); QTRY_VERIFY(!window->property("noteConflict").toBool());
        // Mouse: Use saved text.
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
    void studyTagsEscapeClosesLikeAnnotations() {
        bible->selectVerses(7, 7); const QString alpha = bible->createAndAssignTag(bible->tagScope(), "Alpha");
        bible->selectVerses(3, 3); QVERIFY(bible->setTag(bible->tagScope(), alpha, true));
        navigate("John 3:3");
        auto tags = object("tagsUi");
        auto openTags = [&] {
            QVERIFY(QMetaObject::invokeMethod(tags, "openStudy", Q_ARG(QVariant, "related"), Q_ARG(QVariant, QVariant())));
            QTRY_VERIFY(opened("relatedDrawer")); QTRY_VERIFY(item("tagResults")->hasActiveFocus());
        };
        openTags();
        // Keyboard: / searches; esc leaves the field keeping the query; the next esc closes.
        key(Qt::Key_Slash); QTRY_VERIFY(item("studySearch")->hasActiveFocus()); type("zz");
        key(Qt::Key_Escape); QTRY_VERIFY(item("tagResults")->hasActiveFocus());
        QCOMPARE(tags->property("studyQuery").toString(), QString("zz"));
        QVERIFY(object("studyHints")->property("plainText").toString().contains("esc close"));
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("relatedDrawer")); QTRY_VERIFY(readerFocused());
        // Reopened, the query is gone.
        openTags(); QCOMPARE(tags->property("studyQuery").toString(), QString());
        // Mouse: the esc hint closes too.
        key(Qt::Key_Slash); type("zz"); key(Qt::Key_Return);
        clickHint("studyHintsPart:close"); QTRY_VERIFY(!shown("relatedDrawer"));
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
};

int main(int argc, char **argv) {
    QQuickStyle::setStyle("Basic"); QApplication app(argc, argv);
    A11yUiTest test; return QTest::qExec(&test, argc, argv);
}
#include "tst_a11yui.moc"
