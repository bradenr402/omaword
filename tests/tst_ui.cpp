#include <QtTest/QTest>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QJSValue>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QMetaProperty>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QKeySequence>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QQuickTextDocument>
#include "blockdocument.h"
#include "biblebackend.h"
#include "desktop.h"
#include "soundeffects.h"
#include "usfmimporter.h"
#include "biblebooks.h"
#include "versioncatalog.h"
#include <cmath>
#include <archive.h>
#include <archive_entry.h>

// Run: QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software ./tst_ui
// Optional: OMAWORD_TEST_USFM_DIR contains engnet_usfm.zip and eng-kjv2006_usfm.zip.
// OMAWORD_TEST_KJV can override the KJV ZIP path. Otherwise original synthetic
// USFM exercises reader interactions without bundling Bible text or using a network.
// Current USFM validation requires paragraph_layout=1. Exercise the reader's
// no-layout capability branch with real backend rows and annotation operations.
class NoParagraphLayoutBackend : public BibleBackend
{
    Q_OBJECT
    Q_PROPERTY(bool hasParagraphs READ paragraphLayoutAvailable CONSTANT)
    Q_PROPERTY(QVariantList verses READ fallbackVerses NOTIFY versesChanged)
public:
    bool paragraphLayoutAvailable() const { return false; }
    QVariantList fallbackVerses() const {
        auto rows = verses();
        for (auto &value : rows) {
            auto row = value.toMap(); row["paragraphStart"] = false; value = row;
        }
        return rows;
    }
};

class MockVersionCatalog : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList entries MEMBER entries CONSTANT)
    Q_PROPERTY(bool busy MEMBER busy NOTIFY changed)
    Q_PROPERTY(double progress MEMBER progress NOTIFY changed)
    Q_PROPERTY(QString error MEMBER error NOTIFY changed)
public:
    QVariantList entries = VersionCatalog().entries();
    bool busy = false;
    double progress = 0;
    QString error;
    Q_INVOKABLE void download(const QString &id) { busy = true; progress = 0; error.clear(); emit changed(); emit requested(id); }
    Q_INVOKABLE void cancel() { busy = false; emit changed(); emit canceled(); }
    Q_INVOKABLE void clearError() { error.clear(); emit changed(); }
    void fail(const QString &message) { busy = false; error = message; emit changed(); }
    void finish(const QUrl &url) { busy = false; progress = 1; emit changed(); emit downloadFinished(url); }
signals:
    void changed();
    void requested(const QString &id);
    void canceled();
    void downloadFinished(const QUrl &url);
};

class UiTest : public QObject
{
    Q_OBJECT
    QTemporaryDir fixtures;
    std::unique_ptr<QTemporaryDir> session;
    std::unique_ptr<BibleBackend> bible;
    std::unique_ptr<Desktop> desktop;
    std::unique_ptr<SoundEffects> sounds;
    std::unique_ptr<MockVersionCatalog> catalog;
    std::unique_ptr<QQmlApplicationEngine> engine;
    QQuickWindow *window = nullptr;
    QStringList qmlWarnings;
    QString fixture;
    QString syntheticSource;
    UsfmImporter::Edition fixtureEdition;
    QString netSource() const {
        const auto directory = qEnvironmentVariable("OMAWORD_TEST_USFM_DIR");
        return directory.isEmpty() ? QString() : QDir(directory).filePath("engnet_usfm.zip");
    }
    QString kjvSource() const {
        const auto explicitPath = qEnvironmentVariable("OMAWORD_TEST_KJV");
        if (!explicitPath.isEmpty()) return explicitPath;
        const auto directory = qEnvironmentVariable("OMAWORD_TEST_USFM_DIR");
        return directory.isEmpty() ? QString() : QDir(directory).filePath("eng-kjv2006_usfm.zip");
    }
    QString installedPath() const {
        QString path;
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","ui-installed-path");
            db.setDatabaseName(bible->dataDirectory() + "/personal.sqlite");
            if (db.open()) {
                QSqlQuery q(db);
                q.prepare("SELECT path FROM installed WHERE version_id=?"); q.addBindValue(bible->versionId());
                if (q.exec() && q.next()) path = QDir(bible->dataDirectory()).filePath(q.value(0).toString());
            }
        }
        QSqlDatabase::removeDatabase("ui-installed-path");
        return path;
    }

    static QQuickItem *visualItem(QQuickItem *root, const QString &name) {
        if (root->objectName() == name) return root;
        for (auto child : root->childItems())
            if (auto found = visualItem(child,name)) return found;
        return nullptr;
    }
    QObject *object(const char *name) const {
        if (!window) return nullptr;
        const auto found = window->findChild<QObject *>(QString::fromUtf8(name));
        return found ? found : visualItem(window->contentItem(),QString::fromUtf8(name));
    }
    QQuickItem *item(const char *name) const { return qobject_cast<QQuickItem *>(object(name)); }
    // Hint rows and key lists are rich text (KeyNotation.js); compare their plain reading.
    static QString plainHint(const QString &rich) {
        QTextDocument document; document.setHtml(rich);
        return document.toPlainText().replace(QChar(0x00a0), QChar(' '));
    }
    QString formatKeys(const QStringList &sequences) const {
        QVariant result;
        QMetaObject::invokeMethod(window,"formatKeys",Q_RETURN_ARG(QVariant,result),Q_ARG(QVariant,QVariant(sequences)));
        return result.toString();
    }
    QVariant prop(const char *name, const char *property) const {
        const auto obj = object(name);
        return obj ? obj->property(property) : QVariant();
    }
    void key(Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QTest::keyClick(window, key, modifiers);
        QCoreApplication::processEvents();
    }
    void type(const QString &text) {
        // QWindow has keyClick, but no keyClicks overload. Let Qt translate each
        // Latin-1 key as it would a real keyboard, including shifted punctuation.
        for (const QChar c : text) {
            if (c == '\n') key(Qt::Key_Return);
            else QTest::keyClick(window, c.toLatin1());
        }
        QCoreApplication::processEvents();
    }
    void click(QQuickItem *target, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QVERIFY(target);
        QVERIFY(target->isVisible());
        const QPoint point = target->mapToScene(QPointF(target->width() / 2, qMin(target->height() / 2, 16.0))).toPoint();
        QVERIFY2(QRect(QPoint(0, 0), window->size()).contains(point), qPrintable(QString("Click outside window: %1,%2").arg(point.x()).arg(point.y())));
        QTest::mouseClick(window, Qt::LeftButton, modifiers, point);
        QCoreApplication::processEvents();
    }
    QString bodyText(int verse) const { return rawBodyText(verse).remove(QChar(0xfffc)).remove(QChar(0x2060)); }
    // Rendered verse-row text in document positions, footnote cells included.
    QString rawBodyText(int verse) const {
        auto body = object(qPrintable("verseBody" + QString::number(verse)));
        QString text;
        if (body) QMetaObject::invokeMethod(body,"getText",Q_RETURN_ARG(QString,text),
            Q_ARG(int,0),Q_ARG(int,body->property("length").toInt()));
        return text;
    }
    QPoint textPoint(int verse, int position) const {
        auto body = item(qPrintable("verseBody" + QString::number(verse)));
        QRectF rect;
        if (!body || !QMetaObject::invokeMethod(body,"positionToRectangle",Q_RETURN_ARG(QRectF,rect),Q_ARG(int,position))) return {};
        return body->mapToScene(QPointF(rect.x(),rect.y() + rect.height()/2)).toPoint();
    }
    BlockDocument *blockMapping() const { return window->findChild<BlockDocument *>(); }
    QTextDocument *blockTextDocument() const {
        auto quick = prop("blockBody","textDocument").value<QQuickTextDocument *>();
        return quick ? quick->textDocument() : nullptr;
    }
    int blockOffset(int verse, int offset = 0) const {
        for (const auto &value : blockMapping()->spans()) {
            const auto span = value.toMap();
            if (span.value("number").toInt() == verse) return span.value("textStart").toInt() + offset;
        }
        return -1;
    }
    QRectF blockRect(int position) const {
        QRectF rect;
        QMetaObject::invokeMethod(object("blockBody"),"positionToRectangle",Q_RETURN_ARG(QRectF,rect),Q_ARG(int,position));
        return rect;
    }
    QPoint blockPoint(int verse, int offset = 0) const {
        const auto rect = blockRect(blockOffset(verse,offset));
        return item("blockBody")->mapToScene(QPointF(rect.x(),rect.center().y())).toPoint();
    }
    void drag(const QPoint &from, const QPoint &to) {
        QVERIFY(QRect(QPoint(),window->size()).contains(from));
        QVERIFY(QRect(QPoint(),window->size()).contains(to));
        QTest::mousePress(window,Qt::LeftButton,Qt::NoModifier,from);
        for (int step = 1; step <= 12; ++step)
            QTest::mouseMove(window,from + (to - from) * step / 12,10);
        QTest::mouseRelease(window,Qt::LeftButton,Qt::NoModifier,to);
        QCoreApplication::processEvents();
    }
    void mouseCopy(int verse, Qt::MouseButton button) {
        const auto point = textPoint(verse,4);
        QVERIFY2(QRect(QPoint(),window->size()).contains(point),qPrintable(QString("Copy outside window: %1,%2").arg(point.x()).arg(point.y())));
        QTest::mouseClick(window,button,Qt::NoModifier,point);
        QCoreApplication::processEvents();
    }
    void installDragFixture() {
        // Distinct rich paragraphs exercise document offsets, Unicode, poetry,
        // and the join between delegates independently of the source USFM.
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","ui-drag-fixture");
            db.setDatabaseName(installedPath());
            QVERIFY(db.open());
            QSqlQuery query(db);
            query.prepare("UPDATE verses SET text=?,html=?,poetry=?,heading=? WHERE book=43 AND chapter=3 AND number=?");
            const QStringList texts = {QString::fromUtf8("Start café — λόγος 🌿 and finish."),
                QString::fromUtf8("Poetry sings softly.\nA second line — שלום."),
                QString::fromUtf8("Final naïve résumé ends here.")};
            for (int i = 0; i < texts.size(); ++i) {
                query.bindValue(0,texts[i]);
                query.bindValue(1,QString("<span%1>%2</span>").arg(i == 1 ? " style=\"font-style:italic\"" : "",texts[i].toHtmlEscaped().replace('\n',"<br>")));
                query.bindValue(2,i == 1);
                query.bindValue(3,i == 0 ? "A section title" : "");
                query.bindValue(4,i+1);
                QVERIFY(query.exec());
            }
        }
        QSqlDatabase::removeDatabase("ui-drag-fixture");
        reference("John 3:1");
    }
    void reference(const QString &text) {
        // Ctrl keys belong to the note editor while typing: leave it first.
        if (item("noteEditor") && item("noteEditor")->hasActiveFocus()) item("notesPanel")->forceActiveFocus();
        key(Qt::Key_K, Qt::ControlModifier);
        QTRY_VERIFY(prop("passagePalette", "opened").toBool());
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type(text);
        QCOMPARE(prop("referenceInput", "text").toString(), text);
        key(Qt::Key_Return);
        QTRY_VERIFY(!prop("passagePalette", "visible").toBool());
        QTRY_VERIFY(!window->property("restoringPosition").toBool());
        // Same-chapter jumps slide; let them land.
        QTRY_VERIFY(!object("readerSlide")->property("running").toBool());
    }
    void capture(const QString &name) {
        const QString directory = qEnvironmentVariable("OMAWORD_UI_SCREENSHOTS");
        if (directory.isEmpty()) return;
        QVERIFY(QDir(directory).exists());
        QSignalSpy frames(window,&QQuickWindow::frameSwapped);
        window->update();
        QTRY_VERIFY(frames.count() > 0);
        QTest::qWait(150);
        QVERIFY(window->grabWindow().save(QDir(directory).filePath(name + ".png")));
    }
    bool writeSyntheticUsfm(const QString &path, const QString &edition = "engnet", bool sourceOrder = false, bool collidingNames = false) {
        auto archive = archive_write_new();
        archive_write_set_format_zip(archive);
        if (archive_write_open_filename(archive,qPrintable(path)) != ARCHIVE_OK) { archive_write_free(archive); return false; }
        bool ok = true;
        const auto &books = BibleBooks::canon();
        QVector<int> order;
        if (sourceOrder) order = {69,40,43};
        else if (qEnvironmentVariableIsSet("OMAWORD_UI_FAST_FIXTURE")) order = {43};
        else for (int i = 0; i < books.size(); ++i) {
            order.append(i + 1);
            if (QString::fromLatin1(books[i].code) == "REV") break;
        }
        int ordinal = 0;
        for (int id : order) {
            const auto &book = books[id - 1];
            // Only interaction-relevant boundaries are specialized. The remaining
            // original chapters deliberately do not reproduce a numbering profile.
            const int chapters = qEnvironmentVariableIsSet("OMAWORD_UI_FAST_FIXTURE") && !sourceOrder ? 4 : id == 19 ? 150 : id == 40 ? 28 : id == 43 ? 21
                : id == 46 ? 13 : id == 52 ? 5 : id == 53 ? 3 : id == 69 ? 16
                : QString::fromLatin1(book.code) == "REV" ? 22 : 4;
            QByteArray text = QString("\\id %1 Original reader test edition\n\\h %2\n\\toc1 %2\n\\toc2 %2\n\\toc3 %1\n")
                .arg(book.code,sourceOrder && id == 43 ? "Yochanan" : book.name).toUtf8();
            if (collidingNames && id == 1) text += "\\toc2 John\n";
            for (int chapter = id == 69 ? 10 : 1; chapter <= chapters; ++chapter) {
                const int verses = id == 19 && chapter == 119 ? 176 : id == 40 && chapter == 17 ? 27
                    : id == 43 && chapter == 2 ? 25 : id == 43 && chapter == 3 ? 36 : id == 43 && chapter == 4 ? 54
                    : id == 43 && chapter == 21 ? 25 : id == 53 && chapter == 3 ? 18
                    : QString::fromLatin1(book.code) == "REV" && chapter == 22 ? 21 : 40;
                text += QString("\\c %1\n\\s Reader integration fixture\n").arg(chapter).toUtf8();
                for (int verse = 1; verse <= verses; ++verse) {
                    if (edition == "engnet" && id == 40 && chapter == 17 && verse == 21) continue;
                    text += QString("\\v %1 Synthetic reader fixture %2:%1. This is original test text for checking comfortable wrapping, verse selection, and annotation workflows. A second sentence gives the reading surface enough height to exercise scrolling and restoration.\n").arg(verse).arg(chapter).toUtf8();
                }
            }
            auto entry = archive_entry_new();
            const auto name = QString("%1-%2%3.usfm").arg(++ordinal,3,10,QChar('0')).arg(book.code,edition).toUtf8();
            archive_entry_set_pathname(entry,name.constData());
            archive_entry_set_filetype(entry,AE_IFREG);
            archive_entry_set_perm(entry,0600);
            archive_entry_set_size(entry,text.size());
            ok = archive_write_header(archive,entry) == ARCHIVE_OK && archive_write_data(archive,text.constData(),text.size()) == text.size() && ok;
            archive_entry_free(entry);
        }
        ok = archive_write_close(archive) == ARCHIVE_OK && ok;
        archive_write_free(archive);
        return ok;
    }
    void loadUi(bool verseFixture = false) {
        if (qstrcmp(QTest::currentTestFunction(),"paragraphFallbackRangeContext") == 0)
            bible = std::make_unique<NoParagraphLayoutBackend>();
        else bible = std::make_unique<BibleBackend>();
        // Existing interaction fixtures exercise verse rows explicitly. New
        // preference/default tests opt into the backend's initial paragraph mode.
        const QString testName = QString::fromLatin1(QTest::currentTestFunction());
        if (verseFixture && !testName.startsWith("defaultParagraph") && testName != "fontControlsBeforeImport")
            bible->setReadingMode("verse");
        desktop = std::make_unique<Desktop>();
        sounds = std::make_unique<SoundEffects>(nullptr, fixtures.filePath("sound.ini"), false);
        catalog = std::make_unique<MockVersionCatalog>();
        engine = std::make_unique<QQmlApplicationEngine>();
        connect(engine.get(), &QQmlEngine::warnings, this, [this](const QList<QQmlError> &errors) {
            for (const auto &error : errors) qmlWarnings.append(error.toString());
        });
        engine->rootContext()->setContextProperty("bible", bible.get());
        engine->rootContext()->setContextProperty("desktop", desktop.get());
        engine->rootContext()->setContextProperty("sounds", sounds.get());
        engine->rootContext()->setContextProperty("catalog", catalog.get());
        // Exercise the same bundled QML and standalone panels as the application.
        engine->load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY2(!engine->rootObjects().isEmpty(), qPrintable(qmlWarnings.join('\n')));
        window = qobject_cast<QQuickWindow *>(engine->rootObjects().first());
        QVERIFY(window);
        window->show();
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QTRY_VERIFY(window->isActive());
        QTRY_VERIFY(!window->property("restoringPosition").toBool());
        QVERIFY(item("reading"));
    }
    void unloadUi() {
        if (window) window->close();
        engine.reset(); window = nullptr;
        catalog.reset(); sounds.reset(); desktop.reset(); bible.reset();
    }
    QVariantMap saved(int verse) const {
        for (const auto &row : bible->collection()) {
            const auto map = row.toMap();
            if (map.value("book").toInt() == 43 && map.value("chapter").toInt() == 3
                && map.value("start").toInt() == verse && map.value("end").toInt() == verse) return map;
        }
        return {};
    }
private slots:
    void initTestCase() {
        QVERIFY(fixtures.isValid());
        fixture = fixtures.filePath("bible.sqlite");
        syntheticSource = fixtures.filePath("original_usfm.zip");
        QVERIFY(writeSyntheticUsfm(syntheticSource));
        const auto imported = UsfmImporter::importFile(netSource().isEmpty() ? syntheticSource : netSource(), fixture);
        QVERIFY2(imported.ok, qPrintable(imported.error));
        fixtureEdition = imported.edition;
    }
    void init() {
        session = std::make_unique<QTemporaryDir>();
        QVERIFY(session->isValid());
        qputenv("XDG_DATA_HOME", session->filePath("data").toUtf8());
        const auto data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QVERIFY(QDir().mkpath(data));
        const QString testName = QString::fromLatin1(QTest::currentTestFunction());
        if (testName != "emptyAndImportError" && testName != "asyncFirstImportLayout" && testName != "fontControlsBeforeImport"
                && testName != "catalogDownloadLifecycle") {
            BibleBackend setup;
            const QString filename = "bible-aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee.sqlite";
            QVERIFY(QFile::copy(fixture, data + '/' + filename));
            {
                auto db = QSqlDatabase::addDatabase("QSQLITE","ui-setup");
                db.setDatabaseName(data + "/personal.sqlite"); QVERIFY(db.open()); QSqlQuery q(db);
                q.prepare("INSERT INTO installed(version_id,path,abbreviation,name,copyright) VALUES('engnet',?,'NET',?,?)");
                q.addBindValue(filename); q.addBindValue(fixtureEdition.name);
                q.addBindValue(fixtureEdition.copyright.isNull() ? QString("") : fixtureEdition.copyright); QVERIFY(q.exec());
                QVERIFY(q.exec("UPDATE library SET active='engnet' WHERE id=1"));
            }
            QSqlDatabase::removeDatabase("ui-setup");
        }
        qmlWarnings.clear();
        loadUi(true);
        if (testName != "emptyAndImportError" && testName != "asyncFirstImportLayout" && testName != "fontControlsBeforeImport"
                && testName != "catalogDownloadLifecycle") QVERIFY2(bible->ready(),qPrintable(bible->error()));
    }
    void cleanup() {
        unloadUi();
        QVERIFY2(qmlWarnings.isEmpty(), qPrintable(qmlWarnings.join('\n')));
        session.reset();
    }
    void fontControlsBeforeImport() {
        QVERIFY(!bible->ready());
        QCOMPARE(bible->readingMode(),QString("paragraph"));
        QCOMPARE(bible->fontSize(),18);
        QCOMPARE(window->property("readerSize").toInt(),18);
        key(Qt::Key_Space,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("reading settings"); key(Qt::Key_Return);
        QTRY_VERIFY(prop("settingsDialog","opened").toBool());
        QTRY_VERIFY(item("lineHeightSlider")->hasActiveFocus());
        QCOMPARE(prop("fontSizeValue","text").toString(),QString("18"));
        QCOMPARE(prop("fontSizeSlider","from").toInt(),8);
        QCOMPARE(prop("fontSizeSlider","to").toInt(),40);
        QCOMPARE(prop("fontSizeSlider","stepSize").toInt(),1);
        for (const auto name : {"lineHeightSlider","fontSizeSlider","settingnumbers","readingMode:paragraph"}) {
            item(name)->forceActiveFocus();
            for (int i = 0; i < 4; ++i) { key(Qt::Key_Left); key(Qt::Key_Right); }
            QCOMPARE(bible->lineHeight(),1.5);
            QCOMPARE(bible->fontSize(),18);
        }
        for (int i = 0; i < 16; ++i) {
            key(Qt::Key_Tab); key(Qt::Key_Left); key(Qt::Key_Right);
            QCOMPARE(bible->lineHeight(),1.5);
            QCOMPARE(bible->fontSize(),18);
        }
        item("fontSizeSlider")->forceActiveFocus();
        key(Qt::Key_Up); QCOMPARE(bible->fontSize(),19);
        QCOMPARE(prop("fontSizeValue","text").toString(),QString("19"));
        QCOMPARE(window->property("readerSize").toInt(),19);
        key(Qt::Key_Equal,Qt::ControlModifier); QCOMPARE(bible->fontSize(),20);
        key(Qt::Key_Minus,Qt::ControlModifier); QCOMPARE(bible->fontSize(),19);
        key(Qt::Key_0,Qt::ControlModifier); QCOMPARE(bible->fontSize(),18);
        key(Qt::Key_Equal,Qt::ControlModifier | Qt::AltModifier); QCOMPARE(bible->lineHeight(),1.6);
        key(Qt::Key_0,Qt::ControlModifier | Qt::AltModifier); QCOMPARE(bible->lineHeight(),1.5);
        key(Qt::Key_L,Qt::AltModifier); QCOMPARE(bible->readingMode(),QString("verse"));
        key(Qt::Key_L,Qt::AltModifier); QCOMPARE(bible->readingMode(),QString("block"));
        key(Qt::Key_L,Qt::AltModifier); QCOMPARE(bible->readingMode(),QString("paragraph"));
        bible->setFontSize(27); // UI binding must survive keyboard and slider actions.
        QCOMPARE(window->property("readerSize").toInt(),27);
        QCOMPARE(prop("fontSizeSlider","value").toInt(),27);
        QCOMPARE(prop("fontSizeValue","text").toString(),QString("27"));
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("settingsDialog","visible").toBool());
        unloadUi(); loadUi();
        QCOMPARE(bible->fontSize(),27);
        QCOMPARE(window->property("readerSize").toInt(),27);
        QCOMPARE(bible->readingMode(),QString("paragraph"));
    }
    void catalogSearchAndAttribution() {
        QSignalSpy downloads(catalog.get(),&MockVersionCatalog::requested);
        QVERIFY(QMetaObject::invokeMethod(window,"runCommand",Q_ARG(QVariant,"import")));
        QTRY_VERIFY(item("catalogSearch")->hasActiveFocus());
        QCOMPARE(prop("catalogResults","count").toInt(),7);
        QVERIFY(item("catalogInstalled:engnet")->isVisible());
        QVERIFY(!item("catalogInstalled:engojb")->isVisible());
        type("oRtHoDoX");
        QCOMPARE(prop("catalogResults","count").toInt(),1);
        QTest::qWait(100);
        click(item("catalogDetails:engojb"));
        QTRY_VERIFY(prop("catalogDetails","opened").toBool());
        QString copyright;
        for (const auto &entry : catalog->entries)
            if (entry.toMap().value("id") == "engojb") copyright = entry.toMap().value("copyright").toString();
        QVERIFY(!copyright.isEmpty());
        QCOMPARE(prop("catalogCopyright","text").toString(),copyright);
        QVERIFY(item("catalogProvider")->isVisible());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("catalogDetails","visible").toBool());
        QTRY_VERIFY(item("catalogSearch")->hasActiveFocus());
        key(Qt::Key_A,Qt::ControlModifier); type("no matching version");
        QCOMPARE(prop("catalogResults","count").toInt(),0);
        QCOMPARE(downloads.count(),0);
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("importGuide","visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(window,"runCommand",Q_ARG(QVariant,"about")));
        QTRY_VERIFY(prop("aboutDialog","opened").toBool());
        QVERIFY(prop("aboutSources","text").toString().contains("directly from eBible.org"));
        QVERIFY(prop("aboutSources","text").toString().contains("No connection is needed"));
    }
    void catalogDownloadLifecycle() {
        QVERIFY(!bible->ready());
        QSignalSpy downloads(catalog.get(),&MockVersionCatalog::requested);
        QSignalSpy canceled(catalog.get(),&MockVersionCatalog::canceled);
        QSignalSpy commits(bible.get(),&BibleBackend::sharedCommitted);
        click(item("setupImport"));
        QTRY_VERIFY(prop("importGuide","opened").toBool());
        QTest::qWait(100);
        click(item("catalogInstall:engnet"));
        QCOMPARE(downloads.count(),1); QCOMPARE(downloads.first().first().toString(),QString("engnet"));
        QVERIFY(!bible->busy()); QVERIFY(!bible->ready());
        QVERIFY(!item("importChoose")->isEnabled());
        QVERIFY(!item("catalogInstall:engojb")->isEnabled());
        QVERIFY(!item("catalogInstalled:engnet")->isVisible());
        catalog->progress = 0.42; emit catalog->changed();
        QCOMPARE(prop("catalogProgress","value").toDouble(),0.42);
        QVERIFY(prop("catalogStatus","text").toString().contains("42%"));
        QVERIFY(!prop("toast","text").toString().contains("Installed"));
        catalog->fail("Synthetic connection failure");
        QCOMPARE(prop("catalogError","text").toString(),QString("Synthetic connection failure"));
        QVERIFY(QMetaObject::invokeMethod(object("catalogRetry"),"clicked"));
        QCOMPARE(downloads.count(),2);
        QVERIFY(QMetaObject::invokeMethod(object("catalogCancel"),"clicked"));
        QCOMPARE(canceled.count(),1); QVERIFY(!catalog->busy);
        catalog->finish(QUrl::fromLocalFile(syntheticSource));
        QVERIFY(!bible->busy()); QVERIFY(!bible->ready()); QCOMPARE(commits.count(),0);
        const auto invalidSource = session->filePath("invalid-catalog.zip");
        QFile invalid(invalidSource); QVERIFY(invalid.open(QIODevice::WriteOnly));
        invalid.write("Invalid USFM ZIP"); invalid.close();
        QVERIFY(QMetaObject::invokeMethod(window,"installVersion",Q_ARG(QVariant,"engnet")));
        catalog->finish(QUrl::fromLocalFile(invalidSource));
        QTRY_VERIFY(!bible->busy());
        QVERIFY(!bible->ready()); QVERIFY(!bible->error().isEmpty());
        QVERIFY(!item("catalogInstalled:engnet")->isVisible());
        QVERIFY(!prop("toast","text").toString().contains("Installed"));
        QVERIFY(QMetaObject::invokeMethod(window,"installVersion",Q_ARG(QVariant,"engnet")));
        catalog->finish(QUrl::fromLocalFile(syntheticSource));
        QVERIFY(bible->busy());
        QVERIFY(!prop("toast","text").toString().contains("Installed"));
        QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(),120000);
        QVERIFY2(bible->ready(),qPrintable(bible->error()));
        QCOMPARE(bible->versionId(),QString("engnet"));
        QCOMPARE(prop("toast","text").toString(),QString("Installed NET"));
        const auto commitCount = commits.count();
        catalog->finish(QUrl::fromLocalFile(syntheticSource));
        QVERIFY(!bible->busy()); QCOMPARE(commits.count(),commitCount);
        QVERIFY(QFile::exists(syntheticSource));
        QVERIFY(QMetaObject::invokeMethod(window,"installVersion",Q_ARG(QVariant,"engnet")));
        catalog->finish(QUrl::fromLocalFile(syntheticSource));
        QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(),120000);
        QCOMPARE(bible->versions().size(),1);
        QCOMPARE(prop("toast","text").toString(),QString("Reimported NET"));
        QVERIFY(QMetaObject::invokeMethod(window,"installVersion",Q_ARG(QVariant,"engojb")));
        const auto requestsBeforeClose = downloads.count();
        QVERIFY(window->close());
        QVERIFY(!catalog->busy);
        const auto afterClose = commits.count();
        catalog->finish(QUrl::fromLocalFile(syntheticSource));
        QTest::qWait(100);
        QCOMPARE(downloads.count(),requestsBeforeClose);
        QCOMPARE(commits.count(),afterClose);
        QVERIFY(!bible->busy()); QVERIFY(QFile::exists(syntheticSource));
    }
    void catalogRetryAfterPeerDraftRejection() {
        bool peerDraftSaved = false;
        int flushAttempts = 0;
        BibleBackend peer;
        peer.setFlushCallback([&] { ++flushAttempts; return peerDraftSaved; });
        QSignalSpy downloads(catalog.get(),&MockVersionCatalog::requested);
        QSignalSpy busy(bible.get(),&BibleBackend::busyChanged);
        QSignalSpy commits(bible.get(),&BibleBackend::sharedCommitted);
        const auto cached = QUrl::fromLocalFile(syntheticSource);
        const auto previousToast = prop("toast","text");
        QVERIFY(QMetaObject::invokeMethod(window,"runCommand",Q_ARG(QVariant,"import")));
        QTRY_VERIFY(prop("importGuide","opened").toBool());
        QVERIFY(QMetaObject::invokeMethod(window,"installVersion",Q_ARG(QVariant,"engnet")));
        QCOMPARE(downloads.count(),1);
        catalog->finish(cached);
        QVERIFY(flushAttempts > 0);
        QVERIFY(!bible->busy()); QCOMPARE(busy.count(),0); QCOMPARE(commits.count(),0);
        QCOMPARE(window->property("pendingDownload").toUrl(),cached);
        QCOMPARE(window->property("downloadId").toString(),QString("engnet"));
        QVERIFY(window->property("importingCatalogId").toString().isEmpty());
        QVERIFY(!window->property("replacingCatalogVersion").toBool());
        QVERIFY(!bible->error().isEmpty());
        QCOMPARE(prop("catalogError","text").toString(),bible->error());
        QVERIFY(item("catalogRetry")->isVisible()); QVERIFY(item("catalogRetry")->isEnabled());
        QCOMPARE(prop("toast","text"),previousToast);
        const int rejectedAttempts = flushAttempts;
        QVERIFY(QMetaObject::invokeMethod(object("catalogRetry"),"clicked"));
        QVERIFY(flushAttempts > rejectedAttempts);
        QCOMPARE(downloads.count(),1); QCOMPARE(busy.count(),0); QCOMPARE(commits.count(),0);
        QCOMPARE(window->property("pendingDownload").toUrl(),cached);
        QCOMPARE(window->property("downloadId").toString(),QString("engnet"));
        QVERIFY(item("catalogRetry")->isVisible());
        QCOMPARE(prop("toast","text"),previousToast);
        QVERIFY(QFile::exists(cached.toLocalFile()));

        peerDraftSaved = true;
        QVERIFY(QMetaObject::invokeMethod(object("catalogRetry"),"clicked"));
        QVERIFY(bible->busy());
        QCOMPARE(window->property("importingCatalogId").toString(),QString("engnet"));
        QVERIFY(window->property("pendingDownload").toUrl().isEmpty());
        QVERIFY(window->property("downloadId").toString().isEmpty());
        QCOMPARE(prop("toast","text"),previousToast);
        QCOMPARE(downloads.count(),1);
        QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(),120000);
        QVERIFY2(bible->ready(),qPrintable(bible->error()));
        QVERIFY2(bible->error().isEmpty(),qPrintable(bible->error()));
        QCOMPARE(bible->versionId(),QString("engnet"));
        QCOMPARE(bible->versions().size(),1);
        QCOMPARE(prop("toast","text").toString(),QString("Reimported NET"));
        QVERIFY(window->property("importingCatalogId").toString().isEmpty());
        QVERIFY(commits.count() > 0); QCOMPARE(downloads.count(),1);
        QVERIFY(QFile::exists(cached.toLocalFile()));
    }
    void catalogCompletionGuardsDraftAndSharedImport() {
        reference("John 3:16");
        QVERIFY(QMetaObject::invokeMethod(window,"installVersion",Q_ARG(QVariant,"engnet")));
        QSignalSpy downloads(catalog.get(),&MockVersionCatalog::requested);
        QVERIFY(window->setProperty("syncingNote",true));
        QVERIFY(object("noteEditor")->setProperty("text","Unresolved download draft"));
        QVERIFY(window->setProperty("syncingNote",false));
        BibleBackend peer;
        QVERIFY(peer.navigate("John 3:16"));
        peer.setNoteText("Peer revision");
        QVERIFY(window->property("noteConflict").toBool());
        catalog->finish(QUrl::fromLocalFile(syntheticSource));
        QVERIFY(!bible->busy());
        QCOMPARE(prop("noteEditor","text").toString(),QString("Unresolved download draft"));
        QVERIFY(!window->property("pendingDownload").toUrl().isEmpty());
        QVERIFY(QMetaObject::invokeMethod(window,"loadSavedNote"));
        peer.importUsfm(QUrl::fromLocalFile(syntheticSource));
        QVERIFY(peer.busy()); QVERIFY(bible->busy());
        QVERIFY(!item("versionControl")->isEnabled());
        QVERIFY(QMetaObject::invokeMethod(window,"importDownloaded"));
        QVERIFY(!window->property("pendingDownload").toUrl().isEmpty());
        QVERIFY(QMetaObject::invokeMethod(window,"installVersion",Q_ARG(QVariant,"engojb")));
        QCOMPARE(downloads.count(),0);
        QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(),120000);
        QVERIFY(QMetaObject::invokeMethod(window,"importDownloaded"));
        QVERIFY(bible->busy());
        QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(),120000);
        QVERIFY2(bible->error().isEmpty(),qPrintable(bible->error()));
        QCOMPARE(downloads.count(),0);
        QCOMPARE(bible->noteText(),QString("Peer revision"));
        QVERIFY(QFile::exists(syntheticSource));
    }
    void sourceOrderedChaptersAndNames() {
        const auto source = session->filePath("source-order.zip");
        QVERIFY(writeSyntheticUsfm(source,"engojb",true));
        bible->importUsfm(QUrl::fromLocalFile(source));
        QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(),120000);
        QVERIFY2(bible->error().isEmpty(),qPrintable(bible->error()));
        QCOMPARE(bible->books().first().toMap().value("id").toInt(),69);
        bible->openChapter(69,10);
        QTRY_VERIFY(!window->property("restoringPosition").toBool());
        key(Qt::Key_K,Qt::ControlModifier); QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        const auto book = bible->books().first().toMap();
        const QVariant entry = QVariantMap{{"book",69},{"title",book.value("name")}};
        QVERIFY(QMetaObject::invokeMethod(window,"activatePaletteEntry",Q_ARG(QVariant,entry)));
        QCOMPARE(prop("paletteResults","count").toInt(),7);
        key(Qt::Key_Right);
        QCOMPARE(window->property("paletteChapter").toInt(),10);
        key(Qt::Key_Left);
        QCOMPARE(prop("paletteResults","currentIndex").toInt(),0);
        for (int i = 0; i < 6; ++i) key(Qt::Key_Down);
        key(Qt::Key_Right);
        QCOMPARE(window->property("paletteChapter").toInt(),16);
        key(Qt::Key_Left);
        QCOMPARE(prop("paletteResults","currentIndex").toInt(),6);
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        bible->openChapter(69,16);
        QVERIFY(QMetaObject::invokeMethod(window,"chapterStep",Q_ARG(QVariant,1)));
        QCOMPARE(bible->book(),40); QCOMPARE(bible->chapter(),1);
        QVERIFY(QMetaObject::invokeMethod(window,"chapterStep",Q_ARG(QVariant,-2)));
        QCOMPARE(bible->book(),69); QCOMPARE(bible->chapter(),15);
        bible->openChapter(69,16); bible->selectVerses(40,40);
        QVERIFY(QMetaObject::invokeMethod(window,"moveVerse",Q_ARG(QVariant,1),Q_ARG(QVariant,true),Q_ARG(QVariant,false)));
        QVERIFY(bible->crossChapterSelection());
        QCOMPARE(bible->book(),40); QCOMPARE(bible->cursorVerse(),1);
        const auto range = bible->selectionReference();
        QVERIFY(range.contains(book.value("name").toString()));
        QVERIFY(range.contains("Matthew 1:1"));
        reference("Yochanan 3:16");
        QCOMPARE(bible->book(),43);
        QVERIFY(bible->selectionReference().contains("Yochanan"));
        QVERIFY(prop("passageControl","tip").toString().contains("Yochanan"));
    }
    void defaultParagraphCycleAndFallback() {
        QCOMPARE(bible->readingMode(),QString("paragraph"));
        QCOMPARE(window->property("continuousReader").toBool(),bible->hasParagraphs());
        if (bible->hasParagraphs()) QTRY_VERIFY(blockTextDocument());
        else QTRY_VERIFY(item("verseBody1"));
        key(Qt::Key_L,Qt::AltModifier); QCOMPARE(bible->readingMode(),QString("verse"));
        QTRY_VERIFY(item("verseBody1"));
        key(Qt::Key_L,Qt::AltModifier); QCOMPARE(bible->readingMode(),QString("block"));
        QTRY_VERIFY(blockTextDocument());
        key(Qt::Key_L,Qt::AltModifier); QCOMPARE(bible->readingMode(),QString("paragraph"));
        key(Qt::Key_Space,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("reading mode");
        QTRY_COMPARE(prop("paletteHintreadingMode","text").toString(),QString("Paragraph · alt l"));
        key(Qt::Key_Return); QTRY_COMPARE(bible->readingMode(),QString("verse"));
        key(Qt::Key_F1); QTRY_VERIFY(item("keybindingSearch")->hasActiveFocus());
        type("reading mode");
        QTRY_VERIFY(item("keybindingReading mode"));
        key(Qt::Key_Return); QTRY_COMPARE(bible->readingMode(),QString("block"));
        key(Qt::Key_R,Qt::ControlModifier); QTRY_VERIFY(prop("settingsDialog","opened").toBool());
        QTest::qWait(100);
        click(item("readingMode:paragraph"));
        QCOMPARE(bible->readingMode(),QString("paragraph"));
        QVERIFY(prop("readingMode:paragraph","checked").toBool());
        QVERIFY(!prop("readingMode:block","checked").toBool());
        click(item("readingMode:paragraph"));
        QVERIFY(prop("readingMode:paragraph","checked").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("settingsDialog","visible").toBool());
        unloadUi(); loadUi();
        QCOMPARE(bible->readingMode(),QString("paragraph"));
        QCOMPARE(window->property("continuousReader").toBool(),bible->hasParagraphs());
    }
    void defaultParagraphKjvFlowAndCopy() {
        const auto kjv = kjvSource();
        if (kjv.isEmpty()) QSKIP("Set OMAWORD_TEST_USFM_DIR or OMAWORD_TEST_KJV to exercise KJV USFM.");
        bible->importUsfm(QUrl::fromLocalFile(kjv));
        QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(),120000);
        QCOMPARE(bible->versionId(),QString("eng-kjv2006"));
        QVERIFY(bible->hasParagraphs());
        QCOMPARE(bible->readingMode(),QString("paragraph"));
        reference("Genesis 1:1");
        QTRY_VERIFY(blockMapping() && blockMapping()->spans().size() == bible->verses().size());
        auto doc = blockTextDocument();
        QVERIFY(doc);
        QVERIFY(!doc->toPlainText().contains(QChar(0x00b6)));
        for (auto block = doc->begin(); block.isValid(); block = block.next()) QVERIFY(!block.text().isEmpty());
        bible->setFontSize(12);
        bible->setShowVerseNumbers(false);
        QTRY_COMPARE(blockMapping()->spans().first().toMap().value("textStart"),blockMapping()->spans().first().toMap().value("start"));
        drag(blockPoint(1,3),blockPoint(2,12));
        const auto selected = window->property("freeSelectedText").toString();
        QVERIFY(!selected.isEmpty());
        key(Qt::Key_C,Qt::ControlModifier);
        QCOMPARE(QGuiApplication::clipboard()->text(),selected);
        QTest::mouseClick(window,Qt::MiddleButton,Qt::NoModifier,blockPoint(2,8));
        QVERIFY(QGuiApplication::clipboard()->text().contains("Genesis 1:2"));
        reference("Genesis 1:1");
        key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        type("Paragraph note");
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("notesDrawer","visible").toBool());
        QCOMPARE(bible->noteText(),QString("Paragraph note"));
        const auto noteMarker = "blockInlineMarker:note:open:Genesis 1:1";
        QTRY_VERIFY(item(noteMarker) && item(noteMarker)->isVisible());
        QVERIFY(item("blockInlineGlyph:note:Genesis 1:1")->isVisible());
        click(item(noteMarker));
        QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        QCOMPARE(prop("noteEditor","text").toString(),QString("Paragraph note"));
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("notesDrawer","visible").toBool());
        key(Qt::Key_F); QTRY_VERIFY(bible->focusMode());
        QTRY_VERIFY(!item(noteMarker) || !item(noteMarker)->isVisible());
        QVERIFY(item("chapterTitle")->isVisible());
        QVERIFY(doc->findBlock(blockOffset(6)) != doc->findBlock(blockOffset(5)));
        QVERIFY(doc->toPlainText().startsWith("In the beginning"));
        bible->setReadingMode("block");
        QTRY_COMPARE(doc->blockCount(),1);
        bible->setReadingMode("paragraph");
        QTRY_VERIFY(doc->blockCount() > 1);
        const auto first = doc->findBlock(blockOffset(1));
        // Source paragraph boundaries group 1:1–2, 1:3–5, and 1:6–8.
        QCOMPARE(doc->findBlock(blockOffset(2)),first);
        const auto second = doc->findBlock(blockOffset(3));
        QVERIFY(second != first);
        for (int verse = 4; verse <= 5; ++verse)
            QVERIFY2(doc->findBlock(blockOffset(verse)) == second,
                      qPrintable(QString("Verse %1 starts a separate block: %2").arg(verse).arg(doc->toPlainText().left(1600))));
        const auto third = doc->findBlock(blockOffset(6));
        QVERIFY(third != second);
        QVERIFY(third != first);
        QCOMPARE(doc->findBlock(blockOffset(7)),third);
        QCOMPARE(doc->findBlock(blockOffset(8)),third);
    }
    void emptyAndImportError() {
        QVERIFY(!bible->ready());
        QSignalSpy newWindow(window,SIGNAL(newWindowRequested()));
        QVERIFY(newWindow.isValid());
        key(Qt::Key_N,Qt::ControlModifier);
        QCOMPARE(newWindow.count(),1);
        QCOMPARE(window->title(),QString("OmaWord"));
        QVERIFY(item("setupScroll")->isVisible());
        click(item("setupImport"));
        QTRY_VERIFY(prop("importGuide","opened").toBool());
        QVERIFY(!item("importReplacement")->isVisible());
        QCOMPARE(prop("setupTitle","text").toString(),desktop->property("appName").toString());
        QCOMPARE(prop("setupDescription","text").toString(),QString("Add a version to begin. Install from the catalog or import a USFM ZIP.\nRead offline, bookmark and highlight passages, and keep your notes alongside the text."));
        QCOMPARE(prop("setupPrivacy","text").toString(),QString("All your Bible versions, highlights, bookmarks, and notes stay on this device."));
        QVERIFY(!item("savedControl")->isVisible()); QVERIFY(!item("notesControl"));
        QVERIFY(item("helpControl")->isVisible());
        QCOMPARE(prop("catalogResults","count").toInt(),7);
        QVERIFY(prop("importSupported","text").toString().contains("USFM ZIP"));
        capture("revision-import-guide-setup");
        key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("importGuide","visible").toBool());
        QVERIFY(!prop("importDialog","visible").toBool());
        // Reading settings work before any version is installed.
        key(Qt::Key_R,Qt::ControlModifier);
        QTRY_VERIFY(prop("settingsDialog","opened").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("settingsDialog","visible").toBool());
        key(Qt::Key_Space,Qt::ControlModifier);
        QTRY_VERIFY(prop("passagePalette","opened").toBool());
        type("write a note"); key(Qt::Key_Return);
        QVERIFY(prop("passagePalette","opened").toBool());
        QVERIFY(!window->property("notesOpen").toBool());
        QVERIFY(QMetaObject::invokeMethod(window,"runCommand",Q_ARG(QVariant,"notes")));
        QVERIFY(QMetaObject::invokeMethod(window,"openNotes",Q_ARG(QVariant,true)));
        QVERIFY(!window->property("notesOpen").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        key(Qt::Key_P,Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(prop("passagePalette","opened").toBool());
        type("add version"); key(Qt::Key_Return);
        QTRY_VERIFY(prop("importGuide","opened").toBool());
        QVERIFY(!prop("passagePalette","visible").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("importGuide","visible").toBool());
        key(Qt::Key_F1); QTRY_VERIFY(prop("helpDialog","opened").toBool());
        type("write a note");
        QTRY_VERIFY(item("keybindingWrite a note"));
        QVERIFY(!item("keybindingWrite a note")->isEnabled());
        key(Qt::Key_Return); QVERIFY(prop("helpDialog","opened").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("helpDialog","visible").toBool());
        QVERIFY(!item("readerSurface")->isVisible());
        window->resize(640,480);
        key(Qt::Key_K, Qt::ControlModifier);
        QTRY_VERIFY(prop("passagePalette", "opened").toBool());
        QVERIFY(prop("referenceInput", "text").toString().startsWith('>'));
        key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("passagePalette", "visible").toBool());
        const auto invalidPath = session->filePath("invalid.zip");
        QFile invalid(invalidPath);
        QVERIFY(invalid.open(QIODevice::WriteOnly));
        invalid.write("Invalid USFM ZIP test fixture"); invalid.close();
        QVERIFY(object("importDialog")->setProperty("selectedFile",QUrl::fromLocalFile(invalidPath)));
        QVERIFY(QMetaObject::invokeMethod(object("importDialog"),"accepted"));
        // Metadata inspection can reject invalid archives before async import starts.
        QTRY_VERIFY(!bible->busy());
        QVERIFY(!bible->error().isEmpty());
        QVERIFY(item("errorMessage")->isVisible());
        QCOMPARE(prop("errorMessage","text").toString(),bible->error());
        QVERIFY(!bible->ready());
        QVERIFY(item("setupScroll")->isVisible());
    }
    void asyncFirstImportLayout() {
        const QString usfm = netSource();
        if (usfm.isEmpty()) QSKIP("Set OMAWORD_TEST_USFM_DIR to exercise asynchronous first import into the loaded UI.");
        QVERIFY(!bible->ready());
        QVERIFY(item("setupScroll")->isVisible());
        QVERIFY(!item("readerSurface")->isVisible());
        QVERIFY(desktop->setProperty("fontFamily",QString("VictorMono Nerd Font Mono")));
        QVERIFY(desktop->setProperty("textScale",1.0));
        window->resize(1267,694);
        QSignalSpy ready(bible.get(),&BibleBackend::readyChanged);
        QSignalSpy frames(window,&QQuickWindow::frameSwapped);
        const auto source = session->filePath("download.zip");
        QVERIFY(QFile::copy(usfm,source));
        click(item("setupImport"));
        QTRY_VERIFY(prop("importGuide","opened").toBool());
        // Exercise the existing file acceptance handler without a native picker.
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("importGuide","visible").toBool());
        QVERIFY(object("importDialog")->setProperty("selectedFile",QUrl::fromLocalFile(source)));
        QVERIFY(QMetaObject::invokeMethod(object("importDialog"),"accepted"));
        QVERIFY(bible->busy());
        QVERIFY(prop("busyPopup","visible").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(),120000);
        QVERIFY2(bible->ready(),qPrintable(bible->error()));
        QCOMPARE(ready.count(),1);
        QTRY_VERIFY(!prop("busyPopup","visible").toBool());
        QTRY_VERIFY(item("readerSurface")->isVisible());
        QVERIFY(!item("setupScroll")->isVisible());
        QVERIFY(QFile::remove(source));
        reference("John 1");
        for (const QSize size : {QSize(1267,694),QSize(640,480),QSize(1120,820)}) {
            window->resize(size);
            const auto previousFrames = frames.count();
            window->update();
            QTRY_VERIFY(frames.count() > previousFrames);
            QTest::qWait(100);
            QVERIFY(item("verseNumber1"));
            QVERIFY(item("verseNumber2"));
            QVERIFY(item("verseHeading1"));
            QVERIFY(item("verseHeading1")->height() > 36);
            QCOMPARE(item("verseHeading2")->height(),0.0);
            const auto headingHeight = item("verseHeading1")->height();
            // Hiding an ancestor must not collapse the measured heading and
            // re-enter its implicit-size binding when it becomes visible again.
            item("readerSurface")->setVisible(false);
            QCoreApplication::processEvents();
            QCOMPARE(item("verseHeading1")->height(),headingHeight);
            item("readerSurface")->setVisible(true);
            QCoreApplication::processEvents();
            QCOMPARE(item("verseHeading1")->height(),headingHeight);
            QVERIFY(item("verseNumber1")->mapToItem(item("reading"),QPointF()).y() > 0);
            QVERIFY(item("verseNumber2")->mapToItem(item("reading"),QPointF()).y()
                > item("verseNumber1")->mapToItem(item("reading"),QPointF()).y());
        }
        QVERIFY2(qmlWarnings.isEmpty(),qPrintable(qmlWarnings.join('\n')));
        const auto importedText = bodyText(1);
        QVERIFY(!importedText.isEmpty());
        unloadUi(); loadUi();
        reference("John 1");
        QCOMPARE(bodyText(1),importedText);
    }
    void referenceAliasesAndBrowsing() {
        bible->setVersePicker(false); // Explicit legacy chapter-only browsing.
        reference("John3:16");
        QCOMPARE(bible->book(),43); QCOMPARE(bible->chapter(),3); QCOMPARE(bible->selectedStart(),16);
        QVERIFY(prop("reading", "contentY").toDouble() > 0);
        reference("1 Cor 13:4-7");
        QCOMPARE(bible->book(),46); QCOMPARE(bible->chapter(),13);
        QCOMPARE(bible->selectedStart(),4); QCOMPARE(bible->selectedEnd(),7);
        reference("Ps 23");
        QCOMPARE(bible->book(),19); QCOMPARE(bible->chapter(),23);
        key(Qt::Key_K, Qt::ControlModifier);
        QTRY_VERIFY(prop("passagePalette", "opened").toBool());
        type("John"); key(Qt::Key_Return);
        QTRY_COMPARE(window->property("paletteBook").toInt(),43);
        type("3"); key(Qt::Key_Return);
        QTRY_VERIFY(!prop("passagePalette", "visible").toBool());
        QCOMPARE(bible->book(),43); QCOMPARE(bible->chapter(),3);
        QCOMPARE(bible->selectedStart(),0);
    }
    void invalidReferenceKeepsPassage() {
        reference("John 3:16");
        key(Qt::Key_K, Qt::ControlModifier);
        QTRY_VERIFY(prop("passagePalette", "opened").toBool());
        type("John 300:999"); key(Qt::Key_Return);
        QVERIFY(bible->error().isEmpty());
        QCOMPARE(prop("referencePreview", "text").toString(),bible->resolveReference("John 300:999").value("error").toString());
        QVERIFY(prop("passagePalette", "visible").toBool());
        QCOMPARE(bible->chapter(),3); QCOMPARE(bible->selectedStart(),16);
        key(Qt::Key_A, Qt::ControlModifier); type("Jn 4:2"); key(Qt::Key_Return);
        QTRY_VERIFY(!prop("passagePalette", "visible").toBool());
        QVERIFY(bible->error().isEmpty()); QCOMPARE(bible->chapter(),4);
    }
    void defaultVersePickerAndResume() {
        QVERIFY(bible->versePicker());
        reference("John 3:16-18");
        bible->setNoteText("Browsing must preserve this range note");
        bible->toggleBookmark();
        QTest::qWait(450);
        const auto collection = bible->collection();
        const auto position = bible->readingPosition();
        QSignalSpy navigation(bible.get(),&BibleBackend::navigationChanged);
        QSignalSpy selection(bible.get(),&BibleBackend::selectionChanged);
        key(Qt::Key_K,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("Matthew"); key(Qt::Key_Right);
        type("17");
        QCOMPARE(prop("referencePreview","text").toString(),QString("Browse verses in Matthew 17"));
        key(Qt::Key_Right);
        QCOMPARE(window->property("paletteChapter").toInt(),17);
        QCOMPARE(prop("referenceInput","placeholderText").toString(),QString::fromUtf8("Matthew 17 — verse number"));
        QCOMPARE(prop("paletteResults","count").toInt(),26);
        type("2");
        QCOMPARE(prop("paletteResults","count").toInt(),8); // 2,20,22..27, never 21.
        key(Qt::Key_Down); key(Qt::Key_Down);
        QCOMPARE(prop("referencePreview","text").toString(),QString("Matthew 17:22"));
        const int index = prop("paletteResults","currentIndex").toInt();
        capture("phase-b-verse-picker");
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        key(Qt::Key_Space,Qt::ShiftModifier);
        QVERIFY(!prop("passagePalette","visible").toBool());
        key(Qt::Key_Space); QTRY_VERIFY(prop("passagePalette","opened").toBool());
        QCOMPARE(window->property("paletteChapter").toInt(),17);
        QCOMPARE(prop("referenceInput","text").toString(),QString("2"));
        QCOMPARE(prop("paletteResults","currentIndex").toInt(),index);
        QCOMPARE(prop("referencePreview","text").toString(),QString("Matthew 17:22"));
        key(Qt::Key_Left);
        QCOMPARE(window->property("paletteChapter").toInt(),0);
        QCOMPARE(window->property("paletteBook").toInt(),40);
        QCOMPARE(prop("referencePreview","text").toString(),QString("Browse verses in Matthew 17"));
        key(Qt::Key_Right);
        key(Qt::Key_Up);
        QVERIFY(item("paletteBack")->hasActiveFocus());
        QCOMPARE(prop("referencePreview","text").toString(),QString("Browse chapters in Matthew"));
        key(Qt::Key_Return); key(Qt::Key_Left);
        QCOMPARE(window->property("paletteBook").toInt(),0);
        QCOMPARE(navigation.count(),0); QCOMPARE(selection.count(),0);
        QCOMPARE(bible->readingPosition(),position);
        QCOMPARE(bible->collection(),collection);
        QCOMPARE(bible->selectionReference(),QString::fromUtf8("John 3:16–18"));
        type("Matthew"); key(Qt::Key_Return); type("17"); key(Qt::Key_Return);
        type("21"); key(Qt::Key_Return);
        QVERIFY(prop("passagePalette","visible").toBool());
        QCOMPARE(navigation.count(),0);
        key(Qt::Key_A,Qt::ControlModifier); type("22"); key(Qt::Key_Return);
        QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        QCOMPARE(bible->selectionReference(),QString("Matthew 17:22"));
        reference("John 4"); QCOMPARE(bible->selectedStart(),0);
        reference("John 3:16-18"); QCOMPARE(bible->selectedEnd(),18);
        // Explicit entry shortcuts reset mode and all picker context.
        key(Qt::Key_Space,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("headings"); key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        key(Qt::Key_Space); QTRY_VERIFY(prop("passagePalette","opened").toBool());
        QCOMPARE(prop("referenceInput","text").toString(),QString("> headings"));
        key(Qt::Key_K,Qt::ControlModifier);
        QCOMPARE(prop("referenceInput","text").toString(),QString());
        QCOMPARE(window->property("paletteBook").toInt(),0);
        QCOMPARE(window->property("paletteChapter").toInt(),0);
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        key(Qt::Key_Space,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("verse picker"); key(Qt::Key_Return);
        QTRY_VERIFY(!bible->versePicker());
        unloadUi(); loadUi();
        QVERIFY(!bible->versePicker());
        key(Qt::Key_K,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("John"); key(Qt::Key_Return); type("3"); key(Qt::Key_Right);
        QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        QCOMPARE(bible->passage(),QString("John 3")); QCOMPARE(bible->selectedStart(),0);
        QTRY_COMPARE(prop("reading","contentY").toDouble(),0.0);
    }
    void notesOverlayDismissalAndSaveFailure() {
        reference("John 3:16-18");
        const auto width = item("reading")->width();
        const auto height = item("verseBody16")->height();
        const auto position = prop("reading","contentY");
        key(Qt::Key_W);
        QVERIFY(prop("notesDrawer","visible").toBool());
        QVERIFY(prop("notesDrawer","position").toDouble() < 1);
        QTRY_VERIFY(prop("notesDrawer","opened").toBool());
        QCOMPARE(prop("notesDrawer","edge").toInt(),int(Qt::RightEdge));
        QCOMPARE(item("reading")->width(),width);
        QCOMPARE(item("verseBody16")->height(),height);
        QCOMPARE(prop("reading","contentY"),position);
        type("A guarded draft");
        capture("phase-b-notes-editor");
        key(Qt::Key_Escape);
        QVERIFY(prop("notesDrawer","visible").toBool()); // Exit animates too.
        QTRY_VERIFY(!prop("notesDrawer","visible").toBool());
        QCOMPARE(bible->noteText(),QString("A guarded draft"));
        key(Qt::Key_W); QTRY_VERIFY(prop("notesDrawer","opened").toBool());
        click(item("previewButton"));
        key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        QVERIFY(!window->property("notePreview").toBool());
        {
            // Typing owns ctrl keys: ctrl n opens no window from the note editor.
            QSignalSpy opened(window,SIGNAL(newWindowRequested()));
            const auto text = prop("noteEditor","text").toString();
            key(Qt::Key_N,Qt::ControlModifier);
            QCOMPARE(opened.count(),0);
            QCOMPARE(prop("noteEditor","text").toString(),text);
            QVERIFY(item("noteEditor")->hasActiveFocus());
            QVERIFY(prop("notesDrawer","opened").toBool());
        };
        key(Qt::Key_Left,Qt::AltModifier); key(Qt::Key_Right,Qt::AltModifier);
        QCOMPARE(bible->chapter(),3);
        QVERIFY(prop("notesDrawer","opened").toBool());
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","ui-note-failure");
            db.setDatabaseName(bible->dataDirectory() + "/personal.sqlite");
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec("CREATE TRIGGER reject_note BEFORE INSERT ON annotations BEGIN SELECT RAISE(FAIL,'test note failure'); END"));
            type(" unsaved");
            const auto draft = prop("noteEditor","text").toString();
            QVERIFY(draft != bible->noteText());
            QSignalSpy newWindow(window,SIGNAL(newWindowRequested()));
            QVERIFY(newWindow.isValid());
            for (int dismissal = 0; dismissal < 9; ++dismissal) {
                if (dismissal == 0) key(Qt::Key_Escape);
                if (dismissal == 1) QVERIFY(QMetaObject::invokeMethod(window,"toggleNotes"));
                if (dismissal == 2) QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,QPoint(30,200));
                if (dismissal == 3) {
                    QVERIFY(QMetaObject::invokeMethod(window,"runCommand",Q_ARG(QVariant,"focus")));
                    QVERIFY(!bible->focusMode());
                }
                if (dismissal == 4) { key(Qt::Key_F1); QVERIFY(!prop("helpDialog","visible").toBool()); }
                if (dismissal == 5) {
                    QVERIFY(QMetaObject::invokeMethod(window,"requestDeleteSaved"));
                    QVERIFY(!prop("deleteConfirmation","visible").toBool());
                }
                if (dismissal == 6) {
                    QVERIFY(QMetaObject::invokeMethod(window,"runCommand",Q_ARG(QVariant,"import")));
                    QVERIFY(!prop("importGuide","visible").toBool());
                    QVERIFY(!prop("importDialog","visible").toBool());
                }
                if (dismissal == 7) {
                    QVERIFY(object("importDialog")->setProperty("selectedFile",QUrl::fromLocalFile(fixture)));
                    QVERIFY(QMetaObject::invokeMethod(object("importDialog"),"accepted"));
                    QVERIFY(!bible->busy());
                    QVERIFY(bible->error().contains("test note failure"));
                }
                if (dismissal == 8) {
                    key(Qt::Key_N,Qt::ControlModifier);
                    QCOMPARE(newWindow.count(),0);
                }
                QTest::qWait(250);
                QVERIFY(prop("notesDrawer","opened").toBool());
                QCOMPARE(prop("noteEditor","text").toString(),draft);
                QCOMPARE(bible->selectionReference(),QString::fromUtf8("John 3:16–18"));
            }
            QVERIFY(query.exec("DROP TRIGGER reject_note"));
            QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,QPoint(30,200));
            QTRY_VERIFY(!prop("notesDrawer","visible").toBool());
            QCOMPARE(bible->noteText(),draft);
        }
        QSqlDatabase::removeDatabase("ui-note-failure");
        key(Qt::Key_W); QTRY_VERIFY(prop("notesDrawer","opened").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("notesDrawer","visible").toBool());
        QCOMPARE(bible->selectedStart(),16); QCOMPARE(bible->selectedEnd(),18);
    }
    void peerNoteUpdatesPreserveDirtyDraft() {
        reference("John 3:16");
        key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        type("Initial note");
        QCOMPARE(window->property("loadedNoteText").toString(),QString("Initial note"));
        QCOMPARE(window->property("loadedVersionId").toString(),bible->versionId());
        BibleBackend peer;
        QVERIFY(peer.navigate("John 3:16"));
        peer.setNoteText("Clean peer update");
        QTRY_COMPARE(prop("noteEditor","text").toString(),QString("Clean peer update"));
        QVERIFY(!window->property("noteConflict").toBool());
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","ui-peer-note-failure");
            db.setDatabaseName(bible->dataDirectory() + "/personal.sqlite");
            QVERIFY(db.open()); QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TRIGGER reject_peer_note BEFORE DELETE ON annotations BEGIN SELECT RAISE(ABORT,'peer draft save failure'); END"));
            QVERIFY(object("noteEditor")->setProperty("text","My unsaved draft"));
            QVERIFY(bible->error().contains("peer draft save failure"));
            QCOMPARE(window->property("loadedNoteText").toString(),QString("Clean peer update"));
            QVERIFY(q.exec("DROP TRIGGER reject_peer_note"));
        }
        QSqlDatabase::removeDatabase("ui-peer-note-failure");
        peer.toggleHighlight();
        QVERIFY(QMetaObject::invokeMethod(window,"syncNote"));
        QCOMPARE(prop("noteEditor","text").toString(),QString("My unsaved draft"));
        QVERIFY(!window->property("noteConflict").toBool());
        peer.setNoteText("Newer peer text");
        bible->refreshCollection();
        QTRY_VERIFY(window->property("noteConflict").toBool());
        QCOMPARE(prop("noteEditor","text").toString(),QString("My unsaved draft"));
        QCOMPARE(bible->noteText(),QString("Newer peer text"));
        QVERIFY(item("noteConflictMessage")->isVisible());
        QVERIFY(prop("noteConflictSaved","text").toString().contains("Newer peer text"));
        // Editing, navigation, and window creation cannot silently overwrite it.
        QVERIFY(object("noteEditor")->setProperty("text","My revised draft"));
        QVariant flushed;
        QVERIFY(QMetaObject::invokeMethod(window,"flushNote",Q_RETURN_ARG(QVariant,flushed)));
        QVERIFY(!flushed.toBool());
        QSignalSpy requested(window,SIGNAL(newWindowRequested()));
        key(Qt::Key_N,Qt::ControlModifier);
        QCOMPARE(requested.count(),0);
        key(Qt::Key_Escape); QVERIFY(prop("notesDrawer","visible").toBool());
        QCOMPARE(peer.noteText(),QString("Newer peer text"));
        // Returning to the old baseline is still an edit, not consent to load
        // or overwrite the newer peer revision.
        QVERIFY(object("noteEditor")->setProperty("text","Clean peer update"));
        QCOMPARE(prop("noteEditor","text").toString(),QString("Clean peer update"));
        QVERIFY(window->property("noteConflict").toBool());
        QCOMPARE(peer.noteText(),QString("Newer peer text"));
        QVERIFY(object("noteEditor")->setProperty("text","My revised draft"));
        QVERIFY(QMetaObject::invokeMethod(object("noteRetry"),"clicked"));
        QCOMPARE(peer.noteText(),QString("My revised draft"));
        QVERIFY(!window->property("noteConflict").toBool());
        QCOMPARE(window->property("loadedNoteText").toString(),peer.noteText());
        QVERIFY(object("noteEditor")->setProperty("text","Autosave continues"));
        QCOMPARE(peer.noteText(),QString("Autosave continues"));
        // A repeated peer notification cannot make a staged draft clean.
        QVERIFY(window->setProperty("syncingNote",true));
        QVERIFY(object("noteEditor")->setProperty("text","Another draft"));
        QVERIFY(window->setProperty("syncingNote",false));
        peer.setNoteText("Use this peer revision");
        bible->refreshCollection();
        QVERIFY(window->property("noteConflict").toBool());
        QCOMPARE(prop("noteEditor","text").toString(),QString("Another draft"));
        QVERIFY(QMetaObject::invokeMethod(object("noteUseSaved"),"clicked"));
        QCOMPARE(prop("noteEditor","text").toString(),QString("Use this peer revision"));
        QVERIFY(!window->property("noteConflict").toBool());
    }
    void dirtyDraftScopeGuard() {
        reference("John 3:16");
        key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        QVERIFY(window->setProperty("syncingNote",true));
        QVERIFY(object("noteEditor")->setProperty("text","Draft for original scope"));
        QVERIFY(window->setProperty("syncingNote",false));
        // Exercise an external scope change bypassing the UI's normal flush guard.
        QVERIFY(bible->navigate("John 3:17"));
        QVERIFY(window->property("noteConflict").toBool());
        QCOMPARE(window->property("loadedNoteReference").toString(),QString("John 3:16"));
        QCOMPARE(prop("noteEditor","text").toString(),QString("Draft for original scope"));
        QVERIFY(!item("noteRetry")->isEnabled());
        QVERIFY(QMetaObject::invokeMethod(window,"retryNote"));
        QVERIFY(bible->noteText().isEmpty());
        QVERIFY(bible->navigate("John 3:16"));
        QVERIFY(QMetaObject::invokeMethod(window,"retryNote"));
        QCOMPARE(bible->noteText(),QString("Draft for original scope"));
        QVERIFY(!window->property("noteConflict").toBool());
        QVERIFY(window->setProperty("syncingNote",true));
        QVERIFY(object("noteEditor")->setProperty("text","Draft for another version"));
        QVERIFY(window->setProperty("syncingNote",false));
        // Same verse reference must not authorize a write to a different edition.
        QVERIFY(window->setProperty("loadedVersionId",QString("other-version")));
        QVERIFY(QMetaObject::invokeMethod(window,"syncNote"));
        QVERIFY(window->property("noteConflict").toBool());
        QVERIFY(!item("noteRetry")->isEnabled());
        QVERIFY(QMetaObject::invokeMethod(window,"retryNote"));
        QCOMPARE(bible->noteText(),QString("Draft for original scope"));
        QCOMPARE(prop("noteEditor","text").toString(),QString("Draft for another version"));
        QVERIFY(QMetaObject::invokeMethod(window,"loadSavedNote"));
    }
    void actionableKeybindingsAndPreferences() {
        reference("John 3:16");
        const auto action = [this](const QString &query) {
            key(Qt::Key_F1);
            QTRY_VERIFY(item("keybindingSearch")->hasActiveFocus());
            type(query);
            QTRY_COMPARE(prop("keybindingResults","count").toInt(),1);
            key(Qt::Key_Return);
            QTRY_VERIFY(!prop("helpDialog","visible").toBool());
        };
        action("write a note");
        QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        type("Saved before dispatch");
        // Keys type into the editor; dispatch from the Notes panel itself.
        item("notesPanel")->forceActiveFocus();
        action("navigate to a passage");
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("John 4:2"); key(Qt::Key_Return);
        QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        QCOMPARE(saved(16).value("note").toString(),QString("Saved before dispatch"));
        // Esc from the reader closes an open Notes drawer too.
        QVERIFY(item("readerSurface")->hasActiveFocus()); QVERIFY(prop("notesDrawer","opened").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("notesDrawer","visible").toBool());
        action("previous chapter"); QCOMPARE(bible->chapter(),3);
        action("next chapter"); QCOMPARE(bible->chapter(),4);
        action("open commands");
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        QCOMPARE(prop("referenceInput","text").toString(),QString("> "));
        type("reading settings"); key(Qt::Key_Return);
        QTRY_VERIFY(prop("settingsDialog","opened").toBool());
        QTRY_VERIFY(item("lineHeightSlider")->hasActiveFocus());
        QCOMPARE(bible->lineHeight(),1.5);
        QCOMPARE(prop("lineHeightSlider","from").toDouble(),1.0);
        QCOMPARE(prop("lineHeightSlider","to").toDouble(),3.0);
        QCOMPARE(prop("lineHeightSlider","stepSize").toDouble(),0.1);
        key(Qt::Key_Right); QCOMPARE(bible->lineHeight(),1.5);
        key(Qt::Key_Up); QTRY_COMPARE(bible->lineHeight(),1.6);
        capture("phase-b-line-height");
        for (int i = 0; i < 30; ++i) key(Qt::Key_Equal,Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(bible->lineHeight(),3.0);
        for (int i = 0; i < 30; ++i) key(Qt::Key_Minus,Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(bible->lineHeight(),1.0);
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("settingsDialog","visible").toBool());
        key(Qt::Key_Equal,Qt::ControlModifier | Qt::AltModifier); QCOMPARE(bible->lineHeight(),1.1);
        key(Qt::Key_N,Qt::AltModifier); QVERIFY(!bible->showVerseNumbers());
        key(Qt::Key_H,Qt::AltModifier); QVERIFY(!bible->showHeadings());
        key(Qt::Key_Space,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("reading settings"); key(Qt::Key_Return);
        QTRY_VERIFY(prop("settingsDialog","opened").toBool());
        QTest::qWait(100); // Polish the newly visible settings layout before mouse input.
        QVERIFY(!prop("settingnumbers","checked").toBool());
        QVERIFY(!prop("settingheadings","checked").toBool());
        QVERIFY(prop("settingversePicker","checked").toBool());
        capture("phase-b-settings");
        click(item("settingversePicker")); QVERIFY(!bible->versePicker());
        click(item("settingfocus"));
        QVERIFY(prop("settingsDialog","opened").toBool());
        QVERIFY(bible->focusMode());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("settingsDialog","visible").toBool());
        reference("John 4:2");
        key(Qt::Key_W); QTRY_VERIFY(prop("notesDrawer","opened").toBool());
        QVERIFY(bible->focusMode());
        capture("phase-b-focused-notes");
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("notesDrawer","visible").toBool());
        QVERIFY(bible->focusMode());
        key(Qt::Key_F); QVERIFY(!bible->focusMode());
        key(Qt::Key_W); QTRY_VERIFY(prop("notesDrawer","opened").toBool());
        QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        item("notesPanel")->forceActiveFocus();
        action("focus mode");
        QTRY_VERIFY(!prop("notesDrawer","visible").toBool());
        QVERIFY(bible->focusMode());
        key(Qt::Key_F1); QTRY_VERIFY(prop("helpDialog","opened").toBool());
        const auto search = item("keybindingSearch");
        QVERIFY(!item("keybindingsClose"));
        QCOMPARE(search->width(),item("keybindingResults")->width());
        type("chapter");
        QCOMPARE(prop("keybindingResults","count").toInt(),7);
        QVERIFY(item("keybindingFind in chapter"));
        key(Qt::Key_Down); QCOMPARE(prop("keybindingResults","currentIndex").toInt(),1);
        key(Qt::Key_Up); QCOMPARE(prop("keybindingResults","currentIndex").toInt(),0);
        capture("phase-b-keybindings-focused");
        key(Qt::Key_A,Qt::ControlModifier); type("Shift Click");
        QCOMPARE(prop("keybindingResults","count").toInt(),1);
        QVERIFY(!item("keybindingSelect a range")->isEnabled());
        const auto selected = bible->selectionReference();
        key(Qt::Key_Return);
        QVERIFY(prop("helpDialog","opened").toBool());
        QCOMPARE(bible->selectionReference(),selected);
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("helpDialog","visible").toBool());
        unloadUi(); loadUi();
        QVERIFY(bible->focusMode()); QVERIFY(!window->property("notesOpen").toBool());
        QVERIFY(!bible->showVerseNumbers()); QVERIFY(!bible->showHeadings());
        QVERIFY(!bible->versePicker()); QCOMPARE(bible->lineHeight(),1.1);
        key(Qt::Key_F); QVERIFY(!bible->focusMode());
        key(Qt::Key_N,Qt::AltModifier); key(Qt::Key_H,Qt::AltModifier);
        QVERIFY(bible->showVerseNumbers()); QVERIFY(bible->showHeadings());
    }
    void settingsSmallAndDynamicCommandHints() {
        window->resize(640,480);
        QVERIFY(desktop->setProperty("textScale",1.8));
        key(Qt::Key_Space,Qt::ControlModifier);
        QTRY_VERIFY(prop("passagePalette","opened").toBool());
        type("verse picker");
        QTRY_VERIFY(item("paletteHintversePicker"));
        QCOMPARE(prop("paletteHintversePicker","text").toString(),QString("On · alt p"));
        key(Qt::Key_Return);
        QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        QVERIFY(!bible->versePicker());
        key(Qt::Key_Space); QTRY_VERIFY(prop("passagePalette","opened").toBool());
        QCOMPARE(prop("referenceInput","text").toString(),QString("> verse picker"));
        QTRY_COMPARE(prop("paletteHintversePicker","text").toString(),QString("Off · alt p"));
        key(Qt::Key_A,Qt::ControlModifier); type("> reading settings"); key(Qt::Key_Return);
        QTRY_VERIFY(prop("settingsDialog","opened").toBool());
        QVERIFY(prop("settingsDialog","height").toDouble() <= 432);
        capture("phase-b-settings-scaled");
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("settingsDialog","visible").toBool());
        key(Qt::Key_Space,Qt::ControlModifier);
        QTRY_VERIFY(prop("passagePalette","opened").toBool());
        type("show verse numbers");
        QTRY_COMPARE(prop("paletteHintnumbers","text").toString(),QString("On · alt n"));
        key(Qt::Key_Return); QTRY_VERIFY(!bible->showVerseNumbers());
        key(Qt::Key_Space); QTRY_VERIFY(prop("passagePalette","opened").toBool());
        QTRY_COMPARE(prop("paletteHintnumbers","text").toString(),QString("Off · alt n"));
    }
    void settingsKeysAndSwitchGeometry_data() {
        QTest::addColumn<double>("scale");
        QTest::newRow("normal") << 1.0;
        QTest::newRow("scaled") << 1.8;
    }
    void settingsKeysAndSwitchGeometry() {
        QFETCH(double,scale);
        window->resize(640,480);
        QVERIFY(desktop->setProperty("textScale",scale));
        QVERIFY(!bible->property("blockMode").toBool());
        key(Qt::Key_P,Qt::AltModifier); QVERIFY(!bible->versePicker());
        key(Qt::Key_P,Qt::AltModifier); QVERIFY(bible->versePicker());
        key(Qt::Key_P,Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(prop("passagePalette","opened").toBool());
        key(Qt::Key_Space,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("reading settings"); key(Qt::Key_Return);
        QTRY_VERIFY(prop("settingsDialog","opened").toBool());
        QTest::qWait(100);
        for (const auto id : {"numbers","headings","versePicker","focus"}) {
            const QByteArray name = QByteArray("setting") + id;
            auto control = item(name);
            auto label = item(name + "Label");
            auto shortcut = item(name + "Key");
            auto track = item(name + "Track");
            auto thumb = item(name + "Thumb");
            QVERIFY(control && label && shortcut && track && thumb);
            const auto labelRect = label->mapRectToItem(control,label->boundingRect());
            const auto keyRect = shortcut->mapRectToItem(control,shortcut->boundingRect());
            const auto trackRect = track->mapRectToItem(control,track->boundingRect());
            QVERIFY(labelRect.left() < 1);
            QVERIFY(labelRect.right() < keyRect.left());
            QVERIFY(keyRect.right() < trackRect.left());
            QCOMPARE(trackRect.right(),control->width());
            QCOMPARE(track->property("radius").toDouble(),qMin(desktop->cornerRadius(),qMin(track->width(),track->height()) / 2));
            QCOMPARE(thumb->property("radius").toDouble(),qMin(desktop->cornerRadius(),qMin(thumb->width(),thumb->height()) / 2));
        }
        key(Qt::Key_N,Qt::AltModifier); key(Qt::Key_H,Qt::AltModifier); key(Qt::Key_P,Qt::AltModifier); key(Qt::Key_L,Qt::AltModifier);
        QVERIFY(!bible->showVerseNumbers()); QVERIFY(!bible->showHeadings());
        QVERIFY(!bible->versePicker()); QVERIFY(bible->property("blockMode").toBool());
        key(Qt::Key_Equal,Qt::ControlModifier | Qt::AltModifier); QCOMPARE(bible->lineHeight(),1.6);
        key(Qt::Key_Minus,Qt::ControlModifier | Qt::AltModifier); QCOMPARE(bible->lineHeight(),1.5);
        key(Qt::Key_Minus,Qt::ControlModifier | Qt::AltModifier); QCOMPARE(bible->lineHeight(),1.4);
        key(Qt::Key_0,Qt::ControlModifier | Qt::AltModifier); QCOMPARE(bible->lineHeight(),1.5);
        key(Qt::Key_F);
        QVERIFY(bible->focusMode()); QVERIFY(prop("settingsDialog","opened").toBool());
        QVERIFY(window->property("footerQuiet").toBool());
        QVERIFY(!bible->showVerseNumbers()); QVERIFY(!bible->showHeadings());
        key(Qt::Key_F); QVERIFY(!bible->focusMode());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("settingsDialog","visible").toBool());
        bible->setLineHeight(2.0);
        key(Qt::Key_0,Qt::ControlModifier | Qt::AltModifier); QCOMPARE(bible->lineHeight(),1.5);
        unloadUi(); loadUi();
        QVERIFY(!bible->showVerseNumbers()); QVERIFY(!bible->showHeadings());
        QVERIFY(!bible->versePicker()); QVERIFY(bible->property("blockMode").toBool());
    }
    void paletteChevronsAndFreshSnapshots() {
        key(Qt::Key_K,Qt::ControlModifier);
        QTRY_VERIFY(prop("passagePalette","opened").toBool());
        QTRY_VERIFY(item("paletteChevron0")->isVisible());
        QCOMPARE(prop("paletteBooks","text").toString(),QString("Passage"));
        QVERIFY(prop("paletteBooks","emphasized").toBool());
        QCOMPARE(prop("paletteSearch","text").toString(),QString("Search"));
        QVERIFY(!prop("paletteSearch","emphasized").toBool());
        type("John"); key(Qt::Key_Return);
        QTRY_VERIFY(item("paletteChevron0")->isVisible());
        type("3"); key(Qt::Key_Return);
        QTRY_VERIFY(item("paletteChevron0"));
        QVERIFY(!item("paletteChevron0")->isVisible());
        type("2"); key(Qt::Key_Down);
        const int row = prop("paletteResults","currentIndex").toInt();
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        key(Qt::Key_Space); QTRY_VERIFY(prop("passagePalette","opened").toBool());
        QCOMPARE(prop("referenceInput","text").toString(),QString("2"));
        QCOMPARE(prop("paletteResults","currentIndex").toInt(),row);
        QCOMPARE(window->property("paletteChapter").toInt(),3);
        key(Qt::Key_Space,Qt::ControlModifier);
        QCOMPARE(prop("referenceInput","text").toString(),QString("> "));
        QTRY_VERIFY(item("paletteChevron0"));
        QVERIFY(!item("paletteChevron0")->isVisible());
        type("headings"); key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        key(Qt::Key_Space); QTRY_VERIFY(prop("passagePalette","opened").toBool());
        QCOMPARE(prop("referenceInput","text").toString(),QString("> headings"));
        // The current mode's key keeps the query.
        key(Qt::Key_Space,Qt::ControlModifier);
        QCOMPARE(prop("referenceInput","text").toString(),QString("> headings"));
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        key(Qt::Key_Space); QTRY_VERIFY(prop("passagePalette","opened").toBool());
        QCOMPARE(prop("referenceInput","text").toString(),QString("> headings"));
        bible->setVersePicker(false);
        key(Qt::Key_K,Qt::ControlModifier);
        QCOMPARE(window->property("paletteBook").toInt(),0);
        QCOMPARE(window->property("paletteChapter").toInt(),0);
        QTRY_VERIFY(item("paletteChevron0")->isVisible());
        type("John"); key(Qt::Key_Return);
        QTRY_VERIFY(item("paletteChevron0"));
        QVERIFY(!item("paletteChevron0")->isVisible());
    }
    void keybindingEdgesAndOverlayScrollbar() {
        key(Qt::Key_F1); QTRY_VERIFY(item("keybindingSearch")->hasActiveFocus());
        QTest::qWait(100);
        auto list = item("keybindingResults");
        auto bar = item("keybindingScrollbar");
        QVERIFY(list && bar && item("keybindingFooterRule"));
        QCOMPARE(item("keybindingNavigate to a passage")->width(),list->width());
        QCOMPARE(prop("keybindingFooterRule","height").toDouble(),1.0);
        QVERIFY(bar->width() >= 18);
        const int count = prop("keybindingResults","count").toInt();
        key(Qt::Key_End); QCOMPARE(prop("keybindingResults","currentIndex").toInt(),count - 1);
        key(Qt::Key_Home); QCOMPARE(prop("keybindingResults","currentIndex").toInt(),0);
        QTest::qWait(100);
        const double top = prop("keybindingResults","contentY").toDouble();
        key(Qt::Key_PageDown);
        const int bottomRow = prop("keybindingResults","currentIndex").toInt();
        QVERIFY(bottomRow > 0 && bottomRow < count - 1);
        QCOMPARE(prop("keybindingResults","contentY").toDouble(),top + list->height());
        key(Qt::Key_PageUp);
        QCOMPARE(prop("keybindingResults","contentY").toDouble(),top);
        QCOMPARE(prop("keybindingResults","currentIndex").toInt(),0);
        const QPoint hover = bar->mapToScene(QPointF(bar->width()/2,bar->height()/2)).toPoint();
        QTest::mouseMove(window,hover);
        QTRY_VERIFY(bar->property("hovered").toBool());
        QTRY_COMPARE(item("keybindingScrollbarThumb")->width(),10.0);
        QTest::mouseMove(window,QPoint(5,5));
        QTRY_VERIFY(!bar->property("hovered").toBool());
        QTRY_COMPARE(prop("keybindingScrollbarThumb","opacity").toDouble(),0.5);
        QTRY_COMPARE(prop("keybindingScrollbarThumb","opacity").toDouble(),0.2);
        type("alt p"); QCOMPARE(prop("keybindingResults","count").toInt(),1);
        QVERIFY(item("keybindingVerse picker"));
        key(Qt::Key_A,Qt::ControlModifier); type("reading settings");
        QCOMPARE(prop("keybindingResults","count").toInt(),1);
        key(Qt::Key_A,Qt::ControlModifier); type("no such binding");
        QCOMPARE(prop("keybindingResults","count").toInt(),0);
        key(Qt::Key_End); QCOMPARE(prop("keybindingResults","currentIndex").toInt(),-1);
        key(Qt::Key_A,Qt::ControlModifier); type("reset line height");
        QCOMPARE(prop("keybindingResults","count").toInt(),1);
        bible->setLineHeight(2.0);
        key(Qt::Key_Return); QTRY_COMPARE(bible->lineHeight(),1.5);
    }
    // The current row lies wholly on the page; when page boundaries split
    // every row, it is the row with the most of itself showing.
    bool onPage(QQuickItem *list, const QRectF &rect) const {
        auto inside = [list](const QRectF &r) { return r.top() >= -1 && r.bottom() <= list->height() + 1; };
        if (inside(rect)) return true;
        auto visible = [list](const QRectF &r) { return qMin(r.bottom(),list->height()) - qMax(r.top(),0.0); };
        const auto content = qvariant_cast<QQuickItem *>(list->property("contentItem"));
        for (auto child : content->childItems()) {
            if (!child->isVisible() || child->height() <= 0) continue;
            const auto r = child->mapRectToItem(list,child->boundingRect());
            if (inside(r) || visible(r) > visible(rect) + 1) return false;
        }
        return visible(rect) > 0;
    }
    void keybindingGeometricPages() {
        window->resize(640,480);
        QVERIFY(desktop->setProperty("textScale",1.8));
        key(Qt::Key_F1); QTRY_VERIFY(item("keybindingSearch")->hasActiveFocus());
        QTest::qWait(150);
        auto list = item("keybindingResults");
        auto current = [&]() { return qvariant_cast<QQuickItem *>(prop("keybindingResults","currentItem")); };
        const double first = prop("keybindingResults","contentY").toDouble();
        const int count = prop("keybindingResults","count").toInt();
        QVERIFY(item("keybindingGo to verse number"));
        QVERIFY(item("keybindingGo to verse number")->height() > item("keybindingNext verse")->height());
        for (int i = 0; i < 40; ++i) {
            const double top = prop("keybindingResults","contentY").toDouble();
            const double end = prop("keybindingResults","originY").toDouble()
                + qMax(0.0,prop("keybindingResults","contentHeight").toDouble() - list->height());
            const int row = prop("keybindingResults","currentIndex").toInt();
            key(Qt::Key_PageDown);
            QVERIFY(qAbs(prop("keybindingResults","contentY").toDouble() - qMin(end,top + list->height())) < 1);
            QVERIFY(prop("keybindingResults","currentIndex").toInt() >= row);
            QVERIFY(current());
            const auto rect = current()->mapRectToItem(list,current()->boundingRect());
            QVERIFY2(onPage(list,rect),qPrintable(QString("Row outside page: %1..%2 / %3").arg(rect.top()).arg(rect.bottom()).arg(list->height())));
            if (top >= end - 1) break;
        }
        key(Qt::Key_End); QCOMPARE(prop("keybindingResults","currentIndex").toInt(),count - 1);
        for (int i = 0; i < 40; ++i) {
            const double top = prop("keybindingResults","contentY").toDouble();
            key(Qt::Key_PageUp);
            QVERIFY(qAbs(prop("keybindingResults","contentY").toDouble() - qMax(first,top - list->height())) < 1);
            QVERIFY(current());
            const auto rect = current()->mapRectToItem(list,current()->boundingRect());
            QVERIFY(onPage(list,rect));
            if (top <= first + 1) break;
        }
        // Use executable panel rows for the dispatch check; their order/height
        // can change independently of the full-catalog geometry checks above.
        bible->selectVerses(1,1); // Study needs a selection to be executable.
        type("panel");
        key(Qt::Key_Home); QCOMPARE(prop("keybindingResults","currentIndex").toInt(),0);
        key(Qt::Key_PageDown);
        QVERIFY(current()->isEnabled());
        const auto selectedAction = current()->property("modelData").toMap().value("id").toString();
        // The paged row is also the executable choice when focus stays in search.
        const QHash<QString,const char *> panels = {{"annotations","savedDrawer"},{"toggleNotes","notesDrawer"},{"related","relatedDrawer"}};
        QVERIFY(panels.contains(selectedAction));
        key(Qt::Key_Return);
        QTRY_VERIFY(!prop("helpDialog","visible").toBool());
        QTRY_VERIFY(prop(panels.value(selectedAction),"opened").toBool());
        key(Qt::Key_Escape);
        QTRY_VERIFY(!window->property("modalOpen").toBool());
        QTRY_VERIFY(!prop("notesDrawer","visible").toBool());
        key(Qt::Key_F1); QTRY_VERIFY(item("keybindingSearch")->hasActiveFocus());
        type("verse picker");
        key(Qt::Key_PageDown); key(Qt::Key_PageUp);
        QCOMPARE(prop("keybindingResults","currentIndex").toInt(),0);
        key(Qt::Key_Return); QTRY_VERIFY(!bible->versePicker());
    }
    void savedDeleteConfirmationAndIcons() {
        reference("John 3:16-18");
        bible->setNoteText("An overlapping range survives exact deletion");
        reference("John 3:16"); bible->toggleBookmark(); bible->toggleHighlight();
        bible->setNoteText("Delete this exact note");
        key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        item("notesPanel")->forceActiveFocus();
        key(Qt::Key_Space,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("annotations panel"); key(Qt::Key_Return);
        QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        QTest::qWait(100);
        // Each tab shows only its own annotation on a passage that has all three.
        const auto shown = [&](const char *name) { auto found = item(qPrintable(QString(name) + "John 3:16")); return found && found->isVisible(); };
        QCOMPARE(window->property("collectionTab").toInt(),0);
        QVERIFY(shown("savedBookmark")); QVERIFY(!shown("savedNote")); QVERIFY(!shown("savedNoteText")); QVERIFY(!shown("savedHighlight"));
        QVERIFY(!shown("savedTag")); QVERIFY(!item("savedTagsJohn 3:16"));
        if (qEnvironmentVariableIsSet("OMAWORD_SAVED_SCREENSHOT"))
            QVERIFY(window->grabWindow().save(qEnvironmentVariable("OMAWORD_SAVED_SCREENSHOT") + "-bookmarks.png"));
        click(item("savedTab1")); QTRY_COMPARE(window->property("collectionTab").toInt(),1); QTest::qWait(50);
        QVERIFY(shown("savedHighlight")); QVERIFY(item("savedHighlightJohn 3:16")->inherits("QQuickCanvasItem"));
        QVERIFY(!shown("savedBookmark")); QVERIFY(!shown("savedNote")); QVERIFY(!shown("savedNoteText"));
        if (qEnvironmentVariableIsSet("OMAWORD_SAVED_SCREENSHOT"))
            QVERIFY(window->grabWindow().save(qEnvironmentVariable("OMAWORD_SAVED_SCREENSHOT") + "-highlights.png"));
        click(item("savedTab2")); QTRY_COMPARE(window->property("collectionTab").toInt(),2); QTest::qWait(50);
        QVERIFY(shown("savedNote")); QVERIFY(shown("savedNoteText"));
        QCOMPARE(item("savedNoteTextJohn 3:16")->property("text").toString(),QString("Delete this exact note"));
        QVERIFY(!shown("savedBookmark")); QVERIFY(!shown("savedHighlight"));
        if (qEnvironmentVariableIsSet("OMAWORD_SAVED_SCREENSHOT"))
            QVERIFY(window->grabWindow().save(qEnvironmentVariable("OMAWORD_SAVED_SCREENSHOT") + "-notes.png"));
        click(item("savedTab0")); QTRY_COMPARE(window->property("collectionTab").toInt(),0);
        item("collectionList")->forceActiveFocus(); QTest::qWait(50);
        const auto before = bible->collection();
        const auto selection = bible->selectionReference();
        key(Qt::Key_Delete); QTRY_VERIFY(prop("deleteConfirmation","opened").toBool());
        QTRY_VERIFY(item("deleteCancel")->hasActiveFocus());
        QCOMPARE(prop("deleteReference","text").toString(),QString("John 3:16"));
        key(Qt::Key_J); key(Qt::Key_L); key(Qt::Key_Delete);
        QCOMPARE(prop("collectionList","currentIndex").toInt(),0);
        for (auto arrow : {Qt::Key_Left,Qt::Key_Up,Qt::Key_Right,Qt::Key_Down}) {
            key(arrow); QTRY_VERIFY(item("deleteConfirm")->hasActiveFocus());
            QCOMPARE(bible->collection(),before);
            QVERIFY(prop("deleteConfirmation","opened").toBool());
            key(arrow); QTRY_VERIFY(item("deleteCancel")->hasActiveFocus());
            QCOMPARE(bible->collection(),before);
        }
        key(Qt::Key_Return); QTRY_VERIFY(!prop("deleteConfirmation","visible").toBool());
        QCOMPARE(bible->collection(),before);
        key(Qt::Key_Backspace); QTRY_VERIFY(prop("deleteConfirmation","opened").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("deleteConfirmation","visible").toBool());
        QVERIFY(prop("savedDrawer","opened").toBool());
        QCOMPARE(bible->collection(),before);
        key(Qt::Key_Delete); QTRY_VERIFY(prop("deleteConfirmation","opened").toBool());
        key(Qt::Key_Tab); QTRY_VERIFY(item("deleteConfirm")->hasActiveFocus());
        key(Qt::Key_Up); QTRY_VERIFY(item("deleteCancel")->hasActiveFocus());
        key(Qt::Key_Down); QTRY_VERIFY(item("deleteConfirm")->hasActiveFocus());
        key(Qt::Key_Return); QTRY_VERIFY(!prop("deleteConfirmation","visible").toBool());
        QCOMPARE(bible->collection().size(),1);
        QCOMPARE(bible->selectionReference(),selection);
        QCOMPARE(bible->noteText(),QString());
        QCOMPARE(prop("noteEditor","text").toString(),QString());
        QCOMPARE(prop("collectionList","currentIndex").toInt(),-1);
        QVERIFY(bible->collection().first().toMap().value("note").toString().contains("overlapping"));
        click(item("savedTab2"));
        key(Qt::Key_Delete); QTRY_VERIFY(prop("deleteConfirmation","opened").toBool());
        click(item("deleteConfirm")); QTRY_VERIFY(!prop("deleteConfirmation","visible").toBool());
        QCOMPARE(prop("collectionList","currentIndex").toInt(),-1);
        QCOMPARE(prop("collectionList","count").toInt(),0);
        key(Qt::Key_Delete); QVERIFY(!prop("deleteConfirmation","visible").toBool());
    }
    void savedCollidingBookNames_data() {
        QTest::addColumn<bool>("tagsOnly");
        QTest::newRow("marks-notes-tags") << false;
        QTest::newRow("tags-only") << true;
    }
    void savedCollidingBookNames() {
        QFETCH(bool,tagsOnly);
        const auto source = session->filePath("colliding-names.zip");
        QVERIFY(writeSyntheticUsfm(source,"engojb",false,true));
        bible->importUsfm(QUrl::fromLocalFile(source));
        QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(),120000);
        QVERIFY2(bible->error().isEmpty(),qPrintable(bible->error()));
        QCOMPARE(bible->books().first().toMap().value("name").toString(),QString("John"));
        const auto tag = bible->createTag("Shared tag, distinct books");
        QVERIFY(!tag.isEmpty());
        QVariantMap genesis, john;
        for (int book : {1,43}) {
            bible->openChapter(book,3);
            bible->selectVerses(1,2);
            const auto scope = bible->tagScope();
            QVERIFY(bible->setTag(scope,tag,true));
            if (!tagsOnly) {
                QVERIFY(bible->toggleBookmark());
                QVERIFY(bible->toggleHighlight());
                bible->setNoteText(book == 1 ? "GEN note" : "JHN note");
            }
            if (book == 1) genesis = scope; else john = scope;
        }
        QCOMPARE(genesis.value("reference"),john.value("reference"));
        QCOMPARE(genesis.value("reference").toString(),QString("John 3:1–2"));
        QCOMPARE(bible->collection().size(),2);
        QCOMPARE(bible->collection()[0].toMap().value("book").toInt(),1);
        const auto johnEntry = bible->collection()[1].toMap();
        QCOMPARE(johnEntry.value("book").toInt(),43);
        // Canonical alias parsing really does choose JHN for this display name.
        QVERIFY(bible->navigate("John 3:1-2"));
        QCOMPARE(bible->book(),43);
        item("readerSurface")->forceActiveFocus();
        key(Qt::Key_A,Qt::ShiftModifier); QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        if (tagsOnly) {
            QCOMPARE(prop("collectionList","count").toInt(),0);
            key(Qt::Key_Escape); QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
            key(Qt::Key_T); QTRY_VERIFY(item("tagSearch")->hasActiveFocus());
            QVERIFY(QMetaObject::invokeMethod(object("tagsUi"),"toggleTag",Q_ARG(QVariant,tag)));
            QVERIFY(bible->tagsForScope(john).isEmpty());
            QCOMPARE(bible->tagsForScope(genesis).size(),1);
            QVERIFY(!item("tagResults")->isVisible());
            key(Qt::Key_Escape);
            QTRY_VERIFY(!prop("tagsUi","visibleModal").toBool());
            QCOMPARE(bible->tagScope(),john);
            QCOMPARE(bible->collection().size(),1);
            return;
        }
        object("collectionList")->setProperty("currentIndex",0);
        item("collectionList")->forceActiveFocus();
        key(Qt::Key_Return); QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
        QCOMPARE(bible->book(),1);
        QCOMPARE(bible->tagScope(),genesis);
        QCOMPARE(bible->noteText(),tagsOnly ? QString() : QString("GEN note"));
        if (!tagsOnly) {
            QTRY_VERIFY(prop("notesDrawer","opened").toBool());
            key(Qt::Key_Escape); QTRY_VERIFY(!prop("notesDrawer","visible").toBool());
        }
        key(Qt::Key_A,Qt::ShiftModifier); QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        object("collectionList")->setProperty("currentIndex",0);
        item("collectionList")->forceActiveFocus();
        key(Qt::Key_Delete); QTRY_VERIFY(prop("deleteConfirmation","opened").toBool());
        QCOMPARE(prop("deleteReference","text"),genesis.value("reference"));
        const auto value = prop("deleteConfirmation","scope");
        const auto captured = value.canConvert<QJSValue>() ? value.value<QJSValue>().toVariant().toMap() : value.toMap();
        QCOMPARE(captured,genesis);
        // A new model/list selection cannot retarget the already-open confirmation.
        object("collectionList")->setProperty("currentIndex",1);
        bible->refreshCollection();
        click(item("deleteConfirm")); QTRY_VERIFY(!prop("deleteConfirmation","visible").toBool());
        QCOMPARE(bible->collection(),QVariantList{johnEntry});
        QVERIFY(bible->tagsForScope(genesis).isEmpty());
        QCOMPARE(bible->tagsForScope(john).size(),1);
        QCOMPARE(bible->book(),1);
        QVERIFY(bible->noteText().isEmpty());
        // Disappearance of GEN invalidates its pending confirmation even though
        // JHN still supplies an identical display reference in the collection.
        QVERIFY(bible->setTag(genesis,tag,true));
        QVERIFY(bible->openTaggedPassage(genesis));
        QVERIFY(bible->toggleBookmark());
        object("collectionList")->setProperty("currentIndex",0);
        item("collectionList")->forceActiveFocus();
        key(Qt::Key_Delete); QTRY_VERIFY(prop("deleteConfirmation","opened").toBool());
        BibleBackend peer;
        QVERIFY(peer.deleteAnnotationScope(genesis));
        QVERIFY(QMetaObject::invokeMethod(window,"confirmDeleteSaved"));
        QTRY_VERIFY(!prop("deleteConfirmation","visible").toBool());
        QVERIFY(prop("deleteConfirmation","versionId").toString().isEmpty());
        QCOMPARE(bible->collection(),QVariantList{johnEntry});
    }
    void pendingDeleteVersionScope_data() {
        QTest::addColumn<bool>("peerRemoval");
        QTest::newRow("active-switch") << false;
        QTest::newRow("peer-removal") << true;
    }
    void pendingDeleteVersionScope() {
        QFETCH(bool,peerRemoval);
        // A synthetic second edition keeps this UI scope regression independent
        // of optional downloaded USFM and WindowManager's separate test target.
        const QString name = "bible-11111111-2222-3333-4444-555555555555.sqlite";
        const QString path = bible->dataDirectory() + '/' + name;
        const auto source = session->filePath("second_usfm.zip");
        QVERIFY(writeSyntheticUsfm(source,"eng-kjv2006"));
        const auto imported = UsfmImporter::importFile(source,path);
        QVERIFY2(imported.ok,qPrintable(imported.error));
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","ui-delete-library");
            db.setDatabaseName(bible->dataDirectory() + "/personal.sqlite");
            QVERIFY(db.open()); QSqlQuery q(db);
            q.prepare("INSERT INTO installed(version_id,path,abbreviation,name,copyright) VALUES('eng-kjv2006',?,'KJV',?,'')");
            q.addBindValue(name); q.addBindValue(imported.edition.name); QVERIFY(q.exec());
        }
        QSqlDatabase::removeDatabase("ui-delete-library");
        BibleBackend peer;
        QVERIFY2(peer.switchVersion("eng-kjv2006"),qPrintable(peer.error()));
        QVERIFY(peer.navigate("John 3:16"));
        peer.setNoteText("Keep KJV note"); peer.toggleBookmark(); peer.toggleHighlight();
        const auto kjv = peer.collection();
        reference("John 3:16");
        bible->setNoteText("NET deletion target"); bible->toggleBookmark();
        key(Qt::Key_A,Qt::ShiftModifier); QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        key(Qt::Key_Delete); QTRY_VERIFY(prop("deleteConfirmation","opened").toBool());
        QCOMPARE(prop("deleteConfirmation","versionId").toString(),QString("engnet"));
        QCOMPARE(prop("deleteConfirmation","reference").toString(),QString("John 3:16"));
        QSignalSpy commits(bible.get(),&BibleBackend::sharedCommitted);
        if (peerRemoval) QVERIFY2(peer.removeVersion("engnet",false),qPrintable(peer.error()));
        else QVERIFY2(bible->switchVersion("eng-kjv2006"),qPrintable(bible->error()));
        QCOMPARE(bible->versionId(),QString("eng-kjv2006"));
        commits.clear(); // Switching publishes library metadata; confirmation must not publish anything.
        QVERIFY(QMetaObject::invokeMethod(window,"confirmDeleteSaved")); // Even during close animation.
        QTRY_VERIFY(!prop("deleteConfirmation","visible").toBool());
        QVERIFY(prop("deleteConfirmation","versionId").toString().isEmpty());
        QVERIFY(prop("deleteConfirmation","reference").toString().isEmpty());
        QCOMPARE(commits.count(),0);
        QCOMPARE(bible->collection(),kjv); QCOMPARE(peer.collection(),kjv);
        QCOMPARE(bible->noteText(),QString("Keep KJV note"));
        if (!peerRemoval) {
            QVERIFY(bible->switchVersion("engnet"));
            QVERIFY(QMetaObject::invokeMethod(window,"confirmDeleteSaved"));
            QCOMPARE(bible->noteText(),QString("NET deletion target"));
        }
    }
    void pendingDeleteMissingCapture() {
        reference("John 3:16"); bible->setNoteText("Original target");
        window->setProperty("collectionTab",2);
        key(Qt::Key_A,Qt::ShiftModifier); QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        key(Qt::Key_Delete); QTRY_VERIFY(prop("deleteConfirmation","opened").toBool());
        BibleBackend peer;
        QVERIFY(peer.navigate("John 3:16"));
        QVERIFY(peer.deleteAnnotation("John 3:16"));
        peer.setNoteText("A newly created note at the same reference");
        const auto annotations = peer.collection();
        QVERIFY(QMetaObject::invokeMethod(window,"confirmDeleteSaved"));
        QTRY_VERIFY(!prop("deleteConfirmation","visible").toBool());
        QCOMPARE(bible->collection(),annotations);
        key(Qt::Key_Delete); QTRY_VERIFY(prop("deleteConfirmation","opened").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("deleteConfirmation","visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(window,"confirmDeleteSaved"));
        QCOMPARE(bible->collection(),annotations);
    }
    void savedDeleteFailureAndIndexClamping() {
        for (int verse = 1; verse <= 3; ++verse) {
            reference(QString("John 3:%1").arg(verse));
            bible->setNoteText(QString("Note %1").arg(verse));
        }
        key(Qt::Key_A,Qt::ShiftModifier); QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        key(Qt::Key_Right); key(Qt::Key_Right);
        QCOMPARE(window->property("collectionTab").toInt(),2);
        key(Qt::Key_J); key(Qt::Key_J);
        QCOMPARE(prop("collectionList","currentIndex").toInt(),2);
        const auto before = bible->collection();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","ui-delete-failure");
            db.setDatabaseName(bible->dataDirectory() + "/personal.sqlite");
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec("CREATE TRIGGER reject_delete BEFORE DELETE ON annotations BEGIN SELECT RAISE(FAIL,'test delete failure'); END"));
            key(Qt::Key_Delete); QTRY_VERIFY(prop("deleteConfirmation","opened").toBool());
            click(item("deleteConfirm"));
            QVERIFY(prop("deleteConfirmation","opened").toBool());
            QVERIFY(!bible->error().isEmpty());
            QCOMPARE(bible->collection(),before);
            QCOMPARE(prop("collectionList","currentIndex").toInt(),2);
            QCOMPARE(bible->noteText(),QString("Note 3"));
            QVERIFY(query.exec("DROP TRIGGER reject_delete"));
            click(item("deleteConfirm"));
            QTRY_VERIFY(!prop("deleteConfirmation","visible").toBool());
        }
        QSqlDatabase::removeDatabase("ui-delete-failure");
        QCOMPARE(prop("collectionList","currentIndex").toInt(),1);
        QCOMPARE(bible->collection().size(),2);
        QCOMPARE(bible->noteText(),QString());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
        key(Qt::Key_A,Qt::ShiftModifier); QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        QCOMPARE(prop("collectionList","currentIndex").toInt(),1);
    }
    void referencePreviewIsPure() {
        reference("John 3:16");
        bible->setNoteText("Keep this exact note");
        bible->toggleBookmark();
        QTest::qWait(450);
        const auto position = bible->readingPosition();
        const auto collection = bible->collection();
        QSignalSpy navigation(bible.get(),&BibleBackend::navigationChanged);
        QSignalSpy selection(bible.get(),&BibleBackend::selectionChanged);
        QSignalSpy errors(bible.get(),&BibleBackend::errorChanged);
        key(Qt::Key_K,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        for (const auto &query : {QString("rev4"),QString("John 3:"),QString("John 300:999"),QString("4:2")}) {
            key(Qt::Key_A,Qt::ControlModifier); type(query);
            QVERIFY(item("referencePreview")->isVisible());
            const auto preview = prop("referencePreview","text").toString();
            if (query == "rev4") QCOMPARE(preview,QString("Revelation 4"));
            else if (query == "4:2") QCOMPARE(preview,QString("John 4:2"));
            else {
                QCOMPARE(preview,bible->resolveReference(query).value("error").toString());
                key(Qt::Key_Return);
                QVERIFY(prop("passagePalette","visible").toBool());
            }
            QCOMPARE(bible->selectionReference(),QString("John 3:16"));
            QCOMPARE(bible->noteText(),QString("Keep this exact note"));
            QCOMPARE(bible->readingPosition(),position);
            QCOMPARE(bible->collection(),collection);
            QVERIFY(bible->error().isEmpty());
        }
        QCOMPARE(navigation.count(),0); QCOMPARE(selection.count(),0); QCOMPARE(errors.count(),0);
        key(Qt::Key_A,Qt::ControlModifier); type("rev4"); key(Qt::Key_Return);
        QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        QCOMPARE(bible->passage(),QString("Revelation 4"));
    }
    void rightArrowBrowsingAndEditing() {
        bible->setVersePicker(false);
        reference("John 3:16");
        key(Qt::Key_K,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        key(Qt::Key_Down); key(Qt::Key_Right);
        QCOMPARE(window->property("paletteBook").toInt(),2);
        QCOMPARE(window->property("paletteBookName").toString(),QString("Exodus"));
        QCOMPARE(bible->selectionReference(),QString("John 3:16"));
        type("3");
        QCOMPARE(prop("referenceInput","text").toString(),QString("3"));
        QCOMPARE(prop("referencePreview","text").toString(),QString("Exodus 3"));
        key(Qt::Key_Return);
        QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        QCOMPARE(bible->passage(),QString("Exodus 3"));
        key(Qt::Key_K,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("John");
        key(Qt::Key_Home); key(Qt::Key_Right);
        QCOMPARE(prop("referenceInput","cursorPosition").toInt(),1);
        QCOMPARE(window->property("paletteBook").toInt(),0);
        key(Qt::Key_End); key(Qt::Key_Right);
        QCOMPARE(window->property("paletteBook").toInt(),43);
        key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        key(Qt::Key_K,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("1 John 3:16");
        key(Qt::Key_Home); key(Qt::Key_Right);
        QCOMPARE(prop("referenceInput","cursorPosition").toInt(),1);
        key(Qt::Key_End); key(Qt::Key_Right);
        QCOMPARE(window->property("paletteBook").toInt(),0);
        QCOMPARE(prop("referenceInput","text").toString(),QString("1 John 3:16"));
        QCOMPARE(bible->passage(),QString("Exodus 3"));
        key(Qt::Key_A,Qt::ControlModifier); type("1 John");
        key(Qt::Key_Right);
        QCOMPARE(window->property("paletteBook").toInt(),62);
    }
    void paletteBackKeyboardAndEditing() {
        reference("John 3:16");
        QSignalSpy navigation(bible.get(),&BibleBackend::navigationChanged);
        key(Qt::Key_K,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("John"); key(Qt::Key_Return);
        key(Qt::Key_Up);
        QVERIFY(item("paletteBack")->hasActiveFocus());
        QVERIFY(prop("paletteBack","emphasized").toBool());
        QCOMPARE(prop("paletteResults","currentIndex").toInt(),-1);
        key(Qt::Key_Down);
        QVERIFY(item("referenceInput")->hasActiveFocus());
        QCOMPARE(prop("paletteResults","currentIndex").toInt(),0);
        key(Qt::Key_Up); key(Qt::Key_Return);
        QCOMPARE(window->property("paletteBook").toInt(),0);
        QVERIFY(item("referenceInput")->hasActiveFocus());
        type("John"); key(Qt::Key_Right); key(Qt::Key_Left);
        QCOMPARE(window->property("paletteBook").toInt(),0);
        type("John"); key(Qt::Key_Return);
        type("3:16"); key(Qt::Key_Left);
        QCOMPARE(prop("referenceInput","cursorPosition").toInt(),3);
        QCOMPARE(window->property("paletteBook").toInt(),43);
        key(Qt::Key_A,Qt::ControlModifier); key(Qt::Key_Left);
        QCOMPARE(window->property("paletteBook").toInt(),43);
        QCOMPARE(prop("referenceInput","cursorPosition").toInt(),0);
        key(Qt::Key_Left);
        QCOMPARE(window->property("paletteBook").toInt(),0);
        type("John"); key(Qt::Key_Return); type("1"); key(Qt::Key_Down);
        // Once navigating results, Left backs out even with a query present.
        key(Qt::Key_Left);
        QCOMPARE(window->property("paletteBook").toInt(),0);
        type("John"); key(Qt::Key_Return); key(Qt::Key_Up); key(Qt::Key_Left);
        QCOMPARE(window->property("paletteBook").toInt(),0);
        QCOMPARE(navigation.count(),0);
        QCOMPARE(bible->selectionReference(),QString("John 3:16"));
        key(Qt::Key_A,Qt::ControlModifier); type("not a reference 123");
        QCOMPARE(prop("referencePreview","text").toString(),bible->resolveReference("not a reference 123").value("error").toString());
        QVERIFY(prop("referencePreview","text").toString().startsWith("Reference not recognized"));
    }
    void paletteInlineGeometryAndHints_data() {
        QTest::addColumn<double>("scale");
        QTest::newRow("normal") << 1.0;
        QTest::newRow("scaled") << 1.8;
    }
    void paletteInlineGeometryAndHints() {
        QFETCH(double,scale);
        window->resize(640,480);
        QVERIFY(desktop->setProperty("textScale",scale));
        key(Qt::Key_K,Qt::ControlModifier);
        QTRY_VERIFY(prop("passagePalette","opened").toBool());
        for (const auto &mode : {QString("books"),QString("chapters"),QString("text"),QString("commands")}) {
            if (mode == "chapters") { type("John"); key(Qt::Key_Return); }
            if (mode == "text") { click(item("paletteBooks")); type("/"); }
            if (mode == "commands") click(item("paletteCommands"));
            QTest::qWait(100);
            auto input = item("referenceInput");
            const auto inputBounds = input->mapRectToScene(QRectF(0,0,input->width(),input->height()));
            QVERIFY(input->width() > 100);
            if (mode == "text") {
                QVERIFY(window->property("paletteTextMode").toBool());
                QVERIFY(!item("referencePreview")->isVisible());
                QVERIFY(!item("paletteBack")->isVisible());
                QCOMPARE(prop("referenceInput","text").toString(),QString("/ "));
                QVERIFY(prop("paletteSearch","emphasized").toBool());
                QVERIFY(!prop("paletteBooks","emphasized").toBool());
            }
            QRectF previous;
            const auto tabs = item("paletteSearch")->parentItem();
            QVERIFY(tabs->property("uniformCellSizes").toBool());
            const double tabWidth = (inputBounds.width() - 2 * tabs->property("spacing").toDouble()) / 3;
            for (const auto name : {"paletteBooks","paletteSearch","paletteCommands"}) {
                const auto button = item(name);
                QCOMPARE(button->property("text").toString(),QString(name == QString("paletteBooks") ? "Passage"
                    : name == QString("paletteSearch") ? "Search" : "Command"));
                const auto bounds = button->mapRectToScene(button->boundingRect());
                QVERIFY(bounds.top() > inputBounds.bottom());
                QVERIFY(qAbs(bounds.width() - tabWidth) <= 1);
                if (previous.isNull()) QCOMPARE(bounds.left(),inputBounds.left());
                else {
                    QCOMPARE(bounds.top(),previous.top());
                    QCOMPARE(bounds.height(),previous.height());
                    QVERIFY(bounds.left() > previous.right());
                }
                QVERIFY(QRectF(16,16,608,448).contains(bounds));
                const auto label = qvariant_cast<QQuickItem *>(button->property("contentItem"));
                QVERIFY(label->width() >= label->implicitWidth());
                previous = bounds;
            }
            QVERIFY(qAbs(previous.right() - inputBounds.right()) <= 1);
            QVERIFY(QRectF(16,16,608,448).contains(inputBounds));
            QVERIFY(item("paletteResults")->height() >= 40);
            const auto hints = item("paletteHints");
            QVERIFY(QRectF(16,16,608,448).contains(hints->mapRectToScene(hints->boundingRect())));
            const auto text = plainHint(hints->property("text").toString());
            if (mode == "commands") {
                QCOMPARE(text,QString::fromUtf8("↑ ↓ choose    enter select    esc close"));
                QVERIFY(!item("paletteBack")->isVisible());
            } else if (mode == "chapters") {
                QCOMPARE(text,QString::fromUtf8("↑ ↓ choose    enter select    ← back    esc close"));
            } else QCOMPARE(text,QString::fromUtf8("↑ ↓ choose    enter select    esc close"));
        }
    }
    void normalVisualAndShiftedSelection() {
        reference("John 3:16");
        for (auto down : {Qt::Key_J,Qt::Key_Down}) {
            key(down);
            QCOMPARE(bible->selectedStart(),17); QCOMPARE(bible->selectedEnd(),17);
            QVERIFY(!window->property("verseMode").toBool());
            key(down == Qt::Key_J ? Qt::Key_K : Qt::Key_Up);
            QCOMPARE(bible->cursorVerse(),16);
        }
        key(Qt::Key_V);
        QVERIFY(window->property("verseMode").toBool());
        key(Qt::Key_J); key(Qt::Key_Down);
        QCOMPARE(bible->selectedStart(),16); QCOMPARE(bible->selectedEnd(),18);
        key(Qt::Key_K);
        QCOMPARE(bible->selectedStart(),16); QCOMPARE(bible->selectedEnd(),17);
        key(Qt::Key_Up); key(Qt::Key_Up);
        QCOMPARE(bible->selectedStart(),15); QCOMPARE(bible->selectedEnd(),16);
        QCOMPARE(bible->cursorVerse(),15);
        key(Qt::Key_V);
        QVERIFY(!window->property("verseMode").toBool());
        QCOMPARE(bible->selectedStart(),15); QCOMPARE(bible->selectedEnd(),15);
        for (auto shifted : {Qt::Key_J,Qt::Key_K,Qt::Key_Down,Qt::Key_Up}) {
            reference("John 3:16");
            key(shifted,Qt::ShiftModifier);
            QVERIFY(window->property("verseMode").toBool());
            const bool down = shifted == Qt::Key_J || shifted == Qt::Key_Down;
            QCOMPARE(bible->cursorVerse(),down ? 17 : 15);
            QCOMPARE(bible->selectedStart(),down ? 16 : 15);
            QCOMPARE(bible->selectedEnd(),down ? 17 : 16);
            key(Qt::Key_Escape);
            QVERIFY(!window->property("verseMode").toBool());
            QCOMPARE(bible->selectedStart(),bible->cursorVerse());
            QCOMPARE(bible->selectedEnd(),bible->cursorVerse());
        }
        reference("John 3:16-18");
        key(Qt::Key_V); key(Qt::Key_K);
        QCOMPARE(bible->selectedStart(),17); QCOMPARE(bible->selectedEnd(),18);
        reference("John 3");
        key(Qt::Key_V); key(Qt::Key_J);
        QCOMPARE(bible->selectedStart(),1); QCOMPARE(bible->selectedEnd(),2);
    }
    void visualActionsCompleteFullRange_data() {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<Qt::Key>("action");
        for (const auto &mode : {QString("verse"),QString("block"),QString("paragraph")})
            for (const auto action : {Qt::Key_M,Qt::Key_X,Qt::Key_Y})
                QTest::newRow(qPrintable(mode + QString::number(action))) << mode << action;
    }
    void visualActionsCompleteFullRange() {
        QFETCH(QString,mode); QFETCH(Qt::Key,action);
        bible->setReadingMode(mode);
        reference("John 3:35");
        key(Qt::Key_V); type("3j");
        const auto reference = bible->selectionReference();
        QCOMPARE(reference,QString("John 3:35–4:2"));
        const auto scope = bible->tagScope();
        const auto tag = bible->createAndAssignTag(scope,"Keep <b>literal</b> 🌿");
        QVERIFY(!tag.isEmpty());
        bible->setNoteText("Existing full-range note");
        key(action);
        QVERIFY(!window->property("verseMode").toBool());
        QCOMPARE(bible->selectionReference(),QString("John 4:2"));
        QVERIFY(prop("toast","text").toString() != "Visual selection off");
        if (action == Qt::Key_Y) QVERIFY(QApplication::clipboard()->text().contains(reference));
        QVERIFY(bible->openTaggedPassage(scope));
        QCOMPARE(bible->noteText(),QString("Existing full-range note"));
        QCOMPARE(bible->selectionTags().size(),1);
        if (action == Qt::Key_M) QVERIFY(bible->selectionBookmarked());
        if (action == Qt::Key_X) QVERIFY(bible->selectionHighlighted());
    }
    void visualFailedMarksAndPinnedNotes() {
        reference("John 3:16");
        key(Qt::Key_V); key(Qt::Key_J);
        const auto scope = bible->tagScope();
        const auto reference = bible->selectionReference();
        const auto tag = bible->createAndAssignTag(scope,"Retained tag");
        QVERIFY(!tag.isEmpty());
        QVERIFY(bible->toggleBookmark());
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","ui-visual-failure");
            db.setDatabaseName(bible->dataDirectory() + "/personal.sqlite");
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec("CREATE TRIGGER reject_visual BEFORE DELETE ON annotations BEGIN SELECT RAISE(ABORT,'visual save failure'); END"));
            const auto toast = prop("toast","text");
            for (const auto action : {Qt::Key_M,Qt::Key_X}) {
                key(action);
                QVERIFY(bible->error().contains("visual save failure"));
                QVERIFY(window->property("verseMode").toBool());
                QCOMPARE(bible->selectionReference(),reference);
                QVERIFY(bible->selectionBookmarked());
                QVERIFY(!bible->selectionHighlighted());
                QCOMPARE(prop("toast","text"),toast);
            }
            key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
            type("First saved draft");
            QVERIFY(window->property("verseMode").toBool());
            QVERIFY(bible->noteText().isEmpty());
            key(Qt::Key_Escape);
            QVERIFY(prop("notesDrawer","visible").toBool());
            QVERIFY(query.exec("DROP TRIGGER reject_visual"));
            QVERIFY(QMetaObject::invokeMethod(window,"flushNote"));
            QVERIFY(!window->property("verseMode").toBool());
            QCOMPARE(bible->selectionReference(),reference);
            QCOMPARE(bible->noteText(),QString("First saved draft"));
            type(" extended");
            QCOMPARE(bible->selectionReference(),reference);
            QCOMPARE(bible->noteText(),QString("First saved draft extended"));
            QCOMPARE(prop("toast","text"),toast);
            key(Qt::Key_Escape); QTRY_VERIFY(!prop("notesDrawer","visible").toBool());
            QCOMPARE(bible->selectionReference(),QString("John 3:17"));
        }
        QSqlDatabase::removeDatabase("ui-visual-failure");
        QVERIFY(bible->openTaggedPassage(scope));
        QCOMPARE(bible->noteText(),QString("First saved draft extended"));
        QVERIFY(bible->selectionBookmarked());
        QCOMPARE(bible->selectionTags().size(),1);
        // Opening and canceling without a write leaves Visual and the range intact.
        item("readerSurface")->forceActiveFocus();
        key(Qt::Key_V); key(Qt::Key_K);
        const auto canceled = bible->selectionReference();
        key(Qt::Key_W); QTRY_VERIFY(prop("notesDrawer","opened").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("notesDrawer","visible").toBool());
        QVERIFY(window->property("verseMode").toBool());
        QCOMPARE(bible->selectionReference(),canceled);
    }
    void visualTagPinAndStalePanel() {
        reference("John 3:16");
        key(Qt::Key_V); key(Qt::Key_J);
        const auto scope = bible->tagScope();
        const auto reference = bible->selectionReference();
        const auto first = bible->createTag("First <b>tag</b>");
        const auto second = bible->createTag("Second 🌿");
        QVERIFY(!first.isEmpty() && !second.isEmpty());
        key(Qt::Key_T);
        QTRY_VERIFY(prop("tagsUi","scopePinned").toBool());
        QVERIFY(window->property("verseMode").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("tagsUi","scopePinned").toBool());
        QVERIFY(window->property("verseMode").toBool());
        QCOMPARE(bible->selectionReference(),reference);
        key(Qt::Key_T); QTRY_VERIFY(prop("tagsUi","scopePinned").toBool());
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","ui-tag-failure");
            db.setDatabaseName(bible->dataDirectory() + "/personal.sqlite");
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec("CREATE TRIGGER reject_tag BEFORE INSERT ON tag_assignments BEGIN SELECT RAISE(ABORT,'tag save failure'); END"));
            QVERIFY(QMetaObject::invokeMethod(object("tagsUi"),"toggleTag",Q_ARG(QVariant,first)));
            QVERIFY(window->property("verseMode").toBool());
            QCOMPARE(bible->selectionReference(),reference);
            QVERIFY(bible->selectionTags().isEmpty());
            QVERIFY(query.exec("DROP TRIGGER reject_tag"));
        }
        QSqlDatabase::removeDatabase("ui-tag-failure");
        QVERIFY(QMetaObject::invokeMethod(object("tagsUi"),"toggleTag",Q_ARG(QVariant,first)));
        QVERIFY(!window->property("verseMode").toBool());
        QCOMPARE(bible->selectionReference(),reference);
        QVERIFY(QMetaObject::invokeMethod(object("tagsUi"),"toggleTag",Q_ARG(QVariant,second)));
        QCOMPARE(bible->selectionTags().size(),2);
        QCOMPARE(bible->selectionReference(),reference);
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("tagsUi","scopePinned").toBool());
        QCOMPARE(bible->selectionReference(),QString("John 3:17"));
        QVERIFY(bible->openTaggedPassage(scope));
        item("readerSurface")->forceActiveFocus();
        key(Qt::Key_V); key(Qt::Key_K);
        key(Qt::Key_T); QTRY_VERIFY(prop("tagsUi","scopePinned").toBool());
        // A context change while the picker is open must not complete or collapse
        // a new range, even if it later returns to the same displayed reference.
        QVERIFY(bible->navigate("John 4:1–3"));
        QVERIFY(bible->openTaggedPassage(scope));
        window->setProperty("verseMode",true);
        QVERIFY(QMetaObject::invokeMethod(object("tagsUi"),"toggleTag",Q_ARG(QVariant,first)));
        QVERIFY(window->property("verseMode").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("tagsUi","scopePinned").toBool());
        QCOMPARE(bible->selectionReference(),reference);
        QCOMPARE(bible->selectionTags().size(),1);
    }
    void verseTagMarkersAndSavedTagOnly() {
        reference("John 3:1-2");
        const auto first = bible->tagScope();
        const auto tag = bible->createAndAssignTag(first,"<b>Faith & 🌿</b>");
        QVERIFY(!tag.isEmpty());
        QVERIFY(bible->navigate("John 3:1–3"));
        const auto second = bible->tagScope();
        QVERIFY(bible->setTag(second,tag,true));
        QVERIFY(bible->navigate("John 3:5"));
        QTest::qWait(150);
        QVERIFY(QMetaObject::invokeMethod(window,"scrollToVerse",Q_ARG(QVariant,1),Q_ARG(QVariant,true)));
        QTRY_VERIFY(!object("readerSlide")->property("running").toBool());
        QTRY_VERIFY(item("verseTagMarker1") && item("verseTagMarker1")->isVisible());
        QCOMPARE(prop("verseTagMarker1","entries").value<QJSValue>().toVariant().toList().size(),2);
        click(item("verseTagMarker1"));
        QTRY_VERIFY(prop("verseTagMenu","opened").toBool());
        const auto action = item("verseTagAction:John 3:1–3");
        QVERIFY(action);
        QCOMPARE(action->property("contentItem").value<QQuickItem *>()->property("textFormat").toInt(),0);
        click(action);
        QTRY_VERIFY(prop("tagsUi","scopePinned").toBool());
        QCOMPARE(bible->selectionReference(),QString("John 3:5")); // Marker edits its own frozen range.
        const auto value = prop("tagsUi","editingScope");
        const auto opened = value.canConvert<QJSValue>() ? value.value<QJSValue>().toVariant().toMap() : value.toMap();
        QCOMPARE(opened["start"],second["start"]); QCOMPARE(opened["end"],second["end"]);
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("tagsUi","scopePinned").toBool());
        QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
        key(Qt::Key_A,Qt::ShiftModifier); QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        QCOMPARE(prop("collectionList","count").toInt(),0);
        QCOMPARE(bible->collection().size(),2);
        click(item("savedTab3"));
        QTRY_COMPARE(window->property("collectionTab").toInt(),3);
        QVERIFY(!prop("tagsUi","visibleModal").toBool());
        QVERIFY(prop("savedDrawer","visible").toBool());
        // Annotations → Tags lists the tagged range (once per tag) in the shared list.
        QTRY_VERIFY(prop("collectionList","count").toInt() > 0);
        QVERIFY(item("collectionList")->isVisible());
        QVERIFY(!prop("tagsUi","scopePinned").toBool());
        QVERIFY(!item("tagSearch")->isVisible());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
        key(Qt::Key_H,Qt::AltModifier); QVERIFY(!bible->showHeadings());
        key(Qt::Key_S, Qt::ShiftModifier);
        QTRY_VERIFY(prop("tagsUi","visibleModal").toBool());
        QTRY_VERIFY(prop("relatedDrawer","opened").toBool());
        QTRY_VERIFY(item("studyList")->hasActiveFocus());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("tagsUi","visibleModal").toBool());
        key(Qt::Key_Space,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("manage tags"); key(Qt::Key_Return);
        QTRY_VERIFY(prop("tagsUi","visibleModal").toBool());
        QVERIFY(!prop("savedDrawer","visible").toBool());
        QCOMPARE(prop("tagsUi","page").toString(),QString("manage"));
    }
    void palettePreviewMatchesAcceptance() {
        bible->setVersePicker(false);
        QString psalmName;
        for (const auto &entry : bible->books())
            if (entry.toMap().value("id").toInt() == 19) psalmName = entry.toMap().value("name").toString();
        QVERIFY(!psalmName.isEmpty());
        reference("Genesis 1:1");
        QSignalSpy navigation(bible.get(),&BibleBackend::navigationChanged);
        key(Qt::Key_K,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("John"); key(Qt::Key_Return);
        QCOMPARE(window->property("paletteBook").toInt(),43);
        type("3:16");
        QCOMPARE(prop("referencePreview","text").toString(),QString("John 3:16"));
        QCOMPARE(navigation.count(),0);
        QCOMPARE(bible->selectionReference(),QString("Genesis 1:1"));
        key(Qt::Key_Return);
        QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        QCOMPARE(bible->selectionReference(),QString("John 3:16"));
        navigation.clear();
        key(Qt::Key_K,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("Psalms"); key(Qt::Key_Right);
        QCOMPARE(window->property("paletteBook").toInt(),19);
        type("1");
        QCOMPARE(prop("referencePreview","text").toString(),psalmName + " 1");
        key(Qt::Key_Down);
        QCOMPARE(prop("referencePreview","text").toString(),psalmName + " 10");
        QCOMPARE(navigation.count(),0);
        QCOMPARE(bible->selectionReference(),QString("John 3:16"));
        key(Qt::Key_Return);
        QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        QCOMPARE(bible->book(),19); QCOMPARE(bible->chapter(),10);
        key(Qt::Key_K,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("Psalms"); key(Qt::Key_Return); type("1"); key(Qt::Key_Down);
        QCOMPARE(prop("referencePreview","text").toString(),psalmName + " 10");
        // Editing resets the list override and restores the typed destination.
        type(":2");
        QCOMPARE(prop("referencePreview","text").toString(),psalmName + " 1:2");
        // Arrows with no matching list entry must not discard a valid typed reference.
        key(Qt::Key_Down);
        QCOMPARE(prop("referencePreview","text").toString(),psalmName + " 1:2");
        key(Qt::Key_Return);
        QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        QCOMPARE(bible->chapter(),1); QCOMPARE(bible->cursorVerse(),2);
    }
    void failedCopyDoesNotNotifySuccess() {
        reference("John 3:36-4:1");
        window->setProperty("verseMode",true);
        const auto reference = bible->selectionReference();
        const auto verses = bible->verses();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","ui-copy-failure");
            db.setDatabaseName(installedPath());
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec("ALTER TABLE verses RENAME TO saved_verses"));
            QVERIFY(query.exec("CREATE VIEW verses AS SELECT book,chapter,number,CASE WHEN book=43 AND chapter=4 THEN abs(-9223372036854775808) ELSE text END AS text,html,heading,poetry FROM saved_verses"));
            QApplication::clipboard()->setText("Preserve clipboard on partial read failure");
            key(Qt::Key_Y);
            QVERIFY(!bible->error().isEmpty());
            QVERIFY(item("errorMessage")->isVisible());
            QCOMPARE(QApplication::clipboard()->text(),QString("Preserve clipboard on partial read failure"));
            QVERIFY(!item("toast")->isVisible());
            QVERIFY(!prop("toast","text").toString().contains("copied"));
            QCOMPARE(bible->verses(),verses);
            QVERIFY(window->property("verseMode").toBool());
            QCOMPARE(bible->selectionReference(),reference);
            QVERIFY(query.exec("DROP VIEW verses"));
            QVERIFY(query.exec("ALTER TABLE saved_verses RENAME TO verses"));
        }
        QSqlDatabase::removeDatabase("ui-copy-failure");
        key(Qt::Key_Y);
        QVERIFY(bible->error().isEmpty());
        QVERIFY(item("toast")->isVisible());
        QCOMPARE(prop("toast","text").toString(),reference + " copied");
        QVERIFY(QApplication::clipboard()->text().contains(reference));
        QVERIFY(!window->property("verseMode").toBool());
        QCOMPARE(bible->selectedStart(),bible->cursorVerse());
    }
    void chapterBoundarySelection_data() {
        QTest::addColumn<bool>("forward");
        QTest::newRow("next-first") << true;
        QTest::newRow("previous-last") << false;
    }
    void chapterBoundarySelection() {
        QFETCH(bool,forward);
        const QString origin = forward ? "John 3:36" : "John 4:1";
        const auto direction = forward ? Qt::Key_J : Qt::Key_K;
        const auto reverse = forward ? Qt::Key_K : Qt::Key_J;
        reference(origin);
        key(direction);
        QCOMPARE(bible->chapter(),forward ? 4 : 3);
        QCOMPARE(bible->cursorVerse(),forward ? 1 : 36);
        QCOMPARE(bible->selectedStart(),bible->selectedEnd());
        QVERIFY(!bible->crossChapterSelection());
        const auto cursorVisible = [&] {
            auto verse = item(qPrintable("verseNumber" + QString::number(bible->cursorVerse())));
            if (!verse) return false;
            const double y = verse->mapToItem(item("reading"),QPointF()).y();
            return y >= 0 && y < item("reading")->height();
        };
        QTRY_VERIFY(cursorVisible());
        reference(origin);
        key(direction,Qt::ShiftModifier);
        QVERIFY(bible->crossChapterSelection());
        const QString span = QString::fromUtf8("John 3:36–4:1");
        QCOMPARE(bible->selectionReference(),span);
        QTRY_VERIFY(cursorVisible());
        // Reverse movement contracts toward the canonical anchor, not the
        // visible chapter intersection, and keeps the active endpoint in view.
        key(reverse);
        QCOMPARE(bible->selectionReference(),origin);
        key(direction); key(direction);
        QCOMPARE(bible->cursorVerse(),forward ? 2 : 35);
        QTRY_VERIFY(cursorVisible());
        key(reverse);
        QCOMPARE(bible->selectionReference(),span);
        QTRY_VERIFY(cursorVisible());
        key(Qt::Key_X);
        QVERIFY(!window->property("verseMode").toBool());
        QCOMPARE(bible->selectedStart(),bible->cursorVerse());
        reference("John 3:36-4:1");
        key(Qt::Key_M);
        QVERIFY(bible->selectionHighlighted()); QVERIFY(bible->selectionBookmarked());
        key(Qt::Key_Y);
        const QString copied = QApplication::clipboard()->text();
        QVERIFY(copied.contains(span));
        const auto lines = copied.split('\n');
        // The chapter boundary is a source paragraph break, which the
        // quotation preserves as a blank line between the two verses.
        QCOMPARE(lines.size(),5);
        QVERIFY(lines[0].startsWith(QString::fromUtf8("“³⁶ ")));
        QVERIFY(!lines[0].endsWith(QChar(0x201d)));
        QVERIFY(lines[1].isEmpty());
        QVERIFY(lines[2].startsWith(QString::fromUtf8("¹ ")));
        QVERIFY(lines[2].endsWith(QChar(0x201d)));
        QVERIFY(lines[3].isEmpty());
        QCOMPARE(lines[4],QString::fromUtf8("—") + span);
        // Re-enter Visual for the same complete range before editing the note.
        reference(origin);
        key(direction,Qt::ShiftModifier);
        key(Qt::Key_W);
        QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        const QString note = "An exact cross-chapter note j k h l x v";
        type(note);
        QCOMPARE(bible->noteText(),note);
        key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("notesDrawer","visible").toBool());
        QVERIFY(!window->property("verseMode").toBool());
        QVERIFY(!window->property("notesOpen").toBool());
        QVERIFY(!bible->crossChapterSelection());
        QCOMPARE(bible->selectedStart(),bible->cursorVerse());
        QCOMPARE(bible->selectedEnd(),bible->cursorVerse());
        QCOMPARE(prop("noteEditor","text").toString(),QString());
        // Reopening from storage and then the drawer must recover the entire
        // range, its exact note, and flags on both sides of the boundary.
        unloadUi(); loadUi();
        QCOMPARE(bible->collection().size(),1);
        QCOMPARE(bible->collection().first().toMap().value("reference").toString(),span);
        key(Qt::Key_A,Qt::ShiftModifier);
        QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        const QByteArray name = ("savedEntry" + span).toUtf8();
        QTRY_VERIFY(item(name.constData()));
        click(item(name.constData()));
        QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
        QCOMPARE(bible->selectionReference(),span);
        QVERIFY(bible->crossChapterSelection());
        QVERIFY(bible->selectionHighlighted()); QVERIFY(bible->selectionBookmarked());
        QCOMPARE(prop("noteEditor","text").toString(),note);
        key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("notesDrawer","visible").toBool());
        key(Qt::Key_Y); QCOMPARE(QApplication::clipboard()->text(),copied);
        reference("John 3:36"); QVERIFY(bible->selectionHighlighted()); QVERIFY(bible->noteText().isEmpty());
        reference("John 4:1"); QVERIFY(bible->selectionHighlighted()); QVERIFY(bible->noteText().isEmpty());
    }
    void mouseAnchorAfterCrossChapterMovement() {
        reference("John 4:1");
        key(Qt::Key_K,Qt::ShiftModifier);
        QVERIFY(bible->crossChapterSelection());
        QTRY_VERIFY(item("verseNumber35"));
        click(item("verseNumber35"),Qt::ShiftModifier);
        QCOMPARE(bible->selectionReference(),QString::fromUtf8("John 3:35–36"));
        QCOMPARE(bible->cursorVerse(),35);
        QVERIFY(!bible->crossChapterSelection());
        key(Qt::Key_J);
        QCOMPARE(bible->selectionReference(),QString("John 3:36"));
    }
    void annotationKeysAndNoteIsolation() {
        reference("John 3:16");
        key(Qt::Key_J); QCOMPARE(bible->selectedStart(),17);
        key(Qt::Key_K); QCOMPARE(bible->selectedStart(),16);
        key(Qt::Key_X); QVERIFY(bible->selectionHighlighted());
        key(Qt::Key_M); QVERIFY(bible->selectionBookmarked());
        key(Qt::Key_W);
        QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        const QString first = "# John 3\nj k h l x m v y g are note text, not reader commands.";
        type(first);
        QCOMPARE(prop("noteEditor", "text").toString(),first);
        QCOMPARE(bible->noteText(),first);
        QCOMPARE(bible->selectedStart(),16);
        QVERIFY(bible->selectionHighlighted()); QVERIFY(bible->selectionBookmarked());
        QVERIFY(!window->property("verseMode").toBool());
        QCOMPARE(prop("noteEditor", "cursorPosition").toInt(), first.size());
        // Standard undo must stay inside the editor too.
        type("!"); key(Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(!bible->noteText().endsWith('!'));
        key(Qt::Key_A, Qt::ControlModifier); type(first);
        key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("notesDrawer","visible").toBool());
        key(Qt::Key_J); QCOMPARE(bible->selectedStart(),17);
        QCOMPARE(prop("noteEditor", "text").toString(),QString());
        key(Qt::Key_W);
        QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        const QString second = "A different note for the next verse.";
        type(second);
        // Navigation from the Notes panel must synchronously finish the correct note.
        item("notesPanel")->forceActiveFocus();
        key(Qt::Key_K, Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("John3:16"); key(Qt::Key_Return);
        QTRY_VERIFY(!prop("passagePalette", "visible").toBool());
        QCOMPARE(bible->noteText(),first);
        QCOMPARE(prop("noteEditor", "text").toString(),first);
        bible->refreshCollection();
        QCOMPARE(saved(16).value("note").toString(),first);
        QCOMPARE(saved(17).value("note").toString(),second);
        click(item("previewButton"));
        QVERIFY(window->property("notePreview").toBool());
        QVERIFY(item("notePreview")->isVisible());
        QVERIFY(prop("notePreview","readOnly").toBool());
        // TextEdit serializes parsed Markdown, so compare rendered plain text,
        // not the normalized Markdown returned by its text property.
        QString preview;
        QVERIFY(QMetaObject::invokeMethod(object("notePreview"),"getText",Q_RETURN_ARG(QString,preview),
            Q_ARG(int,0),Q_ARG(int,prop("notePreview","length").toInt())));
        QVERIFY(preview.contains("John 3"));
        QVERIFY(preview.contains("j k h l x m v y g are note text"));
        QCOMPARE(bible->noteText(),first);
        // Recreate both C++ and QML objects to prove the notes reached storage.
        unloadUi(); loadUi();
        QCOMPARE(bible->noteText(),first);
        QCOMPARE(saved(17).value("note").toString(),second);
    }
    void clickRangeAndCollections() {
        reference("John 3:16");
        click(item("verseNumber16"));
        QCOMPARE(bible->selectedStart(),16);
        click(item("verseNumber17"),Qt::ShiftModifier);
        QCOMPARE(bible->selectedStart(),16); QCOMPARE(bible->selectedEnd(),17);
        key(Qt::Key_X); QVERIFY(bible->selectionHighlighted());
        key(Qt::Key_M); QVERIFY(bible->selectionBookmarked());
        key(Qt::Key_A,Qt::ShiftModifier);
        QTRY_VERIFY(prop("savedDrawer", "opened").toBool());
        QTRY_COMPARE(prop("collectionList", "count").toInt(),1);
        const QByteArray entryName = ("savedEntry" + bible->selectionReference()).toUtf8();
        QTRY_VERIFY(item(entryName.constData()));
        click(item(entryName.constData()));
        QTRY_VERIFY(!prop("savedDrawer", "visible").toBool());
        QCOMPARE(bible->selectedStart(),16); QCOMPARE(bible->selectedEnd(),17);
    }
    void freeTextAcrossVerses_data() {
        QTest::addColumn<bool>("reverse");
        QTest::addColumn<bool>("numbers");
        QTest::newRow("forward") << false << true;
        QTest::newRow("reverse") << true << true;
        QTest::newRow("forward-hidden-numbers") << false << false;
        QTest::newRow("reverse-hidden-numbers") << true << false;
    }
    void nonLeftDoubleClickIsolation() {
        installDragFixture();
        bible->selectVerses(1,2);
        bible->setNoteText("Preserve range note");
        const auto savedRange = bible->collection();
        bible->selectVerses(3,3);
        QTest::qWait(150);
        QSignalSpy selection(bible.get(),&BibleBackend::selectionChanged);
        for (auto button : {Qt::RightButton,Qt::MiddleButton}) {
            auto number = item("verseNumber3");
            const auto point = number->mapToScene(QPointF(8,12)).toPoint();
            QTest::mouseDClick(window,button,Qt::NoModifier,point);
            QTest::qWait(250);
            // Right-click opens the context menu inside the selection; dismiss it.
            if (prop("contextMenu","visible").toBool()) { key(Qt::Key_Escape); QTRY_VERIFY(!prop("contextMenu","visible").toBool()); }
            QCOMPARE(selection.count(),0);
            QCOMPARE(bible->selectionReference(),QString("John 3:3"));
            QCOMPARE(bible->collection(),savedRange);
            QVERIFY(!window->property("notesOpen").toBool());
        }
        QTest::mouseDClick(window,Qt::LeftButton,Qt::NoModifier,item("verseNumber3")->mapToScene(QPointF(8,12)).toPoint());
        QTRY_VERIFY(prop("notesDrawer","opened").toBool());
        QCOMPARE(bible->cursorVerse(),3);
    }
    void nestedNotesMenuFocus() {
        reference("John 3:16");
        for (auto menuKey : {Qt::Key_K,Qt::Key_F1}) {
            key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
            // Keys type in the editor; menus open from the Notes panel.
            item("notesPanel")->forceActiveFocus();
            key(menuKey,menuKey == Qt::Key_K ? Qt::ControlModifier : Qt::NoModifier);
            const char *menu = menuKey == Qt::Key_K ? "passagePalette" : "helpDialog";
            QTRY_VERIFY(prop(menu,"opened").toBool());
            key(Qt::Key_Escape); QTRY_VERIFY(!prop(menu,"visible").toBool());
            QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
            type(" jkhln");
            QCOMPARE(bible->cursorVerse(),16);
            QVERIFY(bible->noteText().endsWith(" jkhln"));
            key(Qt::Key_Escape); QTRY_VERIFY(!prop("notesDrawer","visible").toBool());
            QTRY_VERIFY(item("readerSurface")->hasActiveFocus());
        }
    }
    void lineHeightClearsFreeSelectionCoherently() {
        installDragFixture();
        const auto selected = bible->selectionReference();
        for (bool backendChange : {false,true}) {
            drag(textPoint(1,6),textPoint(3,18));
            QVERIFY(!window->property("freeSelectedText").toString().isEmpty());
            QVERIFY(!prop("verseBody2","selectedText").toString().isEmpty());
            if (backendChange) bible->setLineHeight(1.8);
            else key(Qt::Key_Equal,Qt::ControlModifier | Qt::AltModifier);
            QVERIFY(window->property("freeSelectedText").toString().isEmpty());
            for (int v = 1; v <= 3; ++v)
                QVERIFY(prop(qPrintable("verseBody" + QString::number(v)),"selectedText").toString().isEmpty());
            QApplication::clipboard()->setText("No stale selection");
            key(Qt::Key_C,Qt::ControlModifier);
            QCOMPARE(QApplication::clipboard()->text(),bible->copySelection());
            QCOMPARE(bible->selectionReference(),selected);
        }
        key(Qt::Key_F1); QTRY_VERIFY(prop("helpDialog","opened").toBool());
        type("copy selected text");
        QTRY_VERIFY(item("keybindingCopy selected text"));
        QVERIFY(item("keybindingCopy selected text")->isEnabled());
        key(Qt::Key_Return); QTRY_VERIFY(!prop("helpDialog","visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(window,"runCommand",Q_ARG(QVariant,"copyText")));
        QCOMPARE(QApplication::clipboard()->text(),bible->copySelection());
    }
    void blockInlineDocumentAndPreferences() {
        // Restart validates the content digest: create rich text through USFM,
        // rather than mutating an already validated database generation.
        const auto source = session->filePath("001-JHNengnet.usfm");
        QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly));
        const auto content = QString::fromUtf8(
            "\\id JHN Original rich reader fixture\n\\h John\n\\toc2 John\n"
            "\\c 3\n\\s A section title\n"
            "\\v 1 Start café — λόγος 🌿 and finish.\n"
            "\\q1\n\\v 2 \\it Poetry sings softly.\\it*\n\\q1 A second line — שלום.\n"
            "\\p\n\\v 3 Final naïve résumé ends here.\n"
            "\\v 4 Original wrapping text gives the reading surface enough height to exercise line spacing and restoration.\n").toUtf8();
        QCOMPARE(file.write(content),content.size()); file.close();
        bible->importUsfm(QUrl::fromLocalFile(source));
        QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(),120000);
        QVERIFY2(bible->ready() && bible->error().isEmpty(),qPrintable(bible->error()));
        reference("John 3:1");
        QVERIFY(!bible->blockMode());
        key(Qt::Key_L,Qt::AltModifier);
        QTRY_VERIFY(blockMapping() && blockMapping()->spans().size() == bible->verses().size());
        QTRY_VERIFY(item("blockBody")->height() > 0);
        QVERIFY(!item("verseBody1"));
        auto doc = blockTextDocument();
        QVERIFY(doc);
        // Verse 2 begins on verse 1's last visual line, rather than a new row.
        QCOMPARE(blockRect(blockOffset(1)).y(),blockRect(blockOffset(2)).y());
        QVERIFY(blockRect(blockOffset(2)).x() > blockRect(blockOffset(1)).x());
        QVERIFY(doc->toPlainText().contains(QString::fromUtf8("λόγος 🌿")));
        QVERIFY(doc->toPlainText().contains("A section title"));
        const int paragraphs = doc->blockCount();
        key(Qt::Key_H,Qt::AltModifier);
        QTRY_VERIFY(!doc->toPlainText().contains("A section title"));
        QVERIFY(doc->blockCount() < paragraphs);
        QCOMPARE(doc->blockCount(),1);
        QVERIFY(doc->toRawText().contains(QChar::LineSeparator)); // Poetry survives.
        key(Qt::Key_N,Qt::AltModifier);
        QTRY_VERIFY(doc->toPlainText().startsWith("Start café"));
        const auto height = item("blockBody")->height();
        key(Qt::Key_Equal,Qt::ControlModifier | Qt::AltModifier);
        QTRY_VERIFY(item("blockBody")->height() > height);
        key(Qt::Key_N,Qt::AltModifier); key(Qt::Key_H,Qt::AltModifier);
        QTRY_VERIFY(doc->toPlainText().contains("A section title"));
        key(Qt::Key_F);
        QTRY_VERIFY(!doc->toPlainText().contains("A section title"));
        QVERIFY(doc->toPlainText().startsWith("Start café"));
        QVERIFY(bible->showVerseNumbers()); QVERIFY(bible->showHeadings());
        QVERIFY(item("chapterTitle")->isVisible()); QVERIFY(!window->findChild<QQuickItem *>("editionLabel"));
        unloadUi(); loadUi();
        QVERIFY(bible->blockMode()); QVERIFY(bible->focusMode());
        QVERIFY2(bible->ready(),qPrintable(bible->error()));
        QVERIFY(bible->showVerseNumbers()); QVERIFY(bible->showHeadings());
        QTRY_VERIFY(blockTextDocument() && blockTextDocument()->toPlainText().startsWith("Start café"));
        key(Qt::Key_F);
        QTRY_VERIFY(blockTextDocument()->toPlainText().contains("A section title"));
        QVERIFY(item("chapterTitle")->isVisible());
        key(Qt::Key_L,Qt::AltModifier);
        QCOMPARE(bible->readingMode(),QString("paragraph"));
        key(Qt::Key_L,Qt::AltModifier);
        QTRY_VERIFY(item("verseBody1"));
        QVERIFY(item("verseNumberLabel1")->isVisible()); QVERIFY(item("verseHeading1")->isVisible());
        key(Qt::Key_F);
        QTRY_VERIFY(!item("verseNumberLabel1")->isVisible());
        QVERIFY(!item("verseHeading1")->isVisible()); QVERIFY(item("chapterTitle")->isVisible());
        QVERIFY(bible->showVerseNumbers()); QVERIFY(bible->showHeadings());
        key(Qt::Key_F);
        QTRY_VERIFY(item("verseHeading1")->isVisible());
        capture("block-return-to-rows");
    }
    void blockFreeTextAndContextCopy_data() {
        QTest::addColumn<bool>("reverse");
        QTest::addColumn<bool>("numbers");
        QTest::newRow("forward") << false << true;
        QTest::newRow("reverse") << true << true;
        QTest::newRow("forward-no-numbers") << false << false;
        QTest::newRow("reverse-no-numbers") << true << false;
    }
    void blockPointerGlyphBoundaries_data() {
        QTest::addColumn<bool>("numbers");
        QTest::newRow("numbers") << true;
        QTest::newRow("no-numbers") << false;
    }
    void blockPointerGlyphBoundaries() {
        QFETCH(bool,numbers);
        installDragFixture();
        // End verse 2 with RTL text, with no LTR punctuation after it.
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","ui-hit-fixture");
            db.setDatabaseName(installedPath());
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QString::fromUtf8("UPDATE verses SET text='Second שלום',html='Second שלום',poetry=0 WHERE book=43 AND chapter=3 AND number=2")));
        }
        QSqlDatabase::removeDatabase("ui-hit-fixture");
        reference("John 4:1"); reference("John 3:1");
        bible->setShowVerseNumbers(numbers);
        bible->setBlockMode(true);
        QTRY_VERIFY(blockMapping() && blockMapping()->spans().size() > 3);
        QTRY_VERIFY(!window->property("restoringPosition").toBool());
        QTest::qWait(100);
        QStringList quoted, plain;
        for (int v = 1; v <= 3; ++v) {
            bible->selectVerses(v,v);
            quoted.append(bible->copySelection()); plain.append(bible->copySelection(true));
        }
        const auto glyphPoint = [this](int position, double fraction) {
            const auto block = blockTextDocument()->findBlock(position);
            const auto relative = position - block.position();
            const auto line = block.layout()->lineForTextPosition(relative);
            const auto caret = blockRect(position);
            const double a = line.cursorToX(relative,QTextLine::Leading);
            const double b = line.cursorToX(relative,QTextLine::Trailing);
            // Include the QTextDocument block's horizontal origin.
            const double origin = caret.x() - line.cursorToX(relative);
            return QPointF(origin + a + fraction * (b-a),caret.center().y());
        };
        const auto checkClick = [this,&quoted](QPointF point, int verse) {
            for (auto button : {Qt::RightButton,Qt::MiddleButton}) {
                bible->clearSelection();
                // Selection/focus changes can queue viewport positioning. Settle
                // them before mapping this document point for each mouse event.
                QCoreApplication::processEvents();
                QTRY_VERIFY(!window->property("restoringPosition").toBool());
                const auto scene = item("blockBody")->mapToScene(point).toPoint();
                const auto rounded = item("blockBody")->mapFromScene(scene);
                QCOMPARE(blockMapping()->verseAtPoint(rounded.x(),rounded.y()),verse);
                QTest::mouseClick(window,button,Qt::NoModifier,scene);
                if (button == Qt::RightButton) {
                    QTRY_VERIFY(prop("contextMenu","visible").toBool());
                    key(Qt::Key_Escape); QTRY_VERIFY(!prop("contextMenu","visible").toBool());
                }
                QVERIFY2(bible->cursorVerse() == verse,qPrintable(QString("Expected %1, got %2 at %3,%4; local %5,%6; hit now %7; scroll %8")
                    .arg(verse).arg(bible->cursorVerse()).arg(scene.x()).arg(scene.y()).arg(point.x()).arg(point.y())
                    .arg(blockMapping()->verseAtPoint(item("blockBody")->mapFromScene(scene).x(),item("blockBody")->mapFromScene(scene).y()))
                    .arg(prop("reading","contentY").toDouble())));
                if (button == Qt::RightButton) continue;
                QCOMPARE(QApplication::clipboard()->text(),quoted[verse-1]);
                QCOMPARE(prop("toast","text").toString(),QString("John 3:%1 copied").arg(verse));
            }
        };
        for (int v = 1; v <= 2; ++v) {
            const auto span = blockMapping()->spans()[v-1].toMap();
            const int end = span.value("end").toInt();
            // Position mapping still treats the end caret as the next verse.
            QCOMPARE(blockMapping()->verseAt(end),v+1);
            const auto trailing = glyphPoint(end-1,0.85);
            if (v == 1) {
                int nearest = -1;
                QVERIFY(QMetaObject::invokeMethod(object("blockBody"),"positionAt",Q_RETURN_ARG(int,nearest),
                    Q_ARG(qreal,trailing.x()),Q_ARG(qreal,trailing.y())));
                QCOMPARE(nearest,end); // Reproduces the original caret/glyph mismatch.
            }
            checkClick(trailing,v);
            checkClick(glyphPoint(end-1,0.15),v); // Both edges, including RTL.
            // Every part of the inter-verse space belongs to the following verse.
            checkClick(glyphPoint(end,0.15),v+1);
            checkClick(glyphPoint(end,0.85),v+1);
            checkClick(glyphPoint(blockMapping()->positionFor(v+1),0.15),v+1);
            checkClick(glyphPoint(blockOffset(v+1),0.85),v+1);
        }
    }
    void chapterCopyFallbackAndEditorIsolation_data() {
        QTest::addColumn<bool>("block");
        QTest::newRow("rows") << false;
        QTest::newRow("block") << true;
    }
    void chapterCopyFallbackAndEditorIsolation() {
        QFETCH(bool,block);
        bible->setBlockMode(block);
        reference("John 3");
        QCOMPARE(bible->selectedStart(),0);
        const auto quoted = bible->copySelection();
        QVERIFY(quoted.endsWith(QString::fromUtf8("”\n\n—John 3")));
        QApplication::clipboard()->setText("replace chapter");
        key(Qt::Key_C,Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(),quoted);
        QCOMPARE(prop("toast","text").toString(),QString("John 3 copied"));
        QCOMPARE(bible->selectedStart(),0);
        QApplication::clipboard()->setText("replace y");
        key(Qt::Key_Y); QCOMPARE(QApplication::clipboard()->text(),quoted);
        reference("John 3:1");
        key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        type("Only the editor selection");
        key(Qt::Key_A,Qt::ControlModifier); key(Qt::Key_C,Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(),QString("Only the editor selection"));
        key(Qt::Key_End);
        QApplication::clipboard()->setText("no editor selection");
        key(Qt::Key_C,Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(),QString("no editor selection"));
        QCOMPARE(bible->selectionReference(),QString("John 3:1"));
    }
    void blockFreeTextAndContextCopy() {
        QFETCH(bool,reverse); QFETCH(bool,numbers);
        installDragFixture();
        bible->setShowVerseNumbers(numbers);
        bible->setBlockMode(true);
        QTRY_VERIFY(blockMapping() && blockMapping()->spans().size() > 3);
        QTRY_VERIFY(!window->property("restoringPosition").toBool());
        bible->selectVerses(1,3);
        bible->setNoteText("Keep this canonical range note");
        bible->toggleBookmark(); bible->toggleHighlight();
        const auto range = bible->selectionReference();
        const auto annotations = bible->collection();
        const auto quoted = bible->copySelection();
        const auto plain = bible->copySelection(true);
        // Copies preserve the source line break inside poetic verse 2 (995e30c);
        // verses within one prose paragraph still join with a space.
        QCOMPARE(plain,QString::fromUtf8("Start café — λόγος 🌿 and finish. Poetry sings softly.\nA second line — שלום. Final naïve résumé ends here."));
        QCOMPARE(quoted,QString::fromUtf8("“¹ Start café — λόγος 🌿 and finish. ² Poetry sings softly.\nA second line — שלום. ³ Final naïve résumé ends here.”\n\n—John 3:1–3"));
        QTextCursor cursor(blockTextDocument());
        cursor.setPosition(blockOffset(1,6)); cursor.setPosition(blockOffset(3,18),QTextCursor::KeepAnchor);
        const auto documentSelection = cursor.selectedText();
        // Clipboard text uses ordinary newlines for both document paragraphs
        // and explicit line breaks; the TextEdit still exposes Qt separators.
        const auto expected = QString(documentSelection).replace(QChar::ParagraphSeparator,'\n').replace(QChar::LineSeparator,'\n');
        QVERIFY(expected.contains(QString::fromUtf8("café — λόγος 🌿")));
        QVERIFY(expected.contains(QString::fromUtf8("שלום")));
        QVERIFY(!expected.contains(QChar::ObjectReplacementCharacter));
        // Selection changes schedule viewport restoration; annotation commits
        // schedule block geometry work. Sample only after both have settled,
        // otherwise restoration moves the text beneath an in-progress drag.
        QTRY_VERIFY(!window->property("restoringPosition").toBool());
        QSignalSpy frames(window,&QQuickWindow::frameSwapped);
        const auto settled = [&] {
            const auto first = blockPoint(1,6), last = blockPoint(3,18);
            const auto y = prop("reading","contentY");
            const int before = frames.count();
            window->update();
            if (!QTest::qWaitFor([&] { return frames.count() > before; },1000)) return false;
            return !window->property("restoringPosition").toBool() && y == prop("reading","contentY")
                && first == blockPoint(1,6) && last == blockPoint(3,18);
        };
        QTRY_VERIFY(settled());
        const auto first = blockPoint(1,6), last = blockPoint(3,18);
        drag(reverse ? last : first,reverse ? first : last);
        QCOMPARE(window->property("freeSelectedText").toString(),expected);
        QCOMPARE(prop("blockBody","selectedText").toString(),documentSelection);
        QCOMPARE(bible->selectionReference(),range);
        key(Qt::Key_C,Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(),expected);
        key(Qt::Key_Y); QCOMPARE(QApplication::clipboard()->text(),quoted);
        key(Qt::Key_Y,Qt::ShiftModifier); QCOMPARE(QApplication::clipboard()->text(),plain);
        key(Qt::Key_C,Qt::ControlModifier | Qt::ShiftModifier); QCOMPARE(QApplication::clipboard()->text(),plain);
        QCOMPARE(window->property("freeSelectedText").toString(),expected);
        QCOMPARE(bible->collection(),annotations);
        QTest::mouseClick(window,Qt::MiddleButton,Qt::NoModifier,blockPoint(2,4));
        QCOMPARE(QApplication::clipboard()->text(),quoted);
        QCOMPARE(prop("toast","text").toString(),range + " copied");
        QCOMPARE(bible->selectionReference(),range);
        QCOMPARE(window->property("freeSelectedText").toString(),expected);
        bible->selectVerses(1,3);
        QTest::mouseClick(window,Qt::RightButton,Qt::NoModifier,blockPoint(2,4));
        QTRY_VERIFY(prop("contextMenu","visible").toBool());
        QCOMPARE(bible->selectionReference(),range);
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("contextMenu","visible").toBool());
        QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,blockPoint(1,4));
        QCOMPARE(bible->cursorVerse(),1);
        QVERIFY(window->property("freeSelectedText").toString().isEmpty());
        QTest::mouseClick(window,Qt::LeftButton,Qt::ShiftModifier,blockPoint(3,4));
        QCOMPARE(bible->selectedStart(),1); QCOMPARE(bible->selectedEnd(),3);
        key(Qt::Key_C,Qt::ControlModifier); QCOMPARE(QApplication::clipboard()->text(),quoted);
        QTest::mouseClick(window,Qt::RightButton,Qt::NoModifier,blockPoint(2,4));
        QTRY_VERIFY(prop("contextMenu","visible").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("contextMenu","visible").toBool());
        QCOMPARE(window->property("selectionAnchor").toInt(),1);
        key(Qt::Key_W); QTRY_VERIFY(prop("notesDrawer","opened").toBool());
        QCOMPARE(bible->noteText(),QString("Keep this canonical range note"));
        click(item("notesCopy")); QCOMPARE(QApplication::clipboard()->text(),quoted);
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("notesDrawer","visible").toBool());
        key(Qt::Key_Equal,Qt::ControlModifier | Qt::AltModifier);
        QTRY_VERIFY(window->property("freeSelectedText").toString().isEmpty());
        QCOMPARE(bible->collection(),annotations);
        capture("block-unicode-selection");
    }
    void blockVerseNavigationAndScrollbar() {
        bible->setBlockMode(true);
        reference("Matthew 17:20");
        QTRY_VERIFY(blockMapping() && blockMapping()->spans().size() > 20);
        QVERIFY(item("readerScrollbar")->isVisible());
        const auto bar = item("readerScrollbar");
        QTest::mouseMove(window,bar->mapToScene(QPointF(bar->width()/2,bar->height()/2)).toPoint());
        QTRY_COMPARE(item("readerScrollbarThumb")->width(),10.0);
        key(Qt::Key_J); QCOMPARE(bible->cursorVerse(),22);
        key(Qt::Key_K,Qt::ShiftModifier); QCOMPARE(bible->selectedStart(),20); QCOMPARE(bible->selectedEnd(),22);
        key(Qt::Key_Escape); key(Qt::Key_2); key(Qt::Key_7); key(Qt::Key_G);
        QCOMPARE(bible->cursorVerse(),27);
        QTRY_VERIFY(blockPoint(27).y() < window->height());
        key(Qt::Key_J); QCOMPARE(bible->chapter(),18); QCOMPARE(bible->cursorVerse(),1);
        key(Qt::Key_K); QCOMPARE(bible->chapter(),17); QCOMPARE(bible->cursorVerse(),27);
        key(Qt::Key_G); QCOMPARE(bible->cursorVerse(),1);
        QTRY_VERIFY(blockPoint(1).y() >= 0);
        key(Qt::Key_D,Qt::ControlModifier); QCOMPARE(bible->cursorVerse(),6);
        key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        type("Block note"); key(Qt::Key_Escape); QTRY_VERIFY(!prop("notesDrawer","visible").toBool());
        QCOMPARE(bible->noteText(),QString("Block note"));
        key(Qt::Key_X); key(Qt::Key_M);
        QVERIFY(bible->selectionHighlighted()); QVERIFY(bible->selectionBookmarked());
        capture("block-navigation");
    }
    void blockAutoscrollAndCopyCommands() {
        bible->setBlockMode(true);
        reference("Psalm 119:1");
        QTRY_VERIFY(blockMapping() && blockMapping()->spans().size() == 176);
        QTest::qWait(100);
        const auto start = blockPoint(1,4);
        const auto bottom = item("reading")->mapToScene(QPointF(item("reading")->width()/2,
            item("reading")->height() - window->property("floatingSafety").toDouble() - 4)).toPoint();
        QTest::mousePress(window,Qt::LeftButton,Qt::NoModifier,start);
        QTest::mouseMove(window,bottom,100);
        QTRY_VERIFY(prop("reading","contentY").toDouble() > 100);
        const auto before = window->property("freeSelectedText").toString();
        // Autoscroll keeps extending the drag selection (frame rate varies under software rendering).
        QTRY_VERIFY(window->property("freeSelectedText").toString().size() > before.size());
        QTest::mouseRelease(window,Qt::LeftButton,Qt::NoModifier,bottom);
        const auto selected = window->property("freeSelectedText").toString();
        key(Qt::Key_C,Qt::ControlModifier); QCOMPARE(QApplication::clipboard()->text(),selected);
        key(Qt::Key_G); QCOMPARE(bible->cursorVerse(),1);
        QVERIFY(window->property("freeSelectedText").toString().isEmpty());
        const auto quoted = bible->copySelection(), plain = bible->copySelection(true);
        for (bool plainText : {false,true}) {
            key(Qt::Key_Space,Qt::ControlModifier);
            QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
            type(plainText ? "copy passage as plain text" : "copy passage as quotation");
            QCOMPARE(prop("paletteResults","count").toInt(),1);
            key(Qt::Key_Return); QTRY_VERIFY(!prop("passagePalette","visible").toBool());
            QCOMPARE(QApplication::clipboard()->text(),plainText ? plain : quoted);
        }
        reference("John 3:36-4:2");
        QTRY_VERIFY(blockMapping()->spans().size() == bible->verses().size());
        QTest::qWait(100);
        const auto range = bible->selectionReference();
        const auto all = bible->copySelection();
        QSignalSpy selection(bible.get(),&BibleBackend::selectionChanged);
        QTest::mouseClick(window,Qt::MiddleButton,Qt::NoModifier,blockPoint(2,4));
        QCOMPARE(QApplication::clipboard()->text(),all);
        QCOMPARE(bible->selectionReference(),range);
        QCOMPARE(selection.count(),0);
        QCOMPARE(prop("toast","text").toString(),range + " copied");
    }
    void blockFailedContextCopyIsPure() {
        installDragFixture();
        bible->setBlockMode(true);
        QTRY_VERIFY(blockMapping() && blockMapping()->spans().size() > 3);
        QTest::qWait(100);
        const auto range = bible->selectionReference();
        const auto toast = prop("toast","text");
        QApplication::clipboard()->setText("Keep clipboard on failed copy");
        QSignalSpy selection(bible.get(),&BibleBackend::selectionChanged);
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","ui-block-copy-failure");
            db.setDatabaseName(bible->dataDirectory() + "/personal.sqlite");
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec("CREATE TRIGGER reject_copy_state BEFORE UPDATE ON reading BEGIN SELECT RAISE(FAIL,'copy focus failure'); END"));
            QTest::mouseClick(window,Qt::MiddleButton,Qt::NoModifier,blockPoint(2,4));
            QCOMPARE(bible->selectionReference(),range);
            QCOMPARE(QApplication::clipboard()->text(),QString("Keep clipboard on failed copy"));
            QCOMPARE(prop("toast","text"),toast);
            QCOMPARE(selection.count(),0);
            QVERIFY(!bible->error().isEmpty());
        }
        QSqlDatabase::removeDatabase("ui-block-copy-failure");
    }
    void continuousHighlightGeometry() {
        installDragFixture();
        // Put a visible heading inside the range, not only at its beginning.
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","ui-highlight-heading");
            db.setDatabaseName(installedPath());
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("UPDATE verses SET heading='An internal heading' WHERE book=43 AND chapter=3 AND number=2"));
        }
        QSqlDatabase::removeDatabase("ui-highlight-heading");
        reference("John 4:1"); reference("John 3:1");
        bible->selectVerses(1,3); bible->toggleHighlight();
        QTest::qWait(100);
        const auto range = bible->selectionReference();
        for (int v = 1; v < 3; ++v) {
            const auto a = item(qPrintable(QString("highlightSegment%1:").arg(v) + range));
            const auto b = item(qPrintable(QString("highlightSegment%1:").arg(v+1) + range));
            QVERIFY(a && b);
            QCOMPARE(a->mapToScene(QPointF(0,a->height())).y(),b->mapToScene(QPointF()).y());
            const auto accent = item(qPrintable(QString("highlightAccent%1:").arg(v) + range));
            QVERIFY(accent); QCOMPARE(accent->height(),a->height());
            QCOMPARE(a->property("radius").toDouble(),0.0);
            const auto selection = item(qPrintable(QString("selectionSegment%1").arg(v)));
            const auto next = item(qPrintable(QString("selectionSegment%1").arg(v+1)));
            QCOMPARE(selection->mapToScene(QPointF(0,selection->height())).y(),next->mapToScene(QPointF()).y());
        }
        bible->toggleHighlight();
        bible->selectVerses(1,1); bible->toggleHighlight();
        bible->selectVerses(2,2); bible->toggleHighlight();
        QTest::qWait(100);
        auto first = item("highlightSegment1:John 3:1"), second = item("highlightSegment2:John 3:2");
        QVERIFY(first && second);
        QVERIFY(first->mapToScene(QPointF(0,first->height())).y() < second->mapToScene(QPointF()).y());
        bible->setBlockMode(true);
        QTRY_VERIFY(blockMapping() && blockMapping()->spans().size() > 3);
        QTest::qWait(100);
        const auto rects = blockMapping()->rectangles(blockMapping()->positionFor(1),blockOffset(3,18));
        QVERIFY(rects.size() >= 2);
        QCOMPARE(rects.first().toMap().value("y").toDouble(),blockRect(blockOffset(1)).y());
        for (int i = 0; i + 1 < rects.size(); ++i) {
            const auto a = rects[i].toMap(), b = rects[i+1].toMap();
            if (a.value("y") == b.value("y")) continue; // Disjoint bidi runs on one line.
            QVERIFY(qAbs(a.value("y").toDouble() + a.value("height").toDouble() - b.value("y").toDouble()) < 0.01);
        }
        const auto span = blockMapping()->spans()[1].toMap();
        const auto paint = blockMapping()->rectangles(span.value("start").toInt(),span.value("end").toInt());
        const int hebrew = blockTextDocument()->toPlainText().indexOf(QString::fromUtf8("שלום"));
        QVERIFY(hebrew > 0);
        const auto left = blockRect(hebrew + 1), right = blockRect(hebrew + 2);
        const QPointF middle((left.x() + right.x()) / 2,left.center().y());
        bool covered = false;
        for (const auto &value : paint) {
            const auto r = value.toMap();
            covered |= QRectF(r.value("x").toDouble(),r.value("y").toDouble(),
                              r.value("width").toDouble(),r.value("height").toDouble()).contains(middle);
        }
        QVERIFY(covered);
        capture("block-adjacent-highlights");
    }
    void verseAccentSpacing_data() {
        QTest::addColumn<int>("size");
        QTest::addColumn<double>("scale");
        for (int size : {8,18,40})
            for (double scale : {1.0,1.8})
                QTest::newRow(qPrintable(QString("%1px-%2x").arg(size).arg(scale))) << size << scale;
    }
    void verseAccentSpacing() {
        QFETCH(int,size); QFETCH(double,scale);
        // Keep this case independent of a full Psalms fixture.
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","ui-accent-fixture");
            db.setDatabaseName(installedPath()); QVERIFY(db.open());
            QSqlQuery q(db);
            q.prepare("UPDATE verses SET text=?,html=?,heading='' WHERE book=43 AND chapter=3 AND number IN (31,32)");
            const QString text = QString("Agjp long verse wrapping safely across several lines. ").repeated(16).trimmed();
            q.addBindValue(text); q.addBindValue(text);
            QVERIFY(q.exec());
        }
        QSqlDatabase::removeDatabase("ui-accent-fixture");
        bible->setFontSize(size);
        desktop->setProperty("textScale",scale);
        window->resize(700,900);
        QVERIFY2(bible->navigate("John 3:31"),qPrintable(bible->error()));
        bible->setShowHeadings(false);
        bible->setNoteText("Closest packed marker");
        QVERIFY(bible->toggleBookmark());
        QVERIFY(!bible->createAndAssignTag(bible->tagScope(),"Packed").isEmpty());
        QTest::qWait(80);
        auto geometry = [&]() {
            QVariantList result;
            for (int number : {31,32}) {
                auto body = item(qPrintable(QString("verseBody%1").arg(number)));
                result.append(body->x()); result.append(body->y());
                result.append(body->width()); result.append(body->height());
                auto doc = body->property("textDocument").value<QQuickTextDocument *>()->textDocument();
                for (int p = 0; p < doc->characterCount(); ++p) {
                    QRectF caret;
                    QMetaObject::invokeMethod(body,"positionToRectangle",Q_RETURN_ARG(QRectF,caret),Q_ARG(int,p));
                    result.append(caret);
                }
            }
            return result;
        };
        const auto before = geometry();
        for (int number : {31,32}) {
            bible->selectVerses(number,number);
            bible->toggleHighlight();
            QTest::qWait(60);
            QCOMPARE(geometry(),before);
            auto label = item(qPrintable(QString("verseNumberLabel%1").arg(number)));
            auto hit = item(qPrintable(QString("verseNumber%1").arg(number)));
            auto accent = item(qPrintable(QString("highlightAccent%1:John 3:%1").arg(number)));
            QVERIFY(label && hit && accent);
            // Exercise three digits on the real label without changing the
            // edition's canonical verse keys or expanding the fixture.
            if (number == 32) label->setProperty("text","119");
            QTest::qWait(30);
            QCOMPARE(label->property("text").toString(),number == 32 ? QString("119") : QString("31"));
            QCOMPARE(hit->x(),54 * scale); QCOMPARE(hit->width(),17 * scale);
            const auto bounds = label->mapRectToItem(hit->parentItem(),label->boundingRect());
            QVERIFY(bounds.left() > 50 * scale);
            const auto border = accent->mapRectToItem(hit->parentItem(),accent->boundingRect());
            QCOMPARE(border.left(),67 * scale);
            QVERIFY(bounds.right() <= border.left() + 0.01);
            auto grab = label->grabToImage(); QVERIFY(grab);
            QSignalSpy ready(grab.get(),&QQuickItemGrabResult::ready);
            if (grab->image().isNull()) QVERIFY(ready.wait(2000));
            const auto image = grab->image();
            int ink = 0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x) {
                    if (image.pixelColor(x,y).alpha() < 40) continue;
                    ++ink;
                    const double pixelX = label->mapToItem(hit->parentItem(),QPointF(x * label->width() / image.width(),0)).x();
                    QVERIFY2(pixelX > 50 * scale && pixelX < border.left(),
                        qPrintable(QString("label %1..%2 border %3 pixel %4/%5").arg(bounds.left()).arg(bounds.right()).arg(border.left()).arg(x).arg(image.width())));
                }
            QVERIFY(ink > 3);
            bible->toggleHighlight();
            QTest::qWait(60);
            QCOMPARE(geometry(),before);
        }
        item("verseBody31")->forceActiveFocus();
        QCOMPARE(geometry(),before);
    }
    void focusedWorkflowErrors() {
        reference("John 3:16");
        bible->setFocusMode(true);
        for (bool importing : {true,false}) {
            if (importing) {
                const auto path = session->filePath("invalid.zip");
                QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
                file.write("invalid USFM ZIP"); file.close();
                bible->importUsfm(QUrl::fromLocalFile(path));
                QTRY_VERIFY(!bible->busy());
            } else {
                const auto path = session->filePath("not-a-directory");
                QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
                QVERIFY(!bible->exportNotes(QUrl::fromLocalFile(path)));
            }
            QVERIFY(!bible->error().isEmpty());
            QTRY_VERIFY(prop("errorDialog","opened").toBool());
            QCOMPARE(prop("focusedErrorMessage","text").toString(),bible->error());
            QVERIFY(item("focusedErrorMessage")->isVisible());
            QVERIFY(bible->focusMode());
            QVERIFY(!item("errorMessage")->isVisible()); QVERIFY(window->property("footerQuiet").toBool());
            QVERIFY(!window->findChild<QQuickItem *>("editionLabel"));
            capture(importing ? "review-focused-import-error" : "review-focused-export-error");
            key(Qt::Key_Escape); QTRY_VERIFY(!prop("errorDialog","visible").toBool());
            QTRY_VERIFY(item("readerSurface")->hasActiveFocus());
            QVERIFY(bible->focusMode());
        }
    }
    void themedCornerSliders() {
        for (const auto query : {"settings","reading settings"}) {
            key(Qt::Key_Space,Qt::ControlModifier);
            QTRY_VERIFY(prop("passagePalette","opened").toBool());
            type(query); key(Qt::Key_Return);
            QTRY_VERIFY(prop("settingsDialog","opened").toBool());
            auto handle = item("lineHeightSliderHandle");
            auto track = item("lineHeightSliderTrack");
            QVERIFY(handle); QVERIFY(track);
            QCOMPARE(handle->property("radius").toDouble(),qMin(desktop->cornerRadius(),qMin(handle->width(),handle->height()) / 2));
            QCOMPARE(track->property("radius").toDouble(),qMin(desktop->cornerRadius(),qMin(track->width(),track->height()) / 2));
            QCOMPARE(track->property("color"),desktop->property("accent"));
            QCOMPARE(handle->property("color"),desktop->property("surface"));
            const auto old = bible->lineHeight();
            key(Qt::Key_Right); QCOMPARE(bible->lineHeight(),old);
            key(Qt::Key_Up); QTRY_COMPARE(bible->lineHeight(),old + 0.1);
            QVERIFY(desktop->setProperty("accent",QColor("#b454ea")));
            QCOMPARE(track->property("color").value<QColor>(),QColor("#b454ea"));
            capture(QString("review-square-slider-") + query);
            key(Qt::Key_Escape); QTRY_VERIFY(!prop("settingsDialog","visible").toBool());
        }
    }
    void freeTextAcrossVerses() {
        QFETCH(bool,reverse); QFETCH(bool,numbers);
        installDragFixture();
        bible->setShowVerseNumbers(numbers);
        bible->setNoteText("Canonical note must survive free selection");
        QCoreApplication::processEvents();
        const auto canonical = bible->selectionReference();
        const auto note = bible->noteText();
        QSignalSpy selection(bible.get(),&BibleBackend::selectionChanged);
        const int start = 6, end = 18;
        const auto expected = bodyText(1).mid(start) + '\n' + bodyText(2) + '\n' + bodyText(3).left(end);
        QVERIFY(expected.contains(QString::fromUtf8("café — λόγος 🌿")));
        QVERIFY(expected.contains(QString::fromUtf8("שלום")));
        const auto first = textPoint(1,start), last = textPoint(3,end);
        drag(reverse ? last : first,reverse ? first : last);
        QCOMPARE(window->property("freeSelectedText").toString(),expected);
        QCOMPARE(prop("verseBody1","selectedText").toString(),bodyText(1).mid(start));
        QCOMPARE(prop("verseBody2","selectedText").toString(),bodyText(2));
        QCOMPARE(prop("verseBody3","selectedText").toString(),bodyText(3).left(end));
        QCOMPARE(bible->selectionReference(),canonical);
        QCOMPARE(bible->noteText(),note);
        QCOMPARE(selection.count(),0);
        QVERIFY(window->activeFocusItem()->objectName().startsWith("verseBody"));
        QApplication::clipboard()->setText("replace me");
        key(Qt::Key_C,Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(),expected);
        // Ctrl+C also works after moving focus out of the TextEdit.
        item("readerSurface")->forceActiveFocus();
        QApplication::clipboard()->setText("replace again");
        key(Qt::Key_C,Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(),expected);
        // Middle clicks must not consume the free selection or alter the
        // canonical selection before deciding which passage to copy.
        mouseCopy(2,Qt::MiddleButton);
        QCOMPARE(QApplication::clipboard()->text(),bible->copyVerse(2));
        QCOMPARE(window->property("freeSelectedText").toString(),expected);
        QCOMPARE(bible->selectionReference(),QString("John 3:2"));
        QCOMPARE(saved(1).value("note").toString(),note);
        mouseCopy(3,Qt::MiddleButton);
        QCOMPARE(QApplication::clipboard()->text(),bible->copyVerse(3));
        QCOMPARE(bible->selectionReference(),QString("John 3:3"));
        QCOMPARE(saved(1).value("note").toString(),note);
        QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,textPoint(2,4));
        QCOMPARE(bible->cursorVerse(),2);
        QVERIFY(window->property("freeSelectedText").toString().isEmpty());
    }
    void mouseCopySingleAndCanonicalRange() {
        installDragFixture();
        bible->selectVerses(1,2);
        bible->setNoteText("Range note");
        QTest::qWait(100);
        const auto range = bible->selectionReference();
        const auto text = bible->copySelection();
        QSignalSpy selection(bible.get(),&BibleBackend::selectionChanged);
        {
            bible->selectVerses(1,2);
            const auto expected = bible->copySelection();
            mouseCopy(1,Qt::MiddleButton);
            const auto copied = QApplication::clipboard()->text();
            QCOMPARE(copied,expected);
            QCOMPARE(copied,text);
            QCOMPARE(bible->selectionReference(),range);
            mouseCopy(3,Qt::MiddleButton);
            const auto single = QApplication::clipboard()->text();
            QCOMPARE(single,bible->copyVerse(3));
            QVERIFY(single.contains("John 3:3"));
            QVERIFY(!single.contains("John 3:1"));
            QCOMPARE(bible->selectionReference(),QString("John 3:3"));
            QCOMPARE(bible->collection().first().toMap().value("note").toString(),QString("Range note"));
        }
        QVERIFY(selection.count() > 0);
        // A visible intersection still copies the whole canonical cross-chapter
        // range, including the hidden chapter, before focusing the clicked verse.
        reference("John 3:36-4:2");
        const auto all = bible->copySelection();
        QCOMPARE(bible->chapter(),4);
        QVERIFY(QMetaObject::invokeMethod(window,"scrollToVerse",Q_ARG(QVariant,2),Q_ARG(QVariant,true)));
        QTRY_VERIFY(!object("readerSlide")->property("running").toBool());
        QTest::qWait(100);
        selection.clear();
        mouseCopy(2,Qt::MiddleButton);
        QCOMPARE(QApplication::clipboard()->text(),all);
        mouseCopy(2,Qt::RightButton);
        QTRY_VERIFY(prop("contextMenu","visible").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("contextMenu","visible").toBool());
        QCOMPARE(bible->selectionReference(),QString::fromUtf8("John 3:36–4:2"));
        QCOMPARE(selection.count(),0);
    }
    void dragAutoscrollAndOmittedNumbers() {
        reference("Matthew 17:20");
        bible->setShowVerseNumbers(false);
        QVERIFY(!item("verseBody21"));
        const int start = 10, end = 15;
        drag(textPoint(20,start),textPoint(22,end));
        // Positions are document positions; footnote cells (real NET) never reach the copy.
        QCOMPARE(window->property("freeSelectedText").toString(),
                 (rawBodyText(20).mid(start) + '\n' + rawBodyText(22).left(end)).remove(QChar(0xfffc)).remove(QChar(0x2060)));
        // The number gutter remains a canonical range target when labels hide.
        click(item("verseNumber20"));
        click(item("verseNumber22"),Qt::ShiftModifier);
        QCOMPARE(bible->selectedStart(),20); QCOMPARE(bible->selectedEnd(),22);
        const auto gutterPoint = [this](int verse) {
            return item(qPrintable("verseNumber" + QString::number(verse)))->mapToScene(QPointF(8,12)).toPoint();
        };
        drag(gutterPoint(22),gutterPoint(20));
        QCOMPARE(bible->selectedStart(),20); QCOMPARE(bible->selectedEnd(),22);
        QCOMPARE(bible->cursorVerse(),20);
        reference("John 3:10");
        const auto before = prop("reading","contentY").toDouble();
        const QPoint origin = textPoint(10,8);
        const QPoint bottom(origin.x(),window->height() - 80);
        QTest::mousePress(window,Qt::LeftButton,Qt::NoModifier,origin);
        QTest::mouseMove(window,bottom,30);
        QTRY_VERIFY(prop("reading","contentY").toDouble() > before + 100);
        const auto selected = window->property("freeSelectedText").toString();
        QVERIFY(selected.count('\n') >= 2);
        // Reverse the same live gesture and let it scroll back upward.
        const double down = prop("reading","contentY").toDouble();
        const QPoint top(origin.x(),5);
        QTest::mouseMove(window,top,30);
        QTRY_VERIFY(prop("reading","contentY").toDouble() < down - 100);
        QTest::mouseRelease(window,Qt::LeftButton,Qt::NoModifier,top);
        QVERIFY(!window->property("draggingText").toBool());
        QCOMPARE(bible->cursorVerse(),10);
        key(Qt::Key_C,Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(),window->property("freeSelectedText").toString());
    }
    void runtimeReadingPreferences() {
        installDragFixture();
        bible->toggleBookmark(); bible->setNoteText("Marker");
        QCoreApplication::processEvents();
        const auto x = item("verseBody1")->x();
        const auto height = item("verseBody2")->height();
        const auto heading = item("verseHeading1")->height();
        QVERIFY(heading > 0);
        QVERIFY(item("headingPrefix1")->isVisible());
        QCOMPARE(prop("headingPrefix1","text").toString(),QString("#"));
        QCOMPARE(prop("chapterTitle","leftPadding").toDouble(),x);
        bible->setShowVerseNumbers(false);
        QVERIFY(!item("verseNumberLabel1")->isVisible());
        QCOMPARE(item("verseBody1")->x(),x);
        QVERIFY(item("bookmarkMarker1")->isVisible());
        bible->setShowHeadings(false);
        QTRY_COMPARE(item("verseHeading1")->height(),0.0);
        QVERIFY(!item("headingPrefix1")->isVisible());
        bible->setLineHeight(2.5);
        QTRY_VERIFY(item("verseBody2")->height() > height * 1.4);
        bible->setShowHeadings(true);
        QTRY_COMPARE(item("verseHeading1")->height(),heading);
        bible->setShowVerseNumbers(true);
        QVERIFY(item("verseNumberLabel1")->isVisible());
        bible->setLineHeight(1.5);
        QTRY_COMPARE(item("verseBody2")->height(),height);
        key(Qt::Key_W);
        QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        bible->setFocusMode(true);
        QTRY_VERIFY(!prop("notesDrawer","visible").toBool());
        QTRY_VERIFY(item("readerSurface")->hasActiveFocus());
        QVERIFY(!window->findChild<QQuickItem *>("editionLabel"));
        for (const auto name : {"readerScrollbar","notesPanel","bookmarkMarker1","toast"})
            QVERIFY2(!item(name)->isVisible(),name);
        // The footer stays, quiet: only the focus button shows; the rest keep their places, hidden.
        QTRY_VERIFY(!item("readingBottomFade")->isVisible());
        QVERIFY(window->property("footerQuiet").toBool()); QVERIFY(item("focusControl")->opacity() == 1);
        for (const auto name : {"modeControl","passageControl","deskControl"}) QTRY_COMPARE(item(name)->opacity(),0.0);
        QCOMPARE(item("reading")->width(),double(window->width()));
        QVERIFY(item("verseBody1")->isVisible());
        QVERIFY(item("chapterTitle")->isVisible());
        QVERIFY(!item("verseHeading1")->isVisible());
        QVERIFY(!item("verseNumberLabel1")->isVisible());
        QVERIFY(bible->showVerseNumbers()); QVERIFY(bible->showHeadings());
        for (int i = 0; i < 12; ++i) {
            key(Qt::Key_Tab);
            QVERIFY(!window->activeFocusItem() || window->activeFocusItem()->isVisible());
        }
        key(Qt::Key_K,Qt::ControlModifier);
        QTRY_VERIFY(prop("passagePalette","opened").toBool());
        key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        QVERIFY(bible->focusMode());
        key(Qt::Key_Escape);
        QVERIFY(!bible->focusMode());
        QTRY_VERIFY(item("readingBottomFade")->isVisible());
        QVERIFY(item("bookmarkMarker1")->isVisible());
    }
    void readerScrollbarAndHeader() {
        QVERIFY(!window->findChild<QQuickItem *>("editionLabel"));
        reference("John 3:36");
        key(Qt::Key_J);
        QTRY_COMPARE(bible->chapter(),4);
        QTRY_COMPARE(prop("reading","contentY").toDouble(),0.0);
        QVERIFY(item("chapterTitle")->mapToScene(QPointF()).y() >= 0);
        reference("John 3:1");
        QCOMPARE(prop("reading","contentY").toDouble(),0.0);
        auto bar = item("readerScrollbar");
        auto thumb = item("readerScrollbarThumb");
        QVERIFY(bar->isVisible());
        QVERIFY(bar->mapRectToScene(bar->boundingRect()).bottom() < item("modeControl")->mapToScene(QPointF()).y());
        QTest::mouseMove(window,QPoint(2,2));
        QTRY_COMPARE(thumb->width(),4.0);
        QTest::mouseMove(window,bar->mapToScene(QPointF(bar->width()/2,20)).toPoint());
        QTRY_VERIFY(bar->property("hovered").toBool());
        QTRY_COMPARE(thumb->width(),10.0);
        QTest::mouseMove(window,QPoint(2,2));
        key(Qt::Key_PageDown);
        QTRY_VERIFY(thumb->opacity() >= 0.49);
        QTest::qWait(1400);
        QTRY_VERIFY(thumb->opacity() <= 0.21);
        const auto buttonBackground = qvariant_cast<QQuickItem *>(prop("passageControl","background"));
        QCOMPARE(buttonBackground->property("radius").toDouble(),qMin(desktop->cornerRadius(),qMin(buttonBackground->width(),buttonBackground->height()) / 2));
        reference("John 3:36-4:2");
        QCOMPARE(prop("passageControl","text").toString(),bible->selectionReference());
    }
    void floatingControlsAndNotes_data() {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<double>("scale");
        QTest::newRow("wide") << QSize(1120,820) << 1.0;
        QTest::newRow("small") << QSize(640,480) << 1.0;
        QTest::newRow("scaled") << QSize(640,480) << 1.8;
    }
    void floatingControlsAndNotes() {
        QFETCH(QSize,size); QFETCH(double,scale);
        window->resize(size);
        QVERIFY(desktop->setProperty("textScale",scale));
        reference("John 3:16-18");
        QTest::qWait(150);
        QVERIFY(!window->property("header").value<QObject *>());
        QVERIFY(!window->property("footer").value<QObject *>());
        QCOMPARE(item("reading")->height(),double(size.height()));
        QCOMPARE(item("reading")->mapToScene(QPointF()).y(),0.0);
        auto passage = item("passageControl");
        QCOMPARE(prop("passageControl","text").toString(),bible->selectionReference());
        QVERIFY(prop("passageControl","tip").toString().contains(bible->selectionReference()));
        const auto location = item("locationControls");
        const auto version = item("versionControl");
        const auto search = item("searchControl");
        QCOMPARE(location->x() + location->width() / 2,size.width() / 2.0);
        QCOMPARE(search->x(),0.0);
        QVERIFY(search->width() > 0);
        QCOMPARE(passage->x(),search->width());
        QCOMPARE(version->x(),search->width() + passage->width());
        const auto layout = item("layoutControl");
        // Cramped and scaled, the layout button gives way to the passage.
        QCOMPARE(layout->isVisible(),!window->property("footerHideLayout").toBool());
        QCOMPARE(layout->isVisible(),size.width() > 640 || scale < 1.5);
        if (layout->isVisible()) QCOMPARE(layout->x(),search->width() + passage->width() + version->width());
        QCOMPARE(location->width(),search->width() + passage->width() + version->width() + (layout->isVisible() ? layout->width() : 0));
        QCOMPARE(prop("searchControl","glyph").toString(),QString("search"));
        QCOMPARE(prop("versionControl","text").toString(),bible->versionAbbreviation());
        QVERIFY(passage->width() > 0);
        const auto actions = item("floatingActions");
        // Stacked or not, the center row sits between the corner groups.
        QVERIFY(location->x() + location->width() < actions->x());
        QVERIFY(location->x() > item("leftActions")->x() + item("leftActions")->width());
        for (const auto name : {"modeControl","searchControl","passageControl","versionControl","layoutControl","relatedControl","savedControl","deskControl","helpControl"}) {
            auto control = item(name);
            if (control == layout && !layout->isVisible()) continue;
            QVERIFY(control->isVisible());
            QVERIFY(QRectF(QPointF(),size).contains(control->mapRectToScene(QRectF(0,0,control->width(),control->height()))));
            const auto background = qvariant_cast<QQuickItem *>(control->property("background"));
            QVERIFY(background);
            QCOMPARE(background->property("color").value<QColor>().alpha(),0);
            if (control != passage && control != version) QVERIFY(control->property("text").toString().isEmpty());
            control->forceActiveFocus(Qt::TabFocusReason);
            QVERIFY(control->hasActiveFocus());
        }
        key(Qt::Key_Return);
        QTRY_VERIFY(prop("helpDialog","opened").toBool());
        key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("helpDialog","visible").toBool());
        click(item("modeControl")); QVERIFY(window->property("verseMode").toBool());
        click(item("modeControl")); QVERIFY(!window->property("verseMode").toBool());
        capture("omaword-refined-controls-" + QString::fromLatin1(QTest::currentDataTag()));
        click(search);
        QTRY_VERIFY(prop("passagePalette","opened").toBool());
        QCOMPARE(prop("referenceInput","text").toString(),QString("/ "));
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        click(passage);
        QTRY_VERIFY(prop("passagePalette","opened").toBool());
        QCOMPARE(prop("referenceInput","text").toString(),QString());
        capture("omaword-refined-palette-" + QString::fromLatin1(QTest::currentDataTag()));
        key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        click(item("savedControl"));
        QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        QVERIFY(!item("relatedControl")->isEnabled());
        capture("omaword-refined-saved-" + QString::fromLatin1(QTest::currentDataTag()));
        key(Qt::Key_A,Qt::ShiftModifier); QTest::qWait(50); QVERIFY(prop("savedDrawer","visible").toBool()); key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
        // No footer Notes button: w opens Notes.
        QVERIFY(!item("notesControl"));
        key(Qt::Key_W);
        QTRY_VERIFY(prop("notesDrawer","opened").toBool());
        QCOMPARE(prop("notesPanel","color"),desktop->property("background"));
        QVERIFY(prop("noteEditor","placeholderText").toString().isEmpty());
        QCOMPARE(item("reading")->width(),double(size.width()));
        QCOMPARE(location->x() + location->width() / 2,size.width() / 2.0);
        QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        type("20g 12345 remains note text");
        QCOMPARE(bible->noteText(),QString("20g 12345 remains note text"));
        capture("omaword-refined-notes-" + QString::fromLatin1(QTest::currentDataTag()));
        // The outer notes scroller can place its heading above the viewport.
        auto viewport = qvariant_cast<QQuickItem *>(prop("notesScroll","contentItem"));
        QVERIFY(viewport); viewport->setProperty("contentY",0);
        QTest::qWait(100);
        QVERIFY(!item("notesClose"));
        key(Qt::Key_Escape);
        QTRY_VERIFY(!window->property("notesOpen").toBool());
        QCOMPARE(bible->noteText(),QString("20g 12345 remains note text"));
    }
    void scaledNotesLayout_data() {
        QTest::addColumn<int>("width");
        QTest::addColumn<double>("scale");
        QTest::newRow("1080-scaled") << 1080 << 1.8;
        QTest::newRow("1120-scaled") << 1120 << 1.8;
        QTest::newRow("1120-triple") << 1120 << 3.0;
        QTest::newRow("1600-scaled") << 1600 << 1.8;
        QTest::newRow("1920-triple") << 1920 << 3.0;
    }
    void scaledNotesLayout() {
        QFETCH(int,width); QFETCH(double,scale);
        reference("John 3:16");
        window->resize(width,900);
        QVERIFY(desktop->setProperty("textScale",scale));
        key(Qt::Key_W);
        QTRY_VERIFY(prop("notesDrawer","opened").toBool());
        QTest::qWait(100);
        auto passage = item("passageControl");
        QVERIFY(passage->width() > 0);
        QCOMPARE(item("reading")->width(),double(width));
        QCOMPARE(item("notesPanel")->mapToScene(QPointF()).x(),width - item("notesPanel")->width());
        // Switching scale live keeps the reader's geometry intact.
        window->resize(1120,900);
        QVERIFY(desktop->setProperty("textScale",1.0));
        QTRY_COMPARE(item("reading")->width(),1120.0);
        QVERIFY(desktop->setProperty("textScale",3.0));
        QTRY_COMPARE(item("reading")->width(),1120.0);
        QVERIFY(passage->width() >= 0);
    }
    void blockAnnotationChooserKeyboardIsolation() {
        // Notes retain a rail chooser for overlapping ranges. Bookmarks now
        // have individual inline markers rather than a shared rail chooser.
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","ui-annotation-fixture");
            db.setDatabaseName(installedPath());
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("UPDATE verses SET text='Short verse.',html='Short verse.',heading='',poetry=0 WHERE book=43 AND chapter=3 AND number<=4"));
        }
        QSqlDatabase::removeDatabase("ui-annotation-fixture");
        reference("John 4:1");
        reference("John 3:1-2");
        bible->toggleBookmark(); bible->setNoteText("First range note");
        reference("John 3:1-4");
        bible->toggleBookmark(); bible->setNoteText("Second range note");
        reference("John 3:1-2");
        bible->setBlockMode(true);
        QTRY_VERIFY(!window->property("restoringPosition").toBool());
        const auto annotations = bible->collection();
        const char *markerName = "blockInlineMarker:note:open:John 3:1–2";
        QTRY_VERIFY(item(markerName) && item(markerName)->isVisible());
        const auto open = [this,markerName] {
            auto marker = item(markerName);
            QTest::mouseClick(window,Qt::RightButton,Qt::NoModifier,marker->mapToScene(QPointF(marker->width()/2,marker->height()/2)).toPoint());
            QTRY_VERIFY(prop("contextMenu","visible").toBool());
            QCOMPARE(prop("contextMenu","target").toString(),QString("marker"));
            QVERIFY(window->property("modalOpen").toBool());
            QVERIFY(!window->property("readerKeys").toBool());
            QTRY_COMPARE(prop("contextMenu","currentIndex").toInt(),0);
        };
        {
            open();
            const auto selected = bible->selectionReference();
            const auto isolatedKey = [this,&selected,&annotations](Qt::Key code, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
                key(code,modifiers);
                QVERIFY2(prop("contextMenu","visible").toBool(),qPrintable(QString("Menu closed on key %1 modifiers %2").arg(code).arg(int(modifiers))));
                QCOMPARE(bible->selectionReference(),selected);
                QCOMPARE(bible->collection(),annotations);
            };
            for (auto code : {Qt::Key_J,Qt::Key_M,Qt::Key_X,Qt::Key_F}) isolatedKey(code);
            for (auto code : {Qt::Key_K,Qt::Key_Space,Qt::Key_D,Qt::Key_R,Qt::Key_V,Qt::Key_Slash}) isolatedKey(code,Qt::ControlModifier);
            for (auto code : {Qt::Key_N,Qt::Key_L}) isolatedKey(code,Qt::AltModifier);
            isolatedKey(Qt::Key_A,Qt::ShiftModifier);
            QVERIFY(!bible->focusMode());
            QVERIFY(!prop("passagePalette","visible").toBool());
            // Down skips the divider to Edit range; Up returns to Open.
            key(Qt::Key_Down);
            QCOMPARE(prop("contextMenu","currentIndex").toInt(),2);
            QCOMPARE(bible->selectionReference(),selected);
            key(Qt::Key_Up);
            QCOMPARE(prop("contextMenu","currentIndex").toInt(),0);
            key(Qt::Key_Return);
            QTRY_VERIFY(!prop("contextMenu","visible").toBool());
            QCOMPARE(bible->selectionReference(),QString::fromUtf8("John 3:1–2"));
            QCOMPARE(bible->collection(),annotations);
            QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
            QCOMPARE(prop("noteEditor","text").toString(),QString("First range note"));
            key(Qt::Key_Escape);
            QTRY_VERIFY(!window->property("notesOpen").toBool());
            reference("John 3:1-2");
        }
        open();
        key(Qt::Key_Escape);
        QTRY_VERIFY(!window->property("modalOpen").toBool());
        QCOMPARE(bible->selectionReference(),QString::fromUtf8("John 3:1–2"));
    }
    void countedMotionsWithoutSelection() {
        for (bool block : {false,true}) {
            bible->setBlockMode(block);
            reference("John 3"); type("4j"); QCOMPARE(bible->selectionReference(),QString("John 3:4"));
            reference("John 3"); type("4k"); QCOMPARE(bible->selectionReference(),QString("John 3:33"));
            reference("John 3"); type("38"); key(Qt::Key_Down); QCOMPARE(bible->selectionReference(),QString("John 4:2"));
            reference("John 3"); type("38"); key(Qt::Key_Up); QCOMPARE(bible->selectionReference(),QString("John 2:24"));
            reference("Matthew 17"); type("21j"); QCOMPARE(bible->selectionReference(),QString("Matthew 17:22"));
            reference("Matthew 17"); type("7k"); QCOMPARE(bible->selectionReference(),QString("Matthew 17:20"));
            reference("John 3"); key(Qt::Key_J); QCOMPARE(bible->cursorVerse(),1);
            reference("John 3"); key(Qt::Key_K); QCOMPARE(bible->cursorVerse(),36);
            reference("John 3"); type("999999j"); QCOMPARE(bible->selectionReference(),QString("Revelation 22:21"));
            reference("John 3"); type("999999k"); QCOMPARE(bible->selectionReference(),QString("Genesis 1:1"));
        }
    }
    void composableMotions() {
        reference("John 3:16");
        type("4k"); QCOMPARE(bible->cursorVerse(),12);
        QVERIFY(window->property("verseCount").toString().isEmpty());
        key(Qt::Key_J); QCOMPARE(bible->cursorVerse(),13);
        type("3"); key(Qt::Key_Down); QCOMPARE(bible->cursorVerse(),16);
        type("2"); key(Qt::Key_Up); QCOMPARE(bible->cursorVerse(),14);
        type("4"); key(Qt::Key_J,Qt::ShiftModifier);
        QVERIFY(window->property("verseMode").toBool());
        QCOMPARE(bible->selectedStart(),14); QCOMPARE(bible->cursorVerse(),18);
        type("2"); key(Qt::Key_K,Qt::ShiftModifier); QCOMPARE(bible->cursorVerse(),16); QCOMPARE(bible->selectedStart(),14);
        type("3"); key(Qt::Key_Down,Qt::ShiftModifier); QCOMPARE(bible->cursorVerse(),19);
        type("2"); key(Qt::Key_Up,Qt::ShiftModifier); QCOMPARE(bible->cursorVerse(),17);
        type("2j"); QCOMPARE(bible->cursorVerse(),19); QCOMPARE(bible->selectedStart(),14);
        type("3k"); QCOMPARE(bible->cursorVerse(),16); QCOMPARE(bible->selectedStart(),14);
        QVERIFY(window->property("verseCount").toString().isEmpty());

        for (bool visual : {false,true}) {
            reference("John 3:34");
            if (visual) key(Qt::Key_V);
            type("4j");
            QCOMPARE(bible->chapter(),4); QCOMPARE(bible->cursorVerse(),2);
            QCOMPARE(bible->crossChapterSelection(),visual);
            if (visual) QCOMPARE(bible->selectionReference(),QString::fromUtf8("John 3:34–4:2"));
            type("4k");
            QCOMPARE(bible->chapter(),3); QCOMPARE(bible->cursorVerse(),34);
            QCOMPARE(bible->selectedStart(),34); QCOMPARE(bible->selectedEnd(),34);
        }
        reference("John 3:35");
        type("4"); key(Qt::Key_J,Qt::ShiftModifier); QCOMPARE(bible->selectionReference(),QString::fromUtf8("John 3:35–4:3"));
        type("6"); key(Qt::Key_K,Qt::ShiftModifier); QCOMPARE(bible->selectionReference(),QString::fromUtf8("John 3:33–35"));
        QCOMPARE(bible->cursorVerse(),33);
        reference("Matthew 17:20");
        type("3j"); QCOMPARE(bible->cursorVerse(),24); // Omitted verse 21 is not a row.
        type("3k"); QCOMPARE(bible->cursorVerse(),20);

        for (auto forward : {Qt::Key_D,Qt::Key_F,Qt::Key_PageDown}) {
            const auto backward = forward == Qt::Key_D ? Qt::Key_U : forward == Qt::Key_F ? Qt::Key_B : Qt::Key_PageUp;
            const auto modifiers = forward == Qt::Key_PageDown ? Qt::NoModifier : Qt::ControlModifier;
            const int step = forward == Qt::Key_D ? 5 : 10;
            reference("John 3:1");
            type("2"); key(forward,modifiers); QCOMPARE(bible->cursorVerse(),1 + 2 * step);
            type("2"); key(backward,modifiers); QCOMPARE(bible->cursorVerse(),1);
            reference("John 3:34"); key(Qt::Key_V);
            type("2"); key(forward,modifiers); QCOMPARE(bible->cursorVerse(),36);
            type("2"); key(forward,modifiers);
            QCOMPARE(bible->selectionReference(),QString::fromUtf8("John 3:34–4:1"));
        }

        reference("John 3:16");
        type("4"); key(Qt::Key_Home); QCOMPARE(bible->cursorVerse(),1);
        QVERIFY(window->property("verseCount").toString().isEmpty());
        type("4"); key(Qt::Key_End); QCOMPARE(bible->cursorVerse(),36);
        QVERIFY(window->property("verseCount").toString().isEmpty());
        type("20g"); QCOMPARE(bible->cursorVerse(),20);
        type("12"); key(Qt::Key_G,Qt::ShiftModifier); QCOMPARE(bible->cursorVerse(),12);
        type("2l"); QCOMPARE(bible->chapter(),5);
        type("3h"); QCOMPARE(bible->chapter(),2);
        type("2"); key(Qt::Key_Right); QCOMPARE(bible->chapter(),4);
        type("2"); key(Qt::Key_Left); QCOMPARE(bible->chapter(),2);
        reference("John 21:1");
        type("2l"); QCOMPARE(bible->passage(),QString("Acts 2"));
        type("2h"); QCOMPARE(bible->passage(),QString("John 21"));
        type("999999l"); QCOMPARE(bible->passage(),QString("Revelation 22"));
        type("999999h"); QCOMPARE(bible->passage(),QString("Genesis 1"));
        reference("John 3:16");
        type("999999j"); QCOMPARE(bible->selectionReference(),QString("Revelation 22:21"));
        type("999999k"); QCOMPARE(bible->selectionReference(),QString("Genesis 1:1"));

        type("4"); key(Qt::Key_F1); QTRY_VERIFY(item("keybindingSearch")->hasActiveFocus());
        type("3j"); QCOMPARE(prop("keybindingSearch","text").toString(),QString("3j"));
        QVERIFY(window->property("verseCount").toString().isEmpty());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("helpDialog","visible").toBool());
        key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        type("4k 3J"); QCOMPARE(prop("noteEditor","text").toString(),QString("4k 3J"));
        QVERIFY(window->property("verseCount").toString().isEmpty());
    }
    void toggleFeedbackAndFailures() {
        reference("John 3:16");
        const auto checkToast = [this](const QString &text) {
            QCOMPARE(prop("toast","text").toString(),text);
            QVERIFY(item("toast")->isVisible());
        };
        key(Qt::Key_F); QVERIFY(bible->focusMode()); checkToast("Focus mode on");
        QVERIFY(item("chapterTitle")->isVisible());
        QVERIFY(!window->findChild<QQuickItem *>("editionLabel"));
        QVERIFY(window->property("footerQuiet").toBool());
        QVERIFY(!item("verseHeading1")->isVisible());
        QVERIFY(!item("verseNumberLabel1")->isVisible());
        for (const auto &toggle : QList<QPair<Qt::Key,QString>>{{Qt::Key_N,"Show verse numbers"},
                {Qt::Key_H,"Show headings"},{Qt::Key_P,"Verse picker"},
                {Qt::Key_V,"Visual selection"}}) {
            const bool initiallyOn = toggle.first != Qt::Key_V;
            const auto modifiers = toggle.first == Qt::Key_V ? Qt::NoModifier : Qt::AltModifier;
            key(toggle.first,modifiers); checkToast(toggle.second + (initiallyOn ? " off" : " on"));
            key(toggle.first,modifiers); checkToast(toggle.second + (initiallyOn ? " on" : " off"));
        }
        for (const auto mode : {"Block","Paragraph","Verse"}) {
            key(Qt::Key_L,Qt::AltModifier); checkToast(QString("Reading mode ") + mode);
            QTRY_VERIFY(!window->property("restoringPosition").toBool());
        }
        const auto previousToast = prop("toast","text");
        key(Qt::Key_W); QTRY_VERIFY(prop("notesDrawer","opened").toBool()); QCOMPARE(prop("toast","text"),previousToast);
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("notesDrawer","visible").toBool()); QCOMPARE(prop("toast","text"),previousToast);
        key(Qt::Key_Equal,Qt::ControlModifier | Qt::AltModifier); checkToast("Line height 1.6");
        key(Qt::Key_0,Qt::ControlModifier | Qt::AltModifier); checkToast("Line height 1.5");
        QTRY_VERIFY_WITH_TIMEOUT(!item("toast")->isVisible(),4000);
        QVERIFY(window->property("footerQuiet").toBool());
        key(Qt::Key_F); QVERIFY(!bible->focusMode()); checkToast("Focus mode off");
        QTRY_VERIFY_WITH_TIMEOUT(!item("toast")->isVisible(),4000);
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","ui-toggle-failure");
            db.setDatabaseName(bible->dataDirectory() + "/personal.sqlite");
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec("CREATE TRIGGER reject_preferences BEFORE UPDATE ON preferences BEGIN SELECT RAISE(ABORT,'preference disk failure'); END"));
            const auto toast = prop("toast","text");
            for (auto toggle : {Qt::Key_N,Qt::Key_H,Qt::Key_P,Qt::Key_L,Qt::Key_F}) {
                key(toggle,toggle == Qt::Key_F ? Qt::NoModifier : Qt::AltModifier);
                QVERIFY(bible->error().contains("preference disk failure"));
                QCOMPARE(prop("toast","text"),toast);
                QVERIFY(!item("toast")->isVisible());
            }
            QVERIFY(bible->showVerseNumbers()); QVERIFY(bible->showHeadings()); QVERIFY(bible->versePicker());
            QVERIFY(!bible->blockMode()); QVERIFY(!bible->focusMode());
            QVERIFY(query.exec("DROP TRIGGER reject_preferences"));
            key(Qt::Key_N,Qt::AltModifier); checkToast("Show verse numbers off");
            key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
            QTRY_VERIFY(prop("notesDrawer","opened").toBool());
            QCOMPARE(prop("toast","text").toString(),QString("Show verse numbers off"));
            QVERIFY(query.exec("CREATE TRIGGER reject_note BEFORE INSERT ON annotations BEGIN SELECT RAISE(FAIL,'test note failure'); END"));
            type("An unsaved draft");
            key(Qt::Key_Escape);
            QVERIFY(prop("notesDrawer","opened").toBool());
            QCOMPARE(prop("toast","text").toString(),QString("Show verse numbers off"));
            QVERIFY(query.exec("DROP TRIGGER reject_note"));
            key(Qt::Key_Escape); QTRY_VERIFY(!prop("notesDrawer","visible").toBool());
            QCOMPARE(prop("toast","text").toString(),QString("Show verse numbers off"));
            QCOMPARE(bible->noteText(),QString("An unsaved draft"));
        }
        QSqlDatabase::removeDatabase("ui-toggle-failure");
    }
    void countedVerseAndCancellation() {
        reference("John 3:16");
        type("20g"); QCOMPARE(bible->cursorVerse(),20);
        type("12G"); QCOMPARE(bible->cursorVerse(),12);
        QSignalSpy selection(bible.get(),&BibleBackend::selectionChanged);
        QSignalSpy navigation(bible.get(),&BibleBackend::navigationChanged);
        const auto before = bible->selectionReference();
        type("999g"); QCOMPARE(bible->selectionReference(),before);
        type("0g"); QCOMPARE(bible->selectionReference(),before);
        QCOMPARE(selection.count(),0); QCOMPARE(navigation.count(),0);
        QVERIFY(bible->error().isEmpty());
        reference("Matthew 17:20");
        type("21g"); QCOMPARE(bible->cursorVerse(),20);
        QVERIFY(bible->error().isEmpty());
        reference("John 3:16");
        for (const auto action : {Qt::Key_J,Qt::Key_V,Qt::Key_M,Qt::Key_X,Qt::Key_Space,Qt::Key_Z}) {
            type("20"); QCOMPARE(window->property("verseCount").toString(),QString("20"));
            key(action);
            QVERIFY(window->property("verseCount").toString().isEmpty());
            if (action == Qt::Key_Space) { key(Qt::Key_Escape); QTRY_VERIFY(!prop("passagePalette","visible").toBool()); }
        }
        type("20"); key(Qt::Key_Escape);
        QVERIFY(!window->property("verseMode").toBool()); // The successful mark completed Visual mode.
        QVERIFY(window->property("verseCount").toString().isEmpty());
        type("20"); key(Qt::Key_K,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        QVERIFY(window->property("verseCount").toString().isEmpty());
        type("12"); QCOMPARE(prop("referenceInput","text").toString(),QString("12"));
        QTRY_VERIFY(prop("passagePalette","visible").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        type("20"); key(Qt::Key_A,Qt::ShiftModifier);
        QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        type("12g"); QVERIFY(window->property("verseCount").toString().isEmpty());
        key(Qt::Key_A,Qt::ShiftModifier); QTest::qWait(50); QVERIFY(prop("savedDrawer","visible").toBool()); key(Qt::Key_Escape); QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
        type("20"); key(Qt::Key_W);
        QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        QVERIFY(window->property("verseCount").toString().isEmpty());
        type("20g 123"); QCOMPARE(bible->noteText(),QString("20g 123"));
        key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("notesDrawer","visible").toBool());
        type("20"); item("passageControl")->forceActiveFocus(Qt::TabFocusReason);
        QVERIFY(window->property("verseCount").toString().isEmpty());
        item("readerSurface")->forceActiveFocus();
        type("20"); QTest::qWait(2100);
        QVERIFY(window->property("verseCount").toString().isEmpty());
        key(Qt::Key_G); QCOMPARE(bible->cursorVerse(),1);
    }
    void fiveAndTenVerseMovement() {
        for (bool visual : {false,true}) {
            for (auto forward : {Qt::Key_D,Qt::Key_F,Qt::Key_PageDown}) {
                const auto backward = forward == Qt::Key_D ? Qt::Key_U : forward == Qt::Key_F ? Qt::Key_B : Qt::Key_PageUp;
                const auto modifiers = forward == Qt::Key_PageDown ? Qt::NoModifier : Qt::ControlModifier;
                const int step = forward == Qt::Key_D ? 5 : 10;
                reference("John 3:34");
                if (visual) key(Qt::Key_V);
                key(forward,modifiers);
                QCOMPARE(bible->chapter(),3); QCOMPARE(bible->cursorVerse(),36);
                key(forward,modifiers);
                QCOMPARE(bible->chapter(),4); QCOMPARE(bible->cursorVerse(),1);
                QCOMPARE(bible->crossChapterSelection(),visual);
                QTRY_COMPARE(prop("reading","contentY").toDouble(),0.0);
                QVERIFY(item("chapterTitle")->mapToScene(QPointF()).y() >= 0);
                key(forward,modifiers); QCOMPARE(bible->cursorVerse(),1 + step);
                key(backward,modifiers); QCOMPARE(bible->cursorVerse(),1);
                key(backward,modifiers);
                QCOMPARE(bible->chapter(),3); QCOMPARE(bible->cursorVerse(),36);
                key(backward,modifiers); QCOMPARE(bible->cursorVerse(),36 - step);
                QCOMPARE(window->property("verseMode").toBool(),visual);
            }
        }
        reference("Matthew 17:20");
        key(Qt::Key_D,Qt::ControlModifier); QCOMPARE(bible->cursorVerse(),26);
        key(Qt::Key_U,Qt::ControlModifier); QCOMPARE(bible->cursorVerse(),20);
        reference("John 3");
        key(Qt::Key_F,Qt::ControlModifier); QCOMPARE(bible->cursorVerse(),11);
        key(Qt::Key_B,Qt::ControlModifier); QCOMPARE(bible->cursorVerse(),1);
        key(Qt::Key_M); QVERIFY(bible->selectionBookmarked());
        key(Qt::Key_Home); QCOMPARE(bible->cursorVerse(),1);
        key(Qt::Key_End); QCOMPARE(bible->cursorVerse(),36);
        key(Qt::Key_Right); QCOMPARE(bible->chapter(),4);
        key(Qt::Key_Left); QCOMPARE(bible->chapter(),3);
    }
    void verseMovementContext_data() {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<double>("scale");
        QTest::newRow("large") << QSize(1120,1000) << 1.0;
        QTest::newRow("small") << QSize(640,480) << 1.0;
        QTest::newRow("long-scaled") << QSize(640,480) << 1.8;
    }
    void verseMovementContext() {
        QFETCH(QSize,size); QFETCH(double,scale);
        window->resize(size); QVERIFY(desktop->setProperty("textScale",scale));
        reference("John 3:15");
        for (const auto movement : {Qt::Key_J,Qt::Key_K,Qt::Key_J,Qt::Key_Down,Qt::Key_Up}) {
            key(movement);
            // The 110 ms slide must land before measuring.
            QTest::qWait(20); QTRY_VERIFY(!object("readerSlide")->property("running").toBool());
            auto body = item(qPrintable("verseBody" + QString::number(bible->cursorVerse())));
            QVERIFY(body);
            const double y = body->mapToItem(item("reading"),QPointF()).y();
            const double available = item("reading")->height() - window->property("floatingSafety").toDouble();
            const double threeLines = window->property("readerLineHeight").toDouble() * 3;
            if (body->height() + 2 * threeLines <= available) {
                QVERIFY2(y >= threeLines - 1,qPrintable(QString("Top context %1 < %2").arg(y).arg(threeLines)));
                QVERIFY(y + body->height() <= available - threeLines + 1);
            } else {
                QVERIFY(y >= 12 * scale - 1);
                QVERIFY(y + qMin(body->height(),window->property("readerLineHeight").toDouble()) < available);
            }
        }
        key(Qt::Key_End); QTest::qWait(150);
        auto last = item("verseBody36");
        QVERIFY(last);
        if (last->height() < item("reading")->height() - window->property("floatingSafety").toDouble())
            QVERIFY(last->mapToItem(item("reading"),QPointF()).y() + last->height()
                <= item("reading")->height() - window->property("floatingSafety").toDouble() + 1);
    }
    void normalScrollingAndChapterNavigation() {
        reference("John 3");
        const double before = prop("reading","contentY").toDouble();
        key(Qt::Key_Space,Qt::ShiftModifier);
        QCOMPARE(prop("reading","contentY").toDouble(),before);
        QVERIFY(!prop("passagePalette","visible").toBool());
        QCOMPARE(bible->selectedStart(),0);
        key(Qt::Key_F,Qt::ControlModifier);
        QTRY_VERIFY(!object("readerSlide")->property("running").toBool());
        const double paged = prop("reading","contentY").toDouble();
        key(Qt::Key_B,Qt::ControlModifier);
        QTRY_VERIFY(!object("readerSlide")->property("running").toBool());
        QVERIFY(prop("reading","contentY").toDouble() < paged);
        key(Qt::Key_Right,Qt::AltModifier);
        QCOMPARE(bible->chapter(),3);
        key(Qt::Key_Left,Qt::AltModifier);
        QTRY_COMPARE(bible->chapter(),3);
        key(Qt::Key_L); QCOMPARE(bible->chapter(),4);
        key(Qt::Key_H); QCOMPARE(bible->chapter(),3);
        key(Qt::Key_G); QCOMPARE(bible->cursorVerse(),1);
        key(Qt::Key_G,Qt::ShiftModifier); QCOMPARE(bible->cursorVerse(),36);
        QCOMPARE(bible->selectedStart(),36); QCOMPARE(bible->selectedEnd(),36);
    }
    void positionSurvivesRestart() {
        reference("Psalm 119");
        QVERIFY(QMetaObject::invokeMethod(window,"scrollBy",Q_ARG(QVariant,900)));
        QTRY_VERIFY(bible->readingPosition() > 0);
        const double position = bible->readingPosition();
        unloadUi(); loadUi();
        QCOMPARE(bible->book(),19); QCOMPARE(bible->chapter(),119);
        const double travel = prop("reading","contentHeight").toDouble() - item("reading")->height();
        QVERIFY(travel > 0);
        QVERIFY(qAbs(prop("reading","contentY").toDouble() / travel - position) < 0.01);
    }
    void positionWithSelectionSurvivesRestart() {
        reference("Psalm 119:16");
        QVERIFY(QMetaObject::invokeMethod(window,"scrollBy",Q_ARG(QVariant,900)));
        QTest::qWait(450);
        const double position = bible->readingPosition();
        QVERIFY(position > 0);
        unloadUi(); loadUi();
        QCOMPARE(bible->selectedStart(),16);
        const double travel = prop("reading","contentHeight").toDouble() - item("reading")->height();
        QVERIFY(travel > 0);
        QVERIFY(qAbs(prop("reading","contentY").toDouble() / travel - position) < 0.01);
    }
    void revisedShortcutsAndTypingGuards() {
        reference("John 3:16");
        QCOMPARE(window->title(),QString("John 3 — OmaWord"));
        capture("revision-reading-icons");
        key(Qt::Key_K,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("Psalms");
        QCOMPARE(prop("referenceInput","text").toString(),QString("Psalms"));
        // Ctrl and Alt app shortcuts are inactive while typing.
        key(Qt::Key_R,Qt::ControlModifier);
        QCOMPARE(prop("referenceInput","text").toString(),QString("Psalms"));
        QVERIFY(!prop("settingsDialog","visible").toBool());
        key(Qt::Key_N,Qt::AltModifier); QVERIFY(bible->showVerseNumbers());
        key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        key(Qt::Key_R,Qt::ControlModifier);
        QTRY_VERIFY(prop("settingsDialog","opened").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("settingsDialog","visible").toBool());
        key(Qt::Key_W);
        QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        QVERIFY(!item("toast")->isVisible());
        type("notes s "); key(Qt::Key_R,Qt::ControlModifier); key(Qt::Key_Space,Qt::ControlModifier);
        key(Qt::Key_K,Qt::ControlModifier); key(Qt::Key_Question);
        QVERIFY(prop("noteEditor","text").toString().startsWith("notes s ?"));
        key(Qt::Key_H,Qt::AltModifier);
        item("noteEditor")->setProperty("text",QString("notes s ?"));
        QCOMPARE(prop("noteEditor","text").toString(),QString("notes s ?"));
        QVERIFY(!prop("settingsDialog","visible").toBool());
        QVERIFY(!prop("passagePalette","visible").toBool());
        QVERIFY(!prop("helpDialog","visible").toBool());
        QVERIFY(bible->showHeadings());
        // ctrl v pastes into the note instead of opening Versions.
        QApplication::clipboard()->setText("pasted");
        key(Qt::Key_End); key(Qt::Key_V,Qt::ControlModifier);
        QCOMPARE(prop("noteEditor","text").toString(),QString("notes s ?pasted"));
        QVERIFY(!prop("versionDialog","visible").toBool());
        // Outside the editor (preview), the same keys work again.
        click(item("previewButton"));
        QTRY_VERIFY(!item("noteEditor")->hasActiveFocus());
        for (const auto commandKey : {Qt::Key_Space,Qt::Key_P}) {
            const auto modifiers = commandKey == Qt::Key_P ? Qt::ControlModifier | Qt::ShiftModifier : Qt::ControlModifier;
            key(commandKey,modifiers);
            QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
            QCOMPARE(prop("referenceInput","text").toString(),QString("> "));
            type("settings");
            QCOMPARE(prop("referenceInput","text").toString(),QString("> settings"));
            key(Qt::Key_Escape); QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        }
        key(Qt::Key_S);
        QVERIFY(!prop("settingsDialog","visible").toBool());
        QVERIFY(!prop("passagePalette","visible").toBool());
        key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("notesDrawer","visible").toBool());
        QVERIFY(!item("toast")->isVisible());
        QCOMPARE(bible->noteText(),QString("notes s ?pasted"));
        key(Qt::Key_F1); QTRY_VERIFY(item("keybindingSearch")->hasActiveFocus());
        type("settings"); key(Qt::Key_R,Qt::ControlModifier);
        QCOMPARE(prop("keybindingSearch","text").toString(),QString("settings"));
        QVERIFY(!prop("settingsDialog","visible").toBool());
    }
    void uppercaseLettersTypeWhileWriting() {
        reference("John 3:16");
        key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        // Supply the actual shifted text: QTest::keyClick(Qt::Key_V, Shift)
        // otherwise synthesizes lowercase text even with the Shift modifier.
        for (const auto &letter : {QString("V"),QString("R"),QString("A"),QString("D"),QString("L")})
            QTest::sendKeyEvent(QTest::Click,window,Qt::Key(letter.at(0).unicode()),letter,Qt::ShiftModifier);
        QCOMPARE(prop("noteEditor","text").toString(),QString("VRADL"));
        QCOMPARE(bible->noteText(),QString("VRADL"));
        QVERIFY(item("noteEditor")->hasActiveFocus());
        QVERIFY(!window->property("modalOpen").toBool());
        QVERIFY(!prop("versionDialog","visible").toBool());
        QVERIFY(!prop("savedDrawer","visible").toBool());
        QVERIFY(!prop("deskDrawer","visible").toBool());
        click(item("previewButton"));
        key(Qt::Key_V,Qt::ControlModifier);
        QTRY_VERIFY(prop("versionDialog","opened").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("versionDialog","visible").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("notesDrawer","visible").toBool());
        key(Qt::Key_V,Qt::ControlModifier);
        QTRY_VERIFY(prop("versionDialog","opened").toBool());
    }
    void newRootSignal() {
        reference("John 3:16-18");
        const auto selected = bible->selectionReference();
        QSignalSpy requested(window,SIGNAL(newWindowRequested()));
        QVERIFY(requested.isValid());
        key(Qt::Key_N,Qt::ControlModifier);
        QCOMPARE(requested.count(),1);
        QVERIFY(!window->property("notesOpen").toBool());
        key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        QVERIFY(window->setProperty("syncingNote",true));
        QVERIFY(object("noteEditor")->setProperty("text","Save before opening a window"));
        QVERIFY(window->setProperty("syncingNote",false));
        // While typing, ctrl n is the editor's; from the Notes panel it opens a window.
        key(Qt::Key_N,Qt::ControlModifier);
        QCOMPARE(requested.count(),1);
        item("notesPanel")->forceActiveFocus();
        key(Qt::Key_N,Qt::ControlModifier);
        QCOMPARE(requested.count(),2);
        QCOMPARE(bible->noteText(),QString("Save before opening a window"));
        key(Qt::Key_Space,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("new window");
        key(Qt::Key_N,Qt::ControlModifier);
        QCOMPARE(requested.count(),2); // A modal owns its keyboard until closed.
        QCOMPARE(prop("paletteResults","count").toInt(),1);
        key(Qt::Key_Return);
        QTRY_COMPARE(requested.count(),3);
        QVERIFY(!prop("passagePalette","visible").toBool());
        item("notesPanel")->forceActiveFocus();
        key(Qt::Key_F1); QTRY_VERIFY(item("keybindingSearch")->hasActiveFocus());
        type("new window");
        QCOMPARE(prop("keybindingResults","count").toInt(),1);
        key(Qt::Key_Return);
        QTRY_COMPARE(requested.count(),4);
        QVERIFY(!prop("helpDialog","visible").toBool());
        QCOMPARE(bible->selectionReference(),selected);
        QCOMPARE(engine->rootObjects().size(),1); // UI requests creation; the host owns it.
    }
    void blockDeferredWorkEndsWithReader() {
        bible->setReadingMode("block");
        QTRY_VERIFY(blockMapping());
        QTRY_VERIFY(blockTextDocument() && !blockTextDocument()->isEmpty());
        // Queue a rebuild and geometry refresh, then destroy only the reader
        // while its engine remains alive to deliver any escaped callbacks.
        for (int i = 0; i < 3; ++i) {
            bible->setLineHeight(1.6 + i * 0.1);
            bible->setFontSize(19 + i);
            bible->setReadingMode("verse");
            QTRY_VERIFY(!blockMapping());
            QTest::qWait(50);
            QVERIFY2(qmlWarnings.isEmpty(),qPrintable(qmlWarnings.join('\n')));
            bible->setReadingMode("block");
            QTRY_VERIFY(blockMapping());
            QTRY_VERIFY(blockTextDocument() && !blockTextDocument()->isEmpty());
        }
        bible->setLineHeight(2.0);
        bible->setFontSize(24);
        window->close();
        delete window;
        window = nullptr;
        QTest::qWait(50);
        QVERIFY2(qmlWarnings.isEmpty(),qPrintable(qmlWarnings.join('\n')));
    }
    void independentWindowUiShortcuts() {
        QSignalSpy first(window,SIGNAL(newWindowRequested()));
        QVERIFY(first.isValid());
        // Two UI roots exercise shortcut ownership without the host's window factory.
        MockVersionCatalog secondCatalog;
        QQmlApplicationEngine secondEngine;
        secondEngine.rootContext()->setContextProperty("bible",bible.get());
        secondEngine.rootContext()->setContextProperty("desktop",desktop.get());
        secondEngine.rootContext()->setContextProperty("sounds",sounds.get());
        secondEngine.rootContext()->setContextProperty("catalog",&secondCatalog);
        connect(&secondEngine,&QQmlEngine::warnings,this,[this](const QList<QQmlError> &errors) {
            for (const auto &error : errors) qmlWarnings.append(error.toString());
        });
        secondEngine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!secondEngine.rootObjects().isEmpty());
        auto second = qobject_cast<QQuickWindow *>(secondEngine.rootObjects().first());
        QVERIFY(second && second != window);
        QSignalSpy other(second,SIGNAL(newWindowRequested()));
        QVERIFY(other.isValid());
        second->show(); second->requestActivate();
        QVERIFY(QTest::qWaitForWindowExposed(second));
        QTRY_VERIFY(second->isActive());
        QTest::keyClick(second,Qt::Key_N,Qt::ControlModifier);
        QTRY_COMPARE(other.count(),1);
        QCOMPARE(first.count(),0);
        window->requestActivate(); QTRY_VERIFY(window->isActive());
        key(Qt::Key_N,Qt::ControlModifier);
        QCOMPARE(first.count(),1); QCOMPARE(other.count(),1);
        key(Qt::Key_F1); QTRY_VERIFY(prop("helpDialog","opened").toBool());
        QVERIFY(!second->property("modalOpen").toBool());
        second->requestActivate(); QTRY_VERIFY(second->isActive());
        QTest::keyClick(second,Qt::Key_N,Qt::ControlModifier);
        QTRY_COMPARE(other.count(),2);
        QCOMPARE(first.count(),1);
        second->close();
        window->requestActivate(); QTRY_VERIFY(window->isActive());
    }
    void revisedKeybindingRecords() {
        const auto records = window->property("actions").value<QJSValue>().toVariant().toList();
        QVERIFY(!records.isEmpty());
        QStringList ids;
        QHash<QString,QVariantMap> byId;
        for (const auto &record : records) {
            const auto action = record.toMap();
            const auto id = action.value("id").toString();
            QVERIFY(!id.isEmpty()); QVERIFY(!ids.contains(id));
            ids.append(id); byId.insert(id,action);
            QVERIFY(action.value("title") != "Repeat a motion");
            QVERIFY(action.value("title") != "Select text across verses");
        }
        const QList<QStringList> expected = {
            {"books","Navigate to a passage","ctrl k"},
            {"annotations","Annotations panel","shift A"},
            {"selectVerse","Select verse","click"},
            {"clickMenu","Open context menu","right click"},
            {"middleCopy","Copy passage as quotation","middle click"},
            {"contextMenu","Open context menu","menu · shift F10"},
            {"settings","Reading settings","ctrl r"},
            {"commands","Open commands","ctrl space"},
            {"newWindow","New window","ctrl n"},
            {"switchVersion","Switch version","ctrl v"},
            {"bibleSearch","Search Bible","ctrl /"}, {"help","Keybindings","?"},
            {"numbers","Show verse numbers","alt n"}, {"headings","Show headings","alt h"},
            {"versePicker","Verse picker","alt p"}, {"readingMode","Reading mode","alt l"},
            {"soundEffects","Sound effects","alt s"}, {"reduceMotion","Animations","alt m"},
            {"lineMore","Increase line height","ctrl alt ="}, {"lineLess","Decrease line height","ctrl alt -"},
            {"lineReset","Reset line height","ctrl alt 0"}, {"manageTags","Manage tags","ctrl t"},
            {"previousChapter","Previous chapter",QString::fromUtf8("h · ←")}, {"bookmark","Bookmark selection","m"},
            {"highlight","Highlight selection","x"},
            {"removeVersion","Remove version",""},
            {"import","Add version",""}
        };
        for (const auto &record : expected) {
            QCOMPARE(byId[record[0]].value("title").toString(),record[1]);
            QVERIFY(!byId[record[0]].contains("keys"));
            QCOMPARE(formatKeys(byId[record[0]].value("sequences").toStringList()),record[2]);
        }
        QVERIFY(!ids.contains("toggleNotes")); QVERIFY(!ids.contains("tagsBrowser")); QVERIFY(!ids.contains("lineHeight"));
        for (const auto &pair : QList<QStringList>{{"previousVerse","goToVerse"},{"goToVerse","first"},
                {"nextTen","previousChapter"},{"nextChapter","visual"},{"extendPrevious","chapterFind"},
                {"bibleSearch","bookmark"},{"manageTags","copy"},{"clickMenu","annotations"},
                {"related","books"},{"newWindow","focus"}})
            QVERIFY2(ids.indexOf(pair[0]) < ids.indexOf(pair[1]),qPrintable(pair.join(" before ")));
        key(Qt::Key_F1); QTRY_VERIFY(prop("helpDialog","opened").toBool());
        // Hints use stable action IDs: quotation/plain-text titles intentionally repeat.
        for (const auto &record : records) {
            const auto action = record.toMap();
            if (!action.contains("sequences") && !action.value("explanation").toBool()) continue;
            const auto name = "keybindingHint" + action.value("id").toString();
            QTRY_VERIFY(object(qPrintable(name)));
            const auto keys = formatKeys(action.value("sequences").toStringList());
            QCOMPARE(plainHint(prop(qPrintable(name),"text").toString()),keys);
            // Keys in the foreground; the "·" separator muted.
            const auto rich = prop(qPrintable(name),"text").toString();
            if (keys.contains(QString::fromUtf8(" · ")))
                QVERIFY(rich.contains(QString::fromUtf8("<font color=\"%1\"> · </font>").arg(desktop->property("muted").value<QColor>().name())));
        }
        capture("revision-keybindings");
    }
    void catalogKeysCoverAndNeverCollide() {
        // Every action has a key except the Commands-only group; within one
        // context (the app, the Desk, list panels) no key is bound twice.
        const auto records = window->property("actions").value<QJSValue>().toVariant().toList();
        QHash<QString,QString> owners;
        for (const auto &record : records) {
            const auto action = record.toMap();
            const auto id = action.value("id").toString();
            const auto sequences = action.value("sequences").toStringList();
            if (action.value("group").toString() == "Commands only") {
                QVERIFY2(sequences.isEmpty(),qPrintable(id));
                QVERIFY2(action.value("command").toBool(),qPrintable(id));
                continue;
            }
            QVERIFY2(!sequences.isEmpty(),qPrintable(id + " has no key"));
            QVERIFY2(!action.value("group").toString().isEmpty(),qPrintable(id));
            if (action.value("explanation").toBool()) continue;
            const auto scope = action.value("scope").toString();
            const auto context = scope == "desk" || scope == "list" ? scope : QString("app");
            for (const auto &sequence : sequences + action.value("aliases").toStringList()) {
                const auto key = context + ":" + QKeySequence(sequence).toString();
                QVERIFY2(!owners.contains(key),qPrintable(sequence + " bound by " + owners.value(key) + " and " + id));
                owners.insert(key,id);
            }
        }
        for (const auto &removed : {"S","Ctrl+P","Ctrl+M","Ctrl+H","Ctrl+A","Shift+R","Shift+V","Shift+X","Ctrl+Shift+N",
                                    "R","P","B","Ctrl+[","Ctrl+]","Ctrl+\\"})
            QVERIFY2(!owners.contains("app:" + QKeySequence(QString(removed)).toString()),removed);
        // [ ] - and shift H moved from chapters/headings to History.
        for (const auto &[sequence,id] : QList<QPair<QString,QString>>{{"[","historyBack"},{"]","historyForward"},{"-","historySwap"},{"Shift+H","history"}})
            QCOMPARE(owners.value("app:" + QKeySequence(sequence).toString()),id);
    }
    void aliasesWorkButStayHidden() {
        reference("John 3:16");
        key(Qt::Key_F1); QTRY_VERIFY(prop("helpDialog","opened").toBool());
        for (const auto &row : QList<QStringList>{{"Keybindings","help","?"},{"Open commands","commands","ctrl space"},
                {"Go to verse number","goToVerse","20g"}}) {
            key(Qt::Key_A,Qt::ControlModifier); type(row[0]);
            QTRY_VERIFY(object(qPrintable("keybindingHint" + row[1])));
            QCOMPARE(plainHint(prop(qPrintable("keybindingHint" + row[1]),"text").toString()),row[2]);
        }
        for (const auto &query : {QString("keybindings f1"),QString("open commands ctrl shift p")}) {
            key(Qt::Key_A,Qt::ControlModifier); type(query);
            QTRY_COMPARE(prop("keybindingResults","count").toInt(),0);
        }
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("helpDialog","visible").toBool());
        key(Qt::Key_P,Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        QCOMPARE(prop("referenceInput","text").toString(),QString("> "));
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        type("5"); key(Qt::Key_G,Qt::ShiftModifier); QCOMPARE(bible->cursorVerse(),5);
    }
    void paletteModeKeysSwitchAndOneEscapeCloses() {
        reference("John 3:16");
        key(Qt::Key_K,Qt::ControlModifier); QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("John");
        // The current mode's key does nothing.
        key(Qt::Key_K,Qt::ControlModifier);
        QCOMPARE(prop("referenceInput","text").toString(),QString("John"));
        key(Qt::Key_Slash,Qt::ControlModifier);
        QCOMPARE(prop("referenceInput","text").toString(),QString("/ "));
        QVERIFY(window->property("paletteTextMode").toBool());
        type("love");
        key(Qt::Key_Slash,Qt::ControlModifier);
        QCOMPARE(prop("referenceInput","text").toString(),QString("/ love"));
        key(Qt::Key_Space,Qt::ControlModifier);
        QCOMPARE(prop("referenceInput","text").toString(),QString("> "));
        type("headings");
        key(Qt::Key_Space,Qt::ControlModifier);
        QCOMPARE(prop("referenceInput","text").toString(),QString("> headings"));
        key(Qt::Key_P,Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(prop("referenceInput","text").toString(),QString("> headings"));
        QCOMPARE(plainHint(prop("paletteHints","text").toString()),QString::fromUtf8("↑ ↓ choose    enter select    esc close"));
        // One esc closes with a query; space resumes it.
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        QTRY_VERIFY(item("readerSurface")->hasActiveFocus());
        key(Qt::Key_Space); QTRY_VERIFY(prop("passagePalette","opened").toBool());
        QCOMPARE(prop("referenceInput","text").toString(),QString("> headings"));
        key(Qt::Key_K,Qt::ControlModifier);
        QCOMPARE(prop("referenceInput","text").toString(),QString());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        // Plain s no longer searches.
        key(Qt::Key_S); QTest::qWait(50); QVERIFY(!prop("passagePalette","visible").toBool());
        key(Qt::Key_Slash,Qt::ControlModifier); QTRY_VERIFY(prop("passagePalette","opened").toBool());
        QCOMPARE(prop("referenceInput","text").toString(),QString("/ "));
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("passagePalette","visible").toBool());
    }
    void singleEscapeClosesModalsAndPanels() {
        reference("John 3:16");
        key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        type("draft");
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("notesDrawer","visible").toBool());
        QCOMPARE(bible->noteText(),QString("draft"));
        key(Qt::Key_Question); QTRY_VERIFY(item("keybindingSearch")->hasActiveFocus());
        type("bookmark");
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("helpDialog","visible").toBool());
        key(Qt::Key_T); QTRY_VERIFY(item("tagSearch")->hasActiveFocus());
        type("Unsaved query");
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("tagsSurface","visible").toBool());
        QVERIFY(bible->tags().isEmpty());
        key(Qt::Key_Slash); QTRY_VERIFY(item("chapterFindInput")->hasActiveFocus());
        type("love");
        key(Qt::Key_Escape); QTRY_VERIFY(!window->property("findPromptOpen").toBool());
        // The whole multi-step Remove version flow closes with one esc.
        QVERIFY(QMetaObject::invokeMethod(window,"runCommand",Q_ARG(QVariant,QString("removeVersion"))));
        QTRY_VERIFY(prop("versionDialog","opened").toBool());
        key(Qt::Key_Return); QTRY_COMPARE(prop("versionDialog","step").toInt(),1);
        key(Qt::Key_Return); QTRY_COMPARE(prop("versionDialog","step").toInt(),2);
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("versionDialog","visible").toBool());
        QVERIFY(bible->ready());
        QTRY_VERIFY(item("readerSurface")->hasActiveFocus());
    }
    void relatedSlashSearchAndTwoEscapes() {
        reference("John 3:16");
        QVERIFY(!bible->createAndAssignTag(bible->tagScope(),"Love").isEmpty());
        reference("John 3:17");
        QVERIFY(!bible->createAndAssignTag(bible->tagScope(),"Love").isEmpty());
        reference("John 3:16");
        key(Qt::Key_S, Qt::ShiftModifier); QTRY_VERIFY(prop("relatedDrawer","opened").toBool());
        key(Qt::Key_H); QTRY_COMPARE(prop("tagsUi","studyTab").toString(),QString("related"));
        QTRY_VERIFY(item("tagResults")->hasActiveFocus());
        QVERIFY(!item("studySearch")->isVisible());
        key(Qt::Key_Slash); QTRY_VERIFY(item("studySearch")->hasActiveFocus());
        QVERIFY(!item("studyTagCount")->isVisible()); // The field replaces the count line.
        type("zq"); QCOMPARE(prop("studySearch","text").toString(),QString("zq"));
        // esc in the field leaves it and keeps the query, shown in the count line.
        key(Qt::Key_Escape);
        QTRY_VERIFY(!item("studySearch")->hasActiveFocus());
        QVERIFY(item("tagResults")->hasActiveFocus());
        QVERIFY(prop("relatedDrawer","visible").toBool());
        QCOMPARE(prop("tagsUi","studyQuery").toString(),QString("zq"));
        QTRY_VERIFY(item("studyTagCount")->isVisible());
        QCOMPARE(prop("studyTagCount","text").toString(),QString::fromUtf8("0 of 1 passage · “zq”"));
        // esc with a query clears it; the next closes.
        key(Qt::Key_Escape); QCOMPARE(prop("tagsUi","studyQuery").toString(),QString());
        QVERIFY(prop("relatedDrawer","visible").toBool());
        QCOMPARE(prop("studyTagCount","text").toString(),QString("1 passage"));
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("relatedDrawer","visible").toBool());
        QTRY_VERIFY(item("readerSurface")->hasActiveFocus());
    }
    void sidePanelKeysSwitchPanels() {
        reference("John 3:16");
        QVERIFY(!bible->createAndAssignTag(bible->tagScope(),"Switch").isEmpty());
        key(Qt::Key_D, Qt::ShiftModifier); QTRY_VERIFY(prop("deskDrawer","opened").toBool());
        // The same panel's key does nothing.
        key(Qt::Key_D, Qt::ShiftModifier); QTest::qWait(50);
        QVERIFY(prop("deskDrawer","visible").toBool()); QVERIFY(prop("deskDrawer","opened").toBool());
        key(Qt::Key_A, Qt::ShiftModifier);
        QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        QTRY_VERIFY(!prop("deskDrawer","visible").toBool());
        QCOMPARE(window->property("sidePanel").toString(),QString("annotations"));
        key(Qt::Key_A, Qt::ShiftModifier); QTest::qWait(50); QVERIFY(prop("savedDrawer","opened").toBool());
        key(Qt::Key_S, Qt::ShiftModifier);
        QTRY_VERIFY(prop("relatedDrawer","opened").toBool());
        QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
        key(Qt::Key_W);
        QTRY_VERIFY(prop("notesDrawer","opened").toBool());
        QTRY_VERIFY(!prop("relatedDrawer","visible").toBool());
        QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        // While typing, panel keys type.
        QTest::sendKeyEvent(QTest::Click,window,Qt::Key_D,QString("D"),Qt::ShiftModifier);
        QCOMPARE(prop("noteEditor","text").toString(),QString("D"));
        QVERIFY(!prop("deskDrawer","visible").toBool());
        // Outside the editor, a draft that cannot save cancels the switch.
        auto db = QSqlDatabase::addDatabase("QSQLITE","ui-panel-switch");
        db.setDatabaseName(bible->dataDirectory() + "/personal.sqlite");
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec("CREATE TRIGGER reject_switch_note BEFORE UPDATE ON annotations BEGIN SELECT RAISE(FAIL,'switch note failure'); END"));
        QVERIFY(query.exec("CREATE TRIGGER reject_switch_insert BEFORE INSERT ON annotations BEGIN SELECT RAISE(FAIL,'switch note failure'); END"));
        type(" more");
        item("notesPanel")->forceActiveFocus();
        key(Qt::Key_D, Qt::ShiftModifier); QTest::qWait(80);
        QVERIFY(prop("notesDrawer","visible").toBool());
        QVERIFY(!prop("deskDrawer","visible").toBool());
        QVERIFY(bible->noteText() != QString("D more"));
        QVERIFY(query.exec("DROP TRIGGER reject_switch_note"));
        QVERIFY(query.exec("DROP TRIGGER reject_switch_insert"));
        db.close();
        key(Qt::Key_D, Qt::ShiftModifier);
        QTRY_VERIFY(prop("deskDrawer","opened").toBool());
        QTRY_VERIFY(!prop("notesDrawer","visible").toBool());
        QCOMPARE(bible->noteText(),QString("D more"));
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("deskDrawer","visible").toBool());
    }
    void annotationsListKeys() {
        reference("John 3:16");
        QVERIFY(bible->toggleBookmark());
        key(Qt::Key_A,Qt::ShiftModifier); QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        QTRY_VERIFY(item("collectionList")->hasActiveFocus());
        QApplication::clipboard()->clear();
        key(Qt::Key_Y); QTRY_VERIFY(QApplication::clipboard()->text().contains("John 3:16"));
        QVERIFY(QApplication::clipboard()->text().startsWith(QString::fromUtf8("“")));
        key(Qt::Key_Y,Qt::ShiftModifier);
        QTRY_VERIFY(!QApplication::clipboard()->text().contains("John 3:16"));
        QVERIFY(!QApplication::clipboard()->text().isEmpty());
        key(Qt::Key_D); QTRY_COMPARE(bible->desk().size(),1);
        key(Qt::Key_D); QTRY_COMPARE(bible->desk().size(),0);
        // x no longer removes; shift delete has no bulk clear here.
        key(Qt::Key_X); key(Qt::Key_Delete,Qt::ShiftModifier); QTest::qWait(60);
        QCOMPARE(bible->collection().size(),1); QVERIFY(!prop("deleteConfirmation","visible").toBool());
        key(Qt::Key_Delete); QTRY_VERIFY(prop("deleteConfirmation","opened").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("deleteConfirmation","visible").toBool());
        QCOMPARE(bible->collection().size(),1);
        QVERIFY(prop("savedDrawer","visible").toBool());
        key(Qt::Key_Backspace); QTRY_VERIFY(prop("deleteConfirmation","opened").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("deleteConfirmation","visible").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
    }
    void relatedListKeys() {
        reference("John 3:17");
        const auto other = bible->tagScope();
        QVERIFY(!bible->createAndAssignTag(other,"Sent").isEmpty());
        reference("John 3:16");
        QVERIFY(!bible->createAndAssignTag(bible->tagScope(),"Sent").isEmpty());
        key(Qt::Key_S, Qt::ShiftModifier); QTRY_VERIFY(prop("relatedDrawer","opened").toBool());
        key(Qt::Key_H); QTRY_VERIFY(item("tagResults")->hasActiveFocus());
        QTRY_COMPARE(prop("tagResults","count").toInt(),1);
        QApplication::clipboard()->clear();
        key(Qt::Key_Y); QTRY_VERIFY(QApplication::clipboard()->text().contains("John 3:17"));
        key(Qt::Key_Y,Qt::ShiftModifier);
        QTRY_VERIFY(!QApplication::clipboard()->text().contains("John 3:17"));
        key(Qt::Key_D); QTRY_COMPARE(bible->desk().size(),1);
        QCOMPARE(bible->desk().first().toMap().value("reference").toString(),QString("John 3:17"));
        key(Qt::Key_J); QCOMPARE(prop("tagResults","currentIndex").toInt(),0);
        // k from the first row climbs into the chips; j comes back.
        key(Qt::Key_K); QVERIFY(!item("tagResults")->hasActiveFocus());
        key(Qt::Key_J); QTRY_VERIFY(item("tagResults")->hasActiveFocus());
        key(Qt::Key_E,Qt::ShiftModifier); QTRY_VERIFY(window->property("rangeEditing").toBool());
        QTRY_VERIFY(!prop("relatedDrawer","visible").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!window->property("rangeEditing").toBool());
        QCOMPARE(bible->selectionReference(),QString("John 3:16"));
        if (!prop("relatedDrawer","visible").toBool()) { key(Qt::Key_S, Qt::ShiftModifier); QTRY_VERIFY(prop("relatedDrawer","opened").toBool()); }
        QTRY_VERIFY(item("tagResults")->hasActiveFocus());
        // Study never removes tags: delete is inert on its results.
        key(Qt::Key_Delete); QTest::qWait(50);
        QCOMPARE(bible->tagsForScope(other).size(),1);
        QCOMPARE(prop("tagResults","count").toInt(),1);
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("relatedDrawer","visible").toBool());
    }
    void annotationsSearchVisibilityAndEscapes() {
        for (int verse = 1; verse <= 10; ++verse) { reference(QString("John 3:%1").arg(verse)); QVERIFY(bible->toggleBookmark()); }
        key(Qt::Key_A,Qt::ShiftModifier); QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        QCOMPARE(prop("collectionList","count").toInt(),10);
        QVERIFY(!item("savedSearch")->isVisible());
        key(Qt::Key_Slash); QTest::qWait(50); QVERIFY(!item("savedSearch")->hasActiveFocus());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
        reference("John 3:11"); QVERIFY(bible->toggleBookmark());
        key(Qt::Key_A,Qt::ShiftModifier); QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        // Visible past ten items, but never focused on open.
        QTRY_VERIFY(item("savedSearch")->isVisible());
        QTRY_VERIFY(item("collectionList")->hasActiveFocus());
        key(Qt::Key_Slash); QTRY_VERIFY(item("savedSearch")->hasActiveFocus());
        type("3:1"); QTRY_COMPARE(prop("collectionList","count").toInt(),3); // 3:1, 3:10, 3:11
        type("1"); QTRY_COMPARE(prop("collectionList","count").toInt(),1);
        // First esc leaves the field and keeps the query; the next closes.
        key(Qt::Key_Escape);
        QTRY_VERIFY(item("collectionList")->hasActiveFocus());
        QVERIFY(prop("savedDrawer","visible").toBool());
        QCOMPARE(prop("savedSearch","text").toString(),QString("3:11"));
        // A query keeps the field even when the tab drops to ten items.
        QVERIFY(bible->toggleBookmark());
        QTRY_COMPARE(prop("collectionList","count").toInt(),0);
        QVERIFY(item("savedSearch")->isVisible());
        QVERIFY(item("savedEmpty")->isVisible());
        key(Qt::Key_Slash); QTRY_VERIFY(item("savedSearch")->hasActiveFocus());
        key(Qt::Key_A,Qt::ControlModifier); key(Qt::Key_Backspace);
        QTRY_VERIFY(!item("savedSearch")->isVisible());
        QTRY_COMPARE(prop("collectionList","count").toInt(),10);
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
        // Tags tab: its tagged passages count and filter too.
        for (int verse = 1; verse <= 12; ++verse) {
            reference(QString("John 4:%1").arg(verse));
            QVERIFY(!bible->createAndAssignTag(bible->tagScope(),"Water").isEmpty());
        }
        reference("John 4:1");
        key(Qt::Key_A,Qt::ShiftModifier); QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        key(Qt::Key_H); QTRY_COMPARE(window->property("collectionTab").toInt(),3);
        QTRY_VERIFY(item("savedSearch")->isVisible());
        QVERIFY(!item("savedSearch")->hasActiveFocus());
        QTRY_COMPARE(prop("collectionList","count").toInt(),12);
        key(Qt::Key_Slash); QTRY_VERIFY(item("savedSearch")->hasActiveFocus());
        type("4:12");
        QTRY_COMPARE(prop("collectionList","count").toInt(),1);
        QVERIFY(item("savedTagEntry:Water:John 4:12"));
        // A tag name keeps its whole group.
        key(Qt::Key_A,Qt::ControlModifier); type("wat");
        QTRY_COMPARE(prop("collectionList","count").toInt(),12);
        key(Qt::Key_A,Qt::ControlModifier); type("4:12");
        QTRY_COMPARE(prop("collectionList","count").toInt(),1);
        key(Qt::Key_Escape); QTRY_VERIFY(!item("savedSearch")->hasActiveFocus());
        QVERIFY(prop("savedDrawer","visible").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
        key(Qt::Key_A,Qt::ShiftModifier); QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        QCOMPARE(prop("savedSearch","text").toString(),QString());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
    }
    void versionsCtrlIAddsVersion() {
        reference("John 3:16");
        key(Qt::Key_V,Qt::ControlModifier); QTRY_VERIFY(item("versionSearch")->hasActiveFocus());
        type("kj");
        key(Qt::Key_I,Qt::ControlModifier);
        QTRY_VERIFY(!prop("versionDialog","visible").toBool());
        QTRY_VERIFY(prop("importGuide","opened").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("importGuide","visible").toBool());
        // Tab, then enter, still reaches +.
        key(Qt::Key_V,Qt::ControlModifier); QTRY_VERIFY(item("versionSearch")->hasActiveFocus());
        key(Qt::Key_Tab); QTRY_VERIFY(item("versionAdd")->hasActiveFocus());
        key(Qt::Key_Return);
        QTRY_VERIFY(prop("importGuide","opened").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("importGuide","visible").toBool());
        // Remove version has no add step.
        QVERIFY(QMetaObject::invokeMethod(window,"runCommand",Q_ARG(QVariant,QString("removeVersion"))));
        QTRY_VERIFY(prop("versionDialog","opened").toBool());
        key(Qt::Key_I,Qt::ControlModifier); QTest::qWait(60);
        QVERIFY(prop("versionDialog","visible").toBool()); QVERIFY(!prop("importGuide","visible").toBool());
        QVERIFY(!plainHint(prop("versionHints","text").toString()).contains("ctrl i"));
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("versionDialog","visible").toBool());
    }
    void footerOrderAndResponsiveLayout() {
        reference("1 Thessalonians 5:16-18");
        const QStringList names = {"helpControl","modeControl","searchControl","passageControl","versionControl",
            "layoutControl","savedControl","relatedControl","historyControl","deskControl","footerMore"};
        window->setMinimumWidth(320);
        struct Case { int width; int height; double scale; bool stacked; bool layout; bool collapsed; };
        const QList<Case> cases = {{1120,820,1,false,true,false},{900,820,1.5,true,true,false},{700,820,1,true,true,false},
            {700,820,1.5,true,true,false},{640,820,1.5,true,false,false},{560,820,1.5,true,false,true},{560,820,2.4,true,false,true},
            {420,640,1,true,false,true},{360,640,1.5,true,false,true}};
        for (const auto &entry : cases) {
            window->resize(entry.width,entry.height);
            QVERIFY(desktop->setProperty("textScale",entry.scale));
            QTRY_COMPARE(window->width(),entry.width);
            QTest::qWait(120);
            const auto tag = QString("%1@%2").arg(entry.width).arg(entry.scale);
            QVERIFY2(window->property("footerStacked").toBool() == entry.stacked,qPrintable(tag));
            QVERIFY2(item("layoutControl")->isVisible() == entry.layout,qPrintable(tag));
            QVERIFY2(item("footerMore")->isVisible() == entry.collapsed,qPrintable(tag));
            QCOMPARE(item("helpControl")->isVisible(),!entry.collapsed);
            QCOMPARE(item("savedControl")->isVisible(),!entry.collapsed);
            // The footer itself is always one row tall; only the right group stacks.
            QCOMPARE(window->property("footerHeight").toDouble(),window->property("footerButton").toDouble());
            const QStringList right = {"savedControl","relatedControl","historyControl","deskControl","footerMore"};
            QList<QPair<QString,QRectF>> rects;
            for (const auto &name : names) {
                auto button = item(qPrintable(name));
                if (button && button->isVisible()) rects.append({name,button->mapRectToScene(button->boundingRect())});
            }
            const double band = window->height() - window->property("floatingSafety").toDouble();
            const auto column = item("readerColumn")->mapRectToScene(item("readerColumn")->boundingRect());
            const double modeBottom = item("modeControl")->mapRectToScene(item("modeControl")->boundingRect()).bottom();
            for (int i = 0; i < rects.size(); ++i) {
                QVERIFY2(QRectF(0,0,entry.width,entry.height).contains(rects[i].second),qPrintable(tag + " " + rects[i].first));
                if (right.contains(rects[i].first) && entry.stacked && !entry.collapsed) {
                    // The stacked column sits right of the reader text, never over it.
                    QVERIFY2(rects[i].second.left() >= column.right(),qPrintable(tag + " " + rects[i].first));
                } else {
                    // Inside the reader's reserved bottom band: never over the text column.
                    QVERIFY2(rects[i].second.top() >= band - 0.5,qPrintable(tag + " " + rects[i].first));
                    QVERIFY2(qAbs(rects[i].second.bottom() - modeBottom) < 0.5,qPrintable(tag + " " + rects[i].first));
                }
                for (int j = i + 1; j < rects.size(); ++j)
                    QVERIFY2(!rects[i].second.intersects(rects[j].second),qPrintable(tag + " " + rects[i].first + " overlaps " + rects[j].first));
            }
            const auto location = item("locationControls")->mapRectToScene(item("locationControls")->boundingRect());
            QVERIFY2(qAbs(location.center().x() - entry.width / 2.0) <= 1,qPrintable(tag));
            const auto x = [this](const char *name) { return item(name)->mapToScene(QPointF()).x(); };
            const auto y = [this](const char *name) { return item(name)->mapToScene(QPointF()).y(); };
            const auto w = [this](const char *name) { return item(name)->width(); };
            const auto h = [this](const char *name) { return item(name)->height(); };
            QVERIFY(!item("notesControl"));
            // Buttons within a group sit edge to edge.
            const QList<const char *> order = {"savedControl","relatedControl","historyControl","deskControl"};
            if (!entry.collapsed && !entry.stacked) {
                for (int i = 1; i < order.size(); ++i) QCOMPARE(x(order[i]),x(order[i - 1]) + w(order[i - 1]));
            } else if (!entry.collapsed) {
                // A column in the same order, top to bottom, bottom-aligned with the row.
                for (int i = 1; i < order.size(); ++i) { QCOMPARE(x(order[i]),x("savedControl")); QCOMPARE(y(order[i]),y(order[i - 1]) + h(order[i - 1])); }
                QCOMPARE(item("deskControl")->mapRectToScene(item("deskControl")->boundingRect()).bottom(),modeBottom);
                QVERIFY(location.right() <= x("deskControl"));
            }
            if (!entry.collapsed) QCOMPARE(x("modeControl"),x("helpControl") + w("helpControl"));
            QCOMPARE(x("passageControl"),x("searchControl") + w("searchControl"));
            QCOMPARE(x("versionControl"),x("passageControl") + w("passageControl"));
            if (entry.layout) QVERIFY(x("versionControl") < x("layoutControl"));
            const auto passage = item("passageControl")->mapRectToScene(item("passageControl")->boundingRect());
            QCOMPARE(passage.top(),item("modeControl")->mapRectToScene(item("modeControl")->boundingRect()).top());
            capture("footer-" + tag);
        }
        // The "…" menu opens the folded panels.
        click(item("footerMore")); QTRY_VERIFY(prop("contextMenu","visible").toBool());
        QCOMPARE(prop("contextMenu","target").toString(),QString("footer"));
        QVERIFY(QMetaObject::invokeMethod(object("contextMenu"),"triggerId",Q_ARG(QVariant,QString("desk"))));
        QTRY_VERIFY(prop("deskDrawer","opened").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("deskDrawer","visible").toBool());
    }
    void layoutButtonCyclesWithDistinctIcons() {
        reference("John 3:16");
        auto layout = item("layoutControl"); QVERIFY(layout && layout->isVisible());
        QVERIFY(layout->property("tip").toString().contains("alt l"));
        const QStringList cycle = {"paragraph","verse","block"};
        QMap<QString,QImage> icons;
        for (int step = 0; step < 3; ++step) {
            const auto mode = bible->readingMode();
            QCOMPARE(layout->property("glyph").toString(),"layout-" + mode);
            QVERIFY(layout->property("tip").toString().contains(mode,Qt::CaseInsensitive));
            QSignalSpy frames(window,&QQuickWindow::frameSwapped);
            window->update(); QTRY_VERIFY(frames.count() > 0); QTest::qWait(80);
            icons.insert(mode,window->grabWindow().copy(layout->mapRectToScene(layout->boundingRect()).toRect()));
            capture("layout-icon-" + mode);
            click(layout);
            QTRY_COMPARE(bible->readingMode(),cycle[(cycle.indexOf(mode) + 1) % 3]);
            QTRY_VERIFY(!window->property("restoringPosition").toBool());
        }
        QCOMPARE(icons.size(),3);
        QVERIFY(icons["verse"] != icons["paragraph"]); QVERIFY(icons["paragraph"] != icons["block"]);
        QVERIFY(icons["verse"] != icons["block"]);
        capture("layout-icons");
        // alt l cycles too.
        const auto before = bible->readingMode();
        key(Qt::Key_L,Qt::AltModifier); QTRY_COMPARE(bible->readingMode(),cycle[(cycle.indexOf(before) + 1) % 3]);
        QCOMPARE(layout->property("glyph").toString(),"layout-" + bible->readingMode());
    }
    void keyNotation() {
        const QList<QPair<QStringList,QString>> cases = {
            {{"Shift+D"},"shift D"}, {{"Ctrl+K"},"ctrl k"}, {{"Alt+H"},"alt h"}, {{"X"},"x"},
            {{"Up"},QString::fromUtf8("↑")}, {{"Shift+Down"},QString::fromUtf8("shift ↓")},
            {{"Left","Right"},QString::fromUtf8("← · →")}, {{"PgUp"},"pgup"}, {{"PgDown"},"pgdn"},
            {{"Escape"},"esc"}, {{"Return"},"enter"}, {{"Delete"},"delete"}, {{"Shift+Del"},"shift delete"},
            {{"Backspace"},"backspace"}, {{"Space"},"space"}, {{"Tab"},"tab"}, {{"Home","End"},QString::fromUtf8("home · end")},
            {{"Z, Z"},"zz"}, {{"2, 0, G"},"20g"}, {{"2, 0, Shift+G"},"20G"}, {{"?"},"?"}, {{"/"},"/"},
            {{"Ctrl++","Ctrl+="},QString::fromUtf8("ctrl + · ctrl =")}, {{"Ctrl+-"},"ctrl -"},
            {{"Ctrl+Alt+="},"ctrl alt ="}, {{"Ctrl+Shift+C"},"ctrl shift C"}, {{"Shift+F10"},"shift F10"},
            {{"F1"},"F1"}, {{"Menu"},"menu"}, {{"RightClick"},"right click"}, {{"MiddleClick"},"middle click"},
            {{"Click"},"click"}, {{"Shift+Click"},"shift click"}};
        for (const auto &entry : cases) QCOMPARE(formatKeys(entry.first),entry.second);
        // Every catalog key reads in the shared notation: lowercase except a
        // shifted letter or function key, arrows as glyphs, muted separators.
        const QRegularExpression notation(QString::fromUtf8(
            "^((ctrl|alt|shift) )*([a-z0-9?/\\\\+=\\-\\[\\]{}]+|[0-9]+[A-Z]|[↑↓←→]|esc|enter|delete|backspace|space|tab|pgup|pgdn|home|end|menu|F\\d+|click|right click|middle click|[A-Z])$"));
        const auto records = window->property("actions").value<QJSValue>().toVariant().toList();
        for (const auto &record : records) {
            const auto action = record.toMap();
            for (const auto &list : {action.value("sequences").toStringList(),action.value("aliases").toStringList()}) {
                for (const auto &sequence : list) {
                    const auto text = formatKeys({sequence});
                    QVERIFY2(notation.match(text).hasMatch(),qPrintable(sequence + " -> " + text));
                    const bool shifted = text.startsWith("shift ") || text.contains(" shift ");
                    const auto last = text.section(' ',-1);
                    if (last.size() == 1 && last.at(0).isLetter())
                        QVERIFY2(last.at(0).isUpper() == shifted,qPrintable(text));
                }
            }
        }
    }
    void textSizeFeedbackOnlyOnChange() {
        const auto change = [this](const QString &action, int expected) {
            const auto before = window->property("readerSize").toInt();
            const auto meta = object("toast")->metaObject();
            QSignalSpy textChanges(object("toast"),meta->property(meta->indexOfProperty("text")).notifySignal());
            QVERIFY(textChanges.isValid());
            QVERIFY(QMetaObject::invokeMethod(window,"runCommand",Q_ARG(QVariant,action)));
            QCOMPARE(window->property("readerSize").toInt(),expected);
            QCOMPARE(textChanges.count(),before == expected ? 0 : 1);
            if (before != expected) {
                QCOMPARE(prop("toast","text").toString(),QString("Text size %1").arg(expected));
                QVERIFY(item("toast")->isVisible());
            }
        };
        QCOMPARE(window->property("readerSize").toInt(),18);
        change("resetText",18); QVERIFY(!item("toast")->isVisible());
        for (int size = 19; size <= 40; ++size) change("larger",size);
        QTRY_VERIFY_WITH_TIMEOUT(!item("toast")->isVisible(),4000);
        change("larger",40); QVERIFY(!item("toast")->isVisible());
        for (int size = 39; size >= 8; --size) change("smaller",size);
        QTRY_VERIFY_WITH_TIMEOUT(!item("toast")->isVisible(),4000);
        change("smaller",8); QVERIFY(!item("toast")->isVisible());
        change("resetText",18);
        QTRY_VERIFY_WITH_TIMEOUT(!item("toast")->isVisible(),4000);
        change("resetText",18); QVERIFY(!item("toast")->isVisible());
        key(Qt::Key_F); QVERIFY(bible->focusMode());
        key(Qt::Key_Equal,Qt::ControlModifier);
        QCOMPARE(prop("toast","text").toString(),QString("Text size 19"));
        QVERIFY(item("toast")->isVisible());
        key(Qt::Key_Minus,Qt::ControlModifier);
        QCOMPARE(prop("toast","text").toString(),QString("Text size 18"));
        key(Qt::Key_0,Qt::ControlModifier);
        QCOMPARE(window->property("readerSize").toInt(),18);
    }
    void versionPickerKeyboardAndGeometry_data() {
        QTest::addColumn<double>("scale");
        QTest::newRow("normal") << 1.0;
        QTest::newRow("scaled") << 1.8;
        QTest::newRow("triple") << 3.0;
    }
    void versionPickerKeyboardAndGeometry() {
        QFETCH(double,scale);
        QCOMPARE(bible->versionId(),QString("engnet"));
        QCOMPARE(bible->versions().size(),1);
        reference("1 Thessalonians 1:1-2 Thessalonians 3:18");
        const auto selected = bible->selectionReference();
        window->resize(640,480);
        QVERIFY(desktop->setProperty("textScale",scale));
        QTest::qWait(100);
        auto location = item("locationControls");
        auto passage = item("passageControl");
        auto version = item("versionControl");
        QCOMPARE(location->x() + location->width()/2,320.0);
        // Search, passage, and version controls sit flush left to right.
        auto searchButton = item("searchControl");
        QCOMPARE(searchButton->x(),0.0);
        QCOMPARE(passage->x(),searchButton->width());
        QCOMPARE(version->x(),searchButton->width() + passage->width());
        const auto layoutButton = item("layoutControl");
        const double layoutWidth = layoutButton->isVisible() ? layoutButton->width() : 0;
        QCOMPARE(location->width(),searchButton->width() + passage->width() + version->width() + layoutWidth);
        QVERIFY(searchButton->width() > 0 && passage->width() > 0 && version->width() > 0);
        // Cramped or not, the center row sits between the corner groups.
        QVERIFY(location->x() > item("modeControl")->x() + item("modeControl")->width());
        QVERIFY(location->x() + location->width() < item("floatingActions")->x());
        QCOMPARE(prop("passageControlLabel","truncated").toBool(),passage->width() < passage->implicitWidth() - 1);
        QCOMPARE(prop("passageControl","tip").toString(),selected + " · Open passage · ctrl k");
        click(version);
        QTRY_VERIFY(item("versionSearch")->hasActiveFocus());
        QVERIFY(!item("versionClose"));
        auto search = item("versionSearch");
        auto add = item("versionAdd");
        QVERIFY(add && add->isVisible());
        QCOMPARE(add->height(),search->height());
        QCOMPARE(add->width(),add->height());
        QCOMPARE(add->y(),search->y());
        QVERIFY(add->x() > search->x() + search->width());
        QCOMPARE(add->x() + add->width(),item("versionResults")->width());
        QCOMPARE(prop("versionAdd","tip").toString(),QString("Add version · ctrl i"));
        QCOMPARE(plainHint(prop("versionHints","text").toString()),QString::fromUtf8("↑ ↓ choose    ctrl i add    enter select    esc cancel"));
        type("no such version");
        QCOMPARE(prop("versionResults","count").toInt(),0);
        QVERIFY(item("versionEmpty")->isVisible());
        key(Qt::Key_Home); key(Qt::Key_End); key(Qt::Key_Return);
        QVERIFY(prop("versionDialog","visible").toBool()); // Still up (it may still be fading in).
        QCOMPARE(bible->selectionReference(),selected);
        key(Qt::Key_A,Qt::ControlModifier); type("net");
        QCOMPARE(prop("versionResults","count").toInt(),1);
        for (auto arrow : {Qt::Key_Down,Qt::Key_Up,Qt::Key_Home,Qt::Key_End,Qt::Key_PageDown,Qt::Key_PageUp}) {
            key(arrow);
            QCOMPARE(prop("versionResults","currentIndex").toInt(),0);
        }
        key(Qt::Key_Return,Qt::ShiftModifier);
        key(Qt::Key_Enter,Qt::ShiftModifier);
        QVERIFY(prop("versionDialog","visible").toBool()); // Still up (it may still be fading in).
        QVERIFY(!window->property("verseMode").toBool());
        QTest::qWait(100);
        for (auto name : {"versionSearch","versionAdd","versionResults","versionCancel","versionNext"}) {
            auto target = item(name);
            QVERIFY(QRectF(QPointF(),window->size()).contains(target->mapRectToScene(target->boundingRect())));
        }
        key(Qt::Key_Return);
        QTRY_VERIFY(!prop("versionDialog","visible").toBool());
        QCOMPARE(bible->selectionReference(),selected);
        key(Qt::Key_V,Qt::ControlModifier);
        QTRY_VERIFY(prop("versionDialog","opened").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("versionDialog","visible").toBool());
        key(Qt::Key_F); QVERIFY(bible->focusMode());
        QVERIFY(!version->isVisible()); QVERIFY(!passage->isVisible());
        key(Qt::Key_V,Qt::ControlModifier);
        QTRY_VERIFY(prop("versionDialog","opened").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("versionDialog","visible").toBool());
        key(Qt::Key_F1); QTRY_VERIFY(item("keybindingSearch")->hasActiveFocus());
        type("switch version");
        QCOMPARE(prop("keybindingResults","count").toInt(),1);
        key(Qt::Key_Return); QTRY_VERIFY(prop("versionDialog","opened").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("versionDialog","visible").toBool());
        key(Qt::Key_Space,Qt::ControlModifier); QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("switch version"); key(Qt::Key_Return);
        QTRY_VERIFY(prop("versionDialog","opened").toBool());
    }
    void versionPlusCancel_data() {
        QTest::addColumn<int>("activation");
        QTest::newRow("click") << 0;
        QTest::newRow("tab-return") << 1;
        QTest::newRow("tab-enter-empty-search") << 2;
    }
    void versionPlusCancel() {
        QFETCH(int,activation);
        reference("John 3:16-18");
        bible->setNoteText("Preserve the version and range");
        bible->toggleBookmark(); bible->toggleHighlight();
        QTest::qWait(450);
        const auto versions = bible->versions();
        const auto annotations = bible->collection();
        const auto selected = bible->selectionReference();
        const auto position = bible->readingPosition();
        QSignalSpy navigation(bible.get(),&BibleBackend::navigationChanged);
        QSignalSpy selection(bible.get(),&BibleBackend::selectionChanged);
        key(Qt::Key_V,Qt::ControlModifier);
        QTRY_VERIFY(item("versionSearch")->hasActiveFocus());
        if (activation == 2) type("no matching version");
        bool overlapped = false;
        const auto observe = connect(window,&QQuickWindow::activeFocusItemChanged,this,[&] {
            overlapped |= prop("versionDialog","visible").toBool() && prop("importGuide","visible").toBool();
        });
        if (activation == 0) click(item("versionAdd"));
        else {
            key(Qt::Key_Tab); QTRY_VERIFY(item("versionAdd")->hasActiveFocus());
            key(Qt::Key_Return,Qt::ShiftModifier);
            QVERIFY(prop("versionDialog","visible").toBool()); // Still up (it may still be fading in).
            QVERIFY(!prop("importGuide","visible").toBool());
            key(activation == 1 ? Qt::Key_Return : Qt::Key_Enter);
        }
        QTRY_VERIFY(prop("importGuide","opened").toBool());
        QVERIFY(!prop("versionDialog","visible").toBool());
        QVERIFY(!prop("importDialog","visible").toBool());
        disconnect(observe);
        QVERIFY(!overlapped);
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("importGuide","visible").toBool());
        QTRY_VERIFY(item("readerSurface")->hasActiveFocus());
        QVERIFY(!prop("versionDialog","visible").toBool());
        QVERIFY(!prop("importDialog","visible").toBool());
        QVERIFY(!bible->busy());
        QCOMPARE(navigation.count(),0); QCOMPARE(selection.count(),0);
        QCOMPARE(bible->versions(),versions); QCOMPARE(bible->collection(),annotations);
        QCOMPARE(bible->selectionReference(),selected); QCOMPARE(bible->readingPosition(),position);
    }
    void removalWizardCancellationAndLastVersion() {
        reference("John 3:16");
        bible->setNoteText("Retain this edition's note");
        bible->toggleBookmark(); bible->toggleHighlight();
        const auto annotations = bible->collection();
        const auto versions = bible->versions();
        const auto start = [this] {
            key(Qt::Key_Space,Qt::ControlModifier); QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
            type("remove version"); key(Qt::Key_Return);
            QTRY_VERIFY(item("versionSearch")->hasActiveFocus());
            QCOMPARE(prop("versionDialog","step").toInt(),0);
            QVERIFY(!item("versionAdd")->isVisible());
        };
        // Every step supports Escape and Cancel, including after opting to delete annotations.
        for (int step = 0; step <= 2; ++step) for (bool escape : {false,true}) {
            start();
            if (step > 0) {
                key(Qt::Key_Return);
                QCOMPARE(prop("versionDialog","step").toInt(),1);
                QVERIFY(!item("versionAdd")->isVisible());
                QCOMPARE(prop("versionResults","currentIndex").toInt(),0);
                QVERIFY(!prop("versionDialog","removeAnnotations").toBool());
            }
            if (step > 1) {
                key(Qt::Key_Down); key(Qt::Key_Return);
                QCOMPARE(prop("versionDialog","step").toInt(),2);
                QVERIFY(!item("versionAdd")->isVisible());
                QVERIFY(prop("versionSummary","text").toString().contains("Delete its notes"));
                QTRY_VERIFY(item("versionCancel")->hasActiveFocus());
            }
            QCOMPARE(bible->versions(),versions); QCOMPARE(bible->collection(),annotations);
            if (escape) key(Qt::Key_Escape); else click(item("versionCancel"));
            QTRY_VERIFY(!prop("versionDialog","visible").toBool());
            QCOMPARE(bible->versions(),versions); QCOMPARE(bible->collection(),annotations);
        }
        window->resize(640,480); QVERIFY(desktop->setProperty("textScale",1.8));
        start(); key(Qt::Key_Return); key(Qt::Key_Return);
        QCOMPARE(prop("versionDialog","step").toInt(),2);
        QVERIFY(prop("versionSummary","text").toString().contains("Keep its notes"));
        QVERIFY(prop("versionSummary","text").toString().contains("library will be empty"));
        click(item("versionBack")); QCOMPARE(prop("versionDialog","step").toInt(),1);
        QCOMPARE(prop("versionResults","currentIndex").toInt(),0);
        key(Qt::Key_Return);
        QTRY_VERIFY(item("versionCancel")->hasActiveFocus());
        key(Qt::Key_Right); QTRY_VERIFY(item("versionNext")->hasActiveFocus());
        key(Qt::Key_Return,Qt::ShiftModifier);
        QVERIFY(bible->ready()); QCOMPARE(bible->collection(),annotations);
        QTest::qWait(100);
        for (auto name : {"versionSummaryScroll","versionBack","versionCancel","versionNext"}) {
            auto target = item(name);
            QVERIFY(QRectF(QPointF(),window->size()).contains(target->mapRectToScene(target->boundingRect())));
        }
        key(Qt::Key_Return);
        QTRY_VERIFY(!prop("versionDialog","visible").toBool());
        QVERIFY(!bible->ready()); QVERIFY(bible->versions().isEmpty());
        QVERIFY(item("setupImport")->isVisible());
        key(Qt::Key_V,Qt::ControlModifier); QTRY_VERIFY(prop("versionDialog","opened").toBool());
        QVERIFY(item("versionEmpty")->isVisible()); QVERIFY(!item("versionNext")->isEnabled());
        QTest::qWait(100); // Polish the newly opened, empty picker before clicking.
        click(item("versionAdd")); QTRY_VERIFY(prop("importGuide","opened").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("importGuide","visible").toBool());
        const QString usfm = netSource().isEmpty() ? syntheticSource : netSource();
        if (!usfm.isEmpty()) {
            bible->importUsfm(QUrl::fromLocalFile(usfm));
            QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(),120000);
            QVERIFY2(bible->ready(),qPrintable(bible->error()));
            QVERIFY(bible->navigate("John 3:16"));
            QCOMPARE(bible->noteText(),QString("Retain this edition's note"));
            QVERIFY(bible->selectionBookmarked()); QVERIFY(bible->selectionHighlighted());
        }
    }
    void versionCallbacksGuardUnsavedNotes() {
        reference("John 3:16");
        key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        QVERIFY(QMetaObject::invokeMethod(window,"openVersions",Q_ARG(QVariant,true)));
        QTRY_VERIFY(prop("versionDialog","opened").toBool());
        key(Qt::Key_Return); key(Qt::Key_Return);
        QCOMPARE(prop("versionDialog","step").toInt(),2);
        const auto versions = bible->versions();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE","ui-version-note-failure");
            db.setDatabaseName(bible->dataDirectory() + "/personal.sqlite");
            QVERIFY(db.open()); QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TRIGGER reject_version_note BEFORE INSERT ON annotations BEGIN SELECT RAISE(FAIL,'version draft failure'); END"));
            QVERIFY(window->setProperty("syncingNote",true));
            QVERIFY(object("noteEditor")->setProperty("text","Unsaved version draft"));
            QVERIFY(window->setProperty("syncingNote",false));
            QVERIFY(QMetaObject::invokeMethod(window,"switchVersion",Q_ARG(QVariant,"engnet")));
            QVERIFY(prop("versionDialog","visible").toBool()); // Still up (it may still be fading in).
            QVERIFY(bible->error().contains("version draft failure"));
            QVERIFY(QMetaObject::invokeMethod(window,"confirmRemoveVersion"));
            QCOMPARE(bible->versions(),versions);
            QCOMPARE(prop("noteEditor","text").toString(),QString("Unsaved version draft"));
            QVERIFY(bible->noteText().isEmpty());
            QVERIFY(prop("versionDialog","visible").toBool()); // Still up (it may still be fading in).
            key(Qt::Key_Escape); QTRY_VERIFY(!prop("versionDialog","visible").toBool());
            QVERIFY(QMetaObject::invokeMethod(window,"openVersions",Q_ARG(QVariant,false)));
            QVERIFY(!prop("versionDialog","visible").toBool());
            // A draft can also change while the picker is already open.
            QVERIFY(object("versionDialog")->setProperty("removing",false));
            QVERIFY(object("versionDialog")->setProperty("step",0));
            QVERIFY(QMetaObject::invokeMethod(object("versionDialog"),"open"));
            QTRY_VERIFY(prop("versionDialog","opened").toBool());
            click(item("versionAdd"));
            QVERIFY(prop("versionDialog","visible").toBool()); // Still up (it may still be fading in).
            QVERIFY(!prop("importGuide","visible").toBool());
            QVERIFY(!prop("versionDialog","addPending").toBool());
            QCOMPARE(prop("noteEditor","text").toString(),QString("Unsaved version draft"));
            QVERIFY(q.exec("DROP TRIGGER reject_version_note"));
        }
        QSqlDatabase::removeDatabase("ui-version-note-failure");
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("versionDialog","visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(window,"openVersions",Q_ARG(QVariant,false)));
        QTRY_VERIFY(prop("versionDialog","opened").toBool());
        QCOMPARE(bible->noteText(),QString("Unsaved version draft"));
        // Backend rejection must leave the picker open too.
        QVERIFY(QMetaObject::invokeMethod(window,"switchVersion",Q_ARG(QVariant,"missing-version")));
        QVERIFY(prop("versionDialog","visible").toBool()); // Still up (it may still be fading in).
        QCOMPARE(bible->versions(),versions);
        key(Qt::Key_Return); QTRY_VERIFY(!prop("versionDialog","visible").toBool());
    }
    void realKjvSwitchAndRemoveAnnotations() {
        const auto kjv = kjvSource();
        if (kjv.isEmpty()) QSKIP("Set OMAWORD_TEST_USFM_DIR or OMAWORD_TEST_KJV to exercise KJV USFM.");
        reference("John 3:16-18");
        bible->setNoteText("NET annotation"); bible->toggleBookmark();
        const auto netText = bodyText(16);
        const auto selected = bible->selectionReference();
        const auto source = session->filePath("kjv.zip");
        QVERIFY(QFile::copy(kjv,source));
        QVERIFY(object("importDialog")->setProperty("selectedFile",QUrl::fromLocalFile(source)));
        QVERIFY(QMetaObject::invokeMethod(object("importDialog"),"accepted"));
        QVERIFY(bible->busy());
        QVERIFY(!item("versionControl")->isEnabled());
        QVERIFY(QMetaObject::invokeMethod(window,"openVersions",Q_ARG(QVariant,false)));
        QVERIFY(!prop("versionDialog","visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(window,"switchVersion",Q_ARG(QVariant,"engnet")));
        QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(),120000);
        QCOMPARE(bible->versionId(),QString("eng-kjv2006"));
        QCOMPARE(bible->versions().size(),2);
        QCOMPARE(bible->selectionReference(),selected);
        QVERIFY(bible->noteText().isEmpty()); QVERIFY(!bible->selectionBookmarked());
        QVERIFY(bodyText(16) != netText);
        QCOMPARE(prop("versionControl","text").toString(),QString("KJV"));
        QVERIFY(!window->findChild<QQuickItem *>("editionLabel"));
        QVERIFY(QFile::remove(source));
        QVERIFY(QMetaObject::invokeMethod(window,"runCommand",Q_ARG(QVariant,"about")));
        QTRY_VERIFY(prop("aboutDialog","opened").toBool());
        QCOMPARE(prop("aboutEdition","text").toString(),bible->edition());
        QCOMPARE(prop("aboutCopyright","text").toString(),bible->versionCopyright());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("aboutDialog","visible").toBool());
        bible->setNoteText("KJV annotation to delete");
        key(Qt::Key_V,Qt::ControlModifier); QTRY_VERIFY(item("versionSearch")->hasActiveFocus());
        key(Qt::Key_End); QCOMPARE(prop("versionResults","currentIndex").toInt(),1);
        key(Qt::Key_Home); QCOMPARE(prop("versionResults","currentIndex").toInt(),0);
        type("NET"); key(Qt::Key_Return,Qt::ShiftModifier);
        QCOMPARE(bible->versionId(),QString("eng-kjv2006"));
        key(Qt::Key_Return); QTRY_VERIFY(!prop("versionDialog","visible").toBool());
        QCOMPARE(bible->versionId(),QString("engnet"));
        QCOMPARE(bible->selectionReference(),selected);
        QCOMPARE(bible->noteText(),QString("NET annotation")); QVERIFY(bible->selectionBookmarked());
        QCOMPARE(bodyText(16),netText);
        QVERIFY(QMetaObject::invokeMethod(window,"openVersions",Q_ARG(QVariant,true)));
        QTRY_VERIFY(item("versionSearch")->hasActiveFocus());
        type("KJV"); key(Qt::Key_Return); key(Qt::Key_Down); key(Qt::Key_Return);
        QCOMPARE(prop("versionDialog","step").toInt(),2);
        key(Qt::Key_Right); key(Qt::Key_Return);
        QTRY_VERIFY(!prop("versionDialog","visible").toBool());
        QCOMPARE(bible->versions().size(),1); QCOMPARE(bible->versionId(),QString("engnet"));
        QCOMPARE(bible->noteText(),QString("NET annotation"));
        bible->importUsfm(QUrl::fromLocalFile(kjv));
        QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(),120000);
        QCOMPARE(bible->versionId(),QString("eng-kjv2006"));
        QCOMPARE(bible->selectionReference(),selected);
        QVERIFY(bible->noteText().isEmpty());
        // Removing the active version chooses the remaining edition.
        QVERIFY(QMetaObject::invokeMethod(window,"openVersions",Q_ARG(QVariant,true)));
        QTRY_VERIFY(item("versionSearch")->hasActiveFocus());
        type("KJV"); key(Qt::Key_Return); key(Qt::Key_Return);
        key(Qt::Key_Right); key(Qt::Key_Return);
        QTRY_VERIFY(!prop("versionDialog","visible").toBool());
        QCOMPARE(bible->versionId(),QString("engnet"));
        QCOMPARE(bible->noteText(),QString("NET annotation"));
    }
    void versionSwitchFallbackFeedback() {
        const auto kjv = kjvSource();
        if (kjv.isEmpty()) QSKIP("Set OMAWORD_TEST_USFM_DIR or OMAWORD_TEST_KJV to exercise KJV USFM.");
        bible->importUsfm(QUrl::fromLocalFile(kjv));
        QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(),120000);
        QCOMPARE(bible->versionId(),QString("eng-kjv2006"));
        reference("Matthew 17:21");
        key(Qt::Key_F); QVERIFY(bible->focusMode());
        key(Qt::Key_V,Qt::ControlModifier);
        QTRY_VERIFY(item("versionSearch")->hasActiveFocus());
        type("NET"); key(Qt::Key_Return);
        QTRY_VERIFY(!prop("versionDialog","visible").toBool());
        QCOMPARE(bible->versionId(),QString("engnet"));
        QVERIFY(bible->error().isEmpty());
        QCOMPARE(bible->passage(),QString("Matthew 17"));
        QCOMPARE(bible->selectedStart(),0);
        QCOMPARE(prop("toast","text").toString(),bible->status());
        QVERIFY(prop("toast","text").toString().contains("absent"));
        QVERIFY(prop("toast","text").toString().contains("Matthew 17"));
        QVERIFY(prop("toast","text").toString().contains(bible->edition()));
        QVERIFY(item("toast")->isVisible());
        QVERIFY(bible->focusMode());
    }
    void backupScopeAndExport_data() {
        QTest::addColumn<double>("scale");
        QTest::newRow("normal") << 1.0;
        QTest::newRow("small-scaled") << 1.8;
    }
    void backupScopeAndExport() {
        QFETCH(double,scale);
        reference("John 3:16");
        bible->setNoteText("Only the active edition");
        const auto scope = bible->tagScope();
        const auto tag = bible->createAndAssignTag(scope,"Backup <tag> 🌿");
        QVERIFY(!tag.isEmpty());
        const auto collection = bible->collection();
        window->resize(640,480); QVERIFY(desktop->setProperty("textScale",scale));
        const auto openScope = [this] {
            key(Qt::Key_Space,Qt::ControlModifier);
            QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
            type("save annotation backup"); key(Qt::Key_Return);
            QTRY_VERIFY(prop("backupScope","opened").toBool());
            QTRY_VERIFY(item("backupChoose")->hasActiveFocus());
        };
        openScope();
        QVERIFY(window->property("modalOpen").toBool());
        QVERIFY(!prop("backupDialog","visible").toBool());
        const auto explanation = prop("backupScopeExplanation","text").toString();
        QVERIFY(explanation.contains(bible->edition()));
        QVERIFY(explanation.contains("(NET)"));
        QVERIFY(explanation.contains("reading location"));
        QVERIFY(explanation.contains("Other editions are excluded"));
        QVERIFY(explanation.contains("reimport that edition first"));
        QTest::qWait(100);
        for (const auto name : {"backupScopeScroll","backupCancel","backupChoose"}) {
            const auto target = item(name);
            QVERIFY(QRectF(QPointF(),window->size()).contains(target->mapRectToScene(target->boundingRect())));
        }
        key(Qt::Key_V,Qt::ControlModifier); QVERIFY(!prop("versionDialog","visible").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("backupScope","visible").toBool());
        QVERIFY(!prop("backupDialog","visible").toBool()); QCOMPARE(bible->collection(),collection);
        openScope(); click(item("backupCancel"));
        QTRY_VERIFY(!prop("backupScope","visible").toBool());
        QVERIFY(!prop("backupDialog","visible").toBool());
        openScope();
        bool overlapped = false;
        auto observe = connect(window,&QQuickWindow::activeFocusItemChanged,this,[&] {
            overlapped |= prop("backupScope","visible").toBool() && prop("backupDialog","visible").toBool();
        });
        key(Qt::Key_Return);
        QTRY_VERIFY(prop("backupDialog","visible").toBool());
        QVERIFY(!prop("backupScope","visible").toBool()); QVERIFY(!overlapped);
        disconnect(observe);
        QVERIFY(prop("backupDialog","title").toString().contains("NET"));
        QVERIFY(prop("backupDialog","title").toString().contains("active edition only"));
        // Cancel the platform picker, then exercise its acceptance callback directly.
        QVERIFY(QMetaObject::invokeMethod(object("backupDialog"),"reject"));
        QTRY_VERIFY(!prop("backupDialog","visible").toBool());
        window->requestActivate(); QTRY_VERIFY(window->isActive());
        const auto path = session->filePath("active-backup.json");
        QVERIFY(object("backupDialog")->setProperty("selectedFile",QUrl::fromLocalFile(path)));
        QVERIFY(QMetaObject::invokeMethod(object("backupDialog"),"accepted"));
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto backup = QJsonDocument::fromJson(file.readAll()).object();
        // Version 5 (Desk step) adds the global Desk list; empty here.
        QCOMPARE(backup.value("version").toInt(),5);
        QVERIFY(backup.value("desk").isArray());
        QCOMPARE(backup.value("desk").toArray().size(),0);
        QCOMPARE(backup.value("versionId").toString(),QString("engnet"));
        const auto tags = backup.value("tags").toArray();
        QCOMPARE(tags.size(),1);
        QCOMPARE(tags.first().toObject().value("id").toString(),tag);
        QCOMPARE(tags.first().toObject().value("name").toString(),QString("Backup <tag> 🌿"));
        const auto assignments = backup.value("tagAssignments").toArray();
        QCOMPARE(assignments.size(),1);
        const auto assignment = assignments.first().toObject();
        QCOMPARE(assignment.value("tagId").toString(),tag);
        for (const auto field : {"book","chapter","start","endBook","endChapter","end"})
            QCOMPARE(assignment.value(field).toInt(),scope.value(field).toInt());
        QCOMPARE(prop("toast","text").toString(),QString("Saved NET annotation backup"));
        QVERIFY(prop("exportDialog","title").toString().contains("NET notes only"));
        QVERIFY(prop("restoreDialog","title").toString().contains("NET"));
        QCOMPARE(bible->collection(),collection);
    }
    void importGuideAndPicker_data() {
        QTest::addColumn<bool>("notes");
        QTest::addColumn<double>("scale");
        QTest::newRow("reader") << false << 1.0;
        QTest::newRow("notes") << true << 1.0;
        QTest::newRow("small-scaled") << true << 1.8;
    }
    void importGuideAndPicker() {
        QFETCH(bool,notes);
        QFETCH(double,scale);
        reference("John 3:16");
        if (notes) {
            key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
            // Stage a nonempty draft so opening the guide must flush it.
            QVERIFY(window->setProperty("syncingNote",true));
            QVERIFY(object("noteEditor")->setProperty("text","A draft before adding a version"));
            QVERIFY(window->setProperty("syncingNote",false));
            QVERIFY(bible->noteText().isEmpty());
            item("notesPanel")->forceActiveFocus();
        }
        QVERIFY(desktop->setProperty("textScale",scale));
        if (scale > 1) window->resize(640,480);
        key(Qt::Key_Space,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("add version"); key(Qt::Key_Return);
        QTRY_VERIFY(prop("importGuide","opened").toBool());
        QVERIFY(!prop("passagePalette","visible").toBool());
        QVERIFY(!prop("importDialog","visible").toBool());
        QVERIFY(window->property("modalOpen").toBool());
        if (notes) QCOMPARE(bible->noteText(),QString("A draft before adding a version"));
        QVERIFY(item("importReplacement")->isVisible());
        QVERIFY(prop("importReplacement","text").toString().contains("preserves your notes, bookmarks, highlights, and settings"));
        QVERIFY(prop("importSupported","text").toString().contains("USFM ZIP directly from eBible.org"));
        QCOMPARE(prop("catalogResults","count").toInt(),7);
        QCOMPARE(prop("importChoose","text").toString(),QString::fromUtf8("Import USFM ZIP…"));
        QCOMPARE(prop("importDataPath","text").toString(),bible->dataDirectory());
        QVERIFY(prop("importOffline","text").toString().contains("move or delete the source USFM ZIP"));
        QVERIFY(prop("importOffline","text").toString().contains("No connection"));
        const auto checkFocus = [&] {
            if (notes) QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
            else QTRY_VERIFY(item("readerSurface")->hasActiveFocus());
        };
        QTest::qWait(150);
        for (const auto name : {"importChoose","importCancel","importGuideScroll"}) {
            const auto target = item(name);
            QVERIFY(QRectF(QPointF(),window->size()).contains(target->mapRectToScene(target->boundingRect())));
        }
        capture("revision-import-guide-" + QString::fromLatin1(QTest::currentDataTag()));
        if (scale > 1) {
            QVERIFY(prop("importGuideScroll","contentHeight").toDouble() > item("importGuideScroll")->height());
            auto flick = qvariant_cast<QQuickItem *>(prop("importGuideScroll","contentItem"));
            QVERIFY(flick);
            flick->setProperty("contentY",prop("importGuideScroll","contentHeight").toDouble() - flick->height());
            QTest::qWait(100);
            const auto offline = item("importOffline")->mapRectToItem(flick,item("importOffline")->boundingRect());
            QVERIFY(flick->boundingRect().adjusted(-1,-1,1,1).contains(offline));
            capture("revision-import-guide-small-scrolled");
        }
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("importGuide","visible").toBool());
        checkFocus();
        QVERIFY(QMetaObject::invokeMethod(window,"runCommand",Q_ARG(QVariant,"import")));
        QTRY_VERIFY(prop("importGuide","opened").toBool());
        // The test harness disables native dialogs; observe the close/open handoff.
        bool overlapped = false;
        auto observe = connect(window,&QQuickWindow::activeFocusItemChanged,this,[&] {
            overlapped |= prop("importGuide","visible").toBool() && prop("importDialog","visible").toBool();
        });
        click(item("importChoose"));
        QTRY_VERIFY(prop("importDialog","visible").toBool());
        QVERIFY(!prop("importGuide","visible").toBool());
        QVERIFY(!overlapped);
        disconnect(observe);
        QTest::qWait(250); // The non-native picker animates after visibleChanged.
        // Qt may host even a non-native file picker in a separate popup window.
        QVERIFY(QGuiApplication::focusWindow());
        QTest::keyClick(QGuiApplication::focusWindow(),Qt::Key_Escape);
        QTRY_VERIFY(!prop("importDialog","visible").toBool());
        // Offscreen has no window manager to reactivate the parent window.
        window->requestActivate();
        QTRY_VERIFY(window->isActive());
        checkFocus();
        QVERIFY(!bible->busy());
        QCOMPARE(bible->selectionReference(),QString("John 3:16"));
        if (notes) QCOMPARE(bible->noteText(),QString("A draft before adding a version"));
        QVERIFY(QMetaObject::invokeMethod(window,"runCommand",Q_ARG(QVariant,"import")));
        QTRY_VERIFY(prop("importGuide","opened").toBool());
        click(item("importCancel")); QTRY_VERIFY(!prop("importGuide","visible").toBool());
        checkFocus();
    }
    void commandPaletteAndHelp() {
        key(Qt::Key_Space,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        QCOMPARE(prop("referenceInput","text").toString(),QString("> "));
        type("keybindings"); key(Qt::Key_Return);
        QTRY_VERIFY(prop("helpDialog","opened").toBool());
        key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("helpDialog","visible").toBool());
        key(Qt::Key_F1); QTRY_VERIFY(prop("helpDialog","opened").toBool());
        key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("helpDialog","visible").toBool());
        key(Qt::Key_Space,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("about"); key(Qt::Key_Return);
        QTRY_VERIFY(prop("aboutDialog","opened").toBool());
    }
    void searchableKeybindings() {
        reference("John 3:16");
        key(Qt::Key_F1);
        QTRY_VERIFY(prop("helpDialog","opened").toBool());
        QTRY_VERIFY(item("keybindingSearch")->hasActiveFocus());
        QCOMPARE(prop("keybindingSearch","placeholderText").toString(),QString::fromUtf8("Search keybindings…"));
        const int total = prop("keybindingResults","count").toInt();
        QVERIFY(total > 10);
        capture("help");
        for (const auto &query : {QString("m"),QString("x"),QString("bookmark"),QString("highlight selection"),QString("Shift Down")}) {
            key(Qt::Key_A,Qt::ControlModifier); type(query);
            QTRY_COMPARE(prop("keybindingResults","count").toInt(),1);
            const QString action = query == "m" || query == "bookmark" ? "Bookmark selection"
                : query == "Shift Down" ? "Extend to next verse" : "Highlight selection";
            QTRY_VERIFY(item(qPrintable("keybinding" + action)));
            QCOMPARE(bible->selectionReference(),QString("John 3:16"));
            QVERIFY(!bible->selectionBookmarked()); QVERIFY(!bible->selectionHighlighted());
            QVERIFY(!window->property("verseMode").toBool());
            QVERIFY(item("keybindingSearch")->hasActiveFocus());
        }
        key(Qt::Key_A,Qt::ControlModifier); type("no such keybinding jkhlnv");
        QTRY_COMPARE(prop("keybindingResults","count").toInt(),0);
        QVERIFY(item("keybindingEmpty")->isVisible());
        QCOMPARE(bible->selectionReference(),QString("John 3:16"));
        QVERIFY(!window->property("notesOpen").toBool());
        key(Qt::Key_A,Qt::ControlModifier); key(Qt::Key_Backspace);
        QTRY_COMPARE(prop("keybindingResults","count").toInt(),total);
        QVERIFY(!item("keybindingEmpty")->isVisible());
        key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("helpDialog","visible").toBool());
        QTRY_VERIFY(item("readerSurface")->hasActiveFocus());
        key(Qt::Key_M); QVERIFY(bible->selectionBookmarked());
        key(Qt::Key_F1);
        QTRY_VERIFY(item("keybindingSearch")->hasActiveFocus());
        QCOMPARE(prop("keybindingSearch","text").toString(),QString());
        window->resize(640,480);
        QVERIFY(desktop->setProperty("textScale",1.8));
        QTest::qWait(150);
        QVERIFY(item("keybindingResults")->height() > 50);
        const auto searchBounds = item("keybindingSearch")->mapRectToScene(item("keybindingSearch")->boundingRect());
        QVERIFY(QRectF(0,0,640,480).contains(searchBounds));
        type("x"); QTRY_COMPARE(prop("keybindingResults","count").toInt(),1);
        capture("help-scaled");
    }
    void bookmarkGlyphCanonicalScope_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("verse") << QString("verse");
        QTest::newRow("block") << QString("block");
    }
    void bookmarkGlyphCanonicalScope() {
        QFETCH(QString,mode);
        const auto source = session->filePath("bookmark-colliding-names.zip");
        QVERIFY(writeSyntheticUsfm(source,"engojb",false,true));
        bible->importUsfm(QUrl::fromLocalFile(source));
        QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(),120000);
        QVERIFY2(bible->error().isEmpty(),qPrintable(bible->error()));
        bible->openChapter(1,3); bible->selectVerses(1,3);
        const auto scope = bible->tagScope();
        QCOMPARE(scope.value("reference").toString(),QString::fromUtf8("John 3:1–3"));
        QVERIFY(bible->toggleBookmark());
        for (int index = 0; index < 3; ++index) {
            const auto ranges = bible->verses()[index].toMap().value("bookmarkRanges").toList();
            QCOMPARE(ranges.size(),1);
            const auto range = ranges.first().toMap();
            for (auto it = scope.cbegin(); it != scope.cend(); ++it) QCOMPARE(range.value(it.key()),it.value());
            QCOMPARE(range.value("bookmarkStart").toBool(),index == 0);
            QCOMPARE(range.value("bookmarkEnd").toBool(),index == 2);
        }
        // This source's GEN display name deliberately resolves to canonical JHN.
        QVERIFY(bible->navigate(scope.value("reference").toString()));
        QCOMPARE(bible->book(),43);
        bible->openChapter(1,3); bible->selectVerses(5,5);
        bible->setReadingMode(mode);
        QTest::qWait(200);
        QTRY_VERIFY(!window->property("restoringPosition").toBool());
        object("reading")->setProperty("contentY",0);
        const auto name = mode == "verse" ? QString("bookmarkGlyph1")
            : "blockInlineMarker:bookmark:open:" + scope.value("reference").toString();
        QTRY_VERIFY(item(qPrintable(name)) && item(qPrintable(name))->isVisible());
        click(item(qPrintable(name)));
        QTRY_COMPARE(bible->book(),1);
        QCOMPARE(bible->tagScope(),scope);
        QVERIFY(!window->property("verseMode").toBool());
    }
    void annotationTintsInVerseRows() {
        QVERIFY(bible->ready());
        reference("John 3:1-4"); QVERIFY(bible->toggleBookmark());
        reference("John 3:3-6");
        QVERIFY(!bible->createAndAssignTag(bible->tagScope(),"Context").isEmpty());
        reference("John 3:2"); QVERIFY(bible->toggleHighlight());
        reference("John 3:4"); bible->setNoteText("Fourth");
        reference("John 3:8-9"); QVERIFY(bible->toggleBookmark());
        reference("John 3:3");
        bible->setReadingMode("verse");
        QTRY_VERIFY(!window->property("continuousReader").toBool());
        const auto layers = [this](int verse) {
            auto tint = item(qPrintable("annotationTint" + QString::number(verse)));
            return tint && tint->isVisible() ? tint->property("layers").toInt() : 0;
        };
        // Bookmark, highlight, note, and tag ranges each add a layer, capped at three;
        // the selection and cursor change nothing (no context shading).
        const QList<int> expected = {1,2,2,3,1,1,0,1,1,0};
        for (int verse = 1; verse <= expected.size(); ++verse) QTRY_COMPARE(layers(verse),expected[verse - 1]);
        QVERIFY(item("highlightPaint2:John 3:2")); // The highlight still paints on top.
        QVERIFY(!item("contextSegment4"));
        bible->selectVerses(7,7); QTest::qWait(50);
        for (int verse = 1; verse <= expected.size(); ++verse) QCOMPARE(layers(verse),expected[verse - 1]);
        const auto tint = item("annotationTint4");
        QVERIFY(tint->inherits("QQuickCanvasItem"));
        QVERIFY(tint->width() > 0 && tint->height() > 0);
        const auto light = [&]() {
            auto grab = tint->grabToImage(); QVERIFY(grab);
            QSignalSpy ready(grab.data(),&QQuickItemGrabResult::ready); QVERIFY(ready.wait(2000));
            const auto image = grab->image();
            QVERIFY(image.pixelColor(image.width() / 2,image.height() / 2).alpha() > 0);
            QVERIFY(image.pixelColor(image.width() / 2,image.height() / 2).alpha() < 60);
        };
        light();
        // Visual check (OMAWORD_UI_SCREENSHOTS): every layout.
        for (const QString mode : {"verse","paragraph","block"}) {
            bible->setReadingMode(mode); QTest::qWait(150);
            capture("annotation-tints-" + mode);
        }
        bible->setReadingMode("verse"); QTRY_VERIFY(!window->property("continuousReader").toBool());
        bible->setFocusMode(true); QTRY_COMPARE(layers(4),0);
        bible->setFocusMode(false); QTRY_COMPARE(layers(4),3);
        // A range entering from the previous chapter tints its first rows too.
        reference("John 3:36-4:2"); QVERIFY(bible->toggleBookmark());
        reference("John 4:1");
        QTRY_COMPARE(layers(1),1); QCOMPARE(layers(2),1); QCOMPARE(layers(3),0);
        QCOMPARE(item("annotationTint1")->property("layersBefore").toInt(),0);
    }
    void bookmarkRangeMarkers() {
        reference("John 3:16-18");
        key(Qt::Key_M);
        reference("John 3:16");
        int glyphs = 0;
        for (int verse = 1; verse <= 36; ++verse) {
            auto glyph = item(qPrintable("bookmarkGlyph" + QString::number(verse)));
            QVERIFY(glyph);
            if (glyph->isVisible()) ++glyphs;
        }
        QCOMPARE(glyphs,1);
        QVERIFY(item("bookmarkGlyph16")->isVisible());
        QVERIFY(!item("bookmarkGlyph17")->isVisible());
        QVERIFY(prop("bookmarkMarker16","starts").toBool());
        QVERIFY(prop("bookmarkMarker18","ends").toBool());
        for (int verse : {16,17}) {
            auto line = item(qPrintable("bookmarkLine" + QString::number(verse)));
            auto next = item(qPrintable("bookmarkLine" + QString::number(verse + 1)));
            const double bottom = line->mapToScene(QPointF(0,line->height())).y();
            QVERIFY(qAbs(bottom - next->mapToScene(QPointF()).y()) < 0.1);
        }
        capture("bookmark");
        click(item("bookmarkMarker16"));
        QCOMPARE(bible->selectionReference(),QString::fromUtf8("John 3:16–18"));
        // A bookmark-only range has a single component type, so Shift-click
        // skips the chooser and enters the visual range editor directly.
        click(item("bookmarkMarker16"),Qt::ShiftModifier);
        QTRY_VERIFY(prop("rangeEditor","active").toBool());
        QVERIFY(!prop("rangeChooser","visible").toBool());
        QCOMPARE(prop("rangeEditor","components").value<QJSValue>().toVariant().toMap().value("bookmark").toBool(),true);
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("rangeEditor","sessionOpen").toBool());
        QCOMPARE(bible->selectionReference(),QString::fromUtf8("John 3:16–18"));
        // An overlapping range gets its own logical start, while shared rows
        // expose both references and a stable click target.
        reference("John 3:17-20"); key(Qt::Key_M);
        reference("John 3:17");
        QVERIFY(item("bookmarkGlyph17")->isVisible());
        const QString hint = prop("bookmarkMarker17","hint").toString();
        QVERIFY(hint.contains(QString::fromUtf8("John 3:16–18")));
        QVERIFY(hint.contains(QString::fromUtf8("John 3:17–20")));
        QCOMPARE(prop("bookmarkMarker17","choices").value<QJSValue>().toVariant().toList().first().toMap()["reference"].toString(),QString::fromUtf8("John 3:17–20"));
        click(item("bookmarkMarker17"));
        QCOMPARE(bible->selectionReference(),QString::fromUtf8("John 3:17–20"));
        reference("John 3:36-4:2"); key(Qt::Key_M);
        reference("John 3:36");
        QVERIFY(item("bookmarkGlyph36")->isVisible());
        QVERIFY(prop("bookmarkMarker36","continuesAfter").toBool());
        reference("John 4:1");
        QVERIFY(item("bookmarkMarker1")->isVisible());
        QVERIFY(prop("bookmarkMarker1","continuesBefore").toBool());
        QVERIFY(item("bookmarkGlyph1")->isVisible());
        QCOMPARE(item("bookmarkLine1")->y(),0.0);
        QVERIFY(prop("bookmarkMarker2","ends").toBool());
        QVERIFY(!item("bookmarkMarker3")->isVisible());
        capture("bookmark-continuation");
        click(item("bookmarkMarker1"));
        QCOMPARE(bible->selectionReference(),QString::fromUtf8("John 3:36–4:2"));
    }
    void annotationsShortcutContextsAndRelatedControl() {
        reference("John 3:1");
        const auto tag = bible->createAndAssignTag(bible->tagScope(),"Context tag");
        QVERIFY(!tag.isEmpty());
        for (const auto mode : {"verse", "block", "paragraph"}) {
            bible->setReadingMode(mode);
            QTest::qWait(150);
            auto body = qstrcmp(mode,"verse") == 0 ? item("verseBody1") : item("blockBody");
            QVERIFY(body); body->forceActiveFocus();
            key(Qt::Key_A,Qt::ShiftModifier);
            QTRY_VERIFY(prop("savedDrawer","opened").toBool());
            key(Qt::Key_A,Qt::ShiftModifier); QTest::qWait(50); QVERIFY(prop("savedDrawer","visible").toBool()); key(Qt::Key_Escape);
            QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
        }
        key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        type("Editable note selection"); key(Qt::Key_A,Qt::ControlModifier);
        QCOMPARE(prop("noteEditor","selectedText").toString(),QString("Editable note selection"));
        QVERIFY(!prop("savedDrawer","visible").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("notesDrawer","visible").toBool());
        key(Qt::Key_K,Qt::ControlModifier); QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("John"); key(Qt::Key_A,Qt::ControlModifier);
        QCOMPARE(prop("referenceInput","selectedText").toString(),QString("John"));
        QVERIFY(!prop("savedDrawer","visible").toBool());
        key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        window->resize(640,640);
        desktop->setProperty("textScale",1.8);
        key(Qt::Key_A,Qt::ShiftModifier); QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        click(item("savedTab3")); QTRY_COMPARE(window->property("collectionTab").toInt(),3);
        QTRY_VERIFY(item(qPrintable("savedTagEntry:Context tag:John 3:1")));
        QVERIFY(!prop("tagsSurface","visible").toBool());
        QVERIFY(!prop("tagsUi","visibleModal").toBool());
        QVERIFY(!prop("tagsUi","scopePinned").toBool());
        key(Qt::Key_Left); QCOMPARE(window->property("collectionTab").toInt(),2);
        key(Qt::Key_Right); QCOMPARE(window->property("collectionTab").toInt(),3);
        key(Qt::Key_L); QCOMPARE(window->property("collectionTab").toInt(),0);
        key(Qt::Key_H); QCOMPARE(window->property("collectionTab").toInt(),3);
        for (const auto name : {"tagResults", "tagSearch", "createPassageTag"})
            QVERIFY(!item(name)->isVisible());
        key(Qt::Key_A,Qt::ShiftModifier); QTest::qWait(50); QVERIFY(prop("savedDrawer","visible").toBool()); key(Qt::Key_Escape); QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
        key(Qt::Key_T); QTRY_VERIFY(item("tagSearch")->hasActiveFocus());
        QVERIFY(prop("tagsUi","visibleModal").toBool());
        QVERIFY(prop("tagsUi","scopePinned").toBool());
        QVERIFY(!prop("savedDrawer","visible").toBool());
        type("Context"); key(Qt::Key_A,Qt::ControlModifier);
        QCOMPARE(prop("tagSearch","selectedText").toString(),QString("Context"));
        QVERIFY(prop("tagsUi","visibleModal").toBool());
        QVERIFY(QMetaObject::invokeMethod(object("tagsUi"),"manage"));
        QVERIFY(QMetaObject::invokeMethod(object("tagsUi"),"toggleTag",Q_ARG(QVariant,tag)));
        item("tagRenameInput")->forceActiveFocus(); key(Qt::Key_A,Qt::ControlModifier);
        QCOMPARE(prop("tagRenameInput","selectedText").toString(),QString("Context tag"));
        QVERIFY(prop("tagsUi","visibleModal").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("tagsUi","visibleModal").toBool());
        auto related = item("relatedControl"); QVERIFY(related && related->isVisible());
        const auto relatedBounds = related->mapRectToScene(related->boundingRect());
        const auto modeBounds = item("modeControl")->mapRectToScene(item("modeControl")->boundingRect());
        QVERIFY(relatedBounds.left() > modeBounds.right());
        QVERIFY(relatedBounds.left() > window->width() / 2);
        const auto helpBounds = item("helpControl")->mapRectToScene(item("helpControl")->boundingRect());
        QCOMPARE(helpBounds.right(),modeBounds.left()); // Edge to edge.
        // Right group order: Annotations, Study, Desk (no Notes); a column when cramped.
        const auto scene = [this](const char *name) { return item(name)->mapRectToScene(item(name)->boundingRect()); };
        if (window->property("footerStacked").toBool()) {
            QVERIFY(relatedBounds.bottom() < modeBounds.bottom());
            QCOMPARE(scene("deskControl").bottom(),modeBounds.bottom());
            QCOMPARE(scene("savedControl").bottom(),relatedBounds.top());
            QVERIFY(relatedBounds.bottom() <= scene("deskControl").top());
        } else {
            QCOMPARE(relatedBounds.bottom(),modeBounds.bottom());
            QCOMPARE(scene("savedControl").right(),relatedBounds.left());
            QVERIFY(relatedBounds.right() <= scene("deskControl").left());
        }
        // Study draws its own book-and-magnifier glyph, not the tag; Annotations wears the note.
        QCOMPARE(related->property("glyph").toString(),QString("study"));
        QVERIFY(!item("relatedControlGlyph")->isVisible());
        QCOMPARE(prop("savedControlGlyph","kind").toString(),QString("note"));
        QVERIFY(item("savedControlGlyph")->isVisible());
        click(related); QTRY_VERIFY(prop("relatedDrawer","opened").toBool());
        QVERIFY(!prop("savedDrawer","visible").toBool());
        key(Qt::Key_H); QTRY_COMPARE(prop("tagsUi","studyTab").toString(),QString("related"));
        const auto inner = item("studyBody")->mapRectToScene(item("studyBody")->boundingRect());
        const qreal padding = 24 * 1.8;
        QVERIFY(qAbs(inner.left() - (window->width() - prop("relatedDrawer","width").toReal() + padding)) < 0.1);
        QVERIFY(qAbs(inner.right() - (window->width() - padding)) < 0.1);
        QVERIFY(qAbs(inner.top() - padding) < 0.1);
        QVERIFY(qAbs(inner.bottom() - (window->height() - padding)) < 0.1);
        for (const auto name : {"studyHeader", "studyTabs", "studyChips", "tagResults", "studyHints"}) {
            auto button = item(name); QVERIFY2(button && button->isVisible(),name);
            const auto bounds = button->mapRectToScene(button->boundingRect());
            QVERIFY2(inner.adjusted(-1,-1,1,1).contains(bounds),qPrintable(QString("%1: %2,%3 %4x%5 in %6,%7 %8x%9").arg(name).arg(bounds.x()).arg(bounds.y())
                .arg(bounds.width()).arg(bounds.height()).arg(inner.x()).arg(inner.y()).arg(inner.width()).arg(inner.height())));
        }
        QVERIFY(!item("manageTags")->isVisible());
        QVERIFY(!item("createPassageTag")->isVisible());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("relatedDrawer","visible").toBool());
        key(Qt::Key_S, Qt::ShiftModifier); QTRY_VERIFY(prop("relatedDrawer","opened").toBool());
    }
    void rowAnnotationLanePacking() {
        reference("John 3:1-5");
        bible->setReadingMode("verse");
        bible->setNoteText("Long rail starts first");
        reference("John 3:2-3"); QVERIFY(bible->toggleBookmark());
        reference("John 3:3-4");
        QVERIFY(!bible->createAndAssignTag(bible->tagScope(),"Middle overlap").isEmpty());
        reference("John 3:7-8"); QVERIFY(bible->toggleBookmark());
        reference("John 3:10");
        QVERIFY(!bible->createAndAssignTag(bible->tagScope(),"Disjoint tag").isEmpty());
        reference("John 3:1");
        const qreal scale = window->property("scale").toReal();
        for (int verse = 1; verse <= 5; ++verse) {
            auto marker = item(qPrintable(QString("verseNoteMarker%1").arg(verse)));
            QVERIFY(marker && marker->isVisible());
            QCOMPARE(marker->x(),36 * scale);
            if (verse < 5) {
                auto line = item(qPrintable(QString("noteLine%1").arg(verse)));
                auto next = item(qPrintable(QString("noteLine%1").arg(verse + 1)));
                QVERIFY(QLineF(line->mapToScene(QPointF(0,line->height())),next->mapToScene(QPointF())).length() < 0.1);
            }
        }
        for (int verse : {2,3}) QCOMPARE(item(qPrintable(QString("bookmarkMarker%1").arg(verse)))->x(),18 * scale);
        for (int verse : {3,4}) QCOMPARE(item(qPrintable(QString("verseTagMarker%1").arg(verse)))->x(),0.0);
        for (int verse : {7,8}) QCOMPARE(item(qPrintable(QString("bookmarkMarker%1").arg(verse)))->x(),36 * scale);
        QCOMPARE(item("verseTagMarker10")->x(),36 * scale);
        const qreal textX = item("verseBody1")->x();
        bible->setFocusMode(true);
        for (const auto name : {"verseNoteMarker1","bookmarkMarker2","verseTagMarker3"}) QTRY_VERIFY(!item(name)->isVisible());
        bible->setFocusMode(false);
        QTRY_VERIFY(item("verseNoteMarker1")->isVisible());
        QCOMPARE(item("verseBody1")->x(),textX);

        // Sparse, nonmonotonic labels: source ordinals control starts and reuse.
        // Same-kind overlapping/adjacent coverage is aggregated into one lane.
        const QVariantList present{QVariantMap{{"reference","range"}}};
        const QVariantList rows{
            QVariantMap{{"number",80},{"noteRanges",present}},
            QVariantMap{{"number",2},{"noteRanges",present},{"bookmarkRanges",present},{"tagRanges",present}},
            QVariantMap{{"number",78},{"noteRanges",present},{"tagRanges",present}},
            QVariantMap{{"number",1},{"bookmarkRanges",present},{"tagRanges",present}},
            QVariantMap{{"number",40}},
            QVariantMap{{"number",3},{"bookmarkRanges",present},{"tagRanges",present},{"noteRanges",present}}};
        QVariant packed;
        QVERIFY(QMetaObject::invokeMethod(window,"packAnnotationLanes",Q_RETURN_ARG(QVariant,packed),Q_ARG(QVariant,rows)));
        const auto lanes = packed.value<QJSValue>().toVariant().toList();
        QCOMPARE(lanes.size(),6);
        QCOMPARE(lanes[0].toMap()["note"].toInt(),0);
        QCOMPARE(lanes[1].toMap(),(QVariantMap{{"note",0},{"bookmark",1},{"tag",2}}));
        QCOMPARE(lanes[2].toMap(),(QVariantMap{{"note",0},{"tag",2}}));
        QCOMPARE(lanes[3].toMap(),(QVariantMap{{"bookmark",0},{"tag",2}}));
        QVERIFY(lanes[4].toMap().isEmpty());
        QCOMPARE(lanes[5].toMap(),(QVariantMap{{"bookmark",0},{"tag",1},{"note",2}}));
    }
    void rowAnnotationRailsAndExactNoteClick() {
        reference("John 3:1-3");
        bible->setReadingMode("verse");
        const auto scope = bible->tagScope();
        QVERIFY(bible->toggleBookmark());
        QVERIFY(!bible->createAndAssignTag(scope, "Same range").isEmpty());
        bible->setNoteText("Exact range note");
        reference("John 3:5");
        QVERIFY(QMetaObject::invokeMethod(window,"scrollToVerse",Q_ARG(QVariant,1),Q_ARG(QVariant,true)));
        QTRY_VERIFY(!object("readerSlide")->property("running").toBool());
        QTRY_VERIFY(item("verseNoteMarker1") && item("verseNoteMarker1")->isVisible());
        for (const auto kind : {"bookmark", "tag", "note"}) {
            for (int verse = 1; verse <= 3; ++verse) {
                const auto lineName = QString(kind) + "Line" + QString::number(verse);
                auto line = item(qPrintable(lineName)); QVERIFY(line && line->isVisible());
                QCOMPARE(line->width(),window->property("scale").toReal());
                if (verse < 3) {
                    auto next = item(qPrintable(QString(kind) + "Line" + QString::number(verse + 1)));
                    QVERIFY(next);
                    QVERIFY(qAbs(line->mapToScene(QPointF(0,line->height())).y() - next->mapToScene(QPointF()).y()) < 0.1);
                }
            }
        }
        // Hand-drawn rails are seeded per mark: identical across repaint and
        // scrolling, inked, and confined to the marker's reserved column.
        const auto railImage = [](QQuickItem *rail) {
            auto grab = rail->grabToImage();
            if (!grab) return QImage();
            QSignalSpy ready(grab.get(),&QQuickItemGrabResult::ready);
            if (grab->image().isNull()) ready.wait(2000);
            return grab->image();
        };
        auto rail = item("bookmarkRail1"); QVERIFY(rail);
        QCOMPARE(rail->size(),rail->parentItem()->size());
        const auto railBefore = railImage(rail);
        QVERIFY(!railBefore.isNull());
        int railInk = 0;
        for (int y = 0; y < railBefore.height(); ++y)
            for (int x = 0; x < railBefore.width(); ++x) railInk += railBefore.pixelColor(x,y).alpha() > 0;
        QVERIFY(railInk > 0);
        auto readingView = item("bookmarkMarker1");
        while (readingView && !readingView->inherits("QQuickFlickable")) readingView = readingView->parentItem();
        QVERIFY(readingView);
        const auto contentY = readingView->property("contentY");
        readingView->setProperty("contentY",contentY.toReal() + 30); QTest::qWait(50);
        readingView->setProperty("contentY",contentY); QTest::qWait(50);
        QMetaObject::invokeMethod(rail,"requestPaint"); QTest::qWait(50);
        QCOMPARE(railImage(rail),railBefore);
        const auto bookmark = item("bookmarkMarker1"), tag = item("verseTagMarker1"), note = item("verseNoteMarker1");
        QVERIFY(note->x() + note->width() <= tag->x());
        QVERIFY(tag->x() + tag->width() <= bookmark->x());
        QVERIFY(bookmark->x() + bookmark->width() <= item("verseNumber1")->x());
        QCOMPARE(prop("verseNoteMarker1","hint").toString(), scope["reference"].toString());
        // Equal-size captures distinguish the actual filled/outline renderings.
        const auto ink = [](const QImage &image) {
            qint64 amount = 0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x) amount += image.pixelColor(x,y).alpha();
            return amount;
        };
        for (const auto kind : {"note"}) {
            auto footer = item("savedControlGlyph");
            auto row = item(qPrintable(QString(kind) + "Glyph1"));
            QVERIFY(footer && row);
            QVERIFY(!footer->property("filled").toBool());
            QVERIFY(row->property("filled").toBool());
            row->setWidth(footer->width()); row->setHeight(footer->height());
            row->setProperty("ink",footer->property("ink"));
            QTest::qWait(50);
            auto footerGrab = footer->grabToImage(), rowGrab = row->grabToImage();
            QVERIFY(footerGrab && rowGrab);
            QTRY_VERIFY(!footerGrab->image().isNull() && !rowGrab->image().isNull());
            QVERIFY2(ink(rowGrab->image()) > ink(footerGrab->image()) * 1.2,kind);
        }
        // A rejected draft save must leave both navigation and note scope intact.
        window->setProperty("syncingNote", true);
        item("noteEditor")->setProperty("text", "Unsaved current draft");
        window->setProperty("syncingNote", false);
        window->setProperty("noteConflict", true);
        click(note);
        QCOMPARE(bible->selectionReference(),QString("John 3:5"));
        QVERIFY(!prop("notesDrawer","visible").toBool());
        window->setProperty("noteConflict", false);
        click(note);
        QTRY_VERIFY(prop("notesDrawer","opened").toBool());
        QCOMPARE(bible->tagScope(),scope);
        QCOMPARE(bible->noteText(),QString("Exact range note"));
    }
    void tagGlyphPointsLeft() {
        reference("John 3:1");
        QVERIFY(bible->toggleBookmark());
        QVERIFY(!bible->createAndAssignTag(bible->tagScope(),"Glyph").isEmpty());
        // Annotations rows no longer show tag glyphs; check the reader rail's.
        auto glyph = item("tagGlyph1"); QTRY_VERIFY(glyph && glyph->isVisible());
        QTest::qWait(100);
        auto grab = glyph->grabToImage(QSize(120,140)); QVERIFY(grab);
        QSignalSpy ready(grab.data(),&QQuickItemGrabResult::ready);
        QTRY_VERIFY(!grab->image().isNull());
        const auto image = grab->image();
        // The hole and vertical shoulder are on the right, with the point on
        // the left. Check rendered ink at fixed landmarks, not a mirrored copy.
        const auto ink = [&image](QRect region) {
            int amount = 0;
            for (int y = region.top(); y <= region.bottom(); ++y)
                for (int x = region.left(); x <= region.right(); ++x)
                    amount += image.pixelColor(x,y).alpha();
            return amount;
        };
        QVERIFY(ink(QRect(70,20,30,35)) > 1000);
        QVERIFY(ink(QRect(104,20,12,28)) > ink(QRect(4,20,12,28)) + 1000);
    }
    void savedTabFocusActivation() {
        reference("John 3:1"); bible->toggleBookmark();
        reference("John 3:2"); bible->toggleBookmark(); bible->toggleHighlight();
        reference("John 4:1");
        QSignalSpy navigation(bible.get(),&BibleBackend::navigationChanged);
        const auto tabTo = [this](const char *name) {
            QStringList visited;
            for (int i = 0; i < 40; ++i) {
                key(Qt::Key_Tab);
                if (item(name) && window->activeFocusItem() == item(name)) return true;
                visited << (window->activeFocusItem() ? window->activeFocusItem()->objectName() : "<none>");
            }
            qWarning() << "Tab target" << name << "visited" << visited;
            return false;
        };
        key(Qt::Key_A,Qt::ShiftModifier);
        QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        QVERIFY(!item("savedClose"));
        key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
        QCOMPARE(navigation.count(),0);

        key(Qt::Key_A,Qt::ShiftModifier);
        QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        QVERIFY(tabTo("savedTab1")); key(Qt::Key_Enter);
        QCOMPARE(window->property("collectionTab").toInt(),1);
        QCOMPARE(prop("collectionList","count").toInt(),1);
        QCOMPARE(navigation.count(),0);
        QVERIFY(tabTo("savedTab0")); key(Qt::Key_Space);
        QCOMPARE(window->property("collectionTab").toInt(),0);
        QCOMPARE(prop("collectionList","count").toInt(),2);

        for (const auto name : {"savedExport","savedBackup"}) {
            QVERIFY(tabTo(name));
            auto dialog = object(QByteArray(name) == "savedExport" ? "exportDialog" : "backupDialog");
            QVERIFY(dialog);
            // Inspect opening and cancel; never choose a path or write a file.
            key(Qt::Key_Return);
            if (QByteArray(name) == "savedBackup") {
                QTRY_VERIFY(prop("backupScope","opened").toBool());
                QVERIFY(!dialog->property("visible").toBool());
                click(item("backupChoose"));
                QTRY_VERIFY(!prop("backupScope","visible").toBool());
            }
            QTRY_VERIFY(dialog->property("visible").toBool());
            QCOMPARE(navigation.count(),0);
            QVERIFY(QMetaObject::invokeMethod(dialog,"reject"));
            QTRY_VERIFY(!dialog->property("visible").toBool());
            QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
            window->requestActivate();
            QTRY_VERIFY(window->isActive());
            item("readerSurface")->forceActiveFocus();
            key(Qt::Key_A,Qt::ShiftModifier);
            QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        }
        for (auto activation : {Qt::Key_Space,Qt::Key_Return,Qt::Key_Enter}) {
            QVERIFY(tabTo("savedEntryJohn 3:2"));
            // Tab focus and list selection are independent; activate the focused row.
            object("collectionList")->setProperty("currentIndex",0);
            QCOMPARE(window->activeFocusItem(),item("savedEntryJohn 3:2"));
            key(activation);
            QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
            QCOMPARE(bible->selectionReference(),QString("John 3:2"));
            key(Qt::Key_A,Qt::ShiftModifier);
            QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        }
        object("collectionList")->setProperty("currentIndex",0);
        QVERIFY(tabTo("savedTab0")); key(Qt::Key_J);
        QCOMPARE(window->activeFocusItem(),item("collectionList"));
        key(Qt::Key_Return);
        QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
        QCOMPARE(bible->selectionReference(),QString("John 3:2"));
    }
    void savedDeleteUsesTabFocusedEntry_data() {
        QTest::addColumn<int>("deleteKey");
        QTest::newRow("delete") << int(Qt::Key_Delete);
        QTest::newRow("backspace") << int(Qt::Key_Backspace);
    }
    void savedDeleteUsesTabFocusedEntry() {
        QFETCH(int,deleteKey);
        reference("John 3:1"); bible->toggleBookmark();
        reference("John 3:2"); bible->toggleBookmark();
        reference("John 3:3"); bible->toggleBookmark();
        key(Qt::Key_A,Qt::ShiftModifier);
        QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        for (int i = 0; i < 40 && window->activeFocusItem() != item("savedEntryJohn 3:2"); ++i)
            key(Qt::Key_Tab);
        QCOMPARE(window->activeFocusItem(),item("savedEntryJohn 3:2"));
        object("collectionList")->setProperty("currentIndex",0);
        QCOMPARE(window->activeFocusItem(),item("savedEntryJohn 3:2"));
        QVERIFY(prop("savedEntryJohn 3:1","highlighted").toBool());
        key(Qt::Key(deleteKey));
        QTRY_VERIFY(prop("deleteConfirmation","opened").toBool());
        QCOMPARE(prop("deleteReference","text").toString(),QString("John 3:2"));
        QCOMPARE(prop("deleteConfirmation","row").toInt(),1);
        click(item("deleteConfirm"));
        QTRY_VERIFY(!prop("deleteConfirmation","visible").toBool());
        QVERIFY(!saved(1).isEmpty()); QVERIFY(saved(2).isEmpty()); QVERIFY(!saved(3).isEmpty());
        QCOMPARE(prop("collectionList","currentIndex").toInt(),1);
        // With focus back on the list, deletion falls back to its current row.
        item("collectionList")->forceActiveFocus();
        key(Qt::Key(deleteKey));
        QTRY_VERIFY(prop("deleteConfirmation","opened").toBool());
        QCOMPARE(prop("deleteReference","text").toString(),QString("John 3:3"));
        key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("deleteConfirmation","visible").toBool());
        QVERIFY(!saved(3).isEmpty());
    }
    void savedKeyboardIsolationAndToggle() {
        for (int verse = 1; verse <= 12; ++verse) {
            QVERIFY(bible->navigate(QString("John 3:%1").arg(verse)));
            bible->toggleBookmark();
            if (verse == 2) bible->toggleHighlight();
            if (verse == 3) bible->setNoteText("Saved keyboard note");
        }
        reference("John 4:2");
        QTest::qWait(450);
        const auto collection = bible->collection();
        const auto position = prop("reading","contentY").toDouble();
        QSignalSpy navigation(bible.get(),&BibleBackend::navigationChanged);
        QSignalSpy selection(bible.get(),&BibleBackend::selectionChanged);
        QApplication::clipboard()->setText("Saved isolation");
        key(Qt::Key_A,Qt::ShiftModifier);
        QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        QVERIFY(item("collectionList")->hasActiveFocus());
        QCOMPARE(prop("savedTitle","text").toString(),QString("Annotations"));
        QCOMPARE(prop("savedDrawer","edge").toInt(),int(Qt::RightEdge));
        key(Qt::Key_J); QCOMPARE(prop("collectionList","currentIndex").toInt(),1);
        key(Qt::Key_Down); QCOMPARE(prop("collectionList","currentIndex").toInt(),2);
        key(Qt::Key_K); QCOMPARE(prop("collectionList","currentIndex").toInt(),1);
        key(Qt::Key_Up); QCOMPARE(prop("collectionList","currentIndex").toInt(),0);
        for (int i = 0; i < 20; ++i) key(Qt::Key_J);
        QCOMPARE(prop("collectionList","currentIndex").toInt(),11);
        // The current block holds its (Tags: headed) row.
        auto block = qvariant_cast<QQuickItem *>(prop("collectionList","currentItem"));
        QVERIFY(block);
        QQuickItem *current = nullptr;
        for (auto child : block->childItems()) if (child->property("highlighted").isValid()) current = child;
        QVERIFY(current);
        QVERIFY(current->property("highlighted").toBool());
        const auto rowBounds = current->mapRectToItem(item("collectionList"),current->boundingRect());
        QVERIFY(item("collectionList")->boundingRect().adjusted(-1,-1,1,1).contains(rowBounds));
        const auto background = qvariant_cast<QQuickItem *>(current->property("background"));
        QVERIFY(background);
        QVERIFY(background->property("color").value<QColor>().alpha() > 0);
        QCOMPARE(qvariant_cast<QObject *>(background->property("border"))->property("width").toReal(),0.0);
        key(Qt::Key_L); QCOMPARE(window->property("collectionTab").toInt(),1);
        QCOMPARE(prop("collectionList","count").toInt(),1);
        key(Qt::Key_Right); QCOMPARE(window->property("collectionTab").toInt(),2);
        QCOMPARE(prop("collectionList","count").toInt(),1);
        key(Qt::Key_H); QCOMPARE(window->property("collectionTab").toInt(),1);
        key(Qt::Key_Left); QCOMPARE(window->property("collectionTab").toInt(),0);
        for (const auto k : {Qt::Key_M,Qt::Key_X,Qt::Key_N,Qt::Key_V,Qt::Key_G,
                            Qt::Key_BracketLeft,Qt::Key_BracketRight,Qt::Key_PageDown,Qt::Key_Space}) {
            key(k);
            QVERIFY2(navigation.count() == 0,qPrintable(QString("Saved leaked key %1").arg(int(k))));
        }
        key(Qt::Key_J,Qt::ShiftModifier);
        key(Qt::Key_Right,Qt::AltModifier);
        key(Qt::Key_H,Qt::ControlModifier);
        key(Qt::Key_D,Qt::ControlModifier);
        key(Qt::Key_N,Qt::ControlModifier);
        key(Qt::Key_K,Qt::ControlModifier);
        QCOMPARE(navigation.count(),0); QCOMPARE(selection.count(),0);
        QCOMPARE(bible->collection(),collection);
        QCOMPARE(prop("reading","contentY").toDouble(),position);
        QCOMPARE(QApplication::clipboard()->text(),QString("Saved isolation"));
        QVERIFY(!window->property("verseMode").toBool());
        QVERIFY(!window->property("notesOpen").toBool());
        QVERIFY(!prop("passagePalette","visible").toBool());
        key(Qt::Key_A,Qt::ShiftModifier); QTest::qWait(50); QVERIFY(prop("savedDrawer","visible").toBool()); key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
        QVERIFY(item("readerSurface")->hasActiveFocus());
        key(Qt::Key_A,Qt::ShiftModifier);
        QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        key(Qt::Key_Right); key(Qt::Key_Return);
        QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
        QCOMPARE(bible->selectionReference(),QString("John 3:2"));
        key(Qt::Key_A,Qt::ShiftModifier);
        QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        key(Qt::Key_Right); key(Qt::Key_Enter);
        QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
        QCOMPARE(bible->selectionReference(),QString("John 3:3"));
        QCOMPARE(bible->noteText(),QString("Saved keyboard note"));
        QVERIFY(window->property("notesOpen").toBool());
    }
    void savedEmptyStates() {
        window->resize(640,480);
        QVERIFY(desktop->setProperty("textScale",1.8));
        const QStringList messages = {
            "No bookmarks yet.", "No highlights yet.", "No notes yet."
        };
        key(Qt::Key_A,Qt::ShiftModifier);
        QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        for (int tab = 0; tab < 3; ++tab) {
            QTest::qWait(100);
            QCOMPARE(window->property("collectionTab").toInt(),tab);
            QCOMPARE(prop("collectionList","count").toInt(),0);
            QVERIFY(item("savedEmpty")->isVisible());
            QCOMPARE(prop("savedEmpty","text").toString(),messages[tab]);
            const auto bounds = item("savedEmpty")->mapRectToItem(item("collectionList"),item("savedEmpty")->boundingRect());
            QVERIFY(item("collectionList")->boundingRect().contains(bounds));
            key(Qt::Key_J); key(Qt::Key_K); key(Qt::Key_Return);
            QVERIFY(prop("savedDrawer","opened").toBool());
            key(Qt::Key_Right);
        }
        // Tags is the fourth tab: the same list, with its own empty state.
        QTRY_COMPARE(window->property("collectionTab").toInt(),3);
        QVERIFY(!prop("tagsUi","visibleModal").toBool());
        QVERIFY(prop("savedDrawer","visible").toBool());
        QCOMPARE(prop("collectionList","count").toInt(),0);
        QVERIFY(item("savedEmpty")->isVisible());
        QCOMPARE(prop("savedEmpty","text").toString(),QString("No tagged passages yet. Press t on a passage to tag it."));
        QVERIFY(!item("tagSearch")->isVisible());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
        key(Qt::Key_A,Qt::ShiftModifier); QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        QCOMPARE(window->property("collectionTab").toInt(),3);
        click(item("savedTab0")); QCOMPARE(window->property("collectionTab").toInt(),0);
        key(Qt::Key_Left);
        QCOMPARE(window->property("collectionTab").toInt(),3);
        QVERIFY(prop("savedDrawer","visible").toBool());
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
        key(Qt::Key_A,Qt::ShiftModifier);
        QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        item("collectionList")->forceActiveFocus();
        key(Qt::Key_A,Qt::ShiftModifier); QTest::qWait(50); QVERIFY(prop("savedDrawer","visible").toBool()); key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("savedDrawer","visible").toBool());
    }
    void savedPanelGeometry_data() {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<double>("scale");
        QTest::newRow("wide") << QSize(1120,820) << 1.0;
        QTest::newRow("narrow") << QSize(640,480) << 1.0;
        QTest::newRow("scaled") << QSize(640,640) << 1.8;
        QTest::newRow("scaled-short") << QSize(640,480) << 1.8;
    }
    void savedPanelGeometry() {
        QFETCH(QSize,size);
        QFETCH(double,scale);
        for (int verse = 1; verse <= 12; ++verse) {
            QVERIFY(bible->navigate(QString("John 3:%1").arg(verse)));
            bible->toggleBookmark();
            bible->setNoteText("A saved thought with enough text to wrap comfortably within the entry padding.");
        }
        window->resize(size);
        QVERIFY(desktop->setProperty("textScale",scale));
        key(Qt::Key_A,Qt::ShiftModifier);
        QTRY_VERIFY(prop("savedDrawer","opened").toBool());
        QTest::qWait(150);
        auto content = item("savedContent");
        QVERIFY(content);
        const double padding = 16 * qMin(scale,1.4);
        QCOMPARE(prop("savedDrawer","padding").toDouble(),padding);
        const auto bounds = content->mapRectToScene(content->boundingRect());
        QCOMPARE(prop("savedDrawer","edge").toInt(),int(Qt::RightEdge));
        QCOMPARE(prop("savedTitle","text").toString(),QString("Annotations"));
        QCOMPARE(bounds.left(),size.width() - prop("savedDrawer","width").toDouble() + padding);
        QVERIFY(qAbs(bounds.top() - padding) < 0.1);
        QVERIFY(qAbs(size.width() - bounds.right() - padding) < 0.1);
        QVERIFY(qAbs(size.height() - bounds.bottom() - padding) < 0.1);
        auto tabs = item("savedTabs");
        QVERIFY(tabs);
        QList<QRectF> tabRects;
        const QStringList labels = {"Bookmarks", "Highlights", "Notes", "Tags"};
        for (int index = 0; index < 4; ++index) {
            auto tab = item(qPrintable("savedTab" + QString::number(index)));
            QVERIFY(tab);
            const auto tabBounds = tab->mapRectToItem(tabs,tab->boundingRect());
            QVERIFY2(tabs->boundingRect().adjusted(-0.5,-0.5,0.5,0.5).contains(tabBounds),
                qPrintable(QString("Tab %1: %2,%3 %4x%5 in %6x%7").arg(index).arg(tabBounds.x()).arg(tabBounds.y())
                    .arg(tabBounds.width()).arg(tabBounds.height()).arg(tabs->width()).arg(tabs->height())));
            const auto label = qvariant_cast<QQuickItem *>(tab->property("contentItem"));
            QVERIFY(label);
            QCOMPARE(label->property("text").toString(),labels[index]);
            QCOMPARE(label->property("lineCount").toInt(),1);
            QVERIFY(!label->property("truncated").toBool());
            if (!tabRects.isEmpty()) QCOMPARE(tabBounds.top(),tabRects.first().top());
            QVERIFY2(label->width() + 0.5 >= label->implicitWidth(),
                qPrintable(QString("Tab %1 label: %2 available, %3 required").arg(index).arg(label->width()).arg(label->implicitWidth())));
            for (const auto &previous : tabRects) QVERIFY(!previous.intersects(tabBounds));
            tabRects.append(tabBounds);
        }
        auto list = item("collectionList");
        QVERIFY(list->clip());
        QVERIFY(list->height() > 80);
        auto entry = item("savedEntryJohn 3:1");
        QTRY_VERIFY(entry);
        QVERIFY(entry->clip());
        auto entryContent = item("savedEntryContentJohn 3:1");
        QVERIFY(entryContent);
        const auto inner = entryContent->mapRectToItem(entry,entryContent->boundingRect());
        QVERIFY(qAbs(inner.left() - 12 * scale) < 0.1);
        QVERIFY(qAbs(inner.top() - 12 * scale) < 0.1);
        QVERIFY(qAbs(entry->width() - inner.right() - 12 * scale) < 0.1);
        QVERIFY(qAbs(entry->height() - inner.bottom() - 12 * scale) < 0.1);
        auto bar = item("savedScrollbar");
        QVERIFY(bar);
        QCOMPARE(entry->width(),list->width());
        QVERIFY(qAbs(bar->mapToItem(list,QPointF()).x() + bar->width() - list->width()) < 0.1);
        capture("saved-" + QString::fromLatin1(QTest::currentDataTag()));
        click(item("savedTab1"));
        QTRY_COMPARE(prop("collectionList","count").toInt(),0);
        click(item("savedTab0"));
        QTRY_COMPARE(prop("collectionList","count").toInt(),12);
        // Scrolling preserves the full-width clipped viewport and overlay bar.
        QVERIFY(QMetaObject::invokeMethod(list,"positionViewAtEnd"));
        QTRY_VERIFY(prop("collectionList","contentY").toDouble() > 0);
    }
    void restoreConfirmationCancelAndAccept() {
        reference("John 3:16");
        key(Qt::Key_X); key(Qt::Key_M);
        key(Qt::Key_W);
        QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        type("Original backup note n j k h m");
        QCOMPARE(bible->noteText(),QString("Original backup note n j k h m"));
        QCOMPARE(bible->selectedStart(),16);
        const auto backup = QUrl::fromLocalFile(session->filePath("restore-test.json"));
        QVERIFY(bible->exportBackup(backup));
        key(Qt::Key_A,Qt::ControlModifier); type("Current note to preserve on cancel");
        reference("John 4:2");
        const auto current = bible->collection();
        const auto request = [&] {
            return QMetaObject::invokeMethod(window,"requestRestore",Q_ARG(QVariant,QVariant::fromValue(backup)));
        };
        QVERIFY(request());
        QTRY_VERIFY(prop("restoreConfirmation","opened").toBool());
        QTRY_VERIFY(item("restoreCancel")->hasActiveFocus());
        QCOMPARE(prop("restoreBackupPath","text").toString(),backup.toString());
        const auto scope = prop("restoreScopeExplanation","text").toString();
        QVERIFY(scope.contains(bible->edition()));
        QVERIFY(scope.contains("(NET)"));
        QVERIFY(scope.contains("Other editions and global preferences are untouched"));
        QVERIFY(window->property("modalOpen").toBool());
        key(Qt::Key_N); key(Qt::Key_W); key(Qt::Key_H); key(Qt::Key_J);
        key(Qt::Key_Right,Qt::AltModifier);
        key(Qt::Key_K,Qt::ControlModifier);
        QVERIFY(!prop("passagePalette","visible").toBool());
        QCOMPARE(bible->chapter(),4); QCOMPARE(bible->selectedStart(),2);
        QCOMPARE(bible->collection(),current);
        // Return on the initially focused Cancel must not restore.
        key(Qt::Key_Return);
        QTRY_VERIFY(!prop("restoreConfirmation","visible").toBool());
        QCOMPARE(bible->collection(),current); QCOMPARE(bible->chapter(),4);
        QVERIFY(request());
        QTRY_VERIFY(prop("restoreConfirmation","opened").toBool());
        key(Qt::Key_Escape);
        QTRY_VERIFY(!prop("restoreConfirmation","visible").toBool());
        QCOMPARE(bible->collection(),current);
        QVERIFY(request());
        QTRY_VERIFY(prop("restoreConfirmation","opened").toBool());
        key(Qt::Key_Tab);
        QTRY_VERIFY(item("restoreConfirm")->hasActiveFocus());
        key(Qt::Key_Return);
        QTRY_VERIFY(!prop("restoreConfirmation","visible").toBool());
        QCOMPARE(bible->chapter(),3); QCOMPARE(bible->selectedStart(),16);
        QCOMPARE(bible->noteText(),QString("Original backup note n j k h m"));
        QVERIFY(bible->selectionHighlighted()); QVERIFY(bible->selectionBookmarked());
        QCOMPARE(saved(16).value("note").toString(),bible->noteText());
        QCOMPARE(prop("toast","text").toString(),QString("Restored NET annotation backup"));
    }
    void narrowLayoutAndSystemScale() {
        reference("John 3:16");
        window->resize(640,480);
        key(Qt::Key_W);
        QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        QTRY_VERIFY(item("verseNumber16")->mapToItem(item("reading"),QPointF()).y() >= 0);
        QTRY_VERIFY(item("verseNumber16")->mapToItem(item("reading"),QPointF()).y() <= window->property("readerLineHeight").toDouble() * 3 + 1);
        QTRY_VERIFY(prop("notesDrawer","opened").toBool());
        auto panel = item("notesPanel");
        QVERIFY(panel->isVisible());
        const QRectF panelBounds(panel->mapToScene(QPointF()),QSizeF(panel->width(),panel->height()));
        QVERIFY(QRectF(0,0,640,480).contains(panelBounds));
        QVERIFY(item("noteEditor")->width() > 100);
        type("Narrow window note jkhm");
        QCOMPARE(bible->noteText(),QString("Narrow window note jkhm"));
        key(Qt::Key_Escape); key(Qt::Key_Escape);
        QTRY_VERIFY(!panel->isVisible());
        // Desktop's public property uses the same NOTIFY route as a live theme.
        QVERIFY(desktop->setProperty("textScale",1.8));
        QTRY_COMPARE(window->property("scale").toDouble(),1.8);
        QCOMPARE(window->property("font").value<QFont>().family(),desktop->property("fontFamily").toString());
        reference("Jn 4:1");
        QCOMPARE(bible->chapter(),4);
        key(Qt::Key_W);
        QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        QVERIFY(item("noteEditor")->height() >= 100);
        QVERIFY(prop("notesScroll","contentHeight").toDouble() > item("notesScroll")->height());
        // Focusing the editor scrolls the outer panel into view at large scale.
        QTRY_VERIFY(item("noteEditor")->mapToScene(QPointF()).y() < window->height() - 100);
        type("Large-scale editing remains usable.");
        QCOMPARE(bible->noteText(),QString("Large-scale editing remains usable."));
        window->update(); QTest::qWait(250);
        QVERIFY(window->grabWindow().save(fixtures.filePath("omaword-reader-scaled.png")));
    }
    void screenshot() {
        reference("John 3:16-18");
        key(Qt::Key_X);
        key(Qt::Key_M);
        key(Qt::Key_W);
        QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
        type("# A love that gives\nRead the whole conversation.\n\n- Notice the movement from love to life.\n- Return to this passage slowly.");
        key(Qt::Key_Escape);
        QSignalSpy frames(window,&QQuickWindow::frameSwapped);
        window->update();
        QTRY_VERIFY_WITH_TIMEOUT(frames.count() > 0,5000);
        QTest::qWait(250);
        const auto image = window->grabWindow();
        QVERIFY(!image.isNull());
        QCOMPARE(image.size(),window->size() * window->devicePixelRatio());
        QVERIFY(image.save(fixtures.filePath("omaword-reader.png")));
        window->resize(640,480);
        window->update(); QTest::qWait(250);
        QVERIFY(window->grabWindow().save(fixtures.filePath("omaword-reader-narrow.png")));
    }
    QVariant callJs(const char *method, const QVariantList &args = {}) {
        QVariant result;
        bool ok = false;
        switch (args.size()) {
        case 0: ok = QMetaObject::invokeMethod(window,method,Q_RETURN_ARG(QVariant,result)); break;
        case 1: ok = QMetaObject::invokeMethod(window,method,Q_RETURN_ARG(QVariant,result),Q_ARG(QVariant,args[0])); break;
        default: ok = QMetaObject::invokeMethod(window,method,Q_RETURN_ARG(QVariant,result),Q_ARG(QVariant,args[0]),Q_ARG(QVariant,args[1])); break;
        }
        if (!ok) qWarning("callJs failed: %s", method);
        return result.canConvert<QJSValue>() ? result.value<QJSValue>().toVariant() : result;
    }
    static QVariantMap jsMap(const QVariant &value) {
        return value.canConvert<QJSValue>() ? value.value<QJSValue>().toVariant().toMap() : value.toMap();
    }
    QStringList paletteTitles() {
        QStringList titles;
        for (const auto &entry : callJs("paletteEntries").toList()) titles << entry.toMap()["title"].toString();
        return titles;
    }
    void paletteFuzzyCandidates() {
        key(Qt::Key_K,Qt::ControlModifier);
        QTRY_VERIFY(prop("passagePalette","opened").toBool());
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("jn316");
        QCOMPARE(paletteTitles().value(0),QString("John 3:16"));
        QCOMPARE(prop("referencePreview","text").toString(),QString("John 3:16"));
        key(Qt::Key_Return);
        QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        QCOMPARE(bible->selectionReference(),QString("John 3:16"));
        // Ambiguous input lists ranked candidates; arrows choose, j/k still type.
        key(Qt::Key_K,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("j4");
        const auto titles = paletteTitles();
        QVERIFY2(titles.size() >= 4,qPrintable(titles.join(", ")));
        // Which J books have a chapter 4 depends on the fixture (NET: no Joel 4 or Jude 4).
        if (netSource().isEmpty()) QCOMPARE(titles.mid(0,4),(QStringList{"Job 4","Joel 4","John 4","Jude 4"}));
        QCOMPARE(titles.value(0),QString("Job 4"));
        for (const auto &title : titles.mid(0,4)) QVERIFY2(title.startsWith('J') && title.endsWith('4'),qPrintable(titles.join(", ")));
        QCOMPARE(prop("paletteResults","currentIndex").toInt(),0);
        key(Qt::Key_Down); key(Qt::Key_Down); key(Qt::Key_Down); key(Qt::Key_Up);
        QCOMPARE(prop("paletteResults","currentIndex").toInt(),2);
        QCOMPARE(prop("referencePreview","text").toString(),titles.value(2));
        type("k"); QCOMPARE(prop("referenceInput","text").toString(),QString("j4k"));
        key(Qt::Key_Backspace); QCOMPARE(prop("paletteResults","currentIndex").toInt(),0);
        const int john = titles.indexOf("John 4"); QVERIFY2(john > 0,qPrintable(titles.join(", ")));
        for (int i = 0; i < john; ++i) key(Qt::Key_Down);
        key(Qt::Key_Return);
        QTRY_VERIFY(!prop("passagePalette","visible").toBool());
        QCOMPARE(bible->passage(),QString("John 4"));
        // Exact forms are still first; book browsing still lists books.
        key(Qt::Key_K,Qt::ControlModifier);
        QTRY_VERIFY(item("referenceInput")->hasActiveFocus());
        type("Rev 22:1-5"); QCOMPARE(paletteTitles().value(0),QString("Revelation 22:1–5"));
        key(Qt::Key_A,Qt::ControlModifier); type("1 co");
        QCOMPARE(paletteTitles().value(0),QString("1 Corinthians"));
        key(Qt::Key_Escape); QTRY_VERIFY(!prop("passagePalette","visible").toBool());
    }
    void openNotesOn(const QString &passage) {
        reference(passage);
        key(Qt::Key_W); QTRY_VERIFY(item("noteEditor")->hasActiveFocus());
    }
    void noteLinkSuggestions() {
        openNotesOn("John 3:16");
        auto popup = item("noteLinkPopup"); QVERIFY(popup);
        type("See [[");
        QTRY_VERIFY(popup->isVisible());
        type("j4");
        QTRY_VERIFY(item("noteLinkSuggestion3"));
        QCOMPARE(jsMap(prop("noteLinkSuggestion0","modelData"))["reference"].toString(),QString("Job 4"));
        // Under the cursor.
        const auto caret = item("noteEditor")->mapToItem(popup->parentItem(),prop("noteEditor","cursorRectangle").toRectF().bottomLeft());
        QVERIFY(std::abs(popup->y() - caret.y() - 4) < 2);
        key(Qt::Key_Down); key(Qt::Key_Down); key(Qt::Key_Up);
        QCOMPARE(window->property("linkSuggestionIndex").toInt(),1);
        // The fixture decides which book is second (Joel 4 here; NET's Joel has 3 chapters).
        const QString second = jsMap(prop("noteLinkSuggestion1","modelData"))["reference"].toString();
        QVERIFY(second.endsWith(" 4") && second != "Job 4");
        if (netSource().isEmpty()) QCOMPARE(second,QString("Joel 4"));
        key(Qt::Key_Tab);
        QTRY_VERIFY(!popup->isVisible());
        QCOMPARE(prop("noteEditor","text").toString(),"See " + second);
        QVERIFY(item("noteEditor")->hasActiveFocus());
        // Esc dismisses and leaves the text; Notes stays open.
        type(" [[jn3");
        QTRY_VERIFY(popup->isVisible());
        key(Qt::Key_Escape);
        QVERIFY(!popup->isVisible());
        QVERIFY(window->property("notesOpen").toBool());
        QCOMPARE(prop("noteEditor","text").toString(),"See " + second + " [[jn3");
        type("1"); QVERIFY(!popup->isVisible());
        for (int i = 0; i < 7; ++i) key(Qt::Key_Backspace);
        QCOMPARE(prop("noteEditor","text").toString(),"See " + second);
        // Enter accepts the first candidate: the full reference, no brackets, no newline.
        type(" and [[jn42");
        QTRY_VERIFY(popup->isVisible());
        QCOMPARE(jsMap(prop("noteLinkSuggestion0","modelData"))["reference"].toString(),QString("John 4:2"));
        key(Qt::Key_Return);
        QCOMPARE(prop("noteEditor","text").toString(),"See " + second + " and John 4:2");
        QTRY_COMPARE(bible->noteText(),"See " + second + " and John 4:2");
    }
    void noteLinksAndLinkedFrom() {
        openNotesOn("John 3:16");
        type("See Joel 4 and John 4:2");
        QTRY_COMPARE(bible->noteText(),QString("See Joel 4 and John 4:2"));
        // On a link, the hint row offers alt enter.
        QTRY_VERIFY(item("notesHints")->isVisible());
        QCOMPARE(plainHint(prop("notesHints","text").toString()),QString("alt enter open link"));
        item("noteEditor")->setProperty("cursorPosition",1);
        QTRY_VERIFY(!item("notesHints")->isVisible());
        item("noteEditor")->setProperty("cursorPosition",20);
        QTRY_VERIFY(item("notesHints")->isVisible());
        // alt enter follows the link as a History jump; Notes stays open.
        const int jumps = window->property("jumpIndex").toInt();
        key(Qt::Key_Return,Qt::AltModifier);
        QTRY_COMPARE(bible->selectionReference(),QString("John 4:2"));
        QCOMPARE(window->property("jumpIndex").toInt(),jumps + 1);
        QVERIFY(window->property("notesOpen").toBool());
        QCOMPARE(prop("noteEditor","text").toString(),QString());
        // Linked from: notes elsewhere that reference this passage.
        QTRY_VERIFY(item("linkedFrom")->isVisible());
        QCOMPARE(jsMap(prop("linkedFromRow0","modelData"))["reference"].toString(),QString("John 3:16"));
        QVERIFY(!item("linkedFromRow1"));
        click(item("linkedFromRow0"));
        QTRY_COMPARE(bible->selectionReference(),QString("John 3:16"));
        QCOMPARE(window->property("jumpIndex").toInt(),jumps + 2);
        QTRY_COMPARE(prop("noteEditor","text").toString(),QString("See Joel 4 and John 4:2"));
        QTRY_VERIFY(!item("linkedFrom")->isVisible());
        // Preview renders links; activating one jumps.
        click(item("previewButton"));
        QTRY_VERIFY(item("notePreview")->isVisible());
        const auto document = prop("notePreview","textDocument").value<QQuickTextDocument *>()->textDocument();
        QVERIFY(document->toHtml().contains("omaword-passage:"));
        const auto found = document->find("John 4:2");
        QVERIFY(!found.isNull());
        QRectF rect;
        QMetaObject::invokeMethod(object("notePreview"),"positionToRectangle",Q_RETURN_ARG(QRectF,rect),Q_ARG(int,found.selectionStart() + 2));
        const auto point = item("notePreview")->mapToScene(rect.center()).toPoint();
        QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,point);
        QTRY_COMPARE(bible->selectionReference(),QString("John 4:2"));
        // Compact text never links.
        QVERIFY(bible->passageLinks("jn316 and Jn3:16").isEmpty());
    }
    void readerSlideRetargetsAndRespectsAnimations() {
        QVERIFY(!sounds->reduceMotion());
        reference("John 3:1");
        auto slide = object("readerSlide"); QVERIFY(slide);
        QCOMPARE(window->property("slideMs").toInt(),110);
        auto reading = item("reading");
        const double start = reading->property("contentY").toDouble();
        callJs("scrollToVerse",{30,true});
        QVERIFY(slide->property("running").toBool());
        const double first = callJs("readerTargetY").toDouble();
        QVERIFY(first > start + 100);
        QTest::qWait(40);
        const double mid = reading->property("contentY").toDouble();
        QVERIFY2(mid > start && mid < first,qPrintable(QString("%1 %2 %3").arg(start).arg(mid).arg(first)));
        // Retarget mid-flight: from the current position, never jumping back.
        callJs("scrollToVerse",{12,true});
        const double second = callJs("readerTargetY").toDouble();
        QVERIFY(second < first);
        QVERIFY(std::abs(slide->property("from").toDouble() - mid) < 60);
        QTRY_VERIFY_WITH_TIMEOUT(!slide->property("running").toBool(),1000);
        QCOMPARE(reading->property("contentY").toDouble(),second);
        // Held keys keep retargeting: the target is always the newest verse.
        reference("John 3:10");
        for (int i = 0; i < 12; ++i) { key(Qt::Key_J); QTest::qWait(15); }
        QCOMPARE(bible->cursorVerse(),22);
        QTRY_VERIFY_WITH_TIMEOUT(!slide->property("running").toBool(),1000);
        const auto body = item("verseBody22");
        const double top = body->mapToItem(reading,QPointF(0,0)).y();
        QVERIFY(top >= 0 && top + body->height() <= reading->height());
        // zz slides too.
        key(Qt::Key_Z); key(Qt::Key_Z);
        QVERIFY(slide->property("running").toBool() || std::abs(callJs("readerTargetY").toDouble() - reading->property("contentY").toDouble()) < 1);
        QTRY_VERIFY(!slide->property("running").toBool());
        // Disable animations: every scroll is instant.
        sounds->setReduceMotion(true);
        QCOMPARE(window->property("slideMs").toInt(),0);
        callJs("scrollToVerse",{35,true});
        QVERIFY(!slide->property("running").toBool());
        QCOMPARE(reading->property("contentY").toDouble(),callJs("readerTargetY").toDouble());
        const double instant = reading->property("contentY").toDouble();
        key(Qt::Key_K);
        QVERIFY(!slide->property("running").toBool());
        QVERIFY(reading->property("contentY").toDouble() <= instant);
    }
    void focusModeKeepsCursorAt40Percent() {
        sounds->setReduceMotion(true);
        reference("John 3:20");
        key(Qt::Key_F); QTRY_VERIFY(bible->focusMode());
        auto reading = item("reading");
        auto topOf = [&](int verse) {
            auto body = item(qPrintable("verseBody" + QString::number(verse)));
            return body ? body->mapToItem(reading,QPointF(0,0)).y() : -1.0;
        };
        QTRY_VERIFY(std::abs(topOf(20) - 0.4 * reading->height()) < 8);
        for (const auto &step : QList<QPair<Qt::Key,int>>{{Qt::Key_J,21},{Qt::Key_J,22},{Qt::Key_K,21}}) {
            key(step.first); QCOMPARE(bible->cursorVerse(),step.second);
            QTRY_VERIFY2(std::abs(topOf(step.second) - 0.4 * reading->height()) < 8,
                qPrintable(QString("verse %1 at %2 of %3").arg(step.second).arg(topOf(step.second)).arg(reading->height())));
        }
        // The same slide when animations are on.
        sounds->setReduceMotion(false);
        key(Qt::Key_J);
        QVERIFY(object("readerSlide")->property("running").toBool());
        QTRY_VERIFY(!object("readerSlide")->property("running").toBool());
        QVERIFY(std::abs(topOf(22) - 0.4 * reading->height()) < 8);
        key(Qt::Key_F); QTRY_VERIFY(!bible->focusMode());
    }
    void chapterStepsAndHolds() {
        reference("John 3");
        QVERIFY(QMetaObject::invokeMethod(window,"readerFocus")); QCoreApplication::processEvents();
        // Chapter steps, counts, and jumps turn silently.
        const int count = sounds->playCount();
        key(Qt::Key_L); QCOMPARE(bible->chapter(),4);
        key(Qt::Key_H); QCOMPARE(bible->chapter(),3);
        type("5"); key(Qt::Key_L); QCOMPARE(bible->chapter(),8);
        // Twenty chapters back crosses books (Matthew 24 here; Luke 13 in NET).
        type("20"); key(Qt::Key_H);
        if (netSource().isEmpty()) { QCOMPARE(bible->book(),40); QCOMPARE(bible->chapter(),24); }
        QVERIFY(bible->book() < 43);
        QCOMPARE(sounds->playCount(),count);
        reference("John 3");
        const int jumped = sounds->playCount();
        // Holding l repeats chapter steps until release.
        QVERIFY(QMetaObject::invokeMethod(window,"readerFocus")); QCoreApplication::processEvents();
        QTest::keyPress(window,Qt::Key_L);
        QCOMPARE(bible->chapter(),4);
        QTest::qWait(500);
        QVERIFY2(bible->chapter() >= 6,qPrintable(QString::number(bible->chapter())));
        QTest::keyRelease(window,Qt::Key_L);
        const int held = bible->chapter();
        QTest::qWait(300); QCOMPARE(bible->chapter(),held);
        QCOMPARE(sounds->playCount(),jumped);
        // A tap is not a hold.
        key(Qt::Key_H); QCOMPARE(bible->chapter(),held - 1);
        QTest::qWait(300); QCOMPARE(bible->chapter(),held - 1);
    }
};

int main(int argc,char **argv)
{
    QTemporaryDir environment;
    if (!environment.isValid()) return 1;
    // Set before QApplication, fontconfig, Desktop, or QStandardPaths cache state.
    for (const auto name : {"HOME","XDG_DATA_HOME","XDG_CONFIG_HOME","XDG_CACHE_HOME","XDG_STATE_HOME","XDG_RUNTIME_DIR"}) {
        const auto path = environment.filePath(QString::fromLatin1(name));
        QDir().mkpath(path);
        QFile::setPermissions(path,QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        qputenv(name,path.toUtf8());
    }
    qputenv("QT_QPA_PLATFORM","offscreen");
    qputenv("QT_QUICK_BACKEND","software");
    qputenv("QT_FORCE_STDERR_LOGGING","1");
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc,argv);
    app.setOrganizationName("OmaWordUiTests");
    app.setApplicationName("omaword-ui-test");
    QQuickStyle::setStyle("Basic");
    UiTest test;
    return QTest::qExec(&test,argc,argv);
}

#include "tst_ui.moc"
