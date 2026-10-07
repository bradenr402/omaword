#pragma once
// The backup an earlier OmaWord wrote (format 5: one version, bookmarks as a
// flag, no times) holding one version of a format 6 backup, so suites can
// keep checking that older backups restore.

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>

inline QJsonObject legacyBackup(const QJsonObject &root, const QString &versionId)
{
    QJsonObject version;
    for (const auto &value : root.value("versions").toArray())
        if (value.toObject().value("versionId").toString() == versionId) version = value.toObject();
    QJsonArray annotations, assignments, tags, desk;
    QSet<QString> used;
    for (const auto &value : version.value("annotations").toArray()) {
        auto annotation = value.toObject();
        annotation.remove("created"); annotation.remove("updated");
        annotation["bookmark"] = false;
        annotations.append(annotation);
    }
    for (const auto &value : version.value("tagAssignments").toArray()) {
        auto assignment = value.toObject();
        assignment.remove("created");
        used.insert(assignment.value("tagId").toString());
        assignments.append(assignment);
    }
    for (const auto &value : root.value("tags").toArray())
        if (used.contains(value.toObject().value("id").toString())) tags.append(value);
    for (const auto &value : root.value("desk").toArray()) {
        auto item = value.toObject();
        if (item.value("versionId").toString() != versionId) continue;
        item.remove("versionId");
        desk.append(item);
    }
    return {{"format","omaword-backup"},{"version",5},{"edition",version.value("edition")},{"versionId",versionId},
            {"created",root.value("created")},{"reading",version.value("reading")},{"annotations",annotations},
            {"tags",tags},{"tagAssignments",assignments},{"desk",desk}};
}
