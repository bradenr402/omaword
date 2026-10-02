#include <QtTest/QTest>
#include <QApplication>
#include <QClipboard>
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
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QDateTime>
#include <functional>
#include <archive.h>
#include <archive_entry.h>
#include "biblebackend.h"
#include "blockdocument.h"
#include "desktop.h"
#include "soundeffects.h"
#include "versioncatalog.h"

class HistoryUiTest : public QObject
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
    static bool psalm(const QString &path) {
        auto zip = archive_write_new(); archive_write_set_format_zip(zip);
        if (archive_write_open_filename(zip, QFile::encodeName(path).constData()) != ARCHIVE_OK) { archive_write_free(zip); return false; }
        const QByteArray data = QString("\\id PSA Acrostic fixture\n\\h Psalms\n\\toc1 Psalms\n\\toc2 Psalms\n\\c 119\n\\s \u05d0 Aleph\n\\p\n"
            "\\v 1 \u05d0\u05e9\u05e8\u05d9 Blessed are those whose way is blameless.\n"
            "\\v 2 Blessed are those who keep his testimonies.\n"
            "\\s \u05d1 Beth\n\\p\n\\v 3 \u05d1 Beth begins with Hebrew too.\n").toUtf8();
        auto entry = archive_entry_new();
        archive_entry_set_pathname(entry, "19-PSAengpsa.usfm"); archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0600); archive_entry_set_size(entry, data.size());
        bool ok = archive_write_header(zip, entry) == ARCHIVE_OK && archive_write_data(zip, data.constData(), data.size()) == data.size();
        archive_entry_free(entry); ok = archive_write_close(zip) == ARCHIVE_OK && ok; archive_write_free(zip); return ok;
    }
    // A Hebrew-first paragraph stays LTR: its Hebrew opens at the left, and the
    // following Latin word lies to its right.
    void expectLeftToRight(QObject *body, const QString &hebrew, const QString &latin) {
        auto doc = document(body); QVERIFY(doc);
        const auto text = doc->toPlainText();
        const int h = text.indexOf(hebrew), l = text.indexOf(latin, h);
        QVERIFY2(h >= 0 && l > h, qPrintable(text));
        const auto block = doc->findBlock(h);
        QCOMPARE(block.textDirection(), Qt::LeftToRight);
        QVERIFY(!block.layout()->textOption().textDirection() || block.layout()->textOption().textDirection() == Qt::LeftToRight);
        QVERIFY2(caret(body, l).x() > caret(body, h).x(), qPrintable(QString("%1 %2").arg(caret(body, l).x()).arg(caret(body, h).x())));
        QCOMPARE(body->property("effectiveHorizontalAlignment").toInt(), int(Qt::AlignLeft));
        // Left-aligned, not merely LTR: the paragraph's first line starts at the
        // left edge even when it is shorter than the available width.
        const auto line = block.layout()->lineAt(0);
        QVERIFY(line.isValid());
        QVERIFY2(line.naturalTextRect().left() < 2, qPrintable(QString("line starts at %1 of %2")
            .arg(line.naturalTextRect().left()).arg(doc->textWidth())));
    }
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

    // History helpers.
    QVariantList list(const char *name) const { return prop(name).toList(); }
    QString here() const { return bible->selectionReference().isEmpty() ? bible->passage() : bible->selectionReference(); }
    void jumpTo(const QString &reference) {
        QVERIFY(QMetaObject::invokeMethod(window, "jump", Q_ARG(QVariant, reference))); QCoreApplication::processEvents();
        QCOMPARE(bible->selectionReference(), reference);
    }
    int stackIndex() const { return prop("jumpIndex").toInt(); }
    QStringList stack() const {
        QStringList out;
        for (const auto &v : prop("jumpStack").toList()) {
            const auto m = v.toMap();
            out << QString("%1:%2:%3").arg(m["chapter"].toInt()).arg(m["start"].toInt()).arg(m["versionId"].toString());
        }
        return out;
    }
    bool historyOpen() const { return object("historyDrawer")->property("opened").toBool(); }
    void openHistory() { key(Qt::Key_H, Qt::ShiftModifier); QTRY_VERIFY(historyOpen()); QTRY_VERIFY(item("historyList")->hasActiveFocus()); }
    QStringList historyRefs() const {
        QStringList out; for (const auto &row : bible->history()) out << row.toMap()["reference"].toString(); return out;
    }
    QVariantList paletteRows() {
        QVariant entries; QMetaObject::invokeMethod(window, "paletteEntries", Q_RETURN_ARG(QVariant, entries));
        return value(entries).toList();
    }
    void record(const QString &reference) { QVERIFY(bible->navigate(reference)); QVERIFY(bible->recordHistory()); }
    void run(const QString &command) { QVERIFY(QMetaObject::invokeMethod(window, "runCommand", Q_ARG(QVariant, command))); QCoreApplication::processEvents(); }
    void setVisited(int row, const QDateTime &when) {
        const auto id = bible->history()[row].toMap()["id"].toInt();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "history-edit");
            db.setDatabaseName(bible->dataDirectory() + "/personal.sqlite"); QVERIFY(db.open());
            QSqlQuery q(db); q.prepare("UPDATE history SET visited=? WHERE id=?");
            q.addBindValue(when.toUTC().toString(Qt::ISODateWithMs)); q.addBindValue(id); QVERIFY(q.exec());
            db.close();
        }
        QSqlDatabase::removeDatabase("history-edit");
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
        window->requestActivate(); QVERIFY(QTest::qWaitForWindowExposed(window)); QTRY_VERIFY(window->isActive());
        navigate("John 1:1");
        // Tests opt into the dwell; otherwise a slow step could record History.
        window->setProperty("historyDwellMs", 3600000);
    }
    void cleanup() {
        engine.reset(); window = nullptr; catalog.reset(); sounds.reset(); desktop.reset(); bible.reset(); session.reset();
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
    }


    void backForwardWithCountsAndBrowserSemantics() {
        QVERIFY(stack().isEmpty());
        jumpTo("John 1:10"); jumpTo("John 1:20"); jumpTo("John 2:5");
        QCOMPARE(stack(), QStringList({"1:1:engnet", "1:10:engnet", "1:20:engnet", "2:5:engnet"}));
        QCOMPARE(stackIndex(), 3);
        key(Qt::Key_BracketLeft); QCOMPARE(here(), QString("John 1:20")); QCOMPARE(stackIndex(), 2);
        type("2"); key(Qt::Key_BracketLeft); QCOMPARE(here(), QString("John 1:1")); QCOMPARE(stackIndex(), 0);
        key(Qt::Key_BracketLeft); QCOMPARE(here(), QString("John 1:1")); QVERIFY(toast().contains("No earlier jump"));
        key(Qt::Key_BracketRight); QCOMPARE(here(), QString("John 1:10"));
        type("2"); key(Qt::Key_BracketRight); QCOMPARE(here(), QString("John 2:5")); QCOMPARE(stackIndex(), 3);
        // A new jump after going back drops the forward entries.
        key(Qt::Key_BracketLeft); key(Qt::Key_BracketLeft); QCOMPARE(here(), QString("John 1:10"));
        jumpTo("John 2:30");
        QCOMPARE(stack(), QStringList({"1:1:engnet", "1:10:engnet", "2:30:engnet"}));
        key(Qt::Key_BracketRight); QCOMPARE(here(), QString("John 2:30")); QVERIFY(toast().contains("No later jump"));
        // Leaving a stack entry with j/k updates it: back returns where you were.
        key(Qt::Key_BracketLeft); QCOMPARE(here(), QString("John 1:10"));
        key(Qt::Key_J); key(Qt::Key_J); QCOMPARE(here(), QString("John 1:12"));
        key(Qt::Key_BracketRight); key(Qt::Key_BracketLeft); QCOMPARE(here(), QString("John 1:12"));
        // Restoring brings back the scroll position as well as the range.
        navigate("John 2"); bible->clearSelection(); QTRY_VERIFY(!prop("restoringPosition").toBool());
        jumpTo("John 1:2");
        QVERIFY(QMetaObject::invokeMethod(window, "openChapter", Q_ARG(QVariant, 43), Q_ARG(QVariant, 2)));
        auto reading = item("reading"); QTRY_VERIFY(!prop("restoringPosition").toBool());
        const double travel = reading->property("contentHeight").toDouble() - reading->height();
        QVERIFY(travel > 400);
        reading->setProperty("contentY", travel * 0.6); call("savePosition");
        jumpTo("John 1:30");
        key(Qt::Key_BracketLeft); QTRY_VERIFY(!prop("restoringPosition").toBool());
        QCOMPARE(here(), QString("John 2")); QCOMPARE(bible->selectedStart(), 0);
        QVERIFY(qAbs(reading->property("contentY").toDouble() - travel * 0.6) < 2);
    }
    void swapTogglesAndWorksWhenDisabled() {
        jumpTo("John 1:5"); jumpTo("John 2:5");
        key(Qt::Key_Minus); QCOMPARE(here(), QString("John 1:5"));
        key(Qt::Key_Minus); QCOMPARE(here(), QString("John 2:5"));
        // Counts do not apply to -.
        type("3"); key(Qt::Key_Minus); QCOMPARE(here(), QString("John 1:5")); QCOMPARE(prop("verseCount").toString(), QString());
        QVERIFY(bible->setHistoryEnabled(false));
        key(Qt::Key_Minus); QCOMPARE(here(), QString("John 2:5"));
        key(Qt::Key_BracketLeft); QCOMPARE(here(), QString("John 1:5"));
        key(Qt::Key_BracketRight); QCOMPARE(here(), QString("John 2:5"));
        jumpTo("John 1:40"); QCOMPARE(stackIndex(), stack().size() - 1);
        QVERIFY(bible->history().isEmpty());
    }
    void stepsAreNotJumpsButGotoAndVersionSwitchAre() {
        key(Qt::Key_J); key(Qt::Key_K); key(Qt::Key_J); key(Qt::Key_L); key(Qt::Key_H); key(Qt::Key_Down);
        QVERIFY(stack().isEmpty());
        key(Qt::Key_BracketLeft); QVERIFY(toast().contains("No earlier jump"));
        type("20"); key(Qt::Key_G); QCOMPARE(here(), QString("John 1:20")); QCOMPARE(stack().size(), 2);
        key(Qt::Key_G, Qt::ShiftModifier); QCOMPARE(here(), QString("John 1:65")); QCOMPARE(stack().size(), 3);
        key(Qt::Key_G); QCOMPARE(here(), QString("John 1:1")); QCOMPARE(stack().size(), 4);
        install(second); QVERIFY(bible->switchVersion(secondId())); navigate("John 1:7");
        const auto net = bible->versionId(), kjv = secondId();
        const int before = stack().size();
        QVERIFY(QMetaObject::invokeMethod(window, "switchVersion", Q_ARG(QVariant, kjv))); QCoreApplication::processEvents();
        QCOMPARE(bible->versionId(), kjv); QCOMPARE(stack().size(), before + 1);
        key(Qt::Key_BracketLeft); QCOMPARE(bible->versionId(), net); QCOMPARE(here(), QString("John 1:7"));
        key(Qt::Key_Minus); QCOMPARE(bible->versionId(), kjv);
        // Desk and search opens are jumps too.
        QVERIFY(bible->navigate("John 2:3")); QVERIFY(bible->addToDesk()); navigate("John 1:1");
        const int count = stack().size();
        QVERIFY(QMetaObject::invokeMethod(window, "openDeskItem", Q_ARG(QVariant, bible->desk().first().toMap()["id"].toInt())));
        QCOMPARE(here(), QString("John 2:3")); QCOMPARE(stack().size(), count + 1);
    }
    void dwellThresholdRecordsOnlyKeptPassages() {
        window->setProperty("historyDwellMs", 400);
        QVERIFY(bible->clearHistory()); QVERIFY(bible->history().isEmpty());
        jumpTo("John 1:10"); QTest::qWait(150);
        jumpTo("John 1:20"); QTest::qWait(150);
        jumpTo("John 1:30");
        QTRY_COMPARE(historyRefs(), QStringList({"John 1:30"}));
        QTest::qWait(300);
        // Verse steps keep the passage; nothing new is recorded.
        key(Qt::Key_J); QTest::qWait(700); QCOMPARE(historyRefs(), QStringList({"John 1:30"}));
        // A chapter flip is a new passage once kept.
        key(Qt::Key_L); QTRY_COMPARE(bible->history().size(), 2);
        QVERIFY(historyRefs().first().startsWith("John 2"));
        // Revisits move to the top without duplicates.
        jumpTo("John 1:30"); QTRY_COMPARE(historyRefs().first(), QString("John 1:30")); QCOMPARE(bible->history().size(), 2);
        // Default is five seconds.
        window->setProperty("historyDwellMs", 5000);
        QCOMPARE(object("historyDwellTimer")->property("interval").toInt(), 5000);
        // Disabled: nothing is recorded.
        QVERIFY(bible->setHistoryEnabled(false)); window->setProperty("historyDwellMs", 200);
        jumpTo("John 1:50"); QTest::qWait(500); QVERIFY(bible->history().isEmpty());
    }
    void panelGroupsRowsAndListKeys() {
        record("John 1:3"); record("John 2:4-6"); record("John 1:9"); QVERIFY(bible->navigate("John 2")); QVERIFY(bible->recordHistory());
        navigate("John 1:1");
        // Rows: John 2 (today), John 1:9 (yesterday), John 2:4–6 and John 1:3 (earlier).
        setVisited(1, QDateTime::currentDateTime().addDays(-1));
        setVisited(2, QDateTime::currentDateTime().addDays(-5)); setVisited(3, QDateTime::currentDateTime().addDays(-6));
        QVERIFY(bible->recordQuery("refresh"));
        openHistory();
        QVERIFY(item("historyControl")->property("selected").toBool());
        QCOMPARE(object("historyTitle")->property("text").toString(), QString("History"));
        QCOMPARE(object("historyCount")->property("text").toString(), QString("4 passages"));
        QCOMPARE(object("historyHeader0")->property("text").toString(), QString("John 2 · ") + bible->versionAbbreviation());
        QVERIFY(item("historyGroup0")->isVisible()); QCOMPARE(object("historyGroup0")->property("text").toString(), QString("Today"));
        QCOMPARE(object("historyGroup1")->property("text").toString(), QString("Yesterday")); QVERIFY(item("historyGroup1")->isVisible());
        QCOMPARE(object("historyGroup2")->property("text").toString(), QString("Earlier")); QVERIFY(item("historyGroup2")->isVisible());
        QVERIFY(!item("historyGroup3")->isVisible());
        QVERIFY(!object("historyTime0")->property("text").toString().isEmpty());
        QVERIFY(object("historyExcerpt2")->property("text").toString().contains("Common original fixture 2:4"));
        QVERIFY(object("historyHints")->property("text").toString().contains("shift&nbsp;delete"));
        QCOMPARE(object("historyList")->property("currentIndex").toInt(), 0);
        key(Qt::Key_J); key(Qt::Key_Down); QCOMPARE(object("historyList")->property("currentIndex").toInt(), 2);
        key(Qt::Key_K); QCOMPARE(object("historyList")->property("currentIndex").toInt(), 1);
        QGuiApplication::clipboard()->clear(); key(Qt::Key_Y);
        QTRY_VERIFY(QGuiApplication::clipboard()->text().contains("John 1:9"));
        key(Qt::Key_Y, Qt::ShiftModifier); QTRY_VERIFY(!QGuiApplication::clipboard()->text().contains("John 1:9"));
        QVERIFY(QGuiApplication::clipboard()->text().contains("Common original fixture 1:9"));
        key(Qt::Key_D); QCOMPARE(bible->desk().size(), 1); QCOMPARE(bible->desk().first().toMap()["reference"].toString(), QString("John 1:9"));
        key(Qt::Key_D); QVERIFY(bible->desk().isEmpty());
        // A chapter row adds the whole chapter.
        key(Qt::Key_K); key(Qt::Key_D); QCOMPARE(bible->desk().first().toMap()["reference"].toString(), QString("John 2:1–65"));
        key(Qt::Key_J); key(Qt::Key_Delete); QCOMPARE(historyRefs(), QStringList({"John 2", "John 2:4–6", "John 1:3"}));
        QCOMPARE(object("historyList")->property("currentIndex").toInt(), 1);
        key(Qt::Key_Backspace); QCOMPARE(historyRefs(), QStringList({"John 2", "John 1:3"}));
        // Context menu: Open, Copy | Add to Desk | Remove from History.
        QVERIFY(QMetaObject::invokeMethod(window, "openHistoryMenu", Q_ARG(QVariant, 1), Q_ARG(QVariant, 200), Q_ARG(QVariant, 200)));
        QTRY_VERIFY(object("contextMenu")->property("visible").toBool());
        QCOMPARE(object("contextMenu")->property("target").toString(), QString("history"));
        for (auto name : {"contextMenuItem:open", "contextMenuItem:copy", "contextMenuItem:desk", "contextMenuItem:remove"}) QVERIFY2(object(name), name);
        QVERIFY(QMetaObject::invokeMethod(object("contextMenu"), "triggerId", Q_ARG(QVariant, QString("remove"))));
        QTRY_COMPARE(historyRefs(), QStringList({"John 2"}));
        QTRY_VERIFY(!object("contextMenu")->property("visible").toBool());
        QTRY_VERIFY(item("historyList")->hasActiveFocus());
        record("John 1:3"); QTRY_COMPARE(object("historyList")->property("count").toInt(), 2);
        QVERIFY(bible->navigate("John 1:1"));
        // Enter opens and closes the panel; the current stack position is marked.
        key(Qt::Key_K);
        key(Qt::Key_Return);
        QTRY_VERIFY(!object("historyDrawer")->property("visible").toBool());
        QTRY_VERIFY(item("readerSurface")->hasActiveFocus());
        QCOMPARE(here(), QString("John 1:3")); QCOMPARE(stack().last(), QString("1:3:engnet"));
        openHistory();
        QTRY_VERIFY(item("historyCurrent0")->isVisible()); QVERIFY(!item("historyCurrent1")->isVisible());
        // shift delete asks first; esc keeps History.
        key(Qt::Key_Delete, Qt::ShiftModifier); QTRY_VERIFY(object("clearHistoryConfirmation")->property("opened").toBool());
        QVERIFY(object("clearHistoryExplanation")->property("text").toString().contains("2 passages"));
        key(Qt::Key_Escape); QTRY_VERIFY(!object("clearHistoryConfirmation")->property("visible").toBool());
        QCOMPARE(bible->history().size(), 2); QVERIFY(historyOpen());
        QTRY_VERIFY(item("historyList")->hasActiveFocus());
        key(Qt::Key_Backspace, Qt::ShiftModifier); QTRY_VERIFY(object("clearHistoryConfirmation")->property("opened").toBool());
        key(Qt::Key_Right); key(Qt::Key_Return);
        QTRY_VERIFY(bible->history().isEmpty()); QVERIFY(bible->recentQueries().isEmpty());
        QVERIFY(toast().contains("Cleared History"));
        QTRY_VERIFY(item("historyEmpty")->isVisible());
        key(Qt::Key_Escape); QTRY_VERIFY(!object("historyDrawer")->property("visible").toBool());
    }
    // Text size scales every UI text proportionally (the reader by size alone),
    // and both size and line height keys work from panels, modals, and inputs.
    void textSizeAndLineHeightWorkEverywhere() {
        record("John 1:3"); navigate("John 1:2"); QVERIFY(bible->toggleDesk()); navigate("John 1:1");
        QCOMPARE(prop("scale").toDouble(), 1.0);
        const double readerFont = item("verseBody1")->property("font").value<QFont>().pixelSize();
        QCOMPARE(readerFont, 18.0);
        key(Qt::Key_D, Qt::ShiftModifier); QTRY_VERIFY(object("deskDrawer")->property("opened").toBool());
        key(Qt::Key_Equal, Qt::ControlModifier); QCOMPARE(bible->fontSize(), 19);
        QVERIFY(qAbs(prop("scale").toDouble() - 19.0 / 18) < 1e-9);
        QTRY_COMPARE(object("deskText0")->property("font").value<QFont>().pixelSize(), qRound(13 * 19.0 / 18));
        QCOMPARE(item("verseBody1")->property("font").value<QFont>().pixelSize(), 19);
        QCOMPARE(object("toast")->property("font").value<QFont>().pixelSize(), qRound(13 * 19.0 / 18));
        key(Qt::Key_Equal, Qt::ControlModifier | Qt::AltModifier); QCOMPARE(bible->lineHeight(), 1.6);
        QVERIFY(qAbs(object("deskText0")->property("lineHeight").toDouble() - 1.6 / 1.5 * 1.25) < 1e-6);
        QVERIFY(object("deskDrawer")->property("opened").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!object("deskDrawer")->property("visible").toBool());
        openHistory();
        key(Qt::Key_Minus, Qt::ControlModifier | Qt::AltModifier); QCOMPARE(bible->lineHeight(), 1.5);
        QVERIFY(qAbs(object("historyExcerpt0")->property("lineHeight").toDouble() - 1.25) < 1e-6);
        key(Qt::Key_Escape); QTRY_VERIFY(!object("historyDrawer")->property("visible").toBool());
        // In the palette's text field: the field keeps its text.
        call("readerFocus"); key(Qt::Key_K, Qt::ControlModifier); QTRY_VERIFY(object("passagePalette")->property("opened").toBool());
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus()); type("jo");
        const auto typed = object("referenceInput")->property("text").toString();
        key(Qt::Key_Minus, Qt::ControlModifier); QCOMPARE(bible->fontSize(), 18);
        key(Qt::Key_Minus, Qt::ControlModifier); QCOMPARE(bible->fontSize(), 17);
        key(Qt::Key_Equal, Qt::ControlModifier | Qt::AltModifier); QCOMPARE(bible->lineHeight(), 1.6);
        QCOMPARE(object("referenceInput")->property("text").toString(), typed);
        QVERIFY(object("passagePalette")->property("visible").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!object("passagePalette")->property("visible").toBool());
        // In a modal: keybindings, with its search field focused.
        call("readerFocus"); run("help"); QTRY_VERIFY(object("helpDialog")->property("opened").toBool());
        key(Qt::Key_0, Qt::ControlModifier); QCOMPARE(bible->fontSize(), 18);
        key(Qt::Key_0, Qt::ControlModifier | Qt::AltModifier); QCOMPARE(bible->lineHeight(), 1.5);
        QVERIFY(object("helpDialog")->property("visible").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!object("helpDialog")->property("visible").toBool());
        // Limits stay 8–40.
        bible->setFontSize(40); call("readerFocus"); key(Qt::Key_Equal, Qt::ControlModifier); QCOMPARE(bible->fontSize(), 40);
        bible->setFontSize(8); key(Qt::Key_Minus, Qt::ControlModifier); QCOMPARE(bible->fontSize(), 8);
        QVERIFY(qAbs(prop("scale").toDouble() - 8.0 / 18) < 1e-9);
        QCOMPARE(item("verseBody1")->property("font").value<QFont>().pixelSize(), 8);
    }
    // Related: j/k and the arrows walk the source tags and the results as one
    // list, from wherever the panel's keyboard focus is.
    void relatedListKeys() {
        navigate("John 1:2"); const auto alpha = bible->createAndAssignTag(bible->tagScope(), "Alpha");
        const auto beta = bible->createAndAssignTag(bible->tagScope(), "Beta");
        for (auto r : {"John 1:5", "John 1:8", "John 1:12", "John 1:20", "John 2:3"}) { navigate(r); QVERIFY(bible->setTag(bible->tagScope(), alpha, true)); }
        navigate("John 1:2");
        auto results = object("tagResults"), tags = object("tagList");
        const auto openRelated = [&]() {
            call("readerFocus"); key(Qt::Key_L, Qt::ShiftModifier); QTRY_VERIFY(object("relatedDrawer")->property("opened").toBool());
            QTRY_COMPARE(results->property("count").toInt(), 5); QTRY_VERIFY(item("tagResults")->hasActiveFocus());
        };
        openRelated();
        QCOMPARE(tags->property("count").toInt(), 2);
        QCOMPARE(results->property("currentIndex").toInt(), 0);
        key(Qt::Key_J); QCOMPARE(results->property("currentIndex").toInt(), 1);
        key(Qt::Key_Down); QCOMPARE(results->property("currentIndex").toInt(), 2);
        key(Qt::Key_K); QCOMPARE(results->property("currentIndex").toInt(), 1);
        key(Qt::Key_Up); QCOMPARE(results->property("currentIndex").toInt(), 0);
        for (int i = 0; i < 8; ++i) key(Qt::Key_J);
        QCOMPARE(results->property("currentIndex").toInt(), 4); QVERIFY(item("tagResults")->hasActiveFocus());
        for (int i = 0; i < 4; ++i) key(Qt::Key_K);
        QCOMPARE(results->property("currentIndex").toInt(), 0);
        // Above the first result: the source tags, bottom up, then the search field.
        key(Qt::Key_K); QVERIFY(item("tagList")->hasActiveFocus()); QCOMPARE(tags->property("currentIndex").toInt(), 1);
        key(Qt::Key_Up); QCOMPARE(tags->property("currentIndex").toInt(), 0); QVERIFY(item("tagList")->hasActiveFocus());
        key(Qt::Key_J); QCOMPARE(tags->property("currentIndex").toInt(), 1);
        key(Qt::Key_Down); QVERIFY(item("tagResults")->hasActiveFocus()); QCOMPARE(results->property("currentIndex").toInt(), 0);
        key(Qt::Key_K); key(Qt::Key_K); key(Qt::Key_K); QTRY_VERIFY(item("tagSearch")->hasActiveFocus());
        // Typing owns j and k in the search field.
        type("jk"); QCOMPARE(item("tagSearch")->property("text").toString(), QString("jk"));
        key(Qt::Key_Escape); QTRY_VERIFY(item("tagResults")->hasActiveFocus());
        key(Qt::Key_Escape); QTRY_VERIFY(!object("relatedDrawer")->property("visible").toBool());
        // A clicked tag holds focus; j/k still move (the original failure).
        openRelated();
        QTest::mouseClick(window, Qt::LeftButton, {}, item("tag:" + alpha)->mapToScene(QPointF(10, 10)).toPoint());
        QTRY_VERIFY(item("tagList")->hasActiveFocus());
        const int from = tags->property("currentIndex").toInt();
        key(Qt::Key_J);
        QVERIFY(tags->property("currentIndex").toInt() != from || item("tagResults")->hasActiveFocus());
        key(Qt::Key_Escape); QTRY_VERIFY(!object("relatedDrawer")->property("visible").toBool());
        Q_UNUSED(beta)
    }
    // g, shift G, ctrl d/u (5), ctrl f/b and pgdn/pgup (10) in every list panel;
    // never while typing in a panel's search field.
    void panelListMotions() {
        const auto expectMotions = [&](QObject *list, const QString &panel) {
            const auto at = [&]() { return list->property("currentIndex").toInt(); };
            const int last = list->property("count").toInt() - 1;
            QVERIFY2(last >= 20, qPrintable(panel));
            key(Qt::Key_G, Qt::ShiftModifier); QVERIFY2(at() == last, qPrintable(panel));
            key(Qt::Key_G); QVERIFY2(at() == 0, qPrintable(panel));
            key(Qt::Key_D, Qt::ControlModifier); QVERIFY2(at() == 5, qPrintable(panel));
            key(Qt::Key_F, Qt::ControlModifier); QVERIFY2(at() == 15, qPrintable(panel));
            key(Qt::Key_PageDown); QVERIFY2(at() == qMin(25, last), qPrintable(panel));
            key(Qt::Key_PageDown); key(Qt::Key_PageDown); QVERIFY2(at() == last, qPrintable(panel));
            key(Qt::Key_G); key(Qt::Key_PageDown); QVERIFY2(at() == 10, qPrintable(panel));
            key(Qt::Key_U, Qt::ControlModifier); QVERIFY2(at() == 5, qPrintable(panel));
            key(Qt::Key_B, Qt::ControlModifier); QVERIFY2(at() == 0, qPrintable(panel));
            key(Qt::Key_D, Qt::ControlModifier); key(Qt::Key_D, Qt::ControlModifier); key(Qt::Key_PageUp); QVERIFY2(at() == 0, qPrintable(panel));
            key(Qt::Key_G, Qt::ShiftModifier); key(Qt::Key_PageUp); QVERIFY2(at() == last - 10, qPrintable(panel));
        };
        QStringList refs;
        for (int v = 1; v <= 24; ++v) refs << QString("John 1:%1").arg(v * 2);
        navigate("John 2:1"); const auto tag = bible->createAndAssignTag(bible->tagScope(), "Alpha");
        for (const auto &r : refs) {
            navigate(r); QVERIFY(bible->toggleBookmark()); QVERIFY(bible->toggleDesk()); QVERIFY(bible->recordHistory());
            QVERIFY(bible->setTag(bible->tagScope(), tag, true));
        }
        navigate("John 2:1");
        // Desk.
        key(Qt::Key_D, Qt::ShiftModifier); QTRY_VERIFY(object("deskDrawer")->property("opened").toBool()); QTRY_VERIFY(item("deskList")->hasActiveFocus());
        expectMotions(object("deskList"), "desk");
        key(Qt::Key_Escape); QTRY_VERIFY(!object("deskDrawer")->property("visible").toBool());
        // History.
        call("readerFocus"); openHistory(); expectMotions(object("historyList"), "history");
        key(Qt::Key_Escape); QTRY_VERIFY(!object("historyDrawer")->property("visible").toBool());
        // Annotations (Bookmarks), then not while typing in its search field.
        call("readerFocus"); key(Qt::Key_A, Qt::ShiftModifier); QTRY_VERIFY(object("savedDrawer")->property("opened").toBool());
        QTRY_VERIFY(item("collectionList")->hasActiveFocus());
        auto saved = object("collectionList"); expectMotions(saved, "annotations");
        key(Qt::Key_G); key(Qt::Key_Slash); QTRY_VERIFY(item("savedSearch")->hasActiveFocus());
        key(Qt::Key_G, Qt::ShiftModifier); key(Qt::Key_PageDown); key(Qt::Key_D, Qt::ControlModifier);
        QCOMPARE(object("savedSearch")->property("text").toString().toLower(), QString("g")); // Typed, not a motion.
        QVERIFY(item("savedSearch")->hasActiveFocus());
        object("savedSearch")->setProperty("text", QString()); key(Qt::Key_Escape); QTRY_VERIFY(!item("savedSearch")->hasActiveFocus());
        // Annotations · Tags tab: g and shift G reach the first and last range.
        key(Qt::Key_H); QTRY_COMPARE(prop("collectionTab").toInt(), 3);
        QTRY_VERIFY(item("summaryMotionKeys")->property("active").toBool());
        key(Qt::Key_G, Qt::ShiftModifier);
        auto lastFocus = window->activeFocusItem(); QVERIFY(lastFocus); const auto lastName = lastFocus->objectName();
        key(Qt::Key_G); auto firstFocus = window->activeFocusItem(); QVERIFY(firstFocus && firstFocus != lastFocus);
        key(Qt::Key_D, Qt::ControlModifier); QVERIFY(window->activeFocusItem() != firstFocus);
        key(Qt::Key_U, Qt::ControlModifier); QCOMPARE(window->activeFocusItem(), firstFocus);
        key(Qt::Key_PageDown); key(Qt::Key_PageDown); key(Qt::Key_PageDown); key(Qt::Key_PageDown); key(Qt::Key_PageDown);
        QCOMPARE(window->activeFocusItem()->objectName(), lastName);
        key(Qt::Key_Escape); QTRY_VERIFY(!object("savedDrawer")->property("visible").toBool());
        // Related: the results.
        call("readerFocus"); key(Qt::Key_L, Qt::ShiftModifier); QTRY_VERIFY(object("relatedDrawer")->property("opened").toBool());
        QTRY_COMPARE(object("tagResults")->property("count").toInt(), 24); QTRY_VERIFY(item("tagResults")->hasActiveFocus());
        expectMotions(object("tagResults"), "related");
        key(Qt::Key_Slash); QTRY_VERIFY(item("tagSearch")->hasActiveFocus());
        key(Qt::Key_G); QCOMPARE(object("tagSearch")->property("text").toString(), QString("g"));
        key(Qt::Key_Escape); QTRY_VERIFY(item("tagResults")->hasActiveFocus());
        key(Qt::Key_Escape); QTRY_VERIFY(!object("relatedDrawer")->property("visible").toBool());
        // The reader keeps its own meanings once the panels are closed.
        call("readerFocus"); navigate("John 1:20"); key(Qt::Key_D, Qt::ControlModifier); QCOMPARE(bible->cursorVerse(), 25);
    }
    void panelSwitchingWithOtherPanels() {
        record("John 1:3"); navigate("John 1:1");
        key(Qt::Key_D, Qt::ShiftModifier); QTRY_VERIFY(object("deskDrawer")->property("opened").toBool());
        key(Qt::Key_H, Qt::ShiftModifier);
        QTRY_VERIFY(historyOpen()); QTRY_VERIFY(!object("deskDrawer")->property("visible").toBool());
        QTRY_VERIFY(item("historyList")->hasActiveFocus());
        key(Qt::Key_J); key(Qt::Key_H, Qt::ShiftModifier); QTest::qWait(100); QVERIFY(historyOpen()); // Same key: nothing.
        key(Qt::Key_A, Qt::ShiftModifier);
        QTRY_VERIFY(object("savedDrawer")->property("opened").toBool()); QTRY_VERIFY(!object("historyDrawer")->property("visible").toBool());
        key(Qt::Key_H, Qt::ShiftModifier); QTRY_VERIFY(historyOpen()); QTRY_VERIFY(!object("savedDrawer")->property("visible").toBool());
        key(Qt::Key_L, Qt::ShiftModifier); QTRY_VERIFY(prop("sidePanel").toString() == "related");
        QTRY_VERIFY(!object("historyDrawer")->property("visible").toBool());
        key(Qt::Key_H, Qt::ShiftModifier); QTRY_VERIFY(historyOpen());
        key(Qt::Key_W); QTRY_VERIFY(prop("notesOpen").toBool()); QTRY_VERIFY(!object("historyDrawer")->property("visible").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("notesOpen").toBool());
        // One esc closes the History panel.
        call("readerFocus"); openHistory(); key(Qt::Key_Escape); QTRY_VERIFY(!object("historyDrawer")->property("visible").toBool());
        QTRY_VERIFY(item("readerSurface")->hasActiveFocus());
    }
    void footerButtonCommandsAndDisabledToast() {
        window->resize(1200, 820); QTest::qWait(100);
        auto x = [this](const char *name) { return item(name)->mapToScene(QPointF()).x(); };
        QVERIFY(item("historyControl")->isVisible());
        QVERIFY(x("relatedControl") < x("historyControl")); QVERIFY(x("historyControl") < x("deskControl"));
        QVERIFY(item("historyControl")->property("tip").toString().contains("shift H"));
        QMetaObject::invokeMethod(item("historyControl"), "clicked"); QTRY_VERIFY(historyOpen());
        key(Qt::Key_Escape); QTRY_VERIFY(!object("historyDrawer")->property("visible").toBool());
        QCOMPARE(commandTitle("history"), QString("History"));
        QCOMPARE(commandTitle("clear history"), QString("Clear History"));
        QCOMPARE(commandTitle("able history"), QString("Disable History"));
        record("John 1:3"); QVERIFY(bible->recordQuery("light")); navigate("John 1:1");
        run("clearHistory"); QTRY_VERIFY(object("clearHistoryConfirmation")->property("opened").toBool());
        QMetaObject::invokeMethod(object("clearHistoryConfirm"), "clicked");
        QTRY_VERIFY(bible->history().isEmpty()); QVERIFY(bible->recentQueries().isEmpty());
        record("John 1:3"); navigate("John 1:1");
        run("toggleHistory");
        QVERIFY(!bible->historyEnabled()); QVERIFY(bible->history().isEmpty());
        QCOMPARE(commandTitle("able history"), QString("Enable History"));
        QTRY_VERIFY(!item("historyControl")->isVisible());
        QCOMPARE(window->property("footerRightCount").toInt(), 4);
        key(Qt::Key_H, Qt::ShiftModifier); QTest::qWait(100);
        QVERIFY(!object("historyDrawer")->property("visible").toBool());
        QCOMPARE(toast(), QString("History is disabled · enable it in Commands"));
        // The … menu drops History while disabled.
        QVariant entries; QMetaObject::invokeMethod(window, "footerPanelEntries", Q_RETURN_ARG(QVariant, entries));
        QStringList ids; for (const auto &e : value(entries).toList()) ids << e.toMap()["id"].toString();
        QCOMPARE(ids, QStringList({"annotations", "notes", "related", "desk"}));
        run("toggleHistory"); QVERIFY(bible->historyEnabled());
        QTRY_VERIFY(item("historyControl")->isVisible());
        QMetaObject::invokeMethod(window, "footerPanelEntries", Q_RETURN_ARG(QVariant, entries));
        ids.clear(); for (const auto &e : value(entries).toList()) ids << e.toMap()["id"].toString();
        QCOMPARE(ids, QStringList({"annotations", "notes", "related", "history", "desk"}));
        // Collapsed footer: History joins the … menu.
        window->setMinimumWidth(320); window->resize(360, 640); QVERIFY(desktop->setProperty("textScale", 1.5));
        QTRY_VERIFY(item("footerMore")->isVisible()); QVERIFY(!item("historyControl")->isVisible());
        QMetaObject::invokeMethod(item("footerMore"), "clicked"); QTRY_VERIFY(object("contextMenu")->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(object("contextMenu"), "triggerId", Q_ARG(QVariant, QString("history"))));
        QTRY_VERIFY(historyOpen());
        key(Qt::Key_Escape);
    }
    void paletteRecentPassagesAndQueries() {
        for (auto r : {"John 1:3", "John 1:9", "John 2:4", "John 2:7", "John 2:9", "John 2:11", "John 1:30", "John 1:20"}) record(r);
        navigate("John 1:20");
        key(Qt::Key_K, Qt::ControlModifier); QTRY_VERIFY(object("passagePalette")->property("opened").toBool());
        auto rows = paletteRows();
        QStringList recents; for (const auto &r : rows) if (r.toMap().contains("recent")) recents << r.toMap()["title"].toString();
        // Max 5, never the current passage.
        QCOMPARE(recents, QStringList({"John 1:30", "John 2:11", "John 2:9", "John 2:7", "John 2:4"}));
        for (int i = 0; i < rows.size(); ++i) QCOMPARE(rows[i].toMap()["lastRecent"].toBool(), i == 4);
        QCOMPARE(rows.first().toMap()["title"].toString(), QString("John 1:30"));
        // One thin rule (the border color) under the recent group, before the books.
        QTRY_VERIFY(item("paletteRecentRule"));
        QList<QQuickItem *> rules; std::function<void(QQuickItem *)> walk = [&](QQuickItem *node) {
            if (node->objectName() == "paletteRecentRule" && node->isVisible()) rules << node;
            for (auto child : node->childItems()) walk(child);
        };
        walk(item("paletteResults"));
        QCOMPARE(rules.size(), 1);
        QCOMPARE(rules.first()->height(), 1.0);
        QCOMPARE(rules.first()->property("color").value<QColor>(), desktop->property("border").value<QColor>());
        auto list = item("paletteResults");
        const double ruleY = rules.first()->mapToItem(list, QPointF()).y();
        QCOMPARE(rules.first()->parentItem()->property("index").toInt(), 4);
        QVERIFY(ruleY >= rules.first()->parentItem()->mapToItem(list, QPointF()).y() + rules.first()->parentItem()->height());
        QCOMPARE(rows[5].toMap()["title"].toString(), QString("John"));
        QVERIFY(rows.first().toMap()["hint"].toString().startsWith("Recent"));
        type("jo"); QVERIFY(paletteRows().first().toMap()["title"].toString() == "John");
        key(Qt::Key_Backspace); key(Qt::Key_Backspace);
        key(Qt::Key_Down); key(Qt::Key_Return);
        QTRY_VERIFY(!object("passagePalette")->property("visible").toBool());
        QCOMPARE(here(), QString("John 2:11")); QCOMPARE(stack().last(), QString("2:11:engnet"));
        // Search palette: recent queries, newest first, at most five.
        for (const QString q : {"one", "two", "three", "four", "five", "alpha"}) QVERIFY(bible->recordQuery(q));
        key(Qt::Key_Slash, Qt::ControlModifier); QTRY_VERIFY(object("passagePalette")->property("opened").toBool());
        rows = paletteRows(); QStringList queries; for (const auto &r : rows) queries << r.toMap()["title"].toString();
        QCOMPARE(queries, QStringList({"alpha", "five", "four", "three", "two"}));
        key(Qt::Key_Return);
        QCOMPARE(object("referenceInput")->property("text").toString(), QString("/ alpha"));
        QTRY_VERIFY(prop("paletteSearchCurrent").toBool()); QTRY_VERIFY(!bible->searchBusy());
        QVERIFY(paletteRows().size() > 1);
        key(Qt::Key_Down); key(Qt::Key_Return);
        QTRY_VERIFY(!object("passagePalette")->property("visible").toBool());
        QCOMPARE(bible->recentQueries().first(), QString("alpha"));
        QVERIFY(bible->setHistoryEnabled(false));
        key(Qt::Key_K, Qt::ControlModifier); QTRY_VERIFY(object("passagePalette")->property("opened").toBool());
        QCOMPARE(paletteRows().first().toMap()["title"].toString(), QString("John"));
        key(Qt::Key_Escape);
    }
    void twoWindowsShareHistoryButNotStacks() {
        BibleBackend peer; QVERIFY(peer.ready());
        SoundEffects peerSounds(nullptr, session->filePath("sound2.ini"), false);
        QQmlApplicationEngine peerEngine;
        connect(&peerEngine, &QQmlEngine::warnings, this, [this](const QList<QQmlError> &errors) { for (const auto &error : errors) warnings << error.toString(); });
        peerEngine.rootContext()->setContextProperty("bible", &peer);
        peerEngine.rootContext()->setContextProperty("desktop", desktop.get());
        peerEngine.rootContext()->setContextProperty("sounds", &peerSounds);
        peerEngine.rootContext()->setContextProperty("catalog", catalog.get());
        peerEngine.load(QUrl::fromLocalFile(QFINDTESTDATA("../qml/Main.qml")));
        QVERIFY(!peerEngine.rootObjects().isEmpty());
        auto *other = qobject_cast<QQuickWindow *>(peerEngine.rootObjects().first()); QVERIFY(other);
        QVERIFY(QTest::qWaitForWindowExposed(other));
        window->requestActivate(); QTRY_VERIFY(window->isActive());
        other->setProperty("historyDwellMs", 300);
        window->setProperty("historyDwellMs", 300);
        jumpTo("John 1:10"); jumpTo("John 2:2");
        QTRY_VERIFY(historyRefs().contains("John 2:2"));
        QCOMPARE(peer.history(), bible->history());
        QVERIFY(QMetaObject::invokeMethod(other, "jump", Q_ARG(QVariant, QString("John 1:44"))));
        QTRY_VERIFY(peer.history().first().toMap()["reference"].toString() == "John 1:44");
        QCOMPARE(bible->history().first().toMap()["reference"].toString(), QString("John 1:44"));
        QCOMPARE(stack().size(), 3);
        QCOMPARE(value(other->property("jumpStack")).toList().size(), 2);
        // Clearing in one window empties the other's panel.
        QVERIFY(peer.clearHistory()); QVERIFY(bible->history().isEmpty());
        openHistory(); QTRY_VERIFY(item("historyEmpty")->isVisible()); key(Qt::Key_Escape);
        // Disabling elsewhere hides this window's button.
        QVERIFY(peer.setHistoryEnabled(false)); QTRY_VERIFY(!item("historyControl")->isVisible());
        QVERIFY(!bible->historyEnabled());
    }
};

int main(int argc, char **argv) {
    QQuickStyle::setStyle("Basic"); QApplication app(argc, argv);
    HistoryUiTest test; return QTest::qExec(&test, argc, argv);
}
#include "tst_historyui.moc"
