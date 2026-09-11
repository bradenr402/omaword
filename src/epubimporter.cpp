#include "epubimporter.h"
#include "brand.h"

#include <archive.h>
#include <archive_entry.h>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryFile>
#include <QUuid>
#include <QVariant>
#include <QXmlStreamReader>
#include <stdexcept>

namespace EpubImporter {
bool stripLayoutPilcrow(QString *text, QString *html)
{
    static const QRegularExpression marker("^\\s*¶(?:\\s+|$)");
    const auto match = marker.match(*text);
    if (!match.hasMatch()) return false;
    text->remove(0, match.capturedLength());
    if (html) {
        static const QRegularExpression htmlMarker("^(\\s*(?:(?:<i>|<b>)\\s*)*)(?:¶|&para;|&#182;|&#x[bB]6;)(?:\\s+|(?=<|$))");
        const auto rendered = htmlMarker.match(*html);
        if (rendered.hasMatch()) html->replace(0, rendered.capturedLength(), rendered.captured(1));
    }
    return true;
}

// Numeric metadata derived from the trusted NETBIBLE21.epub's embedded markers,
// not document titles. Independently checked against standard KJV maxima:
// NET differs at 2 Cor 13 (13), 3 John 1 (15), and Rev 12 (18).
// See tests/README.md for provenance and the NET numbering checks.
int chapterMaximum(int book, int chapter)
{
    static const QVector<QVector<int>> maxima = {
        {31,25,24,26,32,22,24,22,29,32,32,20,18,24,21,16,27,33,38,18,34,24,20,67,34,35,46,22,35,43,55,32,20,31,29,43,36,30,23,23,57,38,34,34,28,34,31,22,33,26},
        {22,25,22,31,23,30,25,32,35,29,10,51,22,31,27,36,16,27,25,26,36,31,33,18,40,37,21,43,46,38,18,35,23,35,35,38,29,31,43,38},
        {17,16,17,35,19,30,38,36,24,20,47,8,59,57,33,34,16,30,37,27,24,33,44,23,55,46,34},
        {54,34,51,49,31,27,89,26,23,36,35,16,33,45,41,50,13,32,22,29,35,41,30,25,18,65,23,31,40,16,54,42,56,29,34,13},
        {46,37,29,49,33,25,26,20,29,22,32,32,18,29,23,22,20,22,21,20,23,30,25,22,19,19,26,68,29,20,30,52,29,12},
        {18,24,17,24,15,27,26,35,27,43,23,24,33,15,63,10,18,28,51,9,45,34,16,33},
        {36,23,31,24,31,40,25,35,57,18,40,15,25,20,20,31,13,31,30,48,25},
        {22,23,18,22},
        {28,36,21,22,12,21,17,22,27,27,15,25,23,52,35,23,58,30,24,42,15,23,29,22,44,25,12,25,11,31,13},
        {27,32,39,12,25,23,29,18,13,19,27,31,39,33,37,23,29,33,43,26,22,51,39,25},
        {53,46,28,34,18,38,51,66,28,29,43,33,34,31,34,34,24,46,21,43,29,53},
        {18,25,27,44,27,33,20,29,37,36,21,21,25,29,38,20,41,37,37,21,26,20,37,20,30},
        {54,55,24,43,26,81,40,40,44,14,47,40,14,17,29,43,27,17,19,8,30,19,32,31,31,32,34,21,30},
        {17,18,17,22,14,42,22,18,31,19,23,16,22,15,19,14,19,34,11,37,20,12,21,27,28,23,9,27,36,27,21,33,25,33,27,23},
        {11,70,13,24,17,22,28,36,15,44},
        {11,20,32,23,19,19,73,18,38,39,36,47,31},
        {22,23,15,17,14,14,10,17,32,3},
        {22,13,26,21,27,30,21,22,35,22,20,25,28,22,35,22,16,21,29,29,34,30,17,25,6,14,23,28,25,31,40,22,33,37,16,33,24,41,30,24,34,17},
        {6,12,8,8,12,10,17,9,20,18,7,8,6,7,5,11,15,50,14,9,13,31,6,10,22,12,14,9,11,12,24,11,22,22,28,12,40,22,13,17,13,11,5,26,17,11,9,14,20,23,19,9,6,7,23,13,11,11,17,12,8,12,11,10,13,20,7,35,36,5,24,20,28,23,10,12,20,72,13,19,16,8,18,12,13,17,7,18,52,17,16,15,5,23,11,13,12,9,9,5,8,28,22,35,45,48,43,13,31,7,10,10,9,8,18,19,2,29,176,7,8,9,4,8,5,6,5,6,8,8,3,18,3,3,21,26,9,8,24,13,10,7,12,15,21,10,20,14,9,6},
        {33,22,35,27,23,35,27,36,18,32,31,28,25,35,33,33,28,24,29,30,31,29,35,34,28,28,27,28,27,33,31},
        {18,26,22,16,20,12,29,17,18,20,10,14},
        {17,17,11,16,16,13,13,14},
        {31,22,26,6,30,13,25,22,21,34,16,6,22,32,9,14,14,7,25,6,17,25,18,23,12,21,13,29,24,33,9,20,24,17,10,22,38,22,8,31,29,25,28,28,25,13,15,22,26,11,23,15,12,17,13,12,21,14,21,22,11,12,19,12,25,24},
        {19,37,25,31,31,30,34,22,26,25,23,17,27,22,21,21,27,23,15,18,14,30,40,10,38,24,22,17,32,24,40,44,26,22,19,32,21,28,18,16,18,22,13,30,5,28,7,47,39,46,64,34},
        {22,22,66,22,22},
        {28,10,27,17,17,14,27,18,11,22,25,28,23,23,8,63,24,32,14,49,32,31,49,27,17,21,36,26,21,26,18,32,33,31,15,38,28,23,29,49,26,20,27,31,25,24,23,35},
        {21,49,30,37,31,28,28,27,27,21,45,13},
        {11,23,5,19,15,11,16,14,17,15,12,14,16,9},
        {20,32,21}, {15,16,15,13,27,14,17,14,15}, {21}, {17,10,10,11},
        {16,13,12,13,15,16,20}, {15,13,19}, {17,20,19}, {18,15,20}, {15,23},
        {21,13,10,14,11,15,14,23,17,12,17,14,9,21}, {14,17,18,6},
        {25,23,17,25,48,34,29,34,38,42,30,50,58,36,39,28,27,35,30,34,46,46,39,51,46,75,66,20},
        {45,28,35,41,43,56,37,38,50,52,33,44,37,72,47,20},
        {80,52,38,44,39,49,50,56,62,42,54,59,35,35,32,31,37,43,48,47,38,71,56,53},
        {51,25,36,54,47,71,53,59,41,42,57,50,38,31,27,33,26,40,42,31,25},
        {26,47,26,37,42,15,60,40,43,48,30,25,52,28,41,40,34,28,41,38,40,30,35,27,27,32,44,31},
        {32,29,31,25,21,23,25,39,33,21,36,21,14,23,33,27},
        {31,16,23,21,13,20,40,13,27,33,34,31,13,40,58,24},
        {24,17,18,18,21,18,16,24,15,18,33,21,13},
        {24,21,29,31,26,18}, {23,22,21,32,33,24}, {30,30,21,23}, {29,23,25,18},
        {10,20,13,18,28}, {12,17,18}, {20,15,16,16,25,21}, {18,26,17,22},
        {16,15,15}, {25}, {14,18,19,16,14,20,28,13,28,39,40,29,25},
        {27,26,18,17,20}, {25,25,22,19,14}, {21,22,18}, {10,29,24,21,21},
        {13}, {15}, {25}, {20,29,22,11,14,17,17,13,21,11,19,18,18,20,8,21,18,24,21,15,27,21}
    };
    if (book < 1 || book > maxima.size() || chapter < 1 || chapter > maxima[book-1].size()) return 0;
    return maxima[book-1][chapter-1];
}

int chapterVerseCount(int book, int chapter)
{
    const int maximum = chapterMaximum(book,chapter);
    int count = maximum;
    for (int verse = 1; verse <= maximum; ++verse) if (omittedVerse(book,chapter,verse)) --count;
    return count;
}

const QVector<Book> &canon()
{
    static const QVector<Book> value = {
        {"Genesis",50,1533,"gen ge gn"}, {"Exodus",40,1213,"exod exo ex"},
        {"Leviticus",27,859,"lev le lv"}, {"Numbers",36,1288,"num nu nm nb"},
        {"Deuteronomy",34,959,"deut deu dt"}, {"Joshua",24,658,"josh jos jsh"},
        {"Judges",21,618,"judg jdg jdgs"}, {"Ruth",4,85,"rth ru"},
        {"1 Samuel",31,810,"1sam 1sa 1sm"}, {"2 Samuel",24,695,"2sam 2sa 2sm"},
        {"1 Kings",22,816,"1kgs 1ki 1kin"}, {"2 Kings",25,719,"2kgs 2ki 2kin"},
        {"1 Chronicles",29,942,"1chron 1chr 1ch"}, {"2 Chronicles",36,822,"2chron 2chr 2ch"},
        {"Ezra",10,280,"ezr"}, {"Nehemiah",13,406,"neh ne"}, {"Esther",10,167,"esth est es"},
        {"Job",42,1070,"jb"}, {"Psalms",150,2461,"psalm ps psa psm pss"},
        {"Proverbs",31,915,"prov pro pr prv"}, {"Ecclesiastes",12,222,"eccles eccl ecc ec qoheleth"},
        {"Song of Solomon",8,117,"songofsolomon songofsongs song sos so canticles cant"},
        {"Isaiah",66,1292,"isa is"}, {"Jeremiah",52,1364,"jer je jr"},
        {"Lamentations",5,154,"lam la"}, {"Ezekiel",48,1273,"ezek eze ezk"},
        {"Daniel",12,357,"dan da dn"}, {"Hosea",14,197,"hos ho"}, {"Joel",3,73,"jl"},
        {"Amos",9,146,"am"}, {"Obadiah",1,21,"obad ob"}, {"Jonah",4,48,"jon jnh"},
        {"Micah",7,105,"mic mi"}, {"Nahum",3,47,"nah na"}, {"Habakkuk",3,56,"hab hb"},
        {"Zephaniah",3,53,"zeph zep zp"}, {"Haggai",2,38,"hag hg"},
        {"Zechariah",14,211,"zech zec zc"}, {"Malachi",4,55,"mal ml"},
        {"Matthew",28,1068,"matt mat mt"}, {"Mark",16,673,"mrk mar mk mr"},
        {"Luke",24,1149,"luk lk lu"}, {"John",21,878,"joh jhn jn"},
        {"Acts",28,1003,"act ac"}, {"Romans",16,432,"rom ro rm"},
        {"1 Corinthians",16,437,"1cor 1co"}, {"2 Corinthians",13,256,"2cor 2co"},
        {"Galatians",6,149,"gal ga"}, {"Ephesians",6,155,"eph ep"},
        {"Philippians",4,104,"phil php pp"}, {"Colossians",4,95,"col co"},
        {"1 Thessalonians",5,89,"1thess 1thes 1th"}, {"2 Thessalonians",3,47,"2thess 2thes 2th"},
        {"1 Timothy",6,113,"1tim 1ti 1tm"}, {"2 Timothy",4,83,"2tim 2ti 2tm"},
        {"Titus",3,46,"tit ti"}, {"Philemon",1,25,"philem phlm phm"},
        {"Hebrews",13,303,"heb he"}, {"James",5,108,"jas jam jm"},
        {"1 Peter",5,105,"1pet 1pe 1pt"}, {"2 Peter",3,61,"2pet 2pe 2pt"},
        {"1 John",5,105,"1joh 1jhn 1jn"}, {"2 John",1,13,"2joh 2jhn 2jn"},
        {"3 John",1,15,"3joh 3jhn 3jn"}, {"Jude",1,25,"jud jd"},
        {"Revelation",22,405,"revelations rev re rv apocalypse"}
    };
    return value;
}

int bookId(QString name)
{
    name = name.toLower().trimmed();
    static const QRegularExpression ordinal("^(first|second|third|iii|ii|i)\\s+");
    const auto match = ordinal.match(name);
    if (match.hasMatch()) {
        const auto prefix = match.captured(1);
        const int n = (prefix == "first" || prefix == "i") ? 1 : (prefix == "second" || prefix == "ii") ? 2 : 3;
        name.replace(0, match.capturedLength(), QString::number(n));
    }
    name.remove(QRegularExpression("[\\s.]+"));
    for (int i = 0; i < canon().size(); ++i) {
        QString full = QString::fromUtf8(canon()[i].name).toLower();
        full.remove(' ');
        if (name == full || QString::fromLatin1(canon()[i].aliases).split(' ').contains(name))
            return i + 1;
    }
    return 0;
}

bool omittedVerse(int b, int c, int v)
{
    static const QSet<QString> omitted = {"40:17:21", "40:18:11", "40:23:14", "41:7:16",
        "41:9:44", "41:9:46", "41:11:26", "41:15:28", "42:17:36", "42:23:17",
        "43:5:4", "44:8:37", "44:15:34", "44:24:7", "44:28:29", "45:16:24"};
    return omitted.contains(QString("%1:%2:%3").arg(b).arg(c).arg(v));
}

namespace {
[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
constexpr qint64 MaximumVerseOutput = 64 * 1024;
constexpr qint64 MaximumDocumentOutput = 2 * 1024 * 1024;
constexpr qint64 MaximumGlobalOutput = 64 * 1024 * 1024;

// Preflight every XML token, including subtrees later skipped by the renderer.
// This bounds nesting/entity expansion before readElementText/skipCurrentElement
// can hide it. Limits are UTF-16 code units, not compressed archive bytes.
void checkXmlResources(const QByteArray &data, const QString &path, qint64 &global)
{
    QXmlStreamReader xml(data);
    xml.setEntityExpansionLimit(1024);
    int depth = 0, formatting = 0, elements = 0;
    qint64 text = 0;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            if (++depth > 64 || ++elements > 100000) fail("EPUB XML depth or element limit exceeded: " + path);
            if ((xml.name() == u"b" || xml.name() == u"i") && ++formatting > 16)
                fail("EPUB formatting depth limit exceeded: " + path);
        } else if (xml.isEndElement()) {
            --depth;
            if (xml.name() == u"b" || xml.name() == u"i") --formatting;
        } else if (xml.isCharacters()) {
            text += xml.text().size(); global += xml.text().size();
            if (text > MaximumDocumentOutput || global > MaximumGlobalOutput)
                fail("EPUB expanded XML text limit exceeded: " + path);
        }
    }
    if (xml.hasError()) fail("Invalid XML in " + path + ": " + xml.errorString());
}
struct Connection {
    QString name = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSqlDatabase db;
    Connection(const QString &path, bool readOnly = false) {
        db = QSqlDatabase::addDatabase("QSQLITE", name);
        db.setDatabaseName(path);
        db.setConnectOptions(readOnly ? "QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=5000" : "QSQLITE_BUSY_TIMEOUT=5000");
        if (!db.open()) {
            const QString error = db.lastError().text();
            db = QSqlDatabase();
            QSqlDatabase::removeDatabase(name);
            fail("Cannot open Bible database: " + error);
        }
    }
    ~Connection() { db.close(); db = QSqlDatabase(); QSqlDatabase::removeDatabase(name); }
};
void exec(QSqlQuery &q, const QString &sql) { if (!q.exec(sql)) fail("Bible database: " + q.lastError().text()); }
void checkXml(QXmlStreamReader &xml, const QString &path) {
    if (xml.hasError()) fail(QString("Invalid XML in %1 (line %2): %3").arg(path).arg(xml.lineNumber()).arg(xml.errorString()));
}
QString archivePath(QString path) {
    if (path.startsWith('/') || path.contains('\\') || path.split('/').contains(".."))
        fail("The EPUB contains an unsafe entry path.");
    return QDir::cleanPath(path);
}
QHash<QString, QByteArray> readArchive(const QString &path, const QSet<QString> &selected = {})
{
    std::unique_ptr<archive, decltype(&archive_read_free)> a(archive_read_new(), archive_read_free);
    archive_read_support_format_zip(a.get());
    archive_read_support_filter_none(a.get());
    if (archive_read_open_filename(a.get(), QFile::encodeName(path).constData(), 65536) != ARCHIVE_OK)
        fail("Cannot read EPUB: " + QString::fromUtf8(archive_error_string(a.get())));
    QHash<QString, QByteArray> entries;
    archive_entry *entry = nullptr;
    qint64 total = 0, expandedXml = 0;
    int count = 0, rc;
    while ((rc = archive_read_next_header(a.get(), &entry)) == ARCHIVE_OK) {
        if (++count > 10000) fail("The EPUB contains too many entries.");
        const char *raw = archive_entry_pathname_utf8(entry);
        if (!raw) raw = archive_entry_pathname(entry);
        if (!raw) fail("The EPUB contains an unnamed entry.");
        const QString name = archivePath(QString::fromUtf8(raw));
        if (archive_entry_filetype(entry) == AE_IFDIR) continue;
        if (archive_entry_filetype(entry) != AE_IFREG || archive_entry_is_encrypted(entry) == 1)
            fail("Encrypted or non-file EPUB entries are not supported.");
        // Read metadata first, then only selected spine documents. In particular,
        // OCR page books and large navigation indexes need no body scan to reject.
        if (!(name.endsWith(".opf") || name == "META-INF/container.xml" || name == "mimetype"
              || selected.contains(name))) {
            archive_read_data_skip(a.get());
            continue;
        }
        if (entries.contains(name)) fail("The EPUB contains duplicate entries: " + name);
        QByteArray data;
        char buffer[32768];
        la_ssize_t size;
        while ((size = archive_read_data(a.get(), buffer, sizeof(buffer))) > 0) {
            total += size;
            if (data.size() + size > 8 * 1024 * 1024 || total > 128 * 1024 * 1024)
                fail("The EPUB exceeds the supported uncompressed size.");
            data.append(buffer, size);
        }
        if (size < 0) fail("Damaged EPUB entry: " + name);
        if (name != "mimetype") {
            data.replace("&nbsp;", "&#160;");
            checkXmlResources(data,name,expandedXml);
        }
        entries.insert(name, data);
    }
    if (rc != ARCHIVE_EOF) fail("The EPUB archive is damaged.");
    if (entries.value("mimetype").trimmed() != "application/epub+zip") fail("Choose a valid EPUB file.");
    return entries;
}

struct Package {
    Edition edition;
    QStringList paths;
    QString identifier;
};

const QString MarkerHelp = "EPUB lacks reliable book/chapter/verse markers. Page-based OCR may have lost reading order. Use a verse-tagged EPUB with explicit book, chapter and verse markers.";

QString abbreviation(const QString &title)
{
    const QList<QPair<QString, QString>> names = {
        {"NKJV", "new king james"}, {"KJV", "king james|authorized version"},
        {"NIV", "new international version"}, {"WEB", "world english bible"}};
    for (const auto &name : names) {
        const QRegularExpression pattern("\\b(?:" + name.first + '|' + name.second + ")\\b",
                                         QRegularExpression::CaseInsensitiveOption);
        if (pattern.match(title).hasMatch()) return name.first;
    }
    QString initials;
    const auto words = title.split(QRegularExpression("[^\\p{L}\\p{N}]+"), Qt::SkipEmptyParts);
    for (const auto &word : words) initials += word.left(1).toUpper();
    return initials.left(12);
}

QString editionId(const QString &identifier, const QString &title)
{
    const QByteArray identity = (identifier.isEmpty() ? "title:" + title.toCaseFolded() : "identifier:" + identifier).toUtf8();
    return "epub-" + QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex().left(32));
}

