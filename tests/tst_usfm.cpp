#include "usfmimporter.h"
#include "biblebooks.h"
#include <QtTest/QTest>
#include <QCryptographicHash>
#include <QSqlRecord>
#include <QDir>
#include <QFile>
#include <QMap>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QXmlStreamReader>
#include <archive.h>
#include <archive_entry.h>

namespace {
bool zip(const QString &path, const QMap<QString, QByteArray> &entries, bool link = false)
{
    archive *a = archive_write_new(); archive_write_set_format_zip(a);
    bool ok = archive_write_open_filename(a, QFile::encodeName(path).constData()) == ARCHIVE_OK;
    for (auto it = entries.cbegin(); ok && it != entries.cend(); ++it) {
        archive_entry *e = archive_entry_new();
        archive_entry_set_pathname(e, it.key().toUtf8().constData());
        archive_entry_set_filetype(e, link ? AE_IFLNK : AE_IFREG); archive_entry_set_perm(e, 0600);
        if (link) archive_entry_set_symlink(e, "/tmp/target");
        archive_entry_set_size(e, link ? 0 : it.value().size());
        ok = archive_write_header(a, e) == ARCHIVE_OK;
        if (!link && ok) ok = archive_write_data(a, it.value().constData(), it.value().size()) == it.value().size();
        archive_entry_free(e);
    }
    ok = archive_write_close(a) == ARCHIVE_OK && ok; archive_write_free(a); return ok;
}
QMap<QString, QByteArray> unzip(const QString &path)
{
    QMap<QString, QByteArray> entries;
    archive *a = archive_read_new(); archive_read_support_format_zip(a);
    if (archive_read_open_filename(a, QFile::encodeName(path).constData(), 65536) == ARCHIVE_OK) {
        archive_entry *entry;
        while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
            QByteArray bytes; char buffer[65536]; la_ssize_t n;
            while ((n = archive_read_data(a, buffer, sizeof(buffer))) > 0) bytes.append(buffer, n);
            entries.insert(QString::fromUtf8(archive_entry_pathname(entry)), bytes);
        }
    }
    archive_read_free(a); return entries;
}
struct Database {
    QString name = QUuid::createUuid().toString();
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", name);
    explicit Database(const QString &path) { db.setDatabaseName(path); db.open(); }
    ~Database() { db.close(); db = {}; QSqlDatabase::removeDatabase(name); }
    QVariant scalar(const QString &sql) { QSqlQuery q(db); return q.exec(sql) && q.next() ? q.value(0) : QVariant(); }
};
QMap<QString, QByteArray> fixture(const QByteArray &body, const QByteArray &code = "GEN")
{
    return {{"02-" + code + "original.usfm", "\\id " + code + " - Original Test Translation\n\\toc1 Source long name\n\\toc2 Source name\n\\toc3 Src\n" + body},
        {"copr.htm", "<html><title>Original &amp; Test Translation</title><h1>Original</h1><h2>Test</h2><p>copyright &copy; 2026 A &amp; B &#x2014; Authors<br/>Language: English</p><div class='fine'>FOOTER POISON</div></html>"}};
}

