#include "commandline.h"
#include "biblebackend.h"
#include "brand.h"
#include "scripturesearch.h"

#include <QCommandLineParser>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>
#include <algorithm>

// Q_INIT_RESOURCE cannot be used inside a namespace.
static void initCoreResources() { Q_INIT_RESOURCE(core); }

namespace CommandLine {
namespace {
struct Option { QString flags, text; };
struct Command {
    QString name, arguments, summary, description;
    QList<Option> options;
    QStringList examples;
    bool headless;
};
const Option translationOption{QStringLiteral("-t, --translation <name>"),
    QStringLiteral("Use this translation: its abbreviation (NET) or ID (engnet), in any case. Default: the one you read.")};
const Option helpOption{QStringLiteral("-h, --help"), QStringLiteral("Show this help.")};

const QList<Command> &commands()
{
    static const QList<Command> list{
        {"open", "<passage> [--here] [-t <name>]", "Open a passage in a window",
         "Opens the passage in a new OmaWord window, starting OmaWord if it is not running. With --here it opens in "
         "the window you were reading instead. A translation other than the window's switches the window to it.",
         {translationOption, {"    --here", "Open in the window you were reading, not a new one."}, helpOption},
         {"omaword open jn316", "omaword open \"Romans 8\" --here", "omaword open ps23 -t KJV"}, false},
        {"lookup", "[<text>]", "Open the passage palette",
         "Opens the passage palette in the window you were reading, ready to type, with <text> typed into it if "
         "given. Suits a key binding, such as Hyprland's:\n  bindd = SUPER ALT, B, Look up a passage, exec, omaword lookup",
         {helpOption}, {"omaword lookup", "omaword lookup rom 8"}, false},
        {"quote", "<passage>... [--plain] [--json] [-t <name>]", "Print passages as quotations",
         "Prints each passage as OmaWord copies it: the text in quotation marks, a number before each verse, and the "
         "reference after an em dash, with a blank line between passages. Given \"-\" (or no passage while input is "
         "piped in), it reads passages from standard input, one per line. No window opens, and your reading place "
         "stays where it was.",
         {translationOption, {"    --plain", "Print the text alone: no quotation marks, verse numbers, or reference."},
          {"    --json", "Print a JSON array: each passage's reference, translation, text, and verses."}, helpOption},
         {"omaword quote jn316", "omaword quote ps23 -t KJV --plain | wl-copy", "omaword quote \"Genesis 1:1-3\" \"John 1:1-5\"",
          "printf 'jn316\\nrom 8:28\\n' | omaword quote"}, true},
        {"search", "<words>... [--all | -t <name>] [--limit <n>] [--json]", "Search the Bible",
         "Searches as OmaWord's search does: every word in one verse, partial words included, case ignored; a \"quoted "
         "phrase\" may run across verses. Words in slashes are a regular expression, as in chapter find: matched within "
         "one verse, case ignored unless it has a capital letter (\\c ignores case, \\C keeps it). Matches print in "
         "Bible order, the matched words emphasized on a terminal. The exit status is 1 when nothing matches.",
         {translationOption, {"    --all", "Search every installed translation, the one you read first."},
          {"    --limit <n>", "Print at most n matches (0: all). Default: 20 on a terminal, all otherwise."},
          {"    --color <when>", "Emphasize matches: auto (on a terminal, unless NO_COLOR is set), always, or never."},
          {"    --json", "Print a JSON array: each match's reference, translation, text, and matched spans."}, helpOption},
         {"omaword search living water", "omaword search '\"the word became flesh\"' --all", "omaword search '/lov(e|ed) the world/'",
          "omaword search grace --limit 0 | wc -l"}, true},
        {"translations", "[--names] [--json]", "List the installed translations",
         "Lists the installed translations with their abbreviations and IDs; * marks the one you read. "
         "(omaword versions is the same.)",
         {{"    --names", "Print the abbreviations alone, one per line."}, {"    --json", "Print a JSON array."}, helpOption},
         {"omaword translations"}, true},
        {"link", "<passage>... [-t <name>]", "Print an omaword: link to a passage",
         "Prints a link that opens the passage in OmaWord, in the window you were reading: from a web page, a note "
         "elsewhere, or a script. Links look like omaword:John%203:16, or omaword:John%203:16?translation=KJV for a "
         "translation.",
         {translationOption, helpOption}, {"omaword link jn316", "xdg-open \"$(omaword link ps23 -t KJV)\""}, true},
        {"import", "<file>", "Install a translation from a USFM ZIP or file",
         "Installs a translation, or replaces it with a newer copy, from a USFM ZIP archive or a single USFM file. "
         "Add version in OmaWord installs the catalog's translations directly.",
         {helpOption}, {"omaword import ~/Downloads/engwebp_usfm.zip"}, false},
        {"completion", "<bash|zsh|fish>", "Print a shell completion script",
         "Prints the completion script for a shell; bin/install installs bash's. For zsh, save it in a directory on "
         "$fpath, such as ~/.zfunc (with fpath+=(~/.zfunc) before compinit in ~/.zshrc):\n"
         "  omaword completion zsh > ~/.zfunc/_omaword\nFor fish:\n"
         "  omaword completion fish > ~/.config/fish/completions/omaword.fish",
         {helpOption}, {"omaword completion bash"}, true},
        {"help", "[<command>]", "Show help for OmaWord or a command", "", {}, {"omaword help quote"}, true},
    };
    return list;
}

const Command *find(const QString &name)
{
    const QString key = name == QStringLiteral("versions") ? QStringLiteral("translations") : name;
    for (const auto &command : commands()) if (command.name == key) return &command;
    return nullptr;
}

// What the command line prints names the command, its links, and the app as
// branding.env does.
QString branded(QString text)
{
    return text.replace(QStringLiteral("omaword"), Brand::appId()).replace(QStringLiteral("OmaWord"), Brand::name());
}

// Lines of at most `width` columns, each after `indent`.
QString wrap(const QString &text, int width, int indent)
{
    QStringList out;
    for (const QString &paragraph : text.split('\n')) {
        if (paragraph.startsWith(QStringLiteral("  "))) { out << QString(indent, ' ') + paragraph; continue; } // preformatted
        QString line;
        for (const QString &word : paragraph.split(' ', Qt::SkipEmptyParts)) {
            if (!line.isEmpty() && indent + line.size() + 1 + word.size() > width) { out << QString(indent, ' ') + line; line.clear(); }
            line += (line.isEmpty() ? QString() : QStringLiteral(" ")) + word;
        }
        out << QString(indent, ' ') + line;
    }
    return out.join('\n');
}

QString tableRow(const QString &left, const QString &right, int column, int width = 80)
{
    QString text = wrap(right, width, column);
    const QString head = QStringLiteral("  ") + left;
    if (head.size() + 2 > column) return head + '\n' + text;
    return head + text.mid(head.size());
}

void printHelp(QTextStream &out)
{
    QString text = Brand::name() + ' ' + Brand::version() + QStringLiteral(" \u2014 a quiet, offline Bible reader for Omarchy\n\nUsage:\n")
        + tableRow("omaword", "Start OmaWord, or bring it forward", 28) + '\n'
        + tableRow("omaword <passage>", "Open a passage in a new window", 28) + '\n'
        + tableRow("omaword <command> ...", "Run a command (below)", 28) + "\n\nCommands:\n";
    for (const auto &command : commands()) text += tableRow(command.name, command.summary, 16) + '\n';
    text += "\nOptions:\n"
        + tableRow(translationOption.flags, "The translation for open, quote, search, and link: its abbreviation or ID. "
                                            "Default: the one you read.", 28) + '\n'
        + tableRow("-h, --help", "Show help (omaword help <command>: a command's)", 28) + '\n'
        + tableRow("-v, --version", "Show OmaWord's version", 28) + "\n\n"
        + "Passages:\n" + wrap("Full or compact references, as OmaWord's passage palette reads them: \"John 3:16\", jn316, "
                               "1co13, \"rev 3 14-22\", ps23 (a whole chapter), \"Genesis 1:1-2:3\".", 80, 2) + "\n\n"
        + "Links:\n" + wrap("omaword:John%203:16 (or omaword:jn316?translation=KJV) opens the passage where you were "
                            "reading; omaword link prints one. xdg-open and browsers hand them to OmaWord.", 80, 2) + "\n\n"
        + "Examples:\n"
        + "  omaword quote jn316\n  omaword quote ps23 -t KJV --plain | wl-copy\n  omaword search \"living water\" --all\n"
        + "  omaword open \"Romans 8\" --here\n\n"
        + "Exit status:\n" + wrap("0 on success; 1 when it fails (an unknown passage or translation, no match, nothing "
                                  "installed); 2 for a usage error.", 80, 2) + '\n';
    out << branded(text);
    out.flush();
}

void printHelp(QTextStream &out, const Command &command)
{
    QString text = "omaword " + command.name + " \u2014 " + command.summary + "\n\nUsage:\n  omaword " + command.name;
    if (!command.arguments.isEmpty()) text += ' ' + command.arguments;
    text += '\n';
    if (!command.description.isEmpty()) text += '\n' + wrap(command.description, 80, 0) + '\n';
    if (!command.options.isEmpty()) {
        text += "\nOptions:\n";
        for (const auto &option : command.options) text += tableRow(option.flags, option.text, 30) + '\n';
    }
    if (!command.examples.isEmpty()) {
        text += "\nExamples:\n";
        for (const auto &example : command.examples) text += "  " + example + '\n';
    }
    out << branded(text);
    out.flush();
}

int usage(QTextStream &err, const QString &command, const QString &message)
{
    const QString name = command.isEmpty() ? QString() : ' ' + command;
    err << branded("omaword" + name + ": " + message + "\nRun 'omaword help" + name + "' for usage.") << Qt::endl;
    return Usage;
}
int failure(QTextStream &err, const QString &command, const QString &message)
{
    err << branded("omaword " + command + ": ") << message << Qt::endl;
    return Failure;
}

int distance(const QString &a, const QString &b)
{
    QList<int> row(b.size() + 1);
    for (int j = 0; j <= b.size(); ++j) row[j] = j;
    for (int i = 1; i <= a.size(); ++i) {
        int diagonal = row[0];
        row[0] = i;
        for (int j = 1; j <= b.size(); ++j) {
            const int above = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diagonal = above;
        }
    }
    return row[b.size()];
}
// A mistyped command (no digits anywhere, within two edits of one): "qoute".
QString suggestion(const QStringList &words)
{
    if (words.isEmpty() || words.join(' ').contains(QRegularExpression(QStringLiteral("\\d"))) || words.first().size() < 4) return {};
    for (const auto &command : commands())
        if (distance(words.first().toLower(), command.name) <= 2) return command.name;
    return {};
}

// QCommandLineParser for a command, without Qt's own help and errors.
struct Parser {
    QCommandLineParser parser;
    QCommandLineOption help{{QStringLiteral("h"), QStringLiteral("help")}};
    QCommandLineOption translation{{QStringLiteral("t"), QStringLiteral("translation")}, {}, QStringLiteral("name")};
    explicit Parser(const QList<QCommandLineOption> &extra = {}) {
        parser.addOption(help); parser.addOption(translation);
        for (const auto &option : extra) parser.addOption(option);
    }
    bool parse(const QStringList &arguments) { return parser.parse(QStringList{QStringLiteral("omaword")} + arguments); }
};

void lines(QTextStream &in, QStringList *items)
{
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (!line.isEmpty()) *items << line;
    }
}
// The passages named: each argument, or all of them together when they only
// make one together (omaword quote john 3:16). "-", or no argument while
// input is piped in, reads them from standard input, one per line.
QStringList passages(BibleBackend &bible, const QStringList &positional, QTextStream &in, const Terminal &terminal)
{
    QStringList items, words;
    const auto take = [&] {
        if (words.size() > 1 && !bible.readPassage(words.join(' '), true).contains("error")) items << words.join(' ');
        else items << words;
        words.clear();
    };
    for (const auto &word : positional) {
        if (word == QStringLiteral("-")) { take(); lines(in, &items); }
        else words << word;
    }
    take();
    if (positional.isEmpty() && !terminal.input) lines(in, &items);
    return items;
}

QString translationList(const BibleBackend &bible)
{
    QStringList names;
    for (const auto &value : bible.versions()) names << value.toMap().value("abbreviation").toString();
    return names.isEmpty() ? QStringLiteral("none") : names.join(QStringLiteral(", "));
}
bool ready(const BibleBackend &bible, const QString &command, QTextStream &err)
{
    if (bible.ready()) return true;
    failure(err, command, bible.error().isEmpty() || bible.versions().isEmpty()
        ? branded("no translation is installed yet. Add one in OmaWord (Add version) or with omaword import <file>.")
        : bible.error());
    return false;
}
// The translation named on the command line, read in this process alone:
// the version you read stays as it was.
bool useTranslation(BibleBackend &bible, const QString &name, const QString &command, QTextStream &err)
{
    if (!ready(bible, command, err)) return false;
    if (name.isEmpty()) return true;
    const QString id = bible.versionIdFor(name);
    if (id.isEmpty()) {
        failure(err, command, "no installed translation \u201c" + name + "\u201d (installed: " + translationList(bible) + ").");
        return false;
    }
    if (bible.readVersion(id)) return true;
    failure(err, command, bible.error());
    return false;
}

int quote(const QStringList &arguments, QTextStream &out, QTextStream &err, QTextStream &in, const Terminal &terminal)
{
    const QCommandLineOption plain(QStringLiteral("plain")), json(QStringLiteral("json"));
    Parser p({plain, json});
    if (!p.parse(arguments)) return usage(err, "quote", p.parser.errorText());
    if (p.parser.isSet(p.help)) { printHelp(out, *find("quote")); return Success; }
    if (p.parser.positionalArguments().isEmpty() && terminal.input) return usage(err, "quote", "name a passage, such as: omaword quote jn316");
    BibleBackend bible;
    if (!useTranslation(bible, p.parser.value(p.translation), "quote", err)) return Failure;
    const QStringList items = passages(bible, p.parser.positionalArguments(), in, terminal);
    if (items.isEmpty()) return usage(err, "quote", "name a passage, such as: omaword quote jn316");
    QJsonArray array;
    QStringList texts;
    int status = Success;
    for (const auto &item : std::as_const(items)) {
        const auto passage = bible.readPassage(item);
        if (passage.contains("error")) { failure(err, "quote", item + ": " + passage.value("error").toString()); status = Failure; continue; }
        texts << passage.value(p.parser.isSet(plain) ? "plain" : "quotation").toString();
        QJsonArray verses;
        for (const auto &verse : passage.value("verses").toList()) verses.append(QJsonObject::fromVariantMap(verse.toMap()));
        array.append(QJsonObject{{"reference", passage.value("reference").toString()}, {"translation", passage.value("abbreviation").toString()},
                                 {"translationId", passage.value("versionId").toString()}, {"translationName", passage.value("name").toString()},
                                 {"text", passage.value("plain").toString()}, {"quotation", passage.value("quotation").toString()}, {"verses", verses}});
    }
    if (p.parser.isSet(json)) out << QJsonDocument(array).toJson(QJsonDocument::Indented);
    else if (!texts.isEmpty()) out << texts.join(QStringLiteral("\n\n")) << '\n';
    out.flush();
    return status;
}

QString emphasized(const QString &text, const QVariantList &segments, bool color)
{
    if (!color) return text;
    QString out;
    int at = 0;
    for (const auto &value : segments) {
        const auto segment = value.toMap();
        const int start = segment.value("start").toInt(), length = segment.value("length").toInt();
        if (start < at || start + length > text.size()) continue;
        out += text.mid(at, start - at) + QStringLiteral("\x1b[1;4m") + text.mid(start, length) + QStringLiteral("\x1b[0m");
        at = start + length;
    }
    return out + text.mid(at);
}

int search(const QStringList &arguments, QTextStream &out, QTextStream &err, const Terminal &terminal)
{
    const QCommandLineOption all(QStringLiteral("all")), json(QStringLiteral("json"));
    const QCommandLineOption limit(QStringLiteral("limit"), {}, QStringLiteral("n")), colorOption(QStringLiteral("color"), {}, QStringLiteral("when"));
    Parser p({all, json, limit, colorOption});
    if (!p.parse(arguments)) return usage(err, "search", p.parser.errorText());
    if (p.parser.isSet(p.help)) { printHelp(out, *find("search")); return Success; }
    const QString query = p.parser.positionalArguments().join(' ').trimmed();
    if (query.isEmpty()) return usage(err, "search", "name what to search for, such as: omaword search living water");
    if (p.parser.isSet(all) && p.parser.isSet(p.translation)) return usage(err, "search", "--all and --translation search different translations; use one.");
    bool number = true;
    const int count = p.parser.isSet(limit) ? p.parser.value(limit).toInt(&number) : (terminal.output ? 20 : 0);
    if (!number || count < 0) return usage(err, "search", "--limit takes a number (0: every match).");
    const QString when = p.parser.isSet(colorOption) ? p.parser.value(colorOption) : QStringLiteral("auto");
    if (!QStringList{"auto", "always", "never"}.contains(when)) return usage(err, "search", "--color takes auto, always, or never.");
    const bool color = when == QStringLiteral("always") || (when == QStringLiteral("auto") && terminal.color);
    BibleBackend bible;
    if (!useTranslation(bible, p.parser.value(p.translation), "search", err)) return Failure;
    QStringList installed;
    for (const auto &value : bible.versions()) installed << value.toMap().value("id").toString();
    const auto result = ScriptureSearch::searchEditions(bible.searchEditions(p.parser.isSet(all)), query,
                                                        ScriptureSearch::defaultIndexPath(), installed);
    if (!result.error.isEmpty()) return usage(err, "search", result.error);
    const qsizetype shown = count ? std::min<qsizetype>(count, result.matches.size()) : result.matches.size();
    if (p.parser.isSet(json)) {
        QJsonArray array;
        for (qsizetype i = 0; i < shown; ++i) {
            const auto row = result.matches[i].toMap();
            QJsonArray spans;
            for (const auto &segment : row.value("displaySegments").toList()) spans.append(QJsonObject::fromVariantMap(segment.toMap()));
            array.append(QJsonObject{{"reference", row.value("reference").toString()}, {"translation", row.value("versionAbbreviation").toString()},
                                     {"translationId", row.value("versionId").toString()}, {"book", row.value("bookName").toString()},
                                     {"chapter", row.value("chapter").toInt()}, {"verse", row.value("start").toInt()}, {"endVerse", row.value("end").toInt()},
                                     {"text", row.value("text").toString()}, {"matches", spans}});
        }
        out << QJsonDocument(array).toJson(QJsonDocument::Indented);
    } else {
        for (qsizetype i = 0; i < shown; ++i) {
            const auto row = result.matches[i].toMap();
            const QString reference = row.value("reference").toString() + " (" + row.value("versionAbbreviation").toString() + ")";
            // One line per match: the text's own breaks become spaces (same length, so spans hold).
            QString text = row.value("text").toString();
            text.replace('\n', ' ').replace(QChar(0x2028), ' ');
            out << (color ? QStringLiteral("\x1b[1m") + reference + QStringLiteral("\x1b[0m") : reference) << '\n'
                << "  " << emphasized(text, row.value("displaySegments").toList(), color) << '\n';
        }
    }
    out.flush();
    if (result.matches.isEmpty()) { failure(err, "search", "no match for \u201c" + query + "\u201d."); return Failure; }
    if (shown < result.matches.size())
        err << "Showing " << shown << " of " << result.matches.size() << " matches; --limit 0 shows them all." << Qt::endl;
    return Success;
}

int translations(const QStringList &arguments, QTextStream &out, QTextStream &err)
{
    const QCommandLineOption names(QStringLiteral("names")), json(QStringLiteral("json"));
    Parser p({names, json});
    if (!p.parse(arguments)) return usage(err, "translations", p.parser.errorText());
    if (p.parser.isSet(p.help)) { printHelp(out, *find("translations")); return Success; }
    if (!p.parser.positionalArguments().isEmpty() || p.parser.isSet(p.translation)) return usage(err, "translations", "takes no arguments.");
    BibleBackend bible;
    const auto rows = bible.versions();
    if (p.parser.isSet(json)) {
        QJsonArray array;
        for (const auto &value : rows) {
            const auto row = value.toMap();
            array.append(QJsonObject{{"id", row.value("id").toString()}, {"abbreviation", row.value("abbreviation").toString()},
                                     {"name", row.value("name").toString()}, {"active", row.value("active").toBool()}});
        }
        out << QJsonDocument(array).toJson(QJsonDocument::Indented);
        out.flush();
        return Success;
    }
    int width = 0;
    for (const auto &value : rows) width = std::max<int>(width, value.toMap().value("abbreviation").toString().size());
    for (const auto &value : rows) {
        const auto row = value.toMap();
        if (p.parser.isSet(names)) { out << row.value("abbreviation").toString() << '\n'; continue; }
        out << (row.value("active").toBool() ? "* " : "  ") << row.value("abbreviation").toString().leftJustified(width + 2)
            << row.value("name").toString() << "  (" << row.value("id").toString() << ")\n";
    }
    out.flush();
    if (rows.isEmpty() && !p.parser.isSet(names))
        err << branded("No translation is installed yet. Add one in OmaWord (Add version) or with omaword import <file>.") << Qt::endl;
    return Success;
}

int links(const QStringList &arguments, QTextStream &out, QTextStream &err, QTextStream &in, const Terminal &terminal)
{
    Parser p;
    if (!p.parse(arguments)) return usage(err, "link", p.parser.errorText());
    if (p.parser.isSet(p.help)) { printHelp(out, *find("link")); return Success; }
    if (p.parser.positionalArguments().isEmpty() && terminal.input) return usage(err, "link", "name a passage, such as: omaword link jn316");
    BibleBackend bible;
    if (!useTranslation(bible, p.parser.value(p.translation), "link", err)) return Failure;
    const QStringList items = passages(bible, p.parser.positionalArguments(), in, terminal);
    if (items.isEmpty()) return usage(err, "link", "name a passage, such as: omaword link jn316");
    // Without --translation, a link opens in whatever the window reads.
    const QString translation = p.parser.isSet(p.translation) ? bible.versionAbbreviation() : QString();
    int status = Success;
    for (const auto &item : std::as_const(items)) {
        const auto passage = bible.readPassage(item);
        if (passage.contains("error")) { failure(err, "link", item + ": " + passage.value("error").toString()); status = Failure; continue; }
        out << link(passage.value("reference").toString(), translation) << '\n';
    }
    out.flush();
    return status;
}

int completion(const QStringList &arguments, QTextStream &out, QTextStream &err)
{
    Parser p;
    if (!p.parse(arguments)) return usage(err, "completion", p.parser.errorText());
    if (p.parser.isSet(p.help)) { printHelp(out, *find("completion")); return Success; }
    const auto shells = p.parser.positionalArguments();
    const QHash<QString, QString> files{{"bash", "omaword.bash"}, {"zsh", "_omaword"}, {"fish", "omaword.fish"}};
    if (shells.size() != 1 || !files.contains(shells.first())) return usage(err, "completion", "name a shell: bash, zsh, or fish.");
    initCoreResources();
    QFile file(QStringLiteral(":/completions/") + files.value(shells.first()));
    if (!file.open(QIODevice::ReadOnly)) return failure(err, "completion", "the script is missing from this build.");
    out << branded(QString::fromUtf8(file.readAll()));
    out.flush();
    return Success;
}
}

