#include "testenvironment.h"
#include "biblebackend.h"
#include "brand.h"
#include "commandline.h"
#include <QtTest/QTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <cstring>
#include "aofixture.h"

// AO Lab, served from memory: each path's body, or 404.
class FakeReply : public QNetworkReply
{
public:
    FakeReply(const QNetworkRequest &request, const QByteArray &body, int status, QObject *parent)
        : QNetworkReply(parent), m_body(body)
    {
        setRequest(request); setUrl(request.url()); setOperation(QNetworkAccessManager::GetOperation);
        open(QIODevice::ReadOnly);
        QTimer::singleShot(0, this, [this, status] {
            setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
            setHeader(QNetworkRequest::ContentLengthHeader, m_body.size());
            emit metaDataChanged();
            if (isFinished()) return;
            emit readyRead();
            emit downloadProgress(m_body.size(), m_body.size());
            setFinished(true);
            emit finished();
        });
    }
    void abort() override { if (!isFinished()) { setError(OperationCanceledError, "Canceled"); setFinished(true); emit finished(); } }
    qint64 bytesAvailable() const override { return m_body.size() - m_offset + QNetworkReply::bytesAvailable(); }
protected:
    qint64 readData(char *data, qint64 maximum) override
    {
        const qint64 count = std::min<qint64>(maximum, m_body.size() - m_offset);
        if (count <= 0) return isFinished() ? -1 : 0;
        std::memcpy(data, m_body.constData() + m_offset, size_t(count));
        m_offset += count;
        return count;
    }
private:
    QByteArray m_body;
    qint64 m_offset = 0;
};
class FakeNetwork : public QNetworkAccessManager
{
public:
    QHash<QString, QByteArray> bodies;
protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override
    {
        const QString path = request.url().path();
        return new FakeReply(request, bodies.value(path), bodies.contains(path) ? 200 : 404, this);
    }
};