// Independent lexical oracle: split on source chapter/verse lines, then walk
// characters, masking note/reference spans and word attributes. It deliberately
// does not construct formatting, paragraphs, metadata, or importer tokens.
QString oracleWords(QString source)
{
    QString output;
    QStringList notes, references;
    bool hiddenLine = false, attributes = false, word = false;
    static const QRegularExpression headings("^(s[0-9]?|ms[0-9]?|d|qa|r|mr|sp|cl|cp)$");
    static const QRegularExpression paragraphs("^(p.*|m|q[0-9]?|qr|qc|li[0-9]?|b|nb)$");
    for (qsizetype i = 0; i < source.size();) {
        if (source[i] != '\\') {
            if (source[i] == '|' && word && notes.isEmpty() && references.isEmpty()) attributes = true;
            if (source[i] == '\n') hiddenLine = false;
            if (!hiddenLine && notes.isEmpty() && references.isEmpty() && !attributes) output += source[i];
            ++i; continue;
        }
        ++i;
        if (i < source.size() && source[i] == '+') ++i;
        QString marker;
        while (i < source.size() && source[i].isLetterOrNumber()) marker += source[i++];
        const bool close = i < source.size() && source[i] == '*';
        if (close) ++i;
        else if (i < source.size() && source[i] == ' ') ++i;
        if (marker == "w") { word = !close; if (close) attributes = false; }
        if (marker == "f" || marker == "fe" || marker == "x") {
            if (close) notes.removeLast(); else notes.append(marker);
        } else if (notes.isEmpty() && (marker == "va" || marker == "ca" || marker == "vp" || marker == "xt")) {
            if (close) references.removeLast(); else references.append(marker);
        }
        if (notes.isEmpty() && references.isEmpty() && !close) {
            if (headings.match(marker).hasMatch()) hiddenLine = true;
            if (paragraphs.match(marker).hasMatch()) {
                output += ' ';
                qsizetype next = i;
                while (next < source.size() && source[next].isSpace()) ++next;
                if (next < source.size() && source[next] == QChar(0xb6)) i = next + 1;
            }
        }
    }
    output = output.simplified();
    if (output.startsWith("¶ ")) output.remove(0, 2);
    return output;
}
struct Oracle { QMap<QString, QString> verses; QMap<int, int> chapters; int markers = 0; };
Oracle oracle(const QString &source)
{
    Oracle result;
    static const QRegularExpression boundary("(?:^|\n)\\\\([cv]) +([0-9]+)(?=\\s|$)");
    auto matches = boundary.globalMatch(source);
    int chapter = 0, verse = 0;
    qsizetype start = 0;
    auto finish = [&](qsizetype end) {
        if (!verse) return;
        const QString words = oracleWords(source.mid(start, end - start));
        ++result.markers;
        if (!words.isEmpty()) { result.verses.insert(QString("%1:%2").arg(chapter).arg(verse), words); ++result.chapters[chapter]; }
    };
    while (matches.hasNext()) {
        const auto match = matches.next(); finish(match.capturedStart());
        if (match.captured(1) == "c") { chapter = match.captured(2).toInt(); verse = 0; }
        else verse = match.captured(2).toInt();
        start = match.capturedEnd();
    }
    finish(source.size());
    return result;
}
QString renderedWords(const QString &html)
{
    QXmlStreamReader xml("<root>" + html + "</root>");
    QString text;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isCharacters()) text += xml.text();
        if (xml.isStartElement() && xml.name() == u"br") text += ' ';
    }
    return xml.hasError() ? "INVALID GENERATED HTML: " + xml.errorString() : text.simplified();
}
}

