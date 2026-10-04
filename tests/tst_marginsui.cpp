#include <QtTest/QTest>
#include <QElapsedTimer>
#include <QApplication>
#include <QFile>
#include <QJSEngine>
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

    // John 1 (headings at 1, 10, 19, 29, 35), John 2 (none), John 3 (1, 16,
    // 22, 31), John 4 (a heading every three verses, for a tall outline).
    static bool zip(const QString &path, const QMap<QString, QByteArray> &entries) {
        auto archive = archive_write_new(); archive_write_set_format_zip(archive);
        if (archive_write_open_filename(archive, QFile::encodeName(path).constData()) != ARCHIVE_OK) { archive_write_free(archive); return false; }
        bool ok = true;
        for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
            auto entry = archive_entry_new(); const auto name = it.key().toUtf8();
            archive_entry_set_pathname(entry, name.constData()); archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_perm(entry, 0600); archive_entry_set_size(entry, it.value().size());
            ok = archive_write_header(archive, entry) == ARCHIVE_OK && archive_write_data(archive, it.value().constData(), it.value().size()) == it.value().size() && ok;
            archive_entry_free(entry);
        }
        ok = archive_write_close(archive) == ARCHIVE_OK && ok; archive_write_free(archive); return ok;
    }
    static QByteArray john(const QString &title, bool headings) {
        const QMap<int, QList<int>> sections = {{1, {1, 10, 19, 29, 35}}, {2, {}}, {3, {1, 16, 22, 31}},
            {4, {1, 4, 7, 10, 13, 16, 19, 22, 25, 28, 31, 34, 37, 40, 43, 46, 49, 52}}};
        const QMap<int, int> counts = {{1, 51}, {2, 25}, {3, 36}, {4, 54}};
        QByteArray data = QString("\\id JHN %1\n\\h John\n\\toc1 John\n\\toc2 John\n\\toc3 Jn\n").arg(title).toUtf8();
        for (int c = 1; c <= 4; ++c) {
            data += QString("\\c %1\n\\p\n").arg(c).toUtf8();
            for (int v = 1; v <= counts[c]; ++v) {
                if (headings && sections[c].contains(v)) data += QString("\\s Section %1.%2 of the margins fixture\n\\p\n").arg(c).arg(v).toUtf8();
                data += QString("\\v %1 Original margins text %2:%1 with enough ordinary words to wrap across one reading line.\n").arg(v).arg(c).toUtf8();
            }
        }
        return data;
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
        bible->importUsfm(QUrl::fromLocalFile(file)); QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(), 30000);
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
        // Medium: notes shift the column; the outline only uses what is left.
        auto medium = split(1200, true, true);
        QVERIFY(medium["notes"].toMap()["visible"].toBool()); QVERIFY(medium["shift"].toDouble() > 0);
        QVERIFY(!medium["outline"].toMap()["visible"].toBool());
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
        // A steady pen: along the straight runs (clear of the curls) the spine
        // drifts at most ~1px from its line.
        for (int seed = 1; seed < 20; ++seed) {
            double drift = 0;
            for (const auto &p : brace(seed, -1, 200)["points"].toList()) {
                const double x = p.toMap()["x"].toDouble(), y = p.toMap()["y"].toDouble();
                if ((y > 110 && y < 190) || (y > 210 && y < 290)) drift = qMax(drift, std::abs(x - 14.5));
            }
            QVERIFY2(drift <= 1.01, qPrintable(QString::number(drift)));
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
        key(Qt::Key_R, Qt::ControlModifier); QTRY_VERIFY(object("settingsDialog")->property("opened").toBool());
        QVERIFY(item("settingchapterOutline") && item("settingchapterOutline")->isVisible());
        QVERIFY(item("settingmarginNotes") && item("settingmarginNotes")->isVisible());
        key(Qt::Key_Escape); QTRY_VERIFY(!object("settingsDialog")->property("visible").toBool());
        QStringList commands;
        QVERIFY(QMetaObject::invokeMethod(window, "openPalette", Q_ARG(QVariant, true), Q_ARG(QVariant, false)));
        for (const auto &entry : js("paletteEntries").toList()) commands << entry.toMap()["title"].toString();
        QVERIFY2(commands.contains("Chapter outline") && commands.contains("Margin notes"), qPrintable(commands.join(", ")));
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
        }
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
            // A count past the first heading stops there, with a toast.
            type("5"); key(Qt::Key_BraceLeft);
            QTRY_COMPARE(bible->selectionReference(), QString("John 3:1"));
            QCOMPARE(toast(), QString("No more headings above"));
            // History goes back through the heading jumps.
            key(Qt::Key_BracketLeft); QTRY_COMPARE(bible->selectionReference(), QString("John 3:22"));
            // Inside a section, { goes to its own heading.
            navigate("John 3:20"); key(Qt::Key_BraceLeft); QTRY_COMPARE(bible->selectionReference(), QString("John 3:16"));
            // A chapter without headings.
            navigate("John 2:3"); jumps = jumpIndex();
            key(Qt::Key_BraceRight); QCOMPARE(toast(), QString("No headings in John 2")); QCOMPARE(jumpIndex(), jumps);
            QCOMPARE(bible->selectionReference(), QString("John 2:3"));
        }
        // A version without any headings.
        QVERIFY(bible->switchVersion(plainId)); QVERIFY(!bible->hasHeadings()); settle();
        navigate("John 3:2");
        key(Qt::Key_BraceRight); QCOMPARE(toast(), bible->versionAbbreviation() + " has no section headings");
        key(Qt::Key_BraceLeft); QCOMPARE(bible->selectionReference(), QString("John 3:2"));
    }

    // ---- Margin notes -------------------------------------------------------
    void marginNotesPlacementAllLayouts() {
        note("John 3:2", 2, 3, "Two verses with a note about the night visit.");
        note("John 3:16", 16, 16, "A single verse.");
        note("John 3:30", 30, 30, "Late in the chapter: see John 1:1 for the opening.");
        navigate("John 3:1");
        QVERIFY(!bible->marginNotes()); QVERIFY(!item("marginNote:John 3:16") || !item("marginNote:John 3:16")->isVisible());
        key(Qt::Key_W, Qt::AltModifier); QTRY_VERIFY(bible->marginNotes());
        QCOMPARE(toast(), QString("Margin notes on"));
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
        // No notes in this chapter: no shift, outline back.
        navigate("John 2:1"); QTRY_COMPARE(columnX(), natural);
        navigate("John 3:1"); QTRY_VERIFY(outlineShown() == false);
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
        // The drawer simply covers the margin notes: they stay laid out.
        auto drawer = object("notesDrawer");
        const QRectF noteRect(note->mapToScene(QPointF(0, 0)), QSizeF(note->width(), note->height()));
        QVERIFY(drawer->property("x").toDouble() < noteRect.right());
        QVERIFY(note->isVisible());
        QVERIFY(QMetaObject::invokeMethod(window, "closeNotes")); QTRY_VERIFY(!drawer->property("visible").toBool());
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
};
int main(int argc, char **argv) {
    QQuickStyle::setStyle("Basic"); QApplication app(argc, argv);
    MarginsUiTest test; return QTest::qExec(&test, argc, argv);
}
#include "tst_marginsui.moc"
