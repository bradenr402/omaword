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
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <archive.h>
#include <archive_entry.h>
#include "biblebackend.h"
#include "desktop.h"
#include "soundeffects.h"
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
        if (qstrcmp(method, "open") == 0)
            return invoke(method, QVariant());
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
    void key(Qt::Key key, Qt::KeyboardModifiers mods = Qt::NoModifier) { QTest::keyClick(window, key, mods); QCoreApplication::processEvents(); }
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
    property var pendingActionCollapse: null
    property var menuEntries: []
    property string menuKind: ""
    property var copied: null
    property var ranged: null
    function showContextMenu(kind, entries, x, y) { menuKind = kind; menuEntries = entries.map(function(entry) { return entry.separator ? "|" : entry.id }) }
    function copyScope(scope, plain) { copied = scope }
    function toggleScopeDesk(scope) {}
    function openRangeEditor(scope, kind) { ranged = scope }
    function flushNote() { return flushAllowed }
    function savePosition() {}
    function beginJump() { return null }
    function endJump(from) {}
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
    TagsPanel { id: tags; objectName: "testTags"; app: app; bible: testBible; desktop: theme }
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
        // One esc closes the picker, query and all.
        search("query"); key(Qt::Key_Escape); QTRY_VERIFY(!prop("visibleModal").toBool());
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
        QVERIFY(!invoke("openManage").toBool()); QCOMPARE(prop("page").toString(), QString("picker"));
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
        {
            auto *rowBackground = delegate->property("background").value<QQuickItem *>(); QVERIFY(rowBackground);
            QCOMPARE(rowBackground->property("border").value<QObject *>()->property("width").toReal(), 0.0);
            QTRY_VERIFY(rowBackground->property("color").value<QColor>().alpha() > 0);
        }
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
    void modalPickerWithEmbeddedHostKeepsSelection() {
        const auto first = scope(1, 2);
        const auto a = bible->createAndAssignTag(first, "Alpha");
        const auto b = bible->createTag("Beta");
        QVERIFY(invoke("open").toBool());
        QTRY_VERIFY(item("tagSearch")->hasActiveFocus());
        QCOMPARE(prop("page").toString(), QString("picker"));
        QVERIFY(prop("visibleModal").toBool());
        QCOMPARE(prop("hostKind").toString(), QString("picker"));
        QVERIFY(panel->findChild<QObject *>("tagsSurface")->property("modal").toBool());
        QVERIFY(prop("scopePinned").toBool());
        QVERIFY(item("tag:" + a)->property("chosen").toBool());
        QVERIFY(!item("tag:" + b)->property("chosen").toBool());
        QVERIFY(!item("tagResults")->isVisible());
        click("tag:" + a); click("tag:" + b);
        QCOMPARE(ids(bible->tagsForScope(first)), QStringList{b});
        QVERIFY(rows().isEmpty());
        const auto second = scope(5, 6);
        root->setProperty("flushAllowed", false); invoke("followSelection");
        QCOMPARE(prop("editingScope").toMap(), first);
        root->setProperty("flushAllowed", true); invoke("followSelection");
        QCOMPARE(prop("editingScope").toMap(), first);
        QCOMPARE(prop("exactIds").toList(), QVariantList{b});
        invoke("toggleTag", a);
        QCOMPARE(bible->tagsForScope(first).size(), 2);
        QVERIFY(bible->tagsForScope(second).isEmpty());
        // No Done button: a click on the scrim closes the picker (esc too).
        QVERIFY(!item("tagsDone"));
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, QPoint(4, 4));
        QTRY_VERIFY(!panel->findChild<QObject *>("tagsSurface")->property("visible").toBool());
        QVERIFY(invoke("open").toBool());
        QTRY_VERIFY(item("tagSearch")->hasActiveFocus());
        QCOMPARE(prop("editingScope").toMap(), second);
        QVERIFY(prop("exactIds").toList().isEmpty());
        search("Literal <b>🌿</b>"); key(Qt::Key_Return);
        search("Second / literal"); key(Qt::Key_Return);
        QCOMPARE(bible->tagsForScope(second).size(), 2);
        root->setProperty("pendingActionCollapse", first);
        scope(8, 9); invoke("followSelection");
        QCOMPARE(prop("editingScope").toMap(), second);
        invoke("close");
        QTRY_VERIFY(!prop("visibleModal").toBool());
        QVERIFY(!prop("scopePinned").toBool());
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
    void emptyEditorPromptsAndRelatedFindsUnrelatedTags() {
        bible->clearSelection();
        QVERIFY(bible->tagScope().isEmpty());
        QVERIFY(invoke("open").toBool());
        QTRY_VERIFY(item("tagSearch")->hasActiveFocus());
        QVERIFY(item("tagScopePrompt")->isVisible());
        QCOMPARE(item("tagScopePrompt")->property("text").toString(), QString("Select a passage to add or remove tags."));
        search("Cannot assign yet"); key(Qt::Key_Return);
        QVERIFY(bible->tags().isEmpty());
        QVERIFY(!item("createPassageTag")->isEnabled());
        const auto initial = scope(2, 3);
        root->setProperty("flushAllowed", false); invoke("followSelection");
        QVERIFY(prop("editingScope").toMap().isEmpty());
        root->setProperty("flushAllowed", true); invoke("followSelection");
        QVERIFY(prop("editingScope").toMap().isEmpty());
        QVERIFY(invoke("open").toBool());
        QCOMPARE(prop("editingScope").toMap(), initial);
        QVERIFY(!item("tagScopePrompt")->isVisible());
        QVERIFY(prop("scopePinned").toBool());
        invoke("close");
        QTRY_VERIFY(!prop("visibleModal").toBool());
    }