bool headless(const QStringList &arguments)
{
    if (arguments.isEmpty()) return false;
    const QString first = arguments.first();
    if (QStringList{"-h", "--help", "-v", "--version", "help"}.contains(first)) return true;
    if (const auto *command = find(first)) return command->headless || arguments.contains("-h") || arguments.contains("--help");
    return false;
}

int run(const QStringList &arguments, QTextStream &out, QTextStream &err, QTextStream &in, const Terminal &terminal)
{
    const QString first = arguments.value(0);
    if (first == QStringLiteral("-v") || first == QStringLiteral("--version")) { out << Brand::name() << ' ' << Brand::version() << Qt::endl; return Success; }
    if (first == QStringLiteral("-h") || first == QStringLiteral("--help")) { printHelp(out); return Success; }
    const QStringList rest = arguments.mid(1);
    if (first == QStringLiteral("help")) {
        if (rest.isEmpty()) { printHelp(out); return Success; }
        if (const auto *command = find(rest.first())) { printHelp(out, *command); return Success; }
        return usage(err, {}, "no command \u201c" + rest.first() + "\u201d.");
    }
    if (first == QStringLiteral("quote")) return quote(rest, out, err, in, terminal);
    if (first == QStringLiteral("search")) return search(rest, out, err, terminal);
    if (first == QStringLiteral("translations") || first == QStringLiteral("versions")) return translations(rest, out, err);
    if (first == QStringLiteral("link")) return links(rest, out, err, in, terminal);
    if (first == QStringLiteral("completion")) return completion(rest, out, err);
    // open, lookup, or import with --help.
    if (const auto *command = find(first)) { printHelp(out, *command); return Success; }
    return usage(err, {}, "no command \u201c" + first + "\u201d.");
}

