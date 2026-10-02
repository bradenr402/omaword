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
    }
    void cleanup() {
        engine.reset(); window = nullptr; catalog.reset(); sounds.reset(); desktop.reset(); bible.reset(); session.reset();
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
    }

    void toggleWithKeysVisualAndCommandTitle() {
        QCOMPARE(commandTitle("add to desk"), QString("Add to Desk"));
        const auto reference = bible->selectionReference();
        key(Qt::Key_D);
        QCOMPARE(desk().size(), 1);
        QCOMPARE(desk().first().toMap()["reference"].toString(), reference);
        QVERIFY(bible->currentInDesk());
        QVERIFY(toast().contains("added to Desk"));
        QCOMPARE(commandTitle("from desk"), QString("Remove from Desk"));
        key(Qt::Key_D);
        QVERIFY(desk().isEmpty()); QVERIFY(!bible->currentInDesk());
        QVERIFY(toast().contains("removed from Desk"));
        // Visual range: the whole range is added, then Visual completes.
        key(Qt::Key_V); key(Qt::Key_J); key(Qt::Key_J);
        QVERIFY(prop("verseMode").toBool());
        const auto range = bible->selectionReference();
        QVERIFY(range != reference);
        key(Qt::Key_D);
        QCOMPARE(desk().size(), 1);
        QCOMPARE(desk().first().toMap()["reference"].toString(), range);
        QVERIFY(!prop("verseMode").toBool());
        // Counts are consumed rather than repeating the toggle.
        navigate("John 1:5"); type("3"); key(Qt::Key_D);
        QCOMPARE(desk().size(), 2); QCOMPARE(prop("verseCount").toString(), QString());
    }
    void panelShowsFullTextOpensAndSwitchesVersion() {
        QVERIFY(bible->navigate("John 1:2-3")); call("readerFocus"); key(Qt::Key_D);
        install(second); navigate("John 2:8"); key(Qt::Key_D);
        const auto kjv = bible->versionId();
        QVERIFY(bible->switchVersion(secondId())); navigate("John 1:1");
        const auto net = bible->versionId(); QVERIFY(net != kjv);
        QCOMPARE(desk().size(), 2);
        openDesk();
        QVERIFY(item("deskControl")->property("selected").toBool());
        const auto firstItem = desk().first().toMap();
        QCOMPARE(object("deskHeader0")->property("text").toString(), firstItem["reference"].toString() + " · " + firstItem["versionAbbreviation"].toString());
        QCOMPARE(object("deskText0")->property("text").toString().remove(QChar(0x200e)), firstItem["text"].toString());
        QVERIFY(firstItem["text"].toString().contains("ALPHA and beta"));
        QVERIFY(firstItem["text"].toString().contains("here across verses"));
        QCOMPARE(object("deskList")->property("currentIndex").toInt(), 0);
        key(Qt::Key_J); QCOMPARE(object("deskList")->property("currentIndex").toInt(), 1);
        {
            auto *current = qvariant_cast<QQuickItem *>(object("deskList")->property("currentItem")); QVERIFY(current);
            auto *rowBackground = qvariant_cast<QQuickItem *>(current->property("background")); QVERIFY(rowBackground);
            QVERIFY(rowBackground->property("color").value<QColor>().alpha() > 0);
            QCOMPARE(qvariant_cast<QObject *>(rowBackground->property("border"))->property("width").toReal(), 0.0);
        }
        key(Qt::Key_Down); QCOMPARE(object("deskList")->property("currentIndex").toInt(), 1);
        key(Qt::Key_K); QCOMPARE(object("deskList")->property("currentIndex").toInt(), 0);
        key(Qt::Key_J); key(Qt::Key_Return);
        QTRY_VERIFY(!object("deskDrawer")->property("visible").toBool());
        QCOMPARE(bible->versionId(), kjv);
        QCOMPARE(bible->selectionReference(), desk()[1].toMap()["reference"].toString());
        QTRY_VERIFY(item("readerSurface")->hasActiveFocus());
        // Click opens too, back in the other version.
        openDesk();
        QTest::mouseClick(window, Qt::LeftButton, {}, item("deskText0")->mapToScene(QPointF(4, 4)).toPoint());
        QTRY_VERIFY(!object("deskDrawer")->property("visible").toBool());
        QCOMPARE(bible->versionId(), net);
        QCOMPARE(bible->selectionReference(), firstItem["reference"].toString());
        key(Qt::Key_Escape);
    }
    void panelSpacingMatchesAnnotations() {
        for (auto r : {"John 1:1", "John 1:4"}) { navigate(r); key(Qt::Key_D); }
        const auto saved = object("savedDrawer");
        QVERIFY(QMetaObject::invokeMethod(saved, "open")); QTRY_VERIFY(saved->property("opened").toBool());
        const auto savedContent = item("savedContent");
        const QPointF savedOrigin = savedContent->position();
        const auto savedSpacing = savedContent->property("spacing").toReal();
        const auto savedListSpacing = item("collectionList")->property("spacing").toReal();
        QVERIFY(QMetaObject::invokeMethod(saved, "close")); QTRY_VERIFY(!saved->property("visible").toBool());
        openDesk();
        const auto drawer = object("deskDrawer");
        for (auto side : {"topPadding", "bottomPadding", "leftPadding", "rightPadding"})
            QCOMPARE(drawer->property(side).toReal(), saved->property(side).toReal());
        QVERIFY(saved->property("topPadding").toReal() > 0);
        const auto content = item("deskContent");
        QCOMPARE(content->position(), savedOrigin);
        QCOMPARE(content->property("spacing").toReal(), savedSpacing);
        const auto list = item("deskList");
        QCOMPARE(list->property("spacing").toReal(), savedListSpacing);
        // Rows are separated by the list spacing, with padded content.
        auto first = item("deskEntry0"), second = item("deskEntry1");
        QVERIFY(first && second);
        QTRY_COMPARE(second->y() - (first->y() + first->height()), savedListSpacing);
        QVERIFY(first->property("topPadding").toReal() > 0);
        const auto header = item("deskHeader0");
        QCOMPARE(header->mapToItem(first, QPointF()).x(), first->property("leftPadding").toReal());
        // Minimal rows: no per-item buttons; Clean Desk sits across from the title, count below it.
        QVERIFY(!item("deskCopy0") && !item("deskRemove0"));
        const auto title = item("deskTitle"), count = item("deskCount"), clean = item("deskClean");
        const auto titleRect = title->mapRectToItem(content, QRectF(0, 0, title->width(), title->height()));
        const auto countRect = count->mapRectToItem(content, QRectF(0, 0, count->width(), count->height()));
        const auto cleanRect = clean->mapRectToItem(content, QRectF(0, 0, clean->width(), clean->height()));
        QVERIFY(countRect.top() >= titleRect.bottom() - 0.5); QCOMPARE(countRect.left(), titleRect.left());
        QCOMPARE(cleanRect.top(), titleRect.top()); QVERIFY(qAbs(cleanRect.right() - content->width()) < 1);
        // The title starts at the drawer's padding, just like Annotations.
        QCOMPARE(item("deskTitle")->mapToItem(content, QPointF()).y(), item("savedTitle")->mapToItem(savedContent, QPointF()).y());
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
        // Search results and the Desk use the same LTR paragraph.
        search("blessed blameless"); QTRY_VERIFY(item("searchResultText0"));
        expectLeftToRight(item("searchResultText0"), word, "Blessed");
        key(Qt::Key_Escape); QTRY_VERIFY(!object("passagePalette")->property("visible").toBool());
        call("readerFocus"); navigate("PSA 119:1"); key(Qt::Key_D); openDesk();
        const auto shown = object("deskText0")->property("text").toString();
        QVERIFY(!shown.isRightToLeft()); QCOMPARE(QString(shown).remove(QChar(0x200e)), desk().first().toMap()["text"].toString());
        QCOMPARE(item("deskText0")->property("effectiveHorizontalAlignment").toInt(), int(Qt::AlignLeft));
        QGuiApplication::clipboard()->clear(); key(Qt::Key_Y);
        QTRY_VERIFY(!QGuiApplication::clipboard()->text().isEmpty());
        QVERIFY(!QGuiApplication::clipboard()->text().contains(QChar(0x200e)));
        key(Qt::Key_Escape);
    }
    void copyAndRemove() {
        for (auto r : {"John 1:1", "John 1:4", "John 1:6", "John 1:7"}) { navigate(r); key(Qt::Key_D); }
        QCOMPARE(desk().size(), 4);
        openDesk();
        QGuiApplication::clipboard()->clear();
        key(Qt::Key_Y);
        QTRY_VERIFY(QGuiApplication::clipboard()->text().contains("John 1:1"));
        QVERIFY(toast().contains("copied"));
        QVERIFY(deskOpen());
        key(Qt::Key_J); QGuiApplication::clipboard()->clear(); key(Qt::Key_Y);
        QTRY_VERIFY(QGuiApplication::clipboard()->text().contains("John 1:4"));
        key(Qt::Key_K); key(Qt::Key_X);
        QTRY_COMPARE(desk().size(), 3);
        QCOMPARE(desk().first().toMap()["reference"].toString(), QString("John 1:4"));
        QTRY_VERIFY(item("deskList")->hasActiveFocus());
        key(Qt::Key_J); key(Qt::Key_X);
        QCOMPARE(desk().size(), 2);
        QCOMPARE(desk()[1].toMap()["reference"].toString(), QString("John 1:7"));
        QTRY_COMPARE(object("deskList")->property("currentIndex").toInt(), 1);
        key(Qt::Key_Delete);
        QCOMPARE(desk().size(), 1);
        QVERIFY(deskOpen());
        key(Qt::Key_Escape); QTRY_VERIFY(!object("deskDrawer")->property("visible").toBool());
        QTRY_VERIFY(item("readerSurface")->hasActiveFocus());
    }
    void cleanDeskConfirmAndCancel() {
        for (auto r : {"John 1:1", "John 1:4"}) { navigate(r); key(Qt::Key_D); }
        openDesk();
        QTest::mouseClick(window, Qt::LeftButton, {}, item("deskClean")->mapToScene(QPointF(4, 4)).toPoint());
        QTRY_VERIFY(object("cleanDeskConfirmation")->property("opened").toBool());
        QTRY_VERIFY(item("cleanDeskCancel")->hasActiveFocus());
        key(Qt::Key_X); key(Qt::Key_J);
        QCOMPARE(desk().size(), 2);
        key(Qt::Key_Escape);
        QTRY_VERIFY(!object("cleanDeskConfirmation")->property("visible").toBool());
        QVERIFY(deskOpen()); QCOMPARE(desk().size(), 2);
        QTRY_VERIFY(item("deskList")->hasActiveFocus());
        key(Qt::Key_Escape); QTRY_VERIFY(!object("deskDrawer")->property("visible").toBool());
        // Command palette route.
        key(Qt::Key_K, Qt::ControlModifier); QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("> clean desk"); key(Qt::Key_Return);
        QTRY_VERIFY(object("cleanDeskConfirmation")->property("opened").toBool());
        QTRY_VERIFY(item("cleanDeskCancel")->hasActiveFocus());
        key(Qt::Key_Return);
        QTRY_VERIFY(!object("cleanDeskConfirmation")->property("visible").toBool());
        QCOMPARE(desk().size(), 2);
        call("requestCleanDesk"); QTRY_VERIFY(object("cleanDeskConfirmation")->property("opened").toBool());
        QTRY_VERIFY(item("cleanDeskCancel")->hasActiveFocus());
        key(Qt::Key_Right); QVERIFY(item("cleanDeskConfirm")->hasActiveFocus());
        key(Qt::Key_Return);
        QTRY_VERIFY(!object("cleanDeskConfirmation")->property("visible").toBool());
        QVERIFY(desk().isEmpty());
        QVERIFY(toast().contains("Cleaned the Desk"));
        QTRY_VERIFY(item("readerSurface")->hasActiveFocus());
    }
    void cleanDeskShiftXOnlyInDesk() {
        for (auto r : {"John 1:1", "John 1:4"}) { navigate(r); key(Qt::Key_D); }
        QCOMPARE(desk().size(), 2);
        auto confirmation = object("cleanDeskConfirmation");
        // Reader: Shift X is unbound.
        key(Qt::Key_X, Qt::ShiftModifier); QTest::qWait(100);
        QVERIFY(!confirmation->property("visible").toBool()); QCOMPARE(desk().size(), 2);
        // Annotations panel.
        key(Qt::Key_A, Qt::ShiftModifier); QTRY_VERIFY(object("savedDrawer")->property("opened").toBool());
        key(Qt::Key_X, Qt::ShiftModifier); QTest::qWait(100);
        QVERIFY(!confirmation->property("visible").toBool()); QCOMPARE(desk().size(), 2);
        key(Qt::Key_Escape); QTRY_VERIFY(!object("savedDrawer")->property("visible").toBool());
        // Tag panel.
        key(Qt::Key_T); QTRY_VERIFY(object("tagsSurface")->property("opened").toBool());
        key(Qt::Key_X, Qt::ShiftModifier); QTest::qWait(100);
        QVERIFY(!confirmation->property("visible").toBool()); QCOMPARE(desk().size(), 2);
        key(Qt::Key_Escape); QTRY_VERIFY(!object("tagsSurface")->property("visible").toBool());
        // Range editor.
        QVERIFY(bible->toggleBookmark());
        key(Qt::Key_E, Qt::ShiftModifier); QTRY_VERIFY(prop("rangeEditing").toBool());
        key(Qt::Key_X, Qt::ShiftModifier); QTest::qWait(100);
        QVERIFY(!confirmation->property("visible").toBool()); QCOMPARE(desk().size(), 2);
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("rangeEditing").toBool());
        // Desk: Shift X asks to clean everything; plain x still removes one.
        openDesk();
        key(Qt::Key_X, Qt::ShiftModifier);
        QTRY_VERIFY(confirmation->property("opened").toBool());
        QTRY_VERIFY(item("cleanDeskCancel")->hasActiveFocus());
        QCOMPARE(desk().size(), 2);
        key(Qt::Key_Escape); QTRY_VERIFY(!confirmation->property("visible").toBool());
        QVERIFY(deskOpen()); QCOMPARE(desk().size(), 2);
        QTRY_VERIFY(item("deskList")->hasActiveFocus());
        key(Qt::Key_X); QCOMPARE(desk().size(), 1);
        key(Qt::Key_Escape); QTRY_VERIFY(!object("deskDrawer")->property("visible").toBool());
    }
    void liveUpdateFromPeer() {
        openDesk();
        QCOMPARE(item("deskList")->property("count").toInt(), 0);
        QVERIFY(object("deskEmpty")->property("visible").toBool());
        const auto scope = bible->tagScope();
        BibleBackend peer;
        QVERIFY(peer.addToDesk(scope));
        QTRY_COMPARE(item("deskList")->property("count").toInt(), 1);
        QVERIFY(!object("deskEmpty")->property("visible").toBool());
        QVERIFY(bible->currentInDesk());
        QVERIFY(peer.cleanDesk());
        QTRY_COMPARE(item("deskList")->property("count").toInt(), 0);
    }
    void versionRemovalReportsDeskItems() {
        install(second); const auto kjv = bible->versionId();
        navigate("John 1:1"); key(Qt::Key_D); navigate("John 1:4"); key(Qt::Key_D);
        QVERIFY(bible->switchVersion(secondId())); navigate("John 1:1"); key(Qt::Key_D);
        QCOMPARE(desk().size(), 3);
        QVERIFY(QMetaObject::invokeMethod(window, "openVersions", Q_ARG(QVariant, true)));
        QTRY_VERIFY(object("versionDialog")->property("opened").toBool());
        int row = -1; const auto entries = value(object("versionResults")->property("model")).toList();
        for (int i = 0; i < entries.size(); ++i) if (entries[i].toMap()["id"].toString() == kjv) row = i;
        QVERIFY(row >= 0);
        object("versionResults")->setProperty("currentIndex", row);
        key(Qt::Key_Return); key(Qt::Key_Down); key(Qt::Key_Return);
        QTRY_COMPARE(object("versionDialog")->property("step").toInt(), 2);
        QVERIFY(object("versionDialog")->property("removeAnnotations").toBool());
        QVERIFY(object("versionSummary")->property("text").toString().contains("2 Desk items"));
        key(Qt::Key_Right); QVERIFY(item("versionNext")->hasActiveFocus()); key(Qt::Key_Return);
        QTRY_VERIFY(!object("versionDialog")->property("visible").toBool());
        QCOMPARE(desk().size(), 1);
        QVERIFY(toast().contains("2 Desk items removed"));
    }
    void keepingAnnotationsKeepsUnavailableItems() {
        install(second); const auto kjv = bible->versionId();
        navigate("John 1:1"); key(Qt::Key_D);
        QVERIFY(bible->removeVersion(kjv, false));
        QTRY_VERIFY(bible->ready());
        call("readerFocus");
        QCOMPARE(desk().size(), 1);
        openDesk();
        QVERIFY(object("deskUnavailable0")->property("visible").toBool());
        QGuiApplication::clipboard()->clear(); key(Qt::Key_Y);
        QVERIFY(QGuiApplication::clipboard()->text().isEmpty());
        const auto version = bible->versionId(); const auto reference = bible->selectionReference();
        key(Qt::Key_Return);
        QVERIFY(deskOpen()); QCOMPARE(bible->versionId(), version); QCOMPARE(bible->selectionReference(), reference);
        key(Qt::Key_Escape);
    }
    void keysDoNotLeakWhilePanelOpen() {
        navigate("John 1:5"); key(Qt::Key_D);
        openDesk();
        const auto reference = bible->selectionReference(); const auto chapter = bible->chapter();
        for (auto k : {Qt::Key_D, Qt::Key_M, Qt::Key_H, Qt::Key_L, Qt::Key_V, Qt::Key_T, Qt::Key_S, Qt::Key_Slash, Qt::Key_G, Qt::Key_3})
            key(k);
        QVERIFY(deskOpen());
        QCOMPARE(bible->selectionReference(), reference); QCOMPARE(bible->chapter(), chapter);
        QCOMPARE(desk().size(), 1); QVERIFY(!bible->selectionBookmarked());
        QVERIFY(!prop("verseMode").toBool()); QVERIFY(!prop("findPromptOpen").toBool()); QVERIFY(!prop("notesOpen").toBool());
        QVERIFY(!object("passagePalette")->property("visible").toBool());
        QCOMPARE(prop("verseCount").toString(), QString());
        QVERIFY(prop("modalOpen").toBool()); QVERIFY(!prop("readerKeys").toBool());
        // Its own key does nothing; esc closes.
        key(Qt::Key_D, Qt::ShiftModifier); QTest::qWait(50); QVERIFY(deskOpen());
        key(Qt::Key_Escape);
        QTRY_VERIFY(!object("deskDrawer")->property("visible").toBool());
        QTRY_VERIFY(item("readerSurface")->hasActiveFocus());
        key(Qt::Key_J); QVERIFY(bible->selectionReference() != reference);
        // Desk shortcuts are guarded while editing a range.
        QVERIFY(bible->toggleBookmark());
        key(Qt::Key_E, Qt::ShiftModifier); QTRY_VERIFY(prop("rangeEditing").toBool());
        key(Qt::Key_D); key(Qt::Key_D, Qt::ShiftModifier);
        QCOMPARE(desk().size(), 1); QVERIFY(!deskOpen());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("rangeEditing").toBool());
    }
    void keybindingsListed() {
        QVariantList actions = prop("actions").toList();
        QStringList ids; for (const auto &a : actions) ids << a.toMap()["id"].toString();
        for (auto id : {"desk", "toggleDesk", "cleanDesk"}) QVERIFY(ids.contains(id));
        key(Qt::Key_Question); QTRY_VERIFY(object("helpDialog")->property("opened").toBool());
        object("keybindingSearch")->setProperty("text", "desk");
        QVariant rows; QMetaObject::invokeMethod(window, "filteredKeybindings", Q_RETURN_ARG(QVariant, rows));
        QStringList titles; for (const auto &r : value(rows).toList()) titles << r.toMap()["title"].toString();
        QVERIFY(titles.contains("Open Desk")); QVERIFY(titles.contains("Add to Desk"));
        QVERIFY(titles.contains("Clean Desk"));
        for (const auto &r : value(rows).toList())
            if (r.toMap()["id"] == "cleanDesk") { QCOMPARE(r.toMap()["sequences"].toStringList(), QStringList{"Shift+X"}); QCOMPARE(r.toMap()["scope"].toString(), QString("desk")); }
        key(Qt::Key_Escape);
    }
};

int main(int argc, char **argv) {
    QQuickStyle::setStyle("Basic"); QApplication app(argc, argv);
    DeskUiTest test; return QTest::qExec(&test, argc, argv);
}
#include "tst_deskui.moc"
