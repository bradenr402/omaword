#pragma once

#include "singleinstance.h"
#include <QStringList>
#include <QTextStream>

// The command line (`omaword help`). Some commands print and exit without a
// window (quote, search, translations, link, completion, help, --version);
// the rest become a request to the running OmaWord (open, lookup, import, an
// omaword: link, a bare passage), which starts if it is not running yet.
namespace CommandLine {
enum Exit { Success = 0, Failure = 1, Usage = 2 };
struct Terminal {
    bool output = false;    // standard output is a terminal
    bool input = false;     // standard input is a terminal
    bool color = false;     // colors allowed where asked for "auto" (a terminal, NO_COLOR unset)
};
// Whether `arguments` (without the program name) run without a window.
bool headless(const QStringList &arguments);
// Runs a command that needs no window and returns its exit status. `in` feeds
// `quote` and `link` passages, one per line, when they are given as "-" (or
// none are given and it is not a terminal).
int run(const QStringList &arguments, QTextStream &out, QTextStream &err, QTextStream &in, const Terminal &terminal);
// The request for the running OmaWord, or false with `error` (exit status 2).
bool request(const QStringList &arguments, SingleInstance::Request *request, QString *error);
// omaword: links: `omaword:John%203:16`, `omaword:jn316?translation=KJV`,
// `omaword://John%203:16/`. False when `text` is not one; `reference` is
// empty for one naming no passage (`omaword:`, `omaword:?translation=KJV`).
bool parseLink(const QString &text, QString *reference, QString *translation);
// `text` safe to print to a terminal: each control character becomes U+FFFD,
// one for one; line breaks and tabs stay.
QString printable(QString text);
QString link(const QString &reference, const QString &translation = {});
}
