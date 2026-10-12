#include "testenvironment.h"
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
#include <QRegularExpression>
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

// Study features: words of Jesus, Re-import offers, the See also panel, and
// reader footnote markers, against a small original fixture with \wj, \f,
// \x, and \r markup.
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
    QString source, other, third;
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
        auto book = [](const char *code, const char *name, int chapter, int verses, auto special, const char *parallels) {
            QByteArray data = QString("\\id %1 Original study fixture\n\\h %2\n\\toc1 %2\n\\toc2 %2\n").arg(code, name).toUtf8();
            for (int c = 1; c <= chapter; ++c) {
                data += QString("\\c %1\n\\s Study fixture heading\n").arg(c).toUtf8();
                // The first chapter's section lists its parallel passages.
                if (c == 1 && parallels) data += QByteArray("\\r ") + parallels + "\n";
                data += "\\p\n";
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
                return v == 1 ? QString("In the beginning of the original fixture.\\f + \\fr 1:1 \\ft A Genesis note\\f*") : QString(); }, nullptr)},
            {"40-MATstudy.usfm", book("MAT", "Matthew", 1, 25, [](int, int) { return QString(); }, nullptr)},
            {"43-JHNstudy.usfm", book("JHN", "John", 3, 36, [](int c, int v) {
                if (c == 1 && v == 1) return QString("In the beginning was the Word.\\x - \\xo 1:1 \\xt Gen 1:1; Jn 1:14, 2; Mt 1:23; Jn 1:18; Mt 1:2-3\\x* Original words follow.");
                if (c != 3) return QString();
                if (v == 3) return QString("Jesus answered,\\f + \\fr 3:3 \\ft Or \\fq from above\\f* \\wj Truly I tell you, unless one is born again\\wj* he cannot see.");
                if (v == 16) return QString("For God so loved\\f a \\ft Greek \\fq agapao\\f* the world that he gave\\f b \\ft Or only begotten\\f* his Son.");
                return QString(); }, "(Genesis 1:3; Matthew 1:5\u20136)")},
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
    // The top left of a verse's text in the Verse layout, in the window.
    QPointF verseScenePoint(int verse) const {
        auto body = qobject_cast<QQuickItem *>(object("verseBody" + QString::number(verse)));
        return body ? body->mapToScene(QPointF(0, 0)) : QPointF();
    }