private:
    QStringList refs() const {
        QStringList result;
        for (const auto &row : prop("visibleResults").toList()) result << row.toMap().value("reference").toString();
        return result;
    }
    QStringList chipIds() const {
        QStringList result;
        for (const auto &chip : prop("rootChips").toList()) result << chip.toMap().value("id").toString();
        return result;
    }
    void openTagsTab() {
        QVERIFY(invoke("openRelated").toBool());
        QTRY_VERIFY(panel->findChild<QObject *>("relatedDrawer")->property("opened").toBool());
        QVERIFY(QMetaObject::invokeMethod(panel, "setStudyTab", Q_ARG(QVariant, QString("related")), Q_ARG(QVariant, QVariant())));
        QTRY_VERIFY(item("tagResults")->hasActiveFocus());
    }
private slots:
    // Study → Tags: the passage's tags as chips (exact first, then overlapping),
    // other passages ranked by shared tags, chips toggle/solo/restore, a for
    // every installed version, never the passage itself or its overlaps.
    void studyTagsChipsRankingAndVersions() {
        const auto source = scope(2, 3);
        const auto a = bible->createAndAssignTag(source, "Alpha");
        const auto overlap = scope(3, 4);
        const auto b = bible->createAndAssignTag(overlap, "Beta");
        const auto both = scope(8, 9); QVERIFY(bible->setTag(both, a, true)); QVERIFY(bible->setTag(both, b, true));
        const auto alphaOnly = scope(5, 5); QVERIFY(bible->setTag(alphaOnly, a, true));
        const auto betaOnly = scope(11, 11); QVERIFY(bible->setTag(betaOnly, b, true));
        install(kjv); const auto remote = scope(2, 3); QVERIFY(bible->setTag(remote, b, true));
        QVERIFY(bible->openTaggedPassage(source));
        openTagsTab();
        auto drawer = panel->findChild<QObject *>("relatedDrawer");
        // Header: title and the reader's passage.
        QCOMPARE(item("studyTitle")->property("text").toString(), QString("Study"));
        QCOMPARE(item("studyPassage")->property("text").toString(), source["reference"].toString() + " · NET");
        QCOMPARE(prop("relatedSource").toMap(), source);
        QCOMPARE(chipIds(), (QStringList{a, b}));
        QVERIFY(!prop("rootChips").toList()[0].toMap()["overlap"].toBool());
        QVERIFY(prop("rootChips").toList()[1].toMap()["overlap"].toBool());
        // Ranked by shared count, then canonical order; never the source or its overlap.
        QCOMPARE(refs(), (QStringList{both["reference"].toString(), alphaOnly["reference"].toString(), betaOnly["reference"].toString()}));
        QCOMPARE(item("studyTagCount")->property("text").toString(), QString("3 passages"));
        QCOMPARE(item("studyChipCount:" + a)->property("text").toString(), QString("2"));
        QCOMPARE(item("studyChipCount:" + b)->property("text").toString(), QString("2"));
        // The body sits inside the drawer's padding.
        const auto bounds = item("studyBody")->mapRectToScene(item("studyBody")->boundingRect());
        QCOMPARE(bounds.right(), window->width() - 24);
        QCOMPARE(bounds.top(), 24);
        // k from the first row climbs to the chips; space toggles, shift space solos.
        key(Qt::Key_K); QTRY_VERIFY(item("studyChip:" + a)->hasActiveFocus());
        key(Qt::Key_Space);
        QCOMPARE(prop("onIds").toList(), QVariantList{b});
        QCOMPARE(refs(), (QStringList{both["reference"].toString(), betaOnly["reference"].toString()}));
        key(Qt::Key_Space); QCOMPARE(prop("onIds").toList().size(), 2);
        QTest::keyClick(window, Qt::Key_Space, Qt::ShiftModifier);
        QCOMPARE(prop("onIds").toList(), QVariantList{a});
        // Ranking counts only chips that are on: a tie, so canonical order.
        QCOMPARE(refs(), (QStringList{alphaOnly["reference"].toString(), both["reference"].toString()}));
        QTest::keyClick(window, Qt::Key_Space, Qt::ShiftModifier); // Soloing again restores all.
        QCOMPARE(prop("onIds").toList().size(), 2);
        key(Qt::Key_L); QVERIFY(item("studyChip:" + b)->hasActiveFocus());
        QCOMPARE(prop("studyTab").toString(), QString("related"));
        key(Qt::Key_Space); key(Qt::Key_H); key(Qt::Key_Space);
        QVERIFY(prop("onIds").toList().isEmpty());
        QCOMPARE(item("studyTagsEmpty")->property("text").toString(), QString("Turn on a tag to see passages."));
        key(Qt::Key_0); QCOMPARE(prop("onIds").toList().size(), 2); // 0 turns every chip back on.
        // Shift click solos too.
        const auto chip = item("studyChip:" + b);
        QTest::mouseClick(window, Qt::LeftButton, Qt::ShiftModifier, chip->mapToScene(QPointF(chip->width() / 2, chip->height() / 2)).toPoint());
        QTRY_COMPARE(prop("onIds").toList(), QVariantList{b});
        key(Qt::Key_0); QCOMPARE(prop("onIds").toList().size(), 2);
        key(Qt::Key_J); QVERIFY(item("tagResults")->hasActiveFocus());
        // a: every installed version, shown in the count line and on other-version rows.
        key(Qt::Key_A); QVERIFY(prop("allVersions").toBool());
        QCOMPARE(refs().size(), 4);
        QVERIFY(item("studyTagCount")->property("text").toString().endsWith(" · all versions"));
        QString remoteAbbreviation;
        for (const auto &version : bible->versions())
            if (version.toMap()["id"] == remote["versionId"]) remoteAbbreviation = version.toMap()["abbreviation"].toString();
        QVERIFY(!remoteAbbreviation.isEmpty());
        bool remoteRow = false;
        for (int i = 0; i < 4; ++i) {
            QTRY_VERIFY(item("tagResultReference:" + QString::number(i)));
            const auto text = item("tagResultReference:" + QString::number(i))->property("text").toString();
            if (text == remote["reference"].toString() + " · " + remoteAbbreviation) remoteRow = true;
            else QVERIFY2(!text.contains(" · "), qPrintable(text)); // Rows in the reading version stay plain.
        }
        QVERIFY(remoteRow);
        key(Qt::Key_A); QVERIFY(!prop("allVersions").toBool()); QCOMPARE(refs().size(), 3);
        // Enter opens the row; Study stays, and keeps its root while it has focus.
        key(Qt::Key_Return);
        QCOMPARE(bible->tagScope(), both);
        QVERIFY(drawer->property("opened").toBool());
        invoke("followSelection");
        QCOMPARE(prop("relatedSource").toMap(), source);
        QCOMPARE(item("studyPassage")->property("text").toString(), both["reference"].toString() + " · NET"); // The header is live.
        // Reopened, Study follows the reader again.
        invoke("closeForFocusMode"); QTRY_VERIFY(!drawer->property("visible").toBool());
        openTagsTab();
        QCOMPARE(prop("relatedSource").toMap(), both);
        QCOMPARE(chipIds(), (QStringList{a, b}));
        QVERIFY(!refs().contains(both["reference"].toString()));
        invoke("closeForFocusMode"); QTRY_VERIFY(!drawer->property("visible").toBool());
        // Reopening roots at the reader's passage with every chip on.
        QVERIFY(bible->openTaggedPassage(alphaOnly));
        openTagsTab();
        QCOMPARE(prop("relatedSource").toMap(), alphaOnly);
        QCOMPARE(chipIds(), QStringList{a});
        QVERIFY(!prop("allVersions").toBool());
    }
    void studyTagsSearchMenuTagKeyAndEmptyStates() {
        const auto source = scope(2, 3);
        const auto a = bible->createAndAssignTag(source, "Alpha");
        const auto b = bible->createTag("Beta grace");
        const auto first = scope(8, 9); QVERIFY(bible->setTag(first, a, true)); QVERIFY(bible->setTag(first, b, true));
        const auto second = scope(11, 11); QVERIFY(bible->setTag(second, a, true));
        QVERIFY(bible->openTaggedPassage(source));
        openTagsTab();
        QCOMPARE(refs().size(), 2);
        // / shows the field in place of the count line; tag names match.
        QVERIFY(!item("studySearch")->isVisible());
        key(Qt::Key_Slash); QTRY_VERIFY(item("studySearch")->hasActiveFocus());
        QVERIFY(!item("studyTagCount")->isVisible());
        for (const char c : QByteArray("grace")) QTest::keyClick(window, c);
        QTRY_COMPARE(refs(), QStringList{first["reference"].toString()});
        // enter keeps the query and returns to the results.
        key(Qt::Key_Return); QTRY_VERIFY(item("tagResults")->hasActiveFocus());
        QVERIFY(!item("studySearch")->isVisible());
        QCOMPARE(item("studyTagCount")->property("text").toString(), QString::fromUtf8("1 of 2 passages · “grace”"));
        key(Qt::Key_Slash); QTRY_VERIFY(item("studySearch")->hasActiveFocus());
        for (const char c : QByteArray("zz")) QTest::keyClick(window, c);
        QTRY_VERIFY(refs().isEmpty());
        QCOMPARE(item("studyTagsEmpty")->property("text").toString(), QString::fromUtf8("No passages match “gracezz”."));
        key(Qt::Key_Down); QTRY_VERIFY(item("tagResults")->hasActiveFocus());
        // esc closes, as in Annotations; reopening clears the query.
        key(Qt::Key_Escape); QTRY_VERIFY(!panel->findChild<QObject *>("relatedDrawer")->property("visible").toBool());
        openTagsTab(); QCOMPARE(prop("studyQuery").toString(), QString());
        QCOMPARE(refs().size(), 2);
        // The shared row menu (Open, Copy, Copy plain text, Desk), then Edit tags…, Edit range. No delete.
        key(Qt::Key_Menu);
        QCOMPARE(root->property("menuKind").toString(), QString("study"));
        QCOMPARE(variant(root->property("menuEntries")).toStringList(), (QStringList{"open", "copy", "copyPlain", "desk", "|", "tags", "editRange"}));
        key(Qt::Key_Delete); QCOMPARE(bible->tagsForScope(first).size(), 2);
        // y copies; shift E edits the row's range.
        key(Qt::Key_Y); QCOMPARE(variant(root->property("copied")).toMap()["start"], first["start"]);
        key(Qt::Key_E, Qt::ShiftModifier); QCOMPARE(variant(root->property("ranged")).toMap()["start"], first["start"]);
        // t opens the picker on the row's range; Study stays open beneath it.
        key(Qt::Key_T); QTRY_VERIFY(item("tagSearch")->hasActiveFocus());
        QCOMPARE(prop("editingScope").toMap(), first);
        QVERIFY(panel->findChild<QObject *>("relatedDrawer")->property("visible").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!panel->findChild<QObject *>("tagsSurface")->property("visible").toBool());
        QVERIFY(panel->findChild<QObject *>("relatedDrawer")->property("visible").toBool());
        invoke("closeForFocusMode");
        QTRY_VERIFY(!panel->findChild<QObject *>("relatedDrawer")->property("visible").toBool());
        // Tags, but no other passages: the versions hint once another version is installed.
        const auto lonely = scope(6, 6); bible->createAndAssignTag(lonely, "Lonely");
        openTagsTab();
        QCOMPARE(item("studyTagsEmpty")->property("text").toString(), QString("No other passages are tagged Lonely."));
        QVERIFY(!item("studyTagsVersionsHint")->isVisible());
        invoke("closeForFocusMode"); QTRY_VERIFY(!panel->findChild<QObject *>("relatedDrawer")->property("visible").toBool());
        install(kjv); QVERIFY(bible->openTaggedPassage(lonely));
        openTagsTab();
        QVERIFY(item("studyTagsVersionsHint")->isVisible());
        QVERIFY(item("studyTagsVersionsHint")->property("text").toString().contains("include&nbsp;other&nbsp;versions"));
        invoke("closeForFocusMode"); QTRY_VERIFY(!panel->findChild<QObject *>("relatedDrawer")->property("visible").toBool());
        // No tags: the passage, and t (or the button) adds some.
        const auto bare = scope(12, 12);
        openTagsTab();
        QCOMPARE(item("studyTagsEmpty")->property("text").toString(), QString("No tags on ") + bare["reference"].toString() + ".");
        QVERIFY(item("studyAddTags")->isVisible());
        QVERIFY(!item("studyChips")->isVisible());
        key(Qt::Key_T); QTRY_VERIFY(item("tagSearch")->hasActiveFocus());
        QCOMPARE(prop("editingScope").toMap(), bare);
        key(Qt::Key_Escape); QTRY_VERIFY(!panel->findChild<QObject *>("tagsSurface")->property("visible").toBool());
        invoke("closeForFocusMode");
    }
    // A tag chip clicked elsewhere opens Study → Tags with that chip soloed.
    void relatedForTagSolosChip() {
        const auto source = scope(2, 3);
        const auto a = bible->createAndAssignTag(source, "Alpha");
        const auto b = bible->createAndAssignTag(source, "Beta");
        const auto target = scope(8, 9); QVERIFY(bible->setTag(target, b, true));
        bible->openChapter(43, 4); bible->selectVerses(1, 1); // The supplied source wins.
        QVariant opened;
        QVERIFY(QMetaObject::invokeMethod(panel, "openRelatedForTag", Q_RETURN_ARG(QVariant, opened), Q_ARG(QVariant, b), Q_ARG(QVariant, source)));
        QVERIFY(variant(opened).toBool());
        QTRY_VERIFY(panel->findChild<QObject *>("relatedDrawer")->property("opened").toBool());
        QCOMPARE(prop("studyTab").toString(), QString("related"));
        QCOMPARE(prop("relatedSource").toMap(), source);
        QCOMPARE(prop("onIds").toList(), QVariantList{b});
        QCOMPARE(refs(), QStringList{target["reference"].toString()});
        Q_UNUSED(a)
    }
    // The shared header follows the reader: the selection, else its chapter.
    void studyHeaderFollowsReader() {
        scope(2, 3);
        QVERIFY(invoke("openRelated").toBool());
        QTRY_VERIFY(panel->findChild<QObject *>("relatedDrawer")->property("opened").toBool());
        for (const QString tab : {"crossrefs", "footnotes", "related"}) {
            QVERIFY(QMetaObject::invokeMethod(panel, "setStudyTab", Q_ARG(QVariant, tab), Q_ARG(QVariant, QVariant())));
            QVERIFY(item("studyPassage")->isVisible());
            QCOMPARE(item("studyPassage")->property("text").toString(), QString::fromUtf8("John 3:2–3 · NET"));
        }
        bible->selectVerses(7, 7);
        QCOMPARE(item("studyPassage")->property("text").toString(), QString("John 3:7 · NET"));
        bible->clearSelection();
        QCOMPARE(item("studyPassage")->property("text").toString(), QString("John 3 · NET"));
        QCOMPARE(item("studyPassage")->property("font").value<QFont>().pixelSize(), 13);
        QCOMPARE(item("studyPassage")->property("color").value<QColor>(), QColor("#aaaaaa"));
    }
    void managementConfirmationsAndPeerChanges() {
        const auto original = bible->tagScope();
        const auto a = bible->createAndAssignTag(original, "Alpha");
        const auto b = bible->createAndAssignTag(original, "Beta");
        bible->setNoteText("Preserve note"); QVERIFY(bible->toggleBookmark());
        QVERIFY(invoke("open").toBool()); QTRY_VERIFY(item("tagSearch")->hasActiveFocus());
        QVERIFY(rows().isEmpty());
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
        QCOMPARE(bible->noteText(), QString("Preserve note")); QVERIFY(bible->selectionBookmarked());
        click("tag:" + b); click("deleteTag"); click("cancelTagManagement");
        QCOMPARE(bible->tags().size(), 1);
        // A second window deletes the selected tag while its confirmation is open.
        click("deleteTag");
        BibleBackend peer;
        QVERIFY(peer.renameTag(b, "Peer renamed"));
        QTRY_VERIFY(prop("confirmation").toMap().isEmpty());
        click("deleteTag");
        QVERIFY(peer.deleteTag(b));
        QTRY_VERIFY(prop("confirmation").toMap().isEmpty());
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
    void mainIntegrationPinnedScopeAcrossHostChanges() {
        install(kjv);
        QVERIFY(bible->switchVersion("engnet"));
        root.reset(); engine.reset(); window = nullptr; panel = nullptr;
        Desktop desktop;
        SoundEffects sounds(nullptr, session->filePath("sound.ini"), false);
        VersionCatalog catalog;
        // QVERIFY/QCOMPARE can return early: destroy QML before its context objects.
        const auto cleanupQml = qScopeGuard([this] {
            root.reset(); engine.reset(); window = nullptr; panel = nullptr;
        });
        engine = std::make_unique<QQmlEngine>();
        connect(engine.get(), &QQmlEngine::warnings, this, [this](const QList<QQmlError> &errors) {
            for (const auto &error : errors) warnings << error.toString();
        });
        engine->rootContext()->setContextProperty("bible", bible.get());
        engine->rootContext()->setContextProperty("desktop", &desktop);
        engine->rootContext()->setContextProperty("sounds", &sounds);
        engine->rootContext()->setContextProperty("catalog", &catalog);
        QQmlComponent component(engine.get(), QUrl("qrc:/qml/Main.qml"));
        root.reset(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        window = qobject_cast<QQuickWindow *>(root.get()); QVERIFY(window);
        panel = root->findChild<QObject *>("tagsUi"); QVERIFY(panel);
        window->requestActivate(); QVERIFY(QTest::qWaitForWindowExposed(window));
        QTRY_VERIFY(!root->property("restoringPosition").toBool());

        const auto original = scope(1, 3);
        const auto first = bible->createTag("First");
        const auto second = bible->createTag("Second");
        root->setProperty("verseMode", true);
        QVERIFY(QMetaObject::invokeMethod(root.get(), "openPassageTags", Q_ARG(QVariant, original)));
        auto picker = panel->findChild<QObject *>("tagsSurface"); QVERIFY(picker);
        auto annotations = root->findChild<QObject *>("savedDrawer"); QVERIFY(annotations);
        QTRY_VERIFY(picker->property("opened").toBool());
        QVERIFY(picker->property("modal").toBool());
        QCOMPARE(prop("hostKind").toString(), QString("picker"));
        QVERIFY(prop("scopePinned").toBool());
        QVERIFY(!annotations->property("visible").toBool());
        QTRY_VERIFY(item("tagSearch")->hasActiveFocus());
        invoke("toggleTag", first);
        QVERIFY(!root->property("verseMode").toBool());
        QVERIFY(!variant(root->property("pendingActionCollapse")).toMap().isEmpty());

        const auto changed = scope(5, 7);
        QVERIFY(variant(root->property("pendingActionCollapse")).toMap().isEmpty());
        QCOMPARE(prop("editingScope").toMap(), original);
        QVERIFY(picker->property("opened").toBool());
        QVERIFY(prop("scopePinned").toBool());
        invoke("toggleTag", second);
        QCOMPARE(bible->tagsForScope(original).size(), 2);
        QVERIFY(bible->tagsForScope(changed).isEmpty());
        invoke("close");
        QTRY_VERIFY(!picker->property("visible").toBool());
        QVERIFY(!prop("scopePinned").toBool());
        QVERIFY(!annotations->property("visible").toBool());
        QCOMPARE(bible->tagScope(), changed); // Closing cannot collapse the new range.

        QVERIFY(QMetaObject::invokeMethod(root.get(), "openPassageTags", Q_ARG(QVariant, QVariant())));
        QTRY_VERIFY(item("tagSearch")->hasActiveFocus());
        QCOMPARE(prop("editingScope").toMap(), changed);
        QVERIFY(bible->switchVersion("eng-kjv"));
        const auto otherVersion = scope(8, 9);
        QCOMPARE(prop("editingScope").toMap(), changed);
        QTRY_VERIFY(picker->property("opened").toBool()); // After its fade-in.
        QVERIFY(prop("scopePinned").toBool());
        QVERIFY(!annotations->property("visible").toBool());
        QVERIFY(prop("scopeValid").toBool());
        invoke("toggleTag", first);
        QCOMPARE(ids(bible->tagsForScope(changed)), QStringList{first});
        QVERIFY(bible->tagsForScope(otherVersion).isEmpty());

        BibleBackend peer;
        QVERIFY(peer.removeVersion(changed["versionId"].toString(), false));
        QTRY_VERIFY(!prop("scopeValid").toBool());
        QCOMPARE(prop("editingScope").toMap(), changed);
        invoke("toggleTag", second);
        search("Blocked after removal"); invoke("createFromQuery");
        QCOMPARE(bible->tags().size(), 2);
        QVERIFY(bible->tagsForScope(otherVersion).isEmpty());
        QVERIFY(!item("createPassageTag")->isEnabled());
        invoke("close");
        QTRY_VERIFY(!picker->property("visible").toBool());
        QVERIFY(!prop("scopePinned").toBool());
        QVERIFY(!annotations->property("visible").toBool());
        QCOMPARE(bible->tagScope(), otherVersion);
        QVERIFY(QMetaObject::invokeMethod(root.get(), "openPassageTags", Q_ARG(QVariant, QVariant())));
        QTRY_VERIFY(item("tagSearch")->hasActiveFocus());
        QCOMPARE(prop("editingScope").toMap(), otherVersion);
        invoke("close");
    }
    void mainIntegrationVisualCompletionAndDraft() {
        root.reset(); engine.reset(); window = nullptr; panel = nullptr;
        Desktop desktop;
        SoundEffects sounds(nullptr, session->filePath("sound.ini"), false);
        VersionCatalog catalog;
        const auto cleanupQml = qScopeGuard([this] {
            root.reset(); engine.reset(); window = nullptr; panel = nullptr;
        });
        engine = std::make_unique<QQmlEngine>();
        connect(engine.get(), &QQmlEngine::warnings, this, [this](const QList<QQmlError> &errors) {
            for (const auto &error : errors) warnings << error.toString();
        });
        engine->rootContext()->setContextProperty("bible", bible.get());
        engine->rootContext()->setContextProperty("desktop", &desktop);
        engine->rootContext()->setContextProperty("sounds", &sounds);
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
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("scopePinned").toBool());
        QCOMPARE(bible->selectedStart(), bible->cursorVerse());
        QCOMPARE(bible->selectedEnd(), bible->cursorVerse());
        // Opening and canceling alone must leave Visual mode and its full range.
        scope(5, 7); root->setProperty("verseMode", true);
        QVERIFY(QMetaObject::invokeMethod(root.get(), "openPassageTags", Q_ARG(QVariant, bible->tagScope())));
        QTRY_VERIFY(item("tagSearch")->hasActiveFocus()); key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("scopePinned").toBool()); QVERIFY(root->property("verseMode").toBool());
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
    }
