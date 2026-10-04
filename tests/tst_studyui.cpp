#include <QtTest/QTest>
#include <QApplication>
#include <QClipboard>
#include <QCryptographicHash>
#include <QSqlRecord>
#include <QFile>
#include <QJSValue>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QUuid>
#include <archive.h>
#include <archive_entry.h>
#include "biblebackend.h"
#include "blockdocument.h"
#include "desktop.h"
#include "soundeffects.h"
#include "versioncatalog.h"

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
    Q_INVOKABLE void download(const QString &id) { busy = true; emit changed(); emit requested(id); }
    Q_INVOKABLE void cancel() { busy = false; emit changed(); }
    Q_INVOKABLE void clearError() { error.clear(); emit changed(); }
    void finish(const QUrl &url) { busy = false; progress = 1; emit changed(); emit downloadFinished(url); }
signals:
    void changed();
    void requested(const QString &id);
    void downloadFinished(const QUrl &url);
};

// Study features: words of Jesus, Re-import offers, the Study panel's tabs,
// and reader footnote markers, against a small original fixture with \wj,
// \f, and \x markup.
class StudyUiTest : public QObject
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
    QString source, other;
    QString installedFile() const {
        QString path;
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "study-installed");
            db.setDatabaseName(bible->dataDirectory() + "/personal.sqlite");
            if (db.open()) {
                QSqlQuery q(db); q.prepare("SELECT path FROM installed WHERE version_id=?"); q.addBindValue(bible->versionId());
                if (q.exec() && q.next()) path = bible->dataDirectory() + '/' + q.value(0).toString();
            }
        }
        QSqlDatabase::removeDatabase("study-installed");
        return path;
    }
    // Rewrites a schema 4 generation as the schema 3 layout older builds wrote.
    static bool downgradeToSchema3(const QString &path) {
        const QString name = QUuid::createUuid().toString();
        bool ok = false;
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name); db.setDatabaseName(path);
            if (db.open()) {
                QSqlQuery q(db);
                ok = q.exec("DROP TABLE notes") && q.exec("DROP TABLE words_of_jesus")
                    && q.exec("DELETE FROM metadata WHERE key IN ('note_count','words_of_jesus')")
                    && q.exec("UPDATE metadata SET value='3' WHERE key='schema'")
                    && q.exec("UPDATE verses SET html=replace(replace(html,'<span class=\"wj\">',''),'</span>','')");
                QCryptographicHash hash(QCryptographicHash::Sha256);
                auto field = [&](const QByteArray &v) { hash.addData(QByteArray::number(v.size())); hash.addData(":"); hash.addData(v); };
                for (const QString &sql : {QString("SELECT key,value FROM metadata WHERE key<>'content_digest' ORDER BY key"),
                         QString("SELECT id,code,name,abbreviation,ordinal FROM books ORDER BY ordinal"),
                         QString("SELECT book,chapter FROM source_chapters ORDER BY book,chapter"),
                         QString("SELECT book,name FROM book_aliases ORDER BY book,name"),
                         QString("SELECT book,chapter,number,retained FROM source_keys ORDER BY book,chapter,number"),
                         QString("SELECT book,chapter,number,text,html,heading,poetry,paragraphStart FROM verses ORDER BY book,chapter,number")}) {
                    field(sql.toUtf8()); ok = ok && q.exec(sql);
                    while (q.next()) for (int i = 0; i < q.record().count(); ++i) field(q.value(i).toString().toUtf8());
                }
                q.prepare("UPDATE metadata SET value=? WHERE key='content_digest'"); q.addBindValue(QString::fromLatin1(hash.result().toHex()));
                ok = ok && q.exec();
                db.close();
            }
        }
        QSqlDatabase::removeDatabase(name);
        return ok;
    }
    static bool edition(const QString &path) {
        auto zip = archive_write_new(); archive_write_set_format_zip(zip);
        if (archive_write_open_filename(zip, QFile::encodeName(path).constData()) != ARCHIVE_OK) { archive_write_free(zip); return false; }
        auto book = [](const char *code, const char *name, int chapter, int verses, auto special) {
            QByteArray data = QString("\\id %1 Original study fixture\n\\h %2\n\\toc1 %2\n\\toc2 %2\n").arg(code, name).toUtf8();
            for (int c = 1; c <= chapter; ++c) {
                data += QString("\\c %1\n\\s Study fixture heading\n\\p\n").arg(c).toUtf8();
                for (int v = 1; v <= verses; ++v) {
                    const QString text = special(c, v);
                    data += QString("\\v %1 %2\n").arg(v).arg(text.isEmpty()
                        ? QString("Original study text %1:%2 with enough ordinary words to wrap across a reading line.").arg(c).arg(v) : text).toUtf8();
                }
            }
            return data;
        };
        const QMap<QString, QByteArray> entries = {
            {"01-GENstudy.usfm", book("GEN", "Genesis", 1, 31, [](int, int v) {
                return v == 1 ? QString("In the beginning of the original fixture.\\f + \\fr 1:1 \\ft A Genesis note\\f*") : QString(); })},
            {"40-MATstudy.usfm", book("MAT", "Matthew", 1, 25, [](int, int) { return QString(); })},
            {"43-JHNstudy.usfm", book("JHN", "John", 3, 36, [](int c, int v) {
                if (c == 1 && v == 1) return QString("In the beginning was the Word.\\x - \\xo 1:1 \\xt Gen 1:1; Mt 1:2-3\\x* Original words follow.");
                if (c != 3) return QString();
                if (v == 3) return QString("Jesus answered,\\f + \\fr 3:3 \\ft Or \\fq from above\\f* \\wj Truly I tell you, unless one is born again\\wj* he cannot see.");
                if (v == 16) return QString("For God so loved\\f a \\ft Greek \\fq agapao\\f* the world that he gave\\f b \\ft Or only begotten\\f* his Son.");
                return QString(); })},
        };
        bool ok = true;
        for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
            auto entry = archive_entry_new(); const auto name = it.key().toUtf8();
            archive_entry_set_pathname(entry, name.constData()); archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_perm(entry, 0600); archive_entry_set_size(entry, it.value().size());
            ok = archive_write_header(zip, entry) == ARCHIVE_OK && archive_write_data(zip, it.value().constData(), it.value().size()) == it.value().size() && ok;
            archive_entry_free(entry);
        }
        ok = archive_write_close(zip) == ARCHIVE_OK && ok; archive_write_free(zip); return ok;
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
    static QTextDocument *document(QObject *body) {
        const auto quick = body ? body->property("textDocument").value<QQuickTextDocument *>() : nullptr;
        return quick ? quick->textDocument() : nullptr;
    }
    static QRectF caret(QObject *body, int position) {
        QRectF rect;
        QMetaObject::invokeMethod(body, "positionToRectangle", Q_RETURN_ARG(QRectF, rect), Q_ARG(int, position));
        return rect;
    }
    BlockDocument *blockMapping() const { return window->findChild<BlockDocument *>(); }
    void call(const char *method) { QVERIFY(QMetaObject::invokeMethod(window, method)); QCoreApplication::processEvents(); }
    void key(Qt::Key key, Qt::KeyboardModifiers mods = Qt::NoModifier) { QTest::keyClick(window, key, mods); QCoreApplication::processEvents(); }
    void install(const QString &file) {
        bible->importUsfm(QUrl::fromLocalFile(file)); QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(), 30000);
        QVERIFY2(bible->ready() && bible->error().isEmpty(), qPrintable(bible->error()));
    }
    void navigate(const QString &reference) {
        QVERIFY(bible->navigate(reference)); call("readerFocus"); QTRY_VERIFY(!prop("restoringPosition").toBool());
    }
    void setMode(const QString &mode) {
        bible->setReadingMode(mode); QTRY_COMPARE(prop("continuousReader").toBool(), mode != "verse");
        QTest::qWait(50); QCoreApplication::processEvents();
    }
    // Foreground of the first character of `needle` in a rendered document.
    static QColor colorAt(QTextDocument *doc, const QString &needle) {
        if (!doc) return {};
        const int at = doc->toPlainText().indexOf(needle);
        if (at < 0) return {};
        QTextCursor cursor(doc); cursor.setPosition(at + 1);
        const auto format = cursor.charFormat();
        return format.hasProperty(QTextFormat::ForegroundBrush) ? format.foreground().color() : QColor();
    }
    QObject *renderedBody(int verse) const {
        return prop("continuousReader").toBool() ? object("blockBody") : object("verseBody" + QString::number(verse));
    }