private slots:
    void initTestCase() {
        source = fixtures.filePath("study_usfm.zip"); QVERIFY(edition(source));
        // Other editions of John 1:1 (different internal IDs): one for the
        // wrong-file check, and a third version for Open beside's choice.
        const auto oneVerse = [](const QString &path, const char *file, const QByteArray &data) {
            auto zip = archive_write_new(); archive_write_set_format_zip(zip);
            QVERIFY(archive_write_open_filename(zip, QFile::encodeName(path).constData()) == ARCHIVE_OK);
            auto entry = archive_entry_new(); archive_entry_set_pathname(entry, file); archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_perm(entry, 0600); archive_entry_set_size(entry, data.size());
            QVERIFY(archive_write_header(zip, entry) == ARCHIVE_OK && archive_write_data(zip, data.constData(), data.size()) == data.size());
            archive_entry_free(entry); QVERIFY(archive_write_close(zip) == ARCHIVE_OK); archive_write_free(zip);
        };
        other = fixtures.filePath("other_usfm.zip");
        oneVerse(other, "43-JHNother.usfm", "\\id JHN Another fixture\n\\c 1\n\\p\n\\v 1 Another edition.\n");
        third = fixtures.filePath("third_usfm.zip");
        oneVerse(third, "43-JHNthird.usfm", "\\id JHN Third fixture\n\\c 1\n\\p\n\\v 1 A third edition.\n");
    }
    void init() {
        // A position past a verse's text (a stale footnote marker) is a bug, not noise.
        QTest::failOnWarning(QRegularExpression("QTextCursor::setPosition"));
        session = std::make_unique<QTemporaryDir>(); qputenv("XDG_DATA_HOME", session->path().toUtf8());
        bible = std::make_unique<BibleBackend>(); install(source); bible->setReadingMode("verse");
        if (QByteArray(QTest::currentTestFunction()).startsWith("reimport") || QByteArray(QTest::currentTestFunction()) == "wordsOfJesusUnsupported") {
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
        key(Qt::Key_Comma, Qt::ControlModifier); QTRY_VERIFY(object("settingsDialog")->property("opened").toBool());
        QVERIFY(item("settingwordsOfJesus") && item("settingwordsOfJesus")->isVisible());
        QVERIFY(!item("settingwordsOfJesusNote")->isVisible());
    }
    void reimportFromCatalog() {
        catalog->entries = {QVariantMap{{"id","study"},{"name","Study fixture"},{"abbreviation","SF"},{"url","https://example.invalid/study.zip"},
            {"detailsUrl","https://example.invalid"},{"copyright",""},{"description","A fixture edition."},{"year","2026"},
            {"license","Public domain"},{"size",1500000},{"features",QVariantList{}}}};
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
        QCOMPARE(bible->textSchema(), 6);
        QTRY_COMPARE(object("toast")->property("text").toString(), "Reimported " + bible->versionAbbreviation());
        for (const auto &row : bible->versions()) QVERIFY(!row.toMap()["needsReimport"].toBool());
        QVERIFY(bible->navigate("John 3:16")); bible->selectVerses(16, 16); QCOMPARE(bible->noteText(), QString("Kept note"));
        key(Qt::Key_V, Qt::ControlModifier); QTRY_VERIFY(object("versionDialog")->property("opened").toBool());
        QTRY_VERIFY(!item("versionReimport:study") || !item("versionReimport:study")->isVisible());
    }
    // A version without words-of-Jesus marks: the switch is last, disabled,
    // and off whatever is stored; alt j explains instead of toggling.
    void wordsOfJesusUnsupported() {
        QVERIFY(!bible->hasWordsOfJesus());
        bible->setWordsOfJesus(true); QVERIFY(bible->wordsOfJesus());
        const int plays = sounds->playCount();
        key(Qt::Key_J, Qt::AltModifier);
        QTRY_COMPARE(object("toast")->property("text").toString(), "Words of Jesus are not marked in " + bible->versionAbbreviation());
        QCOMPARE(sounds->lastSound(), QString("error")); QVERIFY(sounds->playCount() > plays);
        QVERIFY(bible->wordsOfJesus());
        bible->setWordsOfJesus(false);
        key(Qt::Key_J, Qt::AltModifier); QVERIFY(!bible->wordsOfJesus());
        bible->setWordsOfJesus(true);
        key(Qt::Key_Comma, Qt::ControlModifier); QTRY_VERIFY(object("settingsDialog")->property("opened").toBool());
        auto toggle = item("settingwordsOfJesus"); QVERIFY(toggle && toggle->isVisible());
        QVERIFY(!toggle->isEnabled()); QVERIFY(!toggle->property("checked").toBool());
        // Below Margin notes; the note why sits inside the toggle, right
        // under its title, which keeps the other titles' color.
        auto margin = item("settingmarginNotes"), title = item("settingwordsOfJesusLabel"), hint = item("settingwordsOfJesusNote");
        QVERIFY(hint->isVisible());
        // (This version predates schema 4, which could mark them: a re-import tells.)
        QCOMPARE(hint->property("text").toString(), QString("Words of Jesus are not marked in this version (re-import it in Versions to check)."));
        QVERIFY(toggle->isAncestorOf(hint));
        const auto content = toggle->parentItem();
        QVERIFY(toggle->mapToItem(content, QPointF()).y() > margin->mapToItem(content, QPointF()).y());
        const double gap = hint->mapToItem(toggle, QPointF()).y() - title->mapToItem(toggle, QPointF(0, title->height())).y();
        QVERIFY2(gap >= 0 && gap < 6, qPrintable(QString::number(gap)));
        QCOMPARE(hint->mapToItem(toggle, QPointF()).x(), title->mapToItem(toggle, QPointF()).x());
        QCOMPARE(title->property("color"), item("settingmarginNotesLabel")->property("color"));
        QCOMPARE(hint->property("color").value<QColor>(), qvariant_cast<QObject *>(engine->rootContext()->contextProperty("desktop"))->property("muted").value<QColor>());
        // Its switch is drawn muted: no fill, a faint outline and thumb, unlike an off switch.
        auto track = item("settingwordsOfJesusTrack"), offTrack = item("settingmarginNotesTrack");
        QCOMPARE(track->property("color").value<QColor>().alpha(), 0);
        QVERIFY(track->property("border").value<QObject *>()->property("color").value<QColor>() != offTrack->property("border").value<QObject *>()->property("color").value<QColor>());
        QVERIFY(item("settingwordsOfJesusThumb")->property("color").value<QColor>().alphaF() < 0.5);
        for (auto child : content->childItems()) {
            if (child == toggle || !child->isVisible() || child->metaObject()->indexOfProperty("catalogAction") < 0) continue;
            QVERIFY2(child->mapToItem(content, QPointF()).y() < toggle->mapToItem(content, QPointF()).y(), qPrintable(child->objectName()));
        }
        // Tab never lands on it; a click does nothing.
        for (int i = 0; i < 12; ++i) { key(Qt::Key_Tab); QVERIFY(!toggle->hasActiveFocus()); }
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, toggle->mapToScene(QPointF(toggle->width() - 20, toggle->height() / 2)).toPoint());
        QCoreApplication::processEvents();
        QVERIFY(bible->wordsOfJesus()); QVERIFY(!toggle->property("checked").toBool());
    }
    void reimportLocalFilePicker() {
        key(Qt::Key_Comma, Qt::ControlModifier); QTRY_VERIFY(object("settingsDialog")->property("opened").toBool());
        auto button = item("settingsReimport:study"); QVERIFY(button); QVERIFY(button->isVisible());
        QVERIFY(item("settingwordsOfJesusNote")->isVisible());
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
        QCOMPARE(bible->textSchema(), 6); QVERIFY(bible->hasWordsOfJesus());
        QVERIFY(bible->navigate("John 3:16")); bible->selectVerses(16, 16); QCOMPARE(bible->noteText(), QString("Kept note"));
    }