QString metadataDigest(const QHash<QString, QString> &metadata)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const auto &key : {"version_id", "abbreviation", "edition", "copyright", "source_identifier", "source_keys", "versification"}) {
        const auto value = metadata.value(key).toUtf8();
        hash.addData(QByteArray::number(value.size()) + ':' + value);
    }
    return QString::fromLatin1(hash.result().toHex());
}

// Haiola's internal IDs are not its three-letter filenames (nor NET aliases).
const QStringList &haiolaPrefixes()
{
    static const QStringList prefixes = QString("GN EX LV NU DT JS JG RT S1 S2 K1 K2 R1 R2 ER NH ET JB PS PR EC SS IS JR LM EK DN HS JL AM OB JH MC NM HK ZP HG ZC ML MT MK LK JN AC RM C1 C2 GL EP PP CL H1 H2 T1 T2 TT PM HB JM P1 P2 J1 J2 J3 JD RV").split(' ');
    return prefixes;
}

int kjvMaximum(int book, int chapter)
{
    if (book == 47 && chapter == 13) return 14;
    if (book == 64 && chapter == 1) return 14;
    if (book == 66 && chapter == 12) return 17;
    return chapterMaximum(book, chapter);
}

Package package(const QHash<QString, QByteArray> &entries)
{
    QXmlStreamReader container(entries.value("META-INF/container.xml"));
    QString opf;
    while (!container.atEnd()) {
        container.readNext();
        if (container.isStartElement() && container.name() == u"rootfile" && opf.isEmpty())
            opf = archivePath(container.attributes().value("full-path").toString());
    }
    checkXml(container, "META-INF/container.xml");
    if (opf.isEmpty() || !entries.contains(opf)) fail("The EPUB package manifest is missing.");
    QXmlStreamReader xml(entries.value(opf));
    QHash<QString, QString> manifest;
    QStringList ids;
    QString title, identifier, rights, accessibility, uniqueIdentifier;
    QHash<QString, QString> identifiers;
    QStringList identifierValues;
    int titles = 0;
    const QString base = QFileInfo(opf).path();
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement()) continue;
        if (xml.name() == u"package") uniqueIdentifier = xml.attributes().value("unique-identifier").toString();
        else if (xml.name() == u"title") { title = xml.readElementText().simplified(); ++titles; }
        else if (xml.name() == u"identifier") {
            const auto id = xml.attributes().value("id").toString();
            const auto value = xml.readElementText().trimmed();
            if (!id.isEmpty() && identifiers.contains(id)) fail("Ambiguous EPUB identifier metadata.");
            if (!id.isEmpty()) identifiers.insert(id, value);
            if (!value.isEmpty()) identifierValues.append(value);
        }
        else if (xml.name() == u"rights") rights = xml.readElementText();
        else if (xml.name() == u"meta" && xml.attributes().value("property") == u"schema:accessibilitySummary")
            accessibility += xml.readElementText();
        else if (xml.name() == u"item") {
            const auto attrs = xml.attributes();
            const QString id = attrs.value("id").toString();
            if (manifest.contains(id)) fail("Duplicate EPUB manifest identifier.");
            manifest.insert(id, archivePath(base + '/' + attrs.value("href").toString()));
        } else if (xml.name() == u"itemref") ids.append(xml.attributes().value("idref").toString());
    }
    checkXml(xml, opf);
    Package result;
    if (!uniqueIdentifier.isEmpty()) {
        identifier = identifiers.value(uniqueIdentifier);
        if (identifier.isEmpty()) fail("Incomplete EPUB metadata: the package identifier is missing.");
    } else {
        identifierValues.removeDuplicates();
        if (identifierValues.size() > 1) fail("Ambiguous EPUB identifier metadata: no unique package identifier.");
        if (!identifierValues.isEmpty()) identifier = identifierValues.first();
    }
    if (titles != 1 || title.isEmpty() || title.size() > 512 || abbreviation(title).isEmpty()) {
        if (accessibility.contains("character recognition", Qt::CaseInsensitive) || accessibility.contains("OCR", Qt::CaseInsensitive))
            fail(MarkerHelp);
        fail("Incomplete or ambiguous EPUB metadata: one nonempty edition title is required.");
    }
    if (identifier.size() > 4096) fail("EPUB identifier metadata is too long.");
    result.identifier = identifier;
    if (title == "NET Bible 2.1" && identifier == "urn:uuid:NETBIBLE21")
        result.edition = {"net-2.1", "NET", "NET Bible 2.1", rights};
    else if (title == "King James (Authorized) Version"
             && identifier == "urn:uuid:51004fba-2cea-50c8-8e37-fcebe15c315a")
        result.edition = {"kjv-2006", "KJV", "King James (Authorized) Version", rights};
    else
        result.edition = {editionId(identifier, title), abbreviation(title), title, rights};
    QSet<QString> seen;
    for (const auto &id : ids) {
        if (!manifest.contains(id)) fail("The EPUB spine references a missing manifest item.");
        const QString path = manifest.value(id);
        if (path.endsWith("_notes.xhtml")) continue;
        if (!path.endsWith(".xhtml") && !path.endsWith(".html")) fail("Unsupported EPUB spine document: " + path);
        if (seen.contains(path)) fail("The EPUB spine contains duplicate documents.");
        seen.insert(path);
        result.paths.append(path);
    }
    if (result.paths.isEmpty()) fail("The EPUB reading order is empty.");
    if (result.edition.id != "net-2.1" && result.paths.size() > 256) fail(MarkerHelp);
    return result;
}

