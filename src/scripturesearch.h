#pragma once

#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>
#include <atomic>
#include <memory>

// Value-only engine: safe on a worker thread; no reader objects/connections.
namespace ScriptureSearch {
constexpr int MaximumQuerySize = 4096;
using Cancellation = std::shared_ptr<std::atomic_bool>;
struct Verse { int number = 0; QString text; };
struct Chapter { int book = 0, number = 0; QString name; QVector<Verse> verses; };
struct Result { QString error; QVariantList matches; };
Result find(const Chapter &chapter, const QString &query, bool bibleSyntax = false,
            const Cancellation &cancel = {});
// Chapter find (`/`): `pattern` is a regular expression (QRegularExpression,
// Perl/ECMAScript-like), matched within each verse's source text. Neovim
// smart-case: case-insensitive unless the pattern has an uppercase letter
// (escaped characters such as \S do not count); `\c` anywhere forces
// insensitive, `\C` sensitive (`\c` wins). Empty matches are skipped. An
// invalid pattern yields the error "invalid pattern" and no matches.
Result findPattern(const Chapter &chapter, const QString &pattern, const Cancellation &cancel = {});
// An edition to search: its text database, version ID, and abbreviation.
struct Edition { QString path, versionId, abbreviation; };
// How a Bible search reads its query: Bible syntax (every word in one verse,
// "phrases" across verses), or a regular expression as chapter find takes it
// (findPattern's smart case, \c and \C), matched within each verse, a row per
// verse with every match. A query in slashes, /lov(e|ed)/, is a regular
// expression either way.
enum class Syntax { Bible, Pattern };
// Every edition in the given order, each one's matches in Bible order (book
// order, chapter, verse), tagged with its versionId/versionAbbreviation.
// The search index at indexPath (an FTS5 trigram index of each chapter's
// normalized text, built for an edition the first time it is searched and
// again whenever its file changes) narrows the chapters to scan; matching is
// the same either way. A non-empty `installed` drops every other edition
// from the index.
Result searchEditions(const QVector<Edition> &editions, const QString &query, const QString &indexPath,
                      const QStringList &installed = {}, const Cancellation &cancel = {}, Syntax syntax = Syntax::Bible);
// One edition, through the index at defaultIndexPath().
Result search(const QString &path, const QString &versionId, const QString &abbreviation,
              const QString &query, const Cancellation &cancel = {});
// search-index.sqlite in the cache directory.
QString defaultIndexPath();
// Test hooks: the versions whose prepared text is cached, most recently
// searched first; the versions in an index.
QStringList cachedVersions();
QStringList indexedEditions(const QString &indexPath);
}