class UsfmTest : public QObject {
    Q_OBJECT
    QTemporaryDir root;
private slots:
    void stableBooks() {
        QCOMPARE(BibleBooks::canon().size(), 80);
        for (int i = 0; i < 80; ++i) {
            QCOMPARE(BibleBooks::bookId(BibleBooks::canon()[i].code), i + 1);
            QCOMPARE(BibleBooks::bookId(BibleBooks::canon()[i].name), i + 1);
        }
        QCOMPARE(BibleBooks::bookId("First Samuel"), 9);
        QCOMPARE(BibleBooks::bookId("II Cor."), 47);
        QCOMPARE(BibleBooks::bookId("ESG"), 69);
        QCOMPARE(BibleBooks::bookId("2ES"), 80);
        QCOMPARE(BibleBooks::bookId("FRT"), 0);
    }
    void renderAndMetadata() {
        const QString path = root.filePath("renamed.zip"), target = root.filePath("render.sqlite");
        const QByteArray body = "\\is1 Introduction\n\\ip INTRO POISON \\it intro\\it*\n"
            "\\c 1\n\\ms1 First heading\n\\ms2 Second heading\n\\p\n"
            "\\v 1 ¶ First \\w word|strong=\"H123\" lemma=\"POISON\"\\w* \\add supplied \\+w words|strong=\"G4\"\\+w*\\add* tail."
            "\\f + \\fr 1:1 \\ft NOTE POISON \\+it italic\\+it* \\+fl unclosed subfield\\ft hidden\\f* After."
            "\\x - \\xo 1:1 \\xt XREF POISON\\x*\n\\q1 Second line \\nd Name\\nd* \\qt quote\\qt* \\qs Selah\\qs*.\n"
            "\\q2\n\\v 3 Third \\va 99\\va*verse \\add (see \\+xt REF POISON\\+xt* surrounding)\\add* 37\\ord th\\ord*.\n"
            "\\s1 Carried heading\n\\p\n\\v 4 \\f + \\ft empty note\\f*\n\\v 8 Last ¶ embedded.\n"
            "\\c 3\n\\p\n\\v 7 \\qac Aleph\\qac* \\sup raised\\sup* \\it end\\it*.\n";
        QVERIFY(zip(path, fixture(body)));
        QString error;
        const auto edition = UsfmImporter::inspectFile(path, &error);
        QCOMPARE(edition.id, QString("original"));
        QCOMPARE(edition.name, QString("Original & Test Translation"));
        QCOMPARE(edition.copyright, QString("copyright © 2026 A & B — Authors"));
        auto progress = std::make_shared<std::atomic<double>>(0);
        const auto result = UsfmImporter::importFile(path, target, progress);
        QVERIFY2(result.ok, qPrintable(result.error)); QCOMPARE(result.verseCount, 4); QCOMPARE(progress->load(), 1.0);
        QVERIFY2(UsfmImporter::validateDatabase(target, &error), qPrintable(error));
        Database db(target);
        QCOMPARE(db.scalar("SELECT text FROM verses WHERE chapter=1 AND number=1").toString(),
                 QString("First word supplied words tail. After.\nSecond line Name quote Selah."));
        QCOMPARE(db.scalar("SELECT heading FROM verses WHERE number=1").toString(), QString("First heading\nSecond heading"));
        const auto html = db.scalar("SELECT html FROM verses WHERE number=1").toString();
        QVERIFY(html.contains("<i>")); QVERIFY(html.contains("<br/>")); QVERIFY(!html.contains("strong="));
        QCOMPARE(db.scalar("SELECT poetry FROM verses WHERE number=1").toInt(), 1);
        QCOMPARE(db.scalar("SELECT paragraphStart FROM verses WHERE number=3").toInt(), 1);
        QCOMPARE(db.scalar("SELECT text FROM verses WHERE number=3").toString(), QString("Third verse (see surrounding) 37th."));
        QCOMPARE(db.scalar("SELECT heading FROM verses WHERE number=8").toString(), QString("Carried heading"));
        QCOMPARE(db.scalar("SELECT count(*) FROM source_keys").toInt(), 5);
        QCOMPARE(db.scalar("SELECT count(*) FROM verses WHERE text LIKE '%POISON%' OR html LIKE '%POISON%'").toInt(), 0);
        QCOMPARE(db.scalar("SELECT count(*) FROM book_aliases WHERE name='Source long name'").toInt(), 1);
        QCOMPARE(db.scalar("SELECT text FROM verses WHERE chapter=3").toString(), QString("Aleph raised end."));
    }
    void sourceOrdering() {
        auto entries = fixture("\\c 10\n\\p\n\\v 4 Start.\n\\c 16\n\\v 24 End.\n", "ESG");
        entries["01-FRToriginal.usfm"] = "\\id FRT\n\\ip Front matter only";
        entries["00-INToriginal.sfm"] = "\\id INT\n\\ip Introduction only";
        entries["71-REVoriginal.sfm"] = "\\id REV\n\\c 2\n\\v 2 Revelation source.\n";
        entries["74-ACToriginal.sfm"] = "\\id ACT\n\\c 1\n\\v 1 Acts source.\n";
        const QString path = root.filePath("ordering.zip"), target = root.filePath("ordering.sqlite");
        QVERIFY(zip(path, entries)); const auto result = UsfmImporter::importFile(path, target);
        QVERIFY2(result.ok, qPrintable(result.error));
        Database db(target);
        QCOMPARE(db.scalar("SELECT group_concat(id,',') FROM (SELECT id FROM books ORDER BY ordinal)").toString(), QString("69,66,44"));
        QCOMPARE(db.scalar("SELECT group_concat(chapter,',') FROM (SELECT chapter FROM source_chapters WHERE book=69 ORDER BY chapter)").toString(), QString("10,16"));
    }
    void sourcePreamblesAndNestedNotes() {
        const QString path = root.filePath("preambles.zip"), target = root.filePath("preambles.sqlite");
        auto entries = fixture("\\c 1\n\\s1 Prologue\n\\p UNNUMBERED PROLOGUE POISON\n\\p\n\\v 1 First.\n"
            "\\c 3\n\\qc Acrostic label\n\\q1\n\\v 2 Word\\f + \\ft Outer \\+it italic \\x - \\xt NESTED POISON\\x* note\\+it*\\f* tail.\n");
        QVERIFY(zip(path, entries));
        const auto result = UsfmImporter::importFile(path, target); QVERIFY2(result.ok, qPrintable(result.error));
        Database db(target);
        QCOMPARE(db.scalar("SELECT text FROM verses WHERE chapter=1").toString(), QString("First."));
        QCOMPARE(db.scalar("SELECT text FROM verses WHERE chapter=3").toString(), QString("Word tail."));
        QCOMPARE(db.scalar("SELECT heading FROM verses WHERE chapter=3").toString(), QString("Acrostic label"));
    }
    void notesAndWordsOfJesus() {
        const QString path = root.filePath("schema4.zip"), target = root.filePath("schema4.sqlite");
        auto entries = fixture("\\c 1\n\\s1 Heading\\f + \\ft Heading note\\f*\n\\p\n"
            "\\v 1 ¶ He said,\\f a \\fr 1:1 \\ft Or \\fq spoke\\fq* \\ft plainly\\f* \\wj  \\+w Follow|strong=\"G1\"\\+w* \\+add me\\+add*.\\wj* Then\\x - \\xo 1:1 \\xt Ps 33:6,9; Mk 16:2\\x* went.\n"
            "\\v 2 Plain\\fe 1 \\ft End note \\+w word|strong=\"X\"\\+w*\\fe* text.\n");
        QVERIFY(zip(path, entries));
        const auto result = UsfmImporter::importFile(path, target); QVERIFY2(result.ok, qPrintable(result.error));
        QString error; QVERIFY2(UsfmImporter::validateDatabase(target, &error), qPrintable(error));
        Database db(target);
        QCOMPARE(db.scalar("SELECT value FROM metadata WHERE key='schema'").toString(), QString("4"));
        const QString text = db.scalar("SELECT text FROM verses WHERE number=1").toString();
        QCOMPARE(text, QString("He said, Follow me. Then went."));
        // Heading note anchors at the start of the following verse.
        QCOMPARE(db.scalar("SELECT offset||'|'||kind||'|'||text FROM notes WHERE verse=1 AND seq=1").toString(), QString("0|f|Heading note"));
        QCOMPARE(db.scalar("SELECT offset||'|'||caller||'|'||text FROM notes WHERE verse=1 AND seq=2").toString(),
                 QString("%1|a|1:1 Or spoke plainly").arg(text.indexOf(',') + 1));
        QCOMPARE(db.scalar("SELECT offset||'|'||kind||'|'||targets FROM notes WHERE verse=1 AND seq=3").toString(),
                 QString("%1|x|Ps 33:6,9; Mk 16:2").arg(text.indexOf(" went")));
        QCOMPARE(db.scalar("SELECT kind||'|'||text||'|'||offset FROM notes WHERE verse=2").toString(), QString("fe|End note word|5"));
        QCOMPARE(db.scalar("SELECT start||'-'||end FROM words_of_jesus WHERE verse=1").toString(),
                 QString("%1-%2").arg(text.indexOf("Follow")).arg(text.indexOf(" Then")));
        QVERIFY(db.scalar("SELECT html FROM verses WHERE number=1").toString().contains("<span class=\"wj\">"));
        QCOMPARE(db.scalar("SELECT count(*) FROM words_of_jesus WHERE verse=2").toInt(), 0);
        // Tampering with the new tables is caught by the schema 4 digest.
        { QSqlQuery q(db.db); QVERIFY(q.exec("UPDATE notes SET text='Altered' WHERE verse=2")); }
        QVERIFY(!UsfmImporter::validateDatabase(target, &error));
    }
    void schema3StillValid() {
        // A schema 3 generation (no notes) keeps validating with its own digest.
        const QString path = root.filePath("legacy.zip"), target = root.filePath("legacy.sqlite");
        QVERIFY(zip(path, fixture("\\c 1\n\\p\n\\v 1 Old\\f + \\ft note\\f* text.\n")));
        QVERIFY(UsfmImporter::importFile(path, target).ok);
        {
            Database db(target); QSqlQuery q(db.db);
            QVERIFY(q.exec("DROP TABLE notes")); QVERIFY(q.exec("DROP TABLE words_of_jesus"));
            QVERIFY(q.exec("DELETE FROM metadata WHERE key IN ('note_count','words_of_jesus')"));
            QVERIFY(q.exec("UPDATE metadata SET value='3' WHERE key='schema'"));
            QCryptographicHash hash(QCryptographicHash::Sha256);
            auto field = [&](const QByteArray &v) { hash.addData(QByteArray::number(v.size())); hash.addData(":"); hash.addData(v); };
            for (const QString &sql : {QString("SELECT key,value FROM metadata WHERE key<>'content_digest' ORDER BY key"),
                     QString("SELECT id,code,name,abbreviation,ordinal FROM books ORDER BY ordinal"),
                     QString("SELECT book,chapter FROM source_chapters ORDER BY book,chapter"),
                     QString("SELECT book,name FROM book_aliases ORDER BY book,name"),
                     QString("SELECT book,chapter,number,retained FROM source_keys ORDER BY book,chapter,number"),
                     QString("SELECT book,chapter,number,text,html,heading,poetry,paragraphStart FROM verses ORDER BY book,chapter,number")}) {
                field(sql.toUtf8()); QVERIFY(q.exec(sql));
                while (q.next()) for (int i = 0; i < q.record().count(); ++i) field(q.value(i).toString().toUtf8());
            }
            q.prepare("UPDATE metadata SET value=? WHERE key='content_digest'"); q.addBindValue(QString::fromLatin1(hash.result().toHex()));
            QVERIFY(q.exec());
        }
        QString error; QVERIFY2(UsfmImporter::validateDatabase(target, &error), qPrintable(error));
        Database db(target); { QSqlQuery q(db.db); QVERIFY(q.exec("UPDATE metadata SET value='2' WHERE key='schema'")); }
        QVERIFY(!UsfmImporter::validateDatabase(target, &error));
    }
    void malformed_data() {
        QTest::addColumn<QByteArray>("body"); QTest::addColumn<QString>("error");
        QTest::newRow("bridge") << QByteArray("\\c 1\n\\v 1-2 Words") << QString("integer");
        QTest::newRow("suffix") << QByteArray("\\c 1\n\\v 1a Words") << QString("integer");
        QTest::newRow("duplicate") << QByteArray("\\c 1\n\\v 1 Words\n\\v 1 Again") << QString("ascending");
        QTest::newRow("backwards-chapter") << QByteArray("\\c 3\n\\v 1 Words\n\\c 2\n\\v 1 Again") << QString("ascending");
        QTest::newRow("unknown-body") << QByteArray("\\c 1\n\\v 1 Words \\mystery Scripture") << QString("Unsupported USFM body");
        QTest::newRow("unclosed-style") << QByteArray("\\c 1\n\\v 1 \\add Words") << QString("Unclosed");
        QTest::newRow("mismatch-style") << QByteArray("\\c 1\n\\v 1 \\add Words\\it*") << QString("Mismatched");
        QTest::newRow("unclosed-note") << QByteArray("\\c 1\n\\v 1 Words\\f + \\ft note\n\\v 2 Hidden") << QString("Unclosed");
        QTest::newRow("unclosed-reference") << QByteArray("\\c 1\n\\v 1 Words\\xt reference\n\\v 2 Hidden") << QString("Unclosed");
        QTest::newRow("depth") << QByteArray("\\c 1\n\\v 1 ") + QByteArray("\\add ").repeated(33) + "Word" << QString("depth");
        QTest::newRow("size") << QByteArray("\\c 1\n\\v 1 ") + QByteArray(65537, 'a') << QString("limit");
        QTest::newRow("empty-book") << QByteArray("\\c 1\n\\v 1 \\f + \\ft note\\f*") << QString("integrity");
        QTest::newRow("utf8") << QByteArray("\\c 1\n\\v 1 ") + char(0xff) << QString("UTF-8");
    }
    void malformed() {
        QFETCH(QByteArray, body); QFETCH(QString, error);
        const QString path = root.filePath("malformed.zip"), target = root.filePath("protected.sqlite");
        QVERIFY(zip(path, fixture(body)));
        QFile old(target); QVERIFY(old.open(QIODevice::WriteOnly)); QCOMPARE(old.write("existing target"), 15); old.close();
        const auto result = UsfmImporter::importFile(path, target);
        QVERIFY(!result.ok); QVERIFY2(result.error.contains(error, Qt::CaseInsensitive), qPrintable(result.error));
        QVERIFY(old.open(QIODevice::ReadOnly)); QCOMPARE(old.readAll(), QByteArray("existing target"));
    }
    void unsafeArchives() {
        const QString path = root.filePath("unsafe.zip"); QString error;
        for (const QString &name : {QString("../book.usfm"), QString("/book.usfm"), QString("..\\book.usfm"), QString("C:book.usfm")}) {
            QVERIFY(zip(path, {{name,"\\id GEN\n\\c 1\n\\v 1 Text"}}));
            QVERIFY(UsfmImporter::inspectFile(path, &error).id.isEmpty()); QVERIFY2(error.contains("Unsafe"), qPrintable(name + ": " + error));
        }
        QVERIFY(zip(path, {{"link.usfm",{}}}, true));
        QVERIFY(UsfmImporter::inspectFile(path, &error).id.isEmpty()); QVERIFY2(error.contains("linked"), qPrintable(error));
        auto entries = fixture("\\c 1\n\\v 1 Original");
        entries["03-EXOdifferent.usfm"] = "\\id EXO\n\\c 1\n\\v 1 Other";
        QVERIFY(zip(path, entries)); QVERIFY(UsfmImporter::inspectFile(path, &error).id.isEmpty()); QVERIFY(error.contains("Inconsistent"));
    }
    void standalone() {
        const QString path = root.filePath("book.sfm"), target = root.filePath("standalone.sqlite");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("\\id GEN - An Original Translation\n\\c 1\n\\v 2 Original text.\n"); file.close();
        QString error; const auto edition = UsfmImporter::inspectFile(path, &error);
        QVERIFY2(edition.id.startsWith("usfm-"), qPrintable(error));
        const auto result = UsfmImporter::importFile(path, target); QVERIFY2(result.ok, qPrintable(result.error));
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write("\\id GEN\n\\c 1\n\\v 1 Text"); file.close();
        QVERIFY(UsfmImporter::inspectFile(path, &error).id.isEmpty()); QVERIFY(error.contains("title"));
    }
    void corruption_data() {
        QTest::addColumn<QString>("sql");
        QTest::newRow("deleted-verse") << QString("DELETE FROM verses WHERE number=2");
        QTest::newRow("changed-key") << QString("UPDATE verses SET number=9 WHERE number=2");
        QTest::newRow("changed-text") << QString("UPDATE verses SET text='Corrupted' WHERE number=2");
        QTest::newRow("changed-source") << QString("UPDATE source_keys SET number=9 WHERE number=2");
        QTest::newRow("deleted-chapter") << QString("DELETE FROM source_chapters");
        QTest::newRow("changed-ordinal") << QString("UPDATE books SET ordinal=8");
        QTest::newRow("changed-title") << QString("UPDATE metadata SET value='Mislabelled' WHERE key='edition'");
        QTest::newRow("changed-alias") << QString("UPDATE book_aliases SET name='Altered' WHERE name='Src'");
    }
    void corruption() {
        QFETCH(QString, sql);
        const QString path = root.filePath("integrity.zip"), target = root.filePath("integrity.sqlite");
        QVERIFY(zip(path, fixture("\\c 1\n\\v 1 First.\n\\v 2 Second.")));
        const auto result = UsfmImporter::importFile(path, target); QVERIFY2(result.ok, qPrintable(result.error));
        { Database db(target); QSqlQuery q(db.db); QVERIFY(q.exec(sql)); }
        QString error; QVERIFY(!UsfmImporter::validateDatabase(target, &error)); QVERIFY(!error.isEmpty());
    }
    void actualArchives_data() {
        QTest::addColumn<QString>("id"); QTest::addColumn<QString>("abbreviation");
        for (const auto &pair : QList<QPair<QString,QString>>{{"engnet","NET"},{"eng-kjv2006","KJV"},{"eng-kjv","KJV+"},
                  {"engojb","OJB"},{"engylt","YLT"},{"eng-asv","ASV"},{"engbsb","BSB"}})
            QTest::newRow(qPrintable(pair.first)) << pair.first << pair.second;
    }
    void actualArchives() {
        QFETCH(QString, id); QFETCH(QString, abbreviation);
        const QString directory = qEnvironmentVariable("OMAWORD_TEST_USFM_DIR");
        if (directory.isEmpty()) QSKIP("Set OMAWORD_TEST_USFM_DIR to test the seven source ZIP archives.");
        const QString path = QDir(directory).filePath(id + "_usfm.zip"), target = root.filePath(id + ".sqlite");
        QVERIFY2(QFile::exists(path), qPrintable(path));
        const QString renamed = root.filePath("unrelated-name.zip"); QFile::remove(renamed); QVERIFY(QFile::copy(path, renamed));
        QString error;
        const auto edition = UsfmImporter::inspectFile(renamed, &error);
        QCOMPARE(edition.id, id); QCOMPARE(edition.abbreviation, abbreviation);
        QVERIFY(!edition.copyright.isEmpty()); QVERIFY(!edition.copyright.contains("HTML generated"));
        if (id == "engnet") { QVERIFY(edition.copyright.contains("1996-2016")); QVERIFY(!edition.name.contains("2.1")); }
        if (id == "engojb") QVERIFY(edition.copyright.contains("Creative Commons Attribution"));
        const auto result = UsfmImporter::importFile(renamed, target);
        QVERIFY2(result.ok, qPrintable(result.error));
        QVERIFY2(UsfmImporter::validateDatabase(target, &error), qPrintable(error));
        Database db(target);
        int total = 0, books = 0, markers = 0;
        const auto files = unzip(path);
        for (auto it = files.cbegin(); it != files.cend(); ++it) {
            if (!it.key().endsWith(".usfm") && !it.key().endsWith(".sfm")) continue;
            const QString source = QString::fromUtf8(it.value());
            const QString code = QRegularExpression("\\\\id +([A-Z0-9]{3})").match(source).captured(1);
            if (code == "FRT" || code == "INT") continue;
            const int book = BibleBooks::bookId(code); QVERIFY(book > 0);
            const Oracle expected = oracle(source);
            ++books; markers += expected.markers; total += expected.verses.size();
            QCOMPARE(db.scalar(QString("SELECT count(*) FROM verses WHERE book=%1").arg(book)).toInt(), expected.verses.size());
            QCOMPARE(db.scalar(QString("SELECT ordinal FROM books WHERE id=%1").arg(book)).toInt(), books);
            QSqlQuery q(db.db); QVERIFY(q.exec(QString("SELECT chapter,number,text,html FROM verses WHERE book=%1 ORDER BY chapter,number").arg(book)));
            while (q.next()) {
                const QString key = QString("%1:%2").arg(q.value(0).toInt()).arg(q.value(1).toInt());
                const QString actual = q.value(2).toString().simplified();
                QVERIFY2(expected.verses.contains(key), qPrintable(id + " " + code + " " + key));
                if (actual != expected.verses.value(key)) {
                    qWarning().noquote() << id << code << key << "\nACTUAL:" << actual << "\nSOURCE:" << expected.verses.value(key);
                    QCOMPARE(actual, expected.verses.value(key));
                }
                QVERIFY(!actual.contains("strong=")); QVERIFY(!actual.contains('\\'));
                const QString html = q.value(3).toString();
                QVERIFY(!html.contains("strong=")); QVERIFY(!html.contains("<script"));
                QCOMPARE(renderedWords(html), actual);
            }
            for (auto chapter = expected.chapters.cbegin(); chapter != expected.chapters.cend(); ++chapter)
                QCOMPARE(db.scalar(QString("SELECT count(*) FROM verses WHERE book=%1 AND chapter=%2").arg(book).arg(chapter.key())).toInt(), chapter.value());
        }
        QCOMPARE(result.verseCount, total);
        QCOMPARE(db.scalar("SELECT count(*) FROM source_keys").toInt(), markers);
        QCOMPARE(db.scalar("SELECT count(*) FROM books").toInt(), books);
        if (id == "eng-kjv") {
            QCOMPARE(books, 80);
            QCOMPARE(db.scalar("SELECT group_concat(chapter,',') FROM (SELECT chapter FROM source_chapters WHERE book=69 ORDER BY chapter)").toString(), QString("10,11,12,13,14,15,16"));
        }
        if (id == "engojb") {
            QCOMPARE(db.scalar("SELECT group_concat(code,',') FROM (SELECT code FROM books ORDER BY ordinal DESC LIMIT 3)").toString(), QString("3JN,1PE,ACT"));
            QVERIFY(db.scalar("SELECT text FROM verses WHERE book=2 AND chapter=20 AND number=13").toString().contains("Thou shalt not steal."));
            QVERIFY(!db.scalar("SELECT text FROM verses WHERE book=2 AND chapter=20 AND number=13").toString().contains("14"));
        }
        // Schema 4: notes and words of Jesus survive with valid anchors.
        QCOMPARE(db.scalar("SELECT value FROM metadata WHERE key='schema'").toString(), QString("4"));
        const int footnotes = db.scalar("SELECT count(*) FROM notes WHERE kind IN ('f','fe')").toInt();
        const int xrefs = db.scalar("SELECT count(*) FROM notes WHERE kind='x'").toInt();
        const int jesus = db.scalar("SELECT count(*) FROM words_of_jesus").toInt();
        if (id == "eng-kjv") { QVERIFY(jesus > 1000); QVERIFY(footnotes > 100); }
        if (id == "engbsb" || id == "engojb") QVERIFY(footnotes > 100);
        if (id == "engojb") { QCOMPARE(xrefs, 3); QCOMPARE(db.scalar("SELECT targets FROM notes WHERE kind='x' AND book=1 AND chapter=1 AND verse=1").toString(), QString("Ps 51:10")); }
        QCOMPARE(db.scalar("SELECT count(*) FROM notes WHERE length(text)=0 AND length(targets)=0").toInt(), 0);
        qInfo().noquote() << id << footnotes << "footnotes," << xrefs << "cross references," << jesus << "words-of-Jesus spans";
        qInfo().noquote() << id << books << "books," << markers << "source verse markers," << total << "nonempty verses";
    }
};
QTEST_GUILESS_MAIN(UsfmTest)
#include "tst_usfm.moc"
