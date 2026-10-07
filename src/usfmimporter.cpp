#include "usfmimporter.h"
#include "biblebooks.h"

#include <archive.h>
#include <archive_entry.h>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStringDecoder>
#include <QTemporaryFile>
#include <QUuid>
#include <QVariant>
#include <algorithm>
#include <functional>
#include <stdexcept>

namespace UsfmImporter {
bool stripLayoutPilcrow(QString *text, QString *html)
{
    static const QRegularExpression marker("^\\s*¶(?:\\s+|$)");
    const auto match = marker.match(*text);
    if (!match.hasMatch()) return false;
    text->remove(0, match.capturedLength());
    if (html) {
        static const QRegularExpression rendered("^(\\s*(?:<(?:i|b|sup)>\\s*)*)(?:¶|&para;|&#182;)(?:\\s+|(?=<|$))");
        const auto m = rendered.match(*html);
        if (m.hasMatch()) html->replace(0, m.capturedLength(), m.captured(1));
    }
    return true;
}

namespace {
[[noreturn]] void fail(const QString &s) { throw std::runtime_error(s.toStdString()); }
constexpr qint64 MaxEntry = 32 * 1024 * 1024;
constexpr qint64 MaxTotal = 256 * 1024 * 1024;
constexpr int MaxText = 65536;

QString utf8(const QByteArray &bytes)
{
    QStringDecoder decoder(QStringDecoder::Utf8);
    QString text = decoder(bytes);
    if (decoder.hasError() || text.contains(QChar(0))) fail("Invalid UTF-8 USFM resource.");
    if (text.startsWith(QChar(0xfeff))) text.remove(0, 1);
    return text;
}

bool scriptureFile(const QString &name)
{
    return name.endsWith(".usfm", Qt::CaseInsensitive) || name.endsWith(".sfm", Qt::CaseInsensitive);
}

QMap<QString, QByteArray> resources(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) fail("Cannot open USFM input: " + file.errorString());
    if (file.size() > MaxTotal || file.size() == 0) fail("USFM input is empty or exceeds the size limit.");
    if (scriptureFile(path)) {
        if (file.size() > MaxEntry) fail("USFM book exceeds the size limit.");
        return {{QFileInfo(path).fileName(), file.readAll()}};
    }
    file.close();
    std::unique_ptr<archive, decltype(&archive_read_free)> a(archive_read_new(), archive_read_free);
    archive_read_support_format_zip(a.get());
    if (archive_read_open_filename(a.get(), QFile::encodeName(path).constData(), 65536) != ARCHIVE_OK)
        fail("Cannot read USFM ZIP archive.");
    QMap<QString, QByteArray> result;
    QSet<QString> paths;
    archive_entry *entry = nullptr;
    qint64 total = 0;
    int count = 0, status;
    while ((status = archive_read_next_header(a.get(), &entry)) == ARCHIVE_OK) {
        if (++count > 2048) fail("USFM ZIP entry limit exceeded.");
        const char *raw = archive_entry_pathname_utf8(entry);
        if (!raw) fail("ZIP paths must be UTF-8.");
        const QString name = utf8(QByteArray(raw));
        const auto parts = name.split('/');
        if (name.isEmpty() || name.size() > 1024 || name.startsWith('/') || name.contains('\\')
            || name.contains(':') || parts.contains("..") || parts.contains(".")
            || paths.contains(name.toCaseFolded())) fail("Unsafe or duplicate USFM ZIP path: " + name);
        paths.insert(name.toCaseFolded());
        if (archive_entry_symlink(entry) || archive_entry_hardlink(entry)
            || (archive_entry_filetype(entry) != AE_IFREG && archive_entry_filetype(entry) != AE_IFDIR)
            || archive_entry_is_encrypted(entry)) fail("Unsupported linked or encrypted ZIP entry: " + name);
        const qint64 size = archive_entry_size(entry);
        if (size < 0 || size > MaxEntry) fail("USFM ZIP entry size limit exceeded: " + name);
        const bool wanted = scriptureFile(name) || QFileInfo(name).fileName().compare("copr.htm", Qt::CaseInsensitive) == 0;
        QByteArray bytes;
        char buffer[65536];
        qint64 expanded = 0;
        la_ssize_t n;
        while ((n = archive_read_data(a.get(), buffer, sizeof(buffer))) > 0) {
            expanded += n; total += n;
            if (expanded > MaxEntry || total > MaxTotal) fail("USFM ZIP expanded size limit exceeded.");
            if (wanted) bytes.append(buffer, n);
        }
        if (n < 0 || expanded != size) fail("Damaged USFM ZIP entry: " + name);
        if (wanted && archive_entry_filetype(entry) == AE_IFREG) result.insert(name, bytes);
    }
    if (status != ARCHIVE_EOF) fail("Damaged USFM ZIP archive.");
    return result;
}

// Attribution pages are HTML, often with unbalanced paragraph tags. Decode
// their text without a GUI/font dependency or XML external-entity processing.
QString htmlText(QString html)
{
    if (html.size() > 256 * 1024) fail("Copyright resource exceeds the size limit.");
    html.remove(QRegularExpression("<(script|style)\\b[^>]*>.*?</\\1\\s*>", QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption));
    html.replace(QRegularExpression("<[^>]*>"), " ");
    static const QMap<QString, QString> entities = {
        {"amp","&"}, {"lt","<"}, {"gt",">"}, {"quot","\""}, {"apos","'"},
        {"nbsp"," "}, {"copy","©"}, {"reg","®"}, {"trade","™"}, {"ndash","–"},
        {"mdash","—"}, {"lsquo","‘"}, {"rsquo","’"}, {"ldquo","“"}, {"rdquo","”"},
        {"hellip","…"}, {"para","¶"}, {"bull","•"}, {"eacute","é"}
    };
    static const QRegularExpression entity("&(#x[0-9a-fA-F]+|#[0-9]+|[A-Za-z]+);");
    QString output;
    qsizetype end = 0;
    auto matches = entity.globalMatch(html);
    while (matches.hasNext()) {
        const auto m = matches.next();
        output += html.mid(end, m.capturedStart() - end);
        const QString value = m.captured(1);
        if (value.startsWith('#')) {
            bool ok;
            const bool hex = value.startsWith("#x");
            const uint code = value.mid(hex ? 2 : 1).toUInt(&ok, hex ? 16 : 10);
            if (!ok || !code || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) fail("Invalid copyright character entity.");
            const char32_t ch = code;
            output += QString::fromUcs4(&ch, 1);
        } else output += entities.value(value, m.captured());
        end = m.capturedEnd();
    }
    output += html.mid(end);
    return output.simplified();
}

struct Token { QString marker, text; bool close = false; };
class Lexer {
    const QString &source;
    qsizetype pos = 0;
    int count = 0;
public:
    explicit Lexer(const QString &s) : source(s) {}
    bool next(Token &token) {
        if (pos == source.size()) return false;
        if (++count > 4000000) fail("USFM marker limit exceeded.");
        token = {};
        if (source[pos] == '\\') {
            const auto start = ++pos;
            if (pos < source.size() && source[pos] == '+') ++pos;
            while (pos < source.size() && ((source[pos] >= 'a' && source[pos] <= 'z') || source[pos].isDigit())) ++pos;
            token.marker = source.mid(start, pos - start);
            if (token.marker.isEmpty() || token.marker == "+" || token.marker.size() > 32) fail("Malformed USFM marker.");
            if (pos < source.size() && source[pos] == '*') { token.close = true; ++pos; }
            else if (pos < source.size() && !source[pos].isSpace()) fail("Malformed USFM marker delimiter: " + token.marker);
            // A marker's separating space is syntax; following closing markers
            // have no separator, so adjacent words and punctuation stay adjacent.
            if (!token.close && pos < source.size() && source[pos] == ' ') ++pos;
        }
        auto end = source.indexOf('\\', pos);
        if (end < 0) end = source.size();
        token.text = source.mid(pos, end - pos);
        pos = end;
        return true;
    }
};

struct SourceBook {
    QString path, source, code, name, abbreviation, title, toc1;
    int id = 0, ordinal = 0;
};
struct Input { Edition edition; QVector<SourceBook> books; };

Input load(const QString &path)
{
    const auto files = resources(path);
    Input input;
    QSet<QString> suffixes, descriptions;
    QSet<int> ids;
    QString copyrightPage;
    static const QRegularExpression filename("^(\\d+)-([A-Z0-9]{3})([a-z0-9][a-z0-9-]*)\\.(?:usfm|sfm)$", QRegularExpression::CaseInsensitiveOption);
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        if (!scriptureFile(it.key())) {
            if (!copyrightPage.isEmpty()) fail("Multiple copyright resources in USFM archive.");
            copyrightPage = utf8(it.value());
            continue;
        }
        SourceBook book;
        book.path = it.key(); book.source = utf8(it.value());
        Lexer lexer(book.source); Token token;
        bool seenId = false;
        while (lexer.next(token)) {
            if (token.marker == "c") break;
            if (token.marker == "id") {
                if (seenId) fail("Duplicate USFM book identifier: " + book.path);
                seenId = true;
                const QString id = token.text.simplified();
                book.code = id.section(' ', 0, 0).toUpper();
                book.title = id.section(' ', 1).trimmed();
                if (book.title.startsWith('-')) book.title.remove(0, 1);
                book.title = book.title.trimmed();
            } else if (token.marker == "toc1") book.toc1 = token.text.simplified();
            else if (token.marker == "toc2" || (token.marker == "h" && book.name.isEmpty())) book.name = token.text.simplified();
            else if (token.marker == "toc3") book.abbreviation = token.text.simplified();
        }
        if (!seenId) fail("Missing USFM book identifier: " + book.path);
        const auto match = filename.match(QFileInfo(book.path).fileName());
        if (match.hasMatch()) {
            if (match.captured(2).toUpper() != book.code) fail("USFM filename and book identifier disagree: " + book.path);
            suffixes.insert(match.captured(3).toLower());
        }
        if (book.code == "FRT" || book.code == "INT") continue;
        book.id = BibleBooks::bookId(book.code);
        if (!book.id || QString::fromLatin1(BibleBooks::canon()[book.id - 1].code) != book.code)
            fail("Unsupported USFM book code: " + book.code);
        if (ids.contains(book.id)) fail("Duplicate USFM book: " + book.code);
        ids.insert(book.id);
        if (!book.title.isEmpty()) descriptions.insert(book.title);
        if (book.name.isEmpty()) book.name = QString::fromUtf8(BibleBooks::canon()[book.id - 1].name);
        if (book.abbreviation.isEmpty()) book.abbreviation = book.code;
        if (book.name.size() > 1024 || book.toc1.size() > 2048 || book.abbreviation.size() > 128) fail("USFM book metadata is too long.");
        input.books.append(book);
    }
    if (input.books.isEmpty()) fail("No Scripture books found in USFM input.");
    // Numbered source filenames define navigation, including unusual source orders.
    std::sort(input.books.begin(), input.books.end(), [&](const SourceBook &a, const SourceBook &b) {
        const auto am = filename.match(QFileInfo(a.path).fileName()), bm = filename.match(QFileInfo(b.path).fileName());
        if (am.hasMatch() && bm.hasMatch() && am.captured(1).toInt() != bm.captured(1).toInt())
            return am.captured(1).toInt() < bm.captured(1).toInt();
        return a.path < b.path;
    });
    for (int i = 0; i < input.books.size(); ++i) input.books[i].ordinal = i + 1;
    if (suffixes.size() > 1) fail("Inconsistent internal USFM edition identifiers.");
    QString title, rights, linkedId;
    if (!copyrightPage.isEmpty()) {
        if (copyrightPage.size() > 256 * 1024) fail("Copyright resource exceeds the size limit.");
        const auto options = QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption;
        title = htmlText(QRegularExpression("<title[^>]*>(.*?)</title>", options).match(copyrightPage).captured(1));
        linkedId = QRegularExpression("href=[\"']https?://ebible\\.org/([a-z0-9-]+)[/\"']", options).match(copyrightPage).captured(1).toLower();
        const auto start = QRegularExpression("</h2>", options).globalMatch(copyrightPage);
        auto headings = start;
        qsizetype offset = -1;
        while (headings.hasNext()) offset = headings.next().capturedEnd();
        if (offset >= 0) {
            QString region = copyrightPage.mid(offset);
            const auto footer = QRegularExpression("<div\\s+class=[\"']fine[\"']", options).match(region);
            if (footer.hasMatch()) region.truncate(footer.capturedStart());
            // The first paragraph carries the actual notice; language and site
            // promotion are not attribution. Keep explicit license terms below.
            const auto language = region.indexOf("Language:", 0, Qt::CaseInsensitive);
            rights = htmlText(language >= 0 ? region.left(language) : region.section("</p>", 0, 0));
            const auto patent = region.indexOf("Letters patent");
            if (patent >= 0) rights += "\n" + htmlText(region.mid(patent).section("</p>", 0, 0));
            const auto license = region.indexOf("This translation is made available");
            if (license >= 0) {
                QString terms = region.mid(license);
                const auto end = terms.indexOf("</ul>");
                if (end >= 0) terms.truncate(end + 5);
                rights += "\n" + htmlText(terms);
            }
        }
    }
    const QString internalId = suffixes.isEmpty() ? linkedId : *suffixes.cbegin();
    if (!linkedId.isEmpty() && !internalId.isEmpty() && linkedId != internalId) fail("Copyright and internal USFM edition identifiers disagree.");
    if (title.isEmpty() && descriptions.size() == 1) title = *descriptions.cbegin();
    if (title.isEmpty()) fail("USFM edition title is missing or inconsistent; provide internal edition metadata or copr.htm.");
    if (title.size() > 1024 || rights.size() > 32768) fail("USFM edition metadata exceeds the size limit.");
    static const QMap<QString, QString> abbreviations = {{"engnet","NET"}, {"eng-kjv2006","KJV"},
        {"eng-kjv","KJV+"}, {"engojb","OJB"}, {"engylt","YLT"}, {"eng-asv","ASV"}, {"engbsb","BSB"}};
    input.edition.id = internalId.isEmpty() ? "usfm-" + QString::fromLatin1(QCryptographicHash::hash(title.toUtf8(), QCryptographicHash::Sha256).toHex().left(32)) : internalId;
    input.edition.name = title; input.edition.copyright = rights.trimmed();
    input.edition.abbreviation = abbreviations.value(internalId);
    if (input.edition.abbreviation.isEmpty()) {
        for (const auto &word : title.split(' ', Qt::SkipEmptyParts)) if (word.front().isLetterOrNumber()) input.edition.abbreviation += word.front().toUpper();
        input.edition.abbreviation = input.edition.abbreviation.left(16);
    }
    return input;
}

