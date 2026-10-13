#include "testenvironment.h"
#include "biblebackend.h"
#include "brand.h"
#include "commandline.h"
#include <QtTest/QTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <archive.h>
#include <archive_entry.h>

// The command line in process: the commands that print (CommandLine::run)
// and the requests the running OmaWord takes (CommandLine::request).
class CliTest : public QObject
{
    Q_OBJECT
    QTemporaryDir fixtures;
    QString net, kjv;
    struct Output { int status; QString out, err; };
    static bool archiveFile(const QString &path, const QByteArray &id, const QByteArray &john, const QByteArray &genesis) {
        archive *a = archive_write_new();
        const auto cleanup = qScopeGuard([&] { archive_write_free(a); });
        if (archive_write_set_format_zip(a) != ARCHIVE_OK || archive_write_open_filename(a, QFile::encodeName(path).constData()) != ARCHIVE_OK) return false;
        const QList<QPair<QByteArray, QByteArray>> files{{"01-GEN" + id + ".usfm", "\\id GEN Fixture\n\\h Genesis\n\\c 1\n\\p\n\\v 1 " + genesis + "\n\\v 2 The earth was empty.\n"},
            {"44-JHN" + id + ".usfm", "\\id JHN Fixture\n\\h John\n\\c 3\n\\p\n\\v 16 " + john + "\n\\v 17 He came to save the world.\n"}};
        for (const auto &[name, body] : files) {
            archive_entry *entry = archive_entry_new();
            archive_entry_set_pathname(entry, name.constData()); archive_entry_set_size(entry, body.size());
            archive_entry_set_filetype(entry, AE_IFREG); archive_entry_set_perm(entry, 0600);
            const bool ok = archive_write_header(a, entry) == ARCHIVE_OK && archive_write_data(a, body.constData(), body.size()) == body.size();
            archive_entry_free(entry);
            if (!ok) return false;
        }
        return archive_write_close(a) == ARCHIVE_OK;
    }
    static void install(const QString &path) {
        BibleBackend bible; bible.importUsfm(QUrl::fromLocalFile(path));
        QTRY_VERIFY_WITH_TIMEOUT(!bible.busy(), 30000);
        QVERIFY2(bible.ready() && bible.error().isEmpty(), qPrintable(bible.error()));
    }
    static Output run(const QStringList &arguments, const QString &input = {}, bool terminal = false) {
        QString out, err, in = input;
        QTextStream outStream(&out), errStream(&err), inStream(&in);
        CommandLine::Terminal tty; tty.input = input.isEmpty() && terminal; tty.output = terminal;
        const int status = CommandLine::run(arguments, outStream, errStream, inStream, tty);
        outStream.flush(); errStream.flush();
        return {status, out, err};
    }
private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("OmaWordTests");
        QCoreApplication::setApplicationName("omaword-cli-test");
        net = fixtures.filePath("engnet_usfm.zip"); kjv = fixtures.filePath("eng-kjv2006_usfm.zip");
        QVERIFY(archiveFile(net, "engnet", "For God so loved the world.", "In the beginning God created."));
        QVERIFY(archiveFile(kjv, "eng-kjv2006", "For God so loved the world, that he gave.", "In the beginning God created the heaven."));
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
        // Every line fits 80 columns.
        for (const auto &text : {help.out, quote.out, run({"help", "search"}).out})
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
        QCOMPARE(run({"quote", "jn316", "--translation=ENG-KJV2006", "--plain"}).out, QString("For God so loved the world, that he gave.\n"));
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
        const auto matches = QJsonDocument::fromJson(run({"search", "loved", "--json", "-t", "kjv"}).out.toUtf8()).array();
        QCOMPARE(matches.size(), 1); QCOMPARE(matches.first().toObject()["translation"].toString(), QString("KJV"));
        QCOMPARE(matches.first().toObject()["matches"].toArray().size(), 1);
        // link.
        QCOMPARE(run({"link", "jn316"}).out, QString("omaword:John%203:16\n"));
        QCOMPARE(run({"link", "John 3:16-17", "-t", "kjv"}).out, QString("omaword:John%203:16-17?translation=KJV\n"));
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
        SingleInstance::Request request; QString error;
        QVERIFY(CommandLine::request({"open", "--here", "-t", "KJV", "John", "3:16"}, &request, &error));
        QCOMPARE(request.reference, QString("John 3:16")); QVERIFY(!request.newWindow); QCOMPARE(request.translation, QString("KJV"));
        QVERIFY(CommandLine::request({"jn316"}, &request, &error)); QCOMPARE(request.reference, QString("jn316")); QVERIFY(request.newWindow);
        QVERIFY(CommandLine::request({}, &request, &error)); QVERIFY(request.reference.isEmpty());
        QVERIFY(CommandLine::request({"lookup", "rom", "8"}, &request, &error));
        QVERIFY(request.lookup); QCOMPARE(request.reference, QString("rom 8")); QVERIFY(!request.newWindow);
        QVERIFY(CommandLine::request({"omaword:John%203:16?translation=KJV"}, &request, &error));
        QCOMPARE(request.reference, QString("John 3:16")); QCOMPARE(request.translation, QString("KJV")); QVERIFY(!request.newWindow);
        QVERIFY(CommandLine::request({"import", "bible.zip"}, &request, &error)); QVERIFY(QFileInfo(request.importPath).isAbsolute());
        QVERIFY(CommandLine::request({"--import", "bible.zip"}, &request, &error)); QVERIFY(request.importPath.endsWith("/bible.zip"));
        QVERIFY(!CommandLine::request({"open"}, &request, &error)); QVERIFY(error.contains("name a passage"));
        QVERIFY(!CommandLine::request({"qoute", "something"}, &request, &error)); QVERIFY(error.contains(QStringLiteral("Did you mean \u201cquote\u201d")));
        QVERIFY(!CommandLine::request({"--bogus"}, &request, &error)); QVERIFY(error.contains("bogus"));
        QVERIFY(!CommandLine::request({"jn316", "--here"}, &request, &error));
        QVERIFY(CommandLine::request({"lookup", "-t", "KJV"}, &request, &error)); QVERIFY(request.lookup); QCOMPARE(request.translation, QString("KJV"));
        QVERIFY(!CommandLine::request({"import", "bible.zip", "-t", "KJV"}, &request, &error)); QVERIFY(error.contains("--translation goes with a passage"));
        // Links.
        QString reference, translation;
        QVERIFY(CommandLine::parseLink("omaword://John%203:16/", &reference, &translation)); QCOMPARE(reference, QString("John 3:16")); QVERIFY(translation.isEmpty());
        QVERIFY(CommandLine::parseLink("OmaWord:jn316?t=kjv", &reference, &translation)); QCOMPARE(reference, QString("jn316")); QCOMPARE(translation, QString("kjv"));
        QVERIFY(!CommandLine::parseLink("https://example.org", &reference, &translation));
        QVERIFY(!CommandLine::parseLink("omaword:", &reference, &translation));
        QCOMPARE(CommandLine::link(QStringLiteral("Genesis 1:1\u20132:3"), "KJV"), QString("omaword:Genesis%201:1-2:3?translation=KJV"));
    }
};
QTEST_GUILESS_MAIN(CliTest)
#include "tst_cli.moc"
