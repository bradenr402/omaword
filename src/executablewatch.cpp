#include "executablewatch.h"

#include <QFile>
#include <QFileInfo>
#include <sys/stat.h>

ExecutableWatch::ExecutableWatch(const QString &path, const QString &running, QObject *parent)
    : QObject(parent), m_path(path)
{
    struct stat image {};
    m_known = !path.isEmpty() && ::stat(QFile::encodeName(running).constData(), &image) == 0;
    m_device = image.st_dev;
    m_inode = image.st_ino;
    // The directory, not the file: a file renamed over the path is a new
    // entry there, while a watch on the old file sees only its link count.
    if (m_known) m_watcher.addPath(QFileInfo(path).absolutePath());
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { if (replaced()) emit changed(); });
}

bool ExecutableWatch::replaced() const
{
    if (!m_known) return false;
    struct stat now {};
    return ::stat(QFile::encodeName(m_path).constData(), &now) != 0 || now.st_dev != m_device || now.st_ino != m_inode;
}