bool request(const QStringList &arguments, SingleInstance::Request *request, QString *error)
{
    QStringList words = arguments;
    QString command;
    if (!words.isEmpty() && QStringList{"open", "lookup", "import"}.contains(words.first())) command = words.takeFirst();
    QCommandLineParser parser;
    const QCommandLineOption importOption(QStringLiteral("import"), {}, QStringLiteral("file")), here(QStringLiteral("here"));
    const QCommandLineOption translationOption({QStringLiteral("t"), QStringLiteral("translation")}, {}, QStringLiteral("name"));
    parser.addOption(importOption); parser.addOption(here); parser.addOption(translationOption);
    const QString name = command.isEmpty() ? QString() : ' ' + command;
    auto fail = [&](const QString &message) {
        *error = branded("omaword" + name + ": " + message + "\nRun 'omaword help" + name + "' for usage.");
        return false;
    };
    if (!parser.parse(QStringList{QStringLiteral("omaword")} + words)) return fail(parser.errorText());
    const QStringList positional = parser.positionalArguments();
    *request = SingleInstance::Request::fromArguments({}, parser.value(importOption), parser.isSet(importOption));
    request->translation = parser.value(translationOption);
    if (parser.isSet(here) && command != QStringLiteral("open")) return fail("--here goes with open: omaword open <passage> --here");
    if (parser.isSet(translationOption) && (command == QStringLiteral("lookup") || command == QStringLiteral("import")))
        return fail("--translation goes with a passage: omaword open <passage> -t <name>");
    if (command == QStringLiteral("open")) {
        if (positional.isEmpty()) return fail("name a passage, such as: omaword open jn316");
        request->reference = positional.join(' ');
        request->newWindow = !parser.isSet(here);
    } else if (command == QStringLiteral("lookup")) {
        request->reference = positional.join(' ');
        request->lookup = true;
        request->newWindow = false;
    } else if (command == QStringLiteral("import")) {
        if (positional.size() != 1) return fail("name one USFM ZIP or file, such as: omaword import engwebp_usfm.zip");
        request->importPath = QFileInfo(positional.first()).absoluteFilePath();
        request->newWindow = false;
    } else if (positional.size() == 1 && parseLink(positional.first(), &request->reference, &request->translation)) {
        if (parser.isSet(translationOption)) request->translation = parser.value(translationOption);
        request->newWindow = false;
    } else {
        const QString near = suggestion(positional);
        if (!near.isEmpty()) return fail("no command \u201c" + positional.first() + "\u201d. Did you mean \u201c" + near + "\u201d?");
        request->reference = positional.join(' ');
    }
    if (request->reference.size() > 1024) return fail("the passage is too long.");
    if (request->translation.size() > 128) return fail("the translation's name is too long.");
    return true;
}

