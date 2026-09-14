#pragma once

#include <QString>
#include <atomic>
#include <memory>

namespace UsfmImporter {
struct Edition { QString id, abbreviation, name, copyright; };
struct Result {
    bool ok = false;
    QString error;
    int verseCount = 0;
    Edition edition;
};
Edition inspectFile(const QString &path, QString *error = nullptr);
Result importFile(const QString &path, const QString &target,
                  const std::shared_ptr<std::atomic<double>> &progress = {});
bool validateDatabase(const QString &path, QString *error = nullptr);
bool stripLayoutPilcrow(QString *text, QString *html = nullptr);
}
