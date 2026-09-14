#include <QtTest/QTest>
#include <QApplication>
#include <QDir>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <archive.h>
#include <archive_entry.h>
#include "biblebackend.h"
#include "desktop.h"
#include "versioncatalog.h"

// Original miniature USFM editions, real personal.sqlite and real backend calls.
// The isolated panel uses a window/controller/theme fixture; the integration test
// loads the real Main and components from the application's packaged resources.
class TagsUiTest : public QObject
{
    Q_OBJECT
    QTemporaryDir fixtures;
    std::unique_ptr<QTemporaryDir> session;
    std::unique_ptr<BibleBackend> bible;
    std::unique_ptr<QQmlEngine> engine;
    std::unique_ptr<QObject> root;
    QQuickWindow *window = nullptr;
    QObject *panel = nullptr;
    QStringList warnings;
    QString net, kjv;

    static bool edition(const QString &path, const QString &id) {
        archive *zip = archive_write_new();
        archive_write_set_format_zip(zip);
        if (archive_write_open_filename(zip, QFile::encodeName(path).constData()) != ARCHIVE_OK) {
            archive_write_free(zip); return false;
        }
        QByteArray data = "\\id JHN Original Tags UI Fixture\n\\h John\n\\toc1 John\n\\toc2 John\n";
        for (int chapter = 3; chapter <= 4; ++chapter) {
            data += QString("\\c %1\n\\p\n").arg(chapter).toUtf8();
            for (int verse = 1; verse <= 12; ++verse)
                data += QString("\\v %1 Original tag fixture %2:%1, literal <b>words</b> and café.\n").arg(verse).arg(chapter).toUtf8();
        }
        archive_entry *entry = archive_entry_new();
        const auto name = ("01-JHN" + id + ".usfm").toUtf8();
        archive_entry_set_pathname(entry, name.constData());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0600);
        archive_entry_set_size(entry, data.size());
        bool ok = archive_write_header(zip, entry) == ARCHIVE_OK
            && archive_write_data(zip, data.constData(), data.size()) == data.size();
        archive_entry_free(entry);
        ok = archive_write_close(zip) == ARCHIVE_OK && ok;
        archive_write_free(zip);
        return ok;
    }
    void install(const QString &path) {
        bible->importUsfm(QUrl::fromLocalFile(path));
        QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(), 30000);
        QVERIFY2(bible->ready() && bible->error().isEmpty(), qPrintable(bible->error()));
    }
    static QVariant variant(QVariant value) {
        return value.metaType() == QMetaType::fromType<QJSValue>() ? value.value<QJSValue>().toVariant() : value;
    }
    QVariant prop(const char *name) const { return variant(panel->property(name)); }
    QVariant invoke(const char *method) {
        if (qstrcmp(method, "open") == 0 || qstrcmp(method, "openBrowser") == 0) return invoke(method, QVariant());
        QVariant result;
        if (!QMetaObject::invokeMethod(panel, method, Q_RETURN_ARG(QVariant, result))) QTest::qFail(method, __FILE__, __LINE__);
        return variant(result);
    }
    QVariant invoke(const char *method, const QVariant &arg) {
        QVariant result;
        if (!QMetaObject::invokeMethod(panel, method, Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, arg))) QTest::qFail(method, __FILE__, __LINE__);
        return variant(result);
    }
    static QQuickItem *visual(QQuickItem *item, const QString &name) {
        if (item->objectName() == name) return item;
        for (auto child : item->childItems()) if (auto found = visual(child, name)) return found;
        return nullptr;
    }
    QQuickItem *item(const QString &name) const {
        if (auto found = root->findChild<QQuickItem *>(name)) return found;
        return visual(window->contentItem(), name);
    }
    void click(const QString &name) {
        auto target = item(name);
        QVERIFY2(target, qPrintable(name));
        QVERIFY2(target->isVisible() && target->isEnabled(), qPrintable(name));
        const auto point = target->mapToScene(QPointF(target->width()/2, target->height()/2)).toPoint();
        QVERIFY2(QRect(QPoint(), window->size()).contains(point), qPrintable(name));
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point);
        QCoreApplication::processEvents();
    }
    void key(Qt::Key key) { QTest::keyClick(window, key); QCoreApplication::processEvents(); }
    void search(const QString &text) {
        panel->setProperty("query", text);
        item("tagSearch")->forceActiveFocus();
        QCoreApplication::processEvents();
    }
    void openPicker() {
        QVERIFY(invoke("open").toBool());
        QTRY_VERIFY(prop("visibleModal").toBool());
        QTRY_VERIFY(item("tagSearch")->hasActiveFocus());
    }
    QVariantMap scope(int first, int last) {
        bible->openChapter(43, 3); bible->selectVerses(first, last);
        return bible->tagScope();
    }
    QVariantList rows() const { return prop("results").toList(); }
    QStringList ids(const QVariantList &tags) const {
        QStringList result;
        for (const auto &tag : tags) result << tag.toMap().value("id").toString();
        return result;
    }