struct Note { QString kind, caller, text, targets; int offset = 0; };
struct Span { int start = 0, end = 0; };
struct Verse {
    int chapter = 0, number = 0; QString text, html, heading; bool poetry = false, paragraphStart = false;
    // Schema 5: each text line's poetry level, one digit per line ('0' prose).
    QString levels;
    QVector<Note> notes; QVector<Span> wordsOfJesus;
};
struct Text {
    QString plain, html;
    bool space = false;
    void append(const QString &s) {
        for (QChar c : s) {
            if (c.isSpace()) { space = !plain.isEmpty(); continue; }
            if (space) { plain += ' '; html += ' '; space = false; }
            plain += c; html += QString(c).toHtmlEscaped();
        }
        if (plain.size() > MaxText || html.size() > MaxText * 4) fail("USFM verse or heading text limit exceeded.");
    }
    void line() { if (!plain.isEmpty()) { plain += '\n'; html += "<br/>"; } space = false; }
};

QString openTag(const QString &tag) { return tag == "span" ? QStringLiteral("<span class=\"wj\">") : "<" + tag + ">"; }
QString style(const QString &marker)
{
    if (marker == "add" || marker == "it" || marker == "bk" || marker == "tl" || marker == "qs" || marker == "qac") return "i";
    if (marker == "bd") return "b";
    if (marker == "bdit") return "b";
    if (marker == "sup" || marker == "ord") return "sup";
    // Words of Jesus: a classed span the reader may color (format only).
    if (marker == "wj") return "span";
    return {};
}
const QSet<QString> inlineMarkers = {"w","add","it","bd","bdit","nd","qt","qs","qac","sup","ord","sc","wj","bk","tl","em","no","pn","png","k","sig","sls","dc","ior","rq"};
const QSet<QString> noteFields = {"fr","ft","fk","fq","fqa","fl","fp","fv","fdc","fm","xo","xt","xk","xq","xot","xnt","xdc"};
bool paragraph(const QString &m) {
    static const QRegularExpression pattern("^(p|m|po|pr|cls|pmo|pm|pmc|pmr|pi[0-9]?|mi|pc|ph[0-9]?|q[0-9]?|qr|qc|qm[0-9]?|qd|lh|li[0-9]?|lf|lim[0-9]?|nb|b)$");
    return pattern.match(m).hasMatch();
}
// \q, \q1–\q4 and their \qm forms set a poetry line's indent level; the
// other poetry markers (\qr, \qc, \qd) sit at the first level.
int poetryLevel(const QString &m)
{
    if (!m.startsWith('q')) return 0;
    const QChar last = m.back();
    return last.isDigit() ? std::clamp(last.digitValue(), 1, 4) : 1;
}
bool heading(const QString &m) {
    static const QRegularExpression pattern("^(s[0-9]?|ms[0-9]?|d|sp|qa)$");
    return pattern.match(m).hasMatch();
}
bool header(const QString &m) {
    static const QRegularExpression pattern("^(id|ide|usfm|h[0-9]?|toc[123]|toca[123]|mt[0-9]?|mte[0-9]?|cl|cp|cd|r|mr|sr|rem|sts|restore|lit|periph)$");
    return pattern.match(m).hasMatch();
}
int number(QString &text, const QString &marker)
{
    while (!text.isEmpty() && text.front().isSpace()) text.remove(0, 1);
    const auto end = text.indexOf(QRegularExpression("\\s"));
    const QString value = end < 0 ? text : text.left(end);
    static const QRegularExpression integer("^[1-9][0-9]{0,2}$");
    if (!integer.match(value).hasMatch()) fail("Unsupported USFM " + marker + " number (integer 1–999 required): " + value);
    text = end < 0 ? QString() : text.mid(end + 1);
    return value.toInt();
}