private:
    std::unique_ptr<Desktop> mainDesktop;
    std::unique_ptr<SoundEffects> mainSounds;
    std::unique_ptr<VersionCatalog> mainCatalog;
    bool loadMain() {
        root.reset(); engine.reset(); window = nullptr; panel = nullptr;
        mainDesktop = std::make_unique<Desktop>();
        mainSounds = std::make_unique<SoundEffects>(nullptr, session->filePath("sound.ini"), false);
        mainCatalog = std::make_unique<VersionCatalog>();
        engine = std::make_unique<QQmlEngine>();
        connect(engine.get(), &QQmlEngine::warnings, this, [this](const QList<QQmlError> &errors) {
            for (const auto &error : errors) warnings << error.toString();
        });
        engine->rootContext()->setContextProperty("bible", bible.get());
        engine->rootContext()->setContextProperty("desktop", mainDesktop.get());
        engine->rootContext()->setContextProperty("sounds", mainSounds.get());
        engine->rootContext()->setContextProperty("catalog", mainCatalog.get());
        QQmlComponent component(engine.get(), QUrl("qrc:/qml/Main.qml"));
        root.reset(component.create());
        if (!root) { qWarning() << component.errorString(); return false; }
        window = qobject_cast<QQuickWindow *>(root.get());
        panel = root->findChild<QObject *>("tagsUi");
        window->resize(1100, 820);
        window->requestActivate();
        return QTest::qWaitForWindowExposed(window) && panel;
    }
    void unloadMain() { root.reset(); engine.reset(); window = nullptr; panel = nullptr; mainCatalog.reset(); mainSounds.reset(); mainDesktop.reset(); }
    QVariantMap savedRow(int index) const {
        const auto list = item("collectionList");
        return variant(list->property("model")).toList().value(index).toMap();
    }
    void openAnnotationsTags() {
        root->setProperty("collectionTab", 3);
        QVERIFY(QMetaObject::invokeMethod(root.get(), "runCommand", Q_ARG(QVariant, QString("annotations"))));
        QTRY_VERIFY(root->findChild<QObject *>("savedDrawer")->property("opened").toBool());
        QTRY_VERIFY(item("collectionList")->hasActiveFocus());
    }
