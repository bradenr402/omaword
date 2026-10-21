#include "testenvironment.h"
#include "aofixture.h"
#include "aoimporter.h"
#include <QtTest>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

class AoTest : public QObject
{
    Q_OBJECT
    QTemporaryDir directory;
    QJsonObject sample() const {
        return AoFixture::complete("eng_net", {AoFixture::book("PSA", 1, {AoFixture::chapter(3, {
            QJsonObject{{"type", "heading"}, {"content", QJsonArray{"Original heading"}}},
            QJsonObject{{"type", "hebrew_subtitle"}, {"content", QJsonArray{"A subtitle", QJsonObject{{"noteId", 0}}}}},
            AoFixture::verse(1, {"  ¶ 😀 <one>", QJsonObject{{"noteId", 1}}, ", two ", QJsonObject{{"text", "said"}, {"wordsOfJesus", true}}, QJsonObject{{"lineBreak", true}}, QJsonObject{{"text", "poem"}, {"poem", 2}, {"descriptive", true}}, QJsonObject{{"lineBreak", true}}}),
            AoFixture::verse(2, {}), QJsonObject{{"type", "line_break"}}, AoFixture::verse(4, {"Next paragraph."})
        }, {QJsonObject{{"noteId", 0}, {"caller", QJsonValue::Null}, {"text", "Subtitle note"}, {"reference", QJsonObject{{"chapter", 3}, {"verse", 0}}}},
            QJsonObject{{"noteId", 1}, {"caller", "+"}, {"text", "Verse note"}}})})});
    }
private slots:
    void explicitEmptyChapterRetainsSourceMetadata() {
        const auto source = directory.filePath("empty-chapter.json"), target = directory.filePath("empty-chapter.sqlite");
        // Chapter 5 has a verse but no text (Brenton's Proverbs 30 holds only a note).
        QVERIFY(AoFixture::write(source, AoFixture::complete("eng_ojb", {AoFixture::book("MAL", 1, {
            AoFixture::chapter(3, {AoFixture::verse(1, {"Original retained text."})}), AoFixture::chapter(4, {}),
            AoFixture::chapter(5, {AoFixture::verse(1, {QJsonObject{{"noteId", 0}}})}, {QJsonObject{{"noteId", 0}, {"caller", "+"}, {"text", "See chapter 3."}}})})})));
        const auto result = AoImporter::importFile(source, target); QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(result.verseCount, 1);
        QString error; QVERIFY2(AoImporter::validateDatabase(target, &error), qPrintable(error));
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "ao-empty"); db.setDatabaseName(target); QVERIFY(db.open()); QSqlQuery q(db);
            QVERIFY(q.exec("SELECT count(*) FROM source_chapters")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(), 3);
            QVERIFY(q.exec("SELECT count(*) FROM source_keys WHERE chapter=4")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(), 0);
            QVERIFY(q.exec("SELECT retained FROM source_keys WHERE chapter=5")); QVERIFY(q.next()); QVERIFY(!q.value(0).toBool());
            QVERIFY(q.exec("SELECT count(*) FROM verses WHERE chapter=5")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(), 0);
        }
        QSqlDatabase::removeDatabase("ao-empty");
    }
    // A verse number that does not increase continues the verse before it, in
    // reading order: Brenton repeats a split verse's number, Onkelos numbers a
    // second half 0, a retelling reorders verses.
    void continuedVersesJoinTheVerseBefore() {
        const auto source = directory.filePath("continued.json"), target = directory.filePath("continued.sqlite");
        QVERIFY(AoFixture::write(source, AoFixture::complete("eng_bre", {AoFixture::book("EXO", 1, {AoFixture::chapter(28, {
            AoFixture::verse(1, {"Split once."}), AoFixture::verse(1, {"Then again."}),
            AoFixture::verse(2, {"Before the heading."}), QJsonObject{{"type", "heading"}, {"content", QJsonArray{"Section"}}},
            AoFixture::verse(0, {"Half ", QJsonObject{{"text", "spoken"}, {"wordsOfJesus", true}}, QJsonObject{{"noteId", 1}}, "."}),
            AoFixture::verse(4, {"Four."}), AoFixture::verse(3, {"Three, read after four."}),
            AoFixture::verse(5, {}), AoFixture::verse(5, {"Five, after an empty part."}),
            AoFixture::verse(6, {QJsonObject{{"text", "A line"}, {"poem", 1}}}), AoFixture::verse(6, {QJsonObject{{"text", "and its pair"}, {"poem", 2}}})
        }, {QJsonObject{{"noteId", 1}, {"caller", "a"}, {"text", "Original note"}}})})})));
        const auto result = AoImporter::importFile(source, target); QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(result.verseCount, 5);
        QString error; QVERIFY2(AoImporter::validateDatabase(target, &error), qPrintable(error));
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "ao-continued"); db.setDatabaseName(target); QVERIFY(db.open()); QSqlQuery q(db);
            struct Row { int number; QString text, heading; bool paragraph; };
            const QList<Row> expected{{1, "Split once. Then again.", "", true},
                // The heading between moves up, the part going on a line of its own.
                {2, "Before the heading.\nHalf spoken.", "Section", false},
                {4, "Four. Three, read after four.", "", false},
                {5, "Five, after an empty part.", "", false},
                {6, "A line\nand its pair", "", false}};
            QVERIFY(q.exec("SELECT number,text,heading,paragraphStart FROM verses ORDER BY number"));
            for (const auto &row : expected) {
                QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(), row.number); QCOMPARE(q.value(1).toString(), row.text);
                QCOMPARE(q.value(2).toString(), row.heading); QCOMPARE(q.value(3).toBool(), row.paragraph);
            }
            QVERIFY(!q.next());
            QVERIFY(q.exec("SELECT group_concat(number) FROM (SELECT number FROM source_keys WHERE retained ORDER BY number)"));
            QVERIFY(q.next()); QCOMPARE(q.value(0).toString(), "1,2,4,5,6");
            QVERIFY(q.exec("SELECT count(*) FROM source_keys WHERE NOT retained")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(), 0);
            // The part's note and words of Jesus follow it.
            QVERIFY(q.exec("SELECT verse,offset FROM notes")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(), 2); QCOMPARE(q.value(1).toInt(), 31);
            QVERIFY(q.exec("SELECT verse,start,end FROM words_of_jesus")); QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), 2); QCOMPARE(q.value(1).toInt(), 25); QCOMPARE(q.value(2).toInt(), 31);
            QVERIFY(q.exec("SELECT verse,levels FROM indents")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(), 6); QCOMPARE(q.value(1).toString(), "12");
        }
        QSqlDatabase::removeDatabase("ao-continued");
    }
    // A heading after a chapter's last verse is dropped, its notes with it
    // (the KJV's subscriptions to Paul's letters).
    void trailingHeadingIsDropped() {
        const auto source = directory.filePath("subscription.json"), target = directory.filePath("subscription.sqlite");
        QVERIFY(AoFixture::write(source, AoFixture::complete("eng_kjv", {AoFixture::book("PHM", 1, {AoFixture::chapter(1, {
            AoFixture::verse(1, {"The only verse."}), QJsonObject{{"type", "heading"}, {"content", QJsonArray{"Written from afar."}}},
            QJsonObject{{"type", "hebrew_subtitle"}, {"content", QJsonArray{"A closing", QJsonObject{{"noteId", 0}}}}}
        }, {QJsonObject{{"noteId", 0}, {"caller", "+"}, {"text", "Closing note"}}})})})));
        const auto result = AoImporter::importFile(source, target); QVERIFY2(result.ok, qPrintable(result.error));
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "ao-trailing"); db.setDatabaseName(target); QVERIFY(db.open()); QSqlQuery q(db);
            QVERIFY(q.exec("SELECT text,heading FROM verses")); QVERIFY(q.next());
            QCOMPARE(q.value(0).toString(), "The only verse."); QVERIFY(q.value(1).toString().isEmpty()); QVERIFY(!q.next());
            QVERIFY(q.exec("SELECT count(*) FROM notes")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(), 0);
        }
        QSqlDatabase::removeDatabase("ao-trailing");
    }
    // Every known book imports by its exact code, past 80 too: PSS is the
    // Psalms of Solomon, though "pss" is an alias of Psalms.
    void everyKnownBookImports() {
        QJsonArray books;
        for (int i = 0; i < BibleBooks::canon().size(); ++i) {
            const auto &book = BibleBooks::canon()[i];
            books.append(AoFixture::book(book.code, i + 1, {AoFixture::chapter(1, {AoFixture::verse(1, {"Original text."})})}, book.name, i >= 66));
        }
        const auto source = directory.filePath("canon.json"), target = directory.filePath("canon.sqlite");
        QVERIFY(AoFixture::write(source, AoFixture::complete("eng_web", books)));
        const auto result = AoImporter::importFile(source, target); QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(result.verseCount, int(BibleBooks::canon().size()));
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "ao-canon"); db.setDatabaseName(target); QVERIFY(db.open()); QSqlQuery q(db);
            QVERIFY(q.exec("SELECT count(*),max(id) FROM books")); QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), 86); QCOMPARE(q.value(1).toInt(), 86);
            QVERIFY(q.exec("SELECT id FROM books WHERE code='PSS'")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(), 85);
        }
        QSqlDatabase::removeDatabase("ao-canon");
    }
    void rejectsBrokenContent_data() {
        QTest::addColumn<QString>("scenario");
        for (const auto &name : {"unknown-book", "duplicate-book", "duplicate-order", "duplicate-chapter", "unknown-type", "missing-note", "duplicate-note", "wrong-count", "empty-chapter", "indent-bound", "invalid-number", "deep-json", "oversized-text"})
            QTest::newRow(name) << QString(name);
    }
    void rejectsBrokenContent() {
        QFETCH(QString, scenario);
        auto root = sample(); auto books = root["books"].toArray(); auto book = books[0].toObject();
        auto chapters = book["chapters"].toArray(); auto wrapper = chapters[0].toObject(); auto chapter = wrapper["chapter"].toObject();
        auto items = chapter["content"].toArray(), notes = chapter["footnotes"].toArray();
        if (scenario == "unknown-book") book["id"] = "XYZ";
        if (scenario == "duplicate-book") books.append(book);
        if (scenario == "duplicate-order") { auto other = book; other["id"] = "GEN"; books.append(other); }
        if (scenario == "duplicate-chapter") chapters.append(wrapper);
        if (scenario == "unknown-type") items.append(QJsonObject{{"type", "unexpected"}, {"content", QJsonArray{"Do not lose me"}}});
        if (scenario == "missing-note") notes.removeAt(1);
        if (scenario == "duplicate-note") notes.append(notes[0]);
        if (scenario == "wrong-count") wrapper["numberOfVerses"] = 99;
        if (scenario == "empty-chapter") items = {};
        if (scenario == "indent-bound") items[2] = AoFixture::verse(1, {QJsonObject{{"text", "Poem"}, {"poem", 5}}});
        if (scenario == "invalid-number") items[2] = AoFixture::verse(0, {"Invalid number"});
        if (scenario == "oversized-text") items[2] = AoFixture::verse(1, {QString(65537, 'x')});
        chapter["content"] = items; chapter["footnotes"] = notes; wrapper["chapter"] = chapter;
        chapters[0] = wrapper; book["chapters"] = chapters; books[0] = book; root["books"] = books;
        if (scenario == "deep-json") { QJsonValue nested(1); for (int i = 0; i < 35; ++i) nested = QJsonArray{nested}; root["extra"] = nested; }
        const auto path = directory.filePath("broken.json"); QVERIFY(AoFixture::write(path, root));
        const auto target = directory.filePath("broken.sqlite");
        const auto result = AoImporter::importFile(path, target); QVERIFY2(!result.ok, qPrintable(scenario)); QVERIFY(!result.error.isEmpty()); QVERIFY(!QFile::exists(target));
    }
    void sourceOrderAndMarkerSpacing() {
        auto root = AoFixture::complete("Case_ID", {
            AoFixture::book("REV", 20, {AoFixture::chapter(2, {AoFixture::verse(3, {"Alpha", QJsonObject{{"noteId", 0}}, QJsonObject{{"noteId", 1}}, "Beta,", " ", "(open", QJsonObject{{"noteId", 2}}, ")", QJsonObject{{"heading", "Inline heading"}}, "Tail."})},
                {QJsonObject{{"noteId", 0}, {"caller", "+"}, {"text", "First"}}, QJsonObject{{"noteId", 1}, {"caller", "+"}, {"text", "Second"}}, QJsonObject{{"noteId", 2}, {"caller", QJsonValue::Null}, {"text", "Third"}}})}),
            AoFixture::book("GEN", 10, {AoFixture::chapter(9, {AoFixture::verse(2, {QStringLiteral("עברית 😀 exact")})})})});
        const auto source = directory.filePath("order.json"), target = directory.filePath("order.sqlite");
        QVERIFY(AoFixture::write(source, root)); const auto result = AoImporter::importFile(source, target); QVERIFY2(result.ok, qPrintable(result.error));
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "ao-order"); db.setDatabaseName(target); QVERIFY(db.open()); QSqlQuery q(db);
            QVERIFY(q.exec("SELECT code FROM books ORDER BY ordinal")); QVERIFY(q.next()); QCOMPARE(q.value(0).toString(), "GEN"); QVERIFY(q.next()); QCOMPARE(q.value(0).toString(), "REV");
            QVERIFY(q.exec("SELECT text,heading FROM verses WHERE book=66")); QVERIFY(q.next()); QCOMPARE(q.value(0).toString(), "Alpha Beta, (open) Tail."); QCOMPARE(q.value(1).toString(), "Inline heading");
        }
        QSqlDatabase::removeDatabase("ao-order");
    }
    void contentAndIntegrity() {
        const auto source = directory.filePath("sample.json"), target = directory.filePath("sample.sqlite");
        QVERIFY(AoFixture::write(source, sample()));
        const auto result = AoImporter::importFile(source, target);
        QVERIFY2(result.ok, qPrintable(result.error)); QCOMPARE(result.edition.id, "eng_net"); QCOMPARE(result.edition.abbreviation, "NET"); QCOMPARE(result.verseCount, 2);
        QString error; QVERIFY2(AoImporter::validateDatabase(target, &error), qPrintable(error));
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "ao-inspect"); db.setDatabaseName(target); QVERIFY(db.open());
            QSqlQuery q(db); QVERIFY(q.exec("SELECT text,html,heading,paragraphStart FROM verses WHERE number=1")); QVERIFY(q.next());
            QCOMPARE(q.value(0).toString(), QString::fromUtf8("😀 <one>, two said\npoem"));
            QVERIFY(q.value(1).toString().contains("&lt;one&gt;")); QCOMPARE(q.value(2).toString(), "Original heading\nA subtitle"); QVERIFY(q.value(3).toBool());
            QVERIFY(q.exec("SELECT offset FROM notes ORDER BY seq")); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(), 0); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(), 8);
            QVERIFY(q.exec("SELECT levels FROM indents")); QVERIFY(q.next()); QCOMPARE(q.value(0).toString(), "02");
            QVERIFY(q.exec("SELECT retained FROM source_keys WHERE number=2")); QVERIFY(q.next()); QVERIFY(!q.value(0).toBool());
            QVERIFY(q.exec("UPDATE verses SET text='corrupt' WHERE number=1"));
        }
        QSqlDatabase::removeDatabase("ao-inspect");
        QVERIFY(!AoImporter::validateDatabase(target, &error)); QVERIFY(!error.isEmpty());
    }
    void malformedAndAtomic() {
        const auto source = directory.filePath("invalid.json"), target = directory.filePath("retained.sqlite");
        const auto good = sample(); QVERIFY(AoFixture::write(source, good)); QVERIFY(AoImporter::importFile(source, target).ok);
        QFile original(target); QVERIFY(original.open(QIODevice::ReadOnly)); const auto bytes = original.readAll(); original.close();
        for (const QString &field : {QString("id"), QString("language"), QString("textDirection"), QString("totalNumberOfVerses")}) {
            auto root = good; auto translation = root["translation"].toObject(); translation[field] = field == "totalNumberOfVerses" ? QJsonValue(99) : QJsonValue("../bad"); root["translation"] = translation;
            QVERIFY(AoFixture::write(source, root)); QVERIFY(!AoImporter::importFile(source, target).ok);
            QVERIFY(original.open(QIODevice::ReadOnly)); QCOMPARE(original.readAll(), bytes); original.close();
        }
        QFile truncated(source); QVERIFY(truncated.open(QIODevice::WriteOnly)); truncated.write("{\"translation\":"); truncated.close();
        QVERIFY(!AoImporter::importFile(source, target).ok);
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    AoTest test; return QTest::qExec(&test, argc, argv);
}
#include "tst_ao.moc"
