#include "epubimporter.h"
#include <QtTest/QTest>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QXmlInputSource>
#include <QXmlSimpleReader>
#endif
#include <archive.h>
#include <archive_entry.h>
#include <functional>

namespace {
QString source(const char *variable) { return qEnvironmentVariable(variable); }
QByteArray bytes(const QString &path) {
    QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
QMap<QString, QByteArray> unzip(const QString &path) {
    QMap<QString, QByteArray> result;
    archive *a = archive_read_new(); archive_read_support_format_zip(a);
    if (archive_read_open_filename(a, QFile::encodeName(path).constData(), 65536) == ARCHIVE_OK) {
        archive_entry *entry;
        while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
            QByteArray data; char buffer[32768]; la_ssize_t n;
            while ((n = archive_read_data(a, buffer, sizeof(buffer))) > 0) data.append(buffer, n);
            result.insert(QString::fromUtf8(archive_entry_pathname(entry)), data);
        }
    }
    archive_read_free(a); return result;
}
bool zip(const QString &path, const QMap<QString, QByteArray> &entries) {
    archive *a = archive_write_new(); archive_write_set_format_zip(a);
    bool ok = archive_write_open_filename(a, QFile::encodeName(path).constData()) == ARCHIVE_OK;
    for (auto it = entries.cbegin(); ok && it != entries.cend(); ++it) {
        archive_entry *e = archive_entry_new();
        archive_entry_set_pathname(e, it.key().toUtf8().constData());
        archive_entry_set_filetype(e, AE_IFREG); archive_entry_set_perm(e, 0600);
        archive_entry_set_size(e, it.value().size());
        ok = archive_write_header(a, e) == ARCHIVE_OK
            && archive_write_data(a, it.value().constData(), it.value().size()) == it.value().size();
        archive_entry_free(e);
    }
    ok = archive_write_close(a) == ARCHIVE_OK && ok; archive_write_free(a); return ok;
}
struct Database {
    QString name = QUuid::createUuid().toString();
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", name);
    explicit Database(const QString &path) { db.setDatabaseName(path); db.open(); }
    ~Database() { db.close(); db = {}; QSqlDatabase::removeDatabase(name); }
    QVariant scalar(const QString &sql) {
        QSqlQuery q(db); return q.exec(sql) && q.next() ? q.value(0) : QVariant();
    }
};
}

