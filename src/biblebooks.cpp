#include "biblebooks.h"
#include <QRegularExpression>

namespace BibleBooks {
const QVector<Book> &canon()
{
    static const QVector<Book> books = {
        {"GEN","Genesis","gen ge gn"}, {"EXO","Exodus","exod exo ex"},
        {"LEV","Leviticus","lev le lv"}, {"NUM","Numbers","num nu nm nb"},
        {"DEU","Deuteronomy","deut deu dt"}, {"JOS","Joshua","josh jos jsh"},
        {"JDG","Judges","judg jdg jdgs"}, {"RUT","Ruth","rth ru"},
        {"1SA","1 Samuel","1sam 1sa 1sm"}, {"2SA","2 Samuel","2sam 2sa 2sm"},
        {"1KI","1 Kings","1kgs 1ki 1kin"}, {"2KI","2 Kings","2kgs 2ki 2kin"},
        {"1CH","1 Chronicles","1chron 1chr 1ch"}, {"2CH","2 Chronicles","2chron 2chr 2ch"},
        {"EZR","Ezra","ezr"}, {"NEH","Nehemiah","neh ne"}, {"EST","Esther","esth est es"},
        {"JOB","Job","jb"}, {"PSA","Psalms","psalm ps psa psm pss"},
        {"PRO","Proverbs","prov pro pr prv"}, {"ECC","Ecclesiastes","eccles eccl ecc ec qoheleth"},
        {"SNG","Song of Solomon","songofsolomon songofsongs song sos so canticles cant"},
        {"ISA","Isaiah","isa is"}, {"JER","Jeremiah","jer je jr"},
        {"LAM","Lamentations","lam la"}, {"EZK","Ezekiel","ezek eze ezk"},
        {"DAN","Daniel","dan da dn"}, {"HOS","Hosea","hos ho"}, {"JOL","Joel","jl"},
        {"AMO","Amos","am"}, {"OBA","Obadiah","obad ob"}, {"JON","Jonah","jon jnh"},
        {"MIC","Micah","mic mi"}, {"NAM","Nahum","nah na"}, {"HAB","Habakkuk","hab hb"},
        {"ZEP","Zephaniah","zeph zep zp"}, {"HAG","Haggai","hag hg"},
        {"ZEC","Zechariah","zech zec zc"}, {"MAL","Malachi","mal ml"},
        {"MAT","Matthew","matt mat mt"}, {"MRK","Mark","mrk mar mk mr"},
        {"LUK","Luke","luk lk lu"}, {"JHN","John","joh jhn jn"},
        {"ACT","Acts","act ac"}, {"ROM","Romans","rom ro rm"},
        {"1CO","1 Corinthians","1cor 1co"}, {"2CO","2 Corinthians","2cor 2co"},
        {"GAL","Galatians","gal ga"}, {"EPH","Ephesians","eph ep"},
        {"PHP","Philippians","phil php pp"}, {"COL","Colossians","col co"},
        {"1TH","1 Thessalonians","1thess 1thes 1th"}, {"2TH","2 Thessalonians","2thess 2thes 2th"},
        {"1TI","1 Timothy","1tim 1ti 1tm"}, {"2TI","2 Timothy","2tim 2ti 2tm"},
        {"TIT","Titus","tit ti"}, {"PHM","Philemon","philem phlm phm"},
        {"HEB","Hebrews","heb he"}, {"JAS","James","jas jam jm"},
        {"1PE","1 Peter","1pet 1pe 1pt"}, {"2PE","2 Peter","2pet 2pe 2pt"},
        {"1JN","1 John","1joh 1jhn 1jn"}, {"2JN","2 John","2joh 2jhn 2jn"},
        {"3JN","3 John","3joh 3jhn 3jn"}, {"JUD","Jude","jud jd"},
        {"REV","Revelation","revelations rev re rv apocalypse"},
        {"TOB","Tobit","tob tobias"}, {"JDT","Judith","jdt jth"},
        {"ESG","Esther (Greek)","greekesther additionstoesther esthergreek"},
        {"WIS","Wisdom of Solomon","wis wisdom"}, {"SIR","Sirach","sir ecclesiasticus"},
        {"BAR","Baruch","bar"}, {"S3Y","Song of the Three Holy Children","prayerofazariah songofthree s3y"},
        {"SUS","Susanna","sus"}, {"BEL","Bel and the Dragon","bel"},
        {"1MA","1 Maccabees","1mac 1macc 1ma"}, {"2MA","2 Maccabees","2mac 2macc 2ma"},
        {"1ES","1 Esdras","1esd 1es"}, {"MAN","Prayer of Manasseh","man manasseh"},
        {"2ES","2 Esdras","2esd 2es"}
    };
    return books;
}

int bookId(QString name)
{
    name = name.toLower().trimmed();
    static const QRegularExpression ordinal("^(first|second|third|iii|ii|i)\\s+");
    const auto match = ordinal.match(name);
    if (match.hasMatch()) {
        const QString prefix = match.captured(1);
        name.replace(0, match.capturedLength(), prefix == "first" || prefix == "i" ? "1"
                     : prefix == "second" || prefix == "ii" ? "2" : "3");
    }
    static const QRegularExpression spacing("[\\s.]+");
    name.remove(spacing);
    for (int i = 0; i < canon().size(); ++i) {
        const auto &book = canon()[i];
        if (name == QString::fromLatin1(book.code).toLower()
            || name == QString::fromUtf8(book.name).toLower().remove(spacing)
            || QString::fromLatin1(book.aliases).split(' ').contains(name)) return i + 1;
    }
    return 0;
}
}
