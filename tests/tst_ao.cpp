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
        }, {QJsonObject{{"noteId", 0}, {"caller", QJsonValue::Null}, {"text", "Subtitle note"}},
            QJsonObject{{"noteId", 1}, {"caller", "+"}, {"text", "Verse note"}}})})});
    }
private slots:
    void rejectsBrokenContent_data() {
        QTest::addColumn<QString>("scenario");
        for (const auto &name : {"unknown-book", "duplicate-book", "duplicate-order", "duplicate-chapter", "duplicate-verse", "unknown-type", "missing-note", "duplicate-note", "wrong-count", "empty-chapter", "indent-bound", "invalid-number", "deep-json", "oversized-text"})
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
        if (scenario == "duplicate-verse") items.append(items[2]);
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