private:
    bool studyOpen() const { return window->property("relatedOpen").toBool(); }
    // See also in the Margin: esc closes it, the reader taking the keyboard.
    void closeStudy() {
        key(Qt::Key_Escape); QTRY_VERIFY(!object("notesDrawer")->property("visible").toBool());
        QTRY_VERIFY(window->activeFocusItem() == item("readerSurface")); QVERIFY(!studyOpen());
        // The text has glided back to its place: what is clicked next is where it was found.
        QTRY_COMPARE(window->property("chromeDock").toDouble(), 0.0);
        QTRY_VERIFY(!object("marginSlide")->property("running").toBool());
    }
    // An open peek lies inside the window, its hints across the card's whole
    // width, each hint on one line at its own width (never squeezed).
    void verifyPeekFits() {
        auto card = object("peekCard"); QVERIFY(card);
        QTRY_VERIFY(card->property("opened").toBool());
        QCoreApplication::processEvents();
        const double x = card->property("x").toDouble(), y = card->property("y").toDouble();
        const double width = card->property("width").toDouble(), height = card->property("height").toDouble();
        QVERIFY2(y >= 0 && y + height <= window->height(), qPrintable(QString("card %1 + %2 in %3").arg(y).arg(height).arg(window->height())));
        QVERIFY(x >= 0 && x + width <= window->width());
        auto hints = item("peekHints"); QVERIFY(hints);
        QVERIFY2(hints->width() >= width - 2.5, qPrintable(QString("hints %1 wide in a card %2 wide").arg(hints->width()).arg(width)));
        int parts = 0;
        for (auto part : hints->childItems()) {
            if (!part->objectName().startsWith("peekHintsPart:")) continue;
            ++parts;
            QVERIFY2(qAbs(part->width() - part->implicitWidth()) < 0.5, qPrintable(part->objectName()));
            QVERIFY(part->y() + part->height() <= hints->height() + 0.5);
            for (auto label : part->childItems())
                if (label->metaObject()->indexOfProperty("lineCount") >= 0) QCOMPARE(label->property("lineCount").toInt(), 1);
        }
        QVERIFY(parts >= 3);
    }
    QObject *panel() const {
        for (auto *obj : window->findChildren<QObject *>()) if (obj->property("studyRows").isValid()) return obj;
        return nullptr;
    }
    int studyIndex() const { return object("studyList")->property("currentIndex").toInt(); }
    QVariantList rows() const { return value(panel()->property("studyRows")).toList(); }
    void openStudy() {
        call("readerFocus"); key(Qt::Key_S, Qt::ShiftModifier);
        QTRY_VERIFY(studyOpen());
    }
