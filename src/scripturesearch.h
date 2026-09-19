#pragma once

#include <QString>
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
Result search(const QString &path, const QString &versionId, const QString &abbreviation,
              const QString &query, const Cancellation &cancel = {});
}
