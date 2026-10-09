#include "testenvironment.h"
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
    // See also's rows (every section).
    QVariantList rows() const { return prop("studyRows").toList(); }
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
    function showContextMenu(kind, entries, x, y) { menuKind = kind; menuEntries = entries.map(function(entry) { return entry.separator ? "|" : entry.id }) }
    function copyScope(scope, plain) { copied = scope }
    function toggleScopeDesk(scope) {}
    function moveHereEntry(choices, x, y) { return { id: "moveHere", text: "Move here", enabled: false } }
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
    // The Margin, as Main keeps it: See also is its "related" panel, beside a
    // reader; esc closes it (closePanel), ctrl w moves the keyboard (switchPane).
    property real scale: theme.textScale
    property real controlScale: Math.min(scale, 1.4)
    property string escapeText: "esc"
    function tint(color, alpha) { return Qt.rgba(color.r, color.g, color.b, alpha) }
    function buttonShortcutOverride(event) {}
    function buttonEnter(button, event) {}
    function buttonClicked(button) {}
    property bool relatedOpen: false
    property bool marginKeys: false
    property bool contextMenuOpen: false
    function openRelatedPanel() { relatedOpen = true; tags.focusResults() }
    function closeNotes() { relatedOpen = false; reader.forceActiveFocus(); marginKeys = false; return true }
    function closePanel() { closeNotes() }
    function switchPane() { if (marginKeys) { reader.forceActiveFocus(); marginKeys = false } else { marginKeys = true; tags.focusResults() } }
    property var panelAwayHints: [["Ctrl+W", "keys here", switchPane]]
    property var panelReturnHint: ["Ctrl+W", "to the text", switchPane]
    property color inactiveRowColor: tint(theme.muted, 0.16)
    onActiveFocusItemChanged: {
        for (let at = activeFocusItem; at; at = at.parent) if (at === relatedHost) { marginKeys = true; return }
    }
    Item { id: reader; objectName: "reader"; focus: true }
    Rectangle {
        id: relatedHost
        objectName: "relatedPanel"
        property real inset: 24
        property real gap: 8
        anchors.right: parent.right
        width: 520; height: parent.height
        visible: app.relatedOpen
        color: theme.background
    }
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
    TagsPanel { id: tags; objectName: "testTags"; app: app; bible: testBible; desktop: theme; relatedHost: relatedHost }
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
        QVERIFY(!item("studyList")->isVisible());
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
    // See also's tagged-alike rows.
    QStringList refs() const {
        QStringList result;
        for (const auto &row : rows()) if (row.toMap().value("section") == "tags") result << row.toMap().value("reference").toString();
        return result;
    }
    QStringList rootIds() const {
        QStringList result;
        for (const auto &tag : prop("rootTags").toList()) result << tag.toMap().value("id").toString();
        return result;
    }
    bool seeAlsoOpen() const { return root->property("relatedOpen").toBool(); }
    void openSeeAlso() {
        QVERIFY(invoke("openRelated").toBool());
        QTRY_VERIFY(seeAlsoOpen());
        QTRY_VERIFY(item("studyList")->hasActiveFocus());
    }
    // Keeps only the rows the filter's words match (as typing them in it does).
    void filter(const QString &words) {
        panel->setProperty("studyQuery", words);
        panel->setProperty("studySearchOpen", !words.isEmpty());
        QCoreApplication::processEvents();
    }
