#pragma once

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include "biblebooks.h"

namespace AoFixture {
inline QJsonObject verse(int number, const QJsonArray &pieces)
{
    return {{"type", "verse"}, {"number", number}, {"content", pieces}};
}
inline QJsonObject chapter(int number, const QJsonArray &content, const QJsonArray &notes = {})
{
    int count = 0;
    for (const auto &item : content) if (item.toObject().value("type") == "verse") ++count;
    return {{"numberOfVerses", count}, {"chapter", QJsonObject{{"number", number}, {"content", content}, {"footnotes", notes}}}};
}
inline QJsonObject book(const QString &code, int order, const QJsonArray &chapters, const QString &name = {}, bool apocryphal = false)
{
    int count = 0;
    for (const auto &chapter : chapters) count += chapter.toObject().value("numberOfVerses").toInt();
    const int id = BibleBooks::bookId(code);
    const QString display = !name.isEmpty() ? name : id ? QString::fromUtf8(BibleBooks::canon()[id - 1].name) : code;
    return {{"id", code}, {"name", display}, {"commonName", display},
        {"order", order}, {"numberOfChapters", chapters.size()}, {"totalNumberOfVerses", count},
        {"isApocryphal", apocryphal}, {"chapters", chapters}};
}
inline QJsonObject complete(const QString &id, const QJsonArray &books, const QString &name = QStringLiteral("Original AO fixture"))
{
    int bookCount = 0, chapters = 0, verses = 0;
    for (const auto &value : books) {
        const auto book = value.toObject();
        if (book.value("isApocryphal").toBool()) continue;
        ++bookCount; chapters += book.value("numberOfChapters").toInt(); verses += book.value("totalNumberOfVerses").toInt();
    }
    const QString abbreviation = id == "engnet" ? "NET" : id == "eng-kjv2006" ? "KJV" : id == "eng-kjv" ? "KJV+" : id == "engojb" ? "OJB" : id;
    return {{"translation", QJsonObject{{"id", id}, {"name", name}, {"shortName", abbreviation}, {"language", "eng"},
        {"textDirection", "ltr"}, {"numberOfBooks", bookCount}, {"totalNumberOfChapters", chapters},
        {"totalNumberOfVerses", verses}}}, {"books", books}};
}
inline bool write(const QString &path, const QJsonObject &root)
{
    QSaveFile file(path);
    const auto bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
inline bool searchEdition(const QString &path, const QString &id, bool alternate, bool paragraphs = false)
{
    QJsonArray chapters;
    for (int chapterNumber = 1; chapterNumber <= 2; ++chapterNumber) {
        QJsonArray content{QJsonObject{{"type", "heading"}, {"content", QJsonArray{"First source heading"}}}, QJsonObject{{"type", "line_break"}}};
        for (int number = 1; number <= 65; ++number) {
            if (paragraphs && number > 1 && number % 4 == 1) content.append(QJsonObject{{"type", "line_break"}});
            QString text = QString("Common original fixture %1:%2. ").arg(chapterNumber).arg(number);
            if (number == 2) text += alternate ? "Changed edition without the word." : "ALPHA and beta with alphabet soup. A bridge begins";
            else if (number == 3) text = "here across verses. Common original text. Blue & <bright> original text.";
            else if (number == 8 || number == 20 || number == 50) text += alternate ? "Different words." : "Alpha beta and another alpha.";
            else text += QString("Ordinary words for a long scrolling chapter. ").repeated(4);
            content.append(verse(number, {text}));
        }
        chapters.append(chapter(chapterNumber, content));
    }
    return write(path, complete(id, {book("JHN", 1, chapters, "John")}));
}
inline bool psalm(const QString &path)
{
    return write(path, complete("engpsa", {book("PSA", 1, {chapter(119, {
        QJsonObject{{"type", "heading"}, {"content", QJsonArray{QString::fromUtf8("א Aleph")}}},
        verse(1, {QString::fromUtf8("אשרי Blessed are those whose way is blameless.")}),
        verse(2, {"Blessed are those who keep his testimonies."}),
        QJsonObject{{"type", "heading"}, {"content", QJsonArray{QString::fromUtf8("ב Beth")}}},
        verse(3, {QString::fromUtf8("ב Beth begins with Hebrew too.")})
    })}, "Psalms")}, "Acrostic fixture"));
}
inline bool studyEdition(const QString &path, const QString &revision = {})
{
    QJsonArray books;
    const QStringList codes{"GEN", "MAT", "JHN"};
    for (const auto &code : codes) {
        QJsonArray chapters;
        for (int c = 1; c <= (code == "JHN" ? 3 : 1); ++c) {
            QJsonArray content{QJsonObject{{"type", "heading"}, {"content", QJsonArray{"Study fixture heading"}}}}, notes;
            for (int v = 1; v <= (code == "GEN" ? 31 : code == "MAT" ? 25 : 36); ++v) {
                QJsonArray pieces{QString("Original study text %1:%2 with enough ordinary words to wrap across a reading line.").arg(c).arg(v)};
                auto note = [&](int id, const QString &caller, const QString &text) {
                    notes.append(QJsonObject{{"noteId", id}, {"caller", caller}, {"text", text}});
                    return QJsonObject{{"noteId", id}};
                };
                if (code == "GEN" && v == 1) pieces = {"In the beginning of the original fixture.", note(0, "+", "1:1 A Genesis note")};
                if (code == "JHN" && c == 1 && v == 1) pieces = {"In the beginning was the Word. Original words follow."};
                if (code == "JHN" && c == 3 && v == 3) pieces = {"Jesus answered,", note(0, "+", "3:3 Or from above"), " ", QJsonObject{{"text", "Truly I tell you, unless one is born again"}, {"wordsOfJesus", true}}, " he cannot see."};
                if (code == "JHN" && c == 3 && v == 16) pieces = {"For God so loved", note(1, "a", "Greek agapao"), " the world that he gave", note(2, "b", "Or only begotten"), " his Son."};
                content.append(verse(v, pieces));
            }
            chapters.append(chapter(c, content, notes));
        }
        books.append(book(code, books.size() + 1, chapters));
    }
    auto root = complete("study", books, "Original study fixture");
    auto translation = root["translation"].toObject(); translation["shortName"] = "OSF";
    if (!revision.isEmpty()) translation["sha256"] = revision;
    root["translation"] = translation;
    return write(path, root);
}
}
