#include <QtTest/QTest>
#include <QApplication>
#include <QClipboard>
#include <QCryptographicHash>
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
        QTest::mouseMove(window, center(copy)); QTRY_VERIFY(tint->isVisible());
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
        click(item("deskHintsPart:remove")); QCOMPARE(bible->desk().size(), 1);
        key(Qt::Key_Delete); QCOMPARE(bible->desk().size(), 0);
        // Nothing left to open: those parts stop being clickable.
        QTRY_VERIFY(!item("deskHintsPart:open")->property("clickable").toBool());
        QVERIFY(!item("deskHintsPart:clean")->property("clickable").toBool());
        click(item("deskHintsPart:close")); QTRY_VERIFY(!shown("deskDrawer"));
    }
    void everyPanelHintRowIsClickable() {
        // Palette: esc close and enter select.
        run("books"); QTRY_VERIFY(opened("passagePalette"));
        QVERIFY(!item("paletteHintsPart:choose")->property("clickable").toBool());
        click(item("paletteHintsPart:close")); QTRY_VERIFY(!shown("passagePalette"));
        run("books"); QTRY_VERIFY(opened("passagePalette")); type("john 3:16");
        click(item("paletteHintsPart:select")); QTRY_VERIFY(!shown("passagePalette"));
        QCOMPARE(bible->selectionReference(), QString("John 3:16"));
        // Help: a disabled (explanation) row cannot be selected by its hint.
        run("help"); QTRY_VERIFY(opened("helpDialog")); type("click");
        QTRY_VERIFY(!item("keybindingHintsPart:select")->property("clickable").toBool());
        click(item("keybindingHintsPart:close")); QTRY_VERIFY(!shown("helpDialog"));
        // Versions: cancel by its hint.
        run("switchVersion"); QTRY_VERIFY(opened("versionDialog"));
        QVERIFY(item("versionHintsPart:select")->property("clickable").toBool());
        click(item("versionHintsPart:cancel")); QTRY_VERIFY(!shown("versionDialog"));
        // Reading settings: increase / reset.
        run("settings"); QTRY_VERIFY(opened("settingsDialog"));
        const int size = bible->fontSize();
        click(item("fontSizeHintsPart:increase")); QCOMPARE(bible->fontSize(), size + 1);
        click(item("fontSizeHintsPart:reset")); QCOMPARE(bible->fontSize(), 18);
        click(item("lineHeightHintsPart:increase")); QCOMPARE(bible->lineHeight(), 1.6);
        click(item("lineHeightHintsPart:reset")); QCOMPARE(bible->lineHeight(), 1.5);
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("settingsDialog"));
        // Annotations: open the current row by its hint.
        QVERIFY(bible->navigate("John 3:9")); bible->selectVerses(9, 9); QVERIFY(bible->toggleBookmark());
        navigate("John 1:1");
        run("annotations"); QTRY_VERIFY(opened("savedDrawer"));
        QTRY_VERIFY(item("savedHintsPart:open")->property("clickable").toBool());
        click(item("savedHintsPart:open")); QTRY_VERIFY(!shown("savedDrawer"));
        QCOMPARE(bible->selectionReference(), QString("John 3:9"));
        // Confirmations: cancel by the hint, confirm with enter on the focused button.
        QVERIFY(bible->addToDesk()); run("desk"); QTRY_VERIFY(opened("deskDrawer"));
        key(Qt::Key_Delete, Qt::ShiftModifier); QTRY_VERIFY(opened("cleanDeskConfirmation"));
        click(item("cleanDeskHintsPart:cancel")); QTRY_VERIFY(!shown("cleanDeskConfirmation"));
        QCOMPARE(bible->desk().size(), 1);
        key(Qt::Key_Delete, Qt::ShiftModifier); QTRY_VERIFY(opened("cleanDeskConfirmation"));
        key(Qt::Key_Right); QVERIFY(item("cleanDeskConfirm")->hasActiveFocus());
        click(item("cleanDeskHintsPart:select")); QTRY_VERIFY(bible->desk().isEmpty());
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("deskDrawer"));
        // Study and the tag picker.
        run("related"); QTRY_VERIFY(opened("relatedDrawer"));
        QVERIFY(!item("studyHintsPart:tabs")->property("clickable").toBool());
        click(item("studyHintsPart:close")); QTRY_VERIFY(!shown("relatedDrawer"));
        run("tags"); QTRY_VERIFY(opened("tagsSurface")); type("Grace");
        click(item("tagsHintsPart:toggle / create")); QTRY_COMPARE(bible->tags().size(), 1);
        click(item("tagsHintsPart:close")); QTRY_VERIFY(!shown("tagsSurface"));
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
        key(Qt::Key_Menu); QTRY_VERIFY(menu()->property("opened").toBool());
        const QPoint there = versePoint(8);
        clickAt(there, Qt::RightButton);
        QTRY_VERIFY(menu()->property("opened").toBool());
        QCOMPARE(bible->selectedStart(), 8);
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
        key(Qt::Key_J); click(item("rangeEditHintsPart:swap")); QCOMPARE(object("rangeEditor")->property("endpoint").toString(), QString("start"));
        click(item("rangeEditHintsPart:save")); QTRY_VERIFY(!prop("rangeEditing").toBool());
        QCOMPARE(bible->collection().first().toMap()["end"].toInt(), 7);
        key(Qt::Key_E, Qt::ShiftModifier); QTRY_VERIFY(object("rangeEditor")->property("active").toBool());
        click(item("rangeEditHintsPart:cancel")); QTRY_VERIFY(!prop("rangeEditing").toBool());
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
        for (int i = 0; i < 30 && results->property("currentIndex").toInt() < 92; ++i) key(Qt::Key_PageDown);
        QCOMPARE(results->property("count").toInt(), 100);
        key(Qt::Key_PageDown); QTRY_COMPARE(results->property("count").toInt(), 167);
        key(Qt::Key_Escape); QTRY_VERIFY(!shown("passagePalette"));
        // Mouse: the "↓ more" hint loads the next page.
        run("books"); key(Qt::Key_Escape); QTRY_VERIFY(!shown("passagePalette"));
        search();
        click(item("paletteHintsPart:more")); QTRY_COMPARE(results->property("count").toInt(), 167);
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
        click(item("versionHintsPart:re-import")); QTRY_COMPARE(catalog->requests, QStringList{id});
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
};

int main(int argc, char **argv) {
    QQuickStyle::setStyle("Basic"); QApplication app(argc, argv);
    A11yUiTest test; return QTest::qExec(&test, argc, argv);
}
#include "tst_a11yui.moc"
