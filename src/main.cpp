#include "biblebackend.h"
#include "brand.h"
#include "desktop.h"
#include "singleinstance.h"
#include "windowmanager.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QUrl>
#include <QQuickStyle>
#include <QIcon>
#include <QDir>
#include <QStandardPaths>
#include <QMessageBox>
#include <QTimer>
#include <QTextStream>

// `omaword quote <reference>`: the passage from the version being read, as a
// quotation (or --plain text), on stdout. No display, window, or running
// instance is involved, and the reading place stays where it was.
static int quote(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Prints a passage from the version you read, as a quotation."));
    parser.addHelpOption();
    const QCommandLineOption plain(QStringLiteral("plain"), QStringLiteral("Prints the text alone: no quotation marks, verse numbers, or reference."));
    parser.addOption(plain);
    parser.addPositionalArgument(QStringLiteral("quote"), QStringLiteral("Prints a passage."), QStringLiteral("quote"));
    parser.addPositionalArgument(QStringLiteral("reference"), QStringLiteral("Passage, e.g. \"John 3:16\" or jn316."), QStringLiteral("<reference>"));
    parser.process(app);
    auto words = parser.positionalArguments();
    words.removeFirst();
    QTextStream err(stderr);
    if (words.isEmpty()) { err << Brand::name() << ": name a passage, e.g. " << Brand::storageId() << " quote jn316" << Qt::endl; return 2; }
    BibleBackend bible;
    if (!bible.ready()) {
        err << Brand::name() << ": " << (bible.error().isEmpty() ? QStringLiteral("add a Bible version first.") : bible.error()) << Qt::endl;
        return 1;
    }
    const QString text = bible.quote(words.join(QChar(' ')), parser.isSet(plain));
    if (text.isEmpty()) { err << Brand::name() << ": " << bible.error() << Qt::endl; return 1; }
    QTextStream(stdout) << text << Qt::endl;
    return 0;
}

// An omaword: link's passage ("omaword:John%203:16", "omaword://jn316/"), or "".
static QString linkedReference(const QString &argument)
{
    const QString scheme = Brand::storageId() + QChar(':');
    if (!argument.startsWith(scheme, Qt::CaseInsensitive)) return {};
    QString rest = argument.mid(scheme.size());
    while (rest.startsWith(QChar('/'))) rest.remove(0, 1);
    while (rest.endsWith(QChar('/'))) rest.chop(1);
    return QUrl::fromPercentEncoding(rest.replace(QChar('+'), QChar(' ')).toUtf8()).simplified();
}

int main(int argc, char *argv[]) {
    // Establish the storage identity before resolving application paths.
    QCoreApplication::setApplicationName(Brand::storageId());
    QCoreApplication::setApplicationVersion(Brand::version());
    if (argc > 1 && QByteArray(argv[1]) == "quote") return quote(argc, argv);
    QGuiApplication::setDesktopFileName(Brand::appId());
    QApplication app(argc, argv);
    QGuiApplication::setApplicationDisplayName(Brand::name());
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/branding/icon.svg")));
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QCommandLineParser parser;
    parser.setApplicationDescription(Brand::name() + QStringLiteral(" — a quiet, offline Bible reader for Omarchy."));
    parser.addHelpOption();
    // Display the public name rather than the lowercase storage identity.
    const QCommandLineOption versionOption({QStringLiteral("v"), QStringLiteral("version")}, QStringLiteral("Displays version information."));
    parser.addOption(versionOption);
    QCommandLineOption importOption(QStringLiteral("import"), QStringLiteral("Import a USFM ZIP archive or local USFM file."), QStringLiteral("file"));
    parser.addOption(importOption);
    const QCommandLineOption hereOption(QStringLiteral("here"), QStringLiteral("With open: in the window you were reading, not a new one."));
    parser.addOption(hereOption);
    parser.addPositionalArgument(QStringLiteral("reference"), QStringLiteral("Passage to open in a new window, e.g. \"John 3:16\", or an ")
        + Brand::storageId() + QStringLiteral(": link."), QStringLiteral("[reference]"));
    parser.addPositionalArgument(QStringLiteral("commands"), QStringLiteral("open <reference> [--here]: opens a passage. lookup: the passage palette, "
        "in the window you were reading. quote <reference> [--plain]: prints a passage."), QStringLiteral("[open|lookup|quote]"));
    parser.process(app);
    if (parser.isSet(versionOption)) {
        QTextStream(stdout) << Brand::name() << ' ' << Brand::version() << Qt::endl;
        return 0;
    }

    const QString data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!QDir().mkpath(data)) {
        QMessageBox::critical(nullptr, Brand::name(), QStringLiteral("Cannot create the application data folder: ") + data);
        return 1;
    }
    // `open <reference> [--here]`, `lookup`, an omaword: link (opened where
    // you were reading), or a bare reference (a new window).
    QStringList words = parser.positionalArguments();
    const QString command = words.isEmpty() ? QString() : words.first();
    bool here = false, lookup = false;
    if (command == QStringLiteral("open")) { words.removeFirst(); here = parser.isSet(hereOption); }
    else if (command == QStringLiteral("lookup") && words.size() == 1) { words.clear(); lookup = true; }
    else if (words.size() == 1 && !linkedReference(words.first()).isEmpty()) { words = {linkedReference(words.first())}; here = true; }
    auto initialRequest = SingleInstance::Request::fromArguments(words, parser.value(importOption), parser.isSet(importOption));
    initialRequest.newWindow = !here && !lookup;
    initialRequest.lookup = lookup;
    SingleInstance instance(data, Brand::storageId(), QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation));
    const auto role = instance.start();
    if (role == SingleInstance::Role::Error) {
        QMessageBox::critical(nullptr, Brand::name(), instance.errorString());
        return 1;
    }
    if (role == SingleInstance::Role::Secondary) {
        // Do not construct a backend or replay the primary's original CLI import.
        QTimer::singleShot(0, &instance, [&] {
            instance.forward(initialRequest, [&](SingleInstance::Result result) {
                if (!result.ok) QTextStream(stderr) << Brand::name() << ": " << result.error << Qt::endl;
                app.exit(result.ok ? 0 : 1);
            });
        });
        return app.exec();
    }
    Desktop desktop;
    WindowManager windows(&desktop);
    QTimer::singleShot(0, &instance, [&] {
        windows.openRequest(initialRequest, [&](SingleInstance::Result result) {
            if (!result.ok) QTextStream(stderr) << Brand::name() << ": " << result.error << Qt::endl;
            if (!windows.count()) app.exit(1);
        }, false);
        instance.setHandler([&](const SingleInstance::Request &request, SingleInstance::Reply reply) {
            windows.openRequest(request, std::move(reply));
        });
    });
    return app.exec();
}
