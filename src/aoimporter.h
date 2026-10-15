#pragma once

#include <QString>
#include <QRegularExpression>
#include <atomic>
#include <memory>

namespace AoImporter {
struct Edition { QString id, abbreviation, name, copyright; };
struct Result {
    bool ok = false;
    QString error;
    int verseCount = 0;
    Edition edition;
};
inline bool validId(const QString &id)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_-]{0,127}$"));
    return pattern.match(id).hasMatch();
}
Edition inspectFile(const QString &path, QString *error = nullptr);
Result importFile(const QString &path, const QString &target,
                  const std::shared_ptr<std::atomic<double>> &progress = {});
bool validateDatabase(const QString &path, QString *error = nullptr);
}