struct Verse {
    int book = 0, chapter = 0, number = 0;
    QString text, html, heading;
    bool poetry = false, paragraphStart = false;
};

// Only known body paragraphs are Scripture. Superscripts, editorial headings,
// acrostic labels and speaker labels cannot leak into the verse text.
void parseDocument(QByteArray data, const QString &path, QSqlQuery &insert, QSet<QString> &documents, qint64 &globalOutput)
{
    data.replace("&nbsp;", "&#160;");
    QXmlStreamReader xml(data);
    int book = 0;
    bool bodyParagraph = false, poetry = false, paragraphStart = false;
    QString pendingHeading;
    QStringList styles;
    Verse verse;
    qint64 documentOutput = 0, verseOutput = 0;
    auto charge = [&](qint64 amount, bool scripture = true) {
        if (amount > MaximumDocumentOutput - documentOutput || amount > MaximumGlobalOutput - globalOutput
            || (scripture && amount > MaximumVerseOutput - verseOutput))
            fail("EPUB expanded output limit exceeded: " + path);
        documentOutput += amount; globalOutput += amount;
        if (scripture) verseOutput += amount;
    };
    auto flush = [&] {
        if (!verse.number) return;
        verse.text = verse.text.trimmed();
        verse.html = verse.html.trimmed();
        verse.text.replace(QRegularExpression(" *\\n *"), "\n");
        verse.html.replace(QRegularExpression(" *(?:<br/> *)+$"), "");
        // Acts 24:7 has an empty verse marker in the publisher's EPUB.
        // Omitted text must not become an invented or blank Scripture row.
        if (verse.text.isEmpty() && omittedVerse(verse.book, verse.chapter, verse.number)) {
            // An empty marker cannot consume layout intended for retained text.
            // Later headings may already be pending: keep their source order.
            paragraphStart = paragraphStart || verse.paragraphStart;
            if (!verse.heading.isEmpty()) {
                const int separator = pendingHeading.isEmpty() ? 0 : 1;
                if (verse.heading.size() + pendingHeading.size() + separator > MaximumVerseOutput)
                    fail("EPUB heading output limit exceeded: " + path);
                pendingHeading = verse.heading + (separator ? "\n" + pendingHeading : QString());
            }
            verse = Verse();
            verseOutput = 0;
            return;
        }
        if (verse.text.isEmpty()) fail("Empty verse in " + path);
        insert.bindValue(0, verse.book); insert.bindValue(1, verse.chapter); insert.bindValue(2, verse.number);
        insert.bindValue(3, verse.text); insert.bindValue(4, verse.html);
        insert.bindValue(5, verse.heading.isNull() ? QString("") : verse.heading); insert.bindValue(6, verse.poetry);
        insert.bindValue(7, verse.paragraphStart);
        if (!insert.exec()) fail("Duplicate or invalid verse in " + path + ": " + insert.lastError().text());
        verse = Verse();
        verseOutput = 0;
    };
    auto lineBreak = [&] {
        if (verse.number && !verse.text.trimmed().isEmpty() && !verse.text.endsWith('\n')) {
            charge(6);
            verse.text = verse.text.trimmed() + '\n';
            verse.html = verse.html.trimmed() + "<br/>";
        }
    };
    auto headingText = [&]() {
        QString text;
        int depth = 1;
        while (depth && !xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement()) {
                if (xml.name() == u"sup") xml.skipCurrentElement();
                else ++depth;
            } else if (xml.isEndElement()) --depth;
            else if (xml.isCharacters()) {
                const auto amount = xml.text().size();
                if (amount > MaximumVerseOutput - text.size()) fail("EPUB heading output limit exceeded: " + path);
                charge(amount,false);
                text += xml.text();
            }
        }
        return text.simplified();
    };
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const QString tag = xml.name().toString();
            const QString cl = xml.attributes().value("class").toString();
            if (tag == "title") {
                const auto title = xml.readElementText();
                const auto match = QRegularExpression("^NET Bible 2\\.1 (.+) (\\d+)$").match(title);
                if (!match.hasMatch()) return; // front matter/book chapter index
                book = bookId(match.captured(1));
                const int chapter = match.captured(2).toInt();
                if (!book || chapter < 1 || chapter > canon()[book - 1].chapters) fail("Invalid chapter title in " + path);
                const QString key = QString("%1:%2").arg(book).arg(chapter);
                if (documents.contains(key)) fail("Duplicate chapter document: " + title);
                documents.insert(key);
            } else if (tag == "sup" || tag == "h1" || tag == "h2" || tag == "script" || tag == "style") {
                xml.skipCurrentElement();
            } else if (tag == "p") {
                if (cl == "paragraphtitle" || cl == "psasuper" || cl == "sosspeaker" || cl == "lamhebrew") {
                    const QString heading = headingText();
                    if (pendingHeading.size() + heading.size() + 1 > MaximumVerseOutput)
                        fail("EPUB heading output limit exceeded: " + path);
                    if (!pendingHeading.isEmpty()) pendingHeading += '\n';
                    pendingHeading += heading;
                    bodyParagraph = false;
                } else {
                    bodyParagraph = cl == "bodytext" || cl == "bodyblock" || cl == "quote" || cl.contains("poetry");
                    poetry = cl.contains("poetry");
                    paragraphStart = bodyParagraph;
                    if (!bodyParagraph) xml.skipCurrentElement();
                }
            } else if (tag == "span" && cl == "verse" && bodyParagraph) {
                flush();
                const QString marker = xml.readElementText().trimmed();
                const auto match = QRegularExpression("^(\\d+):(\\d+)$").match(marker);
                if (!match.hasMatch() || !book) fail("Invalid verse marker in " + path);
                verse.book = book; verse.chapter = match.captured(1).toInt(); verse.number = match.captured(2).toInt();
                if (verse.chapter < 1 || verse.chapter > canon()[book-1].chapters || verse.number < 1 || verse.number > chapterMaximum(book,verse.chapter))
                    fail("Out-of-range verse marker in " + path);
                verse.heading = pendingHeading; pendingHeading.clear(); verse.poetry = poetry;
                verse.paragraphStart = paragraphStart || verse.number == 1;
                paragraphStart = false;
                charge(verse.heading.size());
            } else if (tag == "br" && bodyParagraph) lineBreak();
            else if ((tag == "i" || tag == "b") && bodyParagraph) {
                if (styles.size() >= 16) fail("EPUB formatting depth limit exceeded: " + path);
                styles.append(tag);
            }
        } else if (xml.isEndElement()) {
            const QString tag = xml.name().toString();
            if (tag == "p" && bodyParagraph) { lineBreak(); bodyParagraph = false; }
            if ((tag == "i" || tag == "b") && !styles.isEmpty()) styles.removeLast();
        } else if (xml.isCharacters() && bodyParagraph && verse.number) {
            // Reserve a conservative escaped-size bound before allocating text,
            // HTML or wrappers. Counts cumulative output, not just live buffers.
            charge(7 * qint64(xml.text().size()) + 7 * styles.size());
            QString text = xml.text().toString();
            text.replace(QRegularExpression("\\s+"), " ");
            if (verse.text.isEmpty() || verse.text.endsWith('\n')) {
                while (text.startsWith(' ')) text.remove(0, 1);
            }
            verse.text += text;
            // A paragraph beginning inside a verse is an internal line break,
            // not a break before the next verse marker in that paragraph.
            if (!text.trimmed().isEmpty()) paragraphStart = false;
            QString html = text.toHtmlEscaped();
            for (auto it = styles.crbegin(); it != styles.crend(); ++it) html = '<' + *it + '>' + html + "</" + *it + '>';
            verse.html += html;
            if (!text.trimmed().isEmpty()) verse.poetry = verse.poetry || poetry;
        }
    }
    checkXml(xml, path);
    flush();
}