private slots:
    void compareAcrossVersions() {
        // A second edition with only John 1:1: John 3:16 is not in it.
        install(other); QCOMPARE(bible->versions().size(), 2);
        const QString otherId = bible->versionId(), studyId = bible->versions()[1].toMap()["id"].toString();
        // c: a card under the selection with each version's text, the one read first.
        navigate("John 1:1"); const auto anchor = verseScenePoint(1); key(Qt::Key_C);
        auto card = object("peekCard");
        QTRY_VERIFY(card->property("opened").toBool());
        // Its rows are made as the list lays out, after it opens.
        QTRY_VERIFY(object("peekRowText:1"));
        QCOMPARE(object("peekTitle")->property("text").toString(), QString("Compare · John 1:1 · 2 versions"));
        QCOMPARE(object("peekRowTitle:0")->property("text").toString(), bible->versionAbbreviation() + " · reading");
        QCOMPARE(object("peekRowText:0")->property("text").toString(), QString("Another edition."));
        QVERIFY(object("peekRowText:1")->property("text").toString().startsWith("In the beginning was the Word."));
        QVERIFY(card->property("y").toDouble() > anchor.y()); // Under the passage, not over it.
        verifyPeekFits();
        // It starts on the other version; k and j step, never past the ends.
        QCOMPARE(card->property("currentRow").toInt(), 1);
        key(Qt::Key_K); QCOMPARE(card->property("currentRow").toInt(), 0);
        key(Qt::Key_K); QCOMPARE(card->property("currentRow").toInt(), 0);
        key(Qt::Key_J); QCOMPARE(card->property("currentRow").toInt(), 1);
        QVERIFY(object("peekHints")->property("plainText").toString().contains("enter switch"));
        QVERIFY(object("peekHints")->property("plainText").toString().contains("o beside"));
        // o opens the highlighted version beside (WindowManager takes the request); not the one read here.
        {
            QSignalSpy beside(window, SIGNAL(besideRequested(QString)));
            key(Qt::Key_K); key(Qt::Key_O); QVERIFY(card->property("opened").toBool()); QCOMPARE(beside.count(), 0);
            key(Qt::Key_J); key(Qt::Key_O); QTRY_VERIFY(!card->property("visible").toBool());
            QCOMPARE(beside.count(), 1); QCOMPARE(beside.first().first().toString(), studyId);
            key(Qt::Key_C); QTRY_VERIFY(card->property("opened").toBool());
        }
        // y copies its quotation and closes; shift Y the plain text.
        QGuiApplication::clipboard()->clear();
        key(Qt::Key_Y); QTRY_VERIFY(!card->property("visible").toBool());
        QVERIFY(QGuiApplication::clipboard()->text().startsWith(QString::fromUtf8("“")));
        QVERIFY(QGuiApplication::clipboard()->text().contains("In the beginning was the Word."));
        QVERIFY(QGuiApplication::clipboard()->text().endsWith(QString::fromUtf8("— John 1:1")));
        key(Qt::Key_C); QTRY_VERIFY(card->property("opened").toBool());
        key(Qt::Key_Y, Qt::ShiftModifier); QTRY_VERIFY(!card->property("visible").toBool());
        QVERIFY(QGuiApplication::clipboard()->text().startsWith("In the beginning was the Word."));
        // A click chooses a row; enter switches to it at the same passage.
        key(Qt::Key_C); QTRY_VERIFY(card->property("opened").toBool());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, item("peekRow:0")->mapToScene(QPointF(20, 8)).toPoint());
        QCOMPARE(card->property("currentRow").toInt(), 0);
        key(Qt::Key_Return); QVERIFY(card->property("opened").toBool()); // Already reading it.
        key(Qt::Key_J); key(Qt::Key_Return); QTRY_VERIFY(!card->property("visible").toBool());
        QCOMPARE(bible->versionId(), studyId); QCOMPARE(bible->selectionReference(), QString("John 1:1"));
        // Versions (ctrl v) shows the same text beside each version.
        key(Qt::Key_V, Qt::ControlModifier);
        QTRY_VERIFY(object("versionDialog")->property("opened").toBool());
        QCOMPARE(object("versionTitle")->property("text").toString(), QString("Versions · John 1:1"));
        QCOMPARE(object("versionText:" + otherId)->property("text").toString(), QString("Another edition."));
        key(Qt::Key_Escape); QTRY_VERIFY(!object("versionDialog")->property("visible").toBool());
        // Where a version lacks the passage, it says so; ctrl v compares too.
        navigate("John 3:16"); key(Qt::Key_V, Qt::ControlModifier);
        QTRY_VERIFY(object("versionDialog")->property("opened").toBool());
        QCOMPARE(object("versionText:" + otherId)->property("text").toString(), QString("Not in this version"));
        QCOMPARE(object("versionText:" + otherId)->property("color").value<QColor>(), desktop->property("muted").value<QColor>());
        key(Qt::Key_Escape); QTRY_VERIFY(!object("versionDialog")->property("visible").toBool());
        // In the card, a version without the passage says so and copies nothing.
        call("readerFocus"); key(Qt::Key_C); QTRY_VERIFY(card->property("opened").toBool());
        QTRY_VERIFY(object("peekRowText:1"));
        int missing = -1;
        for (int i = 0; i < 2; ++i) if (object("peekRowText:" + QString::number(i))->property("text").toString() == "Not in this version") missing = i;
        QVERIFY(missing >= 0);
        card->setProperty("currentRow", missing); window->setProperty("compareRow", missing);
        key(Qt::Key_Y); QVERIFY(card->property("opened").toBool()); QVERIFY(object("toast")->property("text").toString().startsWith("Not in "));
        key(Qt::Key_Escape); QTRY_VERIFY(!card->property("visible").toBool());
        // A selection below the view: the card opens over the bottom of the
        // passage instead, all of it in the window once its rows are laid out.
        bible->selectVerses(36, 36); call("readerFocus"); key(Qt::Key_C);
        verifyPeekFits();
        QVERIFY(card->property("y").toDouble() + card->property("height").toDouble() < window->height() - 24);
        key(Qt::Key_Escape); QTRY_VERIFY(!card->property("visible").toBool());
        // Hints squeezed to nothing once (as a reader's screenshot showed them,
        // a column of single letters) take the card's width again.
        call("readerFocus"); key(Qt::Key_C); QTRY_VERIFY(card->property("opened").toBool());
        item("peekHints")->setWidth(1);
        QTRY_VERIFY(item("peekHints")->width() > 400);
        verifyPeekFits();
        key(Qt::Key_Escape); QTRY_VERIFY(!card->property("visible").toBool());
        // Without a selection there is nothing to compare: c says why.
        bible->clearSelection(); call("readerFocus"); key(Qt::Key_C);
        QVERIFY(!card->property("visible").toBool());
        QCOMPARE(object("toast")->property("text").toString(), QString("Select a verse first"));
    }
    // Open beside: shift C (Commands, the version button's menu) opens Versions
    // to choose a version for a window beside this one; enter opens it there.
    // In Versions itself, ctrl enter opens the highlighted one beside.
    void openBesideFromVersions() {
        // With one version there is nothing to read beside: shift C says so.
        call("readerFocus"); key(Qt::Key_C, Qt::ShiftModifier);
        QVERIFY(!object("besideDialog")->property("visible").toBool());
        QCOMPARE(object("toast")->property("text").toString(), QString("Add another version to read beside"));
        install(other);
        const QString here = bible->versionAbbreviation(), hereId = bible->versionId();
        const auto otherVersion = [&] {
            for (const auto &row : bible->versions()) if (!row.toMap()["active"].toBool()) return row.toMap();
            return QVariantMap();
        };
        const QString besideId = otherVersion()["id"].toString();
        QSignalSpy beside(window, SIGNAL(besideRequested(QString)));
        // With two, shift C opens the other at once: nothing to choose.
        navigate("John 1:1"); key(Qt::Key_C, Qt::ShiftModifier);
        QCOMPARE(beside.count(), 1); QCOMPARE(beside.last().first().toString(), besideId);
        QVERIFY(!object("besideDialog")->property("visible").toBool()); QVERIFY(!object("versionDialog")->property("visible").toBool());
        // Versions (ctrl v): enter switches, ctrl enter opens beside.
        auto dialog = object("versionDialog"); auto list = object("versionResults");
        key(Qt::Key_V, Qt::ControlModifier); QTRY_VERIFY(dialog->property("opened").toBool());
        QCOMPARE(object("versionTitle")->property("text").toString(), QString("Versions · John 1:1"));
        const auto rows = value(list->property("model")).toList();
        for (int i = 0; i < rows.size(); ++i) if (!rows[i].toMap()["active"].toBool()) list->setProperty("currentIndex", i);
        QVERIFY(object("versionHints")->property("plainText").toString().contains("ctrl enter beside"));
        key(Qt::Key_Return, Qt::ControlModifier); QTRY_VERIFY(!dialog->property("visible").toBool());
        QCOMPARE(beside.count(), 2); QCOMPARE(bible->versionAbbreviation(), here);
        // The version button's menu: switch, the other version beside (by
        // name, at once), and stop following while following.
        call("readerFocus");
        auto button = item("versionControl"); auto menu = object("contextMenu");
        const auto rightClick = [&] {
            QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint());
            QTRY_VERIFY(menu->property("visible").toBool());
        };
        const auto entries = [&] { return value(menu->property("layout")).toList(); };
        const auto ids = [&] {
            QStringList list;
            for (const auto &entry : entries()) list << entry.toMap()["id"].toString();
            return list;
        };
        rightClick(); QCOMPARE(ids(), QStringList({"switch", "beside"}));
        QCOMPARE(entries()[1].toMap()["text"].toString(), "Open " + otherVersion()["abbreviation"].toString() + " beside");
        QVERIFY(QMetaObject::invokeMethod(menu, "triggerId", Q_ARG(QVariant, QString("beside"))));
        QTRY_COMPARE(beside.count(), 3); QVERIFY(!object("besideDialog")->property("visible").toBool());
        // With more, shift C asks which in a card of its own: the versions
        // not read here, with the selection's text in each.
        install(third); navigate("John 1:1");
        const QString thirdHere = bible->versionId();
        key(Qt::Key_C, Qt::ShiftModifier);
        auto card = object("besideDialog"); auto choices = object("besideResults");
        QTRY_VERIFY(card->property("opened").toBool()); QVERIFY(!dialog->property("visible").toBool());
        QCOMPARE(object("besideTitle")->property("text").toString(), QString("Open beside · John 1:1"));
        QTRY_VERIFY(item("besideSearch")->hasActiveFocus());
        QCOMPARE(object("besideHints")->property("plainText").toString(), QString("↑ ↓ choose    enter open beside    esc cancel"));
        auto shown = value(choices->property("model")).toList();
        QCOMPARE(shown.size(), 2);
        for (const auto &row : shown) QVERIFY(row.toMap()["id"].toString() != thirdHere);
        QCOMPARE(object("besideText:" + hereId)->property("text").toString(), QString("Another edition."));
        // Typing filters; arrows choose; enter opens the chosen one beside.
        for (const QChar c : QString("another")) QTest::keyClick(window, c.toLatin1());
        QCoreApplication::processEvents();
        QCOMPARE(item("besideSearch")->property("text").toString(), QString("another"));
        const double cardHeight = card->property("height").toDouble();
        QTRY_COMPARE(value(choices->property("model")).toList().size(), 1);
        // The card keeps its height (and the field its place) as rows filter away.
        QTest::qWait(50); QCOMPARE(card->property("height").toDouble(), cardHeight);
        key(Qt::Key_Down); QCOMPARE(choices->property("currentIndex").toInt(), 0);
        key(Qt::Key_Return); QTRY_VERIFY(!card->property("visible").toBool());
        QCOMPARE(beside.count(), 4); QCOMPARE(beside.last().first().toString(), hereId);
        // The menu's entry asks too; a click on a row opens it; esc cancels.
        call("readerFocus");
        rightClick(); QCOMPARE(entries()[1].toMap()["text"].toString(), QString("Open beside…"));
        QVERIFY(QMetaObject::invokeMethod(menu, "triggerId", Q_ARG(QVariant, QString("beside"))));
        QTRY_VERIFY(card->property("opened").toBool());
        QCOMPARE(value(choices->property("model")).toList().size(), 2);
        key(Qt::Key_Escape); QTRY_VERIFY(!card->property("visible").toBool()); QCOMPARE(beside.count(), 4);
        key(Qt::Key_C, Qt::ShiftModifier); QTRY_VERIFY(card->property("opened").toBool());
        QTRY_VERIFY(item("besideEntry1"));
        auto row = item("besideEntry1");
        const QString clicked = value(choices->property("model")).toList()[1].toMap()["id"].toString();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, row->mapToScene(QPointF(row->width() / 2, row->height() / 2)).toPoint());
        QTRY_VERIFY(!card->property("visible").toBool());
        QCOMPARE(beside.count(), 5); QCOMPARE(beside.last().first().toString(), clicked);
        window->setProperty("followPartner", "KJV"); call("readerFocus");
        rightClick(); QCOMPARE(ids(), QStringList({"switch", "beside", "unfollow"}));
        QCOMPARE(entries().last().toMap()["text"].toString(), QString("Stop following KJV"));
        QSignalSpy unfollow(window, SIGNAL(unfollowRequested()));
        QVERIFY(QMetaObject::invokeMethod(menu, "triggerId", Q_ARG(QVariant, QString("unfollow"))));
        QTRY_COMPARE(unfollow.count(), 1);
    }
    void studyOpensWithoutSelection() {
        // A freshly opened chapter has no selection; Study studies its first verse.
        for (int via = 0; via < 2; ++via) {
            navigate("John 3"); QCOMPARE(bible->selectedStart(), 0);
            if (via == 0) openStudy();
            else {
                auto control = object("relatedControl"); QVERIFY(control);
                QVERIFY(control->property("enabled").toBool());
                QVERIFY(QMetaObject::invokeMethod(control, "clicked"));
                QTRY_VERIFY(studyOpen());
            }
            QCOMPARE(bible->selectedStart(), 1); QCOMPARE(bible->selectionReference(), QString("John 3:1"));
            {   // A rule under the header, as in Marks.
                auto header = item("studyHeader"), rule = item("studyHeaderRule");
                QVERIFY(header && rule && rule->isVisible()); QCOMPARE(rule->height(), 1.0);
                const auto t = header->mapToScene(QPointF(0, header->height())), r = rule->mapToScene(QPointF());
                QVERIFY(r.y() >= t.y()); QVERIFY(r.y() - t.y() < 20); QCOMPARE(rule->width(), header->width());
            }
            closeStudy();
        }
    }
    void seeAlsoSectionsAndKeys() {
        navigate("John 1:1");
        openStudy();
        QCOMPARE(object("studyTitle")->property("text").toString(), QString("See also"));
        // One list in sections: the source's own \\x targets in source order,
        // then its parallel passages (Matthew 1:5–6 is also OpenBible's), then
        // OpenBible.info's (for John 1:1 this fixture has none left).
        QStringList references, sections;
        for (const auto &row : rows()) { references << row.toMap()["reference"].toString(); sections << row.toMap()["section"].toString(); }
        QCOMPARE(references, QStringList({"Genesis 1:1", "John 1:14", "John 1:2", "Matthew 1:23", "John 1:18", "Matthew 1:2\u20133",
            "Genesis 1:3", "Matthew 1:5\u20136"}));
        QCOMPARE(sections, QStringList({"own", "own", "own", "own", "own", "own", "parallel", "parallel"}));
        QTRY_VERIFY(item("studySection:own") && item("studySection:own")->isVisible());
        QCOMPARE(object("studySectionTitle:own")->property("text").toString(), "Cross-references in " + bible->versionAbbreviation());
        // The parallel section's heading may lie past the panel's end: rows are made as they come into view.
        QVERIFY(QMetaObject::invokeMethod(object("studyList"), "positionViewAtIndex", Q_ARG(int, 6), Q_ARG(int, 0)));
        QTRY_VERIFY(object("studySectionTitle:parallel"));
        QCOMPARE(object("studySectionTitle:parallel")->property("text").toString(), QString("Parallel passages"));
        QVERIFY(QMetaObject::invokeMethod(object("studyList"), "positionViewAtBeginning"));
        QVERIFY(!item("studySearch")->isVisible()); QVERIFY(!item("studyTabs"));
        // Shared list keys and motions, across sections (headings are never stops).
        QTRY_VERIFY(item("studyList")->hasActiveFocus());
        QCOMPARE(studyIndex(), 0);
        key(Qt::Key_J); QCOMPARE(studyIndex(), 1);
        key(Qt::Key_Down); QCOMPARE(studyIndex(), 2);
        key(Qt::Key_K); QCOMPARE(studyIndex(), 1);
        key(Qt::Key_G, Qt::ShiftModifier); QCOMPARE(studyIndex(), 7);
        key(Qt::Key_G); key(Qt::Key_G); QCOMPARE(studyIndex(), 0);
        key(Qt::Key_D, Qt::ControlModifier); QCOMPARE(studyIndex(), 5);
        key(Qt::Key_U, Qt::ControlModifier); QCOMPARE(studyIndex(), 0);
        // h and l step nothing: there are no tabs.
        key(Qt::Key_L); key(Qt::Key_H); QCOMPARE(studyIndex(), 0); QVERIFY(studyOpen());
        key(Qt::Key_J);
        QApplication::clipboard()->clear();
        key(Qt::Key_Y);
        QTRY_VERIFY(QApplication::clipboard()->text().contains("John 1:14"));
        // d quotes the row on the Desk.
        QVERIFY(bible->deskPage().isEmpty());
        key(Qt::Key_D); QTRY_VERIFY(bible->deskPage().contains(QStringLiteral("> \u2014 John 1:14 (")));
        // Enter opens the row as a History jump and keeps the panel open on its
        // passage (opening from inside never re-roots it while it has focus).
        const int jumps = prop("jumpStack").toList().size();
        key(Qt::Key_Return);
        QTRY_COMPARE(bible->selectionReference(), QString("John 1:14"));
        QVERIFY(prop("jumpStack").toList().size() > jumps);
        QVERIFY(studyOpen()); QCOMPARE(rows().size(), 8);
        closeStudy();
        // Nothing for a passage: it says so, and how tagging gathers passages.
        navigate("Genesis 1:2");
        openStudy();
        QVERIFY(rows().isEmpty());
        QCOMPARE(object("studyEmpty")->property("text").toString(), QString("Nothing goes with Genesis 1:2 yet. Tag it (t) to gather the passages you tag alike."));
        closeStudy();
        // No \\x of its own: the passages tagged alike, in every version; then,
        // last, OpenBible.info's, by votes, under their heading.
        navigate("John 1:14"); QVERIFY(!bible->createAndAssignTag(bible->tagScope(), "Word made flesh").isEmpty());
        navigate("John 3:16"); const QString love = bible->createAndAssignTag(bible->tagScope(), "Love");
        QVERIFY(!love.isEmpty()); QVERIFY(!bible->createAndAssignTag(bible->tagScope(), "Word made flesh").isEmpty());
        bible->selectVerses(3, 3); QVERIFY(bible->setTag(bible->tagScope(), love, true));
        navigate("John 3:16");
        openStudy();
        references.clear(); sections.clear();
        for (const auto &row : rows()) { references << row.toMap()["reference"].toString(); sections << row.toMap()["section"].toString(); }
        QCOMPARE(references, QStringList({"John 1:14", "John 3:3", "John 3:15", "John 3:36", "John 1:29", "John 1:14", "John 1:18"}));
        QCOMPARE(sections, QStringList({"tags", "tags", "openBible", "openBible", "openBible", "openBible", "openBible"}));
        QCOMPARE(object("studySectionTitle:openBible")->property("text").toString(), QString("OpenBible.info"));
        QTRY_VERIFY(object("studyRowTitle:2"));
        QCOMPARE(object("studyRowTitle:2")->property("text").toString(), QString("John 3:15"));
        QCOMPARE(object("studySectionTitle:tags")->property("text").toString(), QString("Tagged alike"));
        QVERIFY(object("studySectionTag:Love") && object("studySectionTag:Word made flesh"));
        // A row's tags, the shared one in the accent.
        const QString rowTags = object("studyRowTags:0")->property("text").toString();
        QVERIFY(rowTags.contains(desktop->property("accent").value<QColor>().name() + "\">#Word made flesh")); QVERIFY(!rowTags.contains("Love"));
        // Mouse: a heading's tag keeps only its passages; / filters by words too.
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
            item("studySectionTag:Love")->mapToScene(QPointF(item("studySectionTag:Love")->width() / 2, item("studySectionTag:Love")->height() / 2)).toPoint());
        QTRY_COMPARE(rows().size(), 1);
        QCOMPARE(rows()[0].toMap()["reference"].toString(), QString("John 3:3"));
        QVERIFY(item("studySearch")->isVisible()); QCOMPARE(item("studySearch")->property("text").toString(), QString("#Love"));
        QVERIFY(item("studyList")->hasActiveFocus());
        key(Qt::Key_Slash); QTRY_VERIFY(item("studySearch")->hasActiveFocus());
        key(Qt::Key_A, Qt::ControlModifier); for (const char c : QByteArray("1:2")) QTest::keyClick(window, c);
        QTRY_COMPARE(rows().size(), 1); QCOMPARE(rows()[0].toMap()["reference"].toString(), QString("John 1:29"));
        QTest::keyClick(window, 'x'); QTRY_COMPARE(rows().size(), 0);
        QCOMPARE(object("studyEmpty")->property("text").toString(), QString::fromUtf8("Nothing matches “1:2x”."));
        // esc leaves the filter, keeping it; esc again closes See also, and the next open is unfiltered.
        key(Qt::Key_Escape); QTRY_VERIFY(item("studyList")->hasActiveFocus()); QVERIFY(item("studySearch")->isVisible());
        closeStudy();
        openStudy(); QCOMPARE(rows().size(), 7); QVERIFY(!item("studySearch")->isVisible());
        // t edits the current row's tags over the panel.
        key(Qt::Key_G); key(Qt::Key_G); key(Qt::Key_J); key(Qt::Key_T);
        QTRY_VERIFY(object("tagsUi")->property("pickerVisible").toBool());
        QCOMPARE(value(object("tagsUi")->property("editingScope")).toMap()["start"].toInt(), 3);
        key(Qt::Key_Escape); QTRY_VERIFY(!object("tagsUi")->property("pickerVisible").toBool());
        closeStudy();
        // shift L no longer opens the panel.
        call("readerFocus"); key(Qt::Key_L, Qt::ShiftModifier); QTest::qWait(100); QVERIFY(!studyOpen());
    }
    void footnoteMarkersPeekInEveryLayout() {
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
            // Clicking a marker peeks at its note, in place, below the mark.
            const QPointF mark = item(second)->mapToScene(QPointF(item(second)->width() / 2, item(second)->height() / 2));
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, mark.toPoint());
            auto peek = object("peekCard");
            QTRY_VERIFY2(peek->property("opened").toBool(), qPrintable(mode));
            QCOMPARE(object("peekBody")->property("text").toString(), QString("Or only begotten"));
            QCOMPARE(object("peekTitle")->property("text").toString(), QString("Footnote b · John 3:16 · 3 of 3"));
            QVERIFY(peek->property("y").toDouble() > mark.y());
            verifyPeekFits();
            QCOMPARE(bible->selectedStart(), 0); // A peek moves nothing.
            QVERIFY(!studyOpen());
            // ↑ steps to the previous note; y copies it and closes.
            key(Qt::Key_Up); QCOMPARE(object("peekBody")->property("text").toString(), QString("Greek agapao"));
            key(Qt::Key_Y); QTRY_VERIFY(!peek->property("visible").toBool());
            QCOMPARE(QGuiApplication::clipboard()->text(), QString("Greek agapao"));
            // enter opens See also on the note's verse, selecting it.
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, mark.toPoint());
            QTRY_VERIFY(peek->property("opened").toBool());
            key(Qt::Key_Return);
            QTRY_VERIFY2(studyOpen(), qPrintable(mode));
            QCOMPARE(bible->selectionReference(), QString("John 3:16"));
            QCOMPARE(rows().first().toMap()["reference"].toString(), QString("John 3:15"));
            closeStudy();
            // i peeks at the cursor verse's first note; without one it says so.
            call("readerFocus"); key(Qt::Key_I);
            QTRY_VERIFY(peek->property("opened").toBool());
            QCOMPARE(object("peekBody")->property("text").toString(), QString("Greek agapao"));
            key(Qt::Key_Escape); QTRY_VERIFY(!peek->property("visible").toBool());
            bible->selectVerses(4, 4); call("readerFocus"); key(Qt::Key_I);
            QVERIFY(!peek->property("visible").toBool());
            QCOMPARE(object("toast")->property("text").toString(), QString("No footnotes on John 3:4"));
            // Focus mode hides the markers.
            bible->setFocusMode(true);
            QTRY_VERIFY2(!item(first) || !item(first)->isVisible(), qPrintable(mode));
            bible->setFocusMode(false);
            QTRY_VERIFY(item(first) && item(first)->isVisible());
        }
    }
};
int main(int argc, char **argv) {
    QQuickStyle::setStyle("Basic"); QApplication app(argc, argv);
    StudyUiTest test; return QTest::qExec(&test, argc, argv);
}
#include "tst_studyui.moc"
