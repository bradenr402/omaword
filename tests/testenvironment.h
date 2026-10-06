#pragma once
// Every test binary includes this header. Before main() runs, it points HOME
// and the XDG user directories at a private temporary directory, so no test
// (however buggy) can read or write the user's real state, such as the live
// Omarchy theme under $XDG_STATE_HOME/omarchy/current/theme.

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QTemporaryDir>
#include <QtGlobal>
#include <cstdio>
#include <cstdlib>

namespace TestEnvironment {

inline const char *const userVariables[] = {"HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME", "XDG_RUNTIME_DIR"};

// The caller's real home, captured before isolation replaces HOME.
inline QString &realHome() { static QString home; return home; }

// True when path lies inside the real user's dot-directories.
inline bool reachesRealUserState(const QString &path) {
    if (realHome().isEmpty() || path.isEmpty()) return false;
    const QString clean = QDir::cleanPath(QDir(path).absolutePath()) + QLatin1Char('/');
    for (const char *sub : {"/.local/", "/.config/", "/.cache/"}) {
        if (clean.startsWith(QDir::cleanPath(realHome()) + QLatin1String(sub))) return true;
    }
    return clean == QDir::cleanPath(realHome()) + QLatin1Char('/');
}

// True when every user directory variable is set and outside the real home's state.
inline bool isolated() {
    for (const char *name : userVariables) {
        const QString value = qEnvironmentVariable(name);
        if (value.isEmpty() || reachesRealUserState(value)) return false;
    }
    return true;
}

inline void isolate() {
    const QByteArray home = qgetenv("HOME");
    realHome() = QString::fromLocal8Bit(home);
    static QTemporaryDir root;
    if (!root.isValid()) { std::fprintf(stderr, "testenvironment: no temporary directory\n"); std::abort(); }
    for (const char *name : userVariables) {
        const QString path = root.filePath(QString::fromLatin1(name));
        QDir().mkpath(path);
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        qputenv(name, QFile::encodeName(path));
    }
    if (!isolated()) { std::fprintf(stderr, "testenvironment: user directories still reach the real home\n"); std::abort(); }
}

} // namespace TestEnvironment

static void omawordIsolateTestEnvironment() { TestEnvironment::isolate(); }
Q_CONSTRUCTOR_FUNCTION(omawordIsolateTestEnvironment)
