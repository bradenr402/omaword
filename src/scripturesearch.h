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
// The whole edition, matches in Bible order (book order, chapter, verse).
Result search(const QString &path, const QString &versionId, const QString &abbreviation,
              const QString &query, const Cancellation &cancel = {});
// Test hook: the versions whose prepared text is cached, most recently searched first.
QStringList cachedVersions();
}