private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("OmaWordTests");
        QCoreApplication::setApplicationName("omaword-tagsui-test");
        QVERIFY(fixtures.isValid());
        net = fixtures.filePath("engnet_usfm.zip"); kjv = fixtures.filePath("eng-kjv_usfm.zip");
        QVERIFY(edition(net, "engnet")); QVERIFY(edition(kjv, "eng-kjv"));
    }
    void init() {
        session = std::make_unique<QTemporaryDir>(); QVERIFY(session->isValid());
        qputenv("XDG_DATA_HOME", session->path().toUtf8());
        bible = std::make_unique<BibleBackend>();
        install(net); QVERIFY(bible->navigate("John 3:1-3"));
        engine = std::make_unique<QQmlEngine>();
        warnings.clear();
        connect(engine.get(), &QQmlEngine::warnings, this, [this](const QList<QQmlError> &errors) {
            for (const auto &error : errors) warnings << error.toString();
        });
        engine->rootContext()->setContextProperty("testBible", bible.get());
        QQmlComponent component(engine.get());
#ifndef Q_MOC_RUN
        component.setData(R"QML(
import QtQuick
import QtQuick.Controls
ApplicationWindow {
    id: app
    width: 1000; height: 900; visible: true
    property bool flushAllowed: true
    property int completed: 0
    property int closed: 0
    property var completedScope: null
    property bool pinnedAtCompletion: false
    property bool browserOpen: false
    function flushNote() { return flushAllowed }
    function savePosition() {}
    function syncNote() {}
    function scrollToVerse(verse, center) {}
    function restorePanelFocus() {}
    function editingPanelClosed() { closed++ }
    function tagActionCompleted(scope) { completed++; completedScope = scope; pinnedAtCompletion = tags.scopePinned }
    function openPassageTags(scope) { tags.open(scope) }
    QtObject {
        id: theme
        objectName: "testTheme"
        property color background: "#202020"
        property color foreground: "#eeeeee"
        property color surface: "#303030"
        property color muted: "#aaaaaa"
        property color accent: "#88bbff"
        property color border: "#555555"
        property string fontFamily: "sans-serif"
        property real textScale: 1
        property real cornerRadius: 0
    }
    Item { id: browserHost; anchors.fill: parent; anchors.margins: 24; visible: app.browserOpen }
    TagsPanel {
        id: tags; objectName: "testTags"; app: app; bible: testBible; desktop: theme
        embeddedHost: browserHost
        onBrowserRequested: app.browserOpen = true
        onBrowserClosed: app.browserOpen = false
    }
}
)QML", QUrl::fromLocalFile(QStringLiteral(TAGS_QML_DIR) + "/TagsUiHarness.qml"));
#endif
        root.reset(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        window = qobject_cast<QQuickWindow *>(root.get()); QVERIFY(window);
        panel = root->findChild<QObject *>("testTags"); QVERIFY(panel);
        window->requestActivate(); QVERIFY(QTest::qWaitForWindowExposed(window));
        QTRY_VERIFY(window->isActive());
    }
    void cleanup() {
        root.reset(); window = nullptr; panel = nullptr;
        engine.reset(); bible.reset(); session.reset();
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
    }
    void pickerPinnedKeyboardAndLiteralNames() {
        const auto original = bible->tagScope();
        const auto other = scope(2, 4);
        const auto overlapId = bible->createAndAssignTag(other, "Overlap"); QVERIFY(!overlapId.isEmpty());
        bible->selectVerses(1, 3);
        const auto id = bible->createTag(QString::fromUtf8("Café 🌿 <b>literal</b> / flat")); QVERIFY(!id.isEmpty());
        openPicker();
        QCOMPARE(prop("editingScope").toMap(), original);
        QCOMPARE(prop("overlaps").toList().size(), 1);
        QVERIFY(prop("exactIds").toList().isEmpty());
        scope(8, 9); // Selection changes must never retarget the open picker.
        search(QString::fromUtf8("CAFÉ"));
        QCOMPARE(prop("filteredTags").toList().size(), 1);
        key(Qt::Key_Down); key(Qt::Key_Space);
        QCOMPARE(ids(bible->tagsForScope(original)), QStringList{id});
        QVERIFY(bible->selectionTags().isEmpty());
        QCOMPARE(root->property("completed").toInt(), 1);
        QVERIFY(root->property("pinnedAtCompletion").toBool());
        QCOMPARE(variant(root->property("completedScope")).toMap(), original);
        key(Qt::Key_Return);
        QVERIFY(bible->tagsForScope(original).isEmpty());
        search(QString::fromUtf8("New 🕊 <em>tag</em>")); key(Qt::Key_Return);
        QCOMPARE(bible->tagsForScope(original).size(), 1);
        QCOMPARE(bible->tagsForScope(original).first().toMap()["name"].toString(), QString::fromUtf8("New 🕊 <em>tag</em>"));
        search(QString::fromUtf8("new 🕊 <EM>TAG</EM>")); key(Qt::Key_Return);
        QCOMPARE(bible->tagsForScope(original).size(), 1);
        QCOMPARE(bible->tags().size(), 3);
        // Every rendered Text carrying user content explicitly opts out of rich text.
        QList<QQuickItem *> pending{window->contentItem()};
        int literalLabels = 0;
        while (!pending.isEmpty()) {
            auto current = pending.takeLast(); pending.append(current->childItems());
            if (current->property("text").toString().contains("<em>") && current->property("textFormat").isValid()) {
                QCOMPARE(current->property("textFormat").toInt(), 0); ++literalLabels;
            }
        }
        QVERIFY(literalLabels > 0);
        search("query"); key(Qt::Key_Escape);
        QVERIFY(prop("visibleModal").toBool()); QCOMPARE(prop("query").toString(), QString());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("visibleModal").toBool());
        QVERIFY(!prop("scopePinned").toBool()); QCOMPARE(root->property("closed").toInt(), 1);
        QCOMPARE(ids(bible->tagsForScope(other)), QStringList{overlapId});
    }
    void failedFlushAndInvalidNamesDoNotComplete() {
        bible->setNoteText("Keep this note");
        openPicker();
        for (const auto &name : {QString("bad\tname"), QString("bad\nname"), QString("bad\rname"), QString("   ")}) {
            search(name); QVERIFY(!prop("validName").toBool()); invoke("createFromQuery");
        }
        QVERIFY(bible->tags().isEmpty());
        search("Safe"); root->setProperty("flushAllowed", false); key(Qt::Key_Return);
        QVERIFY(bible->tags().isEmpty()); QCOMPARE(root->property("completed").toInt(), 0);
        QVERIFY(!invoke("openBrowser").toBool()); QCOMPARE(prop("page").toString(), QString("picker"));
        QCOMPARE(bible->noteText(), QString("Keep this note"));
        root->setProperty("flushAllowed", true);
        auto store = QSqlDatabase::addDatabase("QSQLITE", "tagsui-failure");
        store.setDatabaseName(bible->dataDirectory() + "/personal.sqlite"); QVERIFY(store.open());
        {
            QSqlQuery sql(store);
            QVERIFY(sql.exec("CREATE TRIGGER fail_tag_save BEFORE INSERT ON tag_assignments BEGIN SELECT RAISE(ABORT,'fixture failure'); END"));
        }
        key(Qt::Key_Return);
        QCOMPARE(bible->tags().size(), 0); QCOMPARE(root->property("completed").toInt(), 0);
        QVERIFY(!prop("message").toString().isEmpty());
        { QSqlQuery sql(store); QVERIFY(sql.exec("DROP TRIGGER fail_tag_save")); }
        store.close(); store = {}; QSqlDatabase::removeDatabase("tagsui-failure");
        key(Qt::Key_Return);
        QCOMPARE(bible->tags().size(), 1); QCOMPARE(root->property("completed").toInt(), 1);
        QCOMPARE(bible->noteText(), QString("Keep this note"));
        invoke("closeForFocusMode"); QTRY_VERIFY(!prop("scopePinned").toBool());
    }
    void pickerRepeatedSpaceAndPeerRefreshKeepHighlight() {
        const auto original = bible->tagScope();
        const auto alpha = bible->createTag("Alpha");
        const auto beta = bible->createTag("Beta");
        QVERIFY(!alpha.isEmpty() && !beta.isEmpty());
        openPicker();
        auto list = item("tagList");
        QCOMPARE(prop("query").toString(), QString());
        key(Qt::Key_Down); key(Qt::Key_Down);
        QCOMPARE(list->property("currentIndex").toInt(), 1);
        QVERIFY(list->hasActiveFocus());
        const auto scroll = list->property("contentY").toReal();
        key(Qt::Key_Space);
        QCOMPARE(ids(bible->tagsForScope(original)), QStringList{beta});
        QCOMPARE(list->property("currentIndex").toInt(), 1);
        QVERIFY(list->hasActiveFocus());
        QCOMPARE(list->property("contentY").toReal(), scroll);
        key(Qt::Key_Space);
        QVERIFY(bible->tagsForScope(original).isEmpty());
        QCOMPARE(list->property("currentIndex").toInt(), 1);
        QVERIFY(list->hasActiveFocus());

        BibleBackend peer;
        QVERIFY(peer.setTag(original, alpha, true));
        QTRY_COMPARE(prop("exactIds").toList(), QVariantList{alpha});
        QCOMPARE(list->property("currentIndex").toInt(), 1);
        QVERIFY(list->hasActiveFocus());
        // Reordering is an identity change, not an index-preserving refresh.
        QVERIFY(peer.renameTag(alpha, "Zulu"));
        QTRY_COMPARE(list->property("currentIndex").toInt(), 0);
        QCOMPARE(prop("filteredTags").toList().first().toMap()["id"].toString(), beta);
        QVERIFY(list->hasActiveFocus());
        key(Qt::Key_Space);
        QVERIFY(ids(bible->tagsForScope(original)).contains(beta));
        key(Qt::Key_Space);
        QCOMPARE(ids(bible->tagsForScope(original)), QStringList{alpha});
    }
    void pickerRefreshPreservesScrolledDelegateAndUnicodeFilter() {
        for (int i = 0; i < 40; ++i)
            QVERIFY(!bible->createTag(QString("Café 🌿 %1").arg(i, 2, 10, QChar('0'))).isEmpty());
        openPicker();
        search(QString::fromUtf8("CAFE\u0301 🌿"));
        QCOMPARE(prop("filteredTags").toList().size(), 40);
        auto list = item("tagList");
        key(Qt::Key_Down);
        for (int i = 0; i < 30; ++i) key(Qt::Key_Down);
        QCOMPARE(list->property("currentIndex").toInt(), 30);
        const auto id = prop("filteredTags").toList()[30].toMap()["id"].toString();
        auto delegate = item("tag:" + id); QVERIFY(delegate);
        delegate->forceActiveFocus();
        const auto scroll = list->property("contentY").toReal();
        QVERIFY(scroll > 0);
        BibleBackend peer;
        QVERIFY(peer.setTag(bible->tagScope(), id, true));
        QTRY_COMPARE(prop("exactIds").toList(), QVariantList{id});
        QCOMPARE(list->property("currentIndex").toInt(), 30);
        QTRY_VERIFY(item("tag:" + id) && item("tag:" + id)->hasActiveFocus());
        QCOMPARE(list->property("contentY").toReal(), scroll);
        QCOMPARE(prop("query").toString(), QString::fromUtf8("CAFE\u0301 🌿"));
        key(Qt::Key_Space);
        QVERIFY(bible->selectionTags().isEmpty());
        QTRY_VERIFY(item("tag:" + id) && item("tag:" + id)->hasActiveFocus());
        QCOMPARE(list->property("contentY").toReal(), scroll);

        search(QString::fromUtf8("CAFE\u0301 🌿 3"));
        QCOMPARE(prop("filteredTags").toList().size(), 10);
        QCOMPARE(list->property("currentIndex").toInt(), 0);
        QVERIFY(item("tagSearch")->hasActiveFocus());
        QVERIFY(peer.renameTag(id, "Outside filter"));
        QTRY_COMPARE(prop("filteredTags").toList().size(), 9);
        QCOMPARE(list->property("currentIndex").toInt(), 0);
        QVERIFY(item("tagSearch")->hasActiveFocus());
        search("No matching tags");
        QCOMPARE(list->property("currentIndex").toInt(), -1);
        QVERIFY(item("tagSearch")->hasActiveFocus());
    }
    void textScaleDoesNotTransformPanel() {
        auto theme = root->findChild<QObject *>("testTheme"); QVERIFY(theme);
        theme->setProperty("textScale", 1.5);
        openPicker();
        QCOMPARE(prop("uiScale").toReal(), 1.5);
        QCOMPARE(qobject_cast<QQuickItem *>(panel)->scale(), 1.0);
        QCOMPARE(item("tagSearch")->property("font").value<QFont>().pixelSize(), 20);
        QCOMPARE(item("tagSearch")->scale(), 1.0);
    }
    void browserAnyAllVersionsAndEnter() {
        const auto first = scope(1, 2);
        const auto a = bible->createAndAssignTag(first, "Alpha");
        const auto b = bible->createAndAssignTag(first, "Beta");
        const auto second = scope(5, 6); QVERIFY(bible->setTag(second, a, true));
        install(kjv); const auto third = scope(9, 10); QVERIFY(bible->setTag(third, a, true)); QVERIFY(bible->setTag(third, b, true));
        QVERIFY(bible->switchVersion(first["versionId"].toString()));
        QVERIFY(invoke("openBrowser").toBool());
        QTRY_VERIFY(item("tagSearch")->hasActiveFocus());
        QVERIFY(rows().isEmpty()); QVERIFY(!prop("allVersions").toBool());
        click("tag:" + a); click("tag:" + b);
        QCOMPARE(rows().size(), 2); QCOMPARE(rows().first().toMap()["sharedCount"].toInt(), 2);
        QVERIFY(!rows().first().toMap()["excerpt"].toString().isEmpty());
        QCOMPARE(rows().first().toMap()["tags"].toList().size(), 2);
        click("tagMatchAll"); QCOMPARE(rows().size(), 1);
        click("tagAllVersions"); QCOMPARE(rows().size(), 2);
        int index = rows().first().toMap()["versionId"] == third["versionId"] ? 0 : 1;
        auto results = item("tagResults"); results->setProperty("currentIndex", index); results->forceActiveFocus();
        root->setProperty("flushAllowed", false); key(Qt::Key_Return);
        QVERIFY(prop("browserVisible").toBool()); QVERIFY(!prop("visibleModal").toBool());
        QCOMPARE(bible->versionId(), first["versionId"].toString());
        root->setProperty("flushAllowed", true); key(Qt::Key_Return);
        QTRY_VERIFY(!prop("browserVisible").toBool());
        QCOMPARE(bible->tagScope(), third);
        QCOMPARE(root->property("completed").toInt(), 0);
    }
    void overlapOpenAndEditRemainExact() {
        const auto original = scope(1, 3);
        const auto other = scope(2, 4);
        const auto id = bible->createAndAssignTag(other, "Overlap");
        QVERIFY(bible->openTaggedPassage(original)); openPicker();
        click("editTagOverlap:" + other["reference"].toString());
        QCOMPARE(prop("editingScope").toMap(), other);
        QCOMPARE(bible->tagScope(), other);
        click("tag:" + id);
        QVERIFY(bible->tagsForScope(other).isEmpty()); QVERIFY(bible->tagsForScope(original).isEmpty());
        QVERIFY(bible->setTag(original, id, true));
        QTRY_COMPARE(prop("overlaps").toList().size(), 1);
        click("openTagOverlap:" + original["reference"].toString());
        QTRY_VERIFY(!prop("visibleModal").toBool());
        QCOMPARE(bible->tagScope(), original);
    }
    void relatedPinnedUnionAndUseCurrent() {
        const auto source = scope(2, 3);
        const auto a = bible->createAndAssignTag(source, "Alpha");
        const auto overlap = scope(3, 4);
        const auto b = bible->createAndAssignTag(overlap, "Beta");
        const auto target = scope(8, 9); QVERIFY(bible->setTag(target, a, true)); QVERIFY(bible->setTag(target, b, true));
        install(kjv); const auto remote = scope(2, 3); QVERIFY(bible->setTag(remote, b, true));
        QVERIFY(bible->openTaggedPassage(source));
        QVERIFY(invoke("openRelated").toBool());
        auto drawer = panel->findChild<QObject *>("relatedDrawer"); QVERIFY(drawer);
        QTRY_VERIFY(drawer->property("opened").toBool());
        QTRY_VERIFY(item("tagSearch")->hasActiveFocus());
        QCOMPARE(drawer->property("edge").toInt(), int(Qt::RightEdge));
        QCOMPARE(drawer->property("height").toReal(), window->height());
        const auto bounds = item("tagsBody")->mapRectToScene(item("tagsBody")->boundingRect());
        QVERIFY(bounds.left() >= window->width() - drawer->property("width").toReal());
        QVERIFY(bounds.right() <= window->width());
        QVERIFY(!panel->findChild<QObject *>("tagsSurface")->property("visible").toBool());
        QVERIFY(item("tagFilterDivider")->isVisible());
        QVERIFY(item("tagFilterDivider")->x() > item("tagMatchAll")->x() + item("tagMatchAll")->width());
        QVERIFY(item("tagSourceVersion")->x() > item("tagFilterDivider")->x());
        QCOMPARE(item("tagSourceVersion")->property("text").toString(), QString("NET only"));
        QCOMPARE(prop("sourceIds").toList().size(), 2);
        QCOMPARE(rows().size(), 1); QCOMPARE(rows().first().toMap()["start"].toInt(), 8);
        item("tagResults")->forceActiveFocus(); key(Qt::Key_Return);
        QCOMPARE(bible->tagScope(), target); QVERIFY(prop("visibleModal").toBool());
        QCOMPARE(prop("relatedSource").toMap(), source);
        QVERIFY(drawer->property("opened").toBool());
        QCOMPARE(item("tagsBody")->window(), window);
        click("tagAllVersions"); QCOMPARE(rows().size(), 2);
        click("tagAllVersions"); QCOMPARE(rows().size(), 2);
        QVERIFY(!item("tagSourceVersion")->property("chosen").toBool());
        QVERIFY(item("tagAllVersions")->property("chosen").toBool());
        const int remoteIndex = rows().first().toMap()["versionId"] == remote["versionId"] ? 0 : 1;
        item("tagResults")->setProperty("currentIndex", remoteIndex);
        item("tagResults")->forceActiveFocus(); key(Qt::Key_Return);
        QCOMPARE(bible->tagScope(), remote);
        QVERIFY(drawer->property("opened").toBool());
        QCOMPARE(prop("relatedSource").toMap(), source);
        QCOMPARE(item("tagSourceVersion")->property("text").toString(), QString("NET only"));
        click("tagSourceVersion"); QCOMPARE(rows().size(), 1); // Still NET source version, despite current KJV.
        click("tagUseCurrent"); QCOMPARE(prop("relatedSource").toMap(), remote);
        QCOMPARE(item("tagSourceVersion")->property("text").toString(), bible->versionAbbreviation() + " only");
        QCOMPARE(prop("filterIds").toList(), QVariantList{b}); QVERIFY(rows().isEmpty());
        click("tagAllVersions"); QCOMPARE(rows().size(), 2);
        invoke("closeForFocusMode"); QTRY_VERIFY(!prop("visibleModal").toBool());
        QVERIFY(bible->openTaggedPassage(target));
        QVERIFY(invoke("openRelated").toBool());
        QTRY_VERIFY(drawer->property("opened").toBool());
        QCOMPARE(prop("relatedSource").toMap(), target);
        QVERIFY(!prop("allVersions").toBool());
    }
    void managementConfirmationsAndPeerChanges() {
        const auto original = bible->tagScope();
        const auto a = bible->createAndAssignTag(original, "Alpha");
        const auto b = bible->createAndAssignTag(original, "Beta");
        bible->setNoteText("Preserve note"); QVERIFY(bible->toggleBookmark());
        QVERIFY(invoke("openBrowser").toBool()); QTRY_VERIFY(item("tagSearch")->hasActiveFocus());
        click("tag:" + a); click("tag:" + b); QCOMPARE(rows().size(), 1);
        invoke("manage"); click("tag:" + a);
        item("tagRenameInput")->setProperty("text", QString::fromUtf8("Αγάπη <b>🌿</b>"));
        click("renameTag"); QVERIFY(!prop("confirmation").toMap().isEmpty());
        QCOMPARE(bible->tags().first().toMap()["name"].toString(), QString("Alpha"));
        click("cancelTagManagement");
        QCOMPARE(bible->tags().first().toMap()["name"].toString(), QString("Alpha"));
        click("renameTag"); click("confirmTagManagement");
        QVERIFY(ids(bible->tags()).contains(a));
        QCOMPARE(bible->tagsForScope(original).size(), 2);
        click("mergeTarget:" + b); click("mergeTags");
        QCOMPARE(bible->tags().size(), 2); click("confirmTagManagement");
        QCOMPARE(ids(bible->tagsForScope(original)), QStringList{b});
        QCOMPARE(prop("filterIds").toList(), QVariantList{b});
        QCOMPARE(bible->noteText(), QString("Preserve note")); QVERIFY(bible->selectionBookmarked());
        click("tag:" + b); click("deleteTag"); click("cancelTagManagement");
        QCOMPARE(bible->tags().size(), 1);
        // A second window deletes the selected tag while its confirmation is open.
        click("deleteTag");
        BibleBackend peer;
        QVERIFY(peer.renameTag(b, "Peer renamed"));
        QTRY_VERIFY(prop("confirmation").toMap().isEmpty());
        QCOMPARE(prop("filterIds").toList(), QVariantList{b});
        click("deleteTag");
        QVERIFY(peer.deleteTag(b));
        QTRY_VERIFY(prop("confirmation").toMap().isEmpty());
        QTRY_VERIFY(prop("filterIds").toList().isEmpty());
        QCOMPARE(prop("managedId").toString(), QString());
        invoke("back"); QVERIFY(rows().isEmpty());
        QCOMPARE(bible->noteText(), QString("Preserve note")); QVERIFY(bible->selectionBookmarked());
    }
    void removedVersionInvalidatesPinnedScope() {
        const auto original = bible->tagScope();
        const auto id = bible->createAndAssignTag(original, "Retained");
        install(kjv); QVERIFY(bible->openTaggedPassage(original));
        openPicker();
        QVERIFY(bible->removeVersion(original["versionId"].toString(), false));
        QTRY_VERIFY(!prop("scopeValid").toBool());
        invoke("toggleTag", id); search("Wrong version"); invoke("createFromQuery");
        QCOMPARE(root->property("completed").toInt(), 0);
        QCOMPARE(bible->tags().size(), 1); QVERIFY(bible->selectionTags().isEmpty());
        invoke("closeForFocusMode"); QTRY_VERIFY(!prop("scopePinned").toBool());
        QVERIFY(!invoke("open", original).toBool());
    }
    void mainIntegrationVisualCompletionAndDraft() {
        root.reset(); engine.reset(); window = nullptr; panel = nullptr;
        Desktop desktop;
        VersionCatalog catalog;
        engine = std::make_unique<QQmlEngine>();
        connect(engine.get(), &QQmlEngine::warnings, this, [this](const QList<QQmlError> &errors) {
            for (const auto &error : errors) warnings << error.toString();
        });
        engine->rootContext()->setContextProperty("bible", bible.get());
        engine->rootContext()->setContextProperty("desktop", &desktop);
        engine->rootContext()->setContextProperty("catalog", &catalog);
        QQmlComponent component(engine.get(), QUrl("qrc:/qml/Main.qml"));
        root.reset(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        window = qobject_cast<QQuickWindow *>(root.get()); QVERIFY(window);
        panel = root->findChild<QObject *>("tagsUi"); QVERIFY(panel);
        window->requestActivate(); QVERIFY(QTest::qWaitForWindowExposed(window));
        QTRY_VERIFY(!root->property("restoringPosition").toBool());
        const auto original = scope(1, 3);
        root->setProperty("verseMode", true);
        QVERIFY(QMetaObject::invokeMethod(root.get(), "openPassageTags", Q_ARG(QVariant, original)));
        QTRY_VERIFY(prop("scopePinned").toBool());
        QVERIFY(root->property("verseMode").toBool());
        search("Main integration"); key(Qt::Key_Return);
        QCOMPARE(bible->tagsForScope(original).size(), 1);
        QVERIFY(!root->property("verseMode").toBool());
        QCOMPARE(bible->selectedStart(), 1); QCOMPARE(bible->selectedEnd(), 3);
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("visibleModal").toBool());
        QCOMPARE(bible->selectedStart(), bible->cursorVerse());
        QCOMPARE(bible->selectedEnd(), bible->cursorVerse());
        // Opening and canceling alone must leave Visual mode and its full range.
        scope(5, 7); root->setProperty("verseMode", true);
        QVERIFY(QMetaObject::invokeMethod(root.get(), "openPassageTags", Q_ARG(QVariant, bible->tagScope())));
        QTRY_VERIFY(item("tagSearch")->hasActiveFocus()); key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("visibleModal").toBool()); QVERIFY(root->property("verseMode").toBool());
        QCOMPARE(bible->selectedStart(), 5); QCOMPARE(bible->selectedEnd(), 7);
        root->setProperty("verseMode", false);
        // Stage an unsaved editor draft without its normal eager-save handler.
        // Opening the picker must run Main's actual flush before moving focus.
        root->setProperty("syncingNote", true);
        item("noteEditor")->setProperty("text", "Unsaved original draft <b>literal</b>");
        root->setProperty("syncingNote", false);
        QVERIFY(bible->noteText().isEmpty());
        QVERIFY(QMetaObject::invokeMethod(root.get(), "openPassageTags", Q_ARG(QVariant, bible->tagScope())));
        QTRY_VERIFY(prop("scopePinned").toBool());
        QCOMPARE(bible->noteText(), QString("Unsaved original draft <b>literal</b>"));
        invoke("closeForFocusMode");
        root.reset(); engine.reset(); window = nullptr; panel = nullptr;
    }
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QQuickStyle::setStyle("Basic");
    TagsUiTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_tagsui.moc"