private slots:
    void initTestCase() {
        source = fixtures.filePath("study_usfm.zip"); QVERIFY(edition(source));
        // Another edition (different internal ID) for the wrong-file check.
        other = fixtures.filePath("other_usfm.zip");
        auto zip = archive_write_new(); archive_write_set_format_zip(zip);
        QVERIFY(archive_write_open_filename(zip, QFile::encodeName(other).constData()) == ARCHIVE_OK);
        const QByteArray data = "\\id JHN Another fixture\n\\c 1\n\\p\n\\v 1 Another edition.\n";
        auto entry = archive_entry_new(); archive_entry_set_pathname(entry, "43-JHNother.usfm"); archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0600); archive_entry_set_size(entry, data.size());
        QVERIFY(archive_write_header(zip, entry) == ARCHIVE_OK && archive_write_data(zip, data.constData(), data.size()) == data.size());
        archive_entry_free(entry); QVERIFY(archive_write_close(zip) == ARCHIVE_OK); archive_write_free(zip);
    }
    void init() {
        session = std::make_unique<QTemporaryDir>(); qputenv("XDG_DATA_HOME", session->path().toUtf8());
        bible = std::make_unique<BibleBackend>(); install(source); bible->setReadingMode("verse");
        if (QByteArray(QTest::currentTestFunction()).startsWith("reimport")) {
            // An installed generation from before schema 4, with a note to keep.
            QVERIFY(bible->navigate("John 3:16")); bible->selectVerses(16, 16); bible->setNoteText("Kept note");
            const QString file = installedFile(); bible.reset();
            QVERIFY(downgradeToSchema3(file)); bible = std::make_unique<BibleBackend>();
            QVERIFY2(bible->ready(), qPrintable(bible->error())); QCOMPARE(bible->textSchema(), 3);
        }
        desktop = std::make_unique<Desktop>(); sounds = std::make_unique<SoundEffects>(nullptr, session->filePath("sound.ini"), false); catalog = std::make_unique<MockCatalog>();
        engine = std::make_unique<QQmlApplicationEngine>(); warnings.clear();
        connect(engine.get(), &QQmlEngine::warnings, this, [this](const QList<QQmlError> &errors) { for (const auto &error : errors) warnings << error.toString(); });
        engine->rootContext()->setContextProperty("bible", bible.get());
        engine->rootContext()->setContextProperty("desktop", desktop.get());
        engine->rootContext()->setContextProperty("sounds", sounds.get());
        engine->rootContext()->setContextProperty("catalog", catalog.get());
        engine->load(QUrl::fromLocalFile(QFINDTESTDATA("../qml/Main.qml")));
        QVERIFY2(!engine->rootObjects().isEmpty(), qPrintable(warnings.join('\n')));
        window = qobject_cast<QQuickWindow *>(engine->rootObjects().first()); QVERIFY(window);
        window->resize(1100, 800);
        window->requestActivate(); QVERIFY(QTest::qWaitForWindowExposed(window)); QTRY_VERIFY(window->isActive());
        window->setProperty("historyDwellMs", 3600000);
        navigate("John 3:3");
    }
    void cleanup() {
        engine.reset(); window = nullptr; catalog.reset(); sounds.reset(); desktop.reset(); bible.reset(); session.reset();
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
    }

    void wordsOfJesusColorWithoutLayoutChange() {
        QVERIFY(bible->hasWordsOfJesus()); QVERIFY(!bible->wordsOfJesus());
        const QColor red = desktop->property("color1").value<QColor>(); QVERIFY(red.isValid());
        for (const QString mode : {"verse", "paragraph", "block"}) {
            setMode(mode); navigate("John 3:3");
            auto body = renderedBody(3); QVERIFY(body);
            QTRY_VERIFY(document(body) && document(body)->toPlainText().contains("Truly I tell you"));
            QVERIFY2(!colorAt(document(body), "Truly").isValid() || colorAt(document(body), "Truly") != red, qPrintable(mode));
            const QString text = document(body)->toPlainText();
            const int after = text.indexOf("he cannot see");
            const QRectF before = caret(body, after);
            const double height = body->property("contentHeight").toDouble();
            key(Qt::Key_J, Qt::AltModifier);
            QTRY_VERIFY(bible->wordsOfJesus());
            body = renderedBody(3);
            QTRY_COMPARE(colorAt(document(body), "Truly"), red);
            // Text outside the span keeps the theme color.
            QVERIFY(colorAt(document(body), "Jesus answered") != red);
            QCOMPARE(document(body)->toPlainText(), text);
            QCOMPARE(body->property("contentHeight").toDouble(), height);
            QCOMPARE(caret(body, after), before);
            key(Qt::Key_J, Qt::AltModifier);
            QTRY_VERIFY(!bible->wordsOfJesus());
            body = renderedBody(3);
            QTRY_VERIFY(colorAt(document(body), "Truly") != red);
            QCOMPARE(caret(body, after), before);
        }
        // The preference persists and Settings lists it with its key.
        key(Qt::Key_J, Qt::AltModifier); QTRY_VERIFY(bible->wordsOfJesus());
        BibleBackend reopened; QVERIFY(reopened.wordsOfJesus());
        key(Qt::Key_R, Qt::ControlModifier); QTRY_VERIFY(object("settingsDialog")->property("opened").toBool());
        QVERIFY(item("settingwordsOfJesus") && item("settingwordsOfJesus")->isVisible());
        QVERIFY(!item("wordsOfJesusUnmarked")->isVisible());
    }
    void reimportFromCatalog() {
        catalog->entries = {QVariantMap{{"id","study"},{"name","Study fixture"},{"abbreviation","SF"},{"url","https://example.invalid/study.zip"},
            {"detailsUrl","https://example.invalid"},{"copyright",""}}};
        emit catalog->changed();
        key(Qt::Key_V, Qt::ControlModifier); QTRY_VERIFY(object("versionDialog")->property("opened").toBool());
        auto button = item("versionReimport:study"); QVERIFY(button); QTRY_VERIFY(button->isVisible());
        // Settings offers it too.
        QSignalSpy requests(catalog.get(), &MockCatalog::requested);
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        QCOMPARE(requests.count(), 1); QCOMPARE(requests.first().first().toString(), QString("study"));
        QTRY_VERIFY(object("importGuide")->property("visible").toBool());
        catalog->finish(QUrl::fromLocalFile(source));
        QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(), 30000);
        QVERIFY2(bible->error().isEmpty(), qPrintable(bible->error()));
        QCOMPARE(bible->textSchema(), 4);
        QTRY_COMPARE(object("toast")->property("text").toString(), "Reimported " + bible->versionAbbreviation());
        for (const auto &row : bible->versions()) QVERIFY(!row.toMap()["needsReimport"].toBool());
        QVERIFY(bible->navigate("John 3:16")); bible->selectVerses(16, 16); QCOMPARE(bible->noteText(), QString("Kept note"));
        key(Qt::Key_V, Qt::ControlModifier); QTRY_VERIFY(object("versionDialog")->property("opened").toBool());
        QTRY_VERIFY(!item("versionReimport:study") || !item("versionReimport:study")->isVisible());
    }
    void reimportLocalFilePicker() {
        key(Qt::Key_R, Qt::ControlModifier); QTRY_VERIFY(object("settingsDialog")->property("opened").toBool());
        auto button = item("settingsReimport:study"); QVERIFY(button); QVERIFY(button->isVisible());
        QVERIFY(item("wordsOfJesusUnmarked")->isVisible());
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        auto dialog = object("importDialog"); QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("visible").toBool());
        QCOMPARE(window->property("reimportId").toString(), QString("study"));
        QVERIFY(dialog->property("title").toString().startsWith("Re-import"));
        // Another edition's file is refused, leaving the old text in place.
        // The picker's accepted file goes through importChosen.
        QVERIFY(QMetaObject::invokeMethod(window, "importChosen", Q_ARG(QVariant, QUrl::fromLocalFile(other))));
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QTRY_VERIFY(!bible->busy());
        QVERIFY2(bible->error().contains("not"), qPrintable(bible->error())); QCOMPARE(bible->textSchema(), 3); QCOMPARE(bible->versions().size(), 1);
        bible->clearError();
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QCOMPARE(window->property("reimportId").toString(), QString());
        key(Qt::Key_V, Qt::ControlModifier); QTRY_VERIFY(object("versionDialog")->property("opened").toBool());
        QVERIFY(QMetaObject::invokeMethod(item("versionReimport:study"), "clicked"));
        QTRY_VERIFY(dialog->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(window, "importChosen", Q_ARG(QVariant, QUrl::fromLocalFile(source))));
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QTRY_VERIFY_WITH_TIMEOUT(!bible->busy(), 30000);
        QVERIFY2(bible->error().isEmpty(), qPrintable(bible->error()));
        QCOMPARE(bible->textSchema(), 4); QVERIFY(bible->hasWordsOfJesus());
        QVERIFY(bible->navigate("John 3:16")); bible->selectVerses(16, 16); QCOMPARE(bible->noteText(), QString("Kept note"));
    }