void parse(const SourceBook &book, const std::function<void(const Verse &, bool)> &sink,
           const std::function<void(int)> &chapterSink)
{
    Lexer lexer(book.source); Token token;
    Verse verse;
    Text body, title;
    QString pendingHeading;
    enum Destination { Ignore, Body, Heading } destination = Ignore;
    struct Frame { QString marker, tag; };
    QVector<Frame> styles;
    QStringList notes, noteStyles, excluded;
    QVector<int> noteStyleDepths;
    int chapter = 0, previousVerse = 0;
    bool pendingParagraph = true, poetry = false, introduction = false;
    int level = 0;
    // Schema 4: top-level notes (\f, \fe, \x) are captured as plain text with
    // their anchor offset in the verse's plain text; \wj spans become ranges.
    Note note;
    QString noteField;
    bool capturing = false, anchored = false, inWj = false;
    int wjStart = -1;
    QVector<Note> pendingNotes;
    auto finishTitle = [&] {
        if (!title.plain.isEmpty()) {
            if (!pendingHeading.isEmpty()) pendingHeading += '\n';
            pendingHeading += title.plain;
            if (pendingHeading.size() > MaxText) fail("USFM heading size limit exceeded.");
        }
        title = {};
    };
    auto finishVerse = [&] {
        if (!verse.number) return;
        verse.text = body.plain.trimmed(); verse.html = body.html.trimmed();
        const int untrimmed = verse.text.size();
        if (stripLayoutPilcrow(&verse.text, &verse.html)) verse.paragraphStart = true;
        // A stripped pilcrow line takes its level with it.
        verse.levels = verse.levels.right(verse.text.count('\n') + 1);
        // Offsets were recorded against the untrimmed text; a stripped layout
        // pilcrow shifts them left. Clamp into the stored verse text.
        const int lead = untrimmed - verse.text.size(), length = verse.text.size();
        for (auto &n : verse.notes) n.offset = std::clamp(n.offset - lead, 0, length);
        QVector<Span> spans;
        for (auto span : verse.wordsOfJesus) {
            span.start = std::clamp(span.start - lead, 0, length); span.end = std::clamp(span.end - lead, 0, length);
            if (span.end > span.start) spans.append(span);
        }
        verse.wordsOfJesus = spans;
        const bool retained = !verse.text.isEmpty();
        sink(verse, retained);
        if (!retained) {
            if (!verse.heading.isEmpty()) pendingHeading = verse.heading + (pendingHeading.isEmpty() ? "" : "\n" + pendingHeading);
            pendingParagraph = pendingParagraph || verse.paragraphStart;
            for (auto n : verse.notes) { n.offset = 0; pendingNotes.append(n); }
        }
        verse = {}; body = {};
    };
    auto append = [&](QString text) {
        if (destination == Ignore || !notes.isEmpty() || !excluded.isEmpty()) return;
        if (!styles.isEmpty() && (styles.back().marker == "w" || styles.back().marker == "+w")) {
            const auto attributes = text.indexOf('|');
            if (attributes >= 0) text.truncate(attributes);
        }
        Text &out = destination == Heading ? title : body;
        if (destination == Body && !text.trimmed().isEmpty()) {
            // Some sources place unnumbered prologues after the first chapter
            // marker (Sirach). They have no Scripture key and are not verses.
            if (!verse.number) return;
            if (pendingParagraph || out.plain.isEmpty()) {
                if (stripLayoutPilcrow(&text)) verse.paragraphStart = verse.paragraphStart || out.plain.isEmpty();
            }
            if (pendingParagraph && !text.trimmed().isEmpty()) {
                if (out.plain.isEmpty()) verse.paragraphStart = true;
                else { out.line(); verse.levels += QChar('0' + level); }
                pendingParagraph = false;
            }
            // The first line's level is the one in force at its first text.
            if (out.plain.isEmpty() && !text.trimmed().isEmpty()) verse.levels = QChar('0' + level);
            verse.poetry = verse.poetry || poetry;
        }
        const auto before = out.plain.size();
        out.append(text);
        if (destination == Body && inWj && wjStart < 0 && out.plain.size() > before)
            wjStart = int(before) + (out.plain.at(before).isSpace() ? 1 : 0);
    };
    auto noteText = [&](QString text) {
        if (!capturing || notes.size() != 1) return;
        if (!noteStyles.isEmpty() && (noteStyles.back() == "w" || noteStyles.back() == "+w")) {
            const auto attributes = text.indexOf('|');
            if (attributes >= 0) text.truncate(attributes);
        }
        note.text += text;
        if (noteField == "xt") note.targets += text;
    };
    while (lexer.next(token)) {
        QString m = token.marker;
        const bool nested = m.startsWith('+');
        if (nested) m.remove(0, 1);
        if (!notes.isEmpty()) {
            if ((m == "f" || m == "fe" || m == "x") && !token.close) {
                if (notes.size() + noteStyles.size() >= 32) fail("USFM note nesting limit exceeded.");
                notes.append(m); noteStyleDepths.append(noteStyles.size());
            } else if (m == notes.back() && token.close) {
                if (noteStyles.size() != noteStyleDepths.back()) fail("Unclosed USFM character style in note.");
                noteStyleDepths.removeLast();
                notes.removeLast();
                if (notes.isEmpty()) {
                    if (capturing) {
                        note.text = note.text.simplified(); note.targets = note.targets.simplified();
                        if (note.text.size() > MaxText) fail("USFM note text limit exceeded.");
                        if (!note.text.isEmpty() || !note.targets.isEmpty()) {
                            if (anchored) verse.notes.append(note); else pendingNotes.append(note);
                        }
                    }
                    capturing = false;
                    append(token.text);
                    continue;
                }
            } else if (m == "c" || m == "v") fail("Unclosed USFM note before " + m + " in " + book.code);
            else if (inlineMarkers.contains(m) && !noteFields.contains(m)) {
                if (token.close) {
                    if (noteStyles.size() <= noteStyleDepths.back() || noteStyles.back() != token.marker) fail("Mismatched USFM note style: " + token.marker);
                    noteStyles.removeLast();
                } else {
                    if (notes.size() + noteStyles.size() >= 32) fail("USFM note nesting limit exceeded.");
                    noteStyles.append(token.marker);
                }
            }
            // Note subfields (including +fl and xt) implicitly end at the next
            // subfield or note close; they are not paired character styles.
            if (noteFields.contains(m) && !token.close) { noteField = m; if (notes.size() == 1) note.text += ' '; }
            noteText(token.text);
            continue;
        }
        if (!excluded.isEmpty()) {
            if (token.close && token.marker == excluded.back()) {
                excluded.removeLast();
                if (excluded.isEmpty()) append(token.text);
            } else if (m == "c" || m == "v") fail("Unclosed USFM alternate-number or reference marker.");
            else if (!token.close && (m == "va" || m == "ca" || m == "vp" || m == "xt" || inlineMarkers.contains(m))) {
                if (excluded.size() >= 32) fail("USFM excluded marker depth exceeded.");
                excluded.append(token.marker);
            } else if (token.close) fail("Mismatched excluded USFM marker: " + token.marker);
            continue;
        }
        if (m.isEmpty()) { append(token.text); continue; }
        if ((m == "f" || m == "fe" || m == "x") && !token.close) {
            notes.append(m); noteStyleDepths.append(0);
            const QString head = token.text.trimmed();
            note = {}; note.kind = m; noteField.clear();
            note.caller = head.section(' ', 0, 0).left(16);
            note.text = head.section(' ', 1);
            capturing = destination != Ignore && !introduction;
            anchored = destination == Body && verse.number;
            note.offset = anchored ? int(body.plain.size()) : 0;
            continue;
        }
        if ((m == "va" || m == "ca" || m == "vp" || m == "xt") && !token.close) { excluded.append(token.marker); continue; }
        if (inlineMarkers.contains(m)) {
            if (m == "wj" && token.close) {
                if (inWj && wjStart >= 0 && destination == Body && verse.number)
                    verse.wordsOfJesus.append({wjStart, int(body.plain.size())});
                inWj = false; wjStart = -1;
            } else if (m == "wj") { inWj = true; wjStart = -1; }
            if (token.close) {
                if (styles.isEmpty() || styles.back().marker != token.marker) fail("Mismatched USFM character style: " + token.marker + " in " + book.code);
                const auto frame = styles.takeLast();
                if (destination != Ignore && !frame.tag.isEmpty()) (destination == Heading ? title : body).html += "</" + frame.tag + ">";
            } else {
                if (styles.size() >= 32) fail("USFM character style depth exceeded.");
                const QString tag = style(m);
                styles.append({token.marker, tag});
                if (destination != Ignore && !tag.isEmpty()) (destination == Heading ? title : body).html += openTag(tag);
            }
            append(token.text); continue;
        }
        if (token.close) fail("Unexpected USFM closing marker: " + token.marker);
        if (!styles.isEmpty()) fail("Unclosed USFM character style before " + m + " in " + book.code);
        if (destination == Heading) finishTitle();
        if (m == "c") {
            finishVerse();
            const int next = number(token.text, m);
            if (next <= chapter) fail("USFM chapters must be strictly ascending: " + book.code);
            if (!token.text.trimmed().isEmpty()) fail("Unexpected text in USFM chapter marker.");
            chapter = next; chapterSink(chapter); previousVerse = 0;
            destination = Body; introduction = false; pendingParagraph = true; poetry = false; level = 0;
        } else if (m == "v") {
            if (!chapter) fail("USFM verse precedes its chapter: " + book.code);
            finishVerse();
            const int next = number(token.text, m);
            if (next <= previousVerse) fail("USFM verses must be strictly ascending: " + book.code + " " + QString::number(chapter));
            previousVerse = next;
            verse.chapter = chapter; verse.number = next; verse.heading = pendingHeading; pendingHeading.clear();
            verse.notes = pendingNotes; pendingNotes.clear();
            verse.paragraphStart = pendingParagraph; verse.poetry = poetry;
            destination = Body; append(token.text);
        } else if (heading(m) && !introduction) {
            destination = Heading; append(token.text);
        } else if (paragraph(m) && chapter && !introduction) {
            destination = Body; pendingParagraph = true;
            if (m != "b") { poetry = m.startsWith('q'); level = poetryLevel(m); }
            // An unnumbered poetry label before a chapter's first verse (for
            // example Psalm 119's first acrostic label) belongs to its heading.
            if (!verse.number && m.startsWith('q')) destination = Heading;
            append(token.text);
        } else if (m.startsWith('i') && !chapter) {
            introduction = true; destination = Ignore;
        } else if (header(m)) {
            if (m == "id" && chapter) fail("Unexpected second USFM book identifier.");
            destination = Ignore;
        } else if (!chapter) destination = Ignore;
        else fail("Unsupported USFM body marker: \\" + token.marker + " in " + book.code);
    }
    if (!notes.isEmpty() || !excluded.isEmpty() || !styles.isEmpty()) fail("Unclosed USFM marker at end of " + book.code);
    if (destination == Heading) finishTitle();
    finishVerse();
    if (!chapter) fail("USFM Scripture book contains no chapters: " + book.code);
}

