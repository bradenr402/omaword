#include "testenvironment.h"
#include <QtTest/QTest>
#include <QAbstractAnimation>
#include <QElapsedTimer>
#include <QApplication>
#include <QFile>
#include <QFont>
#include <QImage>
#include <QJSEngine>
#include <QJSValue>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTextStream>
#include "aofixture.h"
#include "biblebackend.h"
#include "blockdocument.h"
#include "desktop.h"
#include "soundeffects.h"

class MockCatalog : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList entries MEMBER entries NOTIFY changed)
    Q_PROPERTY(bool busy MEMBER busy NOTIFY changed)
    Q_PROPERTY(double progress MEMBER progress NOTIFY changed)
    Q_PROPERTY(QString error MEMBER error NOTIFY changed)
public:
    QVariantList entries;
    bool busy = false;
    double progress = 0;
    QString error;
    Q_INVOKABLE void download(const QString &) {}
    Q_INVOKABLE void cancel() {}
    Q_INVOKABLE void clearError() {}
signals:
    void changed();
    void downloadFinished(const QUrl &url);
};

// Counts the paints of the canvases it watches (paint is emitted as one paints).
class PaintCounter : public QObject
{
    Q_OBJECT
public:
    int count = 0;
    void watch(QQuickItem *root) {
        if (root->inherits("QQuickCanvasItem")) connect(root, SIGNAL(paint(QRect)), this, SLOT(painted()));
        for (auto child : root->childItems()) watch(child);
    }
public slots:
    void painted() { ++count; }
};

// Margins: the chapter outline (left), `{`/`}` heading motions, and margin
// notes (right) with their hand-drawn connectors; plus the pure placement,
// space, and tip functions of Margins.js and the HandDrawn.js connector paths.
class MarginsUiTest : public QObject
{
    Q_OBJECT
    QTemporaryDir fixtures;
    std::unique_ptr<QTemporaryDir> session;
    std::unique_ptr<BibleBackend> bible;
    std::unique_ptr<Desktop> desktop;
    std::unique_ptr<SoundEffects> sounds;
    std::unique_ptr<MockCatalog> catalog;
    std::unique_ptr<QQmlApplicationEngine> engine;
    QQuickWindow *window = nullptr;
    QStringList warnings;
    QString source, plain;
    QString studyId, plainId;

