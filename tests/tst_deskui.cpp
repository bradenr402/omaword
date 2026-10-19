#include "testenvironment.h"
#include <QtTest/QTest>
#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QFontMetricsF>
#include <QJSValue>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickStyle>
#include <QQuickTextDocument>
#include <algorithm>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextFragment>
#include <QTextLayout>
#include "aofixture.h"
#include "biblebackend.h"
#include "blockdocument.h"
#include "desktop.h"
#include "soundeffects.h"
#include "versioncatalog.h"

class DeskUiTest : public QObject
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
    QString desk() const { return bible->deskPage(); }
    // The Desk's quotation of the selected verse: its text, then its reference.
    QString quote() {
        return "> " + bible->copySelection(true) + QStringLiteral("\n>\n> \u2014 ") + bible->selectionReference()
            + " (" + bible->versionAbbreviation() + ")";
    }
    static bool psalm(const QString &path) {
        return AoFixture::psalm(path);
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
    bool deskOpen() const { return prop("deskOpen").toBool() && object("notesDrawer")->property("opened").toBool(); }
    bool readerFocused() const { return window->activeFocusItem() == item("readerSurface"); }
    void openDesk() { key(Qt::Key_D, Qt::ShiftModifier); QTRY_VERIFY(deskOpen()); QTRY_VERIFY(item("deskEditor")->hasActiveFocus()); }
    QString hints() const { return object("deskHints")->property("plainText").toString(); }
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
        // Save on every change, as the assertions below expect (the app waits for a pause).
        window->setProperty("noteSaveDelay", 0);
        window->requestActivate(); QVERIFY(QTest::qWaitForWindowExposed(window)); QTRY_VERIFY(window->isActive());
        navigate("John 1:1");
    }
    void cleanup() {
        engine.reset(); window = nullptr; catalog.reset(); sounds.reset(); desktop.reset(); bible.reset(); session.reset();
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
    }

    void addWithKeysVisualCountsAndUndo() {
        QCOMPARE(commandTitle("add to desk"), QString("Add to Desk"));
        const auto first = quote(), reference = bible->selectionReference();
        key(Qt::Key_D);
        QCOMPARE(desk(), first + "\n");
        QCOMPARE(toast(), reference + " added to the Desk");
        // d adds again, never removes; u takes the latest addition off.
        key(Qt::Key_D); QCOMPARE(desk(), first + "\n\n" + first + "\n");
        key(Qt::Key_U); QCOMPARE(desk(), first + "\n");
        QCOMPARE(toast(), "Addition of " + reference + " to the Desk undone");
        // Visual range: the whole range is quoted, then Visual completes.
        key(Qt::Key_V, Qt::ShiftModifier); key(Qt::Key_J); key(Qt::Key_J);
        QVERIFY(prop("verseMode").toBool());
        const auto range = bible->selectionReference();
        QVERIFY(range != reference);
        key(Qt::Key_D);
        QVERIFY(desk().endsWith(QStringLiteral("\n>\n> \u2014 ") + range + " (" + bible->versionAbbreviation() + ")\n"));
        QVERIFY(!prop("verseMode").toBool());
        // Counts are consumed rather than repeating the addition.
        navigate("John 1:5"); const auto before = desk();
        type("3"); key(Qt::Key_D);
        QCOMPARE(desk(), before + "\n" + quote() + "\n"); QCOMPARE(prop("verseCount").toString(), QString());
    }
    void deskShowsInTheMarginAndTurnsWithNotes() {
        navigate("John 1:4"); key(Qt::Key_D);
        // shift D: the Margin shows the Desk, the keyboard on its page's end.
        openDesk();
        QVERIFY(item("deskControl")->property("selected").toBool());
        QVERIFY(item("deskPanel")->isVisible()); QVERIFY(!item("notesPanel")->isVisible());
        QCOMPARE(prop("sidePanel").toString(), QString("desk"));
        QCOMPARE(item("deskEditor")->property("text").toString(), desk());
        QCOMPARE(item("deskEditor")->property("cursorPosition").toInt(), desk().size());
        // Its header, as Marks': the title, then its passages and words.
        QCOMPARE(item("deskTitle")->property("text").toString(), QString("Desk"));
        const auto summary = [&] { return item("deskSummary")->property("text").toString(); };
        QVERIFY2(summary().startsWith("1 passage · ") && summary().endsWith(" words"), qPrintable(summary()));
        const int words = summary().section(" · ", 1).section(' ', 0, 0).toInt();
        type("Thoughts.");
        QCOMPARE(desk(), quote() + "\nThoughts.");
        QTRY_COMPARE(summary(), QString("1 passage · %1 words").arg(words + 1));
        QVERIFY(hints().endsWith("ctrl w to the text    esc close"));
        // The page has the keyboard: an accent edge toward the text, the
        // text a little behind it (a 10% veil of the background), the footer not.
        QCOMPARE(item("deskPanelEdge")->property("color").value<QColor>(), desktop->property("accent").value<QColor>());
        QTRY_COMPARE(item("readerDim")->opacity(), 0.1); QVERIFY(item("readerDim")->isVisible());
        QCOMPARE(item("readerDim")->property("color").value<QColor>(), desktop->property("background").value<QColor>());
        QVERIFY(item("readerDim")->parentItem()->isAncestorOf(item("reading")));
        QVERIFY(!item("readerDim")->parentItem()->isAncestorOf(item("footerZone")));
        QCOMPARE(item("deskPanel")->opacity(), 1.0);
        // Writing owns the keys: w and j type.
        type(" wj"); QVERIFY(desk().endsWith("Thoughts. wj")); QCOMPARE(bible->selectionReference(), QString("John 1:4"));
        // ctrl w: back to the reader, the Desk still open (its edge a hairline,
        // its hints how to come back); j moves; ctrl w returns to the page.
        key(Qt::Key_W, Qt::ControlModifier); QTRY_VERIFY(readerFocused()); QVERIFY(deskOpen());
        QCOMPARE(item("deskPanelEdge")->property("color").value<QColor>(), desktop->property("border").value<QColor>());
        QCOMPARE(hints(), QString("ctrl w keys here"));
        // Now the page recedes to 90% instead, and the text is clear.
        QTRY_COMPARE(item("deskPanel")->opacity(), 0.9);
        QTRY_VERIFY(!item("readerDim")->isVisible()); QCOMPARE(item("readerDim")->opacity(), 0.0);
        key(Qt::Key_J); QCOMPARE(bible->selectionReference(), QString("John 1:5"));
        key(Qt::Key_W, Qt::ControlModifier); QTRY_VERIFY(item("deskEditor")->hasActiveFocus()); QVERIFY(deskOpen());
        QVERIFY(desk().endsWith("Thoughts. wj")); // ctrl w types nothing.
        // w turns the Margin to the note, and shift D back: it stays open.
        key(Qt::Key_W, Qt::ControlModifier); QTRY_VERIFY(readerFocused());
        key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        QVERIFY(prop("notesOpen").toBool()); QVERIFY(!prop("deskOpen").toBool());
        QCOMPARE(item("notesTitle")->property("text").toString(), QString("Notes"));
        QVERIFY2(item("notesPassage")->property("text").toString().startsWith("John 1:5 · "), qPrintable(item("notesPassage")->property("text").toString()));
        QVERIFY(item("notesPanel")->isVisible()); QVERIFY(!item("deskPanel")->isVisible());
        QVERIFY(object("notesDrawer")->property("opened").toBool());
        key(Qt::Key_W, Qt::ControlModifier); QTRY_VERIFY(readerFocused());
        key(Qt::Key_D, Qt::ShiftModifier); QTRY_VERIFY(item("deskEditor")->hasActiveFocus()); QVERIFY(deskOpen());
        QTRY_VERIFY(item("readerDim")->isVisible());
        // esc closes it, from the page or from the reader; so does its ×; other
        // panels take its place in the Margin. The text is clear again.
        key(Qt::Key_Escape); QTRY_VERIFY(!object("notesDrawer")->property("visible").toBool()); QTRY_VERIFY(readerFocused());
        QTRY_VERIFY(!item("readerDim")->isVisible());
        openDesk(); key(Qt::Key_F6); QTRY_VERIFY(readerFocused());
        key(Qt::Key_Escape); QTRY_VERIFY(!object("notesDrawer")->property("visible").toBool());
        openDesk();
        QTest::mouseClick(window, Qt::LeftButton, {}, item("deskClose")->mapToScene(QPointF(6, 6)).toPoint());
        QTRY_VERIFY(!object("notesDrawer")->property("visible").toBool()); QTRY_VERIFY(readerFocused());
        openDesk(); key(Qt::Key_W, Qt::ControlModifier); QTRY_VERIFY(readerFocused());
        key(Qt::Key_M, Qt::ShiftModifier); QTRY_VERIFY(prop("marksOpen").toBool()); QVERIFY(!deskOpen());
        QVERIFY(item("marksPanel")->isVisible()); QVERIFY(!item("deskPanel")->isVisible());
        QVERIFY(object("notesDrawer")->property("opened").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!object("notesDrawer")->property("visible").toBool()); QTRY_VERIFY(readerFocused());
        QCOMPARE(desk(), bible->deskPage());
    }
    void pageWritesPreviewsAndFollowsLinks() {
        openDesk();
        type("See John 1:4 here."); QCOMPARE(desk(), QString("See John 1:4 here."));
        // alt enter on a link follows it; the Desk stays open.
        item("deskEditor")->setProperty("cursorPosition", 6);
        QTRY_VERIFY(hints().contains("alt enter open link"));
        key(Qt::Key_Return, Qt::AltModifier);
        QTRY_COMPARE(bible->selectionReference(), QString("John 1:4")); QVERIFY(deskOpen());
        // tab types a tab: the page keeps the keyboard.
        item("deskEditor")->forceActiveFocus(); item("deskEditor")->setProperty("cursorPosition", desk().size());
        key(Qt::Key_Tab); QCOMPARE(desk(), QString("See John 1:4 here.\t")); QVERIFY(item("deskEditor")->hasActiveFocus());
        key(Qt::Key_Backtab); QCOMPARE(desk(), QString("See John 1:4 here."));
        // ctrl z undoes typing; ctrl y redoes it, as ctrl shift Z does.
        type("!"); key(Qt::Key_Z, Qt::ControlModifier); QCOMPARE(desk(), QString("See John 1:4 here."));
        key(Qt::Key_Y, Qt::ControlModifier); QCOMPARE(desk(), QString("See John 1:4 here.!"));
        key(Qt::Key_Z, Qt::ControlModifier); key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(desk(), QString("See John 1:4 here.!")); QVERIFY(item("deskEditor")->hasActiveFocus());
        key(Qt::Key_Backspace); QCOMPARE(desk(), QString("See John 1:4 here."));
        // alt v: Preview renders the link; a click on it jumps there.
        navigate("John 1:1"); key(Qt::Key_D, Qt::ShiftModifier); QTRY_VERIFY(item("deskEditor")->hasActiveFocus());
        key(Qt::Key_V, Qt::AltModifier); QTRY_VERIFY(item("deskPreview")->hasActiveFocus());
        QVERIFY(prop("deskPreview").toBool()); QVERIFY(hints().contains("alt v write"));
        const auto preview = item("deskPreview");
        const auto document = preview->property("textDocument").value<QQuickTextDocument *>()->textDocument();
        QVERIFY(document->toHtml().contains("omaword-passage:"));
        const auto found = document->find("John 1:4"); QVERIFY(!found.isNull());
        const auto point = preview->mapToScene(caret(preview, found.selectionStart() + 2).center()).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point);
        QTRY_COMPARE(bible->selectionReference(), QString("John 1:4")); QVERIFY(deskOpen());
        // ctrl w returns to the reader from the preview too; alt v there leaves the Desk alone.
        preview->forceActiveFocus(); key(Qt::Key_W, Qt::ControlModifier); QTRY_VERIFY(readerFocused());
        key(Qt::Key_V, Qt::AltModifier); QVERIFY(prop("deskPreview").toBool());
        key(Qt::Key_D, Qt::ShiftModifier); QTRY_VERIFY(item("deskPreview")->hasActiveFocus());
        key(Qt::Key_V, Qt::AltModifier); QTRY_VERIFY(item("deskEditor")->hasActiveFocus()); QVERIFY(!prop("deskPreview").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!object("notesDrawer")->property("visible").toBool());
    }
    void additionsReachAnOpenDeskAndItsDraft() {
        openDesk(); key(Qt::Key_W, Qt::ControlModifier); QTRY_VERIFY(readerFocused());
        // From the reader, d adds to the open page.
        navigate("John 1:6"); const auto six = quote(); key(Qt::Key_D);
        QTRY_COMPARE(item("deskEditor")->property("text").toString(), six + "\n");
        // A draft on the page saves before an addition: both are kept.
        window->setProperty("noteSaveDelay", 400);
        key(Qt::Key_D, Qt::ShiftModifier); QTRY_VERIFY(item("deskEditor")->hasActiveFocus());
        type("Mine."); QCOMPARE(desk(), six + "\n");
        key(Qt::Key_W, Qt::ControlModifier); QTRY_VERIFY(readerFocused());
        QCOMPARE(desk(), six + "\nMine.");
        item("deskEditor")->forceActiveFocus(); type(" More."); QCOMPARE(desk(), six + "\nMine.");
        navigate("John 1:7"); const auto seven = quote(); key(Qt::Key_D);
        QCOMPARE(desk(), six + "\nMine. More.\n\n" + seven + "\n");
        QTRY_COMPARE(item("deskEditor")->property("text").toString(), desk());
        // Marks adds its row with d too.
        bible->setNoteText("Noted"); call("readerFocus");
        key(Qt::Key_M, Qt::ShiftModifier); QTRY_VERIFY(prop("marksOpen").toBool());
        QTRY_VERIFY(item("collectionList")->hasActiveFocus());
        key(Qt::Key_D); QCOMPARE(desk(), six + "\nMine. More.\n\n" + seven + "\n\n" + seven + "\n");
        key(Qt::Key_Escape); QTRY_VERIFY(!object("notesDrawer")->property("visible").toBool());
    }
    void liveUpdatesAndConflicts() {
        openDesk();
        QCOMPARE(item("deskEditor")->property("text").toString(), QString());
        QVERIFY(!item("deskEditor")->property("placeholderText").toString().isEmpty());
        // Another window's addition shows at once.
        BibleBackend peer;
        QVERIFY(bible->navigate("John 1:2")); const auto scope = bible->tagScope();
        QVERIFY(!peer.appendToDesk(scope).isEmpty());
        QTRY_COMPARE(item("deskEditor")->property("text").toString(), peer.deskPage());
        // A draft the page changed under keeps its text and offers both.
        window->setProperty("noteSaveDelay", 100000);
        item("deskEditor")->forceActiveFocus(); type(" Draft");
        const auto draft = item("deskEditor")->property("text").toString();
        QVERIFY(peer.setDeskPage("Peer page", peer.deskPage()));
        QTRY_VERIFY(prop("deskConflict").toBool());
        QCOMPARE(item("deskEditor")->property("text").toString(), draft);
        QVERIFY(item("deskConflictMessage")->isVisible());
        QVERIFY(hints().contains("ctrl s save my draft"));
        // While it stands, nothing else may replace or lose it.
        QVariant flushed; QVERIFY(QMetaObject::invokeMethod(window, "flushNote", Q_RETURN_ARG(QVariant, flushed)));
        QVERIFY(!flushed.toBool()); QCOMPARE(desk(), QString("Peer page"));
        // ctrl s keeps the draft; ctrl shift S would take the page.
        key(Qt::Key_S, Qt::ControlModifier);
        QTRY_VERIFY(!prop("deskConflict").toBool()); QCOMPARE(desk(), draft); QCOMPARE(peer.deskPage(), draft);
        type("!");
        QVERIFY(peer.setDeskPage("Peer again", peer.deskPage()));
        QTRY_VERIFY(prop("deskConflict").toBool());
        key(Qt::Key_S, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(!prop("deskConflict").toBool());
        QCOMPARE(item("deskEditor")->property("text").toString(), QString("Peer again")); QCOMPARE(desk(), QString("Peer again"));
    }
    void quotesShowAMutedBarAndASmallIndent() {
        navigate("John 1:4"); key(Qt::Key_D);
        openDesk(); key(Qt::Key_V, Qt::AltModifier); QTRY_VERIFY(item("deskPreview")->hasActiveFocus());
        auto preview = item("deskPreview");
        QTRY_VERIFY(item("deskQuoteBars:0"));
        auto bar = item("deskQuoteBars:0");
        const auto document = preview->property("textDocument").value<QQuickTextDocument *>()->textDocument();
        const auto first = document->find("Common original"), last = document->find("John 1:4");
        QVERIFY(!first.isNull() && !last.isNull());
        const QRectF top = caret(preview, first.selectionStart()), bottom = caret(preview, last.selectionStart());
        // A small indent: 14 px from the bar's left edge to the text (Qt's own
        // is 40, on both sides).
        QCOMPARE(qRound(top.x() - bar->x()), 14);
        QVERIFY(bar->width() >= 2 && bar->width() <= 4);
        QCOMPARE(bar->property("color").value<QColor>(), prop("quoteBarColor").value<QColor>());
        // One bar spans the quote: its text and the reference below it.
        QVERIFY(bar->y() <= top.y() + 1); QVERIFY(bar->y() + bar->height() >= bottom.bottom() - 1);
        QVERIFY(!item("deskQuoteBars:1"));
        // A nested quote gets a bar per level (the bars are made anew).
        const double barX = bar->x();
        QVERIFY(bible->setDeskPage(desk() + "\n> > Deeper\n", desk()));
        QTRY_VERIFY(item("deskQuoteBars:2"));
        QList<double> xs;
        for (int i = 0; i < 3; ++i) xs << item("deskQuoteBars:" + QString::number(i))->x() - barX;
        std::sort(xs.begin(), xs.end());
        QCOMPARE(xs, (QList<double>{0, 0, 14}));
        // Notes' preview draws them the same way.
        key(Qt::Key_Escape); QTRY_VERIFY(readerFocused());
        key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        type("Thought"); key(Qt::Key_Return); type("> quoted");
        key(Qt::Key_V, Qt::AltModifier); QTRY_VERIFY(item("notePreview")->hasActiveFocus());
        QTRY_VERIFY(item("noteQuoteBars:0")); QVERIFY(!item("noteQuoteBars:1"));
        key(Qt::Key_Escape); QTRY_VERIFY(!object("notesDrawer")->property("visible").toBool());
    }
    // In Preview a single Enter breaks the line, and paragraphs, lists, and
    // quotes stand a gap apart; the markers telling them apart never show.
    void previewLineBreaksAndGaps() {
        QVERIFY(bible->setDeskPage("Line one\nLine two\n\nNext paragraph\n- a\n- b\n\n> q1\n> q2\n>\n> q3\n\n> r1\n\nAfter", desk()));
        openDesk(); key(Qt::Key_V, Qt::AltModifier); QTRY_VERIFY(item("deskPreview")->hasActiveFocus());
        const auto document = item("deskPreview")->property("textDocument").value<QQuickTextDocument *>()->textDocument();
        QTRY_VERIFY(!document->toPlainText().contains(QChar(0x200b)));
        QStringList texts; QList<double> tops;
        for (auto block = document->begin(); block.isValid(); block = block.next()) { texts << block.text(); tops << block.blockFormat().topMargin(); }
        QCOMPARE(texts, (QStringList{"Line one", "Line two", "Next paragraph", "a", "b", "q1", "q2", "q3", "r1", "After"}));
        // A gap is one line of the preview's text, as a blank line reads.
        const double gap = qRound(QFontMetricsF(item("deskPreview")->property("font").value<QFont>()).lineSpacing());
        QVERIFY(gap >= 15);
        QCOMPARE(tops, (QList<double>{0, 0, gap, gap, 0, gap, 0, gap, gap, gap}));
        // A list stands 1.6 em in (Qt's own 40 px a level reads as a deep
        // step), room for its bullet before the text.
        const double em = item("deskPreview")->property("font").value<QFont>().pixelSize();
        QCOMPARE(document->indentWidth(), double(qRound(1.6 * em)));
        int listed = -1;
        for (auto block = document->begin(); block.isValid(); block = block.next()) if (block.text() == "a") listed = block.position();
        const QRectF paragraph = caret(item("deskPreview"), document->find("Next paragraph").selectionStart()), entry = caret(item("deskPreview"), listed);
        QCOMPARE(qRound(entry.x() - paragraph.x()), qRound(1.6 * em));
        // Two quotes in a row keep separate bars; q1–q3 is one quote.
        QTRY_VERIFY(item("deskQuoteBars:1")); QVERIFY(!item("deskQuoteBars:2"));
        key(Qt::Key_Escape); QTRY_VERIFY(!object("notesDrawer")->property("visible").toBool());
    }
    // Lists stand a little in: 1.6 em a level (Qt's own 40 px reads as a
    // deep step), further only where a list's widest marker ("10.") would
    // not fit before its text; a list's items run on into a nested list and
    // back out, and a new list starts after a gap.
    void listsStandALittleIn() {
        QVERIFY(bible->setDeskPage("Para\n\n- a\n  - nested\n- b\n\n9. nine\n10. ten\n\n- [ ] task", desk()));
        openDesk(); key(Qt::Key_V, Qt::AltModifier); QTRY_VERIFY(item("deskPreview")->hasActiveFocus());
        auto preview = item("deskPreview");
        const auto document = preview->property("textDocument").value<QQuickTextDocument *>()->textDocument();
        QTRY_VERIFY(!document->toPlainText().contains(QChar(0x200b)));
        QStringList texts; QList<double> tops; QHash<QString, double> xs;
        for (auto block = document->begin(); block.isValid(); block = block.next()) {
            texts << block.text(); tops << block.blockFormat().topMargin();
            xs[block.text()] = caret(preview, block.position()).x();
        }
        QCOMPARE(texts, (QStringList{"Para", "a", "nested", "b", "nine", "ten", "task"}));
        const double gap = qRound(QFontMetricsF(preview->property("font").value<QFont>()).lineSpacing());
        QCOMPARE(tops, (QList<double>{0, gap, 0, 0, gap, 0, gap}));
        const QFontMetricsF metrics(preview->property("font").value<QFont>());
        const double level = qRound(1.6 * preview->property("font").value<QFont>().pixelSize());
        const double numbers = std::max(level, std::ceil(metrics.horizontalAdvance(' ') + metrics.horizontalAdvance("10.")));
        QCOMPARE(xs["a"] - xs["Para"], level);
        QCOMPARE(xs["nested"] - xs["Para"], 2 * level);
        QCOMPARE(xs["b"], xs["a"]);
        QCOMPARE(xs["nine"] - xs["Para"], numbers);
        QCOMPARE(xs["ten"], xs["nine"]);
        QVERIFY(xs["task"] - xs["Para"] >= level);
        QVERIFY(level < 40);
    }
    // A heading keeps its size and weight to its end, whatever it holds: a
    // passage link (the preview makes one of a reference), a web link,
    // emphasis, code; at the top of the page or below other text. Its link
    // keeps its color, its emphasis its slant, its code its face.
    void headingsKeepTheirLookThroughLinksAndEmphasis() {
        QVERIFY(bible->setDeskPage("# John 1:4 vs John 1:5\n\nText under it.\n\n# Plain title\n## A *quiet* [link](https://example.org) and `code`\n\n"
                                   "## Plain second\n\n### John 1:6\n\n### Plain third", desk()));
        openDesk(); key(Qt::Key_V, Qt::AltModifier); QTRY_VERIFY(item("deskPreview")->hasActiveFocus());
        const auto document = item("deskPreview")->property("textDocument").value<QQuickTextDocument *>()->textDocument();
        QTRY_VERIFY(!document->toPlainText().contains(QChar(0x200b)));
        // Each level's look, as Qt gives a plain heading.
        QHash<int, QTextCharFormat> plain;
        QList<QTextBlock> headings;
        for (auto block = document->begin(); block.isValid(); block = block.next()) {
            const int level = block.blockFormat().headingLevel();
            if (!level) continue;
            headings << block;
            if (block.text().startsWith("Plain")) plain.insert(level, block.begin().fragment().charFormat());
        }
        QCOMPARE(headings.size(), 6); QCOMPARE(plain.size(), 3);
        QVERIFY(plain[1].font().pixelSize() > plain[2].font().pixelSize());
        QVERIFY(plain[2].font().pixelSize() > plain[3].font().pixelSize());
        const int body = item("deskPreview")->property("font").value<QFont>().pixelSize();
        QVERIFY(plain[3].font().pixelSize() > body);
        int anchors = 0, italics = 0, codes = 0;
        for (const auto &block : headings) {
            const auto &look = plain[block.blockFormat().headingLevel()];
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const auto format = it.fragment().charFormat();
                QVERIFY2(format.font().pixelSize() == look.font().pixelSize() && format.fontWeight() == look.fontWeight(), qPrintable(block.text() + ": " + it.fragment().text()));
                anchors += format.isAnchor(); italics += format.fontItalic(); codes += format.fontFixedPitch();
                if (format.isAnchor()) QVERIFY(format.foreground().color() != look.foreground().color());
            }
        }
        QCOMPARE(anchors, 4); QCOMPARE(italics, 1); QCOMPARE(codes, 1);
        // The page's first line, all links and words, stands as tall as an H1.
        QVERIFY(headings[0].layout()->lineCount() > 0);
        QCOMPARE(headings[0].layout()->lineAt(0).height(), headings[1].layout()->lineAt(0).height());
    }
    // The preview shows no image and loads nothing an image or HTML names:
    // an image is a link to it, an image glyph before its alt text (else
    // "image"), HTML shows as typed; code, autolinks, and passage links stay.
    void previewsShowImagesAsLinksAndHtmlAsTyped() {
        QVERIFY(bible->setDeskPage("Before ![A map](https://example.org/map.png) after\n\n![](https://example.org/x.png)\n\n"
                                   "<img src=\"https://example.org/y.png\"> and <b>bold</b>\n\n`![code](x) <b>` stays\n\n"
                                   "<https://example.org/auto>\n\nWow!John 1:4\n\n```\n<b>![fenced](z)</b>\n```\n\n    ![indented](w)\n\n# A ![map](https://example.org/h.png)", desk()));
        openDesk(); key(Qt::Key_V, Qt::AltModifier); QTRY_VERIFY(item("deskPreview")->hasActiveFocus());
        const auto document = item("deskPreview")->property("textDocument").value<QQuickTextDocument *>()->textDocument();
        QTRY_VERIFY(!document->toPlainText().contains(QChar(0x200b)) && !document->toPlainText().contains(QChar(0x2063)));
        const QString text = document->toPlainText();
        QStringList glyphs, links;
        QList<qreal> heights;
        for (auto block = document->begin(); block.isValid(); block = block.next()) {
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const auto format = it.fragment().charFormat();
                if (format.isImageFormat()) {
                    const auto image = format.toImageFormat();
                    QVERIFY2(image.name().startsWith("omaword-glyph:"), qPrintable(image.name()));
                    QVERIFY(image.width() > 0 && image.height() > 0);
                    const auto picture = document->resource(QTextDocument::ImageResource, QUrl(image.name())).value<QImage>();
                    QVERIFY(!picture.isNull());
                    glyphs << format.anchorHref(); heights << image.height();
                } else if (format.isAnchor()) links << it.fragment().text() + " " + format.anchorHref();
            }
        }
        QCOMPARE(glyphs, (QStringList{"https://example.org/map.png", "https://example.org/x.png", "https://example.org/h.png"}));
        // In a heading, the glyph is the heading's size.
        QVERIFY2(heights[2] >= 1.8 * heights[0], qPrintable(QString("%1 vs %2").arg(heights[2]).arg(heights[0])));
        QVERIFY(text.contains(QStringLiteral("Before \uFFFCA map after")));
        QVERIFY(links.contains("A map https://example.org/map.png"));
        QVERIFY(links.contains("image https://example.org/x.png"));
        QVERIFY(links.contains("https://example.org/auto https://example.org/auto"));
        QVERIFY(text.contains("<img src=\"https://example.org/y.png\"> and <b>bold</b>"));
        QVERIFY(text.contains("![code](x) <b> stays"));
        QVERIFY(text.contains("<b>![fenced](z)</b>"));
        QVERIFY(text.contains("![indented](w)"));
        // A passage right after "!" stays a passage link.
        QVERIFY(text.contains("Wow!John 1:4"));
        QVERIFY(std::any_of(links.cbegin(), links.cend(), [](const QString &link) { return link.startsWith("John 1:4 omaword-passage:"); }));
    }
    void cleanDeskAsksFirstAndUndoes() {
        navigate("John 1:4"); key(Qt::Key_D); navigate("John 1:5"); key(Qt::Key_D);
        const auto page = desk(); QVERIFY(!page.isEmpty());
        QCOMPARE(commandTitle("clean desk"), QString("Clean Desk"));
        // From the Desk: its Clean… button asks first; esc cancels.
        openDesk();
        QTest::mouseClick(window, Qt::LeftButton, {}, item("deskClean")->mapToScene(QPointF(6, 6)).toPoint());
        QTRY_VERIFY(object("cleanDeskConfirmation")->property("opened").toBool());
        QTRY_VERIFY(item("cleanDeskCancel")->hasActiveFocus());
        key(Qt::Key_X); QCOMPARE(desk(), page);
        key(Qt::Key_Escape); QTRY_VERIFY(!object("cleanDeskConfirmation")->property("visible").toBool());
        QCOMPARE(desk(), page); QTRY_VERIFY(item("deskEditor")->hasActiveFocus());
        // From Commands, confirmed: the page empties in every window.
        BibleBackend peer;
        key(Qt::Key_Escape); QTRY_VERIFY(readerFocused());
        key(Qt::Key_K, Qt::ControlModifier); QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("> clean desk"); key(Qt::Key_Return);
        QTRY_VERIFY(object("cleanDeskConfirmation")->property("opened").toBool());
        QTRY_VERIFY(item("cleanDeskCancel")->hasActiveFocus());
        key(Qt::Key_Right); QVERIFY(item("cleanDeskConfirm")->hasActiveFocus()); key(Qt::Key_Return);
        QTRY_VERIFY(!object("cleanDeskConfirmation")->property("visible").toBool());
        QCOMPARE(desk(), QString()); QCOMPARE(peer.deskPage(), QString());
        QVERIFY(toast().startsWith("Cleaned the Desk"));
        QTRY_COMPARE(item("deskEditor")->property("text").toString(), QString());
        QCOMPARE(item("deskSummary")->property("text").toString(), QString("Empty · d adds the passage you read"));
        // u brings it back.
        QTRY_VERIFY(readerFocused()); key(Qt::Key_U); QCOMPARE(desk(), page);
        QTRY_COMPARE(item("deskEditor")->property("text").toString(), page);
        // ctrl shift D asks too: from the reader, and from the Desk page while typing.
        key(Qt::Key_D, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(object("cleanDeskConfirmation")->property("opened").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!object("cleanDeskConfirmation")->property("visible").toBool()); QCOMPARE(desk(), page);
        openDesk(); type("x"); QCOMPARE(desk(), page + "x");
        key(Qt::Key_D, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(object("cleanDeskConfirmation")->property("opened").toBool());
        key(Qt::Key_Right); key(Qt::Key_Return);
        QTRY_VERIFY(!object("cleanDeskConfirmation")->property("visible").toBool()); QCOMPARE(desk(), QString());
        QTRY_VERIFY(item("deskEditor")->hasActiveFocus());
        key(Qt::Key_Escape); QTRY_VERIFY(!object("notesDrawer")->property("visible").toBool());
        // An empty Desk has nothing to clean.
        QVERIFY(bible->cleanDesk()); call("requestCleanDesk");
        QVERIFY(!object("cleanDeskConfirmation")->property("visible").toBool());
        QCOMPARE(toast(), QString("The Desk is already clean"));
    }
    void keybindingsListed() {
        QVariantList actions = prop("actions").toList();
        QStringList ids; for (const auto &a : actions) ids << a.toMap()["id"].toString();
        for (auto id : {"desk", "toggleDesk", "cleanDesk"}) QVERIFY(ids.contains(id));
        for (const auto &a : actions)
            if (a.toMap()["id"] == "cleanDesk") QCOMPARE(a.toMap()["sequences"].toStringList(), QStringList{"Ctrl+Shift+D"});
        key(Qt::Key_Question); QTRY_VERIFY(object("helpDialog")->property("opened").toBool());
        object("keybindingSearch")->setProperty("text", "desk");
        QVariant rows; QMetaObject::invokeMethod(window, "filteredKeybindings", Q_RETURN_ARG(QVariant, rows));
        QStringList titles; for (const auto &r : value(rows).toList()) titles << r.toMap()["title"].toString();
        QVERIFY(titles.contains("Open Desk")); QVERIFY(titles.contains("Add to Desk"));
        // Grouped as README lists them, in the catalog's order within each group.
        object("keybindingSearch")->setProperty("text", "");
        QMetaObject::invokeMethod(window, "filteredKeybindings", Q_RETURN_ARG(QVariant, rows));
        QStringList groups, expected, listed;
        for (const auto &a : actions) if (!groups.contains(a.toMap()["group"].toString())) groups << a.toMap()["group"].toString();
        for (const auto &group : groups) for (const auto &a : actions)
            if (a.toMap()["group"] == group && a.toMap().contains("sequences")) expected << a.toMap()["id"].toString();
        for (const auto &r : value(rows).toList()) listed << r.toMap()["id"].toString();
        QCOMPARE(listed, expected);
        key(Qt::Key_Escape);
        // Each key's hover note names where it works: the editors' and
        // character selection's own gestures are theirs, not the reader's.
        const auto where = [&](const QString &id) {
            for (const auto &a : actions) if (a.toMap()["id"] == id) {
                QVariant title; QMetaObject::invokeMethod(window, "scopeTitle", Q_RETURN_ARG(QVariant, title), Q_ARG(QVariant, a));
                return title.toString();
            }
            return QString();
        };
        QCOMPARE(where("noteLeave"), QString("Writing a note or on the Desk"));
        QCOMPARE(where("noteLinkClick"), QString("Writing a note or on the Desk"));
        QVERIFY(where("charCopy").startsWith("Character selection · "));
        QCOMPARE(where("guideInstall"), QString("Add version guide"));
        QVERIFY(where("manageRename").startsWith("Tag picker"));
        QCOMPARE(where("clickMenu"), QString("Reader gesture"));
        QCOMPARE(where("books"), QString("This window · passage and command search · also while writing a note or on the Desk"));
    }
    void hebrewFirstTextStaysLeftToRight_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("verse") << QString("verse");
        QTest::newRow("paragraph") << QString("paragraph");
        QTest::newRow("block") << QString("block");
    }
    void hebrewFirstTextStaysLeftToRight() {
        QFETCH(QString, mode);
        const QString aleph = QString::fromUtf8("\u05d0"), word = QString::fromUtf8("\u05d0\u05e9\u05e8\u05d9");
        const auto path = fixtures.filePath("engpsa.zip"); QVERIFY(psalm(path)); install(path);
        bible->setReadingMode(mode);
        QTRY_COMPARE(prop("continuousReader").toBool(), mode != "verse");
        navigate("PSA 119:1");
        QCOMPARE(bible->verses().first().toMap()["heading"].toString(), aleph + " Aleph");
        if (mode == "verse") {
            auto heading = item("headingText1"); QVERIFY(heading && heading->isVisible());
            const auto shown = heading->property("text").toString();
            QVERIFY(QString(aleph + " Aleph").isRightToLeft());
            QVERIFY(!shown.isRightToLeft()); QCOMPARE(QString(shown).remove(QChar(0x200e)), aleph + " Aleph");
            QCOMPARE(heading->property("effectiveHorizontalAlignment").toInt(), int(Qt::AlignLeft));
            expectLeftToRight(item("verseBody1"), word, "Blessed");
            expectLeftToRight(item("verseBody3"), QString::fromUtf8("\u05d1"), "Beth");
        } else {
            QTRY_VERIFY(blockMapping() && blockMapping()->spans().size() == bible->verses().size());
            auto body = item("blockBody");
            expectLeftToRight(body, aleph + " Aleph", "Aleph");
            expectLeftToRight(body, word, "Blessed");
            expectLeftToRight(body, QString::fromUtf8("\u05d1 Beth begins"), "Beth begins");
        }
        // The chapter outline lists it left to right too: the letter first, at
        // the entry's left edge, as the reader shows the heading.
        const QSize size = window->size();
        window->resize(1600, size.height()); bible->setChapterOutline(true);
        QTRY_VERIFY(item("outlineEntryText:1") && item("outlineEntryText:1")->isVisible());
        {
            auto entry = item("outlineEntryText:1");
            const auto shown = entry->property("text").toString();
            QVERIFY(!shown.isRightToLeft()); QCOMPARE(QString(shown).remove(QChar(0x200e)), aleph + " Aleph");
            QCOMPARE(entry->property("effectiveHorizontalAlignment").toInt(), int(Qt::AlignLeft));
            QTest::qWait(50);
            auto grab = entry->grabToImage(); QVERIFY(grab);
            QTRY_VERIFY(!grab->image().isNull());
            const auto image = grab->image();
            int left = image.width(), right = -1;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    if (image.pixelColor(x, y).alpha() > 60) { left = qMin(left, x); right = qMax(right, x); }
            QVERIFY2(left < 4 && right < image.width() * 0.75, qPrintable(QString("ink %1..%2 of %3").arg(left).arg(right).arg(image.width())));
        }
        bible->setChapterOutline(false); window->resize(size);
        // Copy output is unchanged: no direction marks.
        QGuiApplication::clipboard()->clear(); key(Qt::Key_Y);
        QTRY_VERIFY(QGuiApplication::clipboard()->text().contains(word + " Blessed"));
        const auto copied = QGuiApplication::clipboard()->text();
        for (auto mark : {0x200e, 0x200f, 0x202a, 0x202b, 0x202c, 0x2066, 0x2067, 0x2068, 0x2069}) QVERIFY(!copied.contains(QChar(mark)));
        QCOMPARE(copied, bible->copySelection());
        if (mode != "verse") return;
        // Search results use the same LTR paragraph.
        search("blessed blameless"); QTRY_VERIFY(item("searchResultText0"));
        expectLeftToRight(item("searchResultText0"), word, "Blessed");
        key(Qt::Key_Escape); QTRY_VERIFY(!object("passagePalette")->property("visible").toBool());
        // The Desk quotes it without direction marks.
        call("readerFocus"); navigate("PSA 119:1"); key(Qt::Key_D);
        QVERIFY(desk().contains(word + " Blessed")); QVERIFY(!desk().contains(QChar(0x200e)));
    }
};

int main(int argc, char **argv) {
    QQuickStyle::setStyle("Basic"); QApplication app(argc, argv);
    DeskUiTest test; return QTest::qExec(&test, argc, argv);
}
#include "tst_deskui.moc"
