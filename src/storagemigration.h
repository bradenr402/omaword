#pragma once

#include <QLockFile>
#include <QString>
#include <memory>

// The caller must own the new SingleInstance lock before run(), and keep this
// object alive until after the backend closes. No user paths are touched by tests.
class StorageMigration
{
public:
    explicit StorageMigration(const QString &dataDirectory, const QString &runtimeDirectory);
    ~StorageMigration();
    bool run();
    QString errorString() const { return m_error; }

private:
    bool archiveRemaining();
    QString m_data, m_old, m_runtime, m_error;
    std::unique_ptr<QLockFile> m_legacyLock;
    bool m_removeEmptyDirectory = false;
    quint64 m_oldDevice = 0, m_oldInode = 0;
};