private slots:
    // See also → Tagged alike: the passage's tags (exact first, then
    // overlapping), other passages ranked by shared tags in every version,
    // never the passage itself or its overlaps; a heading's tag filters.
    void seeAlsoTaggedAlikeRankingAndVersions() {
        const auto source = scope(2, 3);
        const auto a = bible->createAndAssignTag(source, "Alpha");
        const auto overlap = scope(3, 4);
        const auto b = bible->createAndAssignTag(overlap, "Beta");
        const auto both = scope(8, 9); QVERIFY(bible->setTag(both, a, true)); QVERIFY(bible->setTag(both, b, true));
        const auto alphaOnly = scope(5, 5); QVERIFY(bible->setTag(alphaOnly, a, true));
        const auto betaOnly = scope(11, 11); QVERIFY(bible->setTag(betaOnly, b, true));
        install(kjv); const auto remote = scope(2, 3); QVERIFY(bible->setTag(remote, b, true));
        QVERIFY(bible->openTaggedPassage(source));
        openSeeAlso();
        // Header: title and the reader's passage.
        QCOMPARE(item("studyTitle")->property("text").toString(), QString("See also"));
        QCOMPARE(item("studyPassage")->property("text").toString(), source["reference"].toString() + " · NET");
        QCOMPARE(prop("relatedSource").toMap(), source);
        QCOMPARE(rootIds(), (QStringList{a, b}));
        // Ranked by shared count first; never the source or its overlap; every version.
        const auto tagged = refs();
        QCOMPARE(tagged.size(), 4);
        QCOMPARE(tagged.first(), both["reference"].toString());
        for (const auto &scope : {alphaOnly, betaOnly, remote}) QVERIFY(tagged.contains(scope["reference"].toString()));
        // A row from another version says which.
        QString remoteAbbreviation;
        for (const auto &version : bible->versions())
            if (version.toMap()["id"] == remote["versionId"]) remoteAbbreviation = version.toMap()["abbreviation"].toString();
        QVERIFY(!remoteAbbreviation.isEmpty());
        int remoteRow = -1;
        for (int i = 0; i < rows().size(); ++i) if (rows()[i].toMap()["versionId"] == remote["versionId"]) remoteRow = i;
        QVERIFY(remoteRow >= 0);
        QTRY_VERIFY(item("studyRowTitle:" + QString::number(remoteRow)));
        QCOMPARE(item("studyRowTitle:" + QString::number(remoteRow))->property("text").toString(), remote["reference"].toString() + " · from " + remoteAbbreviation);
        // The body sits inside its panel's inset.
        const auto bounds = item("studyBody")->mapRectToScene(item("studyBody")->boundingRect());
        QCOMPARE(bounds.right(), window->width() - 24);
        QCOMPARE(bounds.top(), 24);
        // The heading's tags: one clicked keeps only its passages.
        QTRY_VERIFY(item("studySectionTag:Alpha") && item("studySectionTag:Beta"));
        click("studySectionTag:Alpha");
        QTRY_COMPARE(refs(), (QStringList{both["reference"].toString(), alphaOnly["reference"].toString()}));
        QCOMPARE(prop("studyQuery").toString(), QString("#Alpha"));
        filter("");
        QCOMPARE(refs().size(), 4);
        // Enter opens the row; See also stays, and keeps its root while it has focus.
        QVERIFY(QMetaObject::invokeMethod(item("studyList"), "forceActiveFocus"));
        int bothRow = -1;
        for (int i = 0; i < rows().size(); ++i) if (rows()[i].toMap()["reference"] == both["reference"] && rows()[i].toMap()["section"] == "tags") bothRow = i;
        item("studyList")->setProperty("currentIndex", bothRow);
        key(Qt::Key_Return);
        QCOMPARE(bible->tagScope(), both);
        QVERIFY(seeAlsoOpen());
        invoke("followSelection");
        QCOMPARE(prop("relatedSource").toMap(), source);
        QCOMPARE(item("studyPassage")->property("text").toString(), both["reference"].toString() + " · NET"); // The header is live.
        // Reopened, See also follows the reader again.
        invoke("close"); QTRY_VERIFY(!seeAlsoOpen());
        openSeeAlso();
        QCOMPARE(prop("relatedSource").toMap(), both);
        QCOMPARE(rootIds(), (QStringList{a, b}));
        QVERIFY(!refs().contains(both["reference"].toString()));
        invoke("close"); QTRY_VERIFY(!seeAlsoOpen());
        QVERIFY(bible->openTaggedPassage(alphaOnly));
        openSeeAlso();
        QCOMPARE(prop("relatedSource").toMap(), alphaOnly);
        QCOMPARE(rootIds(), QStringList{a});
    }
    void seeAlsoFilterMenuTagKeyAndEmptyStates() {
        const auto source = scope(2, 3);
        const auto a = bible->createAndAssignTag(source, "Alpha");
        const auto b = bible->createTag("Beta grace");
        const auto first = scope(8, 9); QVERIFY(bible->setTag(first, a, true)); QVERIFY(bible->setTag(first, b, true));
        const auto second = scope(11, 11); QVERIFY(bible->setTag(second, a, true));
        QVERIFY(bible->openTaggedPassage(source));
        openSeeAlso();
        QCOMPARE(refs().size(), 2);
        // / shows the filter; tag names match.
        QVERIFY(!item("studySearch")->isVisible());
        key(Qt::Key_Slash); QTRY_VERIFY(item("studySearch")->hasActiveFocus());
        for (const char c : QByteArray("grace")) QTest::keyClick(window, c);
        QTRY_COMPARE(refs(), QStringList{first["reference"].toString()});
        // enter keeps the filter (still shown) and returns to the list.
        key(Qt::Key_Return); QTRY_VERIFY(item("studyList")->hasActiveFocus());
        QVERIFY(item("studySearch")->isVisible());
        key(Qt::Key_Slash); QTRY_VERIFY(item("studySearch")->hasActiveFocus());
        for (const char c : QByteArray("zz")) QTest::keyClick(window, c);
        QTRY_VERIFY(rows().isEmpty());
        QCOMPARE(item("studyEmpty")->property("text").toString(), QString::fromUtf8("Nothing matches “gracezz”."));
        key(Qt::Key_Down); QTRY_VERIFY(item("studyList")->hasActiveFocus());
        // esc closes See also, as in Marks; the reader takes the keyboard, and
        // reopening clears the filter.
        QVERIFY(item("studyHints")->property("plainText").toString().endsWith("ctrl w to the text    esc close"));
        key(Qt::Key_Escape); QTRY_VERIFY(item("reader")->hasActiveFocus()); QVERIFY(!seeAlsoOpen());
        QVERIFY(!root->property("marginKeys").toBool());
        openSeeAlso(); QCOMPARE(prop("studyQuery").toString(), QString());
        QCOMPARE(refs().size(), 2);
        // ctrl w (switchPane) gives the keyboard to the reader instead, See also
        // staying: its keys wait, its hints say how to come back, and its
        // current row is grey, an inactive selection.
        auto row = item("studyRow:0"); QVERIFY(row);
        const auto rowColor = [&] { return qvariant_cast<QQuickItem *>(row->property("background"))->property("color").value<QColor>(); };
        const QColor accent = rowColor();
        QVERIFY(QMetaObject::invokeMethod(root.get(), "switchPane")); QTRY_VERIFY(item("reader")->hasActiveFocus());
        QVERIFY(seeAlsoOpen());
        key(Qt::Key_Slash); QVERIFY(!item("studySearch")->hasActiveFocus());
        QCOMPARE(item("studyHints")->property("plainText").toString(), QString("ctrl w keys here"));
        QTRY_VERIFY(rowColor() != accent);
        QCOMPARE(rowColor(), root->property("inactiveRowColor").value<QColor>());
        QVERIFY(QMetaObject::invokeMethod(item("studyHints"), "activatePart", Q_ARG(QVariant, 0)));
        QTRY_VERIFY(item("studyList")->hasActiveFocus()); QVERIFY(root->property("marginKeys").toBool());
        // A tagged passage's row menu: the shared row menu, then Edit tags…, Move here. No delete.
        filter("#alpha"); item("studyList")->setProperty("currentIndex", 0);
        key(Qt::Key_Menu);
        QCOMPARE(root->property("menuKind").toString(), QString("study"));
        QCOMPARE(variant(root->property("menuEntries")).toStringList(), (QStringList{"open", "copy", "copyPlain", "desk", "|", "tags", "moveHere"}));
        key(Qt::Key_Delete); QCOMPARE(bible->tagsForScope(first).size(), 2);
        // y copies.
        key(Qt::Key_Y); QCOMPARE(variant(root->property("copied")).toMap()["start"], first["start"]);
        // t opens the picker on the row's range; See also stays open beneath it.
        key(Qt::Key_T); QTRY_VERIFY(item("tagSearch")->hasActiveFocus());
        QCOMPARE(prop("editingScope").toMap(), first);
        QVERIFY(seeAlsoOpen());
        key(Qt::Key_Escape); QTRY_VERIFY(!panel->findChild<QObject *>("tagsSurface")->property("visible").toBool());
        QVERIFY(seeAlsoOpen());
        invoke("close");
        QTRY_VERIFY(!seeAlsoOpen());
        // Tags, but no other passages: no tagged-alike rows.
        const auto lonely = scope(6, 6); bible->createAndAssignTag(lonely, "Lonely");
        openSeeAlso();
        QVERIFY(refs().isEmpty());
        invoke("close"); QTRY_VERIFY(!seeAlsoOpen());
        // With no row to tag, t tags the passage itself.
        const auto bare = scope(12, 12);
        openSeeAlso(); filter("no such words");
        QTRY_VERIFY(rows().isEmpty());
        key(Qt::Key_T); QTRY_VERIFY(item("tagSearch")->hasActiveFocus());
        QCOMPARE(prop("editingScope").toMap(), bare);
        key(Qt::Key_Escape); QTRY_VERIFY(!panel->findChild<QObject *>("tagsSurface")->property("visible").toBool());
        invoke("close");
    }
    // A tag clicked elsewhere opens See also filtered to that tag's passages.
    void relatedForTagFilters() {
        const auto source = scope(2, 3);
        const auto a = bible->createAndAssignTag(source, "Alpha");
        const auto b = bible->createAndAssignTag(source, "Beta");
        const auto target = scope(8, 9); QVERIFY(bible->setTag(target, b, true));
        bible->openChapter(43, 4); bible->selectVerses(1, 1); // The supplied source wins.
        QVariant opened;
        QVERIFY(QMetaObject::invokeMethod(panel, "openRelatedForTag", Q_RETURN_ARG(QVariant, opened), Q_ARG(QVariant, QString("Beta")), Q_ARG(QVariant, source)));
        QVERIFY(variant(opened).toBool());
        QTRY_VERIFY(seeAlsoOpen());
        QCOMPARE(prop("relatedSource").toMap(), source);
        QCOMPARE(prop("studyQuery").toString(), QString("#Beta"));
        QCOMPARE(refs(), QStringList{target["reference"].toString()});
        QCOMPARE(rows().size(), 1); // The filter keeps only tagged passages.
        Q_UNUSED(a)
    }
    // The shared header follows the reader: the selection, else its chapter.
    void studyHeaderFollowsReader() {
        scope(2, 3);
        QVERIFY(invoke("openRelated").toBool());
        QTRY_VERIFY(seeAlsoOpen());
        QVERIFY(item("studyPassage")->isVisible());
        QCOMPARE(item("studyPassage")->property("text").toString(), QString::fromUtf8("John 3:2–3 · NET"));
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
        bible->setNoteText("Preserve note"); QVERIFY(bible->toggleHighlight());
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
        QCOMPARE(bible->noteText(), QString("Preserve note")); QVERIFY(bible->selectionHighlighted());
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
        QCOMPARE(bible->noteText(), QString("Preserve note")); QVERIFY(bible->selectionHighlighted());
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
        QTRY_VERIFY(picker->property("opened").toBool());
        QVERIFY(picker->property("modal").toBool());
        QCOMPARE(prop("hostKind").toString(), QString("picker"));
        QVERIFY(prop("scopePinned").toBool());
        QVERIFY(!root->property("marksOpen").toBool());
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
        QVERIFY(!root->property("marksOpen").toBool());
        QCOMPARE(bible->tagScope(), changed); // Closing cannot collapse the new range.

        QVERIFY(QMetaObject::invokeMethod(root.get(), "openPassageTags", Q_ARG(QVariant, QVariant())));
        QTRY_VERIFY(item("tagSearch")->hasActiveFocus());
        QCOMPARE(prop("editingScope").toMap(), changed);
        QVERIFY(bible->switchVersion("eng-kjv"));
        const auto otherVersion = scope(8, 9);
        QCOMPARE(prop("editingScope").toMap(), changed);
        QTRY_VERIFY(picker->property("opened").toBool()); // After its fade-in.
        QVERIFY(prop("scopePinned").toBool());
        QVERIFY(!root->property("marksOpen").toBool());
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
        QVERIFY(!root->property("marksOpen").toBool());
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
    void openMarks() {
        QVERIFY(QMetaObject::invokeMethod(root.get(), "runCommand", Q_ARG(QVariant, QString("annotations"))));
        QTRY_VERIFY(root->property("marksOpen").toBool());
        QTRY_VERIFY(root->findChild<QObject *>("notesDrawer")->property("opened").toBool());
        QTRY_VERIFY(item("collectionList")->hasActiveFocus());
    }
    // esc from a panel's list closes the Margin; the reader takes the keyboard.
    void closeMargin() {
        key(Qt::Key_Escape); QTRY_VERIFY(!root->findChild<QObject *>("notesDrawer")->property("visible").toBool());
        QTRY_VERIFY(item("readerSurface")->hasActiveFocus());
    }
private slots:
    // Marks: one row per tagged passage with all its tags, a "here" marker and
    // starting row, #tag filtering by a row's tag, and keys that act on the row.
    void marksTagsFilterKeysAndDeletion() {
        const auto cleanupQml = qScopeGuard([this] { unloadMain(); });
        QVERIFY(loadMain());
        QTRY_VERIFY(!root->property("restoringPosition").toBool());
        const auto opening = scope(1, 3);
        const auto beta = bible->createAndAssignTag(scope(5, 5), "Beta");
        const auto alpha = bible->createAndAssignTag(opening, "Alpha");
        const auto here = scope(8, 8);
        QVERIFY(bible->setTag(here, alpha, true)); QVERIFY(bible->setTag(here, beta, true));
        bible->selectVerses(8, 8);
        openMarks();
        // Header: the reader's passage under the title.
        QCOMPARE(item("savedPassage")->property("text").toString(), QString("John 3:8 · NET"));
        const auto list = item("collectionList");
        QTRY_COMPARE(list->property("count").toInt(), 3);
        QStringList order;
        for (int i = 0; i < 3; ++i) order << savedRow(i)["reference"].toString() + " " + ids(savedRow(i)["tags"].toList()).join(",");
        QCOMPARE(order, (QStringList{QString::fromUtf8("John 3:1–3 ") + alpha, "John 3:5 " + beta, "John 3:8 " + alpha + "," + beta}));
        // Starts on the row here; the marker shows only on rows overlapping the reader.
        QCOMPARE(list->property("currentIndex").toInt(), 2);
        QVERIFY(item("savedHereJohn 3:8")->isVisible());
        QVERIFY(!item(QString::fromUtf8("savedHereJohn 3:1–3"))->isVisible());
        // A row without a note shows the verse instead.
        QVERIFY(item("savedTagsJohn 3:8")->isVisible());
        QVERIFY(item("savedExcerptJohn 3:5")->isVisible());
        QVERIFY(!item("savedNoteTextJohn 3:5")->isVisible());
        key(Qt::Key_K); QCOMPARE(list->property("currentIndex").toInt(), 1);
        key(Qt::Key_K); QCOMPARE(list->property("currentIndex").toInt(), 0);
        key(Qt::Key_G, Qt::ShiftModifier); QCOMPARE(list->property("currentIndex").toInt(), 2);
        // Hints: the row's keys.
        const QString hints = item("savedHints")->property("text").toString();
        QVERIFY(hints.contains("note")); QVERIFY(hints.contains("tags")); QVERIFY(hints.contains("delete"));
        // The context menu.
        key(Qt::Key_Menu);
        QTRY_VERIFY(root->findChild<QObject *>("contextMenu")->property("visible").toBool());
        QStringList menuIds;
        for (const auto &entry : variant(root->findChild<QObject *>("contextMenu")->property("layout")).toList())
            menuIds << (entry.toMap().value("separator").toBool() ? QString("|") : entry.toMap().value("id").toString());
        QCOMPARE(menuIds, (QStringList{"open", "copy", "copyPlain", "desk", "|", "note", "tags", "moveHere", "delete", "undo"}));
        key(Qt::Key_Escape); QTRY_VERIFY(!root->findChild<QObject *>("contextMenu")->property("visible").toBool());
        // t: the picker on the row's range, over the drawer, which stays.
        key(Qt::Key_T); QTRY_VERIFY(item("tagSearch")->hasActiveFocus());
        QCOMPARE(prop("editingScope").toMap(), here);
        QVERIFY(root->property("marksOpen").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("visibleModal").toBool());
        QTRY_VERIFY(item("collectionList")->hasActiveFocus());
        QVERIFY(root->property("marksOpen").toBool());
        // delete removes the row's whole mark, at once; u puts it back, ctrl r removes it again.
        QCOMPARE(list->property("currentIndex").toInt(), 2);
        key(Qt::Key_Delete);
        QTRY_VERIFY(bible->tagsForScope(here).isEmpty());
        QCOMPARE(root->findChild<QObject *>("toast")->property("text").toString(), QString("John 3:8 deleted"));
        QCOMPARE(ids(bible->tagsForScope(scope(5, 5))), QStringList{beta});
        QTRY_COMPARE(list->property("count").toInt(), 2);
        key(Qt::Key_U); QTRY_COMPARE(bible->tagsForScope(here).size(), 2);
        QTRY_COMPARE(list->property("count").toInt(), 3);
        key(Qt::Key_R, Qt::ControlModifier); QTRY_VERIFY(bible->tagsForScope(here).isEmpty());
        QTRY_COMPARE(list->property("count").toInt(), 2);
        // Mouse: a row's tag shows only its marks.
        click("savedTag:Beta");
        QTRY_COMPARE(list->property("count").toInt(), 1);
        QCOMPARE(root->property("savedQuery").toString(), QString("#Beta"));
        bible->openChapter(43, 3); bible->selectVerses(8, 8);
        QCOMPARE(item("savedPassage")->property("text").toString(), QString("John 3:8 · NET"));
        bible->clearSelection();
        QCOMPARE(item("savedPassage")->property("text").toString(), QString("John 3 · NET"));
        closeMargin();
        // Reading another version: its marks are labelled with theirs.
        const QString net = bible->versionAbbreviation();
        install(kjv);
        openMarks();
        QTRY_COMPARE(item("collectionList")->property("count").toInt(), 2);
        QCOMPARE(savedRow(0)["versionAbbreviation"].toString(), net);
        QVERIFY(!savedRow(0)["active"].toBool());
        QCOMPARE(item("savedVersionJohn 3:5")->property("text").toString(), "from " + net);
        QVERIFY(item("savedVersionJohn 3:5")->isVisible());
        closeMargin();
    }
    // See also in the app: on the reader's passage, one list, no tabs.
    void mainSeeAlsoOnThePassage() {
        const auto cleanupQml = qScopeGuard([this] { unloadMain(); });
        QVERIFY(loadMain());
        QTRY_VERIFY(!root->property("restoringPosition").toBool());
        scope(2, 2);
        QVERIFY(QMetaObject::invokeMethod(root.get(), "runCommand", Q_ARG(QVariant, QString("related"))));
        QTRY_VERIFY(root->property("relatedOpen").toBool());
        QCOMPARE(item("studyPassage")->property("text").toString(), QString("John 3:2 · NET"));
        QTRY_VERIFY(item("studyList")->hasActiveFocus());
        QVERIFY(!item("studyTabs"));
        key(Qt::Key_H); QVERIFY(item("studyList")->hasActiveFocus()); QVERIFY(root->property("relatedOpen").toBool());
        closeMargin();
    }
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QQuickStyle::setStyle("Basic");
    TagsUiTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_tagsui.moc"
