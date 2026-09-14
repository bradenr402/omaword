#include <QtTest/QTest>
#include <QSignalSpy>
#include <QFileInfo>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QQuickTextDocument>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextLayout>
#include <QJSValue>
#include <QJsonDocument>
#include <QClipboard>
#include <QMimeData>
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QAbstractTextDocumentLayout>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
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
    QVariantMap navigatedScope() const {
        const auto value = backend->property("navigatedScope");
        return value.canConvert<QJSValue>() ? value.value<QJSValue>().toVariant().toMap() : value.toMap();
    }
    static QRectF bracketRect(const QVariantMap &marker) {
        const bool opening = marker["edge"] != "close";
        const double stroke = qMax(1.0,marker["height"].toDouble()/18);
        const double width = marker["paintWidth"].toDouble();
        const double origin = marker["bracketX"].toDouble();
        double left = origin + (opening ? width*0.78-stroke/2 : 0);
        double right = origin + (opening ? width : width*0.65+stroke/2);
        if (marker["rtl"].toBool()) {
            const double cellWidth = marker["width"].toDouble();
            const double oldLeft = left;
            left = cellWidth-right; right = cellWidth-oldLeft;
        }
        return QRectF(marker["x"].toDouble()+left,marker["y"].toDouble()+marker["bracketTop"].toDouble(),
                      right-left,marker["bracketHeight"].toDouble());
    }
    static QList<QRectF> textInkRects(QTextDocument *doc, int start, int end) {
        QList<QRectF> rects;
        for (auto block = doc->findBlock(start); block.isValid() && block.position() < end; block = block.next()) {
            const QPointF origin = doc->documentLayout()->blockBoundingRect(block).topLeft();
            const int first = qMax(0,start-block.position());
            const int count = qMin(end-block.position(),block.length()-1)-first;
            for (const auto &run : block.layout()->glyphRuns(first,count)) {
                const auto indexes = run.glyphIndexes();
                const auto positions = run.positions();
                for (int i = 0; i < indexes.size(); ++i) {
                    const auto rect = run.rawFont().boundingRect(indexes[i]).translated(origin+positions[i]);
                    if (!rect.isEmpty()) rects.append(rect);
                }
            }
        }
        return rects;
    }
    static QImage onBackground(const QImage &image, const QColor &background) {
        QImage result(image.size(),QImage::Format_ARGB32_Premultiplied);
        result.fill(background);
        QPainter painter(&result); painter.drawImage(0,0,image);
        return result;
    }
    static QVariantMap note(QString ref, int start, int end, int chapter = 1, int endChapter = 1) {
        return {{"reference",ref},{"book",1},{"chapter",chapter},{"start",start},
                {"endBook",1},{"endChapter",endChapter},{"end",end},{"note","Saved note"}};
    }
    static QVariantMap bookmark(QString ref, bool first, bool last) {
        return {{"reference",ref},{"bookmarkContinuesBefore",!first},{"bookmarkContinuesAfter",!last}};
    }
    static QVariantMap noteRange(QString ref, bool first, bool last, QVariantMap scope = {}) {
        if (scope.isEmpty()) scope = note(ref,1,3);
        scope.remove("note");
        scope["versionId"] = "test-edition";
        scope["noteStart"] = first; scope["noteEnd"] = last;
        scope["continuesBefore"] = !first; scope["continuesAfter"] = !last;
        return scope;
    }
    void populate(bool adjacent = false) {
        QVariantList verses;
        for (int i = 1; i <= 6; ++i) {
            QVariantList ranges, notes;
            if (i <= 3) ranges.append(bookmark("Genesis 1:1–3",i == 1,i == 3));
            if (adjacent && i >= 4 && i <= 5) ranges.append(bookmark("Genesis 1:4–5",i == 4,i == 5));
            if (i <= 3) notes.append(noteRange("Genesis 1:1–3",i == 1,i == 3));
            if (i >= 4 && i <= 5) notes.append(noteRange("Genesis 1:4–5",i == 4,i == 5,note("Genesis 1:4–5",4,5)));
            verses.append(QVariantMap{{"number",i},{"text",QString("Verse %1 is here.").arg(i)},
                {"heading",i == 1 ? "A section heading" : ""},{"bookmarkRanges",ranges},{"noteRanges",notes}});
        }
        backend->setProperty("verses",verses);
        backend->setProperty("collection",QVariantList{note("Genesis 1:1–3",1,3),note("Genesis 1:4–5",4,5)});
    }
    void click(QQuickItem *target) {
        QVERIFY(target);
        QVERIFY(target->isVisible());
        QTest::qWait(60); // Let popup enter transitions and scene transforms settle.
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
        property int cursorVerse: 0
        property string navigated: ""
        property bool navigationSucceeds: true
        property var navigatedScope: ({})
        function navigate(ref) { navigated = ref; controller.calls += "navigate;"; return navigationSucceeds }
        function openTaggedPassage(scope) { navigatedScope = scope; controller.calls += "scope;"; return navigationSucceeds }
    }
    QtObject {
        id: controller; objectName: "controller"
        property real readerSize: 20
        property real scale: 1
        property real floatingSafety: 0
        property string freeSelectedText: ""
        property string openedBookmark: ""
        property var openedBookmarkScope: ({})
        property var openedTags: ({})
        property var editedScope: ({})
        property string editedKind: ""
        property string calls: ""
        property bool canFlush: true
        property string copied: ""
        function tint(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }
        function clearTextSelection() { calls += "clear;"; freeSelectedText = ""; reader.clearSelection() }
        function flushNote() { calls += "flush;"; return canFlush }
        function syncNote() { calls += "sync;" }
        function openNotes(edit) { calls += edit ? "edit;" : "preview;" }
        function openBookmark(scope) { openedBookmarkScope = scope; openedBookmark = typeof scope === "string" ? scope : scope.reference }
        function openPassageTags(scope) { openedTags = scope }
        function openRangeEditor(scope, kind) { editedScope = scope; editedKind = kind; calls += "range;" }
        function copyFreeText() { copied = freeSelectedText }
        function copyPassage(plain) { calls += "passage:" + plain + ";" }
        function selectVerse(verse, extend) { backend.cursorVerse = verse }
    }
    QtObject {
        id: theme
        property color background: "#181c21"
        property color foreground: "#d9e1ea"
        property color accent: "#8db5d4"
        property color surface: "#222830"
        property color border: "#485563"
        property real textScale: 1
        property real cornerRadius: 8
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
    Reader.AnnotationGlyph { id: sharedGlyph; objectName: "sharedGlyph"; x: 600; y: 500 }
    Canvas {
        objectName: "footerNoteReference"; x: 650; y: 500
        width: sharedGlyph.width; height: sharedGlyph.height
        property color ink: sharedGlyph.ink
        onInkChanged: requestPaint()
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.scale(width / 24, height / 24)
            ctx.strokeStyle = ink
            ctx.lineWidth = 1.6
            ctx.lineCap = "square"
            ctx.lineJoin = "miter"
            ctx.beginPath()
            // Snapshot of the original footer path, independent of the component.
            ctx.moveTo(11, 4); ctx.lineTo(3, 4); ctx.lineTo(3, 21)
            ctx.lineTo(20, 21); ctx.lineTo(20, 13)
            ctx.moveTo(10, 11); ctx.lineTo(18, 3); ctx.lineTo(21, 6)
            ctx.lineTo(13, 14); ctx.lineTo(9, 15); ctx.closePath()
            ctx.moveTo(16, 5); ctx.lineTo(19, 8)
            ctx.stroke()
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
        QTRY_COMPARE(list("inlineMarkers").size(),8);
        QVERIFY(!item("blockAnnotationRail"));
        auto ribbon = item("blockInlineMarker:bookmark:open:Genesis 1:1–3");
        auto noteIcon = item("blockInlineMarker:note:open:Genesis 1:1–3");
        QVERIFY(ribbon && noteIcon);
        QVERIFY2(ribbon->width() > 1,qPrintable(QString("cell width %1; %2").arg(ribbon->width()).arg(QJsonDocument::fromVariant(list("inlineMarkers")).toJson())));
        const auto ribbonRect = ribbon->mapRectToItem(item("blockBody"),ribbon->boundingRect());
        const auto noteRect = noteIcon->mapRectToItem(item("blockBody"),noteIcon->boundingRect());
        QVERIFY(!ribbonRect.intersects(noteRect));
        QVERIFY(ribbonRect.left() >= 0 && noteRect.left() >= 0);
        QVERIFY(!item("blockBookmarkExtent:Genesis 1:1–3"));
        QVERIFY(!item("blockAnnotationMarker:bookmark:Genesis 1:1–3"));
        auto quick = item("blockBody")->property("textDocument").value<QQuickTextDocument *>();
        QVERIFY(quick);
        const auto text = mapping->selectionText(0,quick->textDocument()->characterCount() - 1);
        QVERIFY(!text.contains("Genesis"));
        QVERIFY(!text.contains(QChar(0xfffc)));
        click(ribbon);
        QCOMPARE(controller->property("openedBookmark").toString(),QString("Genesis 1:1–3"));
        click(item("blockInlineMarker:bookmark:open:Genesis 1:4–5"));
        QCOMPARE(controller->property("openedBookmark").toString(),QString("Genesis 1:4–5"));
        click(noteIcon);
        controller->setProperty("calls","");
        click(item("blockInlineMarker:note:open:Genesis 1:4–5"));
        QCOMPARE(navigatedScope()["reference"].toString(),QString("Genesis 1:4–5"));
        QCOMPARE(navigatedScope()["start"].toInt(),4);
        QCOMPARE(navigatedScope()["end"].toInt(),5);
        QCOMPARE(controller->property("calls").toString(),QString("flush;scope;clear;sync;edit;"));
    }
    void continuationAndSelectionGuard() {
        populate();
        auto verses = backend->property("verses").toList();
        for (int i = 0; i < verses.size(); ++i) {
            auto verse = verses[i].toMap();
            verse["bookmarkRanges"] = i < 3 ? QVariantList{bookmark("Genesis 1:30–2:3",false,i == 2)} : QVariantList{};
            verse["noteRanges"] = i < 3 ? QVariantList{noteRange("Genesis 1:30–2:3",false,i == 2,note("Genesis 1:30–2:3",30,3,1,2))} : QVariantList{};
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
        auto icon = item("blockInlineMarker:note:continuation:Genesis 1:30–2:3");
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
        QCOMPARE(controller->property("calls").toString(),QString("flush;scope;"));
        QCOMPARE(controller->property("freeSelectedText").toString(),selection);
        QCOMPARE(item("blockBody")->property("selectedText").toString(),selection);
        backend->setProperty("navigationSucceeds",true);
        controller->setProperty("calls","");
        click(icon);
        QCOMPARE(controller->property("calls").toString(),QString("flush;scope;clear;sync;edit;"));
        QVERIFY(controller->property("freeSelectedText").toString().isEmpty());
        QVERIFY(item("blockBody")->property("selectedText").toString().isEmpty());
        QCOMPARE(navigatedScope()["reference"].toString(),QString("Genesis 1:30–2:3"));
        QCOMPARE(navigatedScope()["start"].toInt(),30);
        QCOMPARE(navigatedScope()["endChapter"].toInt(),2);
    }
    void sourceOrderedNotes_data() {
        QTest::addColumn<int>("book"); QTest::addColumn<int>("chapter");
        QTest::addColumn<QList<int>>("numbers"); QTest::addColumn<int>("firstCovered");
        QTest::addColumn<int>("lastCovered"); QTest::addColumn<bool>("before"); QTest::addColumn<bool>("after");
        QTest::newRow("esdras-start") << 80 << 16 << QList<int>{77,78} << 78 << 78 << false << true;
        QTest::newRow("matthew-end") << 40 << 1 << QList<int>{1,2,3} << 1 << 2 << true << false;
        QTest::newRow("unrelated-book") << 60 << 1 << QList<int>{1,2,3} << 0 << 0 << false << false;
    }
    void sourceOrderedNotes() {
        QFETCH(int,book); QFETCH(int,chapter); QFETCH(QList<int>,numbers);
        QFETCH(int,firstCovered); QFETCH(int,lastCovered); QFETCH(bool,before); QFETCH(bool,after);
        const QString reference = "2 Esdras 16:78–Matthew 1:2";
        auto scope = note(reference,78,2,16,1);
        scope["book"] = 80; scope["endBook"] = 40;
        QVariantList verses;
        for (int number : numbers) {
            QVariantList notes;
            if (firstCovered && number >= firstCovered && number <= lastCovered)
                notes.append(noteRange(reference,!before && number == firstCovered,!after && number == lastCovered,scope));
            verses.append(QVariantMap{{"number",number},{"text",QString("Verse %1 has enough original words to wrap onto several visible lines.").arg(number)},
                                      {"noteRanges",notes}});
        }
        backend->setProperty("book",book); backend->setProperty("chapter",chapter);
        backend->setProperty("collection",QVariantList{QVariantMap{{"reference",reference},{"book",80},{"chapter",16},
            {"start",78},{"endBook",40},{"endChapter",1},{"end",2},{"note","Cross-book note"}}});
        item("reader")->setWidth(210);
        backend->setProperty("verses",verses);
        QTRY_COMPARE(mapping->spans().size(),numbers.size());
        QTRY_COMPARE(list("annotationRanges").size(),firstCovered ? 1 : 0);
        if (!firstCovered) return;
        const auto range = list("annotationRanges").first().toMap();
        QCOMPARE(range["kind"].toString(),QString("note")); QCOMPARE(range["reference"].toString(),reference);
        QCOMPARE(range["continuesBefore"].toBool(),before); QCOMPARE(range["continuesAfter"].toBool(),after);
        const int start = mapping->positionFor(firstCovered);
        const int end = mapping->spans()[numbers.indexOf(lastCovered)].toMap()["end"].toInt();
        QCOMPARE(range["start"].toInt(),start); QCOMPARE(range["end"].toInt(),end);
        QCOMPARE(range["rects"].toList(),mapping->rectangles(start,end));
        QVERIFY(range["rects"].toList().size() > 1);
        click(item("blockInlineMarker:note:" + QString(before ? "continuation:" : "open:") + reference));
        QCOMPARE(navigatedScope()["reference"].toString(),reference);
        QCOMPARE(navigatedScope()["versionId"].toString(),QString("test-edition"));
        for (const auto &key : {"book","chapter","start","endBook","endChapter","end"})
            QCOMPARE(navigatedScope()[key],scope[key]);
        QCOMPARE(list("inlineMarkers").size(),after ? 1 : 2);
        QVERIFY(!item("blockAnnotationRail"));
        // Collection tuples alone must never manufacture membership.
        for (auto &row : verses) { auto verse = row.toMap(); verse.remove("noteRanges"); row = verse; }
        backend->setProperty("verses",verses);
        QTRY_VERIFY(list("annotationRanges").isEmpty());
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
        QTRY_COMPARE(mapping->selectionText(mapping->positionFor(1),mapping->spans().first().toMap().value("textStart").toInt()),QString());
        controller->setProperty("scale",1.6);
        QTRY_VERIFY(item("blockInlineMarker:note:open:Genesis 1:1–3")->height() > 44.0);
        backend->setProperty("focusMode",true);
        QTRY_VERIFY(list("inlineMarkers").isEmpty());
        backend->setProperty("focusMode",false);
        QTRY_COMPARE(list("inlineMarkers").size(),8);
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
        click(item("blockInlineMarker:note:open:Genesis 1:4–5"));
        QCOMPARE(navigatedScope()["reference"].toString(),QString("Genesis 1:4–5"));
        backend->setProperty("focusMode",true);
        QTRY_COMPARE(doc->blockCount(),2);
        QVERIFY(doc->toPlainText().startsWith("Verse 1 is here. Verse 2 is here. Verse 3 is here.\nVerse 4 is here."));
        QVERIFY(!item("blockAnnotationRail"));
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
    void overlappingTagScopes_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("block") << QString("block");
        QTest::newRow("paragraph") << QString("paragraph");
    }
    void overlappingTagScopes() {
        QFETCH(QString,mode);
        populate();
        item("reader")->setProperty("mode",mode);
        const QVariantList tags{QVariantMap{{"id","tag-one"},{"name","<b>Faith & 🌿</b>"}}};
        auto first = note("Genesis 1:1–3",1,3);
        auto continued = note("Genesis 1:30–2:2",30,2,1,2);
        for (auto scope : {&first,&continued}) {
            scope->remove("note");
            (*scope)["versionId"] = "test-edition";
            (*scope)["tags"] = tags;
        }
        auto verses = backend->property("verses").toList();
        for (int i = 0; i < verses.size(); ++i) {
            auto verse = verses[i].toMap();
            QVariantList ranges;
            if (i < 3) {
                auto range = first;
                range["tagStart"] = i == 0;
                range["continuesBefore"] = i != 0;
                range["continuesAfter"] = i != 2;
                ranges.append(range);
            }
            if (i < 2) {
                auto range = continued;
                range["tagStart"] = false;
                range["continuesBefore"] = true;
                range["continuesAfter"] = i != 1;
                ranges.append(range);
            }
            verse["tagRanges"] = ranges;
            verse["paragraphStart"] = i == 0 || i == 3;
            verses[i] = verse;
        }
        backend->setProperty("verses",verses);
        QTRY_COMPARE(list("annotationRanges").size(),5); // Existing marks/notes plus two tagged scopes.
        QVariantMap continuation;
        for (const auto &entry : list("annotationRanges")) {
            const auto range = entry.toMap();
            if (range["kind"] == "tag" && range["reference"] == continued["reference"]) continuation = range;
        }
        QVERIFY(!continuation.isEmpty());
        QCOMPARE(continuation["start"].toInt(),mapping->positionFor(1));
        QCOMPARE(continuation["scope"].toMap()["start"].toInt(),30); // Endpoint must not become a document offset.
        QCOMPARE(continuation["scope"].toMap()["versionId"].toString(),QString("test-edition"));
        QVERIFY(continuation["continuesBefore"].toBool());
        QVERIFY(!continuation["continuesAfter"].toBool());
        QCOMPARE(continuation["tags"].toList(),tags);
        auto marker = item("blockInlineMarker:tag:open:Genesis 1:1–3");
        QVERIFY(marker);
        QVERIFY(marker->property("hint").toString().contains("<b>Faith & 🌿</b>"));
        click(marker);
        click(item("blockInlineMarker:tag:continuation:Genesis 1:30–2:2"));
        const auto value = controller->property("openedTags");
        const auto opened = value.canConvert<QJSValue>() ? value.value<QJSValue>().toVariant().toMap() : value.toMap();
        QCOMPARE(opened["start"].toInt(),30);
        QCOMPARE(opened["endChapter"].toInt(),2);
        QCOMPARE(opened["tags"].toList(),tags);
        QVERIFY(backend->property("navigated").toString().isEmpty());
        for (auto &row : verses) { auto verse = row.toMap(); verse.remove("tagRanges"); row = verse; }
        backend->setProperty("verses",verses);
        QTRY_COMPARE(list("annotationRanges").size(),3); // Removing tags cannot erase marks or notes.
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
    void reservedEndpointsAndUnicodeCopy_data() {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<int>("width");
        QTest::addColumn<bool>("rtl");
        QTest::newRow("block-one-line") << QString("block") << 1800 << false;
        QTest::newRow("block-wrapped") << QString("block") << 190 << false;
        QTest::newRow("paragraph-one-line") << QString("paragraph") << 1800 << false;
        QTest::newRow("paragraph-wrapped") << QString("paragraph") << 190 << false;
        QTest::newRow("rtl-one-line") << QString("paragraph") << 1800 << true;
        QTest::newRow("rtl-wrapped") << QString("paragraph") << 190 << true;
    }
    void reservedEndpointsAndUnicodeCopy() {
        QFETCH(QString,mode); QFETCH(int,width); QFETCH(bool,rtl);
        backend->setProperty("showVerseNumbers",false);
        item("reader")->setProperty("mode",mode);
        item("reader")->setWidth(width);
        const QString first = QString::fromUtf8(rtl ? "אבג שלום < > [ ] 🌿 é text." : "Alpha < > [ ] 🌿 é אבג text.");
        // Original joiners/object characters must survive: filter positions,
        // never globally delete characters that happen to resemble decorations.
        const QString second = QString::fromUtf8("שלום עולם, second long verse with original ") + QChar(0x2060) + QChar(0xfffc) + " marks.";
        QVariantList verses{
            QVariantMap{{"number",1},{"text",first},{"bookmarkRanges",QVariantList{bookmark("A",true,false),bookmark("B",true,true)}}},
            QVariantMap{{"number",2},{"text",second},{"bookmarkRanges",QVariantList{bookmark("A",false,true),bookmark("C",true,true)}}},
            QVariantMap{{"number",3},{"text","Final words."},{"paragraphStart",true}}};
        backend->setProperty("verses",verses);
        QTRY_COMPARE(list("inlineMarkers").size(),6);
        auto doc = item("blockBody")->property("textDocument").value<QQuickTextDocument *>()->textDocument();
        const QString expected = first + ' ' + second + (mode == "paragraph" ? '\n' : ' ') + "Final words.";
        QCOMPARE(mapping->selectionText(0,doc->characterCount()),expected);
        QCOMPARE(mapping->selectionText(doc->characterCount(),0),expected);
        int closeB = -1, openC = -1;
        for (const auto &entry : list("inlineMarkers")) {
            const auto marker = entry.toMap();
            if (marker["reference"] == "B" && marker["edge"] == "close") closeB = marker["position"].toInt();
            if (marker["reference"] == "C" && marker["edge"] == "open") openC = marker["position"].toInt();
        }
        QVERIFY(closeB >= 0 && openC > closeB); // Close before opening the next verse's range.
        for (const auto &entry : list("inlineMarkers")) {
            const auto marker = entry.toMap();
            const int position = marker["position"].toInt();
            QCOMPARE(doc->characterAt(position),QChar::ObjectReplacementCharacter);
            QCOMPARE(mapping->selectionText(position,position+1),QString());
            QVERIFY2(qAbs(marker["width"].toDouble() - marker["cellWidth"].toDouble()) < 0.1,
                     qPrintable(QJsonDocument::fromVariant(marker).toJson()));
            const auto span = mapping->spans()[marker["verse"].toInt()-1].toMap();
            QCOMPARE(mapping->verseAt(position),marker["verse"].toInt());
            QCOMPARE(mapping->verseAtPoint(marker["x"].toDouble()+marker["width"].toDouble()/2,
                                          marker["y"].toDouble()+marker["height"].toDouble()/2),marker["verse"].toInt());
            const auto block = doc->findBlock(position);
            const auto line = block.layout()->lineForTextPosition(position-block.position());
            const int endpoint = marker["edge"] == "open" ? span["textStart"].toInt() : span["textEnd"].toInt()-1;
            QCOMPARE(line.lineNumber(),block.layout()->lineForTextPosition(endpoint-block.position()).lineNumber());
            const QRectF cell(marker["x"].toDouble(),marker["y"].toDouble(),marker["width"].toDouble(),marker["height"].toDouble());
            const auto bracket = bracketRect(marker);
            for (const auto &ink : textInkRects(doc,span["textStart"].toInt(),span["textEnd"].toInt()))
                QVERIFY2(!bracket.intersects(ink),qPrintable("Bracket intersects bidi text: " + QString::fromUtf8(QJsonDocument::fromVariant(marker).toJson())));
            for (const auto &r : mapping->rectangles(span["textStart"].toInt(),span["textEnd"].toInt())) {
                const auto rect = r.toMap();
                const QRectF textRect(rect["x"].toDouble(),rect["y"].toDouble(),rect["width"].toDouble(),rect["height"].toDouble());
                QVERIFY2(!cell.adjusted(0.1,0.1,-0.1,-0.1).intersects(textRect),"Decoration overlaps original text");
            }
        }
        // Full and partial keyboard selections go through the same filtered API.
        auto body = item("blockBody");
        // Exercise the reader's drag update with real TextEdit hit-test offsets.
        const auto dragSpan = mapping->spans()[0].toMap();
        const int dragEnd = dragSpan["textEnd"].toInt();
        QRectF caret;
        QVERIFY(QMetaObject::invokeMethod(body,"positionToRectangle",Q_RETURN_ARG(QRectF,caret),Q_ARG(int,dragEnd)));
        const QPointF pointer = body->mapToItem(item("reader")->parentItem(),QPointF(caret.left(),caret.center().y()));
        item("reader")->setProperty("anchor",0);
        item("reader")->setProperty("pointer",pointer);
        QVERIFY(QMetaObject::invokeMethod(item("reader"),"updateDrag"));
        QCOMPARE(controller->property("freeSelectedText").toString(),first);
        body->forceActiveFocus();
        QTest::keyClick(window,Qt::Key_A,Qt::ControlModifier);
        QTRY_COMPARE(controller->property("freeSelectedText").toString(),expected);
        QTest::keyClick(window,Qt::Key_C,Qt::ControlModifier);
        QCOMPARE(controller->property("copied").toString(),expected);
        const auto span = mapping->spans()[1].toMap();
        const int a = span["textStart"].toInt()+2, b = span["textEnd"].toInt()-2;
        QVERIFY(QMetaObject::invokeMethod(body,"select",Q_ARG(int,a),Q_ARG(int,b)));
        QCOMPARE(controller->property("freeSelectedText").toString(),second.mid(2,second.size()-4));
        // A mark change invalidates the old selection coherently.
        auto v = verses[0].toMap(); v["bookmarkRanges"] = QVariantList{}; verses[0] = v;
        backend->setProperty("verses",verses);
        QTRY_VERIFY(controller->property("freeSelectedText").toString().isEmpty());
        QVERIFY(body->property("selectedText").toString().isEmpty());
    }
    void contextUnionContinuationAndFocus() {
        QVariantList verses;
        for (int i = 1; i <= 7; ++i) {
            QVariantList bookmarks, tags, notes;
            if (i <= 4) bookmarks.append(bookmark("Cross-chapter",false,i == 4));
            if (i >= 2 && i <= 5) tags.append(QVariantMap{{"reference","Overlap"},{"continuesBefore",i != 2},{"continuesAfter",i != 5}});
            if (i == 7) bookmarks.append(bookmark("Unrelated",true,true));
            if (i >= 3 && i <= 6) notes.append(noteRange("Note extension",i == 3,i == 6));
            verses.append(QVariantMap{{"number",i},{"text",QString("Verse %1.").arg(i)},
                {"bookmarkRanges",bookmarks},{"tagRanges",tags},{"noteRanges",notes}});
        }
        backend->setProperty("verses",verses);
        backend->setProperty("cursorVerse",3);
        backend->setProperty("selectedStart",3); backend->setProperty("selectedEnd",3);
        QTRY_COMPARE(list("contextRanges").size(),2);
        auto ranges = list("contextRanges");
        QCOMPARE(ranges[0].toMap()["start"].toInt(),mapping->positionFor(1));
        QCOMPARE(ranges[0].toMap()["end"],mapping->spans()[1].toMap()["end"]);
        QCOMPARE(ranges[1].toMap()["start"].toInt(),mapping->positionFor(4));
        QCOMPARE(ranges[1].toMap()["end"],mapping->spans()[5].toMap()["end"]);
        QVERIFY(item("blockInlineMarker:bookmark:continuation:Cross-chapter"));
        QVERIFY(!item("blockInlineMarker:bookmark:open:Cross-chapter"));
        QVERIFY(item("blockInlineMarker:bookmark:close:Cross-chapter"));
        backend->setProperty("cursorVerse",6);
        QTRY_COMPARE(list("contextRanges").size(),1);
        QCOMPARE(list("contextRanges")[0].toMap()["start"].toInt(),mapping->positionFor(4));
        QCOMPARE(list("contextRanges")[0].toMap()["end"],mapping->spans()[4].toMap()["end"]);
        backend->setProperty("cursorVerse",7);
        QTRY_VERIFY(list("contextRanges").isEmpty());
        backend->setProperty("cursorVerse",3);
        QTRY_COMPARE(list("contextRanges").size(),2);
        backend->setProperty("focusMode",true);
        QTRY_VERIFY(list("inlineMarkers").isEmpty());
        QTRY_VERIFY(list("contextRanges").isEmpty());
        backend->setProperty("focusMode",false);
        QTRY_COMPARE(list("inlineMarkers").size(),8);
        QTRY_COMPARE(list("contextRanges").size(),2);
        QSignalSpy geometry(mapping,&BlockDocument::geometryChanged);
        QTest::qWait(100);
        QCOMPARE(geometry.count(),0); // Painting/selection cannot feed a layout loop.
    }
    void renderedCellsAndLastCharacterHit() {
        backend->setProperty("showVerseNumbers",false);
        backend->setProperty("verses",QVariantList{
            QVariantMap{{"number",1},{"text","Alpha omega."},{"bookmarkRanges",QVariantList{bookmark("A",true,true)}},
                {"noteRanges",QVariantList{noteRange("A",true,true)}},
                {"tagRanges",QVariantList{QVariantMap{{"reference","A"},{"tags",QVariantList{QVariantMap{{"name","One"}},QVariantMap{{"name","Two"}}}}}}}},
            QVariantMap{{"number",2},{"text","Next verse."}}});
        QTRY_COMPARE(list("inlineMarkers").size(),6); // Two tag assignments, one tag icon, plus note/bookmark.
        for (const auto kind : {"bookmark", "tag", "note"}) {
            auto glyph = item("blockInlineGlyph:" + QString(kind) + ":A");
            QVERIFY(glyph && glyph->isVisible());
            QCOMPARE(glyph->property("kind").toString(),QString(kind));
            QVERIFY(glyph->property("filled").toBool());
        }
        QTest::qWait(120);
        const auto image = window->grabWindow();
        QVERIFY(!image.isNull());
        for (const auto &entry : list("inlineMarkers")) {
            const auto marker = entry.toMap();
            auto visual = item("blockInlineMarker:"+marker["kind"].toString()+':'+marker["edge"].toString()+':'+marker["reference"].toString());
            QVERIFY(visual);
            const auto rect = visual->mapRectToScene(visual->boundingRect());
            const qreal ratio = image.devicePixelRatio();
            const auto pixels = image.copy(QRect((rect.topLeft()*ratio).toPoint(),(rect.size()*ratio).toSize()));
            int accentPixels = 0;
            for (int y = 0; y < pixels.height(); ++y)
                for (int x = 0; x < pixels.width(); ++x) {
                    const QColor c = pixels.pixelColor(x,y);
                    if (c.blue() > 100 && c.blue() > c.red()+20) ++accentPixels;
                }
            QVERIFY2(accentPixels > 4,qPrintable("Missing rendered icon/bracket: " + visual->objectName()));
        }
        const auto span = mapping->spans()[0].toMap();
        const auto last = mapping->rectangles(span["textEnd"].toInt()-1,span["textEnd"].toInt()).last().toMap();
        QCOMPARE(mapping->verseAtPoint(last["x"].toDouble()+last["width"].toDouble()-0.1,
                                      last["y"].toDouble()+last["height"].toDouble()/2),1);
        const auto lastSpan = mapping->spans().last().toMap();
        const auto final = mapping->rectangles(lastSpan["textEnd"].toInt()-1,lastSpan["textEnd"].toInt()).last().toMap();
        QCOMPARE(mapping->verseAtPoint(final["x"].toDouble()+final["width"].toDouble()+8,
                                      final["y"].toDouble()+final["height"].toDouble()/2),2);
    }
    void sharedNoteGlyphOutlineAndFilled_data() {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<double>("scale");
        QTest::newRow("block") << QString("block") << 1.0;
        QTest::newRow("paragraph-scaled") << QString("paragraph") << 1.6;
    }
    void bracketFontGeometry_data() {
        QTest::addColumn<int>("size");
        QTest::addColumn<double>("scale");
        QTest::addColumn<double>("leading");
        QTest::addColumn<bool>("rtl");
        for (int size : {8,18,40})
            for (double scale : {1.0,1.6})
                for (double leading : {1.0,3.0})
                    for (bool rtl : {false,true})
                        QTest::newRow(qPrintable(QString("%1px-%2x-leading%3-%4").arg(size).arg(scale).arg(leading).arg(rtl ? "rtl" : "ltr")))
                            << size << scale << leading << rtl;
    }
    void bracketFontGeometry() {
        QFETCH(int,size); QFETCH(double,scale); QFETCH(double,leading); QFETCH(bool,rtl);
        controller->setProperty("readerSize",size);
        controller->setProperty("scale",scale);
        backend->setProperty("lineHeight",leading);
        backend->setProperty("showVerseNumbers",false);
        const QString text = rtl ? QString::fromUtf8("אבג שלום") : QString("Agjp Éy");
        backend->setProperty("verses",QVariantList{
            QVariantMap{{"number",1},{"text",text},{"noteRanges",QVariantList{noteRange("A",true,true)}}},
            QVariantMap{{"number",2},{"text",text},{"paragraphStart",true},{"heading","Section"},
                {"noteRanges",QVariantList{noteRange("B",true,true)}}}});
        item("reader")->setProperty("mode","paragraph");
        QTRY_COMPARE(list("inlineMarkers").size(),4);
        auto body = item("blockBody");
        auto doc = body->property("textDocument").value<QQuickTextDocument *>()->textDocument();
        const QFontMetricsF metrics(doc->defaultFont());
        const auto ink = metrics.tightBoundingRect(text);
        const double annotatedHeight = body->height();
        const auto firstLine = doc->begin().layout()->lineAt(0);
        const double annotatedAscent = firstLine.ascent();
        const double annotatedDescent = firstLine.descent();
        const auto markers = list("inlineMarkers");
        double firstBottom = 0;
        for (const auto &entry : markers) {
            const auto marker = entry.toMap();
            const double top = marker["y"].toDouble()+marker["bracketTop"].toDouble();
            const double bottom = top+marker["bracketHeight"].toDouble();
            QVERIFY(top < marker["textTop"].toDouble());
            QVERIFY(bottom > marker["textBottom"].toDouble());
            QVERIFY(marker["bracketHeight"].toDouble() > ink.height());
            QVERIFY(marker["bracketHeight"].toDouble() <= metrics.height()+3);
            QVERIFY(marker["cellHeight"].toDouble() <= metrics.height()+2);
            QCOMPARE(marker["width"].toDouble(),marker["paintWidth"].toDouble()+marker["textGap"].toDouble());
            QCOMPARE(marker["rtl"].toBool(),rtl);
            const auto bracket = bracketRect(marker);
            const auto span = mapping->spans()[marker["verse"].toInt()-1].toMap();
            const auto inkRects = textInkRects(doc,span["textStart"].toInt(),span["textEnd"].toInt());
            QVERIFY(!inkRects.isEmpty());
            const bool textOnRight = (marker["edge"] != "close") != rtl;
            double gap = 1e6;
            for (const auto &rect : inkRects) {
                QVERIFY(!bracket.intersects(rect));
                gap = qMin(gap,textOnRight ? rect.left()-bracket.right() : bracket.left()-rect.right());
            }
            QVERIFY2(gap > 0,qPrintable(QString("Text gap %1; %2").arg(gap).arg(QString::fromUtf8(QJsonDocument::fromVariant(marker).toJson()))));
            if (size == 18 && scale == 1) QVERIFY(gap >= 1);
            if (leading == 1 && scale == 1 && marker["verse"].toInt() == 1)
                qInfo("%dpx %s %s ink gap: %.2fpx",size,rtl ? "RTL" : "LTR",qPrintable(marker["edge"].toString()),gap);
            if (marker["verse"].toInt() == 1) firstBottom = qMax(firstBottom,bottom);
            else QVERIFY2(top >= firstBottom-0.5,qPrintable(QString("Brackets overlap: top %1 previous bottom %2; %3")
                .arg(top).arg(firstBottom).arg(QString::fromUtf8(QJsonDocument::fromVariant(marker).toJson()))));
            QRectF caret;
            const int position = mapping->spans()[marker["verse"].toInt()-1].toMap()["textStart"].toInt();
            QVERIFY(QMetaObject::invokeMethod(body,"positionToRectangle",Q_RETURN_ARG(QRectF,caret),Q_ARG(int,position)));
            QVERIFY(top >= caret.top()-2);
            QVERIFY(bottom <= caret.bottom()+2);
        }
        QCOMPARE(mapping->selectionText(0,doc->characterCount()),text+"\nSection\n"+text);
        QVERIFY(QMetaObject::invokeMethod(body,"selectAll"));
        QVERIFY(QMetaObject::invokeMethod(body,"copy"));
        QCOMPARE(QGuiApplication::clipboard()->text(),text+"\nSection\n"+text);
        QVERIFY(!QGuiApplication::clipboard()->mimeData()->hasHtml());
        QVERIFY(QMetaObject::invokeMethod(body,"deselect"));
        if (!qEnvironmentVariableIsEmpty("OMAWORD_ANNOTATION_SCREENSHOT")) {
            QTest::qWait(80);
            QVERIFY(window->grabWindow().save(qEnvironmentVariable("OMAWORD_ANNOTATION_SCREENSHOT")
                + QString(".%1.png").arg(QString::fromLatin1(QTest::currentDataTag()))));
        }
        // Only the two-pixel ink padding may contribute to line height.
        backend->setProperty("showHeadings",false);
        QTRY_COMPARE(doc->blockCount(),2);
        const double withoutHeadingHeight = body->height();
        backend->setProperty("focusMode",true);
        QTRY_VERIFY(list("inlineMarkers").isEmpty());
        QVERIFY2(qAbs(body->height()-withoutHeadingHeight) <= 4*leading+1,
                 qPrintable(QString("annotated %1, plain %2, font ascent %3; line %4/%5 plain %6/%7").arg(annotatedHeight).arg(body->height()).arg(metrics.ascent())
                            .arg(annotatedAscent).arg(annotatedDescent).arg(doc->begin().layout()->lineAt(0).ascent()).arg(doc->begin().layout()->lineAt(0).descent())));
    }
    void markerRangeEditing() {
        auto scope = note("Cross-book",78,2,16,1);
        scope.remove("note"); scope["book"] = 80; scope["endBook"] = 40; scope["versionId"] = "frozen-edition";
        auto editedScope = [this]() {
            const auto value = controller->property("editedScope");
            return value.canConvert<QJSValue>() ? value.value<QJSValue>().toVariant().toMap() : value.toMap();
        };
        for (const QString &kind : {QString("bookmark"),QString("note"),QString("tag")}) {
            QVariantMap range{{"reference","Cross-book"},{"scope",scope}};
            backend->setProperty("verses",QVariantList{QVariantMap{{"number",2},{"text","Endpoint."},
                {kind+"Ranges",QVariantList{range}}}});
            QTRY_COMPARE(list("inlineMarkers").size(),2);
            QTRY_VERIFY(item("blockInlineMarker:"+kind+":open:Cross-book"));
            auto marker = item("blockInlineMarker:"+kind+":open:Cross-book");
            QVERIFY(marker);
            QVERIFY(marker->property("hint").toString().contains("Shift-click"));
            const QPoint point = marker->mapToScene(marker->boundingRect().center()).toPoint();
            controller->setProperty("calls","");
            QTest::mouseClick(window,Qt::LeftButton,Qt::ShiftModifier,point);
            QCOMPARE(controller->property("calls").toString(),QString("range;"));
            QCOMPARE(controller->property("editedKind").toString(),kind);
            QCOMPARE(editedScope(),scope);
            controller->setProperty("calls","");
            QTest::mouseClick(window,Qt::RightButton,Qt::NoModifier,point);
            QTRY_VERIFY(item("reader")->property("annotationMenuOpen").toBool());
            QVERIFY(controller->property("calls").toString().isEmpty());
            click(item("blockAnnotationEditRange"));
            QTRY_VERIFY(!item("reader")->property("annotationMenuOpen").toBool());
            QCOMPARE(controller->property("calls").toString(),QString("range;"));
            QCOMPARE(editedScope(),scope);
            controller->setProperty("calls","");
            QTest::mouseClick(window,Qt::RightButton,Qt::NoModifier,point);
            QTRY_VERIFY(item("reader")->property("annotationMenuOpen").toBool());
            click(item("blockAnnotationOpen"));
            QTRY_VERIFY(!item("reader")->property("annotationMenuOpen").toBool());
            QVERIFY(!controller->property("calls").toString().contains("range;"));
            if (kind == "note") QCOMPARE(controller->property("calls").toString(),QString("flush;scope;clear;sync;edit;"));
        }
    }
    void sharedNoteGlyphOutlineAndFilled() {
        QFETCH(QString,mode); QFETCH(double,scale);
        auto shared = item("sharedGlyph");
        QCOMPARE(shared->width(),18.0); QCOMPARE(shared->height(),18.0);
        QCOMPARE(shared->property("kind").toString(),QString("note"));
        QCOMPARE(shared->property("ink").value<QColor>(),QColor(Qt::white));
        controller->setProperty("scale",scale);
        item("reader")->setProperty("mode",mode);
        populate();
        QTRY_COMPARE(list("inlineMarkers").size(),6);
        auto glyph = item("blockInlineGlyph:note:Genesis 1:1–3");
        QVERIFY(glyph && glyph->isVisible());
        QCOMPARE(glyph->property("kind").toString(),QString("note"));
        QVERIFY(glyph->property("filled").toBool());
        QVERIFY(!shared->property("filled").toBool());
        shared->setWidth(glyph->width()); shared->setHeight(glyph->height());
        shared->setProperty("ink",glyph->property("ink"));
        QTest::qWait(80);
        auto actual = glyph->grabToImage();
        auto common = shared->grabToImage();
        auto footer = item("footerNoteReference")->grabToImage();
        QVERIFY(actual && common && footer);
        QTRY_VERIFY(!actual->image().isNull() && !common->image().isNull() && !footer->image().isNull());
        QCOMPARE(common->image(),footer->image());
        const auto ink = [](const QImage &image) {
            qint64 amount = 0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x) amount += image.pixelColor(x,y).alpha();
            return amount;
        };
        QVERIFY(ink(actual->image()) > ink(footer->image()) * 1.2);
        shared->setProperty("filled",true);
        QTest::qWait(80);
        common = shared->grabToImage(); QVERIFY(common);
        QTRY_VERIFY(!common->image().isNull());
        QCOMPARE(actual->image(),common->image());
        backend->setProperty("focusMode",true);
        QTRY_VERIFY(list("inlineMarkers").isEmpty());
        QVERIFY(!item("blockInlineGlyph:note:Genesis 1:1–3"));
    }
    void filledNoteSquareAndTransparentHalo_data() {
        QTest::addColumn<int>("size");
        QTest::addColumn<QColor>("color");
        for (int size : {14,24,96})
            for (const QColor &color : {QColor("#8db5d4"),QColor("#813245")})
                QTest::newRow(qPrintable(QString::number(size)+color.name())) << size << color;
    }
    void filledNoteSquareAndTransparentHalo() {
        QFETCH(int,size); QFETCH(QColor,color);
        auto shared = item("sharedGlyph");
        shared->setWidth(size); shared->setHeight(size);
        shared->setProperty("filled",true); shared->setProperty("ink",color);
        QTest::qWait(80);
        auto grab = shared->grabToImage(); QVERIFY(grab);
        QTRY_VERIFY(!grab->image().isNull());
        const auto image = grab->image();
        if (!qEnvironmentVariableIsEmpty("OMAWORD_ANNOTATION_SCREENSHOT")) {
            const auto path = qEnvironmentVariable("OMAWORD_ANNOTATION_SCREENSHOT")+QString(".glyph%1%2.png").arg(size).arg(color.name());
            QVERIFY(image.save(path));
            for (const auto &background : {QColor("#181c21"),QColor("#eee5d1")})
                QVERIFY(onBackground(image,background).scaled(288,288,Qt::IgnoreAspectRatio,Qt::FastTransformation)
                    .save(path+background.name()+".zoom.png"));
        }
        // Independent path-boolean oracle: a full square minus an expanded
        // pencil, then the original solid pencil (including its exposed cap).
        QPainterPath square; square.addRect(3,4,17,17);
        QPainterPath pencil;
        pencil.moveTo(10,11); pencil.lineTo(18,3); pencil.lineTo(21,6);
        pencil.lineTo(13,14); pencil.lineTo(9,15); pencil.closeSubpath();
        QPainterPathStroker stroker; stroker.setWidth(3); stroker.setJoinStyle(Qt::MiterJoin);
        QTransform enlargement;
        enlargement.translate(12,12); enlargement.scale(1.1,1.1); enlargement.translate(-12,-12);
        const auto silhouette = enlargement.map(square.subtracted(pencil.united(stroker.createStroke(pencil))).united(pencil));
        square = enlargement.map(square);
        pencil = enlargement.map(pencil);
        QImage expected(image.size()*3,QImage::Format_ARGB32_Premultiplied);
        expected.fill(Qt::transparent);
        {
            QPainter painter(&expected);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.scale(expected.width()/24.0,expected.height()/24.0);
            painter.fillPath(silhouette,color);
        }
        expected = expected.scaled(image.size(),Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
        qint64 error = 0;
        int haloPixels = 0, pencilPixels = 0, squarePixels = 0, exposedPixels = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                const auto actual = image.pixelColor(x,y);
                const auto reference = expected.pixelColor(x,y);
                error += qAbs(actual.alpha()-reference.alpha());
                const QPointF p((x+0.5)*24/image.width(),(y+0.5)*24/image.height());
                if (square.contains(p) && reference.alpha() < 30) {
                    QVERIFY2(actual.alpha() < 100,"Pencil halo must cut through to transparency");
                }
                // At 14px a 1.5-unit band is subpixel: count low-alpha samples
                // within the geometric halo, rather than require opaque-grid holes.
                if (square.contains(p) && !silhouette.contains(p) && actual.alpha() < 100) ++haloPixels;
                if (reference.alpha() > 240) {
                    QVERIFY(actual.alpha() > 180);
                    QVERIFY(qAbs(actual.red()-color.red()) < 5);
                    QVERIFY(qAbs(actual.green()-color.green()) < 5);
                    QVERIFY(qAbs(actual.blue()-color.blue()) < 5);
                    if (pencil.contains(p)) ++pencilPixels;
                    else if (square.contains(p)) ++squarePixels;
                }
                if (!square.contains(p) && pencil.contains(p) && actual.alpha() > 80) ++exposedPixels;
            }
        QVERIFY2(error/double(image.width()*image.height()) < 20,"Filled note differs from square-and-pencil silhouette");
        QVERIFY(haloPixels >= 2); QVERIFY(pencilPixels >= 2); QVERIFY(squarePixels >= 10);
        // The one-unit overhang is subpixel at 14px; resolve it at 24px and up.
        if (size >= 24) QVERIFY(exposedPixels > 0);
        if (size == 96) {
            // Both diagonal sides retain a ~1.5-unit band before solid square
            // resumes; the original pencil interior must be filled, not hollow.
            const auto alpha = [&](double x, double y) {
                const auto p = enlargement.map(QPointF(x,y));
                return image.pixelColor(qFloor(p.x()*size/24),qFloor(p.y()*size/24)).alpha();
            };
            QVERIFY(alpha(14,9) > 240);
            QVERIFY(alpha(13.5,6.5) < 30);
            QVERIFY(alpha(15.5,12.5) < 30);
            QVERIFY(alpha(12.5,5.5) > 240);
            QVERIFY(alpha(17,13) > 240);
            QVERIFY(alpha(18.2,3.7) > 180); // Pencil above the square's top.
            QVERIFY(alpha(20.2,6) > 180); // Pencil beyond its right edge.
        }
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
        auto shared = item("sharedGlyph");
        shared->setProperty("filled",true);
        shared->setWidth(14); shared->setHeight(14);
        QTest::qWait(100);
        auto glyph = shared->grabToImage(); QVERIFY(glyph);
        QTRY_VERIFY(!glyph->image().isNull());
        QVERIFY(glyph->image().save(path + ".note14.png"));
        QVERIFY(onBackground(glyph->image(),QColor("#181c21")).scaled(280,280,Qt::IgnoreAspectRatio,Qt::FastTransformation).save(path + ".note14-zoom.png"));
        for (double leading : {1.0,3.0}) {
            backend->setProperty("lineHeight",leading);
            QTest::qWait(100);
            QVERIFY(window->grabWindow().save(path + QString(".leading%1.png").arg(leading)));
        }
        backend->setProperty("lineHeight",1.5);
        controller->setProperty("scale",1.6);
        item("reader")->setWidth(470);
        QTest::qWait(150);
        QVERIFY(window->grabWindow().save(path + ".scaled.png"));
        backend->setProperty("focusMode",true);
        QTest::qWait(150);
        QVERIFY(window->grabWindow().save(path + ".focus.png"));
    }
    void copyFallbackAndNativeSerialization() {
        backend->setProperty("showVerseNumbers",false);
        const QString original = QString::fromUtf8("Unicode 🌿 אבג [text] ") + QChar(0x2060) + QChar(0xfffc) + " original.";
        backend->setProperty("verses",QVariantList{QVariantMap{{"number",1},{"text",original},
            {"bookmarkRanges",QVariantList{bookmark("A",true,true)}},
            {"noteRanges",QVariantList{noteRange("A",true,true),noteRange("B",true,true)}},
            {"tagRanges",QVariantList{QVariantMap{{"reference","A"}}}}}});
        QTRY_COMPARE(list("inlineMarkers").size(),8);
        auto body = item("blockBody");
        auto doc = body->property("textDocument").value<QQuickTextDocument *>()->textDocument();
        auto clipboard = QGuiApplication::clipboard();
        body->forceActiveFocus();
        controller->setProperty("calls","");
        QTest::keyClick(window,Qt::Key_C,Qt::ControlModifier);
        QCOMPARE(controller->property("calls").toString(),QString("passage:false;"));
        QTest::keyClick(window,Qt::Key_A,Qt::ControlModifier);
        QTest::keyClick(window,Qt::Key_C,Qt::ControlModifier);
        QCOMPARE(controller->property("copied").toString(),original);
        QCOMPARE(controller->property("calls").toString(),QString("passage:false;"));
        clipboard->setText("sentinel");
        // Invoke the actual native TextEdit method, including its HTML MIME.
        QVERIFY(QMetaObject::invokeMethod(body,"copy"));
        QCOMPARE(clipboard->text(),original);
        QVERIFY(!clipboard->mimeData()->hasHtml());
        // Select just a joiner: no image URI is present in this native fragment.
        const int joiner = list("inlineMarkers").first().toMap()["position"].toInt()+1;
        QVERIFY(QMetaObject::invokeMethod(body,"select",Q_ARG(int,joiner),Q_ARG(int,joiner+1)));
        QVERIFY(QMetaObject::invokeMethod(body,"copy"));
        QCOMPARE(clipboard->text(),QString());
        QVERIFY(!clipboard->mimeData()->hasHtml());
        // Plain-only native serialization is filtered too.
        QVERIFY(QMetaObject::invokeMethod(body,"selectAll"));
        QTextCursor cursor(doc); cursor.select(QTextCursor::Document);
        clipboard->setText(cursor.selection().toPlainText());
        QCOMPARE(clipboard->text(),original);
        // Unrelated clipboard values, including HTML, retain all their MIME.
        auto external = new QMimeData;
        external->setText("External text"); external->setHtml("<b>External text</b>");
        clipboard->setMimeData(external);
        QCOMPARE(clipboard->text(),QString("External text"));
        QVERIFY(clipboard->mimeData()->hasHtml());
        external = new QMimeData;
        external->setText(cursor.selection().toPlainText()); external->setHtml("<b>Different producer</b>");
        clipboard->setMimeData(external);
        QCOMPARE(clipboard->text(),cursor.selection().toPlainText());
        QVERIFY(clipboard->mimeData()->hasHtml());
        // Clearing this reader's selection disables interception.
        QVERIFY(QMetaObject::invokeMethod(body,"deselect"));
        clipboard->setText(cursor.selection().toPlainText());
        QCOMPARE(clipboard->text(),cursor.selection().toPlainText());
    }
    void primaryKeyboardSerialization() {
        auto clipboard = QGuiApplication::clipboard();
        if (!clipboard->supportsSelection()) QSKIP("Platform does not support PRIMARY selection");
        populate();
        QTRY_VERIFY(!list("inlineMarkers").isEmpty());
        auto body = item("blockBody");
        auto doc = body->property("textDocument").value<QQuickTextDocument *>()->textDocument();
        body->forceActiveFocus();
        QTest::keyClick(window,Qt::Key_A,Qt::ControlModifier);
        const auto expected = mapping->selectionText(0,doc->characterCount());
        QTRY_COMPARE(clipboard->text(QClipboard::Selection),expected);
        QVERIFY(!clipboard->mimeData(QClipboard::Selection)->hasHtml());
        // A later native PRIMARY publication must not restore decorations.
        QTextCursor cursor(doc); cursor.select(QTextCursor::Document);
        auto native = new QMimeData;
        native->setText(cursor.selection().toPlainText()); native->setHtml(cursor.selection().toHtml());
        clipboard->setMimeData(native,QClipboard::Selection);
        QCOMPARE(clipboard->text(QClipboard::Selection),expected);
        QVERIFY(!clipboard->mimeData(QClipboard::Selection)->hasHtml());
    }
    void nestedEndpointOrder() {
        QVariantList verses;
        for (int i = 1; i <= 5; ++i) {
            // Deliberately misleading lexical order, and metadata in a different
            // order from the expected nesting. Include a bookmark/note/tag tie.
            QVariantList bookmarks{bookmark("Z outer 1-5",i == 1,i == 5)};
            if (i <= 3) bookmarks.append(bookmark("A inner 1-3",i == 1,i == 3));
            if (i >= 3) bookmarks.append(bookmark("A inner 3-5",i == 3,i == 5));
            QVariantList tags{QVariantMap{{"reference","Z outer 1-5"},{"continuesBefore",i != 1},{"continuesAfter",i != 5}}};
            QVariantList notes{noteRange("Z outer 1-5",i == 1,i == 5),noteRange("Z outer 1-5",i == 1,i == 5)};
            verses.append(QVariantMap{{"number",i},{"text","Words."},{"bookmarkRanges",bookmarks},{"tagRanges",tags},{"noteRanges",notes}});
        }
        backend->setProperty("verses",verses);
        QTRY_COMPARE(list("inlineMarkers").size(),10);
        QStringList first, last;
        for (const auto &entry : list("inlineMarkers")) {
            const auto marker = entry.toMap();
            const QString key = marker["kind"].toString()+':'+marker["reference"].toString();
            if (marker["verse"].toInt() == 1 && marker["edge"] == "open") first.append(key);
            if (marker["verse"].toInt() == 5 && marker["edge"] == "close") last.append(key);
        }
        QCOMPARE(first,QStringList({"bookmark:Z outer 1-5","note:Z outer 1-5","tag:Z outer 1-5","bookmark:A inner 1-3"}));
        QCOMPARE(last,QStringList({"bookmark:A inner 3-5","tag:Z outer 1-5","note:Z outer 1-5","bookmark:Z outer 1-5"}));
        // Ordering uses source ordinals even when verse labels are nonmonotonic.
        for (int i = 0; i < verses.size(); ++i) { auto verse = verses[i].toMap(); verse["number"] = 80-i; verses[i] = verse; }
        backend->setProperty("verses",verses);
        QTRY_COMPARE(list("inlineMarkers").first().toMap()["verse"].toInt(),80);
        QCOMPARE(list("inlineMarkers").first().toMap()["reference"].toString(),QString("Z outer 1-5"));
    }
    void exactAnnotationScopes() {
        auto scope = note("Cross-book",78,2,16,1);
        scope.remove("note"); scope["book"] = 80; scope["endBook"] = 40; scope["versionId"] = "frozen-edition";
        auto mark = bookmark("Cross-book",false,true);
        for (auto it = scope.begin(); it != scope.end(); ++it) mark[it.key()] = it.value();
        auto notes = noteRange("Cross-book",false,true);
        notes["scope"] = scope;
        backend->setProperty("verses",QVariantList{QVariantMap{{"number",2},{"text","Endpoint."},
            {"bookmarkRanges",QVariantList{mark}},{"noteRanges",QVariantList{notes}}}});
        QTRY_COMPARE(list("inlineMarkers").size(),4);
        click(item("blockInlineMarker:bookmark:continuation:Cross-book"));
        auto value = controller->property("openedBookmarkScope");
        auto opened = value.canConvert<QJSValue>() ? value.value<QJSValue>().toVariant().toMap() : value.toMap();
        QCOMPARE(opened["versionId"],scope["versionId"]);
        QCOMPARE(opened["start"].toInt(),78); QCOMPARE(opened["endBook"].toInt(),40);
        QCOMPARE(opened["end"].toInt(),2);
        controller->setProperty("calls","");
        click(item("blockInlineMarker:note:continuation:Cross-book"));
        value = backend->property("navigatedScope");
        opened = value.canConvert<QJSValue>() ? value.value<QJSValue>().toVariant().toMap() : value.toMap();
        QCOMPARE(opened,scope);
        QCOMPARE(controller->property("calls").toString(),QString("flush;scope;clear;sync;edit;"));
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