struct Connection {
    QString name = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSqlDatabase db;
    Connection(const QString &path, bool readOnly = false) {
        db = QSqlDatabase::addDatabase("QSQLITE", name); db.setDatabaseName(path);
        if (readOnly) db.setConnectOptions("QSQLITE_OPEN_READONLY");
        if (!db.open()) {
            const QString error = db.lastError().text(); db = {}; QSqlDatabase::removeDatabase(name);
            fail("Cannot open USFM database: " + error);
        }
    }
    ~Connection() { db.close(); db = {}; QSqlDatabase::removeDatabase(name); }
};
void exec(QSqlQuery &q, const QString &sql) { if (!q.exec(sql)) fail("USFM database: " + q.lastError().text()); }
void run(QSqlQuery &q) { if (!q.exec()) fail("USFM database: " + q.lastError().text()); }
void hashField(QCryptographicHash &hash, const QByteArray &value) {
    hash.addData(QByteArray::number(value.size())); hash.addData(":"); hash.addData(value);
}
QString queryDigest(QSqlDatabase db, const QStringList &queries)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QSqlQuery q(db);
    for (const QString &sql : queries) {
        hashField(hash, sql.toUtf8()); exec(q, sql);
        while (q.next()) {
            for (int i = 0; i < q.record().count(); ++i) hashField(hash, q.value(i).toString().toUtf8());
        }
        if (q.lastError().isValid()) fail("Cannot read USFM database integrity data.");
    }
    return QString::fromLatin1(hash.result().toHex());
}
const QString keyQuery = "SELECT book,chapter,number,retained FROM source_keys ORDER BY book,chapter,number";
const QStringList integrityQueries = {
    "SELECT key,value FROM metadata WHERE key<>'content_digest' ORDER BY key",
    "SELECT id,code,name,abbreviation,ordinal FROM books ORDER BY ordinal",
    "SELECT book,chapter FROM source_chapters ORDER BY book,chapter",
    "SELECT book,name FROM book_aliases ORDER BY book,name", keyQuery,
    "SELECT book,chapter,number,text,html,heading,poetry,paragraphStart FROM verses ORDER BY book,chapter,number"
};
// Schema 4 adds source notes and words-of-Jesus ranges; schema 3 files keep
// their original digest and remain readable (flagged for re-import).
const QStringList schema4Queries = {
    "SELECT book,chapter,verse,seq,kind,caller,offset,text,targets FROM notes ORDER BY book,chapter,verse,seq",
    "SELECT book,chapter,verse,start,end FROM words_of_jesus ORDER BY book,chapter,verse,start"
};
// Schema 5 adds poetry line levels (verses with a poetry line only).
const QStringList schema5Queries = {"SELECT book,chapter,verse,levels FROM indents ORDER BY book,chapter,verse"};
QStringList digestQueries(const QString &schema)
{
    return schema == "5" ? integrityQueries + schema4Queries + schema5Queries
        : schema == "4" ? integrityQueries + schema4Queries : integrityQueries;
}
void validate(QSqlDatabase db)
{
    QSqlQuery q(db);
    exec(q, "PRAGMA quick_check");
    if (!q.next() || q.value(0).toString() != "ok") fail("USFM database integrity check failed.");
    QMap<QString, QString> metadata;
    exec(q, "SELECT key,value FROM metadata");
    while (q.next()) metadata.insert(q.value(0).toString(), q.value(1).toString());
    const QString schema = metadata.value("schema");
    if ((schema != "3" && schema != "4" && schema != "5") || metadata.value("source_format") != "usfm"
        || metadata.value("version_id").isEmpty() || metadata.value("edition").isEmpty()
        || metadata.value("abbreviation").isEmpty() || metadata.value("paragraph_layout") != "1") fail("Invalid USFM database metadata.");
    int books = 0;
    exec(q, "SELECT id,code,name,abbreviation,ordinal FROM books ORDER BY ordinal");
    while (q.next()) {
        const int id = q.value(0).toInt();
        if (id < 1 || id > BibleBooks::canon().size() || q.value(1).toString() != BibleBooks::canon()[id - 1].code
            || q.value(2).toString().isEmpty() || q.value(3).toString().isEmpty() || q.value(4).toInt() != ++books)
            fail("Invalid USFM book identity or source ordering.");
    }
    if (!books || books != metadata.value("book_count").toInt()) fail("USFM book count mismatch.");
    const QStringList invalid = {
        "SELECT 1 FROM source_keys WHERE book NOT IN (SELECT id FROM books) OR chapter NOT BETWEEN 1 AND 999 OR number NOT BETWEEN 1 AND 999 OR retained NOT IN (0,1) LIMIT 1",
        "SELECT 1 FROM source_keys s LEFT JOIN source_chapters c ON c.book=s.book AND c.chapter=s.chapter WHERE c.book IS NULL LIMIT 1",
        "SELECT 1 FROM source_chapters c WHERE c.book NOT IN (SELECT id FROM books) OR c.chapter NOT BETWEEN 1 AND 999 OR NOT EXISTS (SELECT 1 FROM verses v WHERE v.book=c.book AND v.chapter=c.chapter) LIMIT 1",
        "SELECT 1 FROM books b WHERE NOT EXISTS (SELECT 1 FROM source_chapters c WHERE c.book=b.id) LIMIT 1",
        "SELECT 1 FROM source_keys s LEFT JOIN verses v ON v.book=s.book AND v.chapter=s.chapter AND v.number=s.number WHERE (s.retained=1 AND v.book IS NULL) OR (s.retained=0 AND v.book IS NOT NULL) LIMIT 1",
        "SELECT 1 FROM verses v LEFT JOIN source_keys s ON v.book=s.book AND v.chapter=s.chapter AND v.number=s.number WHERE s.book IS NULL OR length(trim(v.text))=0 OR length(v.text)>65536 OR length(v.html)>262144 OR v.poetry NOT IN (0,1) OR v.paragraphStart NOT IN (0,1) LIMIT 1",
        "SELECT 1 FROM book_aliases WHERE book NOT IN (SELECT id FROM books) OR length(trim(name))=0 LIMIT 1"
    };
    for (const auto &sql : invalid) { exec(q, sql); if (q.next()) fail("USFM source book/chapter/verse integrity mismatch."); }
    if (schema == "5") {
        exec(q, "SELECT 1 FROM indents i LEFT JOIN verses v ON v.book=i.book AND v.chapter=i.chapter AND v.number=i.verse "
                "WHERE v.book IS NULL OR length(i.levels)=0 OR i.levels GLOB '*[^0-4]*' OR NOT i.levels GLOB '*[1-4]*' "
                "OR length(i.levels)<>length(v.text)-length(replace(v.text,char(10),''))+1 LIMIT 1");
        if (q.next()) fail("USFM poetry line integrity mismatch.");
    }
    if (schema == "4" || schema == "5") {
        const QStringList notes = {
            "SELECT 1 FROM notes n LEFT JOIN verses v ON v.book=n.book AND v.chapter=n.chapter AND v.number=n.verse WHERE v.book IS NULL OR n.offset<0 OR n.offset>length(v.text) OR n.kind NOT IN ('f','fe','x') OR length(n.text)>65536 OR length(n.targets)>65536 LIMIT 1",
            "SELECT 1 FROM words_of_jesus w LEFT JOIN verses v ON v.book=w.book AND v.chapter=w.chapter AND v.number=w.verse WHERE v.book IS NULL OR w.start<0 OR w.end<=w.start OR w.end>length(v.text) LIMIT 1"
        };
        for (const auto &sql : notes) { exec(q, sql); if (q.next()) fail("USFM note or words-of-Jesus integrity mismatch."); }
    }
    exec(q, "SELECT count(*) FROM verses");
    if (!q.next() || q.value(0).toInt() <= 0 || q.value(0).toString() != metadata.value("verse_count")) fail("USFM verse count mismatch.");
    if (queryDigest(db, {keyQuery}) != metadata.value("source_keys")) fail("USFM source-key digest mismatch.");
    if (queryDigest(db, digestQueries(schema)) != metadata.value("content_digest"))
        fail("USFM database content digest mismatch.");
}
}

