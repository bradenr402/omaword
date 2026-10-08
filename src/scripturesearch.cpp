#include "scripturesearch.h"

#include <QMap>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QMutex>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTextBoundaryFinder>
#include <QUuid>
#include <algorithm>
#include <tuple>

namespace ScriptureSearch {
namespace {
bool stopped(const Cancellation &cancel) { return cancel && cancel->load(std::memory_order_relaxed); }

QString fold(QString scalar)
{
    // Qt uses simple folding. Supply Unicode's full-fold expansions without
    // canonical/compatibility normalization: accents and punctuation stay literal.
    scalar = scalar.toCaseFolded();
    if (scalar.size() != 1) return scalar;
    const ushort c = scalar[0].unicode();
    if ((c >= 0x1f80 && c <= 0x1f87) || (c >= 0x1f90 && c <= 0x1f97)
        || (c >= 0x1fa0 && c <= 0x1fa7)) return QString(QChar(c - 0x80)) + QChar(0x03b9);
    static const QMap<ushort,QString> expansions{
        {0x00df,QStringLiteral("ss")},{0x0130,QStringLiteral("i\u0307")},
        {0x0149,QStringLiteral("\u02bcn")},{0x01f0,QStringLiteral("j\u030c")},
        {0x0390,QStringLiteral("\u03b9\u0308\u0301")},{0x03b0,QStringLiteral("\u03c5\u0308\u0301")},
        {0x0587,QStringLiteral("\u0565\u0582")},{0x1e96,QStringLiteral("h\u0331")},
        {0x1e97,QStringLiteral("t\u0308")},{0x1e98,QStringLiteral("w\u030a")},
        {0x1e99,QStringLiteral("y\u030a")},{0x1e9a,QStringLiteral("a\u02be")},
        {0x1f50,QStringLiteral("\u03c5\u0313")},{0x1f52,QStringLiteral("\u03c5\u0313\u0300")},
        {0x1f54,QStringLiteral("\u03c5\u0313\u0301")},{0x1f56,QStringLiteral("\u03c5\u0313\u0342")},
        {0x1fb2,QStringLiteral("\u1f70\u03b9")},{0x1fb3,QStringLiteral("\u03b1\u03b9")},
        {0x1fb4,QStringLiteral("\u03ac\u03b9")},{0x1fb6,QStringLiteral("\u03b1\u0342")},
        {0x1fb7,QStringLiteral("\u03b1\u0342\u03b9")},{0x1fc2,QStringLiteral("\u1f74\u03b9")},
        {0x1fc3,QStringLiteral("\u03b7\u03b9")},{0x1fc4,QStringLiteral("\u03ae\u03b9")},
        {0x1fc6,QStringLiteral("\u03b7\u0342")},{0x1fc7,QStringLiteral("\u03b7\u0342\u03b9")},
        {0x1fd2,QStringLiteral("\u03b9\u0308\u0300")},{0x1fd3,QStringLiteral("\u03b9\u0308\u0301")},
        {0x1fd6,QStringLiteral("\u03b9\u0342")},{0x1fd7,QStringLiteral("\u03b9\u0308\u0342")},
        {0x1fe2,QStringLiteral("\u03c5\u0308\u0300")},{0x1fe3,QStringLiteral("\u03c5\u0308\u0301")},
        {0x1fe4,QStringLiteral("\u03c1\u0313")},{0x1fe6,QStringLiteral("\u03c5\u0342")},
        {0x1fe7,QStringLiteral("\u03c5\u0308\u0342")},{0x1ff2,QStringLiteral("\u1f7c\u03b9")},
        {0x1ff3,QStringLiteral("\u03c9\u03b9")},{0x1ff4,QStringLiteral("\u03ce\u03b9")},
        {0x1ff6,QStringLiteral("\u03c9\u0342")},{0x1ff7,QStringLiteral("\u03c9\u0342\u03b9")},
        {0xfb00,QStringLiteral("ff")},{0xfb01,QStringLiteral("fi")},{0xfb02,QStringLiteral("fl")},
        {0xfb03,QStringLiteral("ffi")},{0xfb04,QStringLiteral("ffl")},
        {0xfb05,QStringLiteral("st")},{0xfb06,QStringLiteral("st")},
        {0xfb13,QStringLiteral("\u0574\u0576")},{0xfb14,QStringLiteral("\u0574\u0565")},
        {0xfb15,QStringLiteral("\u0574\u056b")},{0xfb16,QStringLiteral("\u057e\u0576")},
        {0xfb17,QStringLiteral("\u0574\u056d")}};
    return expansions.value(c,scalar);
}

struct Span { int start = 0, end = 0; };
struct Normalized {
    QString text;
    QVector<Span> source;
    Span span(int at) const { return source.isEmpty() ? Span{at,at+1} : source[at]; }
    void compact() {
        // Case changes and single-unit whitespace keep identity offsets. Most
        // prose (including smart quotes) needs no retained per-unit map at all.
        for (int i = 0; i < source.size(); ++i)
            if (source[i].start != i || source[i].end != i+1) return;
        QVector<Span>().swap(source);
    }
};
Normalized normalize(const QString &source)
{
    Normalized out;
    out.text.reserve(source.size()); out.source.reserve(source.size());
    // Most chapters are entirely ASCII. Fill both arrays directly, avoiding a
    // grapheme iterator and heap-allocated QString for every source character.
    // CR/LF is the only multi-unit ASCII grapheme; whitespace collapse already
    // gives it one span covering both source units.
    if (std::all_of(source.cbegin(),source.cend(),[](QChar c) { return c.unicode() < 128; })) {
        out.text.resize(source.size()); out.source.resize(source.size());
        auto *text = out.text.data(); auto *map = out.source.data();
        int size = 0;
        for (int i = 0; i < source.size(); ++i) {
            const ushort c = source[i].unicode();
            if (c == ' ' || (c >= '\t' && c <= '\r')) {
                if (size && text[size-1] == ' ') map[size-1].end = i+1;
                else { text[size] = ' '; map[size++] = {i,i+1}; }
            } else {
                text[size] = QChar(c >= 'A' && c <= 'Z' ? c + ('a'-'A') : c);
                map[size++] = {i,i+1};
            }
        }
        out.text.resize(size); out.source.resize(size);
        out.compact();
        return out;
    }
    QTextBoundaryFinder boundaries(QTextBoundaryFinder::Grapheme,source);
    int begin = 0;
    for (int end = boundaries.toNextBoundary(); end >= 0; begin = end, end = boundaries.toNextBoundary()) {
        for (int i = begin; i < end;) {
            const int length = source[i].isHighSurrogate() && i+1 < end && source[i+1].isLowSurrogate() ? 2 : 1;
            if (source[i].isSpace()) {
                if (!out.text.isEmpty() && out.text.back() == ' ') out.source.last().end = end;
                else { out.text += ' '; out.source.append({begin,end}); }
            } else if (source[i].unicode() < 128) {
                const ushort c = source[i].unicode();
                out.text += QChar(c >= 'A' && c <= 'Z' ? c + ('a'-'A') : c);
                // Keep the whole grapheme even for an ASCII base + combining marks.
                out.source.append({begin,end});
            } else {
                const QString folded = fold(source.mid(i,length));
                out.text += folded;
                for (qsizetype j = 0; j < folded.size(); ++j) out.source.append({begin,end});
            }
            i += length;
        }
    }
    out.compact();
    return out;
}
struct Term { QString text; bool phrase = false; };
struct Query { QString error; QVector<Term> terms; };
Query parse(const QString &input, bool syntax)
{
    Query query;
    if (input.size() > MaximumQuerySize) { query.error = "Search queries may contain at most 4096 UTF-16 units."; return query; }
    auto add = [&](const QString &text, bool phrase) {
        const QString normalized = normalize(text).text.trimmed();
        if (normalized.isEmpty()) return;
        for (const auto &term : query.terms) if (term.text == normalized && term.phrase == phrase) return;
        query.terms.append({normalized,phrase});
    };
    if (!syntax) { add(input,true); return query; }
    QString part; bool quoted = false;
    for (qsizetype i = 0; i < input.size(); ++i) {
        const QChar c = input[i];
        if (c == '\\' && i+1 < input.size() && (input[i+1] == '"' || input[i+1] == '\\')) part += input[++i];
        else if (c == '"') { add(part,quoted); part.clear(); quoted = !quoted; }
        else if (!quoted && c.isSpace()) { add(part,false); part.clear(); }
        else part += c;
    }
    if (quoted) { query.error = "Unfinished quoted phrase."; query.terms.clear(); }
    else add(part,false);
    return query;
}
struct Occurrence { int start, end, first, last, term; };

// Sweep right endpoints, retaining the latest left endpoint for each label.
// Equal-left later covers enclose an existing result and are discarded. This
// avoids a Cartesian product even with many repeated phrases/overlapping hits.
QVector<Span> covers(QVector<std::tuple<int,int,int>> events, int labels)
{
    std::sort(events.begin(),events.end());
    QVector<int> latest(labels,-1); QVector<Span> result;
    for (qsizetype i = 0; i < events.size();) {
        const int end = std::get<0>(events[i]);
        do {
            const auto [right,left,label] = events[i++];
            Q_UNUSED(right);
            latest[label] = std::max(latest[label],left);
        } while (i < events.size() && std::get<0>(events[i]) == end);
        const int start = *std::min_element(latest.cbegin(),latest.cend());
        if (start >= 0 && (result.isEmpty() || start > result.last().start)) result.append({start,end});
    }
    return result;
}

struct PreparedChapter {
    Chapter chapter;
    QString source;
    QVector<int> starts, ends;
    Normalized normalized;
    explicit PreparedChapter(Chapter input) : chapter(std::move(input)) {
        for (auto &verse : chapter.verses) {
            if (!source.isEmpty()) source += ' ';
            starts.append(source.size()); source += verse.text; ends.append(source.size());
            verse.text.clear(); // The flattened source is the sole retained copy.
        }
        normalized = normalize(source);
    }
    qint64 bytes() const {
        return sizeof(*this) + 256 + chapter.name.capacity()*2 + chapter.verses.capacity()*sizeof(Verse)
            + source.capacity()*2 + normalized.text.capacity()*2 + normalized.source.capacity()*sizeof(Span)
            + (starts.capacity()+ends.capacity())*sizeof(int);
    }
};
struct Corpus {
    QVector<PreparedChapter> chapters; qint64 bytes = 0;
    QHash<qint64,int> positions; // book * 1000 + chapter → its index in chapters
    const PreparedChapter *find(int book, int chapter) const {
        const auto at = positions.constFind(qint64(book) * 1000 + chapter);
        return at == positions.cend() ? nullptr : &chapters[*at];
    }
};
// Two immutable generations (two windows reading different versions keep
// theirs), each at most 64 MiB, the least recently searched evicted. Only
// worker value data is shared, never SQL handles. In-flight readers own their
// snapshot after eviction.
QMutex corpusMutex;
struct CachedCorpus { QString identity, versionId; std::shared_ptr<const Corpus> corpus; };
QList<CachedCorpus> cachedCorpora; // Most recently searched first.
constexpr int CachedCorpora = 2;
constexpr qint64 MaximumCorpusBytes = 64 * 1024 * 1024;

Result scan(const PreparedChapter &prepared, const Query &query, bool syntax, const Cancellation &cancel)
{
    Result result{query.error,{}};
    const auto &chapter = prepared.chapter;
    if (!result.error.isEmpty() || query.terms.isEmpty() || chapter.verses.isEmpty() || stopped(cancel)) return result;
    const auto &source = prepared.source;
    const auto &starts = prepared.starts, &ends = prepared.ends;
    const auto &normalized = prepared.normalized;
    QVector<Occurrence> occurrences;
    QVector<int> phraseLabels(query.terms.size(),-1); int phrases = 0;
    for (int term = 0; term < query.terms.size(); ++term) {
        if (query.terms[term].phrase) phraseLabels[term] = phrases++;
        const auto &needle = query.terms[term].text;
        const qsizetype previousCount = occurrences.size();
        int previousStart = -1, previousEnd = -1;
        for (qsizetype at = normalized.text.indexOf(needle); at >= 0; at = normalized.text.indexOf(needle,at+1)) {
            if (stopped(cancel)) return {};
            const int start = normalized.span(at).start, end = normalized.span(at+needle.size()-1).end;
            const int first = std::upper_bound(ends.cbegin(),ends.cend(),start) - ends.cbegin();
            const int last = std::lower_bound(starts.cbegin(),starts.cend(),end) - starts.cbegin() - 1;
            if (first > last || first >= chapter.verses.size() || last < 0) continue;
            if (syntax && !query.terms[term].phrase && first != last) continue;
            // A full-fold expansion or an emoji cluster can yield several
            // normalized hits on the identical source grapheme. Navigate once.
            if (start == previousStart && end == previousEnd) continue;
            previousStart = start; previousEnd = end;
            occurrences.append({start,end,first,last,term});
        }
        if (occurrences.size() == previousCount) return result;
    }
    std::stable_sort(occurrences.begin(),occurrences.end(),[](const auto &a, const auto &b) { return a.start < b.start; });
    QVector<Span> ranges;
    if (!syntax) {
        for (const auto &hit : occurrences) ranges.append({hit.first,hit.last});
    } else if (phrases) {
        QVector<std::tuple<int,int,int>> events;
        for (const auto &hit : occurrences) if (phraseLabels[hit.term] >= 0)
            events.append({hit.last,hit.first,phraseLabels[hit.term]});
        ranges = covers(events,phrases);
    } else {
        for (int i = 0; i < chapter.verses.size(); ++i) ranges.append({i,i});
    }
    for (qsizetype r = 0; r < ranges.size(); ++r) {
        if (stopped(cancel)) return {};
        const auto range = ranges[r];
        QVector<Span> highlights;
        QVector<bool> found(query.terms.size(),false);
        auto collect = [&](const Occurrence &hit) {
            found[hit.term] = true;
            highlights.append({hit.start,hit.end});
        };
        if (!syntax) collect(occurrences[r]);
        else {
            const auto begin = std::lower_bound(occurrences.cbegin(),occurrences.cend(),starts[range.start],
                [](const Occurrence &hit, int start) { return hit.start < start; });
            for (auto it = begin; it != occurrences.cend() && it->start < ends[range.end]; ++it)
                if (it->first >= range.start && it->last <= range.end) collect(*it);
        }
        if (std::find(found.cbegin(),found.cend(),false) != found.cend()) continue;
        QVector<Span> merged;
        for (const auto &span : highlights) {
            if (!merged.isEmpty() && span.start <= merged.last().end) merged.last().end = std::max(merged.last().end,span.end);
            else merged.append(span);
        }
        QVariantList segments, displaySegments;
        for (const auto &span : merged) {
            for (int v = range.start; v <= range.end; ++v) {
                const int begin = std::max(span.start,starts[v]), end = std::min(span.end,ends[v]);
                if (end <= begin) continue;
                segments.append(QVariantMap{{QStringLiteral("verse"),chapter.verses[v].number},{QStringLiteral("start"),begin-starts[v]},{QStringLiteral("length"),end-begin}});
                displaySegments.append(QVariantMap{{QStringLiteral("start"),begin-starts[range.start]},{QStringLiteral("length"),end-begin}});
            }
        }
        const int first = chapter.verses[range.start].number, last = chapter.verses[range.end].number;
        QString reference = chapter.name + ' ' + QString::number(chapter.number) + ':' + QString::number(first);
        if (first != last) reference += QChar(0x2013) + QString::number(last);
        result.matches.append(QVariantMap{{QStringLiteral("book"),chapter.book},{QStringLiteral("chapter"),chapter.number},{QStringLiteral("start"),first},
            {QStringLiteral("endBook"),chapter.book},{QStringLiteral("endChapter"),chapter.number},{QStringLiteral("end"),last},{QStringLiteral("reference"),reference},
            {QStringLiteral("segments"),segments},{QStringLiteral("text"),source.mid(starts[range.start],ends[range.end]-starts[range.start])},
            {QStringLiteral("displaySegments"),displaySegments},{QStringLiteral("bookName"),chapter.name}});
    }
    return result;
}
}

Result find(const Chapter &chapter, const QString &query, bool bibleSyntax, const Cancellation &cancel)
{
    const auto parsed = parse(query,bibleSyntax);
    if (!parsed.error.isEmpty() || parsed.terms.isEmpty() || stopped(cancel)) return {parsed.error,{}};
    return scan(PreparedChapter(chapter),parsed,bibleSyntax,cancel);
}

Result findPattern(const Chapter &chapter, const QString &input, const Cancellation &cancel)
{
    if (input.size() > MaximumQuerySize) return {QStringLiteral("Search queries may contain at most 4096 UTF-16 units."),{}};
    // Strip \c / \C (keeping an escaped backslash), and look for an
    // unescaped uppercase letter for smart-case.
    QString pattern; pattern.reserve(input.size());
    bool forceInsensitive = false, forceSensitive = false, upper = false;
    for (qsizetype i = 0; i < input.size(); ++i) {
        const QChar c = input[i];
        if (c == '\\' && i+1 < input.size()) {
            const QChar next = input[i+1];
            if (next == 'c') { forceInsensitive = true; ++i; continue; }
            if (next == 'C') { forceSensitive = true; ++i; continue; }
            pattern += c; pattern += next; ++i;
            // \p{Lu} / \P{Lu}: the property name is not a typed letter.
            if ((next == 'p' || next == 'P') && i+1 < input.size() && input[i+1] == '{') {
                const qsizetype close = input.indexOf('}',i+1);
                if (close > 0) { pattern += input.mid(i+1,close-i); i = close; }
            }
            continue;
        }
        if (c.isUpper()) upper = true;
        pattern += c;
    }
    if (pattern.isEmpty() || stopped(cancel)) return {};
    const bool insensitive = forceInsensitive || (!forceSensitive && !upper);
    QRegularExpression expression(pattern, QRegularExpression::UseUnicodePropertiesOption
        | (insensitive ? QRegularExpression::CaseInsensitiveOption : QRegularExpression::NoPatternOption));
    if (!expression.isValid()) return {QStringLiteral("invalid pattern"),{}};
    Result result;
    for (const auto &verse : chapter.verses) {
        auto it = expression.globalMatch(verse.text);
        while (it.hasNext()) {
            if (stopped(cancel)) return {};
            const auto match = it.next();
            if (match.capturedLength() <= 0) continue;
            const int start = int(match.capturedStart()), length = int(match.capturedLength());
            const QString reference = chapter.name + ' ' + QString::number(chapter.number) + ':' + QString::number(verse.number);
            result.matches.append(QVariantMap{{QStringLiteral("book"),chapter.book},{QStringLiteral("chapter"),chapter.number},
                {QStringLiteral("start"),verse.number},{QStringLiteral("endBook"),chapter.book},{QStringLiteral("endChapter"),chapter.number},
                {QStringLiteral("end"),verse.number},{QStringLiteral("reference"),reference},
                {QStringLiteral("segments"),QVariantList{QVariantMap{{QStringLiteral("verse"),verse.number},{QStringLiteral("start"),start},{QStringLiteral("length"),length}}}},
                {QStringLiteral("text"),verse.text},
                {QStringLiteral("displaySegments"),QVariantList{QVariantMap{{QStringLiteral("start"),start},{QStringLiteral("length"),length}}}}});
        }
    }
    return result;
}

// The search index: an FTS5 trigram table of every chapter's normalized text
// (its verses joined as PreparedChapter joins them), per edition, with each
// edition's identity when indexed. A substring of the normalized chapter is
// exactly what scan() looks for, so the chapters a query's terms of three or
// more characters MATCH are all the chapters it can match in. The index is a
// disposable cache: another format, a file it cannot open, or an edition
// whose file changed is built again. Searches in every window share it, one
// at a time.
namespace {
constexpr int IndexFormat = 1;
QMutex indexMutex;
struct IndexConnection {
    QString name = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSqlDatabase db;
    explicit IndexConnection(const QString &path) {
        db = QSqlDatabase::addDatabase("QSQLITE",name);
        db.setDatabaseName(path); db.setConnectOptions("QSQLITE_BUSY_TIMEOUT=5000");
    }
    ~IndexConnection() { db.close(); db = {}; QSqlDatabase::removeDatabase(name); }
};
QString prepareIndex(QSqlDatabase &db)
{
    if (!db.open()) return "Cannot open the search index: " + db.lastError().text();
    QSqlQuery q(db);
    if (!q.exec("PRAGMA user_version") || !q.next()) return "Cannot read the search index: " + q.lastError().text();
    const int format = q.value(0).toInt();
    q.finish();
    if (format == IndexFormat) return {};
    for (const QString &sql : {QStringLiteral("DROP TABLE IF EXISTS chapters"), QStringLiteral("DROP TABLE IF EXISTS editions"),
            QStringLiteral("CREATE TABLE editions(version_id TEXT PRIMARY KEY,identity TEXT NOT NULL) WITHOUT ROWID"),
            QStringLiteral("CREATE VIRTUAL TABLE chapters USING fts5(text,version_id UNINDEXED,book UNINDEXED,chapter UNINDEXED,"
                           "ordinal UNINDEXED,tokenize='trigram case_sensitive 1')"),
            QStringLiteral("PRAGMA user_version=%1").arg(IndexFormat)})
        if (!q.exec(sql)) return "Cannot create the search index: " + q.lastError().text();
    return {};
}
// Opens the index at path, starting a new file when the old one cannot be used.
QString openIndex(IndexConnection &index, const QString &path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QString error = prepareIndex(index.db);
    if (error.isEmpty()) return {};
    index.db.close();
    for (const QString &file : {path, path + "-wal", path + "-shm"}) QFile::remove(file);
    return prepareIndex(index.db);
}
// The FTS5 query for the terms the index can find (three characters or more),
// or "" when none can narrow the chapters.
QString indexQuery(const Query &query)
{
    QStringList terms;
    for (const auto &term : query.terms)
        if (term.text.toUcs4().size() >= 3) terms << '"' + QString(term.text).replace('"',"\"\"") + '"';
    return terms.join(" AND ");
}
std::shared_ptr<const Corpus> cachedCorpus(const QString &identity)
{
    QMutexLocker lock(&corpusMutex);
    for (qsizetype i = 0; i < cachedCorpora.size(); ++i) {
        if (cachedCorpora[i].identity != identity) continue;
        cachedCorpora.move(i,0);
        return cachedCorpora.first().corpus;
    }
    return {};
}
void cacheCorpus(const QString &identity, const QString &versionId, std::shared_ptr<Corpus> corpus)
{
    QMutexLocker lock(&corpusMutex);
    cachedCorpora.removeIf([&](const CachedCorpus &entry) { return entry.identity == identity; });
    cachedCorpora.prepend({identity,versionId,std::move(corpus)});
    while (cachedCorpora.size() > CachedCorpora) cachedCorpora.removeLast();
}

// One edition: every match in Bible order (the edition's book order, then
// chapter and verse).
Result searchEdition(const Edition &edition, const Query &parsed, const QString &indexPath, const Cancellation &cancel)
{
    Result result;
    const QString connection = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto cleanup = qScopeGuard([&] { QSqlDatabase::removeDatabase(connection); });
    auto db = QSqlDatabase::addDatabase("QSQLITE",connection);
    db.setDatabaseName(edition.path); db.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=1000");
    if (!db.open()) return {"Cannot open Scripture search database: " + db.lastError().text(),{}};
    QSqlQuery q(db);
    q.prepare("SELECT value FROM metadata WHERE key='version_id'");
    if (!q.exec() || !q.next() || q.value(0).toString() != edition.versionId) return {"Scripture search version is no longer available.",{}};
    q.finish();
    // Installed paths are immutable generation UUIDs. Opening read-only and
    // checking identity even on a hit prevents deleted/reimported files from
    // being served solely from a cache. No connection survives this request.
    const QFileInfo file(edition.path);
    const QString identity = file.absoluteFilePath() + '\n' + edition.versionId + '\n'
        + QString::number(file.size()) + ':' + QString::number(file.lastModified().toMSecsSinceEpoch());
    const auto corpus = cachedCorpus(identity);
    QMutexLocker lock(&indexMutex);
    IndexConnection index(indexPath);
    if (const QString error = openIndex(index,indexPath); !error.isEmpty()) return {error,{}};
    QSqlQuery iq(index.db);
    iq.prepare("SELECT identity FROM editions WHERE version_id=?"); iq.addBindValue(edition.versionId);
    if (!iq.exec()) return {"Cannot read the search index: " + iq.lastError().text(),{}};
    const bool indexed = iq.next() && iq.value(0).toString() == identity;
    iq.finish();
    if (indexed) {
        // The index names the chapters to scan, in Bible order.
        const QString match = indexQuery(parsed);
        iq.prepare(match.isEmpty() ? QStringLiteral("SELECT book,chapter FROM chapters WHERE version_id=? ORDER BY ordinal")
                                   : QStringLiteral("SELECT book,chapter FROM chapters WHERE version_id=? AND chapters MATCH ? ORDER BY ordinal"));
        iq.addBindValue(edition.versionId);
        if (!match.isEmpty()) iq.addBindValue(match);
        if (!iq.exec()) return {"Cannot read the search index: " + iq.lastError().text(),{}};
        QVector<QPair<int,int>> chapters;
        while (iq.next()) chapters.append({iq.value(0).toInt(),iq.value(1).toInt()});
        if (iq.lastError().isValid()) return {"Cannot read the search index: " + iq.lastError().text(),{}};
        iq.finish();
        lock.unlock();
        QHash<int,QString> names;
        if (!corpus) {
            if (!q.exec("SELECT id,name FROM books")) return {"Cannot read Scripture search text: " + q.lastError().text(),{}};
            while (q.next()) names.insert(q.value(0).toInt(),q.value(1).toString());
            q.prepare("SELECT number,text FROM verses WHERE book=? AND chapter=? ORDER BY number");
        }
        for (const auto &[book,number] : std::as_const(chapters)) {
            if (stopped(cancel)) return {};
            if (const auto *prepared = corpus ? corpus->find(book,number) : nullptr) {
                result.matches.append(scan(*prepared,parsed,true,cancel).matches);
                continue;
            }
            if (corpus) continue; // Indexed but absent from a complete copy: nothing to find.
            Chapter chapter{book,number,names.value(book),{}};
            q.addBindValue(book); q.addBindValue(number);
            if (!q.exec()) return {"Cannot read Scripture search text: " + q.lastError().text(),{}};
            while (q.next()) chapter.verses.append({q.value(0).toInt(),q.value(1).toString()});
            if (q.lastError().isValid()) return {"Cannot read complete Scripture search text: " + q.lastError().text(),{}};
            result.matches.append(scan(PreparedChapter(std::move(chapter)),parsed,true,cancel).matches);
        }
        return result;
    }
    // Not indexed yet, or its file changed: read every chapter once, scanning
    // it, indexing it, and keeping it prepared for the next searches.
    if (!index.db.transaction()) return {"Cannot update the search index: " + index.db.lastError().text(),{}};
    const auto rollback = qScopeGuard([&] { index.db.rollback(); });
    iq.prepare("DELETE FROM chapters WHERE version_id=?"); iq.addBindValue(edition.versionId);
    if (!iq.exec()) return {"Cannot update the search index: " + iq.lastError().text(),{}};
    iq.prepare("INSERT INTO chapters(text,version_id,book,chapter,ordinal) VALUES(?,?,?,?,?)");
    q.setForwardOnly(true);
    if (!q.exec("SELECT v.book,v.chapter,v.number,v.text,b.name,b.ordinal FROM verses v JOIN books b ON b.id=v.book ORDER BY b.ordinal,v.chapter,v.number"))
        return {"Cannot read Scripture search text: " + q.lastError().text(),{}};
    auto building = std::make_shared<Corpus>();
    bool keep = true;
    Chapter chapter; int ordinal = 0;
    QString error;
    auto flush = [&] {
        if (chapter.verses.isEmpty() || !error.isEmpty() || stopped(cancel)) return;
        PreparedChapter prepared(std::move(chapter));
        result.matches.append(scan(prepared,parsed,true,cancel).matches);
        iq.addBindValue(prepared.normalized.text); iq.addBindValue(edition.versionId);
        iq.addBindValue(prepared.chapter.book); iq.addBindValue(prepared.chapter.number);
        iq.addBindValue(qint64(ordinal) * 1000 + prepared.chapter.number);
        if (!iq.exec()) { error = "Cannot update the search index: " + iq.lastError().text(); return; }
        if (keep) {
            building->bytes += prepared.bytes();
            if (building->bytes > MaximumCorpusBytes) keep = false;
            else {
                building->positions.insert(qint64(prepared.chapter.book) * 1000 + prepared.chapter.number,int(building->chapters.size()));
                building->chapters.append(std::move(prepared));
            }
        }
    };
    while (q.next()) {
        if (stopped(cancel)) return {};
        const int book = q.value(0).toInt(), number = q.value(1).toInt();
        if (book != chapter.book || number != chapter.number) {
            flush(); chapter = {book,number,q.value(4).toString(),{}}; ordinal = q.value(5).toInt();
        }
        chapter.verses.append({q.value(2).toInt(),q.value(3).toString()});
    }
    if (q.lastError().isValid()) return {"Cannot read complete Scripture search text: " + q.lastError().text(),{}};
    flush();
    if (!error.isEmpty()) return {error,{}};
    if (stopped(cancel)) return {};
    iq.prepare("INSERT INTO editions(version_id,identity) VALUES(?,?) ON CONFLICT(version_id) DO UPDATE SET identity=excluded.identity");
    iq.addBindValue(edition.versionId); iq.addBindValue(identity);
    if (!iq.exec() || !index.db.commit()) return {"Cannot update the search index: " + index.db.lastError().text(),{}};
    if (keep && QFileInfo::exists(edition.path)) cacheCorpus(identity,edition.versionId,std::move(building));
    return result;
}
}

QString defaultIndexPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/search-index.sqlite");
}