// Match one complete, independently defined numbering profile. A source digest
// alone cannot detect a lost final marker whose words joined the preceding verse.
// Profiles are selected by actual keys, never by translation title/abbreviation.
struct SourceKeys {
    QCryptographicHash hash{QCryptographicHash::Sha256};
    int count = 0, chapters = 0, lastBook = 0, lastChapter = 0, lastVerse = 0, chapterSize = 0;
    bool matchesKjv = true, matchesNet = true;
    void requireProfile() const {
        if (!matchesKjv && !matchesNet)
            fail("Verse numbering does not match supported complete NET 2.1 or KJV profiles. Check for missing or unsupported verse markers.");
    }
    void finishChapter() {
        if (!lastBook) return;
        matchesKjv = matchesKjv && lastVerse == kjvMaximum(lastBook,lastChapter)
            && chapterSize == kjvMaximum(lastBook,lastChapter);
        matchesNet = matchesNet && lastVerse == chapterMaximum(lastBook,lastChapter)
            && chapterSize == chapterVerseCount(lastBook,lastChapter);
        requireProfile();
    }
    void add(int b, int c, int v) {
        if (b < 1 || b > 66 || c < 1 || c > canon()[b-1].chapters || v < 1 || v > 176)
            fail("Out-of-range semantic verse marker.");
        if (b != lastBook || c != lastChapter) {
            finishChapter();
            if ((b == lastBook && c != lastChapter + 1)
                || (b != lastBook && (b != lastBook + 1 || c != 1
                    || (lastBook && lastChapter != canon()[lastBook-1].chapters))))
                fail("Missing chapters or decreasing canonical book/chapter markers.");
            ++chapters; lastVerse = 0; chapterSize = 0;
        }
        if (v <= lastVerse) fail("Duplicate or decreasing semantic verse marker.");
        if (v != lastVerse + 1) matchesKjv = false;
        for (int missing = lastVerse + 1; missing < v; ++missing)
            if (!omittedVerse(b,c,missing)) matchesNet = false;
        if (omittedVerse(b,c,v)) matchesNet = false;
        matchesKjv = matchesKjv && v <= kjvMaximum(b,c);
        matchesNet = matchesNet && v <= chapterMaximum(b,c);
        requireProfile();
        hash.addData(QString("%1:%2:%3\n").arg(b).arg(c).arg(v).toUtf8());
        ++count; ++chapterSize; lastBook = b; lastChapter = c; lastVerse = v;
    }
    QString finish() {
        finishChapter();
        if (lastBook != 66 || lastChapter != 22 || chapters != 1189)
            fail("Incomplete Bible: expected 66 books, 1,189 chapters and full verse-tagged text.");
        matchesKjv = matchesKjv && count == 31102;
        matchesNet = matchesNet && count == 31087;
        requireProfile();
        return QString::fromLatin1(hash.result().toHex());
    }
    QString profile() {
        finish();
        return matchesKjv ? "kjv-v1" : "net-2.1-v1";
    }
};

