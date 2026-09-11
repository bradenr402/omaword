#include <QtTest/QTest>
#include <QSignalSpy>
#include <QFileInfo>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QQuickTextDocument>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextBlock>
#include <QTextCursor>
#include <QJSValue>
#include "blockdocument.h"

// Standalone, loads the real BlockReader and QTextDocument without Main or a
// resource rebuild. Optional OMAWORD_ANNOTATION_SCREENSHOT=/tmp/...png.
class BlockAnnotationsTest : public QObject
{
    Q_OBJECT
    std::unique_ptr<QQmlEngine> engine;
    std::unique_ptr<QObject> root;
    QStringList warnings;
    QObject *backend = nullptr;
    QObject *controller = nullptr;
    QQuickWindow *window = nullptr;
    BlockDocument *mapping = nullptr;

    static QQuickItem *find(QQuickItem *item, const QString &name) {
        if (item->objectName() == name) return item;
        for (auto child : item->childItems())
            if (auto match = find(child,name)) return match;
        return nullptr;
    }
    QQuickItem *item(const QString &name) const { return find(window->contentItem(),name); }
    QVariantList list(const char *property) const {
        const auto value = item("reader")->property(property);
        return value.canConvert<QJSValue>() ? value.value<QJSValue>().toVariant().toList() : value.toList();
    }
    static QVariantMap note(QString ref, int start, int end, int chapter = 1, int endChapter = 1) {
        return {{"reference",ref},{"book",1},{"chapter",chapter},{"start",start},
                {"endBook",1},{"endChapter",endChapter},{"end",end},{"note","Saved note"}};
    }
    static QVariantMap bookmark(QString ref, bool first, bool last) {
        return {{"reference",ref},{"bookmarkContinuesBefore",!first},{"bookmarkContinuesAfter",!last}};
    }
    void populate(bool adjacent = false) {
        QVariantList verses;
        for (int i = 1; i <= 6; ++i) {
            QVariantList ranges;
            if (i <= 3) ranges.append(bookmark("Genesis 1:1–3",i == 1,i == 3));
            if (adjacent && i >= 4 && i <= 5) ranges.append(bookmark("Genesis 1:4–5",i == 4,i == 5));
            verses.append(QVariantMap{{"number",i},{"text",QString("Verse %1 is here.").arg(i)},
                {"heading",i == 1 ? "A section heading" : ""},{"bookmarkRanges",ranges}});
        }
        backend->setProperty("verses",verses);
        backend->setProperty("collection",QVariantList{note("Genesis 1:1–3",1,3),note("Genesis 1:4–5",4,5)});
    }
    void click(QQuickItem *target) {
        QVERIFY(target);
        QVERIFY(target->isVisible());
        QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,
                          target->mapToScene(QPointF(target->width()/2,target->height()/2)).toPoint());
        QCoreApplication::processEvents();
    }