    // John 1 (headings at 4, 10, 19, 29, 35), John 2 (none), John 3 (1, 16,
    // 22, 31), John 4 (a heading every three verses, for a tall outline).
    static bool zip(const QString &path, const QMap<QString, QByteArray> &entries) {
        QJsonArray books;
        QString id;
        for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
            id = it.key().mid(6); id.chop(5);
            books.append(QJsonDocument::fromJson(it.value()).object());
        }
        return AoFixture::write(path, AoFixture::complete(id, books));
    }
    static QByteArray john(const QString &title, bool headings) {
        const QMap<int, QList<int>> sections = {{1, {4, 10, 19, 29, 35}}, {2, {}}, {3, {1, 16, 22, 31}},
            {4, {1, 4, 7, 10, 13, 16, 19, 22, 25, 28, 31, 34, 37, 40, 43, 46, 49, 52}}};
        const QMap<int, int> counts = {{1, 51}, {2, 25}, {3, 36}, {4, 54}};
        Q_UNUSED(title)
        QJsonArray chapters;
        for (int c = 1; c <= 4; ++c) {
            QJsonArray content;
            for (int v = 1; v <= counts[c]; ++v) {
                if (headings && sections[c].contains(v)) content.append(QJsonObject{{"type", "heading"}, {"content", QJsonArray{QString("Section %1.%2 of the margins fixture").arg(c).arg(v)}}});
                content.append(AoFixture::verse(v, {QString("Original margins text %1:%2 with enough ordinary words to wrap across one reading line.").arg(c).arg(v)}));
            }
            chapters.append(AoFixture::chapter(c, content));
        }
        return QJsonDocument(AoFixture::book("JHN", 1, chapters)).toJson(QJsonDocument::Compact);
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
    QVariantMap map(const char *name) const { return prop(name).toMap(); }
    QVariant js(const char *method, const QVariantList &args = {}) const {
        QVariant result; bool ok = false;
        switch (args.size()) {
        case 0: ok = QMetaObject::invokeMethod(window, method, Q_RETURN_ARG(QVariant, result)); break;
        case 1: ok = QMetaObject::invokeMethod(window, method, Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, args[0])); break;
        default: ok = QMetaObject::invokeMethod(window, method, Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, args[0]), Q_ARG(QVariant, args[1])); break;
        }
        if (!ok) qWarning("js call failed: %s", method);
        return value(result);
    }
    void key(Qt::Key key, Qt::KeyboardModifiers mods = Qt::NoModifier) { QTest::keyClick(window, key, mods); QCoreApplication::processEvents(); }
    void type(const QString &text) { for (QChar c : text) { QTest::keyClick(window, c.toLatin1()); QCoreApplication::processEvents(); } }
    void install(const QString &file) {
        bible->importAo(QUrl::fromLocalFile(file)); QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(), 30000);
        QVERIFY2(bible->ready() && bible->error().isEmpty(), qPrintable(bible->error()));
    }
    void navigate(const QString &reference) {
        QVERIFY(bible->navigate(reference)); QVERIFY(QMetaObject::invokeMethod(window, "readerFocus"));
        QTRY_VERIFY(!prop("restoringPosition").toBool()); settle();
    }
    void settle() { QTest::qWait(60); QCoreApplication::processEvents(); }
    void setMode(const QString &mode) {
        bible->setReadingMode(mode); QTRY_COMPARE(prop("continuousReader").toBool(), mode != "verse"); settle();
    }
    void resize(int width, int height) { window->resize(width, height); QTRY_COMPARE(window->width(), width); settle(); }
    void note(const QString &reference, int start, int end, const QString &text) {
        QVERIFY(bible->navigate(reference)); bible->selectVerses(start, end); bible->setNoteText(text);
        QVERIFY2(bible->error().isEmpty(), qPrintable(bible->error()));
    }
    QString toast() const { return object("toast")->property("text").toString(); }
    int jumpIndex() const { return window->property("jumpIndex").toInt(); }
    QVariantMap space() const { return map("marginSpace"); }
    bool notesShown() const { return space()["notes"].toMap()["visible"].toBool(); }
    bool outlineShown() const { auto outline = item("chapterOutline"); return outline && outline->isVisible(); }
    double columnX() const { return item("readerColumn")->x(); }
    QVariantList layout() const { return prop("marginLayout").toList(); }
    QVariantMap placement(const QString &reference) const {
        for (const auto &entry : layout()) if (entry.toMap()["reference"] == reference) return entry.toMap();
        return {};
    }
    QVariantMap extent(int verse) const {
        for (const auto &entry : prop("verseExtents").toList()) if (entry.toMap()["number"].toInt() == verse) return entry.toMap();
        return {};
    }
    void click(QQuickItem *target, QPointF at = QPointF(-1, -1)) {
        if (at.x() < 0) at = QPointF(target->width() / 2, target->height() / 2);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, target->mapToScene(at).toPoint()); QCoreApplication::processEvents();
    }
    QJSValue script(QJSEngine &js, const QString &file) {
        QFile f(QFINDTESTDATA("../qml/" + file)); if (!f.open(QIODevice::ReadOnly)) return {};
        QString code = QString::fromUtf8(f.readAll()); code.replace(".pragma library", "");
        return js.evaluate(code, file);
    }
    static QJSValue array(QJSEngine &js, const QList<QVariantMap> &items) {
        auto list = js.newArray(items.size());
        for (int i = 0; i < items.size(); ++i) list.setProperty(i, js.toScriptValue(items[i]));
        return list;
    }
    static QVariantMap noteItem(double top, double bottom, int lines) { return {{"top", top}, {"bottom", bottom}, {"lines", lines}, {"lineHeight", 20}, {"pad", 0}}; }
    QVariantList place(QJSEngine &js, const QList<QVariantMap> &items, double viewport, double upMax = 0) {
        const auto result = js.globalObject().property("place").call({array(js, items), js.toScriptValue(QVariantMap{
            {"viewport", viewport}, {"gap", 10}, {"upMax", upMax}, {"bounds", QVariantMap{{"top", 0}, {"bottom", 100000}}}})});
        return result.toVariant().toList();
    }
    // Every shown note: no overlap (gap kept) and inside its passage's window.
    static void verifyConstraints(const QList<QVariantMap> &items, const QVariantList &placed, double viewport) {
        QList<int> order;
        for (int i = 0; i < items.size(); ++i) if (!placed[i].toMap()["hidden"].toBool()) order << i;
        std::sort(order.begin(), order.end(), [&](int a, int b) { return placed[a].toMap()["y"].toDouble() < placed[b].toMap()["y"].toDouble(); });
        for (int k = 0; k < order.size(); ++k) {
            const auto p = placed[order[k]].toMap(); const auto &i = items[order[k]];
            const double y = p["y"].toDouble(), h = p["height"].toDouble();
            const double bottom = std::min(i["bottom"].toDouble(), i["top"].toDouble() + viewport);
            QVERIFY2(y >= bottom - viewport - 0.5, qPrintable(QString("note %1 above its window").arg(order[k])));
            QVERIFY2(y + h <= i["top"].toDouble() + viewport + 0.5, qPrintable(QString("note %1 below its window").arg(order[k])));
            if (k) QVERIFY(y >= placed[order[k - 1]].toMap()["y"].toDouble() + placed[order[k - 1]].toMap()["height"].toDouble() + 10 - 0.5);
        }
    }
    // ---- Frame timelines (focus mode transitions) ---------------------------
    // One sample per presented frame: what the reader showed, and whether the
    // margins on screen matched the text they point at.
    struct Frame {
        double t = 0; bool focus = false;
        double columnX = 0, slide = 0, navY = 0, navX = 0; // navX/navY: the next-chapter button's zone
        double titleY = 0; // the chapter title as drawn (reading coordinates)
        bool notes = false; double notesOpacity = 0;
        bool outline = false; double outlineOpacity = 0, outlineX = 0;
        // Painted notes: how many, the first one's x (reading coordinates),
        // and the largest gap between a note's placement and its passage
        // (top, line count) as laid out in this frame.
        int shownNotes = 0; double noteX = 0, stale = 0;
        // Painted connectors: the least room between the column's text edge
        // and where a connector's ink may start (3 px when they touch).
        double connectorGap = 1e9;
        bool dimStale = false; // the focus veil's holes off the focused text
        double cursorY = 0, contentY = 0, contentHeight = 0;
        int rebuilds = 0; bool sliding = false;
        bool sync = false; // sampled outside a frame (right after the trigger)
    };
    BlockDocument *blockMapping() const { return window->findChild<BlockDocument *>(); }
    static QRectF caret(QObject *body, int position) {
        QRectF rect; QMetaObject::invokeMethod(body, "positionToRectangle", Q_RETURN_ARG(QRectF, rect), Q_ARG(int, position));
        return rect;
    }
    // A verse's first line, in viewport (reading) coordinates.
    double verseTop(int verse) const {
        auto reading = item("reading");
        if (prop("continuousReader").toBool()) {
            auto body = item("blockBody"); auto mapping = blockMapping();
            if (!body || !mapping) return qQNaN();
            return body->mapRectToItem(reading, caret(body, mapping->positionFor(verse))).y();
        }
        auto body = item("verseBody" + QString::number(verse));
        return body ? body->mapToItem(reading, QPointF(0, 0)).y() : qQNaN();
    }
    Frame frame(double t) const {
        Frame f; f.t = t; f.focus = bible->focusMode();
        f.columnX = columnX(); f.slide = prop("marginSlideOffset").toDouble();
        f.navY = item("chapterNextButtonZone")->y(); f.navX = item("chapterNextButtonZone")->x();
        f.titleY = item("chapterTitle")->mapToItem(item("reading"), QPointF(0, 0)).y();
        auto layer = item("marginLayer"), outline = item("chapterOutline"), reading = item("reading");
        f.notes = layer->isVisible(); f.notesOpacity = layer->opacity();
        f.outline = outline->isVisible(); f.outlineOpacity = outline->opacity(); f.outlineX = outline->x();
        f.contentY = reading->property("contentY").toDouble(); f.contentHeight = reading->property("contentHeight").toDouble();
        f.cursorY = verseTop(bible->cursorVerse());
        f.sliding = object("readerSlide")->property("running").toBool();
        if (auto reader = object("blockReader")) {
            f.rebuilds = reader->property("rebuildCount").toInt();
            auto veil = item("blockFocusDim");
            if (veil->isVisible() && veil->opacity() > 0) {
                QVariant live; QMetaObject::invokeMethod(reader, "verseRangeRects", Q_RETURN_ARG(QVariant, live),
                    Q_ARG(QVariant, reader->property("dimFirst")), Q_ARG(QVariant, reader->property("dimLast")));
                const auto holes = value(reader->property("dimRects")).toList(), rects = value(live).toList();
                f.dimStale = holes.size() != rects.size();
                for (int i = 0; !f.dimStale && i < holes.size(); ++i)
                    f.dimStale = std::abs(holes[i].toMap()["y"].toDouble() - rects[i].toMap()["y"].toDouble()) > 0.5;
            }
        }
        if (f.notes && f.notesOpacity > 0) {
            const auto live = js("measureVerses").toList();
            for (auto child : layer->childItems()) {
                if (!child->objectName().startsWith("marginNote:") || !child->isVisible()) continue;
                const auto place = value(child->property("place")).toMap();
                const int first = value(child->property("modelData")).toMap()["first"].toInt();
                if (!f.shownNotes++) f.noteX = child->mapToItem(reading, QPointF(0, 0)).x();
                // A connector the gliding column covers entirely paints nothing.
                if (auto connector = visual(child, "marginConnector:" + value(child->property("modelData")).toMap()["reference"].toString());
                        connector && connector->parentItem()->width() > 0.5)
                    f.connectorGap = std::min(f.connectorGap, connector->parentItem()->mapToItem(reading, QPointF(0, 0)).x()
                        - (f.columnX + item("readerColumn")->width() - 9));
                if (place["naturalLines"].toInt() != child->property("naturalLines").toInt()) f.stale = std::max(f.stale, 999.0);
                for (const auto &e : live) if (e.toMap()["number"].toInt() == first)
                    f.stale = std::max(f.stale, std::abs(e.toMap()["top"].toDouble() - place["passageTop"].toDouble()));
            }
        }
        return f;
    }
    // Runs `trigger`, then samples every presented frame for `ms` of real time.
    QList<Frame> record(const std::function<void()> &trigger, int ms) {
        QList<Frame> frames; QElapsedTimer clock;
        frames << frame(-1);
        auto connection = connect(window, &QQuickWindow::frameSwapped, this, [&]() { frames << frame(clock.nsecsElapsed() / 1e6); }, Qt::DirectConnection);
        clock.start(); trigger();
        frames << frame(clock.nsecsElapsed() / 1e6); frames.last().sync = true;
        QTest::qWait(ms);
        disconnect(connection);
        return frames;
    }
    static QString describe(const Frame &f) {
        return QString("t=%1 focus=%2 x=%3 off=%4 navY=%19 title=%20 notes=%5/%6 outline=%7/%8 shown=%9 noteX=%10 stale=%11 gap=%12 dimStale=%13 cursorY=%14 contentY=%15 height=%16 rebuilds=%17 slide=%18")
            .arg(f.t, 6, 'f', 1).arg(int(f.focus)).arg(f.columnX, 6, 'f', 1).arg(f.slide, 6, 'f', 1)
            .arg(int(f.notes)).arg(f.notesOpacity, 4, 'f', 2).arg(int(f.outline)).arg(f.outlineOpacity, 4, 'f', 2)
            .arg(f.shownNotes).arg(f.noteX, 6, 'f', 1).arg(f.stale, 5, 'f', 1).arg(f.shownNotes ? f.connectorGap : 0, 5, 'f', 1)
            .arg(int(f.dimStale)).arg(f.cursorY, 6, 'f', 1)
            .arg(f.contentY, 7, 'f', 1).arg(f.contentHeight, 6, 'f', 0).arg(f.rebuilds).arg(int(f.sliding)).arg(f.navY, 5, 'f', 1).arg(f.titleY, 7, 'f', 1) + (f.sync ? " (sync)" : "");
    }
    // OMAWORD_FOCUS_TIMELINE=<dir>: focus-timeline-<name>.txt per transition.
    static void dump(const QString &name, const QList<Frame> &frames) {
        const auto directory = qEnvironmentVariable("OMAWORD_FOCUS_TIMELINE");
        if (directory.isEmpty()) return;
        QFile file(directory + "/focus-timeline-" + name + ".txt");
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
        QTextStream out(&file);
        for (const auto &f : frames) out << describe(f) << '\n';
    }
    // Animation time under test control: animations advance only by step(),
    // exactly `ms` at a time, however long a (software, loaded) frame takes.
    struct StepDriver : QAnimationDriver {
        qint64 now = 0;
        qint64 elapsed() const override { return now; }
        void start() override { now = 0; QAnimationDriver::start(); }
        void step(int ms) { now += ms; advance(); }
    };
    // The frame before `actions[0]`, every frame presented while each action
    // runs, and one per `ms` animation step: actions[i] runs after
    // steps[i - 1] steps (rapid toggles), then steps.last() steps. With
    // OMAWORD_FOCUS_TIMELINE, each step is also saved as
    // <dir>/focus-frames/<name>-<step>.png.
    QList<Frame> stepped(const QString &name, const QList<std::function<void()>> &actions, const QList<int> &steps, int ms = 8) {
        const auto directory = qEnvironmentVariable("OMAWORD_FOCUS_TIMELINE");
        if (!directory.isEmpty()) QDir().mkpath(directory + "/focus-frames");
        StepDriver driver; driver.install();
        // Whatever still animates (a scrollbar fading, a toast) ends first, so
        // the transition's first step is exactly `ms`, not `ms` plus the time
        // since that animation's last tick.
        driver.step(1000); QCoreApplication::processEvents();
        QList<Frame> frames; double now = -1; int step = 0; bool swapped = false;
        auto connection = connect(window, &QQuickWindow::frameSwapped, this, [&]() { swapped = true; frames << frame(now); }, Qt::DirectConnection);
        const auto present = [&]() {
            swapped = false; window->update();
            if (!QTest::qWaitFor([&]() { return swapped; }, 2000)) qWarning("no frame presented");
            if (!directory.isEmpty()) window->grabWindow().save(QString("%1/focus-frames/%2-%3.png").arg(directory, name).arg(step, 2, 10, QChar('0')));
        };
        present();
        now = 0;
        for (int a = 0; a < actions.size(); ++a) {
            actions[a](); present();
            for (int i = 0; i < steps[a]; ++i) { now += ms; ++step; driver.step(ms); present(); }
        }
        disconnect(connection); driver.uninstall();
        return frames;
    }
    QList<Frame> stepped(const QString &name, const std::function<void()> &trigger, int steps = 30) {
        return stepped(name, QList<std::function<void()>>{trigger}, {steps});
    }
    // A focus mode transition (entering or leaving) as frames, against the
    // contract: one coordinated motion, nothing stale, ending exact.
    // - The column starts where it was (no jump), moves monotonically, at
    //   most a quarter of the way per 8 ms step, and ends at `targetX`.
    // - The cursor verse keeps its place through the reflow (the first frame
    //   after the toggle is within 1 px of the last one before it), and from
    //   then on moves only by scrolling: its content position never changes
    //   again (|Δ(cursorY + contentY)| <= 1). 1 px allows for sub-pixel
    //   rounding; any reflow moves it by a line (~27 px) or a heading (~50).
    //   Entering (with a selection) it ends on the typewriter line, 40% down;
    //   leaving it stays put.
    // - Painted notes always match the layout in the same frame; notes and
    //   the outline only fade (never pop), leaving with the column (their
    //   offset from it unchanged) and arriving where they settle (static).
    // - The chapter buttons glide to their new centre (focus mode has no
    //   footer room) the same way, never jumping.
    // - Paragraph/Block rebuild exactly once; the focus veil's holes stay on
    //   the focused text.
    void verifyTransition(const QList<Frame> &frames, bool entering, double targetX, double typewriterY, int rebuilds) {
        QVERIFY(frames.size() > 3);
        const Frame &before = frames.first(), &first = frames[1], &last = frames.last();
        const QString where = describe(first);
        QVERIFY2(std::abs(first.columnX - before.columnX) < 0.5, qPrintable("column jumped: " + where));
        QVERIFY2(std::abs(first.cursorY - before.cursorY) <= 1, qPrintable("cursor verse jumped: " + where));
        QVERIFY2(std::abs(first.navY - before.navY) < 0.5, qPrintable("chapter buttons jumped: " + where));
        // The chapter title neither jumps nor turns back: it moves only as the text scrolls.
        QVERIFY2(std::abs(first.titleY - before.titleY) <= 1, qPrintable(QString("chapter title jumped from %1: ").arg(before.titleY) + where));
        const double titleDirection = last.titleY >= before.titleY ? 1 : -1;
        const double navDistance = std::abs(last.navY - before.navY), navDirection = last.navY > before.navY ? 1 : -1;
        QVERIFY(navDistance > 10);
        QCOMPARE(last.columnX, targetX);
        QCOMPARE(last.slide, 0.0);
        const double distance = std::abs(targetX - before.columnX), direction = targetX > before.columnX ? 1 : -1;
        const Frame *firstNotes = nullptr;
        for (int i = 1; i < frames.size(); ++i) {
            const Frame &f = frames[i], &p = frames[i - 1];
            const QString at = describe(f);
            QVERIFY2((f.columnX - p.columnX) * direction >= -0.01, qPrintable("column turned back: " + at));
            QVERIFY2(std::abs(f.columnX - p.columnX) <= 0.25 * distance + 0.5, qPrintable("column jumped: " + at));
            QVERIFY2((f.navY - p.navY) * navDirection >= -0.01 && std::abs(f.navY - p.navY) <= 0.25 * navDistance + 0.5, qPrintable("chapter buttons jumped: " + at));
            // Across, they stay put (the scrollbar's room is kept as it hides).
            QVERIFY2(std::abs(f.navX - before.navX) < 0.5, qPrintable(QString("next-chapter button moved across from %1: ").arg(before.navX) + at));
            QVERIFY2((f.titleY - p.titleY) * titleDirection >= -0.5, qPrintable("chapter title turned back: " + at));
            if (i > 1) QVERIFY2(std::abs(f.cursorY + f.contentY - p.cursorY - p.contentY) <= 1, qPrintable("text reflowed under the cursor verse: " + at));
            if (entering && typewriterY >= 0) QVERIFY2((f.cursorY - p.cursorY) * (typewriterY - before.cursorY) >= -1, qPrintable("cursor verse turned back: " + at));
            if (!entering) QVERIFY2(std::abs(f.cursorY - before.cursorY) <= 1, qPrintable("cursor verse moved: " + at));
            QVERIFY2(f.stale <= 1, qPrintable("stale notes painted: " + at));
            QVERIFY2(f.connectorGap >= 2.5, qPrintable("connector over the text: " + at));
            QVERIFY2(!f.dimStale, qPrintable("focus veil off the focused text: " + at));
            if (rebuilds >= 0) QVERIFY2(f.rebuilds - before.rebuilds <= 1, qPrintable("rebuilt twice: " + at));
            const bool notes = f.notes && f.notesOpacity > 0, outline = f.outline && f.outlineOpacity > 0;
            const bool hadNotes = p.notes && p.notesOpacity > 0;
            if (entering) {
                QVERIFY2(!notes || f.notesOpacity <= p.notesOpacity + 1e-9, qPrintable("notes faded back in: " + at));
                QVERIFY2(!outline || f.outlineOpacity <= p.outlineOpacity + 1e-9, qPrintable("outline faded back in: " + at));
                // Leaving with the column.
                if (notes && hadNotes) QVERIFY2(std::abs((f.noteX - f.columnX) - (p.noteX - p.columnX)) < 0.5, qPrintable("notes apart from the column: " + at));
                if (outline) QVERIFY2(std::abs((f.outlineX - f.columnX) - (before.outlineX - before.columnX)) < 0.5, qPrintable("outline apart from the column: " + at));
            } else {
                QVERIFY2(!hadNotes || f.notesOpacity >= p.notesOpacity - 1e-9, qPrintable("notes faded back out: " + at));
                QVERIFY2(f.outlineOpacity >= p.outlineOpacity - 1e-9 || !f.outline, qPrintable("outline faded back out: " + at));
                if (notes && !firstNotes) firstNotes = &f;
                // Arriving where they settle, from transparent.
                if (notes) QVERIFY2(std::abs(f.noteX - last.noteX) < 0.5, qPrintable("notes moving in: " + at));
                if (notes && !hadNotes) QVERIFY2(f.notesOpacity < 0.5, qPrintable("notes popped in: " + at));
                if (outline) QVERIFY2(std::abs(f.outlineX - last.outlineX) < 0.5, qPrintable("outline moving in: " + at));
            }
        }
        if (entering) {
            QVERIFY(!last.notes && !last.outline);
            if (typewriterY >= 0) QVERIFY2(std::abs(last.cursorY - typewriterY) <= 1, qPrintable(describe(last)));
        } else {
            QVERIFY(firstNotes);
            QVERIFY(last.notes && last.outline); QCOMPARE(last.notesOpacity, 1.0); QCOMPARE(last.outlineOpacity, 1.0);
            QVERIFY(first.notesOpacity == 0 || !first.notes);
        }
        if (rebuilds >= 0) QCOMPARE(last.rebuilds - before.rebuilds, rebuilds);
        for (const char *name : {"marginSlide", "marginExit", "marginEnter", "chapterNavGlide", "readerSlide", "titleSettle"})
            QVERIFY2(!object(name)->property("running").toBool(), name);
        QVERIFY(!prop("marginsLeaving").toBool());
    }