// Explicit book containers only. Common OSIS/USFM book codes are accepted
// alongside Haiola's own prefixes; no filename or prose/title inference.
int anchorBook(const QString &id)
{
    const int haiola = haiolaPrefixes().indexOf(id) + 1;
    if (haiola) return haiola;
    static const QStringList usfm = QString("GEN EXO LEV NUM DEU JOS JDG RUT 1SA 2SA 1KI 2KI 1CH 2CH EZR NEH EST JOB PSA PRO ECC SNG ISA JER LAM EZK DAN HOS JOL AMO OBA JON MIC NAM HAB ZEP HAG ZEC MAL MAT MRK LUK JHN ACT ROM 1CO 2CO GAL EPH PHP COL 1TH 2TH 1TI 2TI TIT PHM HEB JAS 1PE 2PE 1JN 2JN 3JN JUD REV").split(' ');
    const int standard = usfm.indexOf(id.toUpper()) + 1;
    return standard ? standard : bookId(id);
}

int structuredBook(const QByteArray &data, const QString &path)
{
    QXmlStreamReader xml(data);
    int book = 0;
    bool markers = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement()) continue;
        const auto attrs = xml.attributes();
        if (attrs.value("class") == u"verse") markers = true;
        if (xml.name() != u"div" || attrs.value("class") != u"main") continue;
        if (book) fail("Duplicate structured book container in " + path);
        QString id = attrs.value("id").toString();
        if (id.endsWith("0_0")) id.chop(3);
        book = anchorBook(id);
        if (!book) fail("Unknown semantic book prefix in " + path);
    }
    checkXml(xml,path);
    if (!book && markers) fail("Verse markers lack an explicit canonical book container: " + path);
    return markers ? book : 0;
}

