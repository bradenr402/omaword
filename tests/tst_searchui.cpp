#include <QtTest/QTest>
#include <QApplication>
#include <QFile>
#include <QJSValue>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <archive.h>
#include <archive_entry.h>
#include "biblebackend.h"
#include "desktop.h"
#include "versioncatalog.h"

class SearchUiTest : public QObject
{
    Q_OBJECT
    QTemporaryDir fixtures;
    std::unique_ptr<QTemporaryDir> session;
    std::unique_ptr<BibleBackend> bible;
    std::unique_ptr<Desktop> desktop;
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
            data += QString("\\c %1\n\\p\n").arg(chapter).toUtf8();
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
        key(Qt::Key_S); QTRY_VERIFY(object("passagePalette")->property("opened").toBool());
        QCOMPARE(object("referenceInput")->property("text").toString(), QString("/"));
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
        desktop = std::make_unique<Desktop>(); catalog = std::make_unique<VersionCatalog>();
        engine = std::make_unique<QQmlApplicationEngine>(); warnings.clear();
        connect(engine.get(), &QQmlEngine::warnings, this, [this](const QList<QQmlError> &errors) { for (const auto &error : errors) warnings << error.toString(); });
        engine->rootContext()->setContextProperty("bible", bible.get());
        engine->rootContext()->setContextProperty("desktop", desktop.get());
        engine->rootContext()->setContextProperty("catalog", catalog.get());
        engine->load(QUrl::fromLocalFile(QFINDTESTDATA("../qml/Main.qml")));
        QVERIFY2(!engine->rootObjects().isEmpty(), qPrintable(warnings.join('\n')));
        window = qobject_cast<QQuickWindow *>(engine->rootObjects().first()); QVERIFY(window);
        window->requestActivate(); QVERIFY(QTest::qWaitForWindowExposed(window)); QTRY_VERIFY(window->isActive());
        navigate("John 1:1");
    }
    void cleanup() {
        engine.reset(); window = nullptr; catalog.reset(); desktop.reset(); bible.reset(); session.reset();
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
    }
    void bibleQueriesAndResume() {
        const auto original = bible->tagScope();
        search("alpha beta"); QCOMPARE(bible->searchResultCount(), 8); QCOMPARE(bible->tagScope(), original);
        QVERIFY(!item("referencePreview")->isVisible());
        key(Qt::Key_Down); const int row = object("paletteResults")->property("currentIndex").toInt();
        key(Qt::Key_Escape); QCOMPARE(object("referenceInput")->property("text").toString(), QString("/"));
        key(Qt::Key_Escape); QTRY_VERIFY(!object("passagePalette")->property("visible").toBool());
        key(Qt::Key_Space); QTRY_COMPARE(object("referenceInput")->property("text").toString(), QString("/alpha beta"));
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
        key(Qt::Key_Escape); type("common"); QTRY_VERIFY(prop("paletteSearchCurrent").toBool()); QTRY_VERIFY(!bible->searchBusy());
        QCOMPARE(bible->searchResultCount(), 130); QCOMPARE(bible->searchResults().size(), 100);
        bible->loadMoreSearchResults(); QTRY_COMPARE(object("paletteResults")->property("count").toInt(), 130);
        object("paletteResults")->setProperty("currentIndex", 115);
        QVERIFY(QMetaObject::invokeMethod(object("paletteResults"), "positionViewAtIndex", Q_ARG(int, 115), Q_ARG(int, 2)));
        QTRY_VERIFY(object("paletteResults")->property("contentY").toDouble() > 0);
        key(Qt::Key_Escape); key(Qt::Key_Escape); QTRY_VERIFY(!object("passagePalette")->property("visible").toBool());
        key(Qt::Key_Space); QTRY_VERIFY(prop("paletteSearchCurrent").toBool()); QTRY_VERIFY(!bible->searchBusy());
        QTRY_COMPARE(object("paletteResults")->property("currentIndex").toInt(), 115);
        QTRY_VERIFY(object("paletteResults")->property("contentY").toDouble() > 0);
        key(Qt::Key_K, Qt::ControlModifier); QCOMPARE(object("referenceInput")->property("text").toString(), QString());
        QVERIFY(!prop("paletteTextMode").toBool());
    }
    void previewCancelAndVisualNormal() {
        find("alpha"); key(Qt::Key_Return); QTRY_VERIFY(!prop("findPromptOpen").toBool());
        navigate("John 1:35");
        const auto scope = bible->tagScope(); const auto y = object("reading")->property("contentY").toDouble();
        const auto height = item("reading")->height();
        find("beta"); QCOMPARE(bible->tagScope(), scope); QCOMPARE(item("reading")->height(), height);
        QVERIFY(object("reading")->property("contentY").toDouble() != y);
        key(Qt::Key_Escape); QCOMPARE(bible->tagScope(), scope); QCOMPARE(prop("acceptedSearchQuery").toString(), QString("alpha"));
        QCOMPARE(object("reading")->property("contentY").toDouble(), y);
        key(Qt::Key_V); key(Qt::Key_K, Qt::ShiftModifier); QVERIFY(prop("verseMode").toBool()); const int cursor = bible->cursorVerse();
        key(Qt::Key_Slash); QTRY_VERIFY(prop("findPromptOpen").toBool()); QVERIFY(!prop("verseMode").toBool());
        QCOMPARE(bible->selectedStart(), cursor); QCOMPARE(bible->selectedEnd(), cursor);
        QCOMPARE(object("chapterFindInput")->property("selectedText").toString(), QString("alpha"));
        type("unmatched"); QTest::qWait(180); key(Qt::Key_Escape);
        QCOMPARE(bible->cursorVerse(), cursor); QVERIFY(!prop("verseMode").toBool());
    }
    void countedNavigationAndTypingGuards() {
        find("beta"); key(Qt::Key_Return); QCOMPARE(bible->cursorVerse(), 2);
        key(Qt::Key_3); key(Qt::Key_N); QCOMPARE(bible->cursorVerse(), 50); QCOMPARE(prop("verseCount").toString(), QString());
        key(Qt::Key_N); QCOMPARE(bible->cursorVerse(), 50); QCOMPARE(object("toast")->property("text").toString(), QString("Last match"));
        key(Qt::Key_9); key(Qt::Key_N, Qt::ShiftModifier); QCOMPARE(bible->cursorVerse(), 2);
        QCOMPARE(object("toast")->property("text").toString(), QString("First match"));
        key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus()); type("nw/s");
        QCOMPARE(object("noteEditor")->property("text").toString(), QString("nw/s"));
        QVERIFY(!prop("findPromptOpen").toBool()); QVERIFY(!object("passagePalette")->property("visible").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!object("notesDrawer")->property("visible").toBool());
        key(Qt::Key_N, Qt::ControlModifier); QTRY_VERIFY(object("notesDrawer")->property("visible").toBool());
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
        const double y = object("reading")->property("contentY").toDouble();
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
        QVERIFY(prop("findPromptOpen").toBool()); QVERIFY(item("chapterFindInput")->hasActiveFocus());
        QCOMPARE(bible->tagScope(), scope); QCOMPARE(bible->cursorVerse(), 60);
        QCOMPARE(object("reading")->property("contentY").toDouble(), y);
        QCOMPARE(prop("acceptedSearchQuery").toString(), QString("alpha"));
        QCOMPARE(prop("chapterQueryHistory").toString(), QString("alpha"));
        key(Qt::Key_Escape); QVERIFY(!prop("findPromptOpen").toBool());
        QCOMPARE(bible->tagScope(), scope); QCOMPARE(object("reading")->property("contentY").toDouble(), y);
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
        bible->setFocusMode(true); QTRY_VERIFY(!item("locationControls")->isVisible()); call("readerFocus");
        QVERIFY(!prop("searchSegments").toList().isEmpty());
        key(Qt::Key_Escape);
        QVERIFY(bible->focusMode()); QVERIFY(prop("acceptedSearchQuery").toString().isEmpty());
        QVERIFY(prop("searchSegments").toList().isEmpty()); QCOMPARE(bible->tagScope(), scope);
        key(Qt::Key_Escape); QVERIFY(!bible->focusMode()); QCOMPARE(bible->tagScope(), scope);
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
        QVERIFY(object("reading")->property("contentY").toDouble() > 0);
        key(Qt::Key_Return); QCOMPARE(bible->cursorVerse(), 50);
        key(Qt::Key_N, Qt::ShiftModifier); QCOMPARE(bible->cursorVerse(), 20);
        navigate("John 2:35"); QTRY_VERIFY(!prop("searchSegments").toList().isEmpty());
        QCOMPARE(bible->cursorVerse(), 35); QCOMPARE(prop("searchMatchIndex").toInt(), -1);
        key(Qt::Key_N); QCOMPARE(bible->cursorVerse(), 50);
    }
    void guardsAndLiteralSyntax() {
        find("ALPh"); QVERIFY(prop("searchMatches").toList().size() > 4); key(Qt::Key_Return);
        QVERIFY(bible->toggleBookmark());
        key(Qt::Key_E, Qt::ShiftModifier); QTRY_VERIFY(prop("rangeEditing").toBool());
        const auto scope = bible->tagScope(); key(Qt::Key_Slash); key(Qt::Key_S); key(Qt::Key_N);
        QVERIFY(!prop("findPromptOpen").toBool()); QVERIFY(!object("passagePalette")->property("visible").toBool());
        QCOMPARE(bible->tagScope(), scope); key(Qt::Key_Escape); QTRY_VERIFY(!prop("rangeEditing").toBool());
        find("\"alpha\""); QCOMPARE(prop("searchMatches").toList().size(), 0); QVERIFY(prop("searchError").toString().isEmpty());
        key(Qt::Key_Escape);
        search("beta"); const auto original = bible->tagScope(); window->setProperty("noteConflict", true);
        object("noteEditor")->setProperty("text", "Unsaved conflict draft");
        key(Qt::Key_Return); QVERIFY(object("passagePalette")->property("visible").toBool());
        QCOMPARE(bible->tagScope(), original); QCOMPARE(object("referenceInput")->property("text").toString(), QString("/beta"));
        QCOMPARE(prop("acceptedSearchQuery").toString(), QString("ALPh"));
    }
};

int main(int argc, char **argv) {
    QQuickStyle::setStyle("Basic"); QApplication app(argc, argv);
    SearchUiTest test; return QTest::qExec(&test, argc, argv);
}
#include "tst_searchui.moc"