bool parseLink(const QString &text, QString *reference, QString *translation)
{
    // The scheme the desktop file registers: x-scheme-handler/<the app's ID>.
    const QString scheme = Brand::appId() + QChar(':');
    if (!text.startsWith(scheme, Qt::CaseInsensitive)) return false;
    QString rest = text.mid(scheme.size()), query;
    if (const int hash = rest.indexOf('#'); hash >= 0) rest.truncate(hash);
    if (const int mark = rest.indexOf('?'); mark >= 0) { query = rest.mid(mark + 1); rest.truncate(mark); }
    while (rest.startsWith(QChar('/'))) rest.remove(0, 1);
    while (rest.endsWith(QChar('/'))) rest.chop(1);
    *reference = QUrl::fromPercentEncoding(rest.replace(QChar('+'), QChar(' ')).toUtf8()).simplified();
    const QUrlQuery items(query);
    for (const QString key : {"translation", "t", "version"})
        if (items.hasQueryItem(key)) { *translation = items.queryItemValue(key, QUrl::FullyDecoded); break; }
    return !reference->isEmpty();
}

QString link(const QString &reference, const QString &translation)
{
    QString path = reference;
    path.replace(QChar(0x2013), QChar('-'));
    QString out = Brand::appId() + QChar(':') + QString::fromUtf8(QUrl::toPercentEncoding(path, ":-,;"));
    if (!translation.isEmpty()) out += QStringLiteral("?translation=") + QString::fromUtf8(QUrl::toPercentEncoding(translation));
    return out;
}
}