void parseStructured(const QByteArray &data, const QString &path, QSqlQuery *insert,
              QSet<QString> &documents, qint64 &globalOutput, SourceKeys &keys, bool kjv)
{
    static const QRegularExpression lineWhitespace(" *\\n *"), trailingBreaks(" *(?:<br/> *)+$"), whitespace("\\s+");
    static const QRegularExpression osisMarker("^([1-3]?[A-Za-z]+)\\.([1-9][0-9]*)\\.([1-9][0-9]*)$");
    static const QRegularExpression markerLabel("^(?:[0-9]+(?::[0-9]+)?|[0-9]+\\.)?$");
    QRegularExpression haiolaMarker;
    QXmlStreamReader xml(data);
    int depth = 0, mainDepth = 0, paragraphDepth = 0, book = 0;
    bool poetry = false, paragraphStart = false;
    QString prefix, pendingHeading;
    QVector<int> italics;
    Verse verse;
    qint64 documentOutput = 0, verseOutput = 0;
    auto charge = [&](qint64 amount) {
        if (amount > MaximumVerseOutput - verseOutput || amount > MaximumDocumentOutput - documentOutput
            || amount > MaximumGlobalOutput - globalOutput)
            fail("EPUB expanded output limit exceeded: " + path);
        verseOutput += amount; documentOutput += amount; globalOutput += amount;
    };
    auto flush = [&] {
        if (!verse.number) return;
        verse.text = verse.text.trimmed();
        verse.text.replace(lineWhitespace, "\n");
        verse.html = verse.html.trimmed();
        verse.html.replace(trailingBreaks, "");
        if (stripLayoutPilcrow(&verse.text, &verse.html)) verse.paragraphStart = true;
        if (verse.text.isEmpty()) fail("Empty verse in " + path);
        keys.add(verse.book, verse.chapter, verse.number);
        if (insert) {
            insert->bindValue(0, verse.book); insert->bindValue(1, verse.chapter); insert->bindValue(2, verse.number);
            insert->bindValue(3, verse.text); insert->bindValue(4, verse.html);
            insert->bindValue(5, verse.heading.isNull() ? QString("") : verse.heading);
            insert->bindValue(6, verse.poetry);
            insert->bindValue(7, verse.paragraphStart);
            if (!insert->exec()) fail("Duplicate or invalid verse in " + path + ": " + insert->lastError().text());
        }
        verse = Verse(); verseOutput = 0;
    };
    auto skip = [&] { xml.skipCurrentElement(); --depth; };
    auto note = [&] {
        const auto attrs = xml.attributes();
        const auto classes = attrs.value("class").toString().split(' ');
        const auto types = attrs.value("http://www.idpf.org/2007/ops", "type").toString().split(' ');
        const auto role = attrs.value("role").toString();
        return classes.contains("noteref") || classes.contains("footnote") || classes.contains("endnote")
            || classes.contains("note") || classes.contains("xref")
            || role == "doc-noteref" || role == "doc-footnote" || role == "doc-endnote"
            || types.contains("noteref") || types.contains("footnote") || types.contains("endnote");
    };
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            ++depth;
            const QString tag = xml.name().toString();
            const auto attrs = xml.attributes();
            const QString cl = attrs.value("class").toString();
            if (note() || tag == "aside" || tag == "script" || tag == "style") { skip(); continue; }
            if (tag == "div" && cl == "main") {
                if (book) fail("Duplicate structured main container in " + path);
                const QString id = attrs.value("id").toString();
                prefix = id.endsWith("0_0") ? id.left(id.size() - 3) : id;
                book = anchorBook(prefix);
                if (!book) fail("Unknown semantic book prefix in " + path);
                haiolaMarker.setPattern("^" + QRegularExpression::escape(prefix) + "([1-9][0-9]*)_([1-9][0-9]*)$");
                mainDepth = depth;
            } else if (mainDepth && tag == "div") {
                if (cl == "p" || cl == "q" || cl == "q1" || cl == "q2" || cl == "m") {
                    paragraphDepth = depth; poetry = cl.startsWith('q');
                    paragraphStart = true;
                } else if (cl == "d" || cl == "s" || cl == "s1" || cl == "s2") {
                    flush();
                    QString heading;
                    const int headingDepth = depth;
                    while (depth >= headingDepth && !xml.atEnd()) {
                        xml.readNext();
                        if (xml.isStartElement()) {
                            ++depth;
                            if (note()) skip();
                        } else if (xml.isEndElement()) --depth;
                        else if (xml.isCharacters()) {
                            charge(xml.text().size()); heading += xml.text();
                        }
                    }
                    if (!pendingHeading.isEmpty()) pendingHeading += '\n';
                    pendingHeading += heading.simplified();
                    if (pendingHeading.size() > MaximumVerseOutput) fail("EPUB heading limit exceeded: " + path);
                } else {
                    if (cl == "psalmlabel") { flush(); pendingHeading.clear(); }
                    skip();
                }
            } else if (mainDepth && paragraphDepth && tag == "span" && cl == "verse") {
                flush();
                const QString id = attrs.value("id").toString();
                auto match = haiolaMarker.match(id);
                int chapter = 0, number = 0;
                if (match.hasMatch()) { chapter = match.captured(1).toInt(); number = match.captured(2).toInt(); }
                else {
                    match = osisMarker.match(id);
                    if (!match.hasMatch() || anchorBook(match.captured(1)) != book)
                        fail("Invalid or ambiguous semantic verse marker in " + path);
                    chapter = match.captured(2).toInt(); number = match.captured(3).toInt();
                }
                verse.book = book; verse.chapter = chapter; verse.number = number;
                if (keys.lastBook == book && keys.lastChapter == chapter && number <= keys.lastVerse)
                    fail("Duplicate or decreasing semantic verse marker in " + path);
                if (chapter < 1 || chapter > canon()[book-1].chapters || number < 1 || number > 176
                    || (kjv && number > kjvMaximum(book,chapter))) fail("Out-of-range verse marker in " + path);
                verse.heading = pendingHeading; pendingHeading.clear(); verse.poetry = poetry;
                verse.paragraphStart = paragraphStart || verse.number == 1;
                paragraphStart = false;
                charge(verse.heading.size());
                documents.insert(QString("%1:%2").arg(book).arg(verse.chapter));
                const auto label = xml.readElementText().simplified(); --depth;
                if (!markerLabel.match(label).hasMatch() || (!label.isEmpty()
                    && label != QString::number(number) && label != QString::number(number) + '.'
                    && label != QString("%1:%2").arg(chapter).arg(number)))
                    fail("Ambiguous verse marker label in " + path);
            } else if (mainDepth && paragraphDepth && (cl == "add" || cl == "tl" || tag == "i" || tag == "em")) {
                if (italics.size() >= 16) fail("EPUB formatting depth limit exceeded: " + path);
                italics.append(depth);
            } else if (paragraphDepth && verse.number && tag == "br") {
                charge(6); verse.text += '\n'; verse.html += "<br/>";
            } else if (mainDepth && paragraphDepth && (tag != "span" || (cl != "nd" && cl != "wj"))) {
                fail("Unsupported element inside verse text: " + tag + " in " + path);
            } else if (cl == "verse") {
                fail("Verse marker outside a Scripture paragraph in " + path);
            }
        } else if (xml.isEndElement()) {
            if (!italics.isEmpty() && italics.last() == depth) italics.removeLast();
            if (paragraphDepth == depth) {
                if (verse.number && !verse.text.trimmed().isEmpty() && !verse.text.endsWith('\n')) {
                    charge(6); verse.text = verse.text.trimmed() + '\n'; verse.html = verse.html.trimmed() + "<br/>";
                }
                paragraphDepth = 0;
            }
            if (mainDepth == depth) { flush(); mainDepth = 0; }
            --depth;
        } else if (xml.isCharacters() && paragraphDepth) {
            charge(7 * qint64(xml.text().size()) + (italics.isEmpty() ? 0 : 7));
            QString text = xml.text().toString();
            text.replace(whitespace, " ");
            // A pilcrow is layout only directly after a verse marker or at a
            // source paragraph boundary. Never remove one from quoted prose.
            if (paragraphStart || verse.text.trimmed().isEmpty()) {
                if (stripLayoutPilcrow(&text) && verse.text.trimmed().isEmpty()) verse.paragraphStart = true;
            }
            if (!verse.number) {
                if (!text.trimmed().isEmpty()) fail("Scripture text precedes its explicit verse marker in " + path);
                continue;
            }
            if (verse.text.isEmpty() || verse.text.endsWith('\n') || verse.text.endsWith(' ')) {
                while (text.startsWith(' ')) text.remove(0, 1);
            }
            verse.text += text;
            if (!text.trimmed().isEmpty()) paragraphStart = false;
            const QString html = text.toHtmlEscaped();
            verse.html += italics.isEmpty() || html.isEmpty() ? html : "<i>" + html + "</i>";
            if (!text.trimmed().isEmpty()) verse.poetry = verse.poetry || poetry;
        }
    }
    checkXml(xml, path);
    flush();
    if (!book) fail(MarkerHelp);
}

