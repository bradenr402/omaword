#pragma once

#include <QFileSystemWatcher>
#include <sys/types.h>

// Notices an install replacing the executable this process runs (a new file
// renamed over its path, as bin/install and package managers do). A process
// kept alive only for its clipboard (main.cpp) then quits with its last
// window, so the next launch runs the new build instead of reaching this one.
class ExecutableWatch : public QObject
{
    Q_OBJECT
public:
    // path: where the executable is installed; running: a file that is the
    // running image (the process's own, /proc/self/exe; tests pass the file).
    explicit ExecutableWatch(const QString &path, const QString &running = QStringLiteral("/proc/self/exe"),
                             QObject *parent = nullptr);
    // The file at path is no longer the running one (or is gone).
    bool replaced() const;
signals:
    void changed();
private:
    QString m_path;
    dev_t m_device = 0;
    ino_t m_inode = 0;
    bool m_known = false;
    QFileSystemWatcher m_watcher;
};