Result searchEditions(const QVector<Edition> &editions, const QString &query, const QString &indexPath,
                      const QStringList &installed, const Cancellation &cancel)
{
    const Query parsed = parse(query,true);
    Result result{parsed.error,{}};
    if (!result.error.isEmpty() || parsed.terms.isEmpty() || stopped(cancel)) return result;
    if (!installed.isEmpty()) {
        // Editions no longer installed leave the index.
        QMutexLocker lock(&indexMutex);
        IndexConnection index(indexPath);
        if (const QString error = openIndex(index,indexPath); !error.isEmpty()) return {error,{}};
        QSqlQuery q(index.db);
        QStringList gone;
        if (!q.exec("SELECT version_id FROM editions")) return {"Cannot read the search index: " + q.lastError().text(),{}};
        while (q.next()) if (!installed.contains(q.value(0).toString())) gone << q.value(0).toString();
        q.finish();
        for (const auto &id : std::as_const(gone)) {
            for (const QString table : {"chapters","editions"}) {
                q.prepare("DELETE FROM " + table + " WHERE version_id=?"); q.addBindValue(id);
                if (!q.exec()) return {"Cannot update the search index: " + q.lastError().text(),{}};
            }
        }
    }
    for (const auto &edition : editions) {
        if (stopped(cancel)) return {};
        auto found = searchEdition(edition,parsed,indexPath,cancel);
        if (!found.error.isEmpty()) return found;
        if (stopped(cancel)) return {};
        for (auto &row : found.matches) {
            // These are engine-owned QVariantMaps. Mutate the stored map directly;
            // toMap() would share it and then detach/copy every map node.
            auto &map = *static_cast<QVariantMap *>(row.data());
            map.insert(QStringLiteral("versionId"),edition.versionId); map.insert(QStringLiteral("versionAbbreviation"),edition.abbreviation);
        }
        result.matches.append(found.matches);
    }
    return result;
}

Result search(const QString &path, const QString &versionId, const QString &abbreviation,
              const QString &query, const Cancellation &cancel)
{
    return searchEditions({{path,versionId,abbreviation}},query,defaultIndexPath(),{},cancel);
}

QStringList indexedEditions(const QString &indexPath)
{
    QMutexLocker lock(&indexMutex);
    IndexConnection index(indexPath);
    QStringList editions;
    if (!openIndex(index,indexPath).isEmpty()) return editions;
    QSqlQuery q(index.db);
    if (q.exec("SELECT version_id FROM editions ORDER BY version_id")) while (q.next()) editions << q.value(0).toString();
    return editions;
}
}

QStringList ScriptureSearch::cachedVersions()
{
    QMutexLocker lock(&corpusMutex);
    QStringList versions;
    for (const auto &entry : std::as_const(cachedCorpora)) versions << entry.versionId;
    return versions;
}