QStringList structuredPaths(const Package &info, const QHash<QString, QByteArray> &entries)
{
    QStringList paths;
    for (const auto &path : info.paths) {
        if (!entries.contains(path)) fail("The EPUB is missing " + path);
        const int book = structuredBook(entries.value(path), path);
        if (!book) continue; // front matter/navigation, with no verse markers
        if (book != paths.size() + 1) fail(QString("EPUB spine book anchors do not match the standard canon: %1 (book %2, expected %3).")
            .arg(path).arg(book).arg(paths.size() + 1));
        paths.append(path);
    }
    if (paths.size() != 66) fail(MarkerHelp + " A full Bible with all 66 book documents is required.");
    return paths;
}

void validate(QSqlDatabase &db)
{
    QSqlQuery q(db);
    exec(q, "PRAGMA quick_check");
    if (!q.next() || q.value(0).toString() != "ok") fail("The Bible database failed its integrity check.");
    exec(q, "SELECT key,value FROM metadata");
    QHash<QString, QString> metadata;
    while (q.next()) {
        const auto key = q.value(0).toString();
        if (metadata.contains(key)) fail("Duplicate Bible metadata key.");
        metadata.insert(key, q.value(1).toString());
    }
    const bool kjv = metadata.value("version_id") == "kjv-2006";
    const bool legacyNet = !metadata.contains("version_id") && metadata.value("edition") == "NET Bible 2.1";
    const bool generic = metadata.value("version_id").startsWith("epub-");
    if (generic) {
        const auto title = metadata.value("edition");
        const auto identifier = metadata.value("source_identifier");
        if (title.isEmpty() || title != title.simplified() || title.size() > 512 || identifier != identifier.trimmed()
            || identifier.size() > 4096 || !metadata.contains("source_identifier") || !metadata.contains("copyright")
            || metadata.value("version_id") != editionId(identifier,title)
            || metadata.value("abbreviation").isEmpty() || metadata.value("abbreviation") != abbreviation(title)
            || (metadata.value("versification") != "kjv-v1" && metadata.value("versification") != "net-2.1-v1")
            || metadata.value("schema") != "2"
            || metadata.value("metadata_digest") != metadataDigest(metadata)
            || !QRegularExpression("^[0-9a-f]{64}$").match(metadata.value("source_keys")).hasMatch())
            fail("The Bible database has inconsistent source edition metadata.");
    } else if (!(legacyNet || metadata.value("version_id") == "net-2.1" || kjv)
        || metadata.value("edition") != (kjv ? "King James (Authorized) Version" : "NET Bible 2.1")
        || (!legacyNet && (metadata.value("abbreviation") != (kjv ? "KJV" : "NET") || !metadata.contains("copyright"))))
        fail("The Bible database has an unsupported edition or inconsistent version metadata.");
    auto maximum = [&](int b, int c) { return generic ? 176 : kjv ? kjvMaximum(b,c) : chapterMaximum(b,c); };
    auto omitted = [&](int b, int c, int v) { return !kjv && omittedVerse(b,c,v); };
    exec(q, "SELECT value FROM metadata WHERE key='schema'");
    if (!q.next() || (q.value(0).toString() != "1" && q.value(0).toString() != "2"))
        fail("The Bible database has an unsupported format.");
    if (metadata.value("schema") == "2") {
        exec(q, "SELECT COUNT(*) FROM verses WHERE paragraphStart IS NULL OR paragraphStart NOT IN (0,1)");
        if (!q.next() || q.value(0).toInt()) fail("The Bible has invalid paragraph metadata.");
    } else if (metadata.value("paragraph_layout") == "1") fail("Paragraph metadata requires Bible schema 2.");
    exec(q, "SELECT book,chapter,number,text,html FROM verses ORDER BY book,chapter,number");
    int lastBook = 0, lastChapter = 0, lastVerse = 0, chapters = 0, chapterSize = 0;
    SourceKeys sourceKeys;
    auto finishChapter = [&] {
        if (!generic && lastBook && (lastVerse != maximum(lastBook,lastChapter)
            || chapterSize != (kjv ? maximum(lastBook,lastChapter) : chapterVerseCount(lastBook,lastChapter))))
            fail(QString("Incomplete chapter: %1 %2.").arg(canon()[lastBook-1].name).arg(lastChapter));
    };
    QVector<int> totals(66), counts(66);
    while (q.next()) {
        const int b = q.value(0).toInt(), c = q.value(1).toInt(), v = q.value(2).toInt();
        if (b < 1 || b > 66 || c < 1 || c > canon()[b-1].chapters || v < 1 || v > maximum(b,c)
            || q.value(3).toString().trimmed().isEmpty() || q.value(4).toString().trimmed().isEmpty())
            fail("The Bible contains an invalid or empty verse.");
        if (generic) sourceKeys.add(b,c,v);
        if (b != lastBook || c != lastChapter) {
            finishChapter();
            if (c != (b == lastBook ? lastChapter + 1 : 1)) fail("The Bible is missing a chapter.");
            ++chapters; ++counts[b-1]; lastVerse = 0; chapterSize = 0;
        }
        for (int missing = lastVerse + 1; !generic && missing < v; ++missing)
            if (!omitted(b,c,missing)) fail(QString("Unexpected missing verse: %1 %2:%3").arg(canon()[b-1].name).arg(c).arg(missing));
        if (v <= lastVerse || (!generic && omitted(b,c,v))) fail("The Bible has duplicate or unexpected verse numbering.");
        ++totals[b-1]; ++chapterSize; lastBook = b; lastChapter = c; lastVerse = v;
    }
    if (q.lastError().isValid()) fail(q.lastError().text());
    finishChapter();
    if (generic) {
        if (sourceKeys.finish() != metadata.value("source_keys")) fail("The Bible verse keys differ from the imported source.");
        if (sourceKeys.profile() != metadata.value("versification")) fail("The Bible verse keys differ from the supported numbering profile.");
        return;
    }
    if (chapters != 1189) fail(QString("Incomplete Bible: found %1 of 1,189 chapters.").arg(chapters));
    int total = 0;
    for (int i = 0; i < 66; ++i) {
        int expected = canon()[i].verses;
        if (kjv) {
            expected = 0;
            for (int chapter = 1; chapter <= canon()[i].chapters; ++chapter) expected += kjvMaximum(i+1,chapter);
        }
        if (counts[i] != canon()[i].chapters || totals[i] != expected)
            fail(QString("Incomplete %1: found %2 of %3 verses.").arg(canon()[i].name).arg(totals[i]).arg(expected));
        total += totals[i];
    }
    if (total != (kjv ? 31102 : 31087)) fail("The Bible has an unexpected total verse count.");
}
} // namespace