Edition inspectFile(const QString &path, QString *error)
{
    if (error) error->clear();
    try { return load(path).edition; }
    catch (const std::exception &e) { if (error) *error = QString::fromUtf8(e.what()); return {}; }
}

bool validateDatabase(const QString &path, QString *error)
{
    if (error) error->clear();
    try { Connection connection(path, true); validate(connection.db); return true; }
    catch (const std::exception &e) { if (error) *error = QString::fromUtf8(e.what()); return false; }
}

Result importFile(const QString &path, const QString &target, const std::shared_ptr<std::atomic<double>> &progress)
{
    Result result;
    if (progress) progress->store(0);
    try {
        const Input input = load(path); result.edition = input.edition;
        QTemporaryFile temporary(QFileInfo(target).absolutePath() + "/.usfm-XXXXXX.sqlite");
        if (!temporary.open()) fail("Cannot create temporary USFM database: " + temporary.errorString());
        const QString temporaryPath = temporary.fileName(); temporary.close();
        {
            Connection connection(temporaryPath); QSqlQuery q(connection.db);
            exec(q, "PRAGMA journal_mode=DELETE");
            exec(q, "BEGIN IMMEDIATE");
            exec(q, "CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL) WITHOUT ROWID");
            exec(q, "CREATE TABLE books(id INTEGER PRIMARY KEY,code TEXT NOT NULL UNIQUE,name TEXT NOT NULL,abbreviation TEXT NOT NULL,ordinal INTEGER NOT NULL UNIQUE)");
            exec(q, "CREATE TABLE verses(book INTEGER NOT NULL,chapter INTEGER NOT NULL,number INTEGER NOT NULL,text TEXT NOT NULL,html TEXT NOT NULL,heading TEXT NOT NULL,poetry INTEGER NOT NULL,paragraphStart INTEGER NOT NULL,PRIMARY KEY(book,chapter,number)) WITHOUT ROWID");
            exec(q, "CREATE TABLE source_keys(book INTEGER,chapter INTEGER,number INTEGER,retained INTEGER NOT NULL,PRIMARY KEY(book,chapter,number)) WITHOUT ROWID");
            exec(q, "CREATE TABLE source_chapters(book INTEGER,chapter INTEGER,PRIMARY KEY(book,chapter)) WITHOUT ROWID");
            exec(q, "CREATE TABLE book_aliases(book INTEGER,name TEXT,PRIMARY KEY(book,name)) WITHOUT ROWID");
            exec(q, "CREATE TABLE notes(book INTEGER,chapter INTEGER,verse INTEGER,seq INTEGER,kind TEXT NOT NULL,caller TEXT NOT NULL,offset INTEGER NOT NULL,text TEXT NOT NULL,targets TEXT NOT NULL,PRIMARY KEY(book,chapter,verse,seq)) WITHOUT ROWID");
            exec(q, "CREATE TABLE words_of_jesus(book INTEGER,chapter INTEGER,verse INTEGER,start INTEGER,end INTEGER NOT NULL,PRIMARY KEY(book,chapter,verse,start)) WITHOUT ROWID");
            exec(q, "CREATE TABLE indents(book INTEGER,chapter INTEGER,verse INTEGER,levels TEXT NOT NULL,PRIMARY KEY(book,chapter,verse)) WITHOUT ROWID");
            QSqlQuery insert(connection.db), key(connection.db), chapters(connection.db), aliases(connection.db);
            QSqlQuery noteInsert(connection.db), wjInsert(connection.db), indentInsert(connection.db);
            indentInsert.prepare("INSERT INTO indents VALUES(?,?,?,?)");
            noteInsert.prepare("INSERT INTO notes VALUES(?,?,?,?,?,?,?,?,?)"); wjInsert.prepare("INSERT OR IGNORE INTO words_of_jesus VALUES(?,?,?,?,?)");
            int wordsOfJesus = 0, noteCount = 0;
            insert.prepare("INSERT INTO verses VALUES(?,?,?,?,?,?,?,?)"); key.prepare("INSERT INTO source_keys VALUES(?,?,?,?)");
            chapters.prepare("INSERT INTO source_chapters VALUES(?,?)"); aliases.prepare("INSERT OR IGNORE INTO book_aliases VALUES(?,?)");
            QCryptographicHash fingerprint(QCryptographicHash::Sha256);
            for (const auto &book : input.books) {
                q.prepare("INSERT INTO books VALUES(?,?,?,?,?)");
                q.addBindValue(book.id); q.addBindValue(book.code); q.addBindValue(book.name); q.addBindValue(book.abbreviation); q.addBindValue(book.ordinal); run(q);
                const auto &canonical = BibleBooks::canon()[book.id - 1];
                QStringList names = {book.name, book.abbreviation, book.toc1, book.code, QString::fromUtf8(canonical.name)};
                names.append(QString::fromLatin1(canonical.aliases).split(' ', Qt::SkipEmptyParts));
                for (const QString &name : names) if (!name.isEmpty()) { aliases.bindValue(0, book.id); aliases.bindValue(1, name); run(aliases); }
                hashField(fingerprint, book.path.toUtf8()); hashField(fingerprint, book.source.toUtf8());
                parse(book, [&](const Verse &verse, bool retained) {
                    key.bindValue(0, book.id); key.bindValue(1, verse.chapter); key.bindValue(2, verse.number); key.bindValue(3, retained ? 1 : 0); run(key);
                    if (!retained) return;
                    insert.bindValue(0, book.id); insert.bindValue(1, verse.chapter); insert.bindValue(2, verse.number);
                    insert.bindValue(3, verse.text); insert.bindValue(4, verse.html);
                    insert.bindValue(5, verse.heading.isNull() ? QString("") : verse.heading);
                    insert.bindValue(6, verse.poetry ? 1 : 0); insert.bindValue(7, verse.paragraphStart ? 1 : 0); run(insert);
                    ++result.verseCount;
                    if (verse.levels.contains(QRegularExpression("[1-4]"))) {
                        const QVariantList values = {book.id, verse.chapter, verse.number, verse.levels};
                        for (int i = 0; i < values.size(); ++i) indentInsert.bindValue(i, values[i]);
                        run(indentInsert);
                    }
                    int seq = 0;
                    for (const auto &n : verse.notes) {
                        const QVariantList values = {book.id, verse.chapter, verse.number, ++seq, n.kind, n.caller.isNull() ? QString("") : n.caller,
                            n.offset, n.text.isNull() ? QString("") : n.text, n.targets.isNull() ? QString("") : n.targets};
                        for (int i = 0; i < values.size(); ++i) noteInsert.bindValue(i, values[i]);
                        run(noteInsert); ++noteCount;
                    }
                    for (const auto &span : verse.wordsOfJesus) {
                        const QVariantList values = {book.id, verse.chapter, verse.number, span.start, span.end};
                        for (int i = 0; i < values.size(); ++i) wjInsert.bindValue(i, values[i]);
                        run(wjInsert); ++wordsOfJesus;
                    }
                }, [&](int chapter) { chapters.bindValue(0, book.id); chapters.bindValue(1, chapter); run(chapters); });
                if (progress) progress->store(0.9 * book.ordinal / input.books.size());
            }
            QMap<QString, QString> metadata = {{"schema","5"}, {"note_count",QString::number(noteCount)},
                {"words_of_jesus",QString::number(wordsOfJesus)}, {"source_format","usfm"}, {"paragraph_layout","1"},
                {"version_id",input.edition.id}, {"edition",input.edition.name}, {"abbreviation",input.edition.abbreviation},
                {"copyright",input.edition.copyright}, {"book_count",QString::number(input.books.size())},
                {"verse_count",QString::number(result.verseCount)}, {"source_keys",queryDigest(connection.db, {keyQuery})},
                {"source_fingerprint",QString::fromLatin1(fingerprint.result().toHex())}};
            q.prepare("INSERT INTO metadata VALUES(?,?)");
            for (auto it = metadata.cbegin(); it != metadata.cend(); ++it) {
                q.bindValue(0, it.key()); q.bindValue(1, it.value().isNull() ? QString("") : it.value()); run(q);
            }
            q.bindValue(0, "content_digest"); q.bindValue(1, queryDigest(connection.db, digestQueries("5"))); run(q);
            exec(q, "COMMIT"); validate(connection.db);
        }
        QFile database(temporaryPath);
        if (!database.open(QIODevice::ReadOnly)) fail("Cannot reopen validated USFM database.");
        QSaveFile output(target); output.setDirectWriteFallback(false);
        if (!output.open(QIODevice::WriteOnly)) fail("Cannot install USFM database: " + output.errorString());
        while (!database.atEnd()) {
            const QByteArray bytes = database.read(65536);
            if (bytes.isEmpty() && database.error() != QFile::NoError) fail("Cannot read validated USFM database.");
            if (output.write(bytes) != bytes.size()) fail("Cannot write USFM database: " + output.errorString());
        }
        if (!output.commit()) fail("Cannot commit USFM database: " + output.errorString());
        result.ok = true;
        if (progress) progress->store(1);
    } catch (const std::exception &e) { result.error = QString::fromUtf8(e.what()); }
    return result;
}
}
