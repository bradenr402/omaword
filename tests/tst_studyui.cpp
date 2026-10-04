#include <QtTest/QTest>
#include <QApplication>
#include <QClipboard>
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
    std::unique_ptr<VersionCatalog> catalog;
    std::unique_ptr<QQmlApplicationEngine> engine;
    QQuickWindow *window = nullptr;
    QStringList warnings;
    QString source;
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
    }
    void init() {
        session = std::make_unique<QTemporaryDir>(); qputenv("XDG_DATA_HOME", session->path().toUtf8());
        bible = std::make_unique<BibleBackend>(); install(source); bible->setReadingMode("verse");
        desktop = std::make_unique<Desktop>(); sounds = std::make_unique<SoundEffects>(nullptr, session->filePath("sound.ini"), false); catalog = std::make_unique<VersionCatalog>();
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
};
int main(int argc, char **argv) {
    QQuickStyle::setStyle("Basic"); QApplication app(argc, argv);
    StudyUiTest test; return QTest::qExec(&test, argc, argv);
}
#include "tst_studyui.moc"