private slots:
    // Annotations → Tags: every tagged passage in this version grouped by tag
    // (name order, then canonical order), headings that j/k skip, a "here"
    // marker and starting row, and keys that act on the row's own tag.
    void annotationsTagsGroupsKeysAndRemoval() {
        const auto cleanupQml = qScopeGuard([this] { unloadMain(); });
        QVERIFY(loadMain());
        QTRY_VERIFY(!root->property("restoringPosition").toBool());
        const auto opening = scope(1, 3);
        const auto beta = bible->createAndAssignTag(scope(5, 5), "Beta");
        const auto alpha = bible->createAndAssignTag(opening, "Alpha");
        const auto here = scope(8, 8);
        QVERIFY(bible->setTag(here, alpha, true)); QVERIFY(bible->setTag(here, beta, true));
        bible->selectVerses(8, 8);
        openAnnotationsTags();
        // Header: the reader's passage under the title.
        QCOMPARE(item("savedPassage")->property("text").toString(), QString("John 3:8 · NET"));
        const auto list = item("collectionList");
        QTRY_COMPARE(list->property("count").toInt(), 4);
        QStringList order;
        for (int i = 0; i < 4; ++i) order << savedRow(i)["group"].toString() + " " + savedRow(i)["reference"].toString();
        QCOMPARE(order, (QStringList{QString::fromUtf8("Alpha John 3:1–3"), "Alpha John 3:8", "Beta John 3:5", "Beta John 3:8"}));
        QVERIFY(item("savedTagGroup:Alpha")->isVisible());
        QCOMPARE(item("savedTagGroup:Alpha")->property("text").toString(), QString("Alpha · 2"));
        QCOMPARE(item("savedTagGroup:Beta")->property("text").toString(), QString("Beta · 2"));
        // Starts on the first row here; the marker shows only on rows overlapping the reader.
        QCOMPARE(list->property("currentIndex").toInt(), 1);
        QVERIFY(item("savedHereJohn 3:8")->isVisible());
        QVERIFY(!item(QString::fromUtf8("savedHereJohn 3:1–3"))->isVisible());
        // The tag line lists the range's tags, omitted when it is just the group's tag.
        QCOMPARE(item("savedTagLineJohn 3:8")->property("text").toString(), QString("Alpha · Beta"));
        QVERIFY(!item("savedTagLineJohn 3:5")->isVisible());
        QVERIFY(item("savedTagJohn 3:5")->isVisible());
        QVERIFY(!item("savedBookmarkJohn 3:5")->isVisible());
        // j/k step rows, never headings.
        key(Qt::Key_J); QCOMPARE(list->property("currentIndex").toInt(), 2);
        key(Qt::Key_K); key(Qt::Key_K); QCOMPARE(list->property("currentIndex").toInt(), 0);
        key(Qt::Key_G, Qt::ShiftModifier); QCOMPARE(list->property("currentIndex").toInt(), 3);
        // Hints: this tab's keys.
        const QString hints = item("savedHints")->property("text").toString();
        QVERIFY(hints.contains("remove&nbsp;tag")); QVERIFY(hints.contains("manage"));
        // The context menu.
        key(Qt::Key_Menu);
        QTRY_VERIFY(root->findChild<QObject *>("contextMenu")->property("visible").toBool());
        QStringList menuIds;
        for (const auto &entry : variant(root->findChild<QObject *>("contextMenu")->property("layout")).toList())
            menuIds << (entry.toMap().value("separator").toBool() ? QString("|") : entry.toMap().value("id").toString());
        QCOMPARE(menuIds, (QStringList{"open", "copy", "copyPlain", "desk", "|", "tags", "editRange", "removeTag"}));
        key(Qt::Key_Escape); QTRY_VERIFY(!root->findChild<QObject *>("contextMenu")->property("visible").toBool());
        // t: the picker on the row's range, over the drawer, which stays.
        key(Qt::Key_T); QTRY_VERIFY(item("tagSearch")->hasActiveFocus());
        QCOMPARE(prop("editingScope").toMap(), here);
        QVERIFY(root->findChild<QObject *>("savedDrawer")->property("visible").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("visibleModal").toBool());
        QTRY_VERIFY(item("collectionList")->hasActiveFocus());
        QVERIFY(root->findChild<QObject *>("savedDrawer")->property("visible").toBool());
        // delete removes only this row's tag, after confirming.
        QCOMPARE(list->property("currentIndex").toInt(), 3);
        key(Qt::Key_Delete);
        auto confirmation = root->findChild<QObject *>("deleteConfirmation");
        QTRY_VERIFY(confirmation->property("opened").toBool());
        QCOMPARE(item("deleteTitle")->property("text").toString(), QString("Remove Beta from John 3:8?"));
        key(Qt::Key_Escape); QTRY_VERIFY(!confirmation->property("visible").toBool());
        QCOMPARE(bible->tagsForScope(here).size(), 2);
        key(Qt::Key_Delete); QTRY_VERIFY(confirmation->property("opened").toBool());
        QVERIFY(QMetaObject::invokeMethod(item("deleteConfirm"), "clicked"));
        QTRY_VERIFY(!confirmation->property("visible").toBool());
        QCOMPARE(ids(bible->tagsForScope(here)), QStringList{alpha});
        QCOMPARE(ids(bible->tagsForScope(scope(5, 5))), QStringList{beta});
        QTRY_COMPARE(list->property("count").toInt(), 3);
        bible->openChapter(43, 3); bible->selectVerses(8, 8);
        // Every Annotations tab has a hint row and the passage header.
        for (int tab = 0; tab < 4; ++tab) {
            QVERIFY(QMetaObject::invokeMethod(root.get(), "filterSaved", Q_ARG(QVariant, tab - root->property("collectionTab").toInt())));
            QVERIFY(item("savedHints")->isVisible());
            QVERIFY(item("savedHints")->property("text").toString().contains(tab == 3 ? "remove&nbsp;tag" : "delete"));
            QCOMPARE(item("savedPassage")->property("text").toString(), QString("John 3:8 · NET"));
        }
        bible->clearSelection();
        QCOMPARE(item("savedPassage")->property("text").toString(), QString("John 3 · NET"));
        key(Qt::Key_Escape); QTRY_VERIFY(!root->findChild<QObject *>("savedDrawer")->property("visible").toBool());
        // Tags only in another version: said plainly.
        install(kjv);
        openAnnotationsTags();
        QCOMPARE(item("collectionList")->property("count").toInt(), 0);
        QCOMPARE(item("savedEmpty")->property("text").toString(),
            "No tagged passages in " + bible->versionAbbreviation() + ". Tags in other versions stay with them.");
        key(Qt::Key_Escape); QTRY_VERIFY(!root->findChild<QObject *>("savedDrawer")->property("visible").toBool());
    }
    // Study in the app: Cross-references first, then the remembered tab.
    void mainStudyDefaultAndRememberedTab() {
        const auto cleanupQml = qScopeGuard([this] { unloadMain(); });
        QVERIFY(loadMain());
        QTRY_VERIFY(!root->property("restoringPosition").toBool());
        scope(2, 2);
        QVERIFY(QMetaObject::invokeMethod(root.get(), "runCommand", Q_ARG(QVariant, QString("related"))));
        auto drawer = root->findChild<QObject *>("relatedDrawer");
        QTRY_VERIFY(drawer->property("opened").toBool());
        QCOMPARE(prop("studyTab").toString(), QString("crossrefs"));
        QCOMPARE(item("studyPassage")->property("text").toString(), QString("John 3:2 · NET"));
        QTRY_VERIFY(item("studyList")->hasActiveFocus());
        key(Qt::Key_H); QCOMPARE(prop("studyTab").toString(), QString("related"));
        QTRY_VERIFY(item("tagResults")->hasActiveFocus());
        key(Qt::Key_Escape); QTRY_VERIFY(!drawer->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(root.get(), "runCommand", Q_ARG(QVariant, QString("related"))));
        QTRY_VERIFY(drawer->property("opened").toBool());
        QCOMPARE(prop("studyTab").toString(), QString("related"));
        key(Qt::Key_Escape); QTRY_VERIFY(!drawer->property("visible").toBool());
    }
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QQuickStyle::setStyle("Basic");
    TagsUiTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_tagsui.moc"
