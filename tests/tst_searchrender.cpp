#include "testenvironment.h"
#include <QtTest/QTest>
#include <QAbstractTextDocumentLayout>
#include <QClipboard>
#include <QFileInfo>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextLayout>
#include "blockdocument.h"
#include "searchdocument.h"

class SearchRenderTest : public QObject
{
    Q_OBJECT
    std::unique_ptr<QQmlEngine> engine;
    std::unique_ptr<QObject> root;
    QStringList warnings;
    QQuickWindow *window = nullptr;
    QObject *backend = nullptr;
    QObject *controller = nullptr;
    static QQuickItem *find(QQuickItem *item, const QString &name) {
        if (item->objectName() == name) return item;
        for (auto child : item->childItems())
            if (auto found = find(child, name)) return found;
        return nullptr;
    }
    QQuickItem *item(const QString &name) const { return find(window->contentItem(), name); }
    static QQuickTextDocument *quickDocument(QObject *body) {
        return body->property("textDocument").value<QQuickTextDocument *>();
    }
    static QVariantMap segment(int verse, int start, int length, bool active = false) {
        return {{"verse", verse}, {"start", start}, {"length", length}, {"active", active}};
    }
    static QVariantMap verse(int number, const QString &text, const QString &html = {}) {
        const QVariantList annotations{QVariantMap{{"reference", "Test 1:1"}}};
        return {{"number", number}, {"text", text}, {"html", html}, {"heading", "A test heading"},
                {"paragraphStart", true}, {"noteRanges", annotations}};
    }
    static QRectF caret(QObject *body, int position) {
        QRectF result;
        QMetaObject::invokeMethod(body, "positionToRectangle", Q_RETURN_ARG(QRectF, result), Q_ARG(int, position));
        return result;
    }
    static QList<QRectF> layoutSnapshot(QTextDocument *document) {
        QList<QRectF> result;
        for (auto block = document->begin(); block.isValid(); block = block.next()) {
            result.append(document->documentLayout()->blockBoundingRect(block));
            for (int i = 0; i < block.layout()->lineCount(); ++i) result.append(block.layout()->lineAt(i).rect());
        }
        return result;
    }
    QImage grab(QQuickItem *target) {
        auto result = target->grabToImage();
        if (!result) return {};
        QSignalSpy ready(result.get(), &QQuickItemGrabResult::ready);
        if (result->image().isNull()) ready.wait(3000);
        return result->image();
    }
    static int ink(const QImage &image) {
        int count = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                if (image.pixelColor(x, y).alpha() > 0) ++count;
        return count;
    }
private slots:
    void init() {
        warnings.clear();
        engine = std::make_unique<QQmlEngine>();
        connect(engine.get(), &QQmlEngine::warnings, this, [this](const QList<QQmlError> &errors) {
            for (const auto &error : errors) warnings.append(error.toString());
        });
        const auto path = QFileInfo(QFINDTESTDATA("../qml/SearchHighlights.qml")).absolutePath();
        QQmlComponent component(engine.get());
#ifndef Q_MOC_RUN
        component.setData(R"(
import QtQuick
import QtQuick.Controls
import ")" + QUrl::fromLocalFile(path).toEncoded() + R"(" as Reader
ApplicationWindow {
    id: fixture
    width: 800; height: 700; visible: true
    property string sourceText: ""
    property string richText: ""
    property var rowSegments: []
    QtObject {
        id: theme
        property color accent: "#80b0e0"
        property color foreground: "#eeeeee"
        property color background: "#202020"
        property color surface: "#303030"
        property color border: "#505050"
        property string fontFamily: "DejaVu Sans"
        property real cornerRadius: 0
        property real textScale: 1
    }
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
        property int cursorVerse: 0
        property int selectedStart: 0
        property int selectedEnd: 0
    }
    QtObject {
        id: controller; objectName: "controller"
        property real readerSize: 22
        property real scale: 1
        property real floatingSafety: 0
        property string freeSelectedText: ""
        property var searchSegments: []
        property int clearCalls: 0
        function clearTextSelection() { ++clearCalls; block.clearSelection() }
        function tint(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }
    }
    Reader.SearchHighlights {
        id: highlights; objectName: "rowHighlights"
        x: row.x; y: row.y; width: row.width; height: row.height
        body: row; sourceText: fixture.sourceText; segments: fixture.rowSegments; theme: theme
    }
    TextEdit {
        id: row; objectName: "rowBody"
        x: 37; y: 19; width: 330; height: contentHeight + 14
        padding: 7
        font.family: "DejaVu Sans"; font.pixelSize: 22
        readOnly: true; wrapMode: TextEdit.Wrap
        textFormat: fixture.richText.length ? TextEdit.RichText : TextEdit.PlainText
        text: fixture.richText.length ? fixture.richText : fixture.sourceText
    }
    Flickable {
        id: viewport; x: 31; y: 300; width: 700; height: 380
        contentHeight: block.height
        Reader.BlockReader {
            id: block; objectName: "blockReader"
            width: 480; controller: controller; backend: backend; theme: theme; viewport: viewport
        }
    }
}
)", QUrl::fromLocalFile(path + "/search-render-fixture.qml"));
#endif
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        root.reset(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        window = qobject_cast<QQuickWindow *>(root.get());
        QVERIFY(window);
        backend = root->findChild<QObject *>("backend");
        controller = root->findChild<QObject *>("controller");
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QTest::qWait(30);
    }
    void cleanup() {
        root.reset();
        QCoreApplication::processEvents();
        engine.reset();
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
    }
    void sourceMapping() {
        SourcePositionMap map;
        const QString source = QString::fromUtf8("  A & Straße\n\t😀 é  fin ");
        const QString rendered = QString::fromUtf8("A & Straße\u2028😀 é fin");
        map.build(source, rendered, 23);
        const int s = source.indexOf("Straße"), r = rendered.indexOf("Straße");
        QCOMPARE(map.range(s, 6), qMakePair(23 + r, 23 + r + 6));
        const int emojiS = source.indexOf(QString::fromUtf8("😀"));
        const int emojiR = rendered.indexOf(QString::fromUtf8("😀"));
        QCOMPARE(map.range(emojiS, 2), qMakePair(23 + emojiR, 25 + emojiR));
        QCOMPARE(map.range(emojiS + 1, 1), map.range(emojiS, 2));
        QCOMPARE(map.range(source.indexOf("fin"), 3), qMakePair(23 + int(rendered.indexOf("fin")), 23 + int(rendered.size())));
        QCOMPARE(map.range(-1, 1), qMakePair(-1, -1));
        QCOMPARE(map.range(0, 10000), qMakePair(-1, -1));
        map.build("repeat repeat", "repeat extra repeat");
        QCOMPARE(map.range(7, 6), qMakePair(-1, -1));
    }
    void rowMapping_data() {
        QTest::addColumn<QString>("source");
        QTest::addColumn<QString>("html");
        QTest::addColumn<QString>("needle");
        QTest::newRow("literal-zero") << "First literal phrase and another phrase." << "" << "First literal phrase";
        QTest::newRow("entities-format") << "A & B < C Straße." << "<div>A &amp; B &lt; C <i>Straße</i>.</div>" << "Straße";
        QTest::newRow("paragraph-whitespace") << "One\n  two & three\nlast" << "<div>One</div><div><i>two</i> &amp; three</div><div>last</div>" << "two & three\nlast";
        QTest::newRow("surrogate-combining") << QString::fromUtf8("Prefix 😀 é Straße end") << "" << QString::fromUtf8("😀 é Straße");
        QTest::newRow("bidi") << QString::fromUtf8("Before אבגד after") << "" << QString::fromUtf8("אבגד");
        QTest::newRow("wrapping") << "A phrase that should wrap across several narrow lines without altering any glyph positions." << "" << "phrase that should wrap across several narrow lines";
    }
    void rowMapping() {
        QFETCH(QString, source); QFETCH(QString, html); QFETCH(QString, needle);
        root->setProperty("sourceText", source);
        root->setProperty("richText", html);
        QTest::qWait(40);
        auto body = item("rowBody");
        auto doc = quickDocument(body)->textDocument();
        SearchDocument mapping;
        mapping.attach(quickDocument(body), source);
        const int start = source.indexOf(needle);
        QVERIFY(start >= 0);
        const auto segments = QVariantList{segment(1, start, needle.size())};
        const auto rectangles = mapping.rectangles(segments);
        QVERIFY(!rectangles.isEmpty());
        const auto first = mapping.rectFor(start, needle.size());
        QVERIFY(first.width() > 0);
        QCOMPARE(first, SearchGeometry::firstRect(rectangles));
        if (source.contains(QString::fromUtf8("é"))) {
            const int combining = source.indexOf(QString::fromUtf8("é"));
            // A combining-mark-only occurrence still paints its complete glyph.
            QCOMPARE(mapping.rectFor(combining + 1, 1), mapping.rectFor(combining, 2));
        }
        const auto originalHtml = doc->toHtml();
        const auto originalText = doc->toRawText();
        const auto geometry = layoutSnapshot(doc);
        const auto size = doc->size();
        const int revision = doc->revision();
        QSignalSpy changed(doc, &QTextDocument::contentsChanged);
        QMetaObject::invokeMethod(body, "selectAll");
        const auto selected = body->property("selectedText").toString();
        QMetaObject::invokeMethod(body, "copy");
        const QString copied = QGuiApplication::clipboard()->text();
        root->setProperty("rowSegments", segments);
        QTest::qWait(50);
        QVariant bodyRect;
        QVERIFY(QMetaObject::invokeMethod(item("rowHighlights"), "rectFor", Q_RETURN_ARG(QVariant, bodyRect),
                                         Q_ARG(QVariant, start), Q_ARG(QVariant, needle.size())));
        const auto nativeOrigin = caret(body, 0).topLeft() - mapping.cursorRect(0).topLeft();
        QCOMPARE(bodyRect.toRectF(), first.translated(nativeOrigin));
        auto painted = grab(item("searchHighlightsCanvas"));
        QVERIFY(!painted.isNull());
        QVERIFY(ink(painted) > 0);
        // Verify the canvas is in root coordinates, including TextEdit padding.
        const auto highlightRect = bodyRect.toRectF();
        QVERIFY(painted.pixelColor(qRound(highlightRect.center().x()), qRound(highlightRect.center().y())).alpha() > 0);
        // Hand-drawn edges are seeded per match: repaints are pixel-identical,
        // and the ink stays inside the match rectangle.
        QMetaObject::invokeMethod(item("searchHighlightsCanvas"), "requestPaint");
        QTest::qWait(40);
        QCOMPARE(grab(item("searchHighlightsCanvas")), painted);
        QList<QRectF> matchRects;
        for (const auto &value : rectangles) {
            const auto r = value.toMap();
            matchRects.append(QRectF(r["x"].toDouble(), r["y"].toDouble(), r["width"].toDouble(), r["height"].toDouble())
                .translated(nativeOrigin).adjusted(-1, -1, 1, 1));
        }
        for (int y = 0; y < painted.height(); ++y)
            for (int x = 0; x < painted.width(); ++x) {
                if (painted.pixelColor(x, y).alpha() == 0) continue;
                bool inside = false;
                for (const auto &r : matchRects) inside |= r.contains(QPointF(x + 0.5, y + 0.5));
                QVERIFY(inside);
            }
        root->setProperty("rowSegments", QVariantList{segment(1, start, needle.size(), true)});
        QTest::qWait(40);
        QVERIFY(grab(item("searchHighlightsCanvas")) != painted);
        root->setProperty("rowSegments", QVariantList{});
        QTest::qWait(40);
        QCOMPARE(ink(grab(item("searchHighlightsCanvas"))), 0);
        QCOMPARE(changed.count(), 0);
        QCOMPARE(doc->revision(), revision);
        QCOMPARE(doc->toHtml(), originalHtml);
        QCOMPARE(doc->toRawText(), originalText);
        QCOMPARE(doc->size(), size);
        QCOMPARE(layoutSnapshot(doc), geometry);
        QCOMPARE(body->property("selectedText").toString(), selected);
        QMetaObject::invokeMethod(body, "copy");
        QCOMPARE(QGuiApplication::clipboard()->text(), copied);
        QVERIFY(mapping.rectangles({}).isEmpty());
    }
    void blockMapping_data() {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("numbers");
        QTest::addColumn<bool>("annotations");
        for (const auto mode : {"block", "paragraph"})
            for (bool numbers : {false, true})
                for (bool annotations : {false, true})
                    QTest::newRow(qPrintable(QString("%1-n%2-a%3").arg(mode).arg(numbers).arg(annotations)))
                        << QString(mode) << numbers << annotations;
    }
    void blockMapping() {
        QFETCH(QString, mode); QFETCH(bool, numbers); QFETCH(bool, annotations);
        const QString source = QString::fromUtf8("First Straße & bright\n  😀 é last");
        const QString html = QString::fromUtf8("<div>First <i>Straße</i> &amp; bright</div><div>😀 é last</div>");
        backend->setProperty("focusMode", !annotations);
        backend->setProperty("showVerseNumbers", numbers);
        backend->setProperty("verses", QVariantList{verse(1, source, html), verse(999, "Next literal phrase.")});
        item("blockReader")->setProperty("mode", mode);
        QTest::qWait(60);
        auto mapping = item("blockReader")->findChild<BlockDocument *>();
        QVERIFY(mapping);
        auto body = item("blockBody");
        auto doc = quickDocument(body)->textDocument();
        // Independently rebuild with all combinations, including numbers with
        // annotations hidden (the UI focus-mode switch hides both together).
        mapping->build(quickDocument(body), backend->property("verses").toList(), numbers, true, 1.5,
                       mode, annotations, 22);
        QTest::qWait(30);
        const auto firstSpan = mapping->spans().first().toMap();
        const int textStart = firstSpan.value("textStart").toInt();
        const auto firstRect = mapping->searchRect(segment(1, 0, 5));
        QVERIFY(firstRect.width() > 0);
        QVERIFY(qAbs(firstRect.x() - caret(body, textStart).x()) < 0.1);
        QVERIFY(qAbs(firstRect.y() - caret(body, textStart).y()) < 0.1);
        const int foldedStart = source.indexOf("Straße");
        const auto foldedRect = mapping->searchRect(segment(1, foldedStart, 6));
        QVERIFY(foldedRect.width() > 0);
        QVERIFY(qAbs(foldedRect.x() - caret(body, textStart + foldedStart).x()) < 0.1);
        const int emoji = source.indexOf(QString::fromUtf8("😀"));
        const auto lastRect = mapping->searchRect(segment(1, source.indexOf("last"), 4));
        const int renderedLast = doc->toRawText().indexOf("last", textStart);
        QVERIFY(qAbs(lastRect.x() - caret(body, renderedLast).x()) < 0.1);
        const QVariantList segments{segment(1, 0, 5), segment(1, foldedStart, 6),
                                    segment(1, emoji, source.size() - emoji), segment(999, 0, 4)};
        const auto rects = mapping->searchRectangles(segments);
        QVERIFY(rects.size() >= 4);
        const auto geometry = layoutSnapshot(doc);
        const auto nativeText = doc->toRawText();
        const auto htmlBefore = doc->toHtml();
        const auto markers = mapping->inlineDecorations();
        const auto spans = mapping->spans();
        const int revision = doc->revision();
        const int clears = controller->property("clearCalls").toInt();
        QMetaObject::invokeMethod(body, "selectAll");
        const auto selected = body->property("selectedText").toString();
        QMetaObject::invokeMethod(body, "copy");
        const auto copied = QGuiApplication::clipboard()->text();
        controller->setProperty("searchSegments", segments);
        QTest::qWait(50);
        const auto painted = grab(item("blockSearchBackground"));
        QVERIFY(ink(painted) > 0);
        QMetaObject::invokeMethod(item("blockSearchBackground"), "requestPaint");
        QTest::qWait(40);
        QCOMPARE(grab(item("blockSearchBackground")), painted);
        controller->setProperty("searchSegments", QVariantList{segment(1, foldedStart, 6, true)});
        QTest::qWait(40);
        QVERIFY(grab(item("blockSearchBackground")) != painted);
        controller->setProperty("searchSegments", QVariantList{});
        QTest::qWait(40);
        QCOMPARE(ink(grab(item("blockSearchBackground"))), 0);
        QCOMPARE(controller->property("clearCalls").toInt(), clears);
        QCOMPARE(doc->revision(), revision);
        QCOMPARE(doc->toRawText(), nativeText);
        QCOMPARE(doc->toHtml(), htmlBefore);
        QCOMPARE(layoutSnapshot(doc), geometry);
        QCOMPARE(mapping->inlineDecorations(), markers);
        QCOMPARE(mapping->spans(), spans);
        QCOMPARE(body->property("selectedText").toString(), selected);
        QMetaObject::invokeMethod(body, "copy");
        QCOMPARE(QGuiApplication::clipboard()->text(), copied);
        QVERIFY(!copied.contains(QChar::ObjectReplacementCharacter));
        QVERIFY(mapping->searchRectangles({}).isEmpty());
        QVERIFY(mapping->searchRect(segment(42, 0, 1)).isEmpty());
    }
    void reflowAndTeardown() {
        root->setProperty("sourceText", "First phrase with enough words to wrap after a width change.");
        root->setProperty("rowSegments", QVariantList{segment(1, 0, 58, true)});
        QTest::qWait(40);
        auto body = item("rowBody");
        SearchDocument mapping;
        mapping.attach(quickDocument(body), root->property("sourceText").toString());
        auto before = mapping.rectFor(52, 6);
        body->setWidth(180);
        QTest::qWait(40);
        auto after = mapping.rectFor(52, 6);
        QVERIFY(after.y() > before.y());
        QVERIFY(ink(grab(item("searchHighlightsCanvas"))) > 0);
        auto font = body->property("font").value<QFont>();
        font.setPixelSize(30);
        body->setProperty("font", font);
        QTest::qWait(40);
        QVERIFY(mapping.rectFor(52, 6).height() > after.height());
        QVERIFY(ink(grab(item("searchHighlightsCanvas"))) > 0);
        const QString text = "First phrase with enough words to wrap after a width change.";
        backend->setProperty("verses", QVariantList{verse(1, text)});
        controller->setProperty("searchSegments", QVariantList{segment(1, 0, text.size(), true)});
        QTest::qWait(40);
        auto block = item("blockReader")->findChild<BlockDocument *>();
        QVERIFY(block);
        const auto blockBefore = block->searchRect(segment(1, 52, 6));
        item("blockReader")->setWidth(200);
        QTest::qWait(40);
        const auto blockAfter = block->searchRect(segment(1, 52, 6));
        QVERIFY(blockAfter.y() > blockBefore.y());
        QVERIFY(ink(grab(item("blockSearchBackground"))) > 0);
        controller->setProperty("readerSize", 30);
        QTest::qWait(40);
        QVERIFY(block->searchRect(segment(1, 52, 6)).height() > blockAfter.height());
        QVERIFY(ink(grab(item("blockSearchBackground"))) > 0);
        // Destroy with pending timers, including a source/document replacement.
        root->setProperty("sourceText", "Replacement source.");
        root->setProperty("rowSegments", QVariantList{});
        backend->setProperty("verses", QVariantList{verse(1, "Replacement block.")});
        root.reset();
        QCoreApplication::processEvents();
        QVERIFY(mapping.rectFor(0, 1).isEmpty());
    }
};

int main(int argc, char **argv)
{
    QQuickStyle::setStyle("Basic");
    QGuiApplication app(argc, argv);
    SearchRenderTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_searchrender.moc"