private:
    bool studyOpen() const { return object("relatedDrawer")->property("opened").toBool(); }
    QObject *panel() const {
        for (auto *obj : window->findChildren<QObject *>()) if (obj->property("studyTabs").isValid()) return obj;
        return nullptr;
    }
    QString tab() const { return panel()->property("studyTab").toString(); }
    int studyIndex() const { return object("studyList")->property("currentIndex").toInt(); }
    QVariantList rows() const { return value(panel()->property("studyRows")).toList(); }
    void openStudy() {
        call("readerFocus"); key(Qt::Key_S, Qt::ShiftModifier);
        QTRY_VERIFY(studyOpen());
    }
private slots:
    void studyTabsAndKeys() {
        navigate("John 1:1");
        openStudy();
        QCOMPARE(object("tagsTitle")->property("text").toString(), QString("Study"));
        QCOMPARE(tab(), QString("related"));
        for (int i = 0; i < 3; ++i) QVERIFY(item("studyTab" + QString::number(i))->isVisible());
        // → and l step forward, ← and h back, wrapping like Annotations.
        key(Qt::Key_Right); QCOMPARE(tab(), QString("crossrefs"));
        QTRY_VERIFY(item("studyList")->hasActiveFocus());
        key(Qt::Key_L); QCOMPARE(tab(), QString("footnotes"));
        key(Qt::Key_Right); QCOMPARE(tab(), QString("related"));
        key(Qt::Key_Left); QCOMPARE(tab(), QString("footnotes"));
        key(Qt::Key_H); QCOMPARE(tab(), QString("crossrefs"));
        // Cross references: OpenBible votes, merged with the source's \\x targets.
        QStringList references;
        for (const auto &row : rows()) references << row.toMap()["reference"].toString();
        QCOMPARE(references, QStringList({"Genesis 1:1", "John 1:14", "John 1:2", "Matthew 1:23", "John 1:18", "Matthew 1:2\u20133"}));
        QVERIFY(rows()[0].toMap()["source"].toBool()); QVERIFY(rows()[5].toMap()["source"].toBool());
        QVERIFY(!item("tagSearch")->isVisible()); QVERIFY(!item("tagResults")->isVisible());
        // Shared list keys and motions.
        QCOMPARE(studyIndex(), 0);
        key(Qt::Key_J); QCOMPARE(studyIndex(), 1);
        key(Qt::Key_Down); QCOMPARE(studyIndex(), 2);
        key(Qt::Key_K); QCOMPARE(studyIndex(), 1);
        key(Qt::Key_G, Qt::ShiftModifier); QCOMPARE(studyIndex(), 5);
        key(Qt::Key_G); QCOMPARE(studyIndex(), 0);
        key(Qt::Key_D, Qt::ControlModifier); QCOMPARE(studyIndex(), 5);
        key(Qt::Key_U, Qt::ControlModifier); QCOMPARE(studyIndex(), 0);
        key(Qt::Key_J);
        QApplication::clipboard()->clear();
        key(Qt::Key_Y);
        QTRY_VERIFY(QApplication::clipboard()->text().contains("John 1:14"));
        const int desk = bible->desk().size();
        key(Qt::Key_D); QTRY_COMPARE(bible->desk().size(), desk + 1);
        QCOMPARE(bible->desk().last().toMap()["reference"].toString(), QString("John 1:14"));
        // Enter opens the row as a History jump and keeps Study open on its pinned source.
        const int jumps = prop("jumpStack").toList().size();
        key(Qt::Key_Return);
        QTRY_COMPARE(bible->selectionReference(), QString("John 1:14"));
        QVERIFY(prop("jumpStack").toList().size() > jumps);
        QVERIFY(studyOpen()); QCOMPARE(tab(), QString("crossrefs")); QCOMPARE(rows().size(), 6);
        // Footnotes: the chapter's notes; the cursor verse's notes are marked.
        key(Qt::Key_Escape); QTRY_VERIFY(!object("relatedDrawer")->property("visible").toBool());
        navigate("John 3:16");
        openStudy();
        QCOMPARE(tab(), QString("crossrefs")); // The last tab is remembered.
        key(Qt::Key_Right); QCOMPARE(tab(), QString("footnotes"));
        QCOMPARE(rows().size(), 3);
        QCOMPARE(studyIndex(), 1); // First note of the cursor verse.
        QCOMPARE(rows()[1].toMap()["text"].toString(), QString("Greek agapao"));
        QTRY_VERIFY(item("studyRow:1")->property("atCursor").toBool());
        QVERIFY(item("studyRow:2")->property("atCursor").toBool());
        QVERIFY(!item("studyRow:0")->property("atCursor").toBool());
        key(Qt::Key_K); QCOMPARE(studyIndex(), 0);
        key(Qt::Key_Return);
        QTRY_COMPARE(bible->selectionReference(), QString("John 3:3"));
        QTRY_VERIFY(item("studyRow:0")->property("atCursor").toBool());
        QVERIFY(studyOpen());
        // Related keeps its tag search; / is inert on the other tabs.
        key(Qt::Key_Slash); QVERIFY(!item("tagSearch")->hasActiveFocus());
        key(Qt::Key_Right); QCOMPARE(tab(), QString("related"));
        QVERIFY(item("tagSearch")->isVisible());
        key(Qt::Key_Escape); QTRY_VERIFY(!object("relatedDrawer")->property("visible").toBool());
        // shift L no longer opens the panel.
        call("readerFocus"); key(Qt::Key_L, Qt::ShiftModifier); QTest::qWait(100); QVERIFY(!studyOpen());
    }
    void footnoteMarkersOpenStudyInEveryLayout() {
        for (const QString mode : {"verse", "paragraph", "block"}) {
            setMode(mode); navigate("John 3:16"); bible->clearSelection();
            QTest::qWait(50);
            const bool verseRows = mode == "verse";
            const QString first = verseRows ? "verseFootnoteMark:16.1" : "blockInlineMarker:footnote:footnote:16.1";
            const QString second = verseRows ? "verseFootnoteMark:16.2" : "blockInlineMarker:footnote:footnote:16.2";
            QTRY_VERIFY2(item(first) && item(second), qPrintable(mode));
            QVERIFY(item(first)->isVisible());
            QVERIFY(item(verseRows ? "verseFootnoteMark:3.1" : "blockInlineMarker:footnote:footnote:3.1"));
            auto body = renderedBody(16);
            const QString text = document(body)->toPlainText();
            // Each marker sits right after its anchor word, before the next.
            const int loved = text.indexOf("loved"), world = text.indexOf("the world");
            QVERIFY(loved > 0 && world > loved);
            const auto marker = item(first)->mapRectToItem(qobject_cast<QQuickItem *>(body), item(first)->boundingRect());
            const QRectF afterLoved = caret(body, loved + 5), beforeThe = caret(body, world);
            if (qAbs(afterLoved.y() - beforeThe.y()) < 1) {
                QVERIFY2(marker.left() >= afterLoved.x() - 1 && marker.right() <= beforeThe.x() + 1,
                         qPrintable(QString("%1: %2..%3 not within %4..%5").arg(mode).arg(marker.left()).arg(marker.right()).arg(afterLoved.x()).arg(beforeThe.x())));
            }
            // Cells never reach copied text, and search offsets still map.
            if (!verseRows) {
                const auto mapping = blockMapping(); QVERIFY(mapping);
                int start = -1, end = -1;
                for (const auto &value : mapping->spans()) {
                    const auto span = value.toMap();
                    if (span.value("number").toInt() == 16) { start = span.value("textStart").toInt(); end = span.value("textEnd").toInt(); }
                }
                QVERIFY(start >= 0);
                const QString copied = mapping->selectionText(start, end);
                QCOMPARE(copied, bible->verses()[15].toMap()["text"].toString());
                const int at = copied.indexOf("world");
                QVERIFY(!mapping->searchRectangles({QVariantMap{{"verse",16},{"start",at},{"length",5}}}).isEmpty());
            } else {
                QString plain;
                QMetaObject::invokeMethod(body, "getText", Q_RETURN_ARG(QString, plain), Q_ARG(int, 0), Q_ARG(int, body->property("length").toInt()));
                QVERIFY(plain.remove(QChar(0xfffc)).remove(QChar(0x2060)).contains("For God so loved the world that he gave his Son."));
            }
            // Clicking a marker selects its verse and opens Study → Footnotes there.
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, item(second)->mapToScene(QPointF(item(second)->width() / 2, item(second)->height() / 2)).toPoint());
            QTRY_VERIFY2(studyOpen(), qPrintable(mode));
            QCOMPARE(bible->selectionReference(), QString("John 3:16"));
            QCOMPARE(tab(), QString("footnotes"));
            QCOMPARE(studyIndex(), 2);
            QCOMPARE(rows()[2].toMap()["text"].toString(), QString("Or only begotten"));
            key(Qt::Key_Escape); QTRY_VERIFY(!object("relatedDrawer")->property("visible").toBool());
            // Focus mode hides the markers.
            bible->setFocusMode(true);
            QTRY_VERIFY2(!item(first) || !item(first)->isVisible(), qPrintable(mode));
            bible->setFocusMode(false);
            QTRY_VERIFY(item(first) && item(first)->isVisible());
            panel()->setProperty("studyTab", "related");
        }
    }
};
int main(int argc, char **argv) {
    QQuickStyle::setStyle("Basic"); QApplication app(argc, argv);
    StudyUiTest test; return QTest::qExec(&test, argc, argv);
}
#include "tst_studyui.moc"