private slots:
    void init() {
        warnings.clear();
        engine = std::make_unique<QQmlEngine>();
        connect(engine.get(),&QQmlEngine::warnings,this,[this](const QList<QQmlError> &errors) {
            for (const auto &error : errors) warnings.append(error.toString());
        });
        const QString reader = QFINDTESTDATA("../qml/BlockReader.qml");
        QVERIFY(!reader.isEmpty());
        QQmlComponent component(engine.get());
#ifndef Q_MOC_RUN
        component.setData(R"(
import QtQuick
import QtQuick.Controls
import ")" + QUrl::fromLocalFile(QFileInfo(reader).absolutePath()).toEncoded() + R"(" as Reader
ApplicationWindow {
    width: 850; height: 600; visible: true; color: "#181c21"
    QtObject {
        id: backend; objectName: "backend"
        property var verses: []
        property var collection: []
        property int book: 1
        property int chapter: 1
        property bool showVerseNumbers: true
        property bool showHeadings: true
        property bool focusMode: false
        property real lineHeight: 1.5
        property int selectedStart: 0
        property int selectedEnd: 0
        property string navigated: ""
        property bool navigationSucceeds: true
        function navigate(ref) { navigated = ref; controller.calls += "navigate;"; return navigationSucceeds }
    }
    QtObject {
        id: controller; objectName: "controller"
        property real readerSize: 20
        property real scale: 1
        property real floatingSafety: 0
        property string freeSelectedText: ""
        property string openedBookmark: ""
        property string calls: ""
        property bool canFlush: true
        function tint(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }
        function clearTextSelection() { calls += "clear;"; freeSelectedText = ""; reader.clearSelection() }
        function flushNote() { calls += "flush;"; return canFlush }
        function syncNote() { calls += "sync;" }
        function openNotes(edit) { calls += edit ? "edit;" : "preview;" }
        function openBookmark(ref) { openedBookmark = ref }
    }
    QtObject {
        id: theme
        property color background: "#181c21"
        property color foreground: "#d9e1ea"
        property color accent: "#8db5d4"
        property string fontFamily: "DejaVu Sans"
    }
    Flickable {
        id: viewport; anchors.fill: parent; clip: true
        contentHeight: reader.height + 100
        Reader.BlockReader {
            id: reader; objectName: "reader"
            x: 80; y: 30; width: parent.width - 120
            controller: controller; backend: backend; theme: theme; viewport: viewport
        }
    }
})",QUrl::fromLocalFile(QFileInfo(reader).absolutePath() + "/AnnotationHarness.qml"));
#endif
        root.reset(component.create());
        QVERIFY2(root,qPrintable(component.errorString()));
        window = qobject_cast<QQuickWindow *>(root.get());
        QVERIFY(window);
        backend = root->findChild<QObject *>("backend");
        controller = root->findChild<QObject *>("controller");
        mapping = root->findChild<BlockDocument *>();
        QVERIFY(mapping);
        QVERIFY(QTest::qWaitForWindowExposed(window));
    }
    void cleanup() {
        root.reset();
        engine.reset();
        QVERIFY2(warnings.isEmpty(),qPrintable(warnings.join('\n')));
    }
    void logicalRangesAndInlineGeometry() {
        populate(true);
        QTRY_COMPARE(list("annotationRanges").size(),4);
        const auto ranges = list("annotationRanges");
        // Two logical bookmarks, not five per-verse underlines, plus two notes.
        QCOMPARE(ranges[0].toMap().value("reference").toString(),QString("Genesis 1:1–3"));
        const auto a = ranges[0].toMap();
        const auto b = ranges[2].toMap();
        QCOMPARE(a.value("y"),b.value("y"));
        QVERIFY(b.value("rects").toList().first().toMap().value("x").toDouble()
                > a.value("rects").toList().first().toMap().value("x").toDouble());
        QCOMPARE(list("annotationGroups").size(),1);
        auto ribbon = item("blockAnnotationMarker:bookmark:Genesis 1:1–3");
        auto noteIcon = item("blockAnnotationMarker:note:Genesis 1:1–3");
        QVERIFY(ribbon && noteIcon);
        const auto ribbonRect = ribbon->mapRectToItem(item("blockBody"),ribbon->boundingRect());
        const auto noteRect = noteIcon->mapRectToItem(item("blockBody"),noteIcon->boundingRect());
        QVERIFY(!ribbonRect.intersects(noteRect));
        QVERIFY(ribbonRect.right() < 0 && noteRect.right() < 0);
        auto quick = item("blockBody")->property("textDocument").value<QQuickTextDocument *>();
        QVERIFY(quick);
        const auto text = quick->textDocument()->toPlainText();
        QVERIFY(!text.contains("Genesis"));
        QVERIFY(!text.contains(QChar(0xfffc)));
        click(ribbon);
        QTRY_VERIFY(item("blockAnnotationAction:bookmark:Genesis 1:4–5"));
        click(item("blockAnnotationAction:bookmark:Genesis 1:4–5"));
        QCOMPARE(controller->property("openedBookmark").toString(),QString("Genesis 1:4–5"));
        click(noteIcon);
        QTRY_VERIFY(item("blockAnnotationAction:note:Genesis 1:4–5"));
        controller->setProperty("calls","");
        click(item("blockAnnotationAction:note:Genesis 1:4–5"));
        QCOMPARE(backend->property("navigated").toString(),QString("Genesis 1:4–5"));
        QCOMPARE(controller->property("calls").toString(),QString("flush;navigate;clear;sync;edit;"));
    }
    void continuationAndSelectionGuard() {
        populate();
        auto verses = backend->property("verses").toList();
        for (int i = 0; i < 3; ++i) {
            auto verse = verses[i].toMap();
            verse["bookmarkRanges"] = QVariantList{bookmark("Genesis 1:30–2:3",false,i == 2)};
            verses[i] = verse;
        }
        backend->setProperty("chapter",2);
        backend->setProperty("verses",verses);
        backend->setProperty("collection",QVariantList{note("Genesis 1:30–2:3",30,3,1,2),
            note("Genesis 3:1–3",1,3,3,3)});
        QTRY_COMPARE(list("annotationRanges").size(),2);
        for (const auto &entry : list("annotationRanges")) {
            QVERIFY(entry.toMap().value("continuesBefore").toBool());
            QVERIFY(!entry.toMap().value("continuesAfter").toBool());
        }
        auto icon = item("blockAnnotationMarker:note:Genesis 1:30–2:3");
        QVERIFY(QMetaObject::invokeMethod(item("blockBody"),"select",Q_ARG(int,0),Q_ARG(int,8)));
        const auto selection = item("blockBody")->property("selectedText").toString();
        QVERIFY(!selection.isEmpty());
        controller->setProperty("freeSelectedText",selection);
        controller->setProperty("canFlush",false);
        controller->setProperty("calls","");
        click(icon);
        QCOMPARE(controller->property("calls").toString(),QString("flush;"));
        QVERIFY(backend->property("navigated").toString().isEmpty());
        QCOMPARE(controller->property("freeSelectedText").toString(),selection);
        QCOMPARE(item("blockBody")->property("selectedText").toString(),selection);
        controller->setProperty("canFlush",true);
        backend->setProperty("navigationSucceeds",false);
        controller->setProperty("calls","");
        click(icon);
        QCOMPARE(controller->property("calls").toString(),QString("flush;navigate;"));
        QCOMPARE(controller->property("freeSelectedText").toString(),selection);
        QCOMPARE(item("blockBody")->property("selectedText").toString(),selection);
        backend->setProperty("navigationSucceeds",true);
        controller->setProperty("calls","");
        click(icon);
        QCOMPARE(controller->property("calls").toString(),QString("flush;navigate;clear;sync;edit;"));
        QVERIFY(controller->property("freeSelectedText").toString().isEmpty());
        QVERIFY(item("blockBody")->property("selectedText").toString().isEmpty());
        QCOMPARE(backend->property("navigated").toString(),QString("Genesis 1:30–2:3"));
    }
    void relayoutAndFocus() {
        populate(true);
        QTRY_COMPARE(list("annotationRanges").size(),4);
        const double initialY = list("annotationRanges")[0].toMap().value("y").toDouble();
        const double initialHeight = list("annotationRanges")[0].toMap().value("height").toDouble();
        QSignalSpy geometry(mapping,&BlockDocument::geometryChanged);
        item("reader")->setWidth(210);
        QTRY_VERIFY(list("annotationRanges")[0].toMap().value("height").toDouble() > initialHeight);
        QVERIFY(geometry.count() > 0);
        backend->setProperty("showHeadings",false);
        QTRY_VERIFY(list("annotationRanges")[0].toMap().value("y").toDouble() < initialY);
        const auto previousHeight = list("annotationRanges")[0].toMap().value("height").toDouble();
        controller->setProperty("readerSize",28);
        backend->setProperty("lineHeight",1.9);
        QTRY_VERIFY(list("annotationRanges")[0].toMap().value("height").toDouble() > previousHeight);
        backend->setProperty("showVerseNumbers",false);
        QTRY_COMPARE(mapping->spans().first().toMap().value("textStart"),mapping->spans().first().toMap().value("start"));
        controller->setProperty("scale",1.6);
        QTRY_COMPARE(item("blockAnnotationRail")->width(),64.0);
        backend->setProperty("focusMode",true);
        QTRY_VERIFY(!item("blockAnnotationRail")->isVisible());
        QVERIFY(!item("blockAnnotationMarker:bookmark:Genesis 1:1–3")->isVisible());
        backend->setProperty("focusMode",false);
        QTRY_VERIFY(item("blockAnnotationRail")->isVisible());
    }
    void paragraphBoundariesAndAnnotations() {
        populate(true);
        auto verses = backend->property("verses").toList();
        for (int i = 0; i < verses.size(); ++i) {
            auto verse = verses[i].toMap();
            verse["paragraphStart"] = i == 0 || i == 3;
            if (i == 4) verse["html"] = "Poetry sings.<br>A second line.";
            verses[i] = verse;
        }
        backend->setProperty("verses",verses);
        item("reader")->setProperty("mode","paragraph");
        auto quick = item("blockBody")->property("textDocument").value<QQuickTextDocument *>();
        QVERIFY(quick);
        auto doc = quick->textDocument();
        QTRY_COMPARE(doc->blockCount(),3); // Heading, verses 1–3, verses 4–6.
        QCOMPARE(doc->findBlock(mapping->positionFor(1)),doc->findBlock(mapping->positionFor(3)));
        QVERIFY(doc->findBlock(mapping->positionFor(3)) != doc->findBlock(mapping->positionFor(4)));
        for (auto block = doc->begin(); block.isValid(); block = block.next()) QVERIFY(!block.text().isEmpty());
        QVERIFY(doc->toRawText().contains(QChar::LineSeparator));
        QVERIFY(!doc->toPlainText().contains(QChar(0x00b6)));
        QTRY_COMPARE(list("annotationRanges").size(),4);
        QTRY_VERIFY(list("annotationRanges")[2].toMap().value("y").toDouble()
                    > list("annotationRanges")[0].toMap().value("y").toDouble());
        const auto span = mapping->spans()[3].toMap();
        const auto rect = mapping->rectangles(span.value("start").toInt(),span.value("end").toInt()).first().toMap();
        QCOMPARE(mapping->verseAtPoint(rect.value("x").toDouble() + 2,rect.value("y").toDouble() + 5),4);
        click(item("blockAnnotationMarker:note:Genesis 1:1–3"));
        QTRY_VERIFY(item("blockAnnotationAction:note:Genesis 1:4–5"));
        click(item("blockAnnotationAction:note:Genesis 1:4–5"));
        QCOMPARE(backend->property("navigated").toString(),QString("Genesis 1:4–5"));
        backend->setProperty("focusMode",true);
        QTRY_COMPARE(doc->blockCount(),2);
        QVERIFY(doc->toPlainText().startsWith("Verse 1 is here. Verse 2 is here. Verse 3 is here.\nVerse 4 is here."));
        QVERIFY(!item("blockAnnotationRail")->isVisible());
        QTextCursor selection(doc);
        selection.setPosition(mapping->positionFor(3));
        selection.setPosition(mapping->spans()[3].toMap().value("end").toInt(),QTextCursor::KeepAnchor);
        QCOMPARE(selection.selection().toPlainText(),QString("Verse 3 is here.\nVerse 4 is here."));
        item("reader")->setProperty("mode","block");
        QTRY_COMPARE(doc->blockCount(),1);
        QVERIFY(doc->toPlainText().contains("Verse 3 is here. Verse 4 is here."));
        // The old five-argument API retains block behavior.
        mapping->build(quick,verses,false,false,1.5);
        QCOMPARE(doc->blockCount(),1);
    }
    void paragraphHeadingDoesNotCreateEmptyBlocks() {
        backend->setProperty("verses",QVariantList{
            QVariantMap{{"number",1},{"text","First."},{"heading","First section"},{"paragraphStart",true}},
            QVariantMap{{"number",2},{"text","Second."},{"heading","Second section"},{"paragraphStart",true}},
            QVariantMap{{"number",3},{"text","Third."},{"paragraphStart",true}}});
        item("reader")->setProperty("mode","paragraph");
        auto doc = item("blockBody")->property("textDocument").value<QQuickTextDocument *>()->textDocument();
        QTRY_COMPARE(doc->blockCount(),5);
        for (auto block = doc->begin(); block.isValid(); block = block.next()) QVERIFY(!block.text().isEmpty());
        backend->setProperty("showHeadings",false);
        QTRY_COMPARE(doc->blockCount(),3);
        backend->setProperty("verses",QVariantList{});
        QTRY_VERIFY(doc->isEmpty());
        QCOMPARE(doc->blockCount(),1);
    }
    void screenshot() {
        populate(true);
        QTRY_COMPARE(list("annotationRanges").size(),4);
        if (qEnvironmentVariableIsEmpty("OMAWORD_ANNOTATION_SCREENSHOT")) QSKIP("Optional visual artifact");
        QTest::qWait(150);
        const auto image = window->grabWindow();
        QVERIFY(!image.isNull());
        const auto path = qEnvironmentVariable("OMAWORD_ANNOTATION_SCREENSHOT");
        QVERIFY(image.save(path));
        click(item("blockAnnotationMarker:bookmark:Genesis 1:1–3"));
        QTest::qWait(150);
        QVERIFY(window->grabWindow().save(path + ".chooser.png"));
        QTest::keyClick(window,Qt::Key_Escape);
        controller->setProperty("scale",1.6);
        item("reader")->setWidth(470);
        QTest::qWait(150);
        QVERIFY(window->grabWindow().save(path + ".scaled.png"));
        backend->setProperty("focusMode",true);
        QTest::qWait(150);
        QVERIFY(window->grabWindow().save(path + ".focus.png"));
    }
};

int main(int argc, char **argv)
{
    QGuiApplication app(argc,argv);
    QQuickStyle::setStyle("Basic");
    BlockAnnotationsTest test;
    return QTest::qExec(&test,argc,argv);
}
#include "tst_blockannotations.moc"