Edition inspectFile(const QString &path, QString *error)
{
    if (error) error->clear();
    try {
        const auto info = package(readArchive(path));
        if (info.edition.id != "net-2.1") {
            const auto entries = readArchive(path, QSet<QString>(info.paths.cbegin(), info.paths.cend()));
            const auto paths = structuredPaths(info,entries);
            QSet<QString> documents;
            qint64 output = 0;
            SourceKeys keys;
            for (const auto &document : paths)
                parseStructured(entries.value(document), document, nullptr, documents, output, keys, info.edition.id == "kjv-2006");
            keys.finish();
        }
        return info.edition;
    } catch (const std::exception &e) {
        if (error) *error = QString::fromUtf8(e.what());
        return {};
    }
}

bool validateDatabase(const QString &path, QString *error)
{
    if (error) error->clear();
    try { Connection c(path, true); validate(c.db); return true; }
    catch (const std::exception &e) { if (error) *error = QString::fromUtf8(e.what()); return false; }
}

Result importFile(const QString &epubPath, const QString &databasePath,
                  const std::shared_ptr<std::atomic<double>> &progress)
{
    Result result;
    auto report = [&](double value) { if (progress) progress->store(value); };
    try {
        report(0.01);
        const auto info = package(readArchive(epubPath));
        result.edition = info.edition;
        auto paths = info.paths;
        const bool kjv = info.edition.id == "kjv-2006";
        const bool structured = info.edition.id != "net-2.1";
        const bool generic = structured && !kjv;
        const auto entries = readArchive(epubPath, QSet<QString>(paths.cbegin(), paths.cend()));
        for (const auto &path : paths)
            if (!entries.contains(path)) fail("The EPUB is missing " + path);
        if (structured) paths = structuredPaths(info,entries);
        QLockFile lock(databasePath + ".import.lock");
        if (!lock.tryLock(0)) fail("Another " + Brand::name() + " process is importing. Please try again when it finishes.");
        report(0.1);
        QTemporaryFile temporary(QFileInfo(databasePath).absolutePath() + "/.bible-import-XXXXXX.sqlite");
        if (!temporary.open()) fail("Cannot create import database: " + temporary.errorString());
        const QString stage = temporary.fileName();
        temporary.close();
        {
            Connection c(stage);
            QSqlQuery q(c.db);
            exec(q, "PRAGMA journal_mode=DELETE");
            exec(q, "PRAGMA synchronous=FULL");
            exec(q, "CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL)");
            if (!q.prepare("INSERT INTO metadata VALUES(?,?)")) fail(q.lastError().text());
            const QList<QPair<QString, QString>> metadata = {{"schema", "2"}, {"paragraph_layout", "1"}, {"version_id", info.edition.id},
                {"abbreviation", info.edition.abbreviation}, {"edition", info.edition.name}, {"copyright", info.edition.copyright}};
            for (const auto &item : metadata) {
                q.bindValue(0, item.first); q.bindValue(1, item.second.isNull() ? QString("") : item.second);
                if (!q.exec()) fail(q.lastError().text());
            }
            exec(q, "CREATE TABLE verses(book INTEGER NOT NULL,chapter INTEGER NOT NULL,number INTEGER NOT NULL,text TEXT NOT NULL,html TEXT NOT NULL,heading TEXT NOT NULL,poetry INTEGER NOT NULL CHECK(poetry IN (0,1)),paragraphStart INTEGER NOT NULL CHECK(paragraphStart IN (0,1)),PRIMARY KEY(book,chapter,number)) WITHOUT ROWID");
            if (!c.db.transaction()) fail(c.db.lastError().text());
            QSqlQuery insert(c.db);
            if (!insert.prepare("INSERT INTO verses VALUES(?,?,?,?,?,?,?,?)")) fail(insert.lastError().text());
            QSet<QString> documents;
            qint64 globalOutput = 0;
            SourceKeys keys;
            for (int i = 0; i < paths.size(); ++i) {
                if (structured) parseStructured(entries.value(paths[i]), paths[i], &insert, documents, globalOutput, keys, kjv);
                else parseDocument(entries.value(paths[i]), paths[i], insert, documents, globalOutput);
                report(0.1 + 0.75 * (i + 1) / paths.size());
            }
            if (documents.size() != 1189) fail("The EPUB does not contain all 1,189 chapters.");
            if (structured) keys.finish();
            if (generic) {
                QHash<QString, QString> values;
                for (const auto &item : metadata) values.insert(item.first,item.second);
                values.insert("source_identifier",info.identifier);
                values.insert("source_keys",keys.finish());
                values.insert("versification",keys.profile());
                values.insert("metadata_digest",metadataDigest(values));
                if (!q.prepare("INSERT INTO metadata VALUES(?,?)")) fail(q.lastError().text());
                for (const auto &key : {"source_identifier", "source_keys", "versification", "metadata_digest"}) {
                    q.bindValue(0,key); q.bindValue(1,values.value(key));
                    if (!q.exec()) fail(q.lastError().text());
                }
            }
            if (!c.db.commit()) fail("Cannot commit imported Bible: " + c.db.lastError().text());
            validate(c.db);
            exec(q,"SELECT COUNT(*) FROM verses");
            if (!q.next()) fail("Cannot count imported verses.");
            result.verseCount = q.value(0).toInt();
        }
        report(0.95);
        QFile source(stage);
        QSaveFile destination(databasePath);
        destination.setDirectWriteFallback(false);
        if (!source.open(QIODevice::ReadOnly) || !destination.open(QIODevice::WriteOnly))
            fail("Cannot install Bible database: " + destination.errorString());
        while (!source.atEnd()) {
            const auto bytes = source.read(1024 * 1024);
            if (bytes.isEmpty() && source.error() != QFileDevice::NoError) fail("Cannot read the validated import database.");
            if (destination.write(bytes) != bytes.size()) fail("Cannot save imported Bible: " + destination.errorString());
        }
        if (!destination.commit()) fail("Cannot install imported Bible: " + destination.errorString());
        report(1.0);
        result.ok = true;
    } catch (const std::exception &e) { result.error = QString::fromUtf8(e.what()); }
    return result;
}
} // namespace EpubImporter