class ImporterVersionsTest : public QObject
{
    Q_OBJECT
    QTemporaryDir root;
private slots:
    void editions_data() {
        QTest::addColumn<QString>("path"); QTest::addColumn<QString>("id");
        QTest::addColumn<QString>("abbr"); QTest::addColumn<QString>("name");
        QTest::addColumn<int>("count");
        QTest::newRow("NET") << source("OMABIBLE_TEST_EPUB") << "net-2.1" << "NET" << "NET Bible 2.1" << 31087;
        QTest::newRow("KJV") << source("OMABIBLE_TEST_KJV_EPUB") << "kjv-2006" << "KJV" << "King James (Authorized) Version" << 31102;
    }
    void editions() {
        QFETCH(QString,path); QFETCH(QString,id); QFETCH(QString,abbr); QFETCH(QString,name); QFETCH(int,count);
        if (path.isEmpty()) QSKIP("Set OMABIBLE_TEST_EPUB / OMABIBLE_TEST_KJV_EPUB to a local EPUB.");
        QVERIFY(root.isValid());
        const auto connections = QSqlDatabase::connectionNames();
        const auto before = QDir(root.path()).entryList(QDir::AllEntries | QDir::Hidden);
        QString error = "stale error";
        const auto edition = EpubImporter::inspectFile(path, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(edition.id,id); QCOMPARE(edition.abbreviation,abbr); QCOMPARE(edition.name,name);
        QVERIFY(!edition.copyright.isEmpty());
        QCOMPARE(QSqlDatabase::connectionNames(),connections);
        QCOMPARE(QDir(root.path()).entryList(QDir::AllEntries | QDir::Hidden),before);
        const QString target = root.filePath(id + ".sqlite");
        auto progress = std::make_shared<std::atomic<double>>(0);
        const auto imported = EpubImporter::importFile(path,target,progress);
        QVERIFY2(imported.ok,qPrintable(imported.error)); QCOMPARE(imported.verseCount,count);
        QCOMPARE(imported.edition.id,id); QCOMPARE(imported.edition.copyright,edition.copyright);
        QCOMPARE(progress->load(),1.0);
        QVERIFY2(EpubImporter::validateDatabase(target,&error),qPrintable(error));
        Database db(target); QVERIFY(db.db.isOpen());
        QCOMPARE(db.scalar("SELECT COUNT(*) FROM verses").toInt(),count);
        QCOMPARE(db.scalar("SELECT value FROM metadata WHERE key='schema'").toString(),QString("2"));
        QCOMPARE(db.scalar("SELECT value FROM metadata WHERE key='paragraph_layout'").toString(),QString("1"));
        for (int number : {1,6,9,14,20,24,26})
            QVERIFY2(db.scalar(QString("SELECT paragraphStart FROM verses WHERE book=1 AND chapter=1 AND number=%1").arg(number)).toBool(),qPrintable(QString::number(number)));
        for (int number : {4,5,7,8,10,12,13})
            QVERIFY(!db.scalar(QString("SELECT paragraphStart FROM verses WHERE book=1 AND chapter=1 AND number=%1").arg(number)).toBool());
        QCOMPARE(db.scalar("SELECT paragraphStart FROM verses WHERE book=1 AND chapter=1 AND number=2").toBool(),id == "net-2.1");
        QCOMPARE(db.scalar("SELECT paragraphStart FROM verses WHERE book=1 AND chapter=1 AND number=3").toBool(),id == "kjv-2006");
        for (int number : {27,28,31})
            QCOMPARE(db.scalar(QString("SELECT paragraphStart FROM verses WHERE book=1 AND chapter=1 AND number=%1").arg(number)).toBool(),id == "net-2.1");
        if (id == "kjv-2006") {
            // Independently inspected OEBPS/GEN.xhtml div.p groups. Starts 3
            // and 20 have no pilcrow: ¶ is not the only paragraph signal.
            QList<int> starts;
            QSqlQuery paragraphs(db.db);
            QVERIFY(paragraphs.exec("SELECT number FROM verses WHERE book=1 AND chapter=1 AND paragraphStart=1 ORDER BY number"));
            while (paragraphs.next()) starts.append(paragraphs.value(0).toInt());
            QCOMPARE(starts,QList<int>({1,3,6,9,14,20,24,26,29}));
            QCOMPARE(db.scalar("SELECT COUNT(*) FROM verses WHERE instr(text,'¶') OR instr(html,'¶')").toInt(),0);
        } else {
            QVERIFY(db.scalar("SELECT text FROM verses WHERE book=1 AND chapter=1 AND number=27").toString().contains('\n'));
            QVERIFY(db.scalar("SELECT html FROM verses WHERE book=1 AND chapter=1 AND number=27").toString().contains("<br/>"));
        }
        QCOMPARE(db.scalar("SELECT COUNT(DISTINCT book) FROM verses").toInt(),66);
        QCOMPARE(db.scalar("SELECT COUNT(*) FROM (SELECT DISTINCT book,chapter FROM verses)").toInt(),1189);
        for (const auto &pair : QList<QPair<QString,QString>>{{"version_id",id},{"abbreviation",abbr},{"edition",name},{"copyright",edition.copyright}}) {
            QSqlQuery q(db.db); q.prepare("SELECT value FROM metadata WHERE key=?"); q.addBindValue(pair.first);
            QVERIFY(q.exec()); QVERIFY(q.next()); QCOMPARE(q.value(0).toString(),pair.second);
        }
        const bool kjv = id == "kjv-2006";
        QCOMPARE(db.scalar("SELECT MAX(number) FROM verses WHERE book=47 AND chapter=13").toInt(),kjv ? 14 : 13);
        QCOMPARE(db.scalar("SELECT MAX(number) FROM verses WHERE book=64").toInt(),kjv ? 14 : 15);
        QCOMPARE(db.scalar("SELECT MAX(number) FROM verses WHERE book=66 AND chapter=12").toInt(),kjv ? 17 : 18);
        QCOMPARE(db.scalar("SELECT COUNT(*) FROM verses WHERE book=40 AND chapter=17 AND number=21").toInt(),kjv ? 1 : 0);
        QSqlQuery q(db.db);
        // A whole-verse deletion and an edition swap must both fail validation.
        QVERIFY(db.db.transaction()); QVERIFY(q.exec("DELETE FROM verses WHERE book=1 AND chapter=1 AND number=2"));
        QVERIFY(db.db.commit()); QVERIFY(!EpubImporter::validateDatabase(target,&error));
        // Reimport restores the same stable identity and complete database.
        db.db.close();
        const auto again = EpubImporter::importFile(path,target);
        QVERIFY2(again.ok,qPrintable(again.error)); QCOMPARE(again.edition.id,id);
        QVERIFY(db.db.open());
        QVERIFY(q.exec("UPDATE metadata SET value='unsupported' WHERE key='version_id'"));
        QVERIFY(!EpubImporter::validateDatabase(target,&error));
        if (!kjv) {
            QVERIFY(q.exec("DELETE FROM metadata WHERE key IN ('version_id','abbreviation','copyright')"));
            QVERIFY2(EpubImporter::validateDatabase(target,&error),qPrintable(error));
            // Existing public helpers remain NET numbering metadata.
            QCOMPARE(EpubImporter::chapterMaximum(47,13),13);
            QCOMPARE(EpubImporter::chapterMaximum(64,1),15);
            QCOMPARE(EpubImporter::chapterMaximum(66,12),18);
            QVERIFY(EpubImporter::omittedVerse(40,17,21));
        } else {
            QVERIFY(q.exec("UPDATE metadata SET value='kjv-2006' WHERE key='version_id'"));
        }
    }
    void kjvAllWordsAndFormatting() {
        const auto path = source("OMABIBLE_TEST_KJV_EPUB");
        if (path.isEmpty()) QSKIP("Set OMABIBLE_TEST_KJV_EPUB.");
        const auto target = root.filePath("kjv-2006.sqlite");
        if (!QFile::exists(target)) {
            const auto result = EpubImporter::importFile(path,target);
            QVERIFY2(result.ok,qPrintable(result.error));
        }
        Database db(target);
        const auto entries = unzip(path);
        int checked = 0, supplied = 0, headings = 0, poetry = 0;
        // Independent DOM traversal: compare every source word, including inline
        // tails and nd/wj spans, against the stream parser's database output.
        for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
            if (!it.key().endsWith(".xhtml") && !it.key().endsWith(".html")) continue;
            QDomDocument doc;
            // Retain whitespace-only inline tails in this independent oracle.
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
            QVERIFY(doc.setContent(it.value(),QDomDocument::ParseOption::PreserveSpacingOnlyNodes));
#elif QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
            QXmlInputSource input; input.setData(it.value());
            QXmlSimpleReader reader;
            reader.setFeature("http://trolltech.com/xml/features/report-whitespace-only-CharData",true);
            QVERIFY(doc.setContent(&input,&reader));
QT_WARNING_POP
#else
            QSKIP("The independent DOM oracle requires Qt 6.5+ or Qt 5.");
#endif
            const auto title = doc.elementsByTagName("title").at(0).toElement().text();
            const int book = EpubImporter::bookId(title.mid(QString("King James (Authorized) Version ").size()));
            if (!book) continue;
            QString marker;
            QMap<QString,QPair<QString,QString>> expected;
            std::function<void(QDomNode,bool)> walk = [&](QDomNode node, bool italic) {
                const auto e = node.toElement();
                const auto cl = e.attribute("class");
                if (cl == "noteref" || cl == "footnote") return;
                if (cl == "verse") {
                    marker = e.attribute("id").mid(2); return;
                }
                if (node.isText() && !marker.isEmpty()) {
                    expected[marker].first += node.nodeValue();
                    if (italic) expected[marker].second += node.nodeValue();
                }
                for (auto child = node.firstChild(); !child.isNull(); child = child.nextSibling()) walk(child,italic || cl == "add");
            };
            const auto divs = doc.elementsByTagName("div");
            for (int i = 0; i < divs.size(); ++i) {
                const auto div = divs.at(i).toElement();
                if (div.attribute("class") == "p" || div.attribute("class") == "q") {
                    walk(div,false);
                    if (!marker.isEmpty()) expected[marker].first += ' ';
                }
            }
            for (auto verse = expected.cbegin(); verse != expected.cend(); ++verse) {
                const auto cv = verse.key().split('_');
                QSqlQuery q(db.db); q.prepare("SELECT text,html,heading,poetry FROM verses WHERE book=? AND chapter=? AND number=?");
                q.addBindValue(book); q.addBindValue(cv[0].toInt()); q.addBindValue(cv[1].toInt());
                QVERIFY(q.exec()); QVERIFY(q.next());
                // Independent source oracle: layout pilcrows are not words.
                QString words = verse.value().first.trimmed();
                words.remove(QRegularExpression("^¶\\s+"));
                QCOMPARE(q.value(0).toString().simplified(),words.simplified());
                const QString html = q.value(1).toString();
                QVERIFY(!html.contains("noteref")); QVERIFY(!html.contains("href="));
                if (!verse.value().second.isEmpty()) { QVERIFY(html.contains("<i>")); ++supplied; }
                if (!q.value(2).toString().isEmpty()) ++headings;
                if (q.value(3).toBool()) ++poetry;
                ++checked;
            }
        }
        QCOMPARE(checked,31102); QVERIFY(supplied > 10000); QVERIFY(headings >= 116); QCOMPARE(poetry,2461);
    }
    void netInternalParagraphs() {
        const auto path = source("OMABIBLE_TEST_EPUB");
        if (path.isEmpty()) QSKIP("Set OMABIBLE_TEST_EPUB.");
        auto entries = unzip(path);
        QString chapter;
        for (auto it = entries.cbegin(); it != entries.cend(); ++it)
            if (it.value().contains("NET Bible 2.1 Genesis 1</title>")) { chapter = it.key(); break; }
        QVERIFY(!chapter.isEmpty());
        entries[chapter] = QString("<html><head><title>NET Bible 2.1 Genesis 1</title></head><body>").toUtf8();
        auto &gen = entries[chapter];
        gen += "<p class='bodytext'><span class='verse'>1:1</span>Alpha</p>\n";
        gen += "<p class='bodytext'>continued <span class='verse'>1:2</span>Beta</p>\n";
        gen += "<p class='bodytext'><span class='verse'>1:3</span>Gamma <span class='verse'>1:4</span>Delta</p>\n";
        gen += "<p class='poetry'><span class='verse'>1:5</span>First line</p>\n<p class='poetry'>second line</p>\n";
        gen += "<p class='bodytext'><span class='verse'>1:6</span>Next paragraph.";
        for (int verse = 7; verse <= 31; ++verse)
            gen += "<span class='verse'>1:" + QByteArray::number(verse) + "</span>Fixture words.";
        gen += "</p></body></html>";
        const auto fixture = root.filePath("net-paragraphs.epub"), target = root.filePath("net-paragraphs.sqlite");
        QVERIFY(zip(fixture,entries));
        const auto result = EpubImporter::importFile(fixture,target); QVERIFY2(result.ok,qPrintable(result.error));
        Database db(target);
        QCOMPARE(db.scalar("SELECT text FROM verses WHERE book=1 AND chapter=1 AND number=1").toString(),QString("Alpha\ncontinued"));
        QCOMPARE(db.scalar("SELECT html FROM verses WHERE book=1 AND chapter=1 AND number=1").toString(),QString("Alpha<br/>continued"));
        QCOMPARE(db.scalar("SELECT text FROM verses WHERE book=1 AND chapter=1 AND number=5").toString(),QString("First line\nsecond line"));
        for (int verse : {1,3,5,6}) QVERIFY(db.scalar(QString("SELECT paragraphStart FROM verses WHERE book=1 AND chapter=1 AND number=%1").arg(verse)).toBool());
        for (int verse : {2,4,7}) QVERIFY(!db.scalar(QString("SELECT paragraphStart FROM verses WHERE book=1 AND chapter=1 AND number=%1").arg(verse)).toBool());
    }
    void kjvNotesAndHtml() {
        const auto path = source("OMABIBLE_TEST_KJV_EPUB");
        if (path.isEmpty()) QSKIP("Set OMABIBLE_TEST_KJV_EPUB.");
        auto entries = unzip(path); QVERIFY(entries.contains("OEBPS/GEN.xhtml"));
        auto &gen = entries["OEBPS/GEN.xhtml"];
        int start = gen.indexOf("id=\"GN1_1\""); QVERIFY(start > 0);
        start = gen.indexOf("</span>",start) + 7;
        const int end = gen.indexOf("<span class=\"verse\"",start); QVERIFY(end > start);
        gen.replace(start,end-start,QString("¶ Alpha <span class='add'>supplied</span><a epub:type='noteref'><b>NOTE POISON</b></a> tail <span class='nd'>NAME</span> <span class='wj'>spoken</span><span epub:type='footnote'>FOOTNOTE POISON</span> quoted ‘¶ literal’ end. ").toUtf8());
        // Equivalent XHTML content with .html spine paths is supported.
        entries.insert("OEBPS/GEN.html",entries.take("OEBPS/GEN.xhtml"));
        entries["OEBPS/content.opf"].replace("GEN.xhtml","GEN.html");
        const auto fixture = root.filePath("mutated.epub"); QVERIFY(zip(fixture,entries));
        QString error; QCOMPARE(EpubImporter::inspectFile(fixture,&error).id,QString("kjv-2006"));
        const auto target = root.filePath("mutated.sqlite");
        const auto result = EpubImporter::importFile(fixture,target); QVERIFY2(result.ok,qPrintable(result.error));
        Database db(target);
        QCOMPARE(db.scalar("SELECT text FROM verses WHERE book=1 AND chapter=1 AND number=1").toString(),QString("Alpha supplied tail NAME spoken quoted ‘¶ literal’ end."));
        QCOMPARE(db.scalar("SELECT html FROM verses WHERE book=1 AND chapter=1 AND number=1").toString(),QString("Alpha <i>supplied</i> tail NAME spoken quoted ‘¶ literal’ end."));
        const auto installed = bytes(target);
        entries["OEBPS/GEN.html"].replace("id=\"GN1_2\"","id=\"GN1_1\"");
        QVERIFY(zip(fixture,entries));
        const auto duplicate = EpubImporter::importFile(fixture,target);
        QVERIFY(!duplicate.ok); QVERIFY(duplicate.error.contains("Duplicate")); QCOMPARE(bytes(target),installed);
        entries["OEBPS/GEN.html"].replace("id='GN0_0'","id='ZZ0_0'");
        QVERIFY(zip(fixture,entries));
        QVERIFY(EpubImporter::inspectFile(fixture,&error).id.isEmpty()); QVERIFY(error.contains("prefix"));
    }
    void unsupportedOcr_data() {
        QTest::addColumn<QString>("path");
        QTest::newRow("original-NKJV") << source("OMABIBLE_TEST_NKJV_ORIGINAL_EPUB");
        QTest::newRow("replacement-NKJV") << source("OMABIBLE_TEST_NKJV_EPUB");
    }
    void unsupportedOcr() {
        QFETCH(QString,path);
        if (path.isEmpty()) QSKIP("Set OMABIBLE_TEST_NKJV_ORIGINAL_EPUB / OMABIBLE_TEST_NKJV_EPUB.");
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto before = QSqlDatabase::connectionNames();
        QString error;
        QVERIFY(EpubImporter::inspectFile(path,&error).id.isEmpty());
        QVERIFY2(error.contains("OCR") && error.contains("verse markers"),qPrintable(error));
        const QString target = dir.filePath("bible.sqlite");
        const auto result = EpubImporter::importFile(path,target);
        QVERIFY(!result.ok); QVERIFY(result.edition.id.isEmpty()); QVERIFY(result.error.contains("OCR"));
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden).size(),0);
        QFile sentinel(target); QVERIFY(sentinel.open(QIODevice::WriteOnly)); sentinel.write("existing database sentinel"); sentinel.close();
        const auto original = bytes(target);
        QVERIFY(!EpubImporter::importFile(path,target).ok); QCOMPARE(bytes(target),original);
        QCOMPARE(QSqlDatabase::connectionNames(),before);
    }
    void boundedMetadataAndUnsafeArchive() {
        const QByteArray container = "<container><rootfiles><rootfile full-path='content.opf'/></rootfiles></container>";
        const QByteArray opf = "<package><metadata><meta property='schema:accessibilitySummary'>Generated using automated character recognition</meta></metadata><manifest><item id='p' href='page_0.html'/></manifest><spine><itemref idref='p'/></spine></package>";
        QMap<QString,QByteArray> entries = {{"mimetype","application/epub+zip"},{"META-INF/container.xml",container},{"content.opf",opf},
            {"page_0.html",QByteArray(9*1024*1024,'x')}};
        const auto path = root.filePath("ocr-metadata.epub"); QVERIFY(zip(path,entries));
        QString error;
        QVERIFY(EpubImporter::inspectFile(path,&error).id.isEmpty());
        QVERIFY2(error.contains("OCR"),qPrintable(error)); // body is never inflated/scanned
        entries.insert("../unsafe.html","<html/>"); QVERIFY(zip(path,entries));
        QVERIFY(EpubImporter::inspectFile(path,&error).id.isEmpty()); QVERIFY(error.contains("unsafe"));
    }
};

QTEST_GUILESS_MAIN(ImporterVersionsTest)
#include "tst_importer_versions.moc"