private slots:
    void initTestCase() {
        source = fixtures.filePath("margins_usfm.zip");
        QVERIFY(zip(source, {{"43-JHNmargins.usfm", john("Original margins fixture", true)}}));
        plain = fixtures.filePath("plain_usfm.zip");
        QVERIFY(zip(plain, {{"43-JHNplain.usfm", john("Original plain fixture", false)}}));
    }
    void init() {
        session = std::make_unique<QTemporaryDir>(); qputenv("XDG_DATA_HOME", session->path().toUtf8());
        bible = std::make_unique<BibleBackend>();
        install(plain); plainId = bible->versionId();
        install(source); studyId = bible->versionId();
        QVERIFY(plainId != studyId); QVERIFY(bible->hasHeadings());
        bible->setReadingMode("verse");
        desktop = std::make_unique<Desktop>(); sounds = std::make_unique<SoundEffects>(nullptr, session->filePath("sound.ini"), false);
        sounds->setProperty("reduceMotion", true);
        catalog = std::make_unique<MockCatalog>();
        engine = std::make_unique<QQmlApplicationEngine>(); warnings.clear();
        connect(engine.get(), &QQmlEngine::warnings, this, [this](const QList<QQmlError> &errors) { for (const auto &error : errors) warnings << error.toString(); });
        engine->rootContext()->setContextProperty("bible", bible.get());
        engine->rootContext()->setContextProperty("desktop", desktop.get());
        engine->rootContext()->setContextProperty("sounds", sounds.get());
        engine->rootContext()->setContextProperty("catalog", catalog.get());
        engine->load(QUrl::fromLocalFile(QFINDTESTDATA("../qml/Main.qml")));
        QVERIFY2(!engine->rootObjects().isEmpty(), qPrintable(warnings.join('\n')));
        window = qobject_cast<QQuickWindow *>(engine->rootObjects().first()); QVERIFY(window);
        window->resize(1600, 900);
        window->requestActivate(); QVERIFY(QTest::qWaitForWindowExposed(window)); QTRY_VERIFY(window->isActive());
        window->setProperty("historyDwellMs", 3600000);
        navigate("John 3:2");
        // Start the jump stack so each later jump adds exactly one entry.
        QVERIFY(QMetaObject::invokeMethod(window, "jump", Q_ARG(QVariant, QString("John 3:3"))));
        QTRY_VERIFY(jumpIndex() == 1); navigate("John 3:2");
    }
    void cleanup() {
        engine.reset(); window = nullptr; catalog.reset(); sounds.reset(); desktop.reset(); bible.reset(); session.reset();
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
    }

    // ---- Margins.js -------------------------------------------------------
    void placementAlignsAndResolvesOverlaps() {
        QJSEngine js; QVERIFY(!script(js, "Margins.js").isError());
        // Apart: each note aligns with its passage top.
        QList<QVariantMap> apart = {noteItem(100, 160, 2), noteItem(400, 420, 3), noteItem(900, 1000, 1)};
        auto placed = place(js, apart, 600);
        for (int i = 0; i < apart.size(); ++i) {
            QCOMPARE(placed[i].toMap()["y"].toDouble(), apart[i]["top"].toDouble());
            QVERIFY(!placed[i].toMap()["truncated"].toBool());
        }
        // Same passage top: the later note goes below, and the run rises
        // slightly (at most upMax, half the mean displacement).
        QList<QVariantMap> crowded = {noteItem(200, 220, 3), noteItem(200, 220, 2)};
        placed = place(js, crowded, 600, 15);
        const double first = placed[0].toMap()["y"].toDouble(), second = placed[1].toMap()["y"].toDouble();
        QVERIFY2(first < 200 && first >= 185, qPrintable(QString::number(first)));
        QCOMPARE(second, first + 60 + 10);
        // Without upMax the run stays put and only pushes down.
        placed = place(js, crowded, 600, 0);
        QCOMPARE(placed[0].toMap()["y"].toDouble(), 200.0); QCOMPARE(placed[1].toMap()["y"].toDouble(), 270.0);
        // Input order is kept in the result, whatever the passage order.
        placed = place(js, {noteItem(500, 520, 1), noteItem(100, 120, 1)}, 600);
        QCOMPARE(placed[0].toMap()["y"].toDouble(), 500.0); QCOMPARE(placed[1].toMap()["y"].toDouble(), 100.0);
        // A note pushed past its window is pulled back up, pushing the one
        // above it up too, but never more than upMax above its passage.
        QList<QVariantMap> tight = {noteItem(300, 320, 5), noteItem(320, 340, 5)};
        placed = place(js, tight, 180, 40);
        verifyConstraints(tight, placed, 180);
        QVERIFY(placed[1].toMap()["y"].toDouble() + 100 <= 320 + 180 + 0.5);
        QVERIFY(placed[0].toMap()["y"].toDouble() < 300 && placed[0].toMap()["y"].toDouble() >= 260);
        for (const auto &p : placed) QVERIFY(!p.toMap()["truncated"].toBool());
        // Without room to rise, the same notes are truncated instead.
        placed = place(js, tight, 180, 0);
        verifyConstraints(tight, placed, 180);
        QVERIFY(placed[0].toMap()["truncated"].toBool() || placed[1].toMap()["truncated"].toBool());
    }
    void placementTruncatesLongestFirstMinimally() {
        QJSEngine js; QVERIFY(!script(js, "Margins.js").isError());
        // Three notes on adjacent verses: the last must end by 140 + 280.
        QList<QVariantMap> items = {noteItem(100, 120, 4), noteItem(120, 140, 9), noteItem(140, 160, 3)};
        auto placed = place(js, items, 280);
        verifyConstraints(items, placed, 280);
        // Only the longest loses lines, and only as many as needed.
        QCOMPARE(placed[0].toMap()["lines"].toInt(), 4); QCOMPARE(placed[2].toMap()["lines"].toInt(), 3);
        const int kept = placed[1].toMap()["lines"].toInt();
        QVERIFY(kept < 9); QVERIFY(placed[1].toMap()["truncated"].toBool());
        QVERIFY(!placed[0].toMap()["truncated"].toBool() && !placed[2].toMap()["truncated"].toBool());
        // Minimal: one more line does not fit.
        auto more = items; more[1]["lines"] = kept + 1;
        more[0]["lines"] = 4; more[2]["lines"] = 3;
        QCOMPARE(kept, 8);
        const auto again = place(js, more, 280);
        QCOMPARE(again[1].toMap()["lines"].toInt(), kept);
        // Two long notes: the longer is cut until they tie, then both shrink.
        items = {noteItem(100, 120, 12), noteItem(120, 140, 8)};
        placed = place(js, items, 300);
        verifyConstraints(items, placed, 300);
        QVERIFY(std::abs(placed[0].toMap()["lines"].toInt() - placed[1].toMap()["lines"].toInt()) <= 1);
        // A note taller than the viewport is cut to fit beside its passage.
        items = {noteItem(0, 40, 30)};
        placed = place(js, items, 300);
        QCOMPARE(placed[0].toMap()["lines"].toInt(), 15); // beside its passage: 300 / 20
        // Far-apart notes are never touched by another run's truncation.
        items = {noteItem(100, 120, 12), noteItem(120, 140, 8), noteItem(5000, 5020, 12)};
        placed = place(js, items, 300);
        QCOMPARE(placed[2].toMap()["lines"].toInt(), 12);
        // Impossible at one line: hidden rather than misplaced.
        QList<QVariantMap> packed;
        for (int i = 0; i < 12; ++i) packed << noteItem(100, 110, 1);
        placed = place(js, packed, 200);
        int hidden = 0; for (const auto &p : placed) hidden += p.toMap()["hidden"].toBool();
        QVERIFY(hidden > 0 && hidden < 12);
        verifyConstraints(packed, placed, 200);
    }
    void spaceGivesMarginNotesPriority() {
        QJSEngine js; QVERIFY(!script(js, "Margins.js").isError());
        auto split = [&](double width, bool notes, bool outline) {
            const double column = 760, x = std::min((width - column) / 2, width - 48 - column);
            return js.globalObject().property("space").call({js.toScriptValue(QVariantMap{
                {"width", width}, {"columnX", x}, {"columnWidth", column}, {"minLeft", 48}, {"leftInset", 24}, {"rightInset", 28},
                {"wantNotes", notes}, {"connectorWidth", 52}, {"notesMin", 170}, {"notesPreferred", 250}, {"notesMax", 330},
                {"wantOutline", outline}, {"outlineGap", 12}, {"outlineMin", 120}, {"outlineMax", 220}})}).toVariant().toMap();
        };
        // Wide: both, no shift.
        auto wide = split(1920, true, true);
        QVERIFY(wide["notes"].toMap()["visible"].toBool() && wide["outline"].toMap()["visible"].toBool());
        QCOMPARE(wide["shift"].toDouble(), 0.0);
        // Medium: notes shift the column, but only as far as leaves the
        // outline its minimum while the notes keep theirs.
        auto medium = split(1200, true, true);
        QVERIFY(medium["notes"].toMap()["visible"].toBool()); QVERIFY(medium["shift"].toDouble() > 0);
        QVERIFY(medium["outline"].toMap()["visible"].toBool());
        QCOMPARE(medium["outline"].toMap()["width"].toDouble(), 120.0);
        QVERIFY(medium["notes"].toMap()["width"].toDouble() >= 170);
        QVERIFY(medium["shift"].toDouble() < split(1200, true, false)["shift"].toDouble());
        // Where both cannot fit, the notes win; the outline uses what is left.
        auto crowded = split(1100, true, true);
        QVERIFY(crowded["notes"].toMap()["visible"].toBool()); QVERIFY(!crowded["outline"].toMap()["visible"].toBool());
        QCOMPARE(crowded["shift"].toDouble(), split(1100, true, false)["shift"].toDouble());
        QVERIFY(split(1200, false, true)["outline"].toMap()["visible"].toBool());
        QCOMPARE(split(1200, false, true)["shift"].toDouble(), 0.0);
        // The outline never shifts.
        QCOMPARE(split(1000, false, true)["columnX"].toDouble(), split(1000, false, false)["columnX"].toDouble());
        // Narrow: nothing fits; no shift either.
        auto narrow = split(900, true, true);
        QVERIFY(!narrow["notes"].toMap()["visible"].toBool() && !narrow["outline"].toMap()["visible"].toBool());
        QCOMPARE(narrow["shift"].toDouble(), 0.0);
    }
    void connectorTipsLeanTogether() {
        QJSEngine js; QVERIFY(!script(js, "Margins.js").isError());
        auto tips = [&](double noteTop, double noteBottom, double passageTop, double passageBottom) {
            return js.globalObject().property("tips").call({js.toScriptValue(QVariantMap{{"top", noteTop}, {"bottom", noteBottom}}),
                js.toScriptValue(QVariantMap{{"top", passageTop}, {"bottom", passageBottom}})}).toVariant().toMap();
        };
        // Note below its passage: "{" tip up, "}" tip down, proportionally.
        auto below = tips(150, 250, 100, 200), further = tips(160, 260, 100, 200);
        QVERIFY(below["note"].toDouble() < 0 && below["passage"].toDouble() > 0);
        QVERIFY(std::abs(further["note"].toDouble()) > std::abs(below["note"].toDouble()));
        QCOMPARE(below["note"].toDouble(), -below["passage"].toDouble());
        // Above: the reverse.
        auto above = tips(0, 100, 300, 400);
        QVERIFY(above["note"].toDouble() > 0 && above["passage"].toDouble() < 0);
        // Level: centred.
        auto level = tips(100, 200, 100, 200);
        QCOMPARE(level["note"].toDouble(), 0.0); QCOMPARE(level["passage"].toDouble(), 0.0);
        // Far apart: clamped inside each brace (middle 60%).
        auto far = tips(5000, 5100, 100, 140);
        QCOMPARE(far["note"].toDouble(), -30.0); QCOMPARE(far["passage"].toDouble(), 12.0);
        // Single-line braces keep their tips centred; multi-line ones still lean.
        auto withLines = [&](double noteTop, double noteBottom, int noteLines, double passageTop, double passageBottom, double passageLine) {
            return js.globalObject().property("tips").call({js.toScriptValue(QVariantMap{{"top", noteTop}, {"bottom", noteBottom}}),
                js.toScriptValue(QVariantMap{{"top", passageTop}, {"bottom", passageBottom}}),
                js.toScriptValue(QVariantMap{{"noteLines", noteLines}, {"passageLine", passageLine}})}).toVariant().toMap();
        };
        auto oneLineNote = withLines(5000, 5020, 1, 100, 300, 30);
        QCOMPARE(oneLineNote["note"].toDouble(), 0.0); QCOMPARE(oneLineNote["passage"].toDouble(), 60.0);
        auto oneLinePassage = withLines(5000, 5100, 5, 100, 130, 30);
        QCOMPARE(oneLinePassage["note"].toDouble(), -30.0); QCOMPARE(oneLinePassage["passage"].toDouble(), 0.0);
        auto bothSingle = withLines(150, 170, 1, 100, 140, 30);
        QCOMPARE(bothSingle["note"].toDouble(), 0.0); QCOMPARE(bothSingle["passage"].toDouble(), 0.0);
        // Two lines (passage 60 px at 30 px lines; note 2 lines) shift within 20-80%.
        auto twoLines = withLines(5000, 5040, 2, 100, 160, 30);
        QCOMPARE(twoLines["note"].toDouble(), -12.0); QCOMPARE(twoLines["passage"].toDouble(), 18.0);
    }
    void headingStepsAndSections() {
        QJSEngine js; QVERIFY(!script(js, "Margins.js").isError());
        auto headings = js.evaluate("[{verse: 1}, {verse: 16}, {verse: 22}, {verse: 31}]");
        auto step = [&](int verse, int direction, int count) {
            return js.globalObject().property("headingStep").call({headings, verse, direction, count}).toVariant().toMap();
        };
        QCOMPARE(step(2, 1, 1)["verse"].toInt(), 16);
        QCOMPARE(step(2, 1, 2)["verse"].toInt(), 22);
        auto past = step(2, 1, 5); QCOMPARE(past["verse"].toInt(), 31); QVERIFY(past["short"].toBool()); QCOMPARE(past["moved"].toInt(), 3);
        QCOMPARE(step(20, -1, 1)["verse"].toInt(), 16);
        QCOMPARE(step(16, -1, 1)["verse"].toInt(), 1);
        auto none = step(31, 1, 1); QCOMPARE(none["moved"].toInt(), 0); QVERIFY(none["short"].toBool());
        // With the chapter top (first verse), { ends there, merged with a heading on it.
        auto topStep = [&](const QJSValue &list, int verse, int count) {
            return js.globalObject().property("headingStep").call({list, verse, -1, count, 1}).toVariant().toMap();
        };
        QCOMPARE(topStep(headings, 20, 1)["verse"].toInt(), 16);
        auto merged = topStep(headings, 20, 5); QCOMPARE(merged["verse"].toInt(), 1); QCOMPARE(merged["moved"].toInt(), 2);
        auto later = js.evaluate("[{verse: 4}, {verse: 10}]");
        QCOMPARE(topStep(later, 3, 1)["verse"].toInt(), 1);
        auto toTop = topStep(later, 12, 3); QCOMPARE(toTop["verse"].toInt(), 1); QCOMPARE(toTop["moved"].toInt(), 3);
        QCOMPARE(topStep(js.evaluate("[]"), 7, 1)["verse"].toInt(), 1);
        QCOMPARE(topStep(later, 1, 1)["moved"].toInt(), 0);
        QCOMPARE(js.globalObject().property("headingStep").call({later, 3, 1, 1, 1}).toVariant().toMap()["verse"].toInt(), 4);
        QCOMPARE(js.globalObject().property("sectionOf").call({headings, 21}).toInt(), 16);
        QCOMPARE(js.globalObject().property("sectionOf").call({js.evaluate("[{verse: 3}]"), 2}).toInt(), 0);
    }
    void handDrawnConnectorsAreDeterministic() {
        QJSEngine js; QVERIFY(!script(js, "HandDrawn.js").isError());
        auto brace = [&](int seed, int direction, double tip) {
            return js.globalObject().property("bracePoints").call({10, 100, 300, tip, 9, direction, seed, 1}).toVariant().toMap();
        };
        const auto a = brace(7, -1, 180), b = brace(7, -1, 180), c = brace(8, -1, 180);
        QCOMPARE(a, b);
        QVERIFY(a["points"] != c["points"]);
        // "{" tips left at the given (clamped) height; "}" tips right.
        QCOMPARE(a["tip"].toMap()["x"].toDouble(), 10.0); QCOMPARE(a["tip"].toMap()["y"].toDouble(), 180.0);
        QCOMPARE(brace(7, 1, 180)["tip"].toMap()["x"].toDouble(), 19.0);
        // Tips clamp to 20%–80% of the brace's height (100–300 here).
        QCOMPARE(brace(7, 1, 50)["tip"].toMap()["y"].toDouble(), 140.0);
        QCOMPARE(brace(7, -1, 120)["tip"].toMap()["y"].toDouble(), 140.0);
        QCOMPARE(brace(7, -1, 999)["tip"].toMap()["y"].toDouble(), 260.0);
        QCOMPARE(brace(7, 1, 260)["tip"].toMap()["y"].toDouble(), 260.0);
        QCOMPARE(brace(7, 1, 141)["tip"].toMap()["y"].toDouble(), 141.0);
        // The highlight accent's pen: along the straight runs (clear of the
        // curls) the spine drifts at most the accent's amplitude from its line.
        const double accent = js.globalObject().property("accentAmp").call({1}).toNumber();
        QCOMPARE(accent, 0.6);
        for (int seed = 1; seed < 20; ++seed) {
            double drift = 0;
            for (const auto &p : brace(seed, -1, 200)["points"].toList()) {
                const double x = p.toMap()["x"].toDouble(), y = p.toMap()["y"].toDouble();
                if ((y > 110 && y < 190) || (y > 210 && y < 290)) drift = qMax(drift, std::abs(x - 14.5));
            }
            QVERIFY2(drift <= accent + 1e-9 && drift > 0.2, qPrintable(QString::number(drift)));
        }
        // Ink stays in the brace's column.
        for (const auto &p : a["points"].toList()) {
            const double x = p.toMap()["x"].toDouble(), y = p.toMap()["y"].toDouble();
            QVERIFY2(x >= 10 - 0.01 && x <= 19 + 0.01 && y >= 100 - 0.01 && y <= 300 + 0.01, qPrintable(QString("%1,%2").arg(x).arg(y)));
        }
        auto arrow = [&](int seed) { return js.globalObject().property("wavyArrow").call({QJSValue(QJSValue::NullValue), 200, 50, 20, 90, seed, 1}).toVariant().toMap(); };
        QCOMPARE(arrow(3), arrow(3));
        QVERIFY(arrow(3) != arrow(4));
        // The shaft ends exactly at the "}" tip; the head opens back toward the note.
        const auto shaft = arrow(3)["shaft"].toList();
        QCOMPARE(shaft.last().toMap()["x"].toDouble(), 20.0); QCOMPARE(shaft.last().toMap()["y"].toDouble(), 90.0);
        for (const auto &side : arrow(3)["head"].toList()) QVERIFY(side.toList().last().toMap()["x"].toDouble() > 20);
        // It swoops: an S that leaves (200, 50) and reaches (20, 90) level, its
        // middle crossing at the half-way height, never past either end.
        const auto at = [&](double x) {
            double best = 1e9, y = 0;
            for (const auto &p : shaft) if (std::abs(p.toMap()["x"].toDouble() - x) < best) { best = std::abs(p.toMap()["x"].toDouble() - x); y = p.toMap()["y"].toDouble(); }
            return y;
        };
        QVERIFY2(std::abs(at(190) - 50) < 3, qPrintable(QString::number(at(190))));
        QVERIFY2(std::abs(at(30) - 90) < 3, qPrintable(QString::number(at(30))));
        QVERIFY2(std::abs(at(110) - 70) < 4, qPrintable(QString::number(at(110))));
        for (const auto &p : shaft) {
            const double x = p.toMap()["x"].toDouble(), y = p.toMap()["y"].toDouble();
            QVERIFY(x >= 20 - 0.01 && x <= 200 + 0.01); QVERIFY(y >= 50 - 2 && y <= 90 + 2);
        }
        // The head arrives level, as the shaft does: its two strokes mirror each other.
        const auto head = arrow(3)["head"].toList();
        const double up = head[0].toList().last().toMap()["y"].toDouble() - 90, down = head[1].toList().last().toMap()["y"].toDouble() - 90;
        QVERIFY2(std::abs(up + down) < 1.5, qPrintable(QString("%1 %2").arg(up).arg(down)));
    }

    // ---- Chapter outline --------------------------------------------------
    void outlineVisibilityRules() {
        QVERIFY(!bible->chapterOutline()); QVERIFY(!outlineShown());
        const double x = columnX();
        key(Qt::Key_O, Qt::AltModifier); QTRY_VERIFY(bible->chapterOutline());
        QTRY_VERIFY(outlineShown()); QCOMPARE(toast(), QString("Chapter outline on"));
        // Never shifts the reader.
        QCOMPARE(columnX(), x);
        QVERIFY(item("chapterOutline")->x() + item("chapterOutline")->width() <= columnX());
        QVERIFY(item("outlineEntry:16") && item("outlineEntry:31"));
        // The preference persists; Settings and Commands list it.
        { BibleBackend reopened; QVERIFY(reopened.chapterOutline()); }
        key(Qt::Key_Comma, Qt::ControlModifier); QTRY_VERIFY(object("settingsDialog")->property("opened").toBool());
        QVERIFY(item("settingchapterOutline") && item("settingchapterOutline")->isVisible());
        QVERIFY(item("settingmarginNotes") && item("settingmarginNotes")->isVisible());
        key(Qt::Key_Escape); QTRY_VERIFY(!object("settingsDialog")->property("visible").toBool());
        QStringList commands;
        QVERIFY(QMetaObject::invokeMethod(window, "openPalette", Q_ARG(QVariant, true), Q_ARG(QVariant, false)));
        for (const auto &entry : js("paletteEntries").toList()) commands << entry.toMap()["title"].toString();
        QVERIFY2(commands.contains("Chapter outline") && commands.contains("Show notes in margin"), qPrintable(commands.join(", ")));
        key(Qt::Key_Escape); QTRY_VERIFY(!object("passagePalette")->property("visible").toBool());
        // No headings in this chapter: hidden.
        navigate("John 2:1"); QTRY_VERIFY(!outlineShown());
        navigate("John 3:1"); QTRY_VERIFY(outlineShown());
        // No space: hidden; room again: shown.
        resize(1000, 900); QTRY_VERIFY(!outlineShown());
        resize(1600, 900); QTRY_VERIFY(outlineShown());
        // Focus mode hides it.
        bible->setFocusMode(true); QTRY_VERIFY(!outlineShown());
        bible->setFocusMode(false); QTRY_VERIFY(outlineShown());
        // A version without headings: hidden.
        QVERIFY(bible->switchVersion(plainId)); settle(); QTRY_VERIFY(!outlineShown());
        key(Qt::Key_O, Qt::AltModifier); QTRY_VERIFY(!bible->chapterOutline());
    }
    void outlineSlidesInAndOut() {
        // alt o: the outline slides in from the left as it fades in (a
        // panel's 125 ms, easing out) and leaves the same way, quicker (85 ms,
        // easing in); the column stays put. Disable animations: at once.
        sounds->setProperty("reduceMotion", false);
        navigate("John 3:3");
        QVERIFY(!bible->chapterOutline()); QVERIFY(!outlineShown());
        const double x = columnX();
        const auto check = [&](const QList<Frame> &frames, bool entering) {
            const auto &last = frames.last();
            QCOMPARE(last.outline, entering);
            int between = 0;
            for (int i = 1; i < frames.size(); ++i) {
                const auto &f = frames[i], &previous = frames[i - 1];
                QVERIFY2(std::abs(f.columnX - x) < 0.5, qPrintable(describe(f)));
                if (!f.outline || i == 1) continue;
                // One motion: opacity and place change together, one way.
                QVERIFY2(entering ? f.outlineOpacity >= previous.outlineOpacity : f.outlineOpacity <= previous.outlineOpacity, qPrintable(describe(f)));
                QVERIFY2(entering ? f.outlineX >= previous.outlineX : f.outlineX <= previous.outlineX, qPrintable(describe(f)));
                between += f.outlineOpacity > 0.05 && f.outlineOpacity < 0.95;
            }
            QVERIFY2(between > 1, "the outline popped rather than slid");
        };
        // The first frame at or after `t` ms of animation.
        const auto at = [](const QList<Frame> &frames, double t) {
            for (const auto &f : frames) if (f.t >= t) return f;
            return frames.last();
        };
        const auto in = stepped("outline-in", [&] { key(Qt::Key_O, Qt::AltModifier); }, 20);
        QVERIFY(bible->chapterOutline());
        const double rest = in.last().outlineX;
        QCOMPARE(in.last().outlineOpacity, 1.0);
        QVERIFY(in[1].outline); QVERIFY(in[1].outlineOpacity < 0.05); QVERIFY(in[1].outlineX < rest - 10);
        check(in, true);
        // In place after 125 ms.
        QCOMPARE(at(in, 125).outlineOpacity, 1.0); QCOMPARE(at(in, 125).outlineX, rest);
        QVERIFY(at(in, 120).outlineOpacity < 1);
        const auto out = stepped("outline-out", [&] { key(Qt::Key_O, Qt::AltModifier); }, 20);
        QVERIFY(!bible->chapterOutline());
        QVERIFY(out[1].outline); QCOMPARE(out[1].outlineX, rest);
        check(out, false);
        QVERIFY(at(out, 80).outline); QVERIFY(!at(out, 85).outline); // Gone after 85 ms.
        sounds->setProperty("reduceMotion", true);
        const auto on = stepped("outline-on-instant", [&] { key(Qt::Key_O, Qt::AltModifier); }, 2);
        QVERIFY(on[1].outline); QCOMPARE(on[1].outlineOpacity, 1.0); QCOMPARE(on[1].outlineX, rest);
        const auto off = stepped("outline-off-instant", [&] { key(Qt::Key_O, Qt::AltModifier); }, 2);
        QVERIFY(!off[1].outline);
    }
    void outlineStickyCurrentAndJump() {
        bible->setChapterOutline(true);
        for (const QString mode : {"verse", "paragraph", "block"}) {
            setMode(mode); navigate("John 3:2");
            QTRY_VERIFY(outlineShown());
            auto outline = item("chapterOutline");
            QTRY_COMPARE(window->property("outlineCurrent").toInt(), 1);
            QVERIFY(item("outlineEntry:1")->property("isCurrent").toBool());
            const QPointF before = outline->mapToScene(QPointF(0, 0));
            // Scrolled away (no selection on screen): the reading line decides,
            // and the outline stays where it is.
            QVERIFY(QMetaObject::invokeMethod(window, "clearTextSelection"));
            const auto at22 = extent(22); QVERIFY(!at22.isEmpty());
            QVERIFY(QMetaObject::invokeMethod(window, "setReaderY", Q_ARG(QVariant, at22["top"].toDouble() - 40)));
            settle();
            QCOMPARE(outline->mapToScene(QPointF(0, 0)), before);
            QTRY_VERIFY2(window->property("outlineCurrent").toInt() == 22 || window->property("outlineCurrent").toInt() == 16, qPrintable(mode));
            // Click a heading: a History jump to its verse, highlighted.
            const int jumps = jumpIndex();
            click(item("outlineEntry:31"));
            QTRY_COMPARE(bible->selectionReference(), QString("John 3:31"));
            QCOMPARE(jumpIndex(), jumps + 1);
            QTRY_COMPARE(window->property("outlineCurrent").toInt(), 31);
            QVERIFY(item("outlineEntry:31")->property("isCurrent").toBool());
            QVERIFY(!item("outlineEntry:1")->property("isCurrent").toBool());
            QCOMPARE(outline->mapToScene(QPointF(0, 0)), before);
            // The chapter title heads the outline, larger and bolder than sections.
            auto title = item("outlineTitleText");
            QCOMPARE(title->property("text").toString(), bible->passage());
            QCOMPARE(bible->passage(), QString("John 3"));
            QVERIFY(item("outlineTitle")->y() < item("outlineEntry:1")->y());
            QVERIFY(title->property("font").value<QFont>().pixelSize() > 12.5 * window->property("readerScale").toDouble() + 1);
            QVERIFY(title->property("font").value<QFont>().bold());
            QVERIFY(!item("outlineTitle")->property("isCurrent").toBool());
            // Clicking it jumps to the top of the chapter.
            click(item("outlineTitle"));
            QTRY_COMPARE(bible->selectionReference(), QString("John 3:1"));
            QCOMPARE(jumpIndex(), jumps + 2);
            QTRY_VERIFY(item("reading")->property("contentY").toDouble() < 1);
            // Before the first heading (John 1 starts its sections at verse 4)
            // the title is current.
            navigate("John 1:2"); QTRY_VERIFY(outlineShown());
            QTRY_COMPARE(window->property("outlineCurrent").toInt(), 0);
            QCOMPARE(item("outlineTitleText")->property("text").toString(), QString("John 1"));
            QVERIFY(item("outlineTitle")->property("isCurrent").toBool());
            QVERIFY(item("outlineTitleMark")->isVisible());
            QVERIFY(!item("outlineEntry:4")->property("isCurrent").toBool());
            navigate("John 1:5"); QTRY_COMPARE(window->property("outlineCurrent").toInt(), 4);
            QVERIFY(!item("outlineTitle")->property("isCurrent").toBool());
        }
    }
    // Whole book: a step into a chapter shown beside brings its view to the
    // reader, and the chapter left keeps its own beside: its text is not
    // laid out again, nor any of its paint repainted, either way.
    void wholeBookCrossingsMoveChapterViews_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("verse") << "verse"; QTest::newRow("block") << "block";
    }
    void wholeBookCrossingsMoveChapterViews() {
        QFETCH(QString, mode);
        const bool verse = mode == "verse";
        bible->setWholeBook(true); setMode(mode);
        navigate("John 3:36"); QTRY_VERIFY(item("contextChapter:John 4")); settle();
        // A chapter's text: the verse's row (Verse) or the chapter's document.
        const auto body = [&](const QString &prefix, int number) {
            return item(prefix + (verse ? "verseBody" + QString::number(number) : QString("blockBody")));
        };
        const auto rebuilds = [&](QQuickItem *text) { return verse ? 0 : text->parentItem()->property("rebuildCount").toInt(); };
        const auto cross = [&](Qt::Key step, const QString &from, int last, const QString &to, int first) {
            auto leaving = body("", last), arriving = body("context:" + to + ":", first);
            QVERIFY(leaving && arriving);
            const int left = rebuilds(leaving), arrived = rebuilds(arriving);
            PaintCounter paints;
            paints.watch(item("readerColumn"));
            key(step);
            QTRY_COMPARE(bible->passage(), to);
            QTRY_VERIFY(!prop("restoringPosition").toBool()); settle();
            QCOMPARE(body("", first), arriving);
            QCOMPARE(body("context:" + from + ":", last), leaving);
            QCOMPARE(rebuilds(arriving), arrived); QCOMPARE(rebuilds(leaving), left);
            QCOMPARE(paints.count, 0);
        };
        cross(Qt::Key_J, "John 3", 36, "John 4", 1);
        cross(Qt::Key_K, "John 4", 1, "John 3", 36);
        bible->setWholeBook(false);
    }
    // Without Whole book, the chapters either side of the one read are made
    // ahead: a step into one moves its view into the reader, and the one left
    // stays made, for a step back; neither is laid out again.
    void chapterCrossingsMovePreparedViews_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("verse") << "verse"; QTest::newRow("block") << "block";
    }
    void chapterCrossingsMovePreparedViews() {
        QFETCH(QString, mode);
        const bool verse = mode == "verse";
        setMode(mode);
        navigate("John 3:36");
        const auto body = [&](const QString &prefix, int number) {
            return item(prefix + (verse ? "verseBody" + QString::number(number) : QString("blockBody")));
        };
        // A Verse view's rows sit in it; a flowing view's text is its own.
        const auto view = [&](QQuickItem *text) { return verse ? text->parentItem()->parentItem() : text->parentItem(); };
        const auto rebuilds = [&](QQuickItem *text) { return verse ? 0 : view(text)->property("rebuildCount").toInt(); };
        const QString john3 = QString("prepared:%1:3:").arg(bible->book()), john4 = QString("prepared:%1:4:").arg(bible->book());
        QTRY_VERIFY(body(john4, 1));
        auto arriving = body(john4, 1), leaving = body("", 36);
        QTRY_VERIFY(!view(arriving)->property("building").toBool());
        const int arrived = rebuilds(arriving), left = rebuilds(leaving);
        key(Qt::Key_J);
        QTRY_COMPARE(bible->passage(), QString("John 4"));
        QTRY_VERIFY(!prop("restoringPosition").toBool()); settle();
        QCOMPARE(body("", 1), arriving);
        QCOMPARE(body(john3, 36), leaving);
        key(Qt::Key_K);
        QTRY_COMPARE(bible->passage(), QString("John 3"));
        QTRY_VERIFY(!prop("restoringPosition").toBool()); settle();
        QCOMPARE(body("", 36), leaving);
        QCOMPARE(body(john4, 1), arriving);
        QCOMPARE(rebuilds(arriving), arrived); QCOMPARE(rebuilds(leaving), left);
    }
    // Whole book: verse steps across chapters, down and back up past a
    // chapter far above that parked and comes back, move all the text one
    // way, together, frame by frame; the outline's current section is the
    // cursor's throughout, never one the view only passes on its way there.
    void wholeBookStepsKeepTextAndOutlineSteady_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("verse") << "verse"; QTest::newRow("block") << "block";
    }
    void wholeBookStepsKeepTextAndOutlineSteady() {
        QFETCH(QString, mode);
        bible->setChapterOutline(true); bible->setWholeBook(true); setMode(mode);
        resize(1400, 500);
        // The whole of John loaded (scrolled through from John 1), John 1
        // parks once the chapter read is far below it.
        navigate("John 1:40");
        auto reading = item("reading");
        for (int i = 0; i < 40 && !item("contextChapter:John 4"); ++i) {
            reading->setProperty("contentY", reading->property("contentHeight").toDouble() - reading->height());
            QTest::qWait(30);
        }
        QTRY_VERIFY(item("contextChapter:John 4"));
        navigate("John 3:30");
        const auto john1 = [&] { return item("contextChapter:John 1"); };
        QTRY_VERIFY(john1() && john1()->property("parked").toBool()); settle();
        sounds->setProperty("reduceMotion", false); settle();
        const QMap<int, QList<int>> sections = {{1, {4, 10, 19, 29, 35}}, {2, {}}, {3, {1, 16, 22, 31}},
            {4, {1, 4, 7, 10, 13, 16, 19, 22, 25, 28, 31, 34, 37, 40, 43, 46, 49, 52}}};
        const auto section = [&](int chapter, int verse) {
            int at = 0;
            for (int heading : sections[chapter]) if (heading <= verse) at = heading;
            return QString("%1:%2").arg(chapter).arg(at);
        };
        QMap<QString, double> previous; int direction = 1;
        const auto check = [&](const QString &at) {
            const QString where = at + " at " + bible->selectionReference();
            QCOMPARE(prop("outlineCurrent").toString(), section(bible->chapter(), bible->cursorVerse()));
            QMap<QString, double> now;
            const double top = reading->property("contentY").toDouble() - prop("columnHold").toDouble();
            for (const auto &span : js("shownVerseSpans", {js("measureVerses")}).toList())
                now[span.toMap()["key"].toString()] = span.toMap()["top"].toDouble() - top;
            double low = 1e9, high = -1e9;
            for (auto it = now.cbegin(); it != now.cend(); ++it)
                if (previous.contains(it.key())) { low = std::min(low, it.value() - previous[it.key()]); high = std::max(high, it.value() - previous[it.key()]); }
            if (high >= low) {
                QVERIFY2(high - low <= 1, qPrintable(QString("text shifted apart by %1..%2, %3").arg(low).arg(high).arg(where)));
                QVERIFY2(direction > 0 ? high <= 1 : low >= -1, qPrintable(QString("text moved back by %1..%2, %3").arg(low).arg(high).arg(where)));
            }
            previous = now;
        };
        StepDriver driver; driver.install(); driver.step(1000); QCoreApplication::processEvents();
        const auto step = [&](Qt::Key key) {
            QTest::keyClick(window, key); check("key");
            for (int i = 0; i < 6; ++i) { driver.step(16); QCoreApplication::processEvents(); check("frame"); }
        };
        check("start");
        for (int i = 0; i < 10; ++i) step(Qt::Key_J);
        QCOMPARE(bible->selectionReference(), QString("John 4:4"));
        direction = -1;
        for (int i = 0; i < 70 && bible->selectionReference() != "John 2:10"; ++i) step(Qt::Key_K);
        QCOMPARE(bible->selectionReference(), QString("John 2:10"));
        QVERIFY(!john1()->property("parked").toBool());
        driver.uninstall();
        sounds->setProperty("reduceMotion", true);
        bible->setWholeBook(false); bible->setChapterOutline(false);
    }
    // The whole book's outline: the book over its chapters, each with its
    // headings under it; the section read is current, in whichever chapter
    // shown it is; an entry of another chapter goes there, as a jump.
    void wholeBookOutline() {
        bible->setChapterOutline(true);
        // Reading settings has its switch, and Commands has it too.
        key(Qt::Key_Comma, Qt::ControlModifier); QTRY_VERIFY(object("settingsDialog")->property("opened").toBool());
        QVERIFY(item("settingwholeBook") && item("settingwholeBook")->isVisible());
        QCOMPARE(item("settingwholeBook")->property("text").toString(), QString("Whole book"));
        click(item("settingwholeBook")); QTRY_VERIFY(bible->wholeBook());
        QVERIFY(item("settingwholeBook")->property("checked").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!object("settingsDialog")->property("visible").toBool());
        QStringList commands;
        QVERIFY(QMetaObject::invokeMethod(window, "openPalette", Q_ARG(QVariant, true), Q_ARG(QVariant, false)));
        for (const auto &entry : js("paletteEntries").toList()) commands << entry.toMap()["title"].toString();
        QVERIFY2(commands.contains("Whole book"), qPrintable(commands.join(", ")));
        key(Qt::Key_Escape); QTRY_VERIFY(!object("passagePalette")->property("visible").toBool());
        navigate("John 3:2");
        QTRY_VERIFY(outlineShown());
        QCOMPARE(item("outlineTitleText")->property("text").toString(), QString("John"));
        for (const QString key : {"1:0", "1:4", "2:0", "3:0", "3:16", "4:0", "4:52"}) QVERIFY2(item("outlineEntry:" + key), qPrintable(key));
        QCOMPARE(item("outlineEntryText:3:0")->property("text").toString().remove(QChar(0x200e)), QString("John 3"));
        QVERIFY(item("outlineEntryText:3:16")->x() > item("outlineEntryText:3:0")->x());
        QTRY_COMPARE(prop("outlineCurrent").toString(), QString("3:1"));
        // Scrolled up into John 2 (shown above): its chapter is current.
        QTRY_VERIFY(item("contextChapter:John 2"));
        const auto john2 = item("contextChapter:John 2");
        item("reading")->setProperty("contentY", john2->y() + 100);
        QTRY_COMPARE(prop("outlineCurrent").toString(), QString("2:0"));
        QCOMPARE(bible->passage(), QString("John 3"));
        // (John 1 comes above, the view staying in John 2.)
        QTRY_VERIFY(item("contextChapter:John 1")); settle();
        QCOMPARE(prop("outlineCurrent").toString(), QString("2:0"));
        const int jumps = jumpIndex();
        item("outlineList")->setProperty("contentY", item("outlineEntry:4:10")->y() - 20); settle();
        click(item("outlineEntry:4:10"));
        QTRY_COMPARE(bible->passage(), QString("John 4")); QCOMPARE(bible->cursorVerse(), 10);
        QCOMPARE(jumpIndex(), jumps + 1);
        QTRY_VERIFY(!prop("restoringPosition").toBool());
        QTRY_COMPARE(prop("outlineCurrent").toString(), QString("4:10"));
        // A chapter's own entry opens it at its first verse, by key too:
        // enter on it, wherever the outline has scrolled itself meanwhile.
        item("chapterOutline")->forceActiveFocus(); QTRY_VERIFY(item("chapterOutline")->hasActiveFocus());
        item("chapterOutline")->setProperty("keyIndex", 1);
        QVERIFY(item("outlineEntryKey:1:0")->isVisible());
        key(Qt::Key_Return); QTRY_COMPARE(bible->selectionReference(), QString("John 1:1"));
        // { } step through the book's sections, across chapters (a chapter
        // without a heading at its start stops at its top).
        navigate("John 3:31");
        key(Qt::Key_BraceRight); QTRY_COMPARE(bible->selectionReference(), QString("John 4:1"));
        QTRY_VERIFY(!prop("restoringPosition").toBool());
        key(Qt::Key_BraceLeft); QTRY_COMPARE(bible->selectionReference(), QString("John 3:31"));
        navigate("John 3:2");
        key(Qt::Key_BraceLeft); QTRY_COMPARE(bible->selectionReference(), QString("John 3:1"));
        QTRY_VERIFY(!prop("restoringPosition").toBool());
        key(Qt::Key_BraceLeft); QTRY_COMPARE(bible->selectionReference(), QString("John 2:1"));
        QTRY_VERIFY(!prop("restoringPosition").toBool());
        key(Qt::Key_BraceLeft); QTRY_COMPARE(bible->selectionReference(), QString("John 1:35"));
        // Off: the chapter's own outline again.
        bible->setWholeBook(false);
        QTRY_COMPARE(item("outlineTitleText")->property("text").toString(), QString("John 1"));
        QVERIFY(item("outlineEntry:4")); QVERIFY(!item("outlineEntry:3:0"));
        bible->setChapterOutline(false);
    }
    void outlineScrollsToCurrentWhenTall() {
        bible->setChapterOutline(true);
        resize(1600, 520);
        navigate("John 4:1");
        QTRY_VERIFY(outlineShown());
        auto list = item("outlineList"); QVERIFY(list);
        QVERIFY(list->property("contentHeight").toDouble() > list->height());
        QCOMPARE(list->property("contentY").toDouble(), 0.0);
        navigate("John 4:53"); bible->selectVerses(53, 53);
        QTRY_COMPARE(window->property("outlineCurrent").toInt(), 52);
        QTRY_VERIFY(list->property("contentY").toDouble() > 0);
        auto current = item("outlineEntry:52");
        const double y = current->mapToItem(list, QPointF(0, 0)).y();
        QVERIFY2(y >= 0 && y + current->height() <= list->height() + 0.5, qPrintable(QString::number(y)));
    }

    // ---- { and } -------------------------------------------------------------
    void headingKeysCountsHistoryAndToasts() {
        for (const QString mode : {"verse", "paragraph", "block"}) {
            setMode(mode); navigate("John 3:2");
            int jumps = jumpIndex();
            key(Qt::Key_BraceRight);
            QTRY_COMPARE(bible->selectionReference(), QString("John 3:16"));
            QCOMPARE(jumpIndex(), ++jumps);
            type("2"); key(Qt::Key_BraceRight);
            QTRY_COMPARE(bible->selectionReference(), QString("John 3:31"));
            QCOMPARE(jumpIndex(), ++jumps);
            // The verse is brought on screen.
            QTRY_VERIFY(!extent(31).isEmpty());
            const double top = extent(31)["top"].toDouble(), y = item("reading")->property("contentY").toDouble();
            QVERIFY2(top >= y && top < y + item("reading")->height(), qPrintable(mode));
            key(Qt::Key_BraceRight);
            QCOMPARE(bible->selectionReference(), QString("John 3:31")); QCOMPARE(jumpIndex(), jumps);
            QCOMPARE(toast(), QString("No more headings below"));
            key(Qt::Key_BraceLeft); QTRY_COMPARE(bible->selectionReference(), QString("John 3:22")); QCOMPARE(jumpIndex(), ++jumps);
            // A count past the first heading stops at the chapter top.
            type("5"); key(Qt::Key_BraceLeft);
            QTRY_COMPARE(bible->selectionReference(), QString("John 3:1")); QCOMPARE(jumpIndex(), ++jumps);
            QTRY_VERIFY(item("reading")->property("contentY").toDouble() < 1);
            // Already there: a brief toast, no jump.
            key(Qt::Key_BraceLeft); QCOMPARE(toast(), QString("Already at the top of John 3"));
            QCOMPARE(bible->selectionReference(), QString("John 3:1")); QCOMPARE(jumpIndex(), jumps);
            // History goes back through the heading jumps.
            key(Qt::Key_BracketLeft); QTRY_COMPARE(bible->selectionReference(), QString("John 3:22"));
            // Inside a section, { goes to its own heading.
            navigate("John 3:20"); key(Qt::Key_BraceLeft); QTRY_COMPARE(bible->selectionReference(), QString("John 3:16"));
            // A chapter without headings.
            navigate("John 2:3"); jumps = jumpIndex();
            key(Qt::Key_BraceRight); QCOMPARE(toast(), QString("No headings in John 2")); QCOMPARE(jumpIndex(), jumps);
            QCOMPARE(bible->selectionReference(), QString("John 2:3"));
            // { goes to the chapter top instead.
            key(Qt::Key_BraceLeft); QTRY_COMPARE(bible->selectionReference(), QString("John 2:1")); QCOMPARE(jumpIndex(), ++jumps);
            // Before the first heading: the chapter top too.
            navigate("John 1:3"); jumps = jumpIndex();
            key(Qt::Key_BraceLeft); QTRY_COMPARE(bible->selectionReference(), QString("John 1:1")); QCOMPARE(jumpIndex(), ++jumps);
            QTRY_VERIFY(item("reading")->property("contentY").toDouble() < 1);
        }
        // A version without any headings.
        QVERIFY(bible->switchVersion(plainId)); QVERIFY(!bible->hasHeadings()); settle();
        navigate("John 3:2");
        key(Qt::Key_BraceRight); QCOMPARE(toast(), bible->versionAbbreviation() + " has no section headings");
        const int jumps = jumpIndex();
        key(Qt::Key_BraceLeft); QTRY_COMPARE(bible->selectionReference(), QString("John 3:1")); QCOMPARE(jumpIndex(), jumps + 1);
        key(Qt::Key_BraceLeft); QCOMPARE(toast(), QString("Already at the top of John 3"));
    }

    // ---- Margin notes -------------------------------------------------------
    void marginNotesPlacementAllLayouts() {
        note("John 3:2", 2, 3, "Two verses with a note about the night visit.");
        note("John 3:16", 16, 16, "A single verse.");
        note("John 3:30", 30, 30, "Late in the chapter: see John 1:1 for the opening.");
        navigate("John 3:1");
        QVERIFY(!bible->marginNotes()); QVERIFY(!item("marginNote:John 3:16") || !item("marginNote:John 3:16")->isVisible());
        key(Qt::Key_W, Qt::AltModifier); QTRY_VERIFY(bible->marginNotes());
        QCOMPARE(toast(), QString("Show notes in margin on"));
        { BibleBackend reopened; QVERIFY(reopened.marginNotes()); }
        for (const QString mode : {"verse", "paragraph", "block"}) {
            setMode(mode); navigate("John 3:1");
            QTRY_COMPARE(layout().size(), 3);
            const auto column = item("readerColumn");
            for (const QString reference : {"John 3:2–3", "John 3:16", "John 3:30"}) {
                auto note = item("marginNote:" + reference); QVERIFY2(note, qPrintable(reference));
                QTRY_VERIFY(note->isVisible());
                const auto place = placement(reference);
                const int first = reference == "John 3:2–3" ? 2 : reference == "John 3:16" ? 16 : 30;
                // Aligned with its passage (nothing crowds these), right of the text.
                QCOMPARE(place["passageTop"].toDouble(), extent(first)["top"].toDouble());
                QVERIFY2(std::abs(note->y() - extent(first)["top"].toDouble()) < 1, qPrintable(mode + " " + reference));
                QVERIFY(note->x() >= column->x() + column->width());
                QVERIFY(note->x() + note->width() <= item("reading")->width());
                QVERIFY(note->property("visible").toBool());
                auto text = item("marginNoteText:" + reference); QVERIFY(text);
                QVERIFY(text->property("font").value<QFont>().italic());
                QVERIFY(!text->property("truncated").toBool());
                // Connector: "}" at the passage, "{" before the note, arrow into "}".
                auto connector = item("marginConnector:" + reference); QVERIFY(connector);
                QTRY_VERIFY(!value(connector->property("geometry")).toMap().isEmpty());
                const auto geometry = value(connector->property("geometry")).toMap();
                const auto noteTip = geometry["noteTip"].toMap(), passageTip = geometry["passageTip"].toMap(), arrowEnd = geometry["arrowEnd"].toMap();
                QVERIFY(noteTip["x"].toDouble() < note->x());
                QVERIFY(passageTip["x"].toDouble() > column->x() + column->width() - 9);
                QVERIFY(passageTip["x"].toDouble() < noteTip["x"].toDouble());
                QVERIFY(arrowEnd["x"].toDouble() > passageTip["x"].toDouble() && arrowEnd["x"].toDouble() - passageTip["x"].toDouble() < 5);
                QCOMPARE(arrowEnd["y"].toDouble(), passageTip["y"].toDouble());
                // Tips lean toward each other.
                const double noteCentre = place["y"].toDouble() + place["height"].toDouble() / 2;
                const double passageCentre = (place["passageTop"].toDouble() + place["passageBottom"].toDouble()) / 2;
                const double offset = noteCentre - passageCentre;
                QVERIFY(place["tips"].toMap()["note"].toDouble() * offset <= 0);
                QVERIFY(place["tips"].toMap()["passage"].toDouble() * offset >= 0);
                QVERIFY(std::abs(noteTip["y"].toDouble() - (noteCentre + place["tips"].toMap()["note"].toDouble())) < 1);
                QVERIFY(passageTip["y"].toDouble() >= place["passageTop"].toDouble() && passageTip["y"].toDouble() <= place["passageBottom"].toDouble());
                // Deterministic repaint.
                QVERIFY(QMetaObject::invokeMethod(connector, "requestPaint")); settle();
                QCOMPARE(value(connector->property("geometry")).toMap(), geometry);
            }
            // Range: the "}" spans both verses.
            const auto range = placement("John 3:2–3");
            QCOMPARE(range["passageBottom"].toDouble() > extent(2)["bottom"].toDouble() - 1, true);
        }
        // Focus mode hides them.
        bible->setFocusMode(true); QTRY_VERIFY(!item("marginLayer")->isVisible());
        bible->setFocusMode(false); QTRY_VERIFY(item("marginLayer")->isVisible());
        key(Qt::Key_W, Qt::AltModifier); QTRY_VERIFY(!bible->marginNotes());
        QTRY_VERIFY(!item("marginLayer")->isVisible());
    }
    // Notes in a chapter shown beside the one read (a selection across
    // chapters) are in the margin too, beside their passage there; a note
    // across both chapters is one note, beside all of its passage.
    void marginNotesInChaptersShownBeside() {
        note("John 3:30", 30, 32, "A note on the chapter shown beside.");
        QVERIFY(bible->navigate("John 3:36-4:2")); bible->setNoteText("Across the chapters.");
        note("John 4:5", 5, 5, "In the chapter read.");
        bible->setMarginNotes(true);
        const auto span = [this](const QString &key) {
            for (const auto &entry : js("shownVerseSpans", {QVariant()}).toList()) if (entry.toMap()["key"] == key) return entry.toMap();
            return QVariantMap();
        };
        const QString beside = QString::fromUtf8("John 3:30–32"), across = QString::fromUtf8("John 3:36–4:2");
        for (const QString mode : {"verse", "paragraph", "block"}) {
            setMode(mode); navigate("John 3:35-4:1");
            QCOMPARE(bible->passage(), QString("John 4")); QTRY_VERIFY(item("contextChapter:John 3"));
            QTRY_VERIFY(notesShown()); QTRY_COMPARE(layout().size(), 3);
            QTRY_COMPARE(placement(beside)["passageTop"].toDouble(), span("43:3:30")["top"].toDouble());
            QCOMPARE(placement(beside)["passageBottom"].toDouble(), span("43:3:32")["bottom"].toDouble());
            QCOMPARE(placement(across)["passageTop"].toDouble(), span("43:3:36")["top"].toDouble());
            QCOMPARE(placement(across)["passageBottom"].toDouble(), span("43:4:2")["bottom"].toDouble());
            QVERIFY(item("marginNote:" + beside) && item("marginNote:" + beside)->isVisible());
            // In the margin, they leave the text unmarked (as in the chapter read).
            if (mode == "verse") {
                auto rail = item("context:John 3:verseNoteMarker30"); QVERIFY(!rail || !rail->isVisible());
            } else {
                auto marker = item("context:John 3:blockInlineMarker:note:open:" + beside); QVERIFY(!marker || !marker->isVisible());
            }
        }
        bible->setMarginNotes(false);
    }
    void marginNotesHideInlineNoteMarks() {
        note("John 3:16", 16, 16, "A note shown in the margin.");
        QVERIFY(bible->navigate("John 3:16")); bible->selectVerses(16, 16);
        QVERIFY(!bible->createAndAssignTag(bible->tagScope(), "Stays").isEmpty());
        resize(1400, 900);
        setMode("verse"); navigate("John 3:16");
        const auto shown = [this](const QString &name) { auto found = item(name); return found && found->isVisible(); };
        const auto shot = [this](const QString &name) {
            const auto directory = qEnvironmentVariable("OMAWORD_MARGINS_SCREENSHOTS");
            if (!directory.isEmpty()) { settle(); QVERIFY(window->grabWindow().save(directory + "/" + name + ".png")); }
        };
        QTRY_VERIFY(shown("verseNoteMarker16")); QVERIFY(shown("verseTagMarker16"));
        shot("inline-notes-verse-off");
        bible->setMarginNotes(true); QTRY_VERIFY(notesShown());
        // Only the note mark goes; the rails do not shift.
        const double tagX = item("verseTagMarker16")->x();
        QTRY_VERIFY(!shown("verseNoteMarker16")); QVERIFY(shown("verseTagMarker16"));
        QCOMPARE(item("verseTagMarker16")->x(), tagX);
        shot("inline-notes-verse-margin");
        // No room for the margin: the inline mark returns.
        resize(900, 900); QTRY_VERIFY(!notesShown()); QTRY_VERIFY(shown("verseNoteMarker16"));
        resize(1400, 900); QTRY_VERIFY(notesShown()); QTRY_VERIFY(!shown("verseNoteMarker16"));
        const auto blockKinds = [this]() {
            QStringList out;
            auto reader = object("blockReader");
            if (!reader) return out;
            for (const auto &marker : value(reader->property("inlineMarkers")).toList()) out << marker.toMap()["kind"].toString();
            return out;
        };
        for (const QString mode : {"paragraph", "block"}) {
            setMode(mode); navigate("John 3:16");
            QTRY_VERIFY(blockKinds().contains("tag")); QVERIFY(!blockKinds().contains("note"));
            shot("inline-notes-" + mode + "-margin");
            bible->setMarginNotes(false); QTRY_VERIFY(!notesShown());
            QTRY_VERIFY(blockKinds().contains("note")); QVERIFY(blockKinds().contains("tag"));
            shot("inline-notes-" + mode + "-off");
            bible->setMarginNotes(true); QTRY_VERIFY(notesShown());
            QTRY_VERIFY(!blockKinds().contains("note"));
        }
        bible->setMarginNotes(false);
    }
    void marginNotesShiftReaderAndOutlineYields() {
        note("John 3:16", 16, 16, "A note that needs room in the margin.");
        bible->setChapterOutline(true);
        resize(1200, 900);
        navigate("John 3:16");
        const double natural = window->property("readerNaturalX").toDouble();
        QTRY_VERIFY(outlineShown());
        QCOMPARE(columnX(), natural);
        bible->setMarginNotes(true);
        QTRY_VERIFY(notesShown());
        // The notes shift the reader left; the outline yields its space.
        QTRY_VERIFY(columnX() < natural);
        QCOMPARE(columnX(), natural - space()["shift"].toDouble());
        QTRY_VERIFY(!outlineShown());
        auto note = item("marginNote:John 3:16"); QTRY_VERIFY(note && note->isVisible());
        QVERIFY(note->x() + note->width() <= 1200);
        const double shifted = columnX();
        // No notes in this chapter: the room stays, so the column does not move.
        navigate("John 2:1"); QTRY_VERIFY(prop("marginNoteEntries").toList().isEmpty());
        settle(); QCOMPARE(columnX(), shifted); QVERIFY(notesShown()); QVERIFY(!outlineShown());
        QVERIFY(prop("marginLayout").toList().isEmpty());
        navigate("John 3:1"); QCOMPARE(columnX(), shifted); QTRY_VERIFY(outlineShown() == false);
        // Where both fit at their minimums, the notes shift the column only
        // as far as leaves the outline its own.
        navigate("John 3:16");
        bool both = false;
        for (int width = 1200; width <= 1800 && !both; width += 20) { resize(width, 900); both = notesShown() && outlineShown(); }
        QVERIFY(both);
        const double kept = space()["shift"].toDouble();
        bible->setChapterOutline(false); QTRY_VERIFY(!outlineShown()); settle();
        QVERIFY2(space()["shift"].toDouble() > kept, qPrintable(QString("%1 / %2").arg(space()["shift"].toDouble()).arg(kept)));
        bible->setChapterOutline(true); QTRY_VERIFY(outlineShown());
        resize(1200, 900); navigate("John 3:1");
        // The scrollbar's width is kept too: a chapter that fits without
        // scrolling keeps the place (wide enough that the room sets the shift).
        resize(1400, 900);
        const double scrolling = columnX();
        QVERIFY(item("readerScrollbar")->isVisible()); QVERIFY(space()["shift"].toDouble() > 0);
        QVERIFY(scrolling > window->property("readerSideInset").toDouble());
        window->resize(1400, 20000); QTRY_COMPARE(window->height(), 20000); settle();
        QTRY_VERIFY(!item("readerScrollbar")->isVisible());
        QCOMPARE(columnX(), scrolling);
        resize(1200, 900);
        // Too narrow even after shifting: hidden, no shift.
        resize(900, 900); QTRY_VERIFY(!notesShown());
        QCOMPARE(columnX(), window->property("readerNaturalX").toDouble());
        // Wide: both, no shift.
        resize(1920, 900); QTRY_VERIFY(notesShown() && outlineShown());
        QCOMPARE(space()["shift"].toDouble(), 0.0);
    }
    void marginNotesToggleSlidesTheReader() {
        note("John 3:16", 16, 16, "A note that needs room in the margin.");
        resize(1200, 900);
        navigate("John 3:16");
        const double natural = window->property("readerNaturalX").toDouble();
        auto layer = item("marginLayer");
        // Motion allowed: the column passes through intermediate x on the way.
        sounds->setProperty("reduceMotion", false);
        auto sample = [&](double from, double to) {
            bool between = false;
            QElapsedTimer clock; clock.start();
            while (clock.elapsed() < 600) {
                const double x = columnX();
                if ((x - from) * (x - to) < 0) between = true;
                if (x == to && clock.elapsed() > 80) break;
                QTest::qWait(5);
            }
            return between;
        };
        key(Qt::Key_W, Qt::AltModifier); QVERIFY(bible->marginNotes());
        const double shifted = natural - space()["shift"].toDouble();
        QVERIFY(shifted < natural);
        QVERIFY(sample(natural, shifted));
        QTRY_COMPARE(columnX(), shifted);
        QTRY_COMPARE(layer->opacity(), 1.0);
        key(Qt::Key_W, Qt::AltModifier); QVERIFY(!bible->marginNotes());
        QTRY_VERIFY(layer->opacity() < 1); // fading out before the slide back
        QVERIFY(sample(shifted, natural));
        QTRY_COMPARE(columnX(), natural);
        // Resizing does not animate.
        key(Qt::Key_W, Qt::AltModifier); QTRY_COMPARE(columnX(), shifted);
        window->resize(1300, 900); QTRY_COMPARE(window->width(), 1300);
        QCOMPARE(window->property("marginSlideOffset").toDouble(), 0.0);
        QCOMPARE(columnX(), space()["columnX"].toDouble());
        // Reduce motion: the toggle jumps.
        sounds->setProperty("reduceMotion", true);
        key(Qt::Key_W, Qt::AltModifier);
        QCOMPARE(columnX(), window->property("readerNaturalX").toDouble()); QCOMPARE(layer->opacity(), 1.0);
        key(Qt::Key_W, Qt::AltModifier);
        QCOMPARE(columnX(), space()["columnX"].toDouble()); QVERIFY(columnX() < window->property("readerNaturalX").toDouble());
        bible->setMarginNotes(false);
    }
    void crowdedMarginNotesTruncate() {
        const QString long1 = QString("A long reflection that keeps going. ").repeated(30);
        const QString long2 = QString("Another long note, shorter than the first. ").repeated(8);
        note("John 3:4", 4, 4, long1); note("John 3:5", 5, 5, long2); note("John 3:6", 6, 6, "Short.");
        bible->setMarginNotes(true);
        resize(1600, 520);
        navigate("John 3:4");
        QTRY_COMPARE(layout().size(), 3);
        const auto first = placement("John 3:4"), second = placement("John 3:5"), third = placement("John 3:6");
        QVERIFY(first["truncated"].toBool());
        QVERIFY(!third["truncated"].toBool());
        QVERIFY(first["naturalLines"].toInt() > first["lines"].toInt());
        auto text = item("marginNoteText:John 3:4");
        QTRY_VERIFY(text->property("truncated").toBool());
        QCOMPARE(text->property("lineCount").toInt(), first["lines"].toInt());
        // The constraint holds: each note shares one viewport with its passage.
        const double viewport = item("reading")->height() - window->property("floatingSafety").toDouble();
        for (const auto &entry : layout()) {
            const auto p = entry.toMap(); if (p["hidden"].toBool()) continue;
            QVERIFY(std::max(p["y"].toDouble() + p["height"].toDouble(), p["passageBottom"].toDouble())
                - std::min(p["y"].toDouble(), p["passageTop"].toDouble()) <= viewport + 1);
        }
        QVERIFY(second["y"].toDouble() >= first["y"].toDouble() + first["height"].toDouble());
    }
    void marginNoteClickLinkAndDrawer() {
        note("John 3:16", 16, 16, "Compare John 1:1 with this verse.");
        bible->setMarginNotes(true);
        navigate("John 3:10");
        auto note = item("marginNote:John 3:16"); QTRY_VERIFY(note && note->isVisible());
        QVERIFY(QMetaObject::invokeMethod(window, "scrollToVerse", Q_ARG(QVariant, 16), Q_ARG(QVariant, true))); settle();
        // Clicking the note opens Notes on it.
        auto text = item("marginNoteText:John 3:16");
        click(note, QPointF(note->width() - 4, 4));
        QTRY_VERIFY(object("notesDrawer")->property("opened").toBool());
        QCOMPARE(bible->selectionReference(), QString("John 3:16"));
        // Docked, Notes narrows the reader, and margin notes step aside until it closes.
        auto drawer = object("notesDrawer");
        QVERIFY(window->property("notesDocked").toBool());
        QTRY_COMPARE(window->property("notesDock").toDouble(), drawer->property("width").toDouble());
        settle();
        QTRY_VERIFY(!notesShown());
        QVERIFY(QMetaObject::invokeMethod(window, "closeNotes")); QTRY_VERIFY(!drawer->property("visible").toBool());
        QTRY_VERIFY(notesShown());
        QTRY_COMPARE(window->property("notesDock").toDouble(), 0.0);
        settle();
        text = item("marginNoteText:John 3:16"); QTRY_VERIFY(text && text->isVisible());
        // The exact passage link is clickable: a History jump.
        QPointF link(-1, -1);
        for (int y = 2; y < text->height() && link.x() < 0; y += 3)
            for (int x = 2; x < text->width(); x += 3) {
                QString at; QMetaObject::invokeMethod(text, "linkAt", Q_RETURN_ARG(QString, at), Q_ARG(qreal, x), Q_ARG(qreal, y));
                if (at.startsWith("omaword-passage:")) { link = QPointF(x, y); break; }
            }
        QVERIFY(link.x() >= 0);
        const int jumps = jumpIndex();
        click(text, link);
        QTRY_COMPARE(bible->selectionReference(), QString("John 1:1"));
        QCOMPARE(jumpIndex(), jumps + 1);
        QVERIFY(!object("notesDrawer")->property("visible").toBool());
    }

    // Opening docked Notes: the reader lays out once at its new width and
    // glides there, its margin notes stepping aside; closing glides back with
    // them, never stale.
    void notesDockGlidesWithItsMargins() {
        sounds->setProperty("reduceMotion", false);
        note("John 3:16", 16, 16, "A note beside its passage.");
        bible->setMarginNotes(true);
        resize(1800, 900);
        navigate("John 3:16");
        QTRY_VERIFY(notesShown()); QTRY_VERIFY(item("marginNote:John 3:16") && item("marginNote:John 3:16")->isVisible()); settle();
        const double before = columnX();
        const auto check = [&](const QList<Frame> &frames, double from, double to) {
            QVERIFY(frames.size() > 4);
            QVERIFY2(std::abs(frames.first().columnX - from) < 0.5, qPrintable(describe(frames.first())));
            QVERIFY2(std::abs(frames.last().columnX - to) < 0.5, qPrintable(describe(frames.last())));
            int between = 0;
            for (int i = 1; i < frames.size(); ++i) {
                const auto &f = frames[i];
                // Monotonic toward its place, and the notes beside it laid out for it.
                QVERIFY2((to - frames[i - 1].columnX) * (to - f.columnX) >= 0 && std::abs(to - f.columnX) <= std::abs(to - frames[i - 1].columnX) + 0.5,
                         qPrintable(describe(frames[i - 1]) + "\n" + describe(f)));
                if (f.shownNotes) {
                    QVERIFY2(f.stale < 1, qPrintable(describe(f)));
                    QVERIFY2(std::abs((f.noteX - f.columnX) - (frames.last().noteX - frames.last().columnX)) < 1, qPrintable(describe(f)));
                }
                between += std::abs(f.columnX - from) > 1 && std::abs(f.columnX - to) > 1;
            }
            if (!between) for (const auto &f : frames) qWarning().noquote() << describe(f);
            QVERIFY2(between > 0, "the column jumped rather than glided");
        };
        const auto open = stepped("notes-open", [&] { key(Qt::Key_W); }, 30);
        QVERIFY(prop("notesDocked").toBool()); QVERIFY(object("notesDrawer")->property("opened").toBool());
        const double docked = columnX();
        QVERIFY(docked < before);
        check(open, before, docked);
        QCOMPARE(open.last().shownNotes, 0);
        const auto close = stepped("notes-close", [&] { QVERIFY(QMetaObject::invokeMethod(window, "closeNotes")); }, 30);
        check(close, docked, before);
        QVERIFY(close.last().shownNotes > 0);
        // Disable animations: it jumps.
        sounds->setProperty("reduceMotion", true);
        const auto jump = stepped("notes-jump", [&] { key(Qt::Key_W); }, 3);
        QCOMPARE(jump[1].columnX, docked);
    }

    // A panel docking beside the text (Notes, Marks) or a resize changes the
    // reader's width: a selection still wholly in view stays where it is,
    // and one out of view comes back to the middle.
    void panelsKeepAVisibleSelectionStill() {
        resize(1800, 900);
        navigate("John 3:16");
        const auto contentY = [this] { return item("reading")->property("contentY").toDouble(); };
        const auto opened = [this](const QString &mode) { return prop("marginMode").toString() == mode && object("notesDrawer")->property("opened").toBool(); };
        const auto closed = [this] { return !object("notesDrawer")->property("visible").toBool(); };
        const auto view = [&] {
            const double top = contentY();
            return std::make_pair(top, top + item("reading")->height() - prop("floatingSafety").toDouble());
        };
        const auto inView = [&] {
            const auto verse = js("measureVerse", {16}).toMap(); const auto [top, bottom] = view();
            return verse["top"].toDouble() >= top && verse["bottom"].toDouble() <= bottom;
        };
        // Away from where a jump puts it, still in view.
        QVERIFY(QMetaObject::invokeMethod(window, "scrollBy", Q_ARG(QVariant, -150))); settle();
        QVERIFY(inView());
        const double before = contentY();
        key(Qt::Key_W); QTRY_VERIFY(opened("notes")); QVERIFY(prop("notesDocked").toBool()); settle();
        QCOMPARE(contentY(), before); QVERIFY(inView());
        key(Qt::Key_Escape); QTRY_VERIFY(closed()); settle();
        QCOMPARE(contentY(), before);
        key(Qt::Key_M, Qt::ShiftModifier); QTRY_VERIFY(opened("marks")); settle();
        QCOMPARE(contentY(), before);
        key(Qt::Key_Escape); QTRY_VERIFY(closed()); settle();
        QCOMPARE(contentY(), before);
        resize(1500, 900);
        QCOMPARE(contentY(), before);
        // Scrolled out of view, it comes back to the middle.
        QVERIFY(QMetaObject::invokeMethod(window, "scrollBy", Q_ARG(QVariant, 1500))); settle();
        QVERIFY(!inView());
        key(Qt::Key_M, Qt::ShiftModifier); QTRY_VERIFY(opened("marks")); QTRY_VERIFY(inView());
        const auto verse = js("measureVerse", {16}).toMap(); const auto [top, bottom] = view();
        QVERIFY(std::abs((verse["top"].toDouble() + verse["bottom"].toDouble()) / 2 - (top + bottom) / 2) < 2);
    }

    void panelOpeningCentersClippedSelection_data() {
        QTest::addColumn<QString>("panel");
        QTest::addColumn<int>("width");
        QTest::addColumn<bool>("clippedAbove");
        for (const QString panel : {"notes", "desk", "marks"})
            for (int width : {1000, 1800})
                for (bool above : {false, true})
                    QTest::newRow(qPrintable(panel + QString("-%1-%2").arg(width).arg(above ? "above" : "below"))) << panel << width << above;
    }
    void panelOpeningCentersClippedSelection() {
        QFETCH(QString, panel); QFETCH(int, width); QFETCH(bool, clippedAbove);
        resize(width, 900); navigate("John 3:16");
        const auto verse = js("measureVerse", {16}).toMap();
        const double available = item("reading")->height() - prop("floatingSafety").toDouble();
        // Only eight pixels are clipped: partially visible must count too.
        js("setReaderY", {clippedAbove ? verse["top"].toDouble() + 8 : verse["bottom"].toDouble() - available - 8});
        if (panel == "notes") key(Qt::Key_W);
        else key(panel == "desk" ? Qt::Key_D : Qt::Key_M, Qt::ShiftModifier);
        QTRY_VERIFY(object("notesDrawer")->property("opened").toBool());
        QTRY_VERIFY(!object("readerSlide")->property("running").toBool());
        const auto centered = [&] {
            const auto now = js("measureVerse", {16}).toMap();
            const double middle = item("reading")->property("contentY").toDouble()
                + (item("reading")->height() - prop("floatingSafety").toDouble()) / 2;
            return std::abs((now["top"].toDouble() + now["bottom"].toDouble()) / 2 - middle) < 2;
        };
        QTRY_VERIFY(centered());
        // Switching panels also checks visibility, without a drawer resize.
        js("setReaderY", {js("measureVerse", {16}).toMap()["top"].toDouble() + 8});
        if (panel == "notes") QVERIFY(QMetaObject::invokeMethod(window, "openDesk"));
        else QVERIFY(QMetaObject::invokeMethod(window, "openNotes", Q_ARG(QVariant, true)));
        QTRY_VERIFY(centered());
    }

    // ---- Focus mode with margins ----------------------------------------------
    // Entering and leaving focus mode with margin notes and the outline on:
    // frame by frame (verifyTransition), with rapid retargeting toggles and
    // Disable animations. OMAWORD_FOCUS_TIMELINE=<dir> dumps every timeline
    // (real time and stepped) and the stepped frames as PNGs.
    void focusModeTransitions_data() {
        QTest::addColumn<QString>("mode");
        for (const char *mode : {"verse", "paragraph", "block"}) QTest::newRow(mode) << QString(mode);
    }
    void focusModeTransitions() {
        QFETCH(QString, mode);
        note("John 3:2", 2, 3, "Two verses with a note about the night visit.");
        note("John 3:14", 14, 14, "Lifted up: compare the bronze serpent.");
        note("John 3:16", 16, 17, "The heart of the chapter, and why it was sent.");
        note("John 3:19", 19, 19, "Light and darkness.");
        note("John 3:30", 30, 30, "Late in the chapter.");
        bible->setMarginNotes(true); bible->setChapterOutline(true);
        // Wide enough for both margins, narrow enough that the notes shift the column.
        resize(1400, 900);
        setMode(mode); navigate("John 3:16");
        QTRY_VERIFY(notesShown()); QTRY_VERIFY(outlineShown()); QTRY_VERIFY(layout().size() >= 4);
        const double shifted = columnX(), centred = window->property("readerNaturalX").toDouble();
        QVERIFY2(centred - shifted > 40, qPrintable(QString::number(centred - shifted)));
        const double typewriter = 0.4 * item("reading")->height();
        const int rebuilds = mode == "verse" ? -1 : 1;
        sounds->setProperty("reduceMotion", false);
        // Real time, for the record only (frame pacing here is software rendering).
        if (!qEnvironmentVariableIsEmpty("OMAWORD_FOCUS_TIMELINE")) {
            dump(mode + "-enter", record([&]() { key(Qt::Key_F); }, 400));
            dump(mode + "-leave", record([&]() { key(Qt::Key_F); }, 400));
        }
        QVERIFY(!bible->focusMode());
        // From the chapter as navigated: the cursor verse near the top.
        navigate("John 3:16"); QTRY_COMPARE(columnX(), shifted);
        QVERIFY(QMetaObject::invokeMethod(window, "scrollToVerse", Q_ARG(QVariant, 16), Q_ARG(QVariant, true))); settle();
        QVERIFY(std::abs(verseTop(16) - typewriter) > 100);
        const auto enter = stepped(mode + "-enter", [&]() { key(Qt::Key_F); });
        dump(mode + "-enter-stepped", enter);
        QVERIFY(bible->focusMode());
        verifyTransition(enter, true, centred, typewriter, rebuilds); if (QTest::currentTestFailed()) return;
        // Scrolled a little in focus mode, so leaving shows the anchor hold.
        QVERIFY(QMetaObject::invokeMethod(window, "setReaderY", Q_ARG(QVariant, item("reading")->property("contentY").toDouble() - 37))); settle();
        const auto leave = stepped(mode + "-leave", [&]() { key(Qt::Key_F); });
        dump(mode + "-leave-stepped", leave);
        QVERIFY(!bible->focusMode());
        verifyTransition(leave, false, shifted, -1, rebuilds); if (QTest::currentTestFailed()) return;
        // At the chapter top the top stays: the verse holds through the
        // reflow, then the reader slides back up to the title.
        navigate("John 3:1"); QTRY_COMPARE(item("reading")->property("contentY").toDouble(), 0.0);
        for (const bool entering : {true, false}) {
            const QString name = mode + (entering ? "-top-enter" : "-top-leave");
            const auto top = stepped(name, [&]() { key(Qt::Key_F); });
            dump(name + "-stepped", top);
            QCOMPARE(bible->focusMode(), entering);
            QVERIFY2(std::abs(top[1].cursorY - top[0].cursorY) <= 1, qPrintable(describe(top[1])));
            for (int i = 2; i < top.size(); ++i)
                QVERIFY2(std::abs(top[i].cursorY + top[i].contentY - top[i - 1].cursorY - top[i - 1].contentY) <= 1, qPrintable(describe(top[i])));
            // The chapter title stays where it is throughout.
            for (const auto &f : top) QVERIFY2(std::abs(f.titleY - top[0].titleY) <= 1, qPrintable(QString("chapter title moved from %1: ").arg(top[0].titleY) + describe(f)));
            QCOMPARE(top.last().contentY, 0.0);
        }
        navigate("John 3:16"); QTRY_COMPARE(columnX(), shifted);

        // Rapid toggles retarget from wherever things are: no jumps, nothing
        // stuck, one rebuild per toggle.
        const auto toggle = [&]() { key(Qt::Key_F); };
        const auto rapid = stepped(mode + "-rapid", {toggle, toggle, toggle, toggle, toggle}, {3, 2, 1, 4, 30});
        dump(mode + "-rapid-stepped", rapid);
        QVERIFY(bible->focusMode());
        const double navSpan = std::abs(enter.last().navY - enter.first().navY);
        for (int i = 1; i < rapid.size(); ++i) {
            const Frame &f = rapid[i], &p = rapid[i - 1];
            const QString at = describe(f);
            QVERIFY2(std::abs(f.columnX - p.columnX) <= 0.25 * (centred - shifted) + 0.5, qPrintable("column jumped: " + at));
            QVERIFY2(std::abs(f.navY - p.navY) <= 0.25 * navSpan + 0.5, qPrintable("chapter buttons jumped: " + at));
            QVERIFY2(f.columnX >= shifted - 0.01 && f.columnX <= centred + 0.01, qPrintable("column overshot: " + at));
            const double opacity = f.notes ? f.notesOpacity : 0, previous = p.notes ? p.notesOpacity : 0;
            QVERIFY2(std::abs(opacity - previous) <= 0.3, qPrintable("notes popped: " + at));
            QVERIFY2(f.stale <= 1, qPrintable("stale notes painted: " + at));
            QVERIFY2(f.connectorGap >= 2.5, qPrintable("connector over the text: " + at));
            QVERIFY2(!f.dimStale, qPrintable("focus veil off the focused text: " + at));
            // Each toggle holds the cursor verse; between them it only scrolls.
            if (f.focus != p.focus) QVERIFY2(std::abs(f.cursorY - p.cursorY) <= 1, qPrintable("cursor verse jumped: " + at));
            else QVERIFY2(std::abs(f.cursorY + f.contentY - p.cursorY - p.contentY) <= 1, qPrintable("text reflowed under the cursor verse: " + at));
        }
        QCOMPARE(rapid.last().columnX, centred); QCOMPARE(rapid.last().slide, 0.0);
        QVERIFY(!rapid.last().notes && !rapid.last().outline);
        QVERIFY(std::abs(rapid.last().cursorY - typewriter) <= 1);
        if (rebuilds > 0) QCOMPARE(rapid.last().rebuilds - rapid.first().rebuilds, 5);
        QCOMPARE(prop("marginFocusLevel").toDouble(), 0.0);
        QCOMPARE(rapid.last().navY, enter.last().navY);
        for (const char *name : {"marginSlide", "marginExit", "marginEnter", "chapterNavGlide", "readerSlide"})
            QVERIFY2(!object(name)->property("running").toBool(), name);
        key(Qt::Key_F); QTRY_COMPARE(columnX(), shifted);
        QTRY_COMPARE(item("marginLayer")->opacity(), 1.0); QCOMPARE(item("chapterOutline")->opacity(), 1.0);

        // Disable animations: the final state in the first frame, still one
        // rebuild and the reading position held.
        sounds->setProperty("reduceMotion", true);
        navigate("John 3:16"); settle();
        const auto instantEnter = stepped(mode + "-instant-enter", [&]() { key(Qt::Key_F); }, 2);
        const Frame &on = instantEnter[1];
        QCOMPARE(on.columnX, centred); QVERIFY(!on.notes && !on.outline);
        QVERIFY2(std::abs(on.cursorY - typewriter) <= 1, qPrintable(describe(on)));
        if (rebuilds > 0) QCOMPARE(instantEnter.last().rebuilds - instantEnter.first().rebuilds, 1);
        const auto instantLeave = stepped(mode + "-instant-leave", [&]() { key(Qt::Key_F); }, 2);
        const Frame &off = instantLeave[1];
        QCOMPARE(off.columnX, shifted); QVERIFY(off.notes && off.outline);
        QCOMPARE(off.notesOpacity, 1.0); QCOMPARE(off.outlineOpacity, 1.0); QCOMPARE(off.stale, 0.0);
        QVERIFY2(std::abs(off.cursorY - instantLeave.first().cursorY) <= 1, qPrintable(describe(off)));
        if (rebuilds > 0) QCOMPARE(instantLeave.last().rebuilds - instantLeave.first().rebuilds, 1);
        bible->setMarginNotes(false); bible->setChapterOutline(false);
    }
};
int main(int argc, char **argv) {
    QQuickStyle::setStyle("Basic"); QApplication app(argc, argv);
    MarginsUiTest test; return QTest::qExec(&test, argc, argv);
}
#include "tst_marginsui.moc"
