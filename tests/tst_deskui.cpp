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
#include <archive.h>
#include <archive_entry.h>
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
    QString desk() const { return bible->deskPage(); }
    // The Desk's quotation of the selected verse: its text, then its reference.
    QString quote() {
        return "> " + bible->copySelection(true) + QStringLiteral("\n>\n> \u2014 ") + bible->selectionReference()
            + " (" + bible->versionAbbreviation() + ")";
    }
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
        type("Thoughts.");
        QCOMPARE(desk(), quote() + "\nThoughts.");
        QVERIFY(hints().contains("esc back to reading"));
        // Writing owns the keys: w and j type.
        type(" wj"); QVERIFY(desk().endsWith("Thoughts. wj")); QCOMPARE(bible->selectionReference(), QString("John 1:4"));
        // esc: back to the reader, the Desk still open; j moves; shift D returns to the page.
        key(Qt::Key_Escape); QTRY_VERIFY(readerFocused()); QVERIFY(deskOpen());
        key(Qt::Key_J); QCOMPARE(bible->selectionReference(), QString("John 1:5"));
        key(Qt::Key_D, Qt::ShiftModifier); QTRY_VERIFY(item("deskEditor")->hasActiveFocus()); QVERIFY(deskOpen());
        // w turns the Margin to the note, and shift D back: it stays open.
        key(Qt::Key_Escape); QTRY_VERIFY(readerFocused());
        key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        QVERIFY(prop("notesOpen").toBool()); QVERIFY(!prop("deskOpen").toBool());
        QVERIFY(item("notesPanel")->isVisible()); QVERIFY(!item("deskPanel")->isVisible());
        QVERIFY(object("notesDrawer")->property("opened").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(readerFocused());
        key(Qt::Key_D, Qt::ShiftModifier); QTRY_VERIFY(item("deskEditor")->hasActiveFocus()); QVERIFY(deskOpen());
        // Other panels close it; the reader's esc closes it; so does its ×.
        key(Qt::Key_Escape); key(Qt::Key_Escape); QTRY_VERIFY(!object("notesDrawer")->property("visible").toBool());
        openDesk();
        QTest::mouseClick(window, Qt::LeftButton, {}, item("deskClose")->mapToScene(QPointF(6, 6)).toPoint());
        QTRY_VERIFY(!object("notesDrawer")->property("visible").toBool()); QTRY_VERIFY(readerFocused());
        openDesk(); key(Qt::Key_Escape);
        key(Qt::Key_M, Qt::ShiftModifier); QTRY_VERIFY(object("savedDrawer")->property("opened").toBool());
        QTRY_VERIFY(!object("notesDrawer")->property("visible").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!object("savedDrawer")->property("visible").toBool());
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
        // Its esc returns to the reader too; alt v there leaves the Desk alone.
        preview->forceActiveFocus(); key(Qt::Key_Escape); QTRY_VERIFY(readerFocused());
        key(Qt::Key_V, Qt::AltModifier); QVERIFY(prop("deskPreview").toBool());
        key(Qt::Key_D, Qt::ShiftModifier); QTRY_VERIFY(item("deskPreview")->hasActiveFocus());
        key(Qt::Key_V, Qt::AltModifier); QTRY_VERIFY(item("deskEditor")->hasActiveFocus()); QVERIFY(!prop("deskPreview").toBool());
        key(Qt::Key_Escape); key(Qt::Key_Escape); QTRY_VERIFY(!object("notesDrawer")->property("visible").toBool());
    }
    void additionsReachAnOpenDeskAndItsDraft() {
        openDesk(); key(Qt::Key_Escape); QTRY_VERIFY(readerFocused());
        // From the reader, d adds to the open page.
        navigate("John 1:6"); const auto six = quote(); key(Qt::Key_D);
        QTRY_COMPARE(item("deskEditor")->property("text").toString(), six + "\n");
        // A draft on the page saves before an addition: both are kept.
        window->setProperty("noteSaveDelay", 400);
        key(Qt::Key_D, Qt::ShiftModifier); QTRY_VERIFY(item("deskEditor")->hasActiveFocus());
        type("Mine."); QCOMPARE(desk(), six + "\n");
        key(Qt::Key_Escape); QTRY_VERIFY(readerFocused());
        QCOMPARE(desk(), six + "\nMine.");
        item("deskEditor")->forceActiveFocus(); type(" More."); QCOMPARE(desk(), six + "\nMine.");
        navigate("John 1:7"); const auto seven = quote(); key(Qt::Key_D);
        QCOMPARE(desk(), six + "\nMine. More.\n\n" + seven + "\n");
        QTRY_COMPARE(item("deskEditor")->property("text").toString(), desk());
        // Marks adds its row with d too.
        bible->setNoteText("Noted"); call("readerFocus");
        key(Qt::Key_M, Qt::ShiftModifier); QTRY_VERIFY(object("savedDrawer")->property("opened").toBool());
        QTRY_VERIFY(item("collectionList")->hasActiveFocus());
        key(Qt::Key_D); QCOMPARE(desk(), six + "\nMine. More.\n\n" + seven + "\n\n" + seven + "\n");
        key(Qt::Key_Escape); QTRY_VERIFY(!object("savedDrawer")->property("visible").toBool());
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
        key(Qt::Key_Escape); key(Qt::Key_Escape); QTRY_VERIFY(!object("notesDrawer")->property("visible").toBool());
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
        // Two quotes in a row keep separate bars; q1–q3 is one quote.
        QTRY_VERIFY(item("deskQuoteBars:1")); QVERIFY(!item("deskQuoteBars:2"));
        key(Qt::Key_Escape); key(Qt::Key_Escape);
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
        key(Qt::Key_Escape); key(Qt::Key_Escape);
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
        key(Qt::Key_Escape);
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