// The command line in process: the commands that print (CommandLine::run)
// and the requests the running OmaWord takes (CommandLine::request).
class CliTest : public QObject
{
    Q_OBJECT
    QTemporaryDir fixtures;
    QString net, kjv;
    struct Output { int status; QString out, err; };
    static bool archiveFile(const QString &path, const QByteArray &id, const QByteArray &john, const QByteArray &genesis,
                            const QByteArray &title = "Fixture", const QByteArray &johnName = "John") {
        auto root = AoFixture::complete(QString::fromUtf8(id), {
            AoFixture::book("GEN", 1, {AoFixture::chapter(1, {AoFixture::verse(1, {QString::fromUtf8(genesis)}), AoFixture::verse(2, {"The earth was empty."})})}),
            AoFixture::book("JHN", 2, {AoFixture::chapter(3, {AoFixture::verse(16, {QString::fromUtf8(john)}), AoFixture::verse(17, {"He came to save the world."})})}, QString::fromUtf8(johnName))
        }, QString::fromUtf8(title));
        if (id == "evil") {
            auto translation = root["translation"].toObject(); translation["shortName"] = "EB"; root["translation"] = translation;
        }
        return AoFixture::write(path, root);
    }
    static void install(const QString &path) {
        BibleBackend bible; bible.importAo(QUrl::fromLocalFile(path));
        QTRY_VERIFY_WITH_TIMEOUT(!bible.busy(), 30000);
        QVERIFY2(bible.ready() && bible.error().isEmpty(), qPrintable(bible.error()));
    }
    static Output run(const QStringList &arguments, const QString &input = {}, bool terminal = false,
                      const VersionCatalog::Dependencies &network = {}) {
        QString out, err, in = input;
        QTextStream outStream(&out), errStream(&err), inStream(&in);
        CommandLine::Terminal tty; tty.input = input.isEmpty() && terminal; tty.output = terminal;
        const int status = CommandLine::run(arguments, outStream, errStream, inStream, tty, network);
        outStream.flush(); errStream.flush();
        return {status, out, err};
    }
private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("OmaWordTests");
        QCoreApplication::setApplicationName("omaword-cli-test");
        net = fixtures.filePath("eng_net_complete.json"); kjv = fixtures.filePath("eng_kjv_complete.json");
        QVERIFY(archiveFile(net, "eng_net", "For God so loved the world.", "In the beginning God created."));
        QVERIFY(archiveFile(kjv, "eng_kjv", "For God so loved the world, that he gave.", "In the beginning God created the heaven."));
    }
    void helpVersionAndUsage() {
        auto help = run({"--help"});
        QCOMPARE(help.status, 0);
        for (const QString part : {"Usage:", "Commands:", "quote", "search", "translations", "Options:", "--translation", "Passages:", "Links:", "Exit status"})
            QVERIFY2(help.out.contains(part), qPrintable(part));
        QCOMPARE(run({"help"}).out, help.out);
        const auto quote = run({"help", "quote"});
        QCOMPARE(quote.status, 0);
        QVERIFY(quote.out.startsWith(QStringLiteral("omaword quote \u2014 Print passages as quotations")));
        QVERIFY(quote.out.contains("-t, --translation <name>") && quote.out.contains("--plain") && quote.out.contains("Examples:"));
        QCOMPARE(run({"quote", "--help"}).out, quote.out);
        QCOMPARE(run({"versions", "--help"}).out, run({"help", "translations"}).out);
        QVERIFY(run({"open", "--help"}).out.contains("--here"));
        // A group lists its commands; its name alone runs the default one.
        const auto group = run({"translations", "--help"});
        QVERIFY2(group.out.contains("omaword translations [list] [--names] [--json]\n"), qPrintable(group.out));
        QVERIFY(group.out.contains("Commands:\n  list") && group.out.contains("(the default)"));
        QCOMPARE(run({"translations", "list", "--help"}).out, run({"help", "translations", "list"}).out);
        QCOMPARE(run({"help", "versions", "list"}).out, run({"help", "translations", "list"}).out);
        auto unknownCommand = run({"translations", "shwo"});
        QCOMPARE(unknownCommand.status, 2);
        QVERIFY2(unknownCommand.err.contains(QStringLiteral("no command \u201cshwo\u201d; it has list")), qPrintable(unknownCommand.err));
        // Every page fits 80 columns: the main one, each command's, and each of a group's commands'.
        const auto commandsOn = [](const QString &page) {
            QStringList names;
            const auto lines = page.split('\n');
            for (qsizetype i = lines.indexOf("Commands:") + 1; i > 0 && i < lines.size() && lines[i].startsWith("  "); ++i)
                if (lines[i][2] != ' ') names << lines[i].trimmed().section(' ', 0, 0);
            return names;
        };
        QStringList pages{help.out};
        for (const auto &name : commandsOn(help.out)) {
            const auto page = run({"help", name});
            QVERIFY2(page.status == 0, qPrintable(name));
            pages << page.out;
            for (const auto &subcommand : commandsOn(page.out)) pages << run({"help", name, subcommand}).out;
        }
        QVERIFY(pages.size() > 10);
        for (const auto &text : std::as_const(pages))
            for (const auto &line : text.split('\n')) QVERIFY2(line.size() <= 80, qPrintable(line));
        auto missing = run({"help", "nope"});
        QCOMPARE(missing.status, 2); QVERIFY(missing.err.contains("no command"));
        QCOMPARE(run({"--version"}).out.trimmed(), Brand::name() + ' ' + Brand::version());
        auto bad = run({"quote", "--bogus"});
        QCOMPARE(bad.status, 2); QVERIFY(bad.err.contains("bogus")); QVERIFY(bad.err.contains("omaword help quote"));
        QCOMPARE(run({"quote"}, {}, true).status, 2);
    }
    void nothingInstalled() {
        auto quote = run({"quote", "jn316"});
        QCOMPARE(quote.status, 1); QVERIFY(quote.err.contains("no translation is installed"));
        QCOMPARE(run({"translations"}).status, 0);
        QCOMPARE(run({"translations", "show"}).status, 1);
    }
    void quoteSearchTranslationsAndLinks() {
        install(kjv); install(net);
        // A quotation as OmaWord copies it, the em dash spaced; words of one passage join.
        const QString john = QStringLiteral("\u201c\u00b9\u2076 For God so loved the world.\u201d\n\n\u2014 John 3:16\n");
        QCOMPARE(run({"quote", "John 3:16"}).out, john);
        QCOMPARE(run({"quote", "john", "3:16"}).out, john);
        QCOMPARE(run({"quote", "jn316"}).out, john);
        QCOMPARE(run({"quote", "jn316", "--plain"}).out, QString("For God so loved the world.\n"));
        auto kjvQuote = run({"quote", "jn316", "-t", "kjv"});
        QCOMPARE(kjvQuote.status, 0); QVERIFY(kjvQuote.out.contains("that he gave"));
        QCOMPARE(run({"quote", "jn316", "--translation=ENG_KJV", "--plain"}).out, QString("For God so loved the world, that he gave.\n"));
        auto unknown = run({"quote", "jn316", "-t", "xyz"});
        QCOMPARE(unknown.status, 1); QVERIFY(unknown.err.contains("no installed translation")); QVERIFY(unknown.err.contains("NET"));
        // Several passages, a blank line apart; a bad one fails alone.
        auto two = run({"quote", "jn316", "gen1:1", "--plain"});
        QCOMPARE(two.status, 0); QCOMPARE(two.out, QString("For God so loved the world.\n\nIn the beginning God created.\n"));
        auto partly = run({"quote", "jn316", "Nowhere 9"});
        QCOMPARE(partly.status, 1); QVERIFY(partly.out.contains("John 3:16")); QVERIFY(partly.err.contains("Nowhere 9"));
        // Passages from standard input.
        QCOMPARE(run({"quote", "-", "--plain"}, "jn316\n\ngen 1:1\n").out, two.out);
        QCOMPARE(run({"quote", "--plain"}, "jn316\n").out, QString("For God so loved the world.\n"));
        // A chapter.
        QVERIFY(run({"quote", "John 3"}).out.endsWith(QStringLiteral("\u2014 John 3\n")));
        // JSON.
        const auto json = QJsonDocument::fromJson(run({"quote", "jn316", "--json", "-t", "KJV"}).out.toUtf8()).array();
        QCOMPARE(json.size(), 1);
        const auto first = json.first().toObject();
        QCOMPARE(first["reference"].toString(), QString("John 3:16")); QCOMPARE(first["translation"].toString(), QString("KJV"));
        QCOMPARE(first["verses"].toArray().size(), 1); QCOMPARE(first["verses"].toArray().first().toObject()["verse"].toInt(), 16);
        // translations: * marks the one being read.
        auto list = run({"translations"});
        QCOMPARE(list.status, 0); QVERIFY(list.out.contains("* NET")); QVERIFY(list.out.contains("  KJV"));
        QCOMPARE(run({"translations", "--names"}).out, QString("NET\nKJV\n"));
        QCOMPARE(run({"translations", "list", "--names"}).out, QString("NET\nKJV\n"));
        QCOMPARE(QJsonDocument::fromJson(run({"versions", "--json"}).out.toUtf8()).array().size(), 2);
        // search.
        auto found = run({"search", "loved"});
        QCOMPARE(found.status, 0); QCOMPARE(found.out, QString("John 3:16 (NET)\n  For God so loved the world.\n"));
        auto everywhere = run({"search", "loved", "--all"});
        QVERIFY(everywhere.out.contains("John 3:16 (NET)") && everywhere.out.contains("John 3:16 (KJV)"));
        QVERIFY(everywhere.out.indexOf("(NET)") < everywhere.out.indexOf("(KJV)"));
        auto colored = run({"search", "loved", "--color", "always"});
        QVERIFY(colored.out.contains("\x1b[1;4mloved\x1b[0m"));
        QVERIFY(!run({"search", "loved"}, {}, true).out.contains("\x1b")); // NO_COLOR-style: no color unless allowed.
        auto limited = run({"search", "the", "--limit", "1"});
        QCOMPARE(limited.out.count("\n  "), 1); QVERIFY(limited.err.contains("Showing 1 of"));
        auto none = run({"search", "nowhere"});
        QCOMPARE(none.status, 1); QVERIFY(none.err.contains("no match"));
        // Words use smart case, as in the app: a capital matches it; \c ignores it.
        QCOMPARE(run({"search", "The"}).out, QString("Genesis 1:2 (NET)\n  The earth was empty.\n"));
        QCOMPARE(run({"search", "the", "--limit", "0"}).out.count("\n  "), 4);
        QCOMPARE(run({"search", "THE"}).status, 1);
        QCOMPARE(run({"search", "THE\\c", "--limit", "0"}).out.count("\n  "), 4);
        // Words in slashes are a regular expression, smart case, within one verse.
        QCOMPARE(run({"search", "/lov(e|ed) the/"}).out, QString("John 3:16 (NET)\n  For God so loved the world.\n"));
        QVERIFY(run({"search", "/loved?/", "--color", "always"}).out.contains("\x1b[1;4mloved\x1b[0m"));
        auto beginning = run({"search", "/^In the/", "--all"});
        QCOMPARE(beginning.status, 0);
        QCOMPARE(beginning.out.count("Genesis 1:1"), 2);
        QCOMPARE(run({"search", "/^IN the/"}).status, 1);
        QCOMPARE(run({"search", "/^IN the\\c/"}).status, 0);
        auto badPattern = run({"search", "/(god/"});
        QCOMPARE(badPattern.status, 2); QVERIFY2(badPattern.err.contains("Invalid regular expression"), qPrintable(badPattern.err));
        const auto regexJson = QJsonDocument::fromJson(run({"search", "/wor\\w+/", "--json"}).out.toUtf8()).array();
        QCOMPARE(regexJson.size(), 2); // world, in John 3:16 and 3:17
        QCOMPARE(regexJson.first().toObject()["matches"].toArray().first().toObject()["length"].toInt(), 5);
        QCOMPARE(run({"search", "x", "--all", "-t", "KJV"}).status, 2);
        QCOMPARE(run({"search", "x", "--limit", "many"}).status, 2);
        // --in: a Testament, the Apocrypha, or books, filtered before --limit counts.
        QCOMPARE(run({"search", "the", "--in", "ot", "--limit", "0"}).out.count("\n  "), 2);
        QCOMPARE(run({"search", "the", "--in", "NT", "--limit", "0"}).out.count("\n  "), 2);
        QCOMPARE(run({"search", "the", "--in", "jn", "--in", "Genesis", "--limit", "0"}).out.count("\n  "), 4);
        auto within = run({"search", "the", "--in", "John", "--limit", "1"});
        QVERIFY(within.out.startsWith("John 3:16 (NET)")); QVERIFY2(within.err.contains("Showing 1 of 2"), qPrintable(within.err));
        QCOMPARE(run({"search", "the", "--in", "apocrypha"}).status, 1);
        QVERIFY(run({"search", "loved", "--all", "--in", "nt"}).out.contains("John 3:16 (KJV)"));
        const auto inJson = QJsonDocument::fromJson(run({"search", "the", "--in", "ot", "--json"}).out.toUtf8()).array();
        QCOMPARE(inJson.size(), 2); QCOMPARE(inJson.first().toObject()["book"].toString(), QString("Genesis"));
        auto notABook = run({"search", "the", "--in", "Hezekiah"});
        QCOMPARE(notABook.status, 2); QVERIFY2(notABook.err.contains("\u201cHezekiah\u201d"), qPrintable(notABook.err));
        const auto matches = QJsonDocument::fromJson(run({"search", "loved", "--json", "-t", "kjv"}).out.toUtf8()).array();
        QCOMPARE(matches.size(), 1); QCOMPARE(matches.first().toObject()["translation"].toString(), QString("KJV"));
        QCOMPARE(matches.first().toObject()["matches"].toArray().size(), 1);
        // link.
        QCOMPARE(run({"link", "jn316"}).out, QString("omaword:John%203:16\n"));
        QCOMPARE(run({"link", "John 3:16-17", "-t", "kjv"}).out, QString("omaword:John%203:16-17?translation=KJV\n"));
    }
    // What a translation brings (its name, a book's, a verse) and what you
    // type print with each control character as U+FFFD, one for one: no
    // escape sequence reaches the terminal, and search emphasis still
    // covers its word. JSON keeps the text exact, escaped.
    void controlCharactersPrintReplaced() {
        const QString evil = fixtures.filePath("evil_usfm.zip");
        QVERIFY(archiveFile(evil, "evil", "For God so \x1b]0;owned\x07 \xc2\x9b" "2J\x7f loved the world.", "In the beginning.",
                            "Evil\x1b[31m Bible", "Jo\x1b[2Jhn"));
        install(evil);
        const auto controls = [](const QString &text) {
            int count = 0;
            for (const QChar c : text) count += (c.unicode() < 0x20 && c != '\n' && c != '\t') || (c.unicode() >= 0x7f && c.unicode() <= 0x9f);
            return count;
        };
        const QString verse = QStringLiteral("For God so \uFFFD]0;owned\uFFFD \uFFFD" "2J\uFFFD loved the world.");
        QCOMPARE(run({"quote", "jn316", "-t", "EB", "--plain"}).out, verse + '\n');
        const auto quotation = run({"quote", "jn316", "-t", "EB"});
        QCOMPARE(controls(quotation.out), 0); QVERIFY2(quotation.out.endsWith(QStringLiteral("Jo\uFFFD[2Jhn 3:16\n")), qPrintable(quotation.out));
        const auto found = run({"search", "loved", "-t", "EB", "--color", "always"});
        QCOMPARE(found.out, QStringLiteral("\x1b[1mJo\uFFFD[2Jhn 3:16 (EB)\x1b[0m\n  For God so \uFFFD]0;owned\uFFFD \uFFFD" "2J\uFFFD \x1b[1;4mloved\x1b[0m the world.\n"));
        const auto list = run({"translations"});
        QCOMPARE(controls(list.out), 0); QVERIFY2(list.out.contains(QStringLiteral("Evil\uFFFD[31m Bible")), qPrintable(list.out));
        const auto json = run({"quote", "jn316", "-t", "EB", "--json"}).out;
        QCOMPARE(controls(json), 0); QVERIFY(json.contains("\\u001b]0;owned\\u0007 \\u009b2J\\u007f loved"));
        QCOMPARE(QJsonDocument::fromJson(json.toUtf8()).array().first().toObject()["text"].toString(),
                 QStringLiteral("For God so \x1b]0;owned\x07 \u009b" "2J\x7f loved the world."));
        const auto unknown = run({"quote", "jn316", "-t", "x\x1b[31m"});
        QCOMPARE(unknown.status, 1); QCOMPARE(controls(unknown.err), 0); QVERIFY(unknown.err.contains(QStringLiteral("x\uFFFD[31m")));
        const auto command = run({"nope\x1b[2J"});
        QCOMPARE(command.status, 2); QCOMPARE(controls(command.err), 0); QVERIFY(command.err.contains(QStringLiteral("nope\uFFFD[2J")));
        SingleInstance::Request request; QString error;
        QVERIFY(!CommandLine::request({"--bogus\x1b[2J"}, &request, &error)); QCOMPARE(controls(error), 0);
        QCOMPARE(CommandLine::printable(QStringLiteral("a\tb\nc\rd\u0085e\u00a0f")), QStringLiteral("a\tb\nc\uFFFDd\uFFFDe\u00a0f"));
    }
    // read lays a passage out as the reader does, with your settings for
    // verse numbers, headings, and the words of Jesus in red.
    void readLaysOutAsTheReader() {
        const QString path = fixtures.filePath("eng_rdr_complete.json");
        const auto poem = [](const QString &text, int level) { return QJsonObject{{"text", text}, {"poem", level}}; };
        QVERIFY(AoFixture::write(path, AoFixture::complete("eng_rdr", {
            AoFixture::book("PSA", 1, {AoFixture::chapter(23, {
                QJsonObject{{"type", "heading"}, {"content", QJsonArray{"A song of trust"}}},
                AoFixture::verse(1, {poem("The shepherd keeps me;", 1), poem("I lack nothing.", 2)}),
                AoFixture::verse(2, {poem("He rests me in green fields;", 1), QJsonObject{{"noteId", 0}}, poem("he leads me by still water.", 2)})
            }, {QJsonObject{{"noteId", 0}, {"caller", "+"}, {"text", "Or meadows"}}})}),
            AoFixture::book("JHN", 2, {AoFixture::chapter(3, {
                AoFixture::verse(16, {"For God so loved the world."}),
                AoFixture::verse(17, {"Jesus said,", " ", QJsonObject{{"text", "Truly I tell you."}, {"wordsOfJesus", true}}}),
                QJsonObject{{"type", "line_break"}},
                AoFixture::verse(18, {"A new paragraph."})
            }), AoFixture::chapter(4, {
                QJsonObject{{"type", "heading"}, {"content", QJsonArray{"At the well"}}},
                AoFixture::verse(1, {"Then he left."})
            })})
        }, "Reader fixture")));
        install(path);
        const QString psalm = QStringLiteral("Psalms 23 (eng_rdr)\n\nA song of trust\n\n"
                                             "  \u00b9 The shepherd keeps me;\n    I lack nothing.\n"
                                             "  \u00b2 He rests me in green fields;[1]\n    he leads me by still water.\n");
        auto read = run({"read", "ps23", "-t", "eng_rdr"});
        QCOMPARE(read.status, 0); QCOMPARE(read.out, psalm);
        QCOMPARE(run({"read", "ps23", "-t", "eng_rdr", "--footnotes"}).out, psalm + "\n[1] Psalms 23:2 \u2014 Or meadows\n");
        // Chapters and paragraphs, across a chapter.
        QCOMPARE(run({"read", "John 3:16-4:1", "-t", "eng_rdr"}).out,
                 QStringLiteral("John 3:16\u20134:1 (eng_rdr)\n\n\u00b9\u2076 For God so loved the world. \u00b9\u2077 Jesus said, Truly I tell you.\n\n"
                                "\u00b9\u2078 A new paragraph.\n\nJohn 4\n\nAt the well\n\n\u00b9 Then he left.\n"));
        // Several passages, a blank line apart; standard input as quote reads it.
        QCOMPARE(run({"read", "-", "-t", "eng_rdr"}, "jn316\njn41\n").out,
                 QStringLiteral("John 3:16 (eng_rdr)\n\n\u00b9\u2076 For God so loved the world.\n\n"
                                "John 4:1 (eng_rdr)\n\nAt the well\n\n\u00b9 Then he left.\n"));
        auto partly = run({"read", "jn316", "Nowhere 9", "-t", "eng_rdr"});
        QCOMPARE(partly.status, 1); QVERIFY(partly.out.contains("loved")); QVERIFY(partly.err.contains("Nowhere 9"));
        QCOMPARE(run({"read", "jn316", "--color", "sometimes"}).status, 2);
        // Color: bold titles and headings; the words of Jesus in red where OmaWord shows them so.
        QCOMPARE(run({"read", "jn317", "-t", "eng_rdr", "--color", "always"}).out,
                 QStringLiteral("\x1b[1mJohn 3:17 (eng_rdr)\x1b[22m\n\n\u00b9\u2077 Jesus said, Truly I tell you.\n"));
        {
            BibleBackend settings; settings.setWordsOfJesus(true); settings.setShowVerseNumbers(false); settings.setShowHeadings(false);
        }
        QCOMPARE(run({"read", "jn317", "-t", "eng_rdr", "--color", "always"}).out,
                 QStringLiteral("\x1b[1mJohn 3:17 (eng_rdr)\x1b[22m\n\nJesus said, \x1b[31mTruly I tell you.\x1b[39m\n"));
        QVERIFY(!run({"read", "ps23", "-t", "eng_rdr"}).out.contains("A song of trust"));
        {
            BibleBackend settings; settings.setWordsOfJesus(false); settings.setShowVerseNumbers(true); settings.setShowHeadings(true);
        }
        // Lines wrap to the terminal's width, a poem line's parts indented past it.
        QString out, err, in;
        QTextStream outStream(&out), errStream(&err), inStream(&in);
        CommandLine::Terminal narrow; narrow.columns = 24;
        QCOMPARE(CommandLine::run({"read", "ps23", "-t", "eng_rdr"}, outStream, errStream, inStream, narrow), 0);
        outStream.flush();
        QCOMPARE(out, QStringLiteral("Psalms 23 (eng_rdr)\n\nA song of trust\n\n"
                                     "  \u00b9 The shepherd keeps\n    me;\n    I lack nothing.\n"
                                     "  \u00b2 He rests me in green\n    fields;[1]\n    he leads me by still\n      water.\n"));
        // JSON: each verse with its heading, paragraph, poem levels, footnotes, and words of Jesus.
        const auto json = QJsonDocument::fromJson(run({"read", "ps23", "jn317", "-t", "eng_rdr", "--json"}).out.toUtf8()).array();
        QCOMPARE(json.size(), 2);
        const auto first = json.first().toObject();
        QCOMPARE(first["reference"].toString(), QString("Psalms 23")); QCOMPARE(first["translationId"].toString(), QString("eng_rdr"));
        const auto verse = first["verses"].toArray().at(1).toObject();
        QCOMPARE(verse["text"].toString(), QString("He rests me in green fields;\nhe leads me by still water."));
        QCOMPARE(first["verses"].toArray().at(0).toObject()["heading"].toString(), QString("A song of trust"));
        QVERIFY(first["verses"].toArray().at(0).toObject()["paragraphStart"].toBool()); QVERIFY(!verse["paragraphStart"].toBool());
        QCOMPARE(verse["levels"].toArray(), QJsonArray({1, 2}));
        QCOMPARE(verse["footnotes"].toArray(), QJsonArray({QJsonObject{{"offset", 28}, {"text", "Or meadows"}}}));
        QCOMPARE(json.last().toObject()["verses"].toArray().first().toObject()["wordsOfJesus"].toArray(),
                 QJsonArray({QJsonObject{{"start", 12}, {"end", 29}}}));
        // quote's JSON keeps its verses as they were.
        const auto quoted = QJsonDocument::fromJson(run({"quote", "jn317", "-t", "eng_rdr", "--json"}).out.toUtf8()).array();
        QCOMPARE(quoted.first().toObject()["verses"].toArray().first().toObject().keys(), QStringList({"book", "chapter", "text", "verse"}));
    }
    // quote --all: each passage in every translation, the one you read first.
    void quoteEveryTranslation() {
        const auto every = run({"quote", "jn316", "jn317", "--all"});
        QCOMPARE(every.status, 0); QVERIFY(every.err.isEmpty());
        QVERIFY2(every.out.startsWith(QStringLiteral("\u201c\u00b9\u2076 For God so loved the world.\u201d\n\n\u2014 John 3:16 (eng_rdr)\n\n")), qPrintable(every.out));
        QVERIFY(every.out.contains(QStringLiteral("\u201c\u00b9\u2076 For God so loved the world, that he gave.\u201d\n\n\u2014 John 3:16 (KJV)\n\n")));
        for (const QString abbreviation : {"(eng_rdr)", "(NET)", "(KJV)", "(EB)"}) QCOMPARE(every.out.count("3:16 " + abbreviation), 1);
        QVERIFY(every.out.lastIndexOf("3:16 (") < every.out.indexOf("3:17 ("));
        const auto json = QJsonDocument::fromJson(run({"quote", "jn316", "--all", "--json"}).out.toUtf8()).array();
        QCOMPARE(json.size(), 4); QCOMPARE(json.first().toObject()["translation"].toString(), QString("eng_rdr"));
        // A passage some translation lacks is noted; one none has fails.
        const auto psalm = run({"quote", "ps23:1", "--all"});
        QCOMPARE(psalm.status, 0); QVERIFY(psalm.out.endsWith(QStringLiteral("\u2014 Psalms 23:1 (eng_rdr)\n")));
        QVERIFY2(psalm.err.contains("ps23:1 (NET): ") && psalm.err.contains("ps23:1 (KJV): "), qPrintable(psalm.err));
        QCOMPARE(run({"quote", "Nowhere 9", "--all"}).status, 1);
        QCOMPARE(run({"quote", "jn316", "--all", "-t", "KJV"}).status, 2);
        QCOMPARE(run({"quote", "jn316", "--all", "--plain"}).status, 2);
    }
    // refs gives what See also does: passages tagged alike, then OpenBible.info's.
    void refsFollowSeeAlso() {
        const QString path = fixtures.filePath("study_complete.json");
        QVERIFY(AoFixture::studyEdition(path)); install(path);
        const auto scope = [](int book, int start, int end) {
            return QVariantMap{{"versionId", "study"}, {"book", book}, {"chapter", 1}, {"start", start}, {"endBook", book}, {"endChapter", 1}, {"end", end}};
        };
        {
            BibleBackend bible;
            for (const auto &[at, name] : QList<QPair<QVariantMap, QString>>{{scope(1, 1, 1), "beginning"}, {scope(40, 1, 1), "beginning"},
                                                                              {scope(1, 1, 2), "word"}, {scope(43, 1, 1), "word"}})
                QVERIFY2(!bible.createAndAssignTag(at, name).isEmpty(), qPrintable(bible.error()));
        }
        // Its own tags and those of marks over it; never itself, nor a mark over it.
        const auto found = run({"refs", "gen1:1", "-t", "OSF"});
        QCOMPARE(found.status, 0);
        QVERIFY2(found.out.startsWith(QStringLiteral("Genesis 1:1 (OSF)\n\nTagged alike\nMatthew 1:1 (OSF) \u00b7 beginning\n  Original study text 1:1 ")),
                 qPrintable(found.out));
        QVERIFY(found.out.contains(QStringLiteral("\nJohn 1:1 (OSF) \u00b7 word\n  In the beginning was the Word.")));
        QVERIFY(found.out.contains(QStringLiteral("\n\nOpenBible.info\nJohn 1:1\u20133\n  In the beginning was the Word. Original words follow. ")));
        QVERIFY(!found.out.contains("\nGenesis 1:"));
        const auto quoted = run({"refs", "gen1:1", "-t", "OSF", "--quote"});
        QVERIFY(quoted.out.contains(QStringLiteral("\n\u201c\u00b9 In the beginning was the Word. Original words follow. \u00b2 Original study text 1:2 ")));
        QVERIFY(quoted.out.contains(QStringLiteral("\u201d\n\n\u2014 John 1:1\u20133 (OSF)\n")));
        const auto limited = run({"refs", "gen1:1", "-t", "OSF", "--limit", "1"});
        QCOMPARE(limited.out.count("\n  "), 2); QVERIFY(limited.err.contains("--limit 0"));
        const auto json = QJsonDocument::fromJson(run({"refs", "gen1:1", "-t", "OSF", "--json"}).out.toUtf8()).array().first().toObject();
        QCOMPARE(json["reference"].toString(), QString("Genesis 1:1"));
        const auto tagged = json["tagged"].toArray();
        QCOMPARE(tagged.size(), 2); QCOMPARE(tagged.first().toObject()["tags"].toArray(), QJsonArray({"beginning"}));
        QVERIFY(tagged.first().toObject()["text"].toString().startsWith("Original study text 1:1 "));
        const auto cross = json["crossReferences"].toArray();
        QVERIFY(!cross.isEmpty()); QCOMPARE(cross.first().toObject()["reference"].toString(), QStringLiteral("John 1:1\u20133"));
        QVERIFY(cross.first().toObject()["votes"].toInt() > 0);
        // Verses only; nothing to give fails.
        QCOMPARE(run({"refs", "Genesis 1", "-t", "OSF"}).status, 2);
        const auto nothing = run({"refs", "jn317", "-t", "NET"});
        QCOMPARE(nothing.status, 1); QVERIFY(nothing.out.isEmpty()); QVERIFY(nothing.err.contains("nothing for John 3:17"));
    }
    void booksAndOutlines() {
        QCOMPARE(run({"books", "-t", "OSF"}).out, QString("Genesis  1 chapter\nMatthew  1 chapter\nJohn     3 chapters\n"));
        QCOMPARE(run({"books", "list", "-t", "OSF"}).out, run({"books", "-t", "OSF"}).out);
        const auto books = QJsonDocument::fromJson(run({"books", "--json", "-t", "OSF"}).out.toUtf8()).array();
        QCOMPARE(books.size(), 3);
        QCOMPARE(books.last().toObject()["name"].toString(), QString("John")); QCOMPARE(books.last().toObject()["code"].toString(), QString("JHN"));
        QCOMPARE(books.last().toObject()["chapters"].toInt(), 3);
        QCOMPARE(run({"books", "John"}).status, 2);
        // A book's headings chapter by chapter, or one chapter's (that of verses too).
        const QString heading = QStringLiteral("  1    Study fixture heading\n");
        QCOMPARE(run({"books", "outline", "John", "-t", "OSF"}).out, "John 1\n" + heading + "John 2\n" + heading + "John 3\n" + heading);
        QCOMPARE(run({"books", "outline", "jn2", "-t", "OSF"}).out, "John 2\n" + heading);
        QCOMPARE(run({"books", "outline", "John", "3:16", "-t", "OSF"}).out, "John 3\n" + heading);
        const auto outline = QJsonDocument::fromJson(run({"books", "outline", "jn", "--json", "-t", "OSF"}).out.toUtf8()).array();
        QCOMPARE(outline.size(), 3); QCOMPARE(outline.first().toObject()["title"].toString(), QString("John 1"));
        QCOMPARE(outline.first().toObject()["headings"].toArray(), QJsonArray({QJsonObject{{"verse", 1}, {"text", "Study fixture heading"}}}));
        QCOMPARE(run({"books", "outline", "-t", "OSF"}).status, 2);
        const auto missing = run({"books", "outline", "Tobit", "-t", "OSF"});
        QCOMPARE(missing.status, 1); QVERIFY2(missing.err.contains(QStringLiteral("OSF has no such book: \u201cTobit\u201d")), qPrintable(missing.err));
        QCOMPARE(run({"books", "outline", "Nowhere", "-t", "OSF"}).status, 1);
    }
    void translationDetails() {
        const QString path = fixtures.filePath("eng_rev_complete.json");
        auto root = AoFixture::complete("eng_rev", {AoFixture::book("JHN", 1, {AoFixture::chapter(3, {
            AoFixture::verse(16, {"He said,", QJsonObject{{"noteId", 0}}, " ", QJsonObject{{"text", "Follow me."}, {"wordsOfJesus", true}}})
        }, {QJsonObject{{"noteId", 0}, {"caller", "+"}, {"text", "Or answered"}}})})}, "Revised fixture");
        auto translation = root["translation"].toObject();
        translation["shortName"] = "REV"; translation["sha256"] = "0123abcd"; translation["languageEnglishName"] = "English";
        translation["licenseNotice"] = "Original fixture text, free to quote.";
        root["translation"] = translation;
        QVERIFY(AoFixture::write(path, root)); install(path);
        const QString details = QStringLiteral("Revised fixture (REV, eng_rev)\nEnglish \u00b7 1 book \u00b7 1 verse \u00b7 1 footnote \u00b7 the words of Jesus marked\n"
                                               "AO revision 0123abcd\n\nOriginal fixture text, free to quote.\n");
        QCOMPARE(run({"translations", "show", "rev"}).out, details);
        QCOMPARE(run({"translations", "show"}).out, details); // The one you read: the last installed.
        QCOMPARE(run({"versions", "show", "eng_rev"}).out, details);
        const auto json = QJsonDocument::fromJson(run({"translations", "show", "OSF", "--json"}).out.toUtf8()).object();
        QCOMPARE(json["id"].toString(), QString("study")); QCOMPARE(json["books"].toInt(), 3); QCOMPARE(json["footnotes"].toInt(), 4);
        QCOMPARE(json["wordsOfJesus"].toInt(), 1); QVERIFY(!json["active"].toBool()); QVERIFY(json["revision"].toString().isEmpty());
        const auto unknown = run({"translations", "show", "XYZ"});
        QCOMPARE(unknown.status, 1); QVERIFY(unknown.err.contains("no installed translation") && unknown.err.contains("REV"));
        QCOMPARE(run({"translations", "show", "-t", "REV"}).status, 2);
    }
    // Your marks, the Desk, History, and the ribbon, read as the app saved them.
    void marksAndPlaces() {
        QCOMPARE(run({"marks"}).status, 0); QCOMPARE(run({"marks"}).err, QString("No marks in REV yet.\n"));
        QCOMPARE(run({"desk"}).err, QString("The Desk is empty.\n")); QCOMPARE(run({"history"}).err, QString("History is empty.\n"));
        const auto unplaced = run({"ribbon"});
        QCOMPARE(unplaced.status, 1); QVERIFY(unplaced.err.contains("the ribbon lies nowhere"));
        {
            BibleBackend bible;
            QVERIFY(bible.readVersion("eng_net")); QVERIFY(bible.navigate("John 3:16")); QVERIFY(bible.toggleHighlight());
            const QVariantMap john{{"versionId", "eng_net"}, {"book", 43}, {"chapter", 3}, {"start", 16}, {"endBook", 43}, {"endChapter", 3}, {"end", 16}};
            QVERIFY2(bible.setScopeNote(john, "Love\n\nGiven.", ""), qPrintable(bible.error()));
            QVERIFY(!bible.createAndAssignTag(QVariantMap{{"versionId", "eng_net"}, {"book", 1}, {"chapter", 1}, {"start", 1},
                                                          {"endBook", 1}, {"endChapter", 1}, {"end", 1}}, "creation").isEmpty());
            QVERIFY(bible.setDeskPage("# Study\n\nGrace.", "")); QVERIFY(bible.recordHistory()); QVERIFY(bible.placeRibbon());
        }
        // marks: one line each, in Bible order, in the translation named or read.
        const QString genesis = QStringLiteral("Genesis 1:1 (NET) \u00b7 #creation\n  In the beginning God created.\n");
        const QString john = QStringLiteral("John 3:16 (NET) \u00b7 highlighted\n  Love Given.\n");
        QCOMPARE(run({"marks", "-t", "NET"}).out, genesis + john);
        QCOMPARE(run({"marks", "list", "-t", "NET"}).out, genesis + john);
        QCOMPARE(run({"marks", "-t", "NET", "--notes"}).out, john);
        QCOMPARE(run({"marks", "-t", "NET", "--highlights"}).out, john);
        QCOMPARE(run({"marks", "-t", "NET", "love", "JOHN"}).out, john);
        QCOMPARE(run({"marks", "-t", "NET", "--tag", "cre"}).out, genesis);
        QCOMPARE(run({"marks", "-t", "NET", "#cre"}).out, genesis);
        const auto none = run({"marks", "-t", "NET", "love", "--tag", "cre"});
        QCOMPARE(none.status, 1); QCOMPARE(none.err, QString("omaword marks list: no mark in NET matches.\n"));
        QCOMPARE(run({"marks", "--all", "--tag", "beginning"}).out.count(" (OSF) \u00b7 #beginning\n"), 2);
        QVERIFY(run({"marks", "--all"}).out.contains(john));
        QCOMPARE(run({"marks", "--all", "-t", "NET"}).status, 2);
        const auto json = QJsonDocument::fromJson(run({"marks", "-t", "NET", "--json"}).out.toUtf8()).array();
        QCOMPARE(json.size(), 2);
        const auto mark = json.last().toObject();
        QCOMPARE(mark["reference"].toString(), QString("John 3:16")); QCOMPARE(mark["translationId"].toString(), QString("eng_net"));
        QVERIFY(mark["highlight"].toBool()); QCOMPARE(mark["note"].toString(), QString("Love\n\nGiven."));
        QVERIFY(!mark["created"].toString().isEmpty()); QCOMPARE(json.first().toObject()["tags"].toArray(), QJsonArray({"creation"}));
        // marks show: those on a passage or within it, notes whole.
        const QString whole = QStringLiteral("John 3:16 (NET) \u00b7 highlighted\n  Love\n\n  Given.\n");
        QCOMPARE(run({"marks", "show", "jn316", "-t", "NET"}).out, whole);
        QCOMPARE(run({"marks", "show", "John", "3", "-t", "NET"}).out, whole);
        QVERIFY(run({"marks", "show", "gen1:2", "-t", "OSF"}).out.startsWith(QStringLiteral("Genesis 1:1\u20132 (OSF) \u00b7 #word\n")));
        const auto empty = run({"marks", "show", "jn317", "-t", "NET"});
        QCOMPARE(empty.status, 1); QVERIFY(empty.err.contains("no marks on John 3:17 in NET"));
        QCOMPARE(QJsonDocument::fromJson(run({"marks", "show", "jn316", "-t", "NET", "--json"}).out.toUtf8()).array().size(), 1);
        // tags, by name, with how many passages each marks.
        QCOMPARE(run({"tags"}).out, QString("beginning  2 passages\ncreation   1 passage\nword       2 passages\n"));
        QCOMPARE(QJsonDocument::fromJson(run({"tags", "list", "--json"}).out.toUtf8()).array().first().toObject(),
                 QJsonObject({{"name", "beginning"}, {"count", 2}}));
        QCOMPARE(run({"tags", "grace"}).status, 2);
        // The Desk, History, and the ribbon.
        QCOMPARE(run({"desk"}).out, QString("# Study\n\nGrace.\n"));
        const auto visited = run({"history"});
        QVERIFY2(QRegularExpression("^\\d{4}-\\d\\d-\\d\\d \\d\\d:\\d\\d  John 3:16 \\(NET\\)\n$").match(visited.out).hasMatch(), qPrintable(visited.out));
        QVERIFY(!QJsonDocument::fromJson(run({"history", "--json"}).out.toUtf8()).array().first().toObject()["visitedAt"].toString().isEmpty());
        QCOMPARE(run({"ribbon"}).out, QString("John 3:16 (NET)\n"));
        QCOMPARE(QJsonDocument::fromJson(run({"ribbon", "--json"}).out.toUtf8()).object()["translationId"].toString(), QString("eng_net"));
        { BibleBackend bible; QVERIFY(bible.setHistoryEnabled(false)); }
        QVERIFY(run({"history"}).err.contains("History is off"));
        { BibleBackend bible; QVERIFY(bible.setHistoryEnabled(true)); }
    }
    // Export and Back up as the app writes them, printed or in a file of yours alone.
    void exportsAndBackups() {
        QTemporaryDir directory;
        const auto markdown = run({"marks", "export"});
        QCOMPARE(markdown.status, 0);
        QVERIFY2(markdown.out.startsWith("# " + Brand::name() + " notes\n\n"), qPrintable(markdown.out));
        for (const QString section : {"## Fixture (NET)\n\n", "### John 3:16\n\n*Highlight*\n\nLove\n\nGiven.\n\n", "### Genesis 1:1\n\nTags: creation\n\n"})
            QVERIFY2(markdown.out.contains(section), qPrintable(markdown.out));
        QString live;
        {
            BibleBackend bible;
            QVERIFY(bible.exportNotes(QUrl::fromLocalFile(directory.path())));
            QFile exported(directory.filePath("omaword-notes.md"));
            QVERIFY(exported.open(QIODevice::ReadOnly)); QCOMPARE(QString::fromUtf8(exported.readAll()), markdown.out);
            live = bible.dataDirectory() + "/personal.sqlite";
        }
        const auto privateFile = [](const QString &path) {
            return QFileInfo(path).exists() && !(QFile::permissions(path) & (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther));
        };
        const QString notes = directory.filePath("notes.md");
        const auto written = run({"marks", "export", "-o", notes});
        QCOMPARE(written.status, 0); QVERIFY(written.out.isEmpty()); QVERIFY(privateFile(notes));
        QFile file(notes); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(QString::fromUtf8(file.readAll()), markdown.out);
        const auto onDatabase = run({"marks", "export", "-o", live});
        QCOMPARE(onDatabase.status, 1); QVERIFY2(onDatabase.err.contains("outside the live database"), qPrintable(onDatabase.err));
        QCOMPARE(run({"marks", "export", "notes.md"}).status, 2);
        // The JSON backup Restore reads.
        const QString backup = directory.filePath("backup.json");
        QCOMPARE(run({"marks", "backup", backup}).status, 0); QVERIFY(privateFile(backup));
        QFile saved(backup); QVERIFY(saved.open(QIODevice::ReadOnly));
        const auto root = QJsonDocument::fromJson(saved.readAll()).object();
        QCOMPARE(root["format"].toString(), QString("omaword-backup")); QCOMPARE(root["deskPage"].toString(), QString("# Study\n\nGrace."));
        const auto printed = QJsonDocument::fromJson(run({"marks", "backup", "-"}).out.toUtf8()).object();
        QCOMPARE(printed["versions"], root["versions"]); QCOMPARE(printed["tags"], root["tags"]);
        QCOMPARE(run({"marks", "backup", live}).status, 1);
        QCOMPARE(run({"marks", "backup"}).status, 2); QCOMPARE(run({"marks", "backup", "a.json", "b.json"}).status, 2);
    }
    // AO Lab's catalog as OmaWord came with it, offline, and refreshed.
    void catalogFromAoLab() {
        const auto english = run({"catalog"});
        QCOMPARE(english.status, 0); QCOMPARE(english.out.count('\n'), 51);
        QVERIFY(english.out.contains("(eng_kjv) \u00b7 installed")); QVERIFY(!english.out.contains("(spa_"));
        QVERIFY(run({"catalog", "--language", "spa"}).out.contains("(spa_r09)"));
        QCOMPARE(run({"catalog", "list", "--all-languages"}).out.count('\n'), 1256);
        QVERIFY(run({"catalog", "berean", "STANDARD"}).out.contains("BSB  Berean Standard Bible  (BSB)\n"));
        QCOMPARE(run({"catalog", "nothing like this"}).status, 1);
        QCOMPARE(run({"catalog", "--language", "spa", "--all-languages"}).status, 2);
        QJsonObject kjv;
        for (const auto &value : QJsonDocument::fromJson(run({"catalog", "--json"}).out.toUtf8()).array())
            if (value.toObject()["id"] == "eng_kjv") kjv = value.toObject();
        QVERIFY(kjv["installed"].toBool()); QVERIFY(kjv["installable"].toBool()); QCOMPARE(kjv["abbreviation"].toString(), QString("KJV"));
        // One translation, by ID or abbreviation, as Details & Attribution shows it.
        const auto shown = run({"catalog", "show", "eng_kjv"});
        QVERIFY2(shown.out.startsWith(QStringLiteral("King James (Authorized) Version (KJV, eng_kjv)\nEnglish \u00b7 Book set unverified \u00b7 66 books")),
                 qPrintable(shown.out));
        QVERIFY(shown.out.contains("\nInstalled; AO Lab has another revision.\nPermissions: https://ebible.org/Scriptures/details.php?id=eng-kjv2006\n"));
        QCOMPARE(run({"catalog", "show", "kjv"}).out, shown.out);
        QVERIFY(run({"catalog", "show", "spa_r09"}).out.contains("OmaWord cannot install it: Language support not yet verified"));
        QCOMPARE(run({"catalog", "show", "nope"}).status, 1); QCOMPARE(run({"catalog", "show"}).status, 2);
        // Refreshed from AO Lab, it is the catalog read from then on.
        FakeNetwork network;
        QTemporaryDir data;
        VersionCatalog::Dependencies dependencies;
        dependencies.manager = &network; dependencies.dataDirectory = data.path(); dependencies.cacheDirectory = data.path();
        const auto entry = [](const QString &id, const QString &name, const QString &language) {
            return QJsonObject{{"id", id}, {"name", name}, {"shortName", id.toUpper()}, {"language", language}, {"textDirection", "ltr"},
                               {"numberOfBooks", 66}, {"totalNumberOfChapters", 1189}, {"totalNumberOfVerses", 31102},
                               {"completeTranslationApiLink", "/api/" + id + "/complete.json"}};
        };
        network.bodies["/api/available_translations.json"] = QJsonDocument(QJsonObject{{"translations", QJsonArray{
            entry("eng_new", "New Original Fixture", "eng"), entry("spa_new", "Nueva", "spa")}}}).toJson();
        const auto refreshed = run({"catalog", "refresh"}, {}, false, dependencies);
        QCOMPARE(refreshed.status, 0); QCOMPARE(refreshed.out, QString("AO Lab's catalog lists 2 translations.\n"));
        QCOMPARE(run({"catalog", "--all-languages"}, {}, false, dependencies).out,
                 QStringLiteral("ENG_NEW  New Original Fixture  (eng_new)\nSPA_NEW  Nueva  (spa_new) \u00b7 cannot be installed\n"));
        network.bodies.clear();
        const auto failed = run({"catalog", "refresh"}, {}, false, dependencies);
        QCOMPARE(failed.status, 1); QVERIFY2(failed.err.contains("HTTP 404"), qPrintable(failed.err));
        QVERIFY(run({"catalog"}, {}, false, dependencies).out.contains("(eng_new)"));
    }
    // Installs, removals, and imports change the library as Versions does,
    // the version you read staying the one you read.
    void installRemoveAndImport() {
        FakeNetwork network;
        QTemporaryDir cache;
        VersionCatalog::Dependencies dependencies;
        dependencies.manager = &network; dependencies.cacheDirectory = cache.path(); dependencies.dataDirectory = cache.path();
        auto asv = AoFixture::complete("eng_asv", {AoFixture::book("JHN", 1, {AoFixture::chapter(3, {AoFixture::verse(16, {"God loved the world so."})})})},
                                       "Fixture Standard");
        auto translation = asv["translation"].toObject(); translation["shortName"] = "ASV"; asv["translation"] = translation;
        network.bodies["/api/eng_asv/complete.json"] = QJsonDocument(asv).toJson(QJsonDocument::Compact);
        const auto reading = [] {
            for (const auto &line : run({"translations"}).out.split('\n')) if (line.startsWith("* ")) return line;
            return QString();
        };
        const QString before = reading();
        QVERIFY(!before.isEmpty());
        // From AO Lab's catalog, by abbreviation or ID; again, it replaces the copy, marks kept.
        const auto installed = run({"translations", "install", "asv"}, {}, false, dependencies);
        QCOMPARE(installed.status, 0); QCOMPARE(installed.out, QString("Installed ASV (eng_asv).\n")); QVERIFY(installed.err.isEmpty());
        QCOMPARE(reading(), before);
        QCOMPARE(run({"quote", "jn316", "-t", "ASV", "--plain"}).out, QString("God loved the world so.\n"));
        {
            BibleBackend bible;
            QVERIFY(!bible.createAndAssignTag(QVariantMap{{"versionId", "eng_asv"}, {"book", 43}, {"chapter", 3}, {"start", 16},
                                                          {"endBook", 43}, {"endChapter", 3}, {"end", 16}}, "kept").isEmpty());
        }
        QCOMPARE(run({"translations", "install", "eng_asv"}, {}, false, dependencies).out, QString("Replaced ASV (eng_asv).\n"));
        QVERIFY(run({"marks", "-t", "ASV"}).out.contains("#kept"));
        // Removed, its marks wait for it; with --delete-marks they go too.
        const auto removed = run({"translations", "remove", "asv"});
        QCOMPARE(removed.status, 0); QCOMPARE(removed.out, QString("Removed ASV (eng_asv); your marks in it stay for when you install it again.\n"));
        QVERIFY(!run({"translations", "--names"}).out.contains("ASV"));
        QCOMPARE(run({"translations", "install", "asv"}, {}, false, dependencies).status, 0);
        QVERIFY(run({"marks", "-t", "ASV"}).out.contains("#kept"));
        QCOMPARE(run({"translations", "remove", "ASV", "--delete-marks"}).out, QString("Removed ASV (eng_asv), and your marks in it.\n"));
        QCOMPARE(run({"translations", "install", "asv"}, {}, false, dependencies).status, 0);
        QCOMPARE(run({"marks", "-t", "ASV"}).err, QString("No marks in ASV yet.\n"));
        QCOMPARE(reading(), before);
        // Each that fails says why; the others install.
        const auto failing = run({"translations", "install", "eng_bar", "nope", "spa_r09"}, {}, false, dependencies);
        QCOMPARE(failing.status, 1); QVERIFY(failing.out.isEmpty());
        QVERIFY2(failing.err.contains("eng_bar: Download failed (HTTP 404).") && failing.err.contains(QStringLiteral("no translation \u201cnope\u201d"))
                 && failing.err.contains("spa_r09 cannot be installed"), qPrintable(failing.err));
        QCOMPARE(run({"translations", "install"}).status, 2);
        QCOMPARE(run({"translations", "remove", "nope"}).status, 1);
        // Progress shows on a terminal, its line cleared when done.
        QString out, err, in;
        QTextStream outStream(&out), errStream(&err), inStream(&in);
        CommandLine::Terminal terminal; terminal.errors = true;
        QCOMPARE(CommandLine::run({"translations", "install", "asv"}, outStream, errStream, inStream, terminal, dependencies), 0);
        errStream.flush();
        QVERIFY2(err.contains(QStringLiteral("\rDownloading ASV\u2026 100%")) && err.contains(QStringLiteral("\rInstalling ASV\u2026 "))
                 && err.endsWith("\r\x1b[K"), qPrintable(err));
        // A file, as omaword import (the same) installs it.
        const QString file = fixtures.filePath("eng_imp_complete.json");
        QVERIFY(AoFixture::write(file, AoFixture::complete("eng_imp", {AoFixture::book("JHN", 1, {AoFixture::chapter(3, {
            AoFixture::verse(16, {"Imported words."})})})}, "Imported fixture")));
        QCOMPARE(run({"import", file}).out, QString("Installed eng_imp (eng_imp).\n"));
        QCOMPARE(run({"translations", "import", file}).out, QString("Replaced eng_imp (eng_imp).\n"));
        QCOMPARE(reading(), before);
        const auto missing = run({"import", fixtures.filePath("missing.json")});
        QCOMPARE(missing.status, 1); QVERIFY2(missing.err.contains("Cannot read the selected AO JSON file."), qPrintable(missing.err));
        QCOMPARE(run({"import"}).status, 2);
        QCOMPARE(run({"help", "import"}).out, run({"help", "translations", "import"}).out);
        // With none installed, the first becomes the one you read.
        QTemporaryDir home;
        const QByteArray previous = qgetenv("XDG_DATA_HOME");
        const auto restore = qScopeGuard([&] { qputenv("XDG_DATA_HOME", previous); });
        qputenv("XDG_DATA_HOME", QFile::encodeName(home.path()));
        QCOMPARE(run({"import", file}).status, 0);
        QCOMPARE(run({"translations"}).out, QString("* eng_imp  Imported fixture  (eng_imp)\n"));
    }
    // A verse the translation joined to another is quoted as the verse holding
    // it, which stderr names.
    void joinedVerseNote() {
        const QString retold = fixtures.filePath("eng_ret_complete.json");
        QVERIFY(AoFixture::write(retold, AoFixture::complete("eng_ret", {AoFixture::book("GEN", 1, {AoFixture::chapter(6, {
            AoFixture::verse(17, {"Seventeen."}), AoFixture::verse(14, {"Fourteen, told after seventeen."}), AoFixture::verse(18, {"Eighteen."})})})}, "Retold")));
        install(retold);
        const auto quoted = run({"quote", "Genesis 6:14", "-t", "eng_ret"});
        QCOMPARE(quoted.status, 0); QVERIFY2(quoted.out.endsWith(QStringLiteral("\u2014 Genesis 6:17\n")), qPrintable(quoted.out));
        QCOMPARE(quoted.err, QString("Genesis 6:14 is part of 6:17 in eng_ret.\n"));
        const auto json = QJsonDocument::fromJson(run({"quote", "Genesis 6:14", "-t", "eng_ret", "--json"}).out.toUtf8()).array();
        QCOMPARE(json.first().toObject()["note"].toString(), QString("Genesis 6:14 is part of 6:17 in eng_ret"));
        QVERIFY(!QJsonDocument::fromJson(run({"quote", "Genesis 6:17", "-t", "eng_ret", "--json"}).out.toUtf8()).array().first().toObject().contains("note"));
        QVERIFY(run({"quote", "Genesis 6:17", "-t", "eng_ret"}).err.isEmpty());
    }
    void completionScripts() {
        QVERIFY(run({"completion", "bash"}).out.contains("complete -F _omaword omaword"));
        QVERIFY(run({"completion", "zsh"}).out.startsWith("#compdef omaword"));
        QVERIFY(run({"completion", "fish"}).out.contains("complete -c omaword"));
        QCOMPARE(run({"completion", "tcsh"}).status, 2);
    }
    void requestsForTheRunningApp() {
        QVERIFY(!CommandLine::headless({})); QVERIFY(!CommandLine::headless({"jn316"})); QVERIFY(!CommandLine::headless({"open", "jn316"}));
        QVERIFY(CommandLine::headless({"quote", "jn316"})); QVERIFY(CommandLine::headless({"open", "--help"})); QVERIFY(CommandLine::headless({"-v"}));
        QVERIFY(CommandLine::headless({"import", "bible.json"})); QVERIFY(CommandLine::headless({"marks"}));
        SingleInstance::Request request; QString error;
        QVERIFY(CommandLine::request({"open", "--here", "-t", "KJV", "John", "3:16"}, &request, &error));
        QCOMPARE(request.reference, QString("John 3:16")); QVERIFY(!request.newWindow); QCOMPARE(request.translation, QString("KJV"));
        QVERIFY(CommandLine::request({"jn316"}, &request, &error)); QCOMPARE(request.reference, QString("jn316")); QVERIFY(request.newWindow);
        QVERIFY(CommandLine::request({}, &request, &error)); QVERIFY(request.reference.isEmpty());
        QVERIFY(CommandLine::request({"lookup", "rom", "8"}, &request, &error));
        QVERIFY(request.lookup); QCOMPARE(request.reference, QString("rom 8")); QVERIFY(!request.newWindow);
        QVERIFY(CommandLine::request({"omaword:John%203:16?translation=KJV"}, &request, &error));
        QCOMPARE(request.reference, QString("John 3:16")); QCOMPARE(request.translation, QString("KJV")); QVERIFY(!request.newWindow);
        QVERIFY(CommandLine::request({"--import", "bible.zip"}, &request, &error)); QVERIFY(request.importPath.endsWith("/bible.zip"));
        QVERIFY(!CommandLine::request({"open"}, &request, &error)); QVERIFY(error.contains("name a passage"));
        QVERIFY(!CommandLine::request({"qoute", "something"}, &request, &error)); QVERIFY(error.contains(QStringLiteral("Did you mean \u201cquote\u201d")));
        QVERIFY(!CommandLine::request({"--bogus"}, &request, &error)); QVERIFY(error.contains("bogus"));
        QVERIFY(!CommandLine::request({"jn316", "--here"}, &request, &error));
        QVERIFY(CommandLine::request({"lookup", "-t", "KJV"}, &request, &error)); QVERIFY(request.lookup); QCOMPARE(request.translation, QString("KJV"));
        // Links.
        QString reference, translation;
        QVERIFY(CommandLine::parseLink("omaword://John%203:16/", &reference, &translation)); QCOMPARE(reference, QString("John 3:16")); QVERIFY(translation.isEmpty());
        QVERIFY(CommandLine::parseLink("OmaWord:jn316?t=kjv", &reference, &translation)); QCOMPARE(reference, QString("jn316")); QCOMPARE(translation, QString("kjv"));
        QVERIFY(!CommandLine::parseLink("https://example.org", &reference, &translation));
        // A link naming no passage brings the reader you were using forward,
        // switched to the translation it names: never a new window.
        QVERIFY(CommandLine::parseLink("omaword:", &reference, &translation)); QVERIFY(reference.isEmpty());
        QVERIFY(CommandLine::request({"omaword:"}, &request, &error));
        QVERIFY(request.reference.isEmpty()); QVERIFY(request.translation.isEmpty()); QVERIFY(!request.newWindow);
        QVERIFY(CommandLine::request({"omaword://?translation=KJV"}, &request, &error));
        QVERIFY(request.reference.isEmpty()); QCOMPARE(request.translation, QString("KJV")); QVERIFY(!request.newWindow);
        QCOMPARE(CommandLine::link(QStringLiteral("Genesis 1:1\u20132:3"), "KJV"), QString("omaword:Genesis%201:1-2:3?translation=KJV"));
    }
};
QTEST_GUILESS_MAIN(CliTest)
#include "tst_cli.moc"
