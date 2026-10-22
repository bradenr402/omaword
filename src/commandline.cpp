#include "commandline.h"
#include "biblebackend.h"
#include "biblebooks.h"
#include "brand.h"
#include "scripturesearch.h"

#include <QCommandLineParser>
#include <QDateTime>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>
#include <algorithm>

// Q_INIT_RESOURCE cannot be used inside a namespace.
static void initCoreResources() { Q_INIT_RESOURCE(core); }

namespace CommandLine {
namespace {
struct Option { QString flags, text; };
// A command, a group of them named for what they manage (translations), or
// one of a group's (translations list).
struct Command {
    QString name, arguments, summary, description;
    QList<Option> options;
    QStringList examples;
    bool headless;
    // A group: the command its name alone runs, and whether words after the
    // name go to that command (omaword marks grace) rather than naming one.
    QString defaultCommand = {};
    bool words = false;
};
const Option translationOption{QStringLiteral("-t, --translation <name>"),
    QStringLiteral("Use this translation: its abbreviation (NET) or ID (eng_net), in any case. Default: the one you read.")};
const Option helpOption{QStringLiteral("-h, --help"), QStringLiteral("Show this help.")};

const QList<Command> &commands()
{
    static const QList<Command> list{
        {"open", "<passage> [--here] [-t <name>]", "Open a passage in a window",
         "Opens the passage in a new OmaWord window, starting OmaWord if it is not running. With --here it opens in "
         "the window you were reading instead. A translation other than the window's switches the window to it.",
         {translationOption, {"    --here", "Open in the window you were reading, not a new one."}, helpOption},
         {"omaword open jn316", "omaword open \"Romans 8\" --here", "omaword open ps23 -t KJV"}, false},
        {"lookup", "[<text>] [-t <name>]", "Look up a passage in a small window",
         "Opens a small window on the workspace you are on, with <text> typed into it if given: type a passage, enter "
         "shows it, y copies it (shift Y the plain text), enter opens it in the window you were reading, esc closes. "
         "With more than one version installed, tab and shift tab read it in another, which enter then opens. "
         "It can be resized and opens at its last size. Suits a key binding; in Omarchy, in ~/.config/hypr/bindings.lua, "
         "with a rule that floats and centers it and shows and hides it at once, as Omarchy's menus do:\n"
         "  o.bind(\"SUPER + ALT + B\", \"Look up a passage\", \"omaword lookup\")\n"
         "  o.window({ class = \"^omaword$\", title = \"^Look up · OmaWord$\" },\n"
         "           { float = true, center = true, no_anim = true })",
         {translationOption, helpOption}, {"omaword lookup", "omaword lookup rom 8", "omaword lookup -t KJV"}, false},
        {"read", "<passage>... [--footnotes] [--json] [-t <name>]", "Print passages as the reader lays them out",
         "Prints each passage as OmaWord's reader shows it, under its reference and translation: section headings, "
         "paragraphs, poem lines indented by their level, verse numbers, and a numbered marker where a footnote is. "
         "Verse numbers, headings, and the words of Jesus in red follow OmaWord's settings. Lines wrap to the "
         "terminal's width, piped into a pager too. Given \"-\" (or no passage while input is piped in), it reads "
         "passages from standard input, one per line.",
         {translationOption, {"    --footnotes", "Print the footnotes after each passage, numbered as their markers."},
          {"    --color <when>", "Bold the titles and headings, and color the words of Jesus: auto (on a terminal, unless "
                               "NO_COLOR is set), always, or never."},
          {"    --json", "Print a JSON array: each passage's reference, translation, and verses, with their headings, "
                         "paragraphs, poem levels, footnotes, and words of Jesus."}, helpOption},
         {"omaword read ps23", "omaword read \"John 3\" --footnotes -t KJV", "omaword read \"Romans 8\" | less"}, true},
        {"quote", "<passage>... [--all | -t <name>] [--plain] [--json]", "Print passages as quotations",
         "Prints each passage as OmaWord copies it: the text in quotation marks, a number before each verse, and the "
         "reference after an em dash, with a blank line between passages. Given \"-\" (or no passage while input is "
         "piped in), it reads passages from standard input, one per line. No window opens, and your reading place "
         "stays where it was.",
         {translationOption, {"    --all", "Quote each passage in every installed translation, the one you read first, "
                                         "each reference followed by the translation's abbreviation."},
          {"    --plain", "Print the text alone: no quotation marks, verse numbers, or reference."},
          {"    --json", "Print a JSON array: each passage's reference, translation, text, and verses."}, helpOption},
         {"omaword quote jn316", "omaword quote ps23 -t KJV --plain | wl-copy", "omaword quote \"Genesis 1:1-3\" \"John 1:1-5\"",
          "omaword quote rom828 --all", "printf 'jn316\\nrom 8:28\\n' | omaword quote"}, true},
        {"refs", "<passage>... [--quote] [--limit <n>] [--json] [-t <name>]", "Print cross-references and passages tagged alike",
         "Prints what OmaWord's See also gives for each passage: the passages tagged alike (sharing its tags, or those "
         "of marks over it) in every translation, then OpenBible.info's cross-references, most voted first, each with "
         "its opening words. See also follows verses, not whole chapters. The exit status is 1 when a passage has "
         "neither.",
         {translationOption, {"    --quote", "Print each passage in full, as a quotation."},
          {"    --limit <n>", "Print at most n passages a section (0: all). Default: 20 on a terminal, all otherwise."},
          {"    --color <when>", "Bold the references: auto (on a terminal, unless NO_COLOR is set), always, or never."},
          {"    --json", "Print a JSON array: each passage's passages tagged alike and cross-references, with their text."},
          helpOption},
         {"omaword refs jn316", "omaword refs rom828 --quote --limit 5"}, true},
        {"search", "<words>... [--all | -t <name>] [--in <part>]... [--limit <n>]", "Search the Bible",
         "Searches as OmaWord's search does: every word in one verse, partial words included; a \"quoted phrase\" may "
         "run across verses. Words in slashes are a regular expression, as in chapter find, matched within one verse. "
         "Case is ignored unless a word (or the expression) has a capital letter: \\c ignores it, \\C keeps it. Matches "
         "print in Bible order, the matched words emphasized on a terminal. The exit status is 1 when nothing matches.",
         {translationOption, {"    --all", "Search every installed translation, the one you read first."},
          {"    --in <part>", "Search only these books: ot, nt, apocrypha (the books after them), or a book, such as "
                              "John or jn. Repeat it for more."},
          {"    --limit <n>", "Print at most n matches (0: all). Default: 20 on a terminal, all otherwise."},
          {"    --color <when>", "Emphasize matches: auto (on a terminal, unless NO_COLOR is set), always, or never."},
          {"    --json", "Print a JSON array: each match's reference, translation, text, and matched spans."}, helpOption},
         {"omaword search living water", "omaword search '\"the word became flesh\"' --all", "omaword search '/lov(e|ed) the world/'",
          "omaword search shepherd --in ot --in John", "omaword search grace --limit 0 | wc -l"}, true},
        {"link", "<passage>... [-t <name>]", "Print an omaword: link to a passage",
         "Prints a link that opens the passage in OmaWord, in the window you were reading: from a web page, a note "
         "elsewhere, or a script. Links look like omaword:John%203:16, or omaword:John%203:16?translation=KJV for a "
         "translation.",
         {translationOption, helpOption}, {"omaword link jn316", "xdg-open \"$(omaword link ps23 -t KJV)\""}, true},
        {"books", "", "List a translation's books, or a book's outline", "", {}, {}, true, "list"},
        {"books list", "[--json] [-t <name>]", "List the translation's books",
         "Lists the translation's books, in its order, with how many chapters each has.",
         {translationOption, {"    --json", "Print a JSON array: each book's name, code, abbreviation, and chapters."}, helpOption},
         {"omaword books", "omaword books -t KJV --json"}, true},
        {"books outline", "<book|chapter> [--json] [-t <name>]", "Print a book's section headings",
         "Prints the section headings of a book, or of one of its chapters, as the whole-book view's outline has "
         "them: each chapter, then its headings after the verses they open.",
         {translationOption, {"    --json", "Print a JSON array: each chapter's number, title, and headings."}, helpOption},
         {"omaword books outline John", "omaword books outline \"Psalm 119\" -t KJV"}, true},
        {"marks", "", "List your highlights, notes, and tags", "", {}, {}, true, "list", true},
        {"marks list", "[<words>...] [--tag <name>]... [options]", "List your marks",
         "Lists your marks (each passage you highlighted, wrote a note on, or tagged) in the translation you read, in "
         "Bible order: its reference and translation, whether it is highlighted, and its tags, then its note on one "
         "line, or its opening words without one. As in OmaWord's Marks, each word must match a mark's reference, "
         "translation, note, tags, or text, ignoring case, and #name matches a tag starting with name, as --tag does. "
         "The exit status is 1 when nothing matches.",
         {translationOption, {"    --all", "List the marks in every installed translation."},
          {"    --tag <name>", "Only marks with a tag starting with name. Repeat it to need more tags."},
          {"    --notes", "Only marks with a note."}, {"    --highlights", "Only highlighted marks."},
          {"    --json", "Print a JSON array: each mark's reference, translation, highlight, note, tags, text, and "
                         "when it was made and changed."}, helpOption},
         {"omaword marks", "omaword marks --tag grace --all", "omaword marks shepherd --notes"}, true},
        {"marks show", "<passage> [--json] [-t <name>]", "Print the marks on a passage",
         "Prints the marks on a passage or within it, in the translation you read, as marks list does but with each "
         "note whole, as you wrote it. The exit status is 1 when there are none.",
         {translationOption, {"    --json", "Print a JSON array, as marks list does."}, helpOption},
         {"omaword marks show jn316", "omaword marks show \"Romans 8\""}, true},
        {"marks export", "[-o <file>]", "Print your marks as Markdown",
         "Prints your marks as Markdown, as OmaWord's Export writes them: a section for each translation, then each "
         "mark's reference, highlight, tags, and note. With -o it writes them to a file instead, readable by you alone.",
         {{"-o, --output <file>", "Write them to this file."}, helpOption},
         {"omaword marks export > notes.md", "omaword marks export -o ~/Documents/omaword-notes.md"}, true},
        {"marks backup", "<file|->", "Back up your marks as JSON",
         "Writes the backup OmaWord's Back up writes, which its Restore reads: every translation's marks and reading "
         "place, your tags, and the Desk, in a file readable by you alone. Given -, it prints the backup instead.",
         {helpOption}, {"omaword marks backup ~/omaword-backup.json", "omaword marks backup - | gzip > backup.json.gz"}, true},
        {"tags", "", "List your tags", "", {}, {}, true, "list"},
        {"tags list", "[--json]", "List your tags",
         "Lists your tags by name, each with how many passages it marks; omaword marks --tag <name> --all lists them.",
         {{"    --json", "Print a JSON array: each tag's name and count."}, helpOption}, {"omaword tags"}, true},
        {"desk", "", "Print the Desk", "", {}, {}, true, "show"},
        {"desk show", "", "Print the Desk",
         "Prints the Desk, the Markdown page every window shares, as last saved.",
         {helpOption}, {"omaword desk", "omaword desk > study.md"}, true},
        {"history", "", "List the passages you visited", "", {}, {}, true, "list"},
        {"history list", "[--json]", "List the passages you visited",
         "Lists History, the passages you visited, newest first, each after when you did.",
         {{"    --json", "Print a JSON array: each visit's reference, translation, and time."}, helpOption},
         {"omaword history", "omaword history | head"}, true},
        {"ribbon", "", "Print where the ribbon lies", "", {}, {}, true, "show"},
        {"ribbon show", "[--json]", "Print where the ribbon lies",
         "Prints the passage where the ribbon lies, the reading place you keep. The exit status is 1 when it lies "
         "nowhere.",
         {{"    --json", "Print a JSON object: its reference, translation, and when it was placed."}, helpOption},
         {"omaword ribbon"}, true},
        {"translations", "", "List the installed translations, or show one",
         "omaword versions is the same.", {}, {}, true, "list"},
        {"translations list", "[--names] [--json]", "List the installed translations",
         "Lists the installed translations with their abbreviations and IDs; * marks the one you read.",
         {{"    --names", "Print the abbreviations alone, one per line."}, {"    --json", "Print a JSON array."}, helpOption},
         {"omaword translations"}, true},
        {"translations show", "[<name>] [--json]", "Show a translation's source and rights",
         "Prints what OmaWord knows of an installed translation, by default the one you read: its name, "
         "abbreviation, ID, language, and contents, AO's revision of it, and its rights as its source gives them, "
         "which About & Attribution shows too.",
         {{"    --json", "Print a JSON object."}, helpOption},
         {"omaword translations show", "omaword translations show KJV"}, true},
        {"catalog", "", "Browse AO Lab's catalog of translations", "", {}, {}, true, "list", true},
        {"catalog list", "[<words>...] [--language <code> | --all-languages]", "List the translations AO Lab offers",
         "Lists the translations in AO Lab's catalog, as last refreshed (else as OmaWord came with it), in English by "
         "default: each one's abbreviation, name, and ID, and whether it is installed. Each word must match a "
         "translation's name, English name, ID, or abbreviation, ignoring case. The exit status is 1 when none does.",
         {{"    --language <code>", "List the translations in this language, by its ISO 639-3 code, such as spa."},
          {"    --all-languages", "List the translations in every language."},
          {"    --json", "Print a JSON array: each translation's ID, abbreviation, names, language, contents, and whether "
                         "it is installed or can be."}, helpOption},
         {"omaword catalog", "omaword catalog standard", "omaword catalog --language spa"}, true},
        {"catalog show", "<id> [--json]", "Show a translation in AO Lab's catalog",
         "Prints what AO Lab's catalog says of a translation, named by its ID or abbreviation: its name, language, "
         "and contents, whether it is installed, and its terms and permissions, as Details & Attribution shows them.",
         {{"    --json", "Print a JSON object."}, helpOption}, {"omaword catalog show eng_kjv", "omaword catalog show BSB"}, true},
        {"catalog refresh", "", "Download AO Lab's current catalog",
         "Downloads AO Lab's current catalog, which catalog list and show read from then on, as do the OmaWord "
         "windows that open after it.",
         {helpOption}, {"omaword catalog refresh"}, true},
        {"import", "<file>", "Install a translation from AO complete JSON",
         "Installs a translation, or replaces it with a newer copy, from AO standard complete JSON. "
         "Add version in OmaWord installs the catalog's translations directly.",
         {helpOption}, {"omaword import ~/Downloads/eng_net_complete.json"}, false},
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

const Command *find(QString name)
{
    if (name == QStringLiteral("versions") || name.startsWith(QStringLiteral("versions "))) name.replace(0, 8, QStringLiteral("translations"));
    for (const auto &command : commands()) if (command.name == name) return &command;
    return nullptr;
}
// A group's commands, in the order listed.
QList<const Command *> subcommands(const Command &group)
{
    QList<const Command *> list;
    for (const auto &command : commands()) if (command.name.startsWith(group.name + ' ')) list << &command;
    return list;
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
    for (const auto &command : commands())
        if (!command.name.contains(' ')) text += tableRow(command.name, command.summary, 16) + '\n';
    text += "\nOptions:\n"
        + tableRow(translationOption.flags, "The translation a command reads: its abbreviation or ID. "
                                            "Default: the one you read.", 28) + '\n'
        + tableRow("-h, --help", "Show help (omaword help <command>: a command's)", 28) + '\n'
        + tableRow("-v, --version", "Show OmaWord's version", 28) + "\n\n"
        + "Passages:\n" + wrap("Full or compact references, as OmaWord's passage palette reads them: \"John 3:16\", jn316, "
                               "1co13, \"rev 3 14-22\", ps23 (a whole chapter), \"Genesis 1:1-2:3\".", 80, 2) + "\n\n"
        + "Links:\n" + wrap("omaword:John%203:16 (or omaword:jn316?translation=KJV) opens the passage where you were "
                            "reading; omaword link prints one. xdg-open and browsers hand them to OmaWord.", 80, 2) + "\n\n"
        + "Examples:\n"
        + "  omaword read ps23 --footnotes\n"
        + "  omaword quote jn316\n  omaword quote ps23 -t KJV --plain | wl-copy\n  omaword search \"living water\" --all\n"
        + "  omaword open \"Romans 8\" --here\n\n"
        + "Exit status:\n" + wrap("0 on success; 1 when it fails (an unknown passage or translation, no match, nothing "
                                  "installed); 2 for a usage error.", 80, 2) + '\n';
    out << branded(text);
    out.flush();
}

// "omaword translations [list] [--names] [--json]": a group's default
// command may go unnamed.
QString usageLine(const Command &command)
{
    QString name = command.name;
    if (const int space = name.indexOf(' '); space > 0 && find(name.left(space))->defaultCommand == name.mid(space + 1))
        name = name.left(space) + QStringLiteral(" [") + name.mid(space + 1) + ']';
    return "omaword " + name + (command.arguments.isEmpty() ? QString() : ' ' + command.arguments);
}

void printHelp(QTextStream &out, const Command &command)
{
    QString text = "omaword " + command.name + " \u2014 " + command.summary + "\n\nUsage:\n";
    const auto list = subcommands(command);
    if (list.isEmpty()) text += "  " + usageLine(command) + '\n';
    for (const auto *subcommand : list) text += "  " + usageLine(*subcommand) + '\n';
    if (!command.description.isEmpty()) text += '\n' + wrap(command.description, 80, 0) + '\n';
    if (!list.isEmpty()) {
        text += "\nCommands:\n";
        for (const auto *subcommand : list) {
            const QString name = subcommand->name.mid(command.name.size() + 1);
            text += tableRow(name, subcommand->summary + (name == command.defaultCommand ? " (the default)" : ""), 16) + '\n';
        }
        text += "\nRun 'omaword help " + command.name + " <command>' for a command's options.\n";
    }
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
    err << branded("omaword" + name + ": " + printable(message) + "\nRun 'omaword help" + name + "' for usage.") << Qt::endl;
    return Usage;
}
int failure(QTextStream &err, const QString &command, const QString &message)
{
    err << branded("omaword " + command + ": ") << printable(message) << Qt::endl;
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
        if (!command.name.contains(' ') && distance(words.first().toLower(), command.name) <= 2) return command.name;
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

// JSON as QJsonDocument writes it, but with DEL and the C1 controls, which
// it leaves raw, escaped as well: the values read back exact, and no control
// character reaches a terminal.
QString json(const QByteArray &bytes)
{
    const QString text = QString::fromUtf8(bytes);
    QString escaped;
    escaped.reserve(text.size());
    for (const QChar c : text) {
        if (c.unicode() >= 0x7f && c.unicode() <= 0x9f) escaped += QStringLiteral("\\u%1").arg(int(c.unicode()), 4, 16, QChar('0'));
        else escaped += c;
    }
    return escaped;
}
QString json(const QJsonDocument &document) { return json(document.toJson(QJsonDocument::Indented)); }
QString json(const QJsonArray &array) { return json(QJsonDocument(array)); }

// --color <when>: auto (a terminal, unless NO_COLOR is set), always, or
// never; false for anything else.
bool colorWhen(const Parser &p, const QCommandLineOption &option, const Terminal &terminal, bool *color)
{
    const QString when = p.parser.isSet(option) ? p.parser.value(option) : QStringLiteral("auto");
    *color = when == QStringLiteral("always") || (when == QStringLiteral("auto") && terminal.color);
    return QStringList{"auto", "always", "never"}.contains(when);
}

// Lines longer than `width` columns broken between words, a poem line's
// parts indented two spaces past it; escape sequences take no room.
QString wrapLines(const QString &text, int width)
{
    if (width <= 0) return text;
    static const QRegularExpression escape(QStringLiteral("\x1b\\[[0-9;]*m"));
    QStringList lines;
    for (const QString &line : text.split('\n')) {
        qsizetype indent = 0;
        while (indent < line.size() && line[indent] == ' ') ++indent;
        const qsizetype hanging = indent ? indent + 2 : 0;
        QString current = line.left(indent);
        qsizetype used = indent;
        bool empty = true;
        for (const QString &word : line.mid(indent).split(' ')) {
            const qsizetype size = QString(word).remove(escape).size();
            if (!empty && used + 1 + size > width) { lines << current; current = QString(hanging, ' '); used = hanging; empty = true; }
            if (!empty) { current += ' '; ++used; }
            current += word; used += size; empty = false;
        }
        lines << current;
    }
    return lines.join('\n');
}

int quote(const QStringList &arguments, QTextStream &out, QTextStream &err, QTextStream &in, const Terminal &terminal)
{
    const QCommandLineOption plain(QStringLiteral("plain")), json(QStringLiteral("json")), all(QStringLiteral("all"));
    Parser p({plain, json, all});
    if (!p.parse(arguments)) return usage(err, "quote", p.parser.errorText());
    if (p.parser.isSet(p.help)) { printHelp(out, *find("quote")); return Success; }
    if (p.parser.positionalArguments().isEmpty() && terminal.input) return usage(err, "quote", "name a passage, such as: omaword quote jn316");
    if (p.parser.isSet(all) && p.parser.isSet(p.translation)) return usage(err, "quote", "--all and --translation quote different translations; use one.");
    if (p.parser.isSet(all) && p.parser.isSet(plain)) return usage(err, "quote", "--plain names no translation, so it cannot go with --all.");
    BibleBackend bible;
    if (!useTranslation(bible, p.parser.value(p.translation), "quote", err)) return Failure;
    const QStringList items = passages(bible, p.parser.positionalArguments(), in, terminal);
    if (items.isEmpty()) return usage(err, "quote", "name a passage, such as: omaword quote jn316");
    // --all: every installed translation, the one you read first, each read
    // once; the quotations print by passage.
    QStringList ids{bible.versionId()};
    if (p.parser.isSet(all))
        for (const auto &value : bible.versions())
            if (!ids.contains(value.toMap().value("id").toString())) ids << value.toMap().value("id").toString();
    QList<QJsonArray> rows(items.size());
    QList<QStringList> texts(items.size()), errors(items.size());
    for (const auto &id : std::as_const(ids)) {
        if (!bible.readVersion(id)) return failure(err, "quote", bible.error());
        for (qsizetype i = 0; i < items.size(); ++i) {
            const auto passage = bible.readPassage(items[i]);
            if (passage.contains("error")) {
                errors[i] << items[i] + (ids.size() > 1 ? " (" + bible.versionAbbreviation() + ")" : QString()) + ": " + passage.value("error").toString();
                continue;
            }
            // A verse the translation joined to another is quoted as the one holding it, and says so.
            if (passage.contains("note")) err << printable(passage.value("note").toString()) << '.' << Qt::endl;
            texts[i] << (p.parser.isSet(plain) ? passage.value("plain").toString()
                         : passage.value("quotation").toString() + (ids.size() > 1 ? " (" + passage.value("abbreviation").toString() + ')' : QString()));
            QJsonArray verses;
            for (const auto &value : passage.value("verses").toList()) {
                const auto verse = value.toMap();
                verses.append(QJsonObject{{"book", verse.value("book").toString()}, {"chapter", verse.value("chapter").toInt()},
                                          {"verse", verse.value("verse").toInt()}, {"text", verse.value("text").toString()}});
            }
            QJsonObject row{{"reference", passage.value("reference").toString()}, {"translation", passage.value("abbreviation").toString()},
                            {"translationId", passage.value("versionId").toString()}, {"translationName", passage.value("name").toString()},
                            {"text", passage.value("plain").toString()}, {"quotation", passage.value("quotation").toString()}, {"verses", verses}};
            if (passage.contains("note")) row.insert("note", passage.value("note").toString());
            rows[i].append(row);
        }
    }
    // A passage some translation lacks is noted; one none has fails.
    QJsonArray array;
    QStringList quoted;
    int status = Success;
    for (qsizetype i = 0; i < items.size(); ++i) {
        for (const auto &error : std::as_const(errors[i])) failure(err, "quote", error);
        if (texts[i].isEmpty()) status = Failure;
        else quoted << texts[i].join(QStringLiteral("\n\n"));
        for (const auto &row : std::as_const(rows[i])) array.append(row);
    }
    if (p.parser.isSet(json)) out << CommandLine::json(array);
    else if (!quoted.isEmpty()) out << printable(quoted.join(QStringLiteral("\n\n"))) << '\n';
    out.flush();
    return status;
}

// A passage as OmaWord's reader lays it out: its title, then chapter titles
// and section headings on lines of their own, paragraphs a blank line apart,
// poem lines indented two spaces a level, verse numbers, and "[n]" where the
// nth footnote is (its note added to `notes`); with color, bold titles and
// headings, and the words of Jesus in red.
QString readerText(const BibleBackend &bible, const QVariantMap &passage, bool color, QStringList *notes)
{
    const auto bold = [&](const QString &text) { return color ? QStringLiteral("\x1b[1m") + text + QStringLiteral("\x1b[22m") : text; };
    const bool red = color && bible.wordsOfJesus();
    QString text = bold(printable(passage.value("reference").toString() + " (" + passage.value("abbreviation").toString() + ')'));
    int book = 0, chapter = 0;
    bool poemEnd = false;
    for (const auto &value : passage.value("verses").toList()) {
        const auto verse = value.toMap();
        const int number = verse.value("verse").toInt();
        const auto levels = verse.value("indents").toList();
        const QString heading = bible.showHeadings() ? verse.value("heading").toString() : QString();
        const bool first = !book, newChapter = book && (verse.value("bookId").toInt() != book || verse.value("chapter").toInt() != chapter);
        book = verse.value("bookId").toInt(); chapter = verse.value("chapter").toInt();
        text += first || newChapter || !heading.isEmpty() ? QStringLiteral("\n\n")
            : TextSelection::verseJoin(verse.value("paragraphStart").toBool(), poemEnd, !levels.isEmpty() && levels.first().toInt() > 0);
        if (newChapter) text += bold(printable(bible.passageReference(book, chapter))) + "\n\n";
        if (!heading.isEmpty()) text += bold(printable(heading)) + "\n\n";
        poemEnd = !levels.isEmpty() && levels.last().toInt() > 0;
        // Footnotes go before the character at their offset; the words of
        // Jesus run from start to end.
        const QString plain = printable(verse.value("text").toString());
        auto footnotes = verse.value("footnotes").toList();
        std::stable_sort(footnotes.begin(), footnotes.end(), [](const QVariant &a, const QVariant &b) {
            return a.toMap().value("offset").toInt() < b.toMap().value("offset").toInt();
        });
        QList<QPair<int, QString>> markers;
        for (const auto &note : std::as_const(footnotes)) {
            *notes << printable(QStringLiteral("[%1] ").arg(notes->size() + 1) + bible.passageReference(book, chapter, number)
                                + QStringLiteral(" \u2014 ") + note.toMap().value("text").toString());
            markers.append({std::clamp(note.toMap().value("offset").toInt(), 0, int(plain.size())), QStringLiteral("[%1]").arg(notes->size())});
        }
        QSet<int> starts, ends;
        if (red) for (const auto &span : verse.value("wordsOfJesus").toList()) {
            starts.insert(span.toMap().value("start").toInt()); ends.insert(span.toMap().value("end").toInt());
        }
        const auto indent = [&](int line) { return QString(2 * levels.value(line).toInt(), ' '); };
        text += indent(0) + (bible.showVerseNumbers() ? TextSelection::superscript(number) + ' ' : QString());
        int line = 0;
        qsizetype marker = 0;
        for (int i = 0; i <= plain.size(); ++i) {
            if (ends.contains(i)) text += QStringLiteral("\x1b[39m");
            for (; marker < markers.size() && markers[marker].first == i; ++marker) text += markers[marker].second;
            if (starts.contains(i)) text += QStringLiteral("\x1b[31m");
            if (i == plain.size()) break;
            text += plain[i];
            if (plain[i] == '\n') text += indent(++line);
        }
    }
    return text + '\n';
}

int read(const QStringList &arguments, QTextStream &out, QTextStream &err, QTextStream &in, const Terminal &terminal)
{
    const QCommandLineOption footnotes(QStringLiteral("footnotes")), json(QStringLiteral("json"));
    const QCommandLineOption colorOption(QStringLiteral("color"), {}, QStringLiteral("when"));
    Parser p({footnotes, json, colorOption});
    if (!p.parse(arguments)) return usage(err, "read", p.parser.errorText());
    if (p.parser.isSet(p.help)) { printHelp(out, *find("read")); return Success; }
    if (p.parser.positionalArguments().isEmpty() && terminal.input) return usage(err, "read", "name a passage, such as: omaword read ps23");
    bool color = false;
    if (!colorWhen(p, colorOption, terminal, &color)) return usage(err, "read", "--color takes auto, always, or never.");
    BibleBackend bible;
    if (!useTranslation(bible, p.parser.value(p.translation), "read", err)) return Failure;
    const QStringList items = passages(bible, p.parser.positionalArguments(), in, terminal);
    if (items.isEmpty()) return usage(err, "read", "name a passage, such as: omaword read ps23");
    QJsonArray array;
    QStringList texts;
    int status = Success;
    for (const auto &item : std::as_const(items)) {
        const auto passage = bible.readPassage(item);
        if (passage.contains("error")) { failure(err, "read", item + ": " + passage.value("error").toString()); status = Failure; continue; }
        if (passage.contains("note")) err << printable(passage.value("note").toString()) << '.' << Qt::endl;
        QStringList notes;
        texts << readerText(bible, passage, color, &notes);
        if (p.parser.isSet(footnotes) && !notes.isEmpty()) texts.last() += '\n' + notes.join('\n') + '\n';
        QJsonArray verses;
        for (const auto &value : passage.value("verses").toList()) {
            const auto verse = value.toMap();
            QJsonArray levels, notesJson, words;
            for (const auto &level : verse.value("indents").toList()) levels.append(level.toInt());
            for (const auto &note : verse.value("footnotes").toList())
                notesJson.append(QJsonObject{{"offset", note.toMap().value("offset").toInt()}, {"text", note.toMap().value("text").toString()}});
            for (const auto &span : verse.value("wordsOfJesus").toList())
                words.append(QJsonObject{{"start", span.toMap().value("start").toInt()}, {"end", span.toMap().value("end").toInt()}});
            verses.append(QJsonObject{{"book", verse.value("book").toString()}, {"chapter", verse.value("chapter").toInt()},
                                      {"verse", verse.value("verse").toInt()}, {"text", verse.value("text").toString()},
                                      {"heading", verse.value("heading").toString()}, {"paragraphStart", verse.value("paragraphStart").toBool()},
                                      {"levels", levels}, {"footnotes", notesJson}, {"wordsOfJesus", words}});
        }
        QJsonObject row{{"reference", passage.value("reference").toString()}, {"translation", passage.value("abbreviation").toString()},
                        {"translationId", passage.value("versionId").toString()}, {"translationName", passage.value("name").toString()},
                        {"verses", verses}};
        if (passage.contains("note")) row.insert("note", passage.value("note").toString());
        array.append(row);
    }
    if (p.parser.isSet(json)) out << CommandLine::json(array);
    else if (!texts.isEmpty()) out << wrapLines(texts.join('\n'), terminal.columns);
    out.flush();
    return status;
}

int refs(const QStringList &arguments, QTextStream &out, QTextStream &err, QTextStream &in, const Terminal &terminal)
{
    const QCommandLineOption quoteOption(QStringLiteral("quote")), json(QStringLiteral("json"));
    const QCommandLineOption limit(QStringLiteral("limit"), {}, QStringLiteral("n")), colorOption(QStringLiteral("color"), {}, QStringLiteral("when"));
    Parser p({quoteOption, json, limit, colorOption});
    if (!p.parse(arguments)) return usage(err, "refs", p.parser.errorText());
    if (p.parser.isSet(p.help)) { printHelp(out, *find("refs")); return Success; }
    if (p.parser.positionalArguments().isEmpty() && terminal.input) return usage(err, "refs", "name a passage, such as: omaword refs jn316");
    bool number = true;
    const int count = p.parser.isSet(limit) ? p.parser.value(limit).toInt(&number) : (terminal.output ? 20 : 0);
    if (!number || count < 0) return usage(err, "refs", "--limit takes a number (0: every passage).");
    bool color = false;
    if (!colorWhen(p, colorOption, terminal, &color)) return usage(err, "refs", "--color takes auto, always, or never.");
    const auto bold = [&](const QString &text) { return color ? QStringLiteral("\x1b[1m") + text + QStringLiteral("\x1b[22m") : text; };
    BibleBackend bible;
    if (!useTranslation(bible, p.parser.value(p.translation), "refs", err)) return Failure;
    const QStringList items = passages(bible, p.parser.positionalArguments(), in, terminal);
    if (items.isEmpty()) return usage(err, "refs", "name a passage, such as: omaword refs jn316");
    QJsonArray array;
    QStringList texts;
    int status = Success;
    bool limited = false;
    for (const auto &item : std::as_const(items)) {
        const auto passage = bible.readPassage(item);
        if (passage.contains("error")) { failure(err, "refs", item + ": " + passage.value("error").toString()); status = Failure; continue; }
        if (passage.contains("note")) err << printable(passage.value("note").toString()) << '.' << Qt::endl;
        auto scope = passage.value("scope").toMap();
        const QString reference = passage.value("reference").toString(), abbreviation = passage.value("abbreviation").toString();
        if (!scope.value("start").toInt()) return usage(err, "refs", reference + " is a chapter; See also follows verses, such as John 3:16.");
        scope.insert("versionId", passage.value("versionId"));
        // As See also has them: passages tagged alike (with its tags, and
        // those of marks over it) in every translation, then OpenBible.info's.
        QVariantList ids;
        for (const auto &tag : bible.tagsForScope(scope)) if (!ids.contains(tag.toMap().value("id"))) ids << tag.toMap().value("id");
        for (const auto &range : bible.tagsForScope(scope, true))
            for (const auto &tag : range.toMap().value("tags").toList()) if (!ids.contains(tag.toMap().value("id"))) ids << tag.toMap().value("id");
        const QList<QPair<QString, QVariantList>> sections{{"Tagged alike", ids.isEmpty() ? QVariantList() : bible.relatedPassages(scope, ids, false, true)},
                                                          {"OpenBible.info", bible.crossReferences(scope)}};
        if (sections[0].second.isEmpty() && sections[1].second.isEmpty()) {
            failure(err, "refs", "nothing for " + reference + ": no passage tagged alike and no cross-reference.");
            status = Failure;
            continue;
        }
        QString text = bold(printable(reference + " (" + abbreviation + ')')) + '\n';
        QJsonObject row{{"reference", reference}, {"translation", abbreviation}, {"translationId", passage.value("versionId").toString()}};
        for (const auto &[name, rows] : sections) {
            const qsizetype shown = count ? std::min<qsizetype>(count, rows.size()) : rows.size();
            limited = limited || shown < rows.size();
            if (shown) text += '\n' + bold(name) + '\n';
            QJsonArray found;
            for (qsizetype i = 0; i < shown; ++i) {
                const auto at = rows[i].toMap();
                const bool tagged = name == QStringLiteral("Tagged alike");
                const QString version = at.value("versionAbbreviation").toString();
                QStringList tags;
                for (const auto &tag : at.value("tags").toList()) tags << tag.toMap().value("name").toString();
                if (p.parser.isSet(quoteOption)) text += '\n' + printable(bible.copyScope(at) + " (" + version + ')') + '\n';
                else {
                    QString excerpt = printable(at.value("excerpt").toString());
                    excerpt.replace('\n', ' ');
                    const QString label = at.value("reference").toString() + (tagged ? " (" + version + ") \u00b7 " + tags.join(" \u00b7 ") : QString());
                    text += bold(printable(label)) + "\n  " + excerpt + '\n';
                }
                if (!p.parser.isSet(json)) continue;
                QJsonObject entry{{"reference", at.value("reference").toString()}, {"translation", version},
                                  {"translationId", at.value("versionId").toString()}, {"text", bible.copyScope(at, true)}};
                if (tagged) entry.insert("tags", QJsonArray::fromStringList(tags));
                else entry.insert("votes", at.value("votes").toInt());
                found.append(entry);
            }
            row.insert(name == QStringLiteral("Tagged alike") ? "tagged" : "crossReferences", found);
        }
        texts << text;
        array.append(row);
    }
    if (p.parser.isSet(json)) out << CommandLine::json(array);
    else if (!texts.isEmpty()) out << texts.join('\n');
    out.flush();
    if (limited) err << "Each section shows its first " << count << "; --limit 0 shows them all." << Qt::endl;
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
    const QCommandLineOption in(QStringLiteral("in"), {}, QStringLiteral("part"));
    Parser p({all, json, limit, colorOption, in});
    if (!p.parse(arguments)) return usage(err, "search", p.parser.errorText());
    if (p.parser.isSet(p.help)) { printHelp(out, *find("search")); return Success; }
    const QString query = p.parser.positionalArguments().join(' ').trimmed();
    if (query.isEmpty()) return usage(err, "search", "name what to search for, such as: omaword search living water");
    if (p.parser.isSet(all) && p.parser.isSet(p.translation)) return usage(err, "search", "--all and --translation search different translations; use one.");
    bool number = true;
    const int count = p.parser.isSet(limit) ? p.parser.value(limit).toInt(&number) : (terminal.output ? 20 : 0);
    if (!number || count < 0) return usage(err, "search", "--limit takes a number (0: every match).");
    bool color = false;
    if (!colorWhen(p, colorOption, terminal, &color)) return usage(err, "search", "--color takes auto, always, or never.");
    BibleBackend bible;
    if (!useTranslation(bible, p.parser.value(p.translation), "search", err)) return Failure;
    // --in: a Testament, the Apocrypha (the books after them), or a book as
    // references name it; book IDs are the same in every translation.
    QSet<int> books;
    for (const QString &part : p.parser.values(in)) {
        const QString key = part.trimmed().toLower();
        const auto span = [&](int first, int last) { for (int book = first; book <= last; ++book) books.insert(book); };
        if (key == QStringLiteral("ot")) span(1, 39);
        else if (key == QStringLiteral("nt")) span(40, 66);
        else if (key == QStringLiteral("apocrypha")) span(67, int(BibleBooks::canon().size()));
        else if (const int book = bible.resolveBook(part.trimmed())) books.insert(book);
        else return usage(err, "search", "--in takes ot, nt, apocrypha, or a book, such as John: \u201c" + part + "\u201d is none of these.");
    }
    QStringList installed;
    for (const auto &value : bible.versions()) installed << value.toMap().value("id").toString();
    const auto result = ScriptureSearch::searchEditions(bible.searchEditions(p.parser.isSet(all)), query,
                                                        ScriptureSearch::defaultIndexPath(), installed);
    if (!result.error.isEmpty()) return usage(err, "search", result.error);
    QVariantList matches = result.matches;
    if (!books.isEmpty())
        matches.erase(std::remove_if(matches.begin(), matches.end(), [&](const QVariant &match) {
            return !books.contains(match.toMap().value("book").toInt());
        }), matches.end());
    const qsizetype shown = count ? std::min<qsizetype>(count, matches.size()) : matches.size();
    if (p.parser.isSet(json)) {
        QJsonArray array;
        for (qsizetype i = 0; i < shown; ++i) {
            const auto row = matches[i].toMap();
            QJsonArray spans;
            for (const auto &segment : row.value("displaySegments").toList()) spans.append(QJsonObject::fromVariantMap(segment.toMap()));
            array.append(QJsonObject{{"reference", row.value("reference").toString()}, {"translation", row.value("versionAbbreviation").toString()},
                                     {"translationId", row.value("versionId").toString()}, {"book", row.value("bookName").toString()},
                                     {"chapter", row.value("chapter").toInt()}, {"verse", row.value("start").toInt()}, {"endVerse", row.value("end").toInt()},
                                     {"text", row.value("text").toString()}, {"matches", spans}});
        }
        out << CommandLine::json(array);
    } else {
        for (qsizetype i = 0; i < shown; ++i) {
            const auto row = matches[i].toMap();
            const QString reference = printable(row.value("reference").toString() + " (" + row.value("versionAbbreviation").toString() + ")");
            // One line per match: the text's own breaks become spaces (each
            // replacement the same length, so spans hold).
            QString text = printable(row.value("text").toString());
            text.replace('\n', ' ').replace(QChar(0x2028), ' ');
            out << (color ? QStringLiteral("\x1b[1m") + reference + QStringLiteral("\x1b[0m") : reference) << '\n'
                << "  " << emphasized(text, row.value("displaySegments").toList(), color) << '\n';
        }
    }
    out.flush();
    if (matches.isEmpty()) { failure(err, "search", "no match for \u201c" + query + "\u201d."); return Failure; }
    if (shown < matches.size())
        err << "Showing " << shown << " of " << matches.size() << " matches; --limit 0 shows them all." << Qt::endl;
    return Success;
}

int translations(const QStringList &arguments, QTextStream &out, QTextStream &err)
{
    const QCommandLineOption names(QStringLiteral("names")), json(QStringLiteral("json"));
    Parser p({names, json});
    if (!p.parse(arguments)) return usage(err, "translations list", p.parser.errorText());
    if (p.parser.isSet(p.help)) { printHelp(out, *find("translations list")); return Success; }
    if (!p.parser.positionalArguments().isEmpty() || p.parser.isSet(p.translation)) return usage(err, "translations list", "takes no arguments.");
    BibleBackend bible;
    const auto rows = bible.versions();
    if (p.parser.isSet(json)) {
        QJsonArray array;
        for (const auto &value : rows) {
            const auto row = value.toMap();
            array.append(QJsonObject{{"id", row.value("id").toString()}, {"abbreviation", row.value("abbreviation").toString()},
                                     {"name", row.value("name").toString()}, {"active", row.value("active").toBool()}});
        }
        out << CommandLine::json(array);
        out.flush();
        return Success;
    }
    int width = 0;
    for (const auto &value : rows) width = std::max<int>(width, value.toMap().value("abbreviation").toString().size());
    for (const auto &value : rows) {
        const auto row = value.toMap();
        if (p.parser.isSet(names)) { out << row.value("abbreviation").toString() << '\n'; continue; }
        out << (row.value("active").toBool() ? "* " : "  ") << row.value("abbreviation").toString().leftJustified(width + 2)
            << printable(row.value("name").toString()) << "  (" << row.value("id").toString() << ")\n";
    }
    out.flush();
    if (rows.isEmpty() && !p.parser.isSet(names))
        err << branded("No translation is installed yet. Add one in OmaWord (Add version) or with omaword import <file>.") << Qt::endl;
    return Success;
}

int translationDetails(const QStringList &arguments, QTextStream &out, QTextStream &err)
{
    const QCommandLineOption json(QStringLiteral("json"));
    Parser p({json});
    if (!p.parse(arguments)) return usage(err, "translations show", p.parser.errorText());
    if (p.parser.isSet(p.help)) { printHelp(out, *find("translations show")); return Success; }
    if (p.parser.isSet(p.translation)) return usage(err, "translations show", "name the translation alone: omaword translations show KJV");
    const QString name = p.parser.positionalArguments().join(' ').trimmed();
    BibleBackend bible;
    if (name.isEmpty() && !ready(bible, "translations show", err)) return Failure;
    const QString id = name.isEmpty() ? bible.versionId() : bible.versionIdFor(name);
    if (id.isEmpty()) return failure(err, "translations show", "no installed translation \u201c" + name + "\u201d (installed: " + translationList(bible) + ").");
    const auto details = bible.versionDetails(id);
    if (details.isEmpty()) return failure(err, "translations show", "cannot read " + id + "'s text.");
    const auto count = [&](const char *key) { return details.value(key).toInt(); };
    if (p.parser.isSet(json)) {
        out << CommandLine::json(QJsonDocument(QJsonObject{{"id", id}, {"abbreviation", details.value("abbreviation").toString()},
            {"name", details.value("name").toString()}, {"active", id == bible.versionId()}, {"language", details.value("language").toString()},
            {"books", count("books")}, {"verses", count("verses")}, {"footnotes", count("footnotes")}, {"wordsOfJesus", count("wordsOfJesus")},
            {"revision", details.value("revision").toString()}, {"rights", details.value("copyright").toString()}}));
        out.flush();
        return Success;
    }
    const auto counted = [](int n, const QString &noun) { return QString::number(n) + ' ' + noun + (n == 1 ? "" : "s"); };
    QStringList contents{details.value("language").toString(), counted(count("books"), "book"), counted(count("verses"), "verse")};
    if (count("footnotes")) contents << counted(count("footnotes"), "footnote");
    if (count("wordsOfJesus")) contents << QStringLiteral("the words of Jesus marked");
    out << printable(details.value("name").toString() + " (" + details.value("abbreviation").toString() + ", " + id + ")\n"
                     + contents.join(QStringLiteral(" \u00b7 ")) + '\n');
    if (!details.value("revision").toString().isEmpty()) out << "AO revision " << printable(details.value("revision").toString()) << '\n';
    if (!details.value("copyright").toString().isEmpty()) out << '\n' << printable(details.value("copyright").toString()) << '\n';
    out.flush();
    return Success;
}

// The catalog entry a name means: an ID in any case, else an abbreviation
// (the English translation's, when several share it); {} with `error` else.
QVariantMap catalogEntry(const QVariantList &entries, const QString &name, QString *error)
{
    for (const auto &value : entries) if (value.toMap().value("id").toString() == name) return value.toMap();
    for (const auto &value : entries) if (value.toMap().value("id").toString().compare(name, Qt::CaseInsensitive) == 0) return value.toMap();
    QVariantList matches, english;
    for (const auto &value : entries) if (value.toMap().value("abbreviation").toString().compare(name, Qt::CaseInsensitive) == 0) matches << value;
    for (const auto &value : std::as_const(matches)) if (value.toMap().value("language").toString() == QStringLiteral("eng")) english << value;
    if (matches.size() == 1) return matches.first().toMap();
    if (english.size() == 1) return english.first().toMap();
    QStringList ids;
    for (const auto &value : std::as_const(matches)) ids << value.toMap().value("id").toString();
    *error = matches.isEmpty() ? "AO Lab's catalog has no translation \u201c" + name + "\u201d; omaword catalog lists them."
                               : "several translations go by \u201c" + name + "\u201d (" + ids.join(", ") + "); name one by its ID.";
    return {};
}
// Whether a catalog entry is installed, and whether AO Lab has another
// revision of it (Versions offers to download it again).
QString catalogState(const QVariantMap &entry, const QHash<QString, QString> &installed)
{
    const QString id = entry.value("id").toString(), revision = entry.value("sha256").toString();
    if (installed.contains(id)) return !revision.isEmpty() && revision != installed.value(id) ? QStringLiteral("installed; AO Lab has another revision")
                                                                                             : QStringLiteral("installed");
    return entry.value("compatibilityReason").toString().isEmpty() ? QString() : QStringLiteral("cannot be installed");
}
QHash<QString, QString> installedRevisions()
{
    BibleBackend bible;
    QHash<QString, QString> installed;
    for (const auto &value : bible.versions()) installed.insert(value.toMap().value("id").toString(), value.toMap().value("revision").toString());
    return installed;
}
QJsonObject catalogJson(const QVariantMap &entry, const QHash<QString, QString> &installed)
{
    const QString id = entry.value("id").toString();
    return {{"id", id}, {"abbreviation", entry.value("abbreviation").toString()}, {"name", entry.value("name").toString()},
            {"englishName", entry.value("englishName").toString()}, {"language", entry.value("language").toString()},
            {"description", entry.value("description").toString()}, {"installed", installed.contains(id)},
            {"installable", entry.value("compatibilityReason").toString().isEmpty()}, {"reason", entry.value("compatibilityReason").toString()},
            {"revision", entry.value("sha256").toString()}, {"rights", entry.value("copyright").toString()},
            {"permissions", entry.value("detailsUrl").toString()}};
}

int catalogList(const QStringList &arguments, QTextStream &out, QTextStream &err, const VersionCatalog::Dependencies &network)
{
    const QCommandLineOption language(QStringLiteral("language"), {}, QStringLiteral("code")), every(QStringLiteral("all-languages"));
    const QCommandLineOption json(QStringLiteral("json"));
    Parser p({language, every, json});
    if (!p.parse(arguments)) return usage(err, "catalog list", p.parser.errorText());
    if (p.parser.isSet(p.help)) { printHelp(out, *find("catalog list")); return Success; }
    if (p.parser.isSet(p.translation)) return usage(err, "catalog list", "--translation names an installed one; list them with omaword translations.");
    if (p.parser.isSet(language) && p.parser.isSet(every)) return usage(err, "catalog list", "--language and --all-languages list different ones; use one.");
    VersionCatalog catalog(network);
    if (catalog.entries().isEmpty()) return failure(err, "catalog list", catalog.error());
    const auto installed = installedRevisions();
    // As Add version filters: one language (English unless named), every word in its names, ID, or abbreviation.
    const QString code = p.parser.isSet(every) ? QString() : p.parser.isSet(language) ? p.parser.value(language).trimmed() : QStringLiteral("eng");
    const QStringList words = p.parser.positionalArguments().join(' ').toLower().split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    QVariantList shown;
    for (const auto &value : catalog.entries()) {
        const auto entry = value.toMap();
        if (!code.isEmpty() && entry.value("language").toString() != code) continue;
        const QString fields = QStringList{entry.value("name").toString(), entry.value("englishName").toString(),
                                           entry.value("id").toString(), entry.value("abbreviation").toString()}.join('\n').toLower();
        if (std::all_of(words.cbegin(), words.cend(), [&](const QString &word) { return fields.contains(word); })) shown << entry;
    }
    if (shown.isEmpty()) return failure(err, "catalog list", "no translation in AO Lab's catalog matches.");
    QJsonArray array;
    int width = 0;
    for (const auto &value : std::as_const(shown)) width = std::max<int>(width, value.toMap().value("abbreviation").toString().size());
    for (const auto &value : std::as_const(shown)) {
        const auto entry = value.toMap();
        array.append(catalogJson(entry, installed));
        const QString state = catalogState(entry, installed);
        if (!p.parser.isSet(json))
            out << printable(entry.value("abbreviation").toString().leftJustified(width + 2) + entry.value("name").toString()
                             + "  (" + entry.value("id").toString() + ')' + (state.isEmpty() ? QString() : QStringLiteral(" \u00b7 ") + state)) << '\n';
    }
    if (p.parser.isSet(json)) out << CommandLine::json(array);
    out.flush();
    return Success;
}

int catalogShow(const QStringList &arguments, QTextStream &out, QTextStream &err, const VersionCatalog::Dependencies &network)
{
    const QCommandLineOption json(QStringLiteral("json"));
    Parser p({json});
    if (!p.parse(arguments)) return usage(err, "catalog show", p.parser.errorText());
    if (p.parser.isSet(p.help)) { printHelp(out, *find("catalog show")); return Success; }
    const QString name = p.parser.positionalArguments().join(' ').trimmed();
    if (name.isEmpty() || p.parser.isSet(p.translation)) return usage(err, "catalog show", "name a translation by its ID, such as: omaword catalog show eng_kjv");
    VersionCatalog catalog(network);
    QString error;
    const auto entry = catalogEntry(catalog.entries(), name, &error);
    if (entry.isEmpty()) return failure(err, "catalog show", catalog.entries().isEmpty() ? catalog.error() : error);
    const auto installed = installedRevisions();
    if (p.parser.isSet(json)) { out << CommandLine::json(QJsonDocument(catalogJson(entry, installed))); out.flush(); return Success; }
    QString language = entry.value("languageEnglishName").toString();
    if (language.isEmpty()) language = entry.value("language").toString();
    QString state = catalogState(entry, installed);
    state = state.isEmpty() ? QStringLiteral("Not installed.")
        : state == QStringLiteral("cannot be installed") ? "OmaWord cannot install it: " + entry.value("compatibilityReason").toString()
        : state.left(1).toUpper() + state.mid(1) + '.';
    QString text = entry.value("name").toString() + " (" + entry.value("abbreviation").toString() + ", " + entry.value("id").toString() + ")\n"
                   + language + QStringLiteral(" \u00b7 ") + entry.value("description").toString() + '\n' + state + '\n';
    if (!entry.value("detailsUrl").toString().isEmpty()) text += "Permissions: " + entry.value("detailsUrl").toString() + '\n';
    if (!entry.value("copyright").toString().isEmpty()) text += '\n' + entry.value("copyright").toString() + '\n';
    out << printable(text);
    out.flush();
    return Success;
}

int catalogRefresh(const QStringList &arguments, QTextStream &out, QTextStream &err, const VersionCatalog::Dependencies &network)
{
    Parser p;
    if (!p.parse(arguments)) return usage(err, "catalog refresh", p.parser.errorText());
    if (p.parser.isSet(p.help)) { printHelp(out, *find("catalog refresh")); return Success; }
    if (!p.parser.positionalArguments().isEmpty() || p.parser.isSet(p.translation)) return usage(err, "catalog refresh", "takes no arguments.");
    VersionCatalog catalog(network);
    QEventLoop loop;
    QObject::connect(&catalog, &VersionCatalog::busyChanged, &loop, [&] { if (!catalog.busy()) loop.quit(); });
    catalog.refresh();
    if (catalog.busy()) loop.exec();
    if (!catalog.error().isEmpty()) return failure(err, "catalog refresh", catalog.error());
    out << "AO Lab's catalog lists " << catalog.entries().size() << " translations." << Qt::endl;
    return Success;
}

int books(const QStringList &arguments, QTextStream &out, QTextStream &err)
{
    const QCommandLineOption json(QStringLiteral("json"));
    Parser p({json});
    if (!p.parse(arguments)) return usage(err, "books list", p.parser.errorText());
    if (p.parser.isSet(p.help)) { printHelp(out, *find("books list")); return Success; }
    if (!p.parser.positionalArguments().isEmpty()) return usage(err, "books list", "takes no arguments; omaword books outline <book> prints one's headings.");
    BibleBackend bible;
    if (!useTranslation(bible, p.parser.value(p.translation), "books list", err)) return Failure;
    const auto rows = bible.books();
    QJsonArray array;
    int width = 0;
    for (const auto &value : rows) width = std::max<int>(width, value.toMap().value("name").toString().size());
    for (const auto &value : rows) {
        const auto row = value.toMap();
        const int chapters = row.value("chapters").toInt();
        array.append(QJsonObject{{"name", row.value("name").toString()}, {"code", row.value("code").toString()},
                                 {"abbreviation", row.value("abbreviation").toString()}, {"chapters", chapters}});
        if (!p.parser.isSet(json))
            out << printable(row.value("name").toString()).leftJustified(width + 2) << chapters << (chapters == 1 ? " chapter\n" : " chapters\n");
    }
    if (p.parser.isSet(json)) out << CommandLine::json(array);
    out.flush();
    return Success;
}

int outline(const QStringList &arguments, QTextStream &out, QTextStream &err)
{
    const QCommandLineOption json(QStringLiteral("json"));
    Parser p({json});
    if (!p.parse(arguments)) return usage(err, "books outline", p.parser.errorText());
    if (p.parser.isSet(p.help)) { printHelp(out, *find("books outline")); return Success; }
    const QString name = p.parser.positionalArguments().join(' ').trimmed();
    if (name.isEmpty()) return usage(err, "books outline", "name a book or chapter, such as: omaword books outline John");
    BibleBackend bible;
    if (!useTranslation(bible, p.parser.value(p.translation), "books outline", err)) return Failure;
    // A book's name, else a chapter (or the chapter of verses) as passages read.
    int book = bible.resolveBook(name), chapter = 0;
    if (!book) {
        const auto passage = bible.readPassage(name);
        if (passage.contains("error")) return failure(err, "books outline", name + ": " + passage.value("error").toString());
        book = passage.value("scope").toMap().value("book").toInt();
        chapter = passage.value("scope").toMap().value("chapter").toInt();
    }
    QVariantList chapters = bible.bookOutline(book);
    if (chapters.isEmpty()) return failure(err, "books outline", bible.versionAbbreviation() + " has no such book: \u201c" + name + "\u201d.");
    if (chapter)
        chapters.erase(std::remove_if(chapters.begin(), chapters.end(), [&](const QVariant &value) {
            return value.toMap().value("chapter").toInt() != chapter;
        }), chapters.end());
    QJsonArray array;
    for (const auto &value : std::as_const(chapters)) {
        const auto row = value.toMap();
        QJsonArray headings;
        if (!p.parser.isSet(json)) out << printable(row.value("title").toString()) << '\n';
        for (const auto &heading : row.value("headings").toList()) {
            const int verse = heading.toMap().value("verse").toInt();
            const QString text = heading.toMap().value("text").toString();
            headings.append(QJsonObject{{"verse", verse}, {"text", text}});
            // A heading of several lines (a psalm's book and title) is several headings.
            if (!p.parser.isSet(json))
                for (const auto &line : text.split('\n')) out << "  " << QString::number(verse).leftJustified(5) << printable(line) << '\n';
        }
        array.append(QJsonObject{{"chapter", row.value("chapter").toInt()}, {"title", row.value("title").toString()}, {"headings", headings}});
    }
    if (p.parser.isSet(json)) out << CommandLine::json(array);
    out.flush();
    return Success;
}

// A stored UTC time in local time: "2026-10-22 14:03".
QString localTime(const QString &utc)
{
    const auto time = QDateTime::fromString(utc, Qt::ISODateWithMs);
    return time.isValid() ? time.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")) : utc;
}

// A mark as marks list and marks show print it, as Marks shows it: its
// reference and translation, highlight, and tags, then its note (on one
// line, or whole) or, without one, its opening words.
QString markText(const QVariantMap &mark, bool whole)
{
    QStringList head{mark.value("reference").toString() + " (" + mark.value("versionAbbreviation").toString() + ')'};
    if (mark.value("highlight").toBool()) head << QStringLiteral("highlighted");
    QStringList tags;
    for (const auto &tag : mark.value("tags").toList()) tags << '#' + tag.toMap().value("name").toString();
    if (!tags.isEmpty()) head << tags.join(' ');
    const QString note = mark.value("note").toString();
    QStringList lines{head.join(QStringLiteral(" \u00b7 "))};
    for (const QString &line : (note.isEmpty() ? mark.value("excerpt").toString().simplified() : whole ? note : note.simplified()).split('\n'))
        lines << (line.isEmpty() ? line : QStringLiteral("  ") + line);
    return printable(lines.join('\n')) + '\n';
}
QJsonObject markJson(const QVariantMap &mark)
{
    QJsonArray tags;
    for (const auto &tag : mark.value("tags").toList()) tags.append(tag.toMap().value("name").toString());
    return {{"reference", mark.value("reference").toString()}, {"translation", mark.value("versionAbbreviation").toString()},
            {"translationId", mark.value("versionId").toString()}, {"highlight", mark.value("highlight").toBool()},
            {"note", mark.value("note").toString()}, {"tags", tags}, {"text", mark.value("excerpt").toString()},
            {"created", mark.value("created").toString()}, {"updated", mark.value("updated").toString()}};
}

int marks(const QStringList &arguments, QTextStream &out, QTextStream &err)
{
    const QCommandLineOption all(QStringLiteral("all")), tagOption(QStringLiteral("tag"), {}, QStringLiteral("name"));
    const QCommandLineOption notes(QStringLiteral("notes")), highlights(QStringLiteral("highlights")), json(QStringLiteral("json"));
    Parser p({all, tagOption, notes, highlights, json});
    if (!p.parse(arguments)) return usage(err, "marks list", p.parser.errorText());
    if (p.parser.isSet(p.help)) { printHelp(out, *find("marks list")); return Success; }
    if (p.parser.isSet(all) && p.parser.isSet(p.translation)) return usage(err, "marks list", "--all and --translation list different translations; use one.");
    BibleBackend bible;
    if (!useTranslation(bible, p.parser.value(p.translation), "marks list", err)) return Failure;
    // As Marks filters them: every word must match, #name a tag starting so.
    QStringList words = p.parser.positionalArguments().join(' ').split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    for (const auto &name : p.parser.values(tagOption)) words << '#' + name.trimmed();
    QJsonArray array;
    QString text;
    for (const auto &value : bible.marks()) {
        const auto mark = value.toMap();
        if (!p.parser.isSet(all) && mark.value("versionId").toString() != bible.versionId()) continue;
        if ((p.parser.isSet(notes) && mark.value("note").toString().isEmpty()) || (p.parser.isSet(highlights) && !mark.value("highlight").toBool())) continue;
        QStringList tags;
        for (const auto &tag : mark.value("tags").toList()) tags << tag.toMap().value("name").toString().toLower();
        const QString fields = QStringList{mark.value("reference").toString(), mark.value("versionAbbreviation").toString(),
                                           mark.value("note").toString(), tags.join('\n'), mark.value("excerpt").toString()}.join('\n').toLower();
        const bool matches = std::all_of(words.cbegin(), words.cend(), [&](const QString &word) {
            if (word.size() > 1 && word.startsWith('#'))
                return std::any_of(tags.cbegin(), tags.cend(), [&](const QString &name) { return name.startsWith(word.mid(1).toLower()); });
            return fields.contains(word.toLower());
        });
        if (!matches) continue;
        array.append(markJson(mark));
        text += markText(mark, false);
    }
    if (p.parser.isSet(json)) out << CommandLine::json(array);
    else out << text;
    out.flush();
    if (!array.isEmpty()) return Success;
    const QString where = p.parser.isSet(all) ? QStringLiteral("any translation") : bible.versionAbbreviation();
    if (words.isEmpty() && !p.parser.isSet(notes) && !p.parser.isSet(highlights)) { err << "No marks in " << printable(where) << " yet." << Qt::endl; return Success; }
    return failure(err, "marks list", "no mark in " + where + " matches.");
}

int marksOn(const QStringList &arguments, QTextStream &out, QTextStream &err)
{
    const QCommandLineOption json(QStringLiteral("json"));
    Parser p({json});
    if (!p.parse(arguments)) return usage(err, "marks show", p.parser.errorText());
    if (p.parser.isSet(p.help)) { printHelp(out, *find("marks show")); return Success; }
    const QString name = p.parser.positionalArguments().join(' ').trimmed();
    if (name.isEmpty()) return usage(err, "marks show", "name a passage, such as: omaword marks show jn316");
    BibleBackend bible;
    if (!useTranslation(bible, p.parser.value(p.translation), "marks show", err)) return Failure;
    const auto passage = bible.readPassage(name);
    if (passage.contains("error")) return failure(err, "marks show", name + ": " + passage.value("error").toString());
    if (passage.contains("note")) err << printable(passage.value("note").toString()) << '.' << Qt::endl;
    // Bible order in this translation (its books', then chapters and verses);
    // a whole chapter's scope runs from verse 0 to past its last.
    QHash<int, int> order;
    const auto books = bible.books();
    for (int i = 0; i < books.size(); ++i) order.insert(books[i].toMap().value("id").toInt(), i);
    const auto at = [&](const QVariantMap &scope, bool end) {
        const int verse = scope.value(end ? "end" : "start").toInt();
        return std::make_tuple(order.value(scope.value(end ? "endBook" : "book").toInt(), 1000),
                               scope.value(end ? "endChapter" : "chapter").toInt(), end && !verse ? 1000 : verse);
    };
    const auto scope = passage.value("scope").toMap();
    QJsonArray array;
    QStringList texts;
    for (const auto &value : bible.marks()) {
        const auto mark = value.toMap();
        if (mark.value("versionId") != passage.value("versionId") || at(mark, false) > at(scope, true) || at(scope, false) > at(mark, true)) continue;
        array.append(markJson(mark));
        texts << markText(mark, true);
    }
    if (array.isEmpty())
        return failure(err, "marks show", "no marks on " + passage.value("reference").toString() + " in " + passage.value("abbreviation").toString() + '.');
    if (p.parser.isSet(json)) out << CommandLine::json(array);
    else out << texts.join('\n');
    out.flush();
    return Success;
}

int exportMarks(const QStringList &arguments, QTextStream &out, QTextStream &err)
{
    const QCommandLineOption output({QStringLiteral("o"), QStringLiteral("output")}, {}, QStringLiteral("file"));
    Parser p({output});
    if (!p.parse(arguments)) return usage(err, "marks export", p.parser.errorText());
    if (p.parser.isSet(p.help)) { printHelp(out, *find("marks export")); return Success; }
    if (!p.parser.positionalArguments().isEmpty() || p.parser.isSet(p.translation))
        return usage(err, "marks export", "takes no arguments; name a file with -o, such as: omaword marks export -o notes.md");
    BibleBackend bible;
    if (!ready(bible, "marks export", err)) return Failure;
    const QString markdown = bible.notesMarkdown();
    if (!p.parser.isSet(output)) { out << printable(markdown); out.flush(); return Success; }
    if (!bible.saveExport(QFileInfo(p.parser.value(output)).absoluteFilePath(), markdown.toUtf8())) return failure(err, "marks export", bible.error());
    return Success;
}

int backup(const QStringList &arguments, QTextStream &out, QTextStream &err)
{
    Parser p;
    if (!p.parse(arguments)) return usage(err, "marks backup", p.parser.errorText());
    if (p.parser.isSet(p.help)) { printHelp(out, *find("marks backup")); return Success; }
    const QStringList positional = p.parser.positionalArguments();
    if (positional.size() != 1 || p.parser.isSet(p.translation))
        return usage(err, "marks backup", "name one file, or - to print it, such as: omaword marks backup omaword-backup.json");
    BibleBackend bible;
    if (!ready(bible, "marks backup", err)) return Failure;
    if (positional.first() == QStringLiteral("-")) {
        QByteArray bytes;
        if (!bible.backupJson(&bytes)) return failure(err, "marks backup", bible.error());
        out << CommandLine::json(bytes);
        out.flush();
        return Success;
    }
    if (!bible.exportBackup(QUrl::fromLocalFile(QFileInfo(positional.first()).absoluteFilePath()))) return failure(err, "marks backup", bible.error());
    return Success;
}

int tags(const QStringList &arguments, QTextStream &out, QTextStream &err)
{
    const QCommandLineOption json(QStringLiteral("json"));
    Parser p({json});
    if (!p.parse(arguments)) return usage(err, "tags list", p.parser.errorText());
    if (p.parser.isSet(p.help)) { printHelp(out, *find("tags list")); return Success; }
    if (!p.parser.positionalArguments().isEmpty() || p.parser.isSet(p.translation)) return usage(err, "tags list", "takes no arguments.");
    BibleBackend bible;
    const auto rows = bible.tags();
    QJsonArray array;
    int width = 0;
    for (const auto &value : rows) width = std::max<int>(width, value.toMap().value("name").toString().size());
    for (const auto &value : rows) {
        const auto row = value.toMap();
        const int count = row.value("count").toInt();
        array.append(QJsonObject{{"name", row.value("name").toString()}, {"count", count}});
        if (!p.parser.isSet(json))
            out << printable(row.value("name").toString()).leftJustified(width + 2) << count << (count == 1 ? " passage\n" : " passages\n");
    }
    if (p.parser.isSet(json)) out << CommandLine::json(array);
    out.flush();
    if (rows.isEmpty() && !p.parser.isSet(json)) err << "No tags yet." << Qt::endl;
    return Success;
}

int desk(const QStringList &arguments, QTextStream &out, QTextStream &err)
{
    Parser p;
    if (!p.parse(arguments)) return usage(err, "desk show", p.parser.errorText());
    if (p.parser.isSet(p.help)) { printHelp(out, *find("desk show")); return Success; }
    if (!p.parser.positionalArguments().isEmpty() || p.parser.isSet(p.translation)) return usage(err, "desk show", "takes no arguments.");
    BibleBackend bible;
    const QString page = bible.deskPage();
    if (page.isEmpty()) { err << "The Desk is empty." << Qt::endl; return Success; }
    out << printable(page) << (page.endsWith('\n') ? "" : "\n");
    out.flush();
    return Success;
}

int history(const QStringList &arguments, QTextStream &out, QTextStream &err)
{
    const QCommandLineOption json(QStringLiteral("json"));
    Parser p({json});
    if (!p.parse(arguments)) return usage(err, "history list", p.parser.errorText());
    if (p.parser.isSet(p.help)) { printHelp(out, *find("history list")); return Success; }
    if (!p.parser.positionalArguments().isEmpty() || p.parser.isSet(p.translation)) return usage(err, "history list", "takes no arguments.");
    BibleBackend bible;
    const auto rows = bible.history();
    QJsonArray array;
    for (const auto &value : rows) {
        const auto row = value.toMap();
        array.append(QJsonObject{{"reference", row.value("reference").toString()}, {"translation", row.value("versionAbbreviation").toString()},
                                 {"translationId", row.value("versionId").toString()}, {"visitedAt", row.value("visitedAt").toString()}});
        if (!p.parser.isSet(json))
            out << localTime(row.value("visitedAt").toString()) << "  "
                << printable(row.value("reference").toString() + " (" + row.value("versionAbbreviation").toString() + ")\n");
    }
    if (p.parser.isSet(json)) out << CommandLine::json(array);
    out.flush();
    if (!bible.historyEnabled()) err << "History is off in OmaWord, so it keeps nothing." << Qt::endl;
    else if (rows.isEmpty() && !p.parser.isSet(json)) err << "History is empty." << Qt::endl;
    return Success;
}

int ribbon(const QStringList &arguments, QTextStream &out, QTextStream &err)
{
    const QCommandLineOption json(QStringLiteral("json"));
    Parser p({json});
    if (!p.parse(arguments)) return usage(err, "ribbon show", p.parser.errorText());
    if (p.parser.isSet(p.help)) { printHelp(out, *find("ribbon show")); return Success; }
    if (!p.parser.positionalArguments().isEmpty() || p.parser.isSet(p.translation)) return usage(err, "ribbon show", "takes no arguments.");
    BibleBackend bible;
    const auto ribbon = bible.ribbon();
    if (ribbon.isEmpty()) return failure(err, "ribbon show", "the ribbon lies nowhere; place it in OmaWord.");
    const QString id = ribbon.value("versionId").toString();
    QString abbreviation = id;
    for (const auto &value : bible.versions()) if (value.toMap().value("id").toString() == id) abbreviation = value.toMap().value("abbreviation").toString();
    if (p.parser.isSet(json))
        out << CommandLine::json(QJsonDocument(QJsonObject{{"reference", ribbon.value("reference").toString()}, {"translation", abbreviation},
                                                           {"translationId", id}, {"placedAt", ribbon.value("placedAt").toString()}}));
    else out << printable(ribbon.value("reference").toString() + " (" + abbreviation + ")\n");
    out.flush();
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
        if (passage.contains("note")) err << printable(passage.value("note").toString()) << '.' << Qt::endl;
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

// Imported text (a book or translation name, a verse) or an argument could
// hold an escape sequence that rewrites the screen or the window's title.
// Replaced rather than removed, so match offsets in a line still hold.
QString printable(QString text)
{
    for (QChar &c : text) {
        const char16_t u = c.unicode();
        if ((u < 0x20 && u != '\n' && u != '\t') || (u >= 0x7f && u <= 0x9f)) c = QChar::ReplacementCharacter;
    }
    return text;
}

bool headless(const QStringList &arguments)
{
    if (arguments.isEmpty()) return false;
    const QString first = arguments.first();
    if (QStringList{"-h", "--help", "-v", "--version", "help"}.contains(first)) return true;
    if (const auto *command = find(first)) return command->headless || arguments.contains("-h") || arguments.contains("--help");
    return false;
}

int run(const QStringList &arguments, QTextStream &out, QTextStream &err, QTextStream &in, const Terminal &terminal,
        const VersionCatalog::Dependencies &network)
{
    const QString first = arguments.value(0);
    if (first == QStringLiteral("-v") || first == QStringLiteral("--version")) { out << Brand::name() << ' ' << Brand::version() << Qt::endl; return Success; }
    if (first == QStringLiteral("-h") || first == QStringLiteral("--help")) { printHelp(out); return Success; }
    QStringList rest = arguments.mid(1);
    if (first == QStringLiteral("help")) {
        if (rest.isEmpty()) { printHelp(out); return Success; }
        if (const auto *command = find(rest.join(' '))) { printHelp(out, *command); return Success; }
        return usage(err, {}, "no command \u201c" + rest.join(' ') + "\u201d.");
    }
    const Command *command = find(first);
    if (!command) return usage(err, {}, "no command \u201c" + first + "\u201d.");
    // A group runs the command named after it, else its default one.
    if (!command->defaultCommand.isEmpty()) {
        const QString next = rest.value(0);
        if (next == QStringLiteral("-h") || next == QStringLiteral("--help")) { printHelp(out, *command); return Success; }
        if (const auto *named = find(command->name + ' ' + next)) { command = named; rest.removeFirst(); }
        else if (!command->words && !next.isEmpty() && !next.startsWith('-')) {
            QStringList names;
            for (const auto *subcommand : subcommands(*command)) names << subcommand->name.mid(command->name.size() + 1);
            return usage(err, command->name, "no command \u201c" + next + "\u201d; it has " + names.join(QStringLiteral(", ")) + '.');
        } else command = find(command->name + ' ' + command->defaultCommand);
    }
    const QString &name = command->name;
    if (name == QStringLiteral("read")) return read(rest, out, err, in, terminal);
    if (name == QStringLiteral("quote")) return quote(rest, out, err, in, terminal);
    if (name == QStringLiteral("refs")) return refs(rest, out, err, in, terminal);
    if (name == QStringLiteral("search")) return search(rest, out, err, terminal);
    if (name == QStringLiteral("translations list")) return translations(rest, out, err);
    if (name == QStringLiteral("translations show")) return translationDetails(rest, out, err);
    if (name == QStringLiteral("catalog list")) return catalogList(rest, out, err, network);
    if (name == QStringLiteral("catalog show")) return catalogShow(rest, out, err, network);
    if (name == QStringLiteral("catalog refresh")) return catalogRefresh(rest, out, err, network);
    if (name == QStringLiteral("books list")) return books(rest, out, err);
    if (name == QStringLiteral("books outline")) return outline(rest, out, err);
    if (name == QStringLiteral("marks list")) return marks(rest, out, err);
    if (name == QStringLiteral("marks show")) return marksOn(rest, out, err);
    if (name == QStringLiteral("marks export")) return exportMarks(rest, out, err);
    if (name == QStringLiteral("marks backup")) return backup(rest, out, err);
    if (name == QStringLiteral("tags list")) return tags(rest, out, err);
    if (name == QStringLiteral("desk show")) return desk(rest, out, err);
    if (name == QStringLiteral("history list")) return history(rest, out, err);
    if (name == QStringLiteral("ribbon show")) return ribbon(rest, out, err);
    if (name == QStringLiteral("link")) return links(rest, out, err, in, terminal);
    if (name == QStringLiteral("completion")) return completion(rest, out, err);
    // open, lookup, or import with --help.
    printHelp(out, *command);
    return Success;
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
        *error = branded("omaword" + name + ": " + printable(message) + "\nRun 'omaword help" + name + "' for usage.");
        return false;
    };
    if (!parser.parse(QStringList{QStringLiteral("omaword")} + words)) return fail(parser.errorText());
    const QStringList positional = parser.positionalArguments();
    *request = SingleInstance::Request::fromArguments({}, parser.value(importOption), parser.isSet(importOption));
    request->translation = parser.value(translationOption);
    if (parser.isSet(here) && command != QStringLiteral("open")) return fail("--here goes with open: omaword open <passage> --here");
    if (parser.isSet(translationOption) && command == QStringLiteral("import"))
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
        if (positional.size() != 1) return fail("name one AO complete JSON file, such as: omaword import eng_net_complete.json");
        request->importPath = QFileInfo(positional.first()).absoluteFilePath();
        request->newWindow = false;
    } else if (positional.size() == 1 && parseLink(positional.first(), &request->reference, &request->translation)) {
        // Even a link naming no passage goes to the reader you were using
        // (switched to its translation, if it names one): never a new window.
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
    return true;
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
