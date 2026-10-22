#include "brand.h"
#include "commandline.h"
#include "desktop.h"
#include "executablewatch.h"
#include "singleinstance.h"
#include "windowmanager.h"
#include <QApplication>
#include <QClipboard>
#include <QQuickStyle>
#include <QIcon>
#include <QDir>
#include <QStandardPaths>
#include <QMessageBox>
#include <QTimer>
#include <QTextStream>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    // Establish the storage identity before resolving application paths.
    QCoreApplication::setApplicationName(Brand::storageId());
    QCoreApplication::setApplicationVersion(Brand::version());
    QStringList arguments;
    for (int i = 1; i < argc; ++i) arguments << QString::fromLocal8Bit(argv[i]);
    // Commands that print (quote, search, help, …) need no display or window.
    if (CommandLine::headless(arguments)) {
        QCoreApplication app(argc, argv);
        QTextStream out(stdout), err(stderr), in(stdin);
        CommandLine::Terminal terminal;
        terminal.output = isatty(STDOUT_FILENO);
        terminal.input = isatty(STDIN_FILENO);
        terminal.color = terminal.output && qEnvironmentVariableIsEmpty("NO_COLOR") && qgetenv("TERM") != "dumb";
        // As man does, lines fit the terminal even when piped into a pager.
        for (const int descriptor : {STDOUT_FILENO, STDERR_FILENO}) {
            winsize size{};
            if (isatty(descriptor) && ioctl(descriptor, TIOCGWINSZ, &size) == 0 && size.ws_col) { terminal.columns = size.ws_col; break; }
        }
        return CommandLine::run(arguments, out, err, in, terminal);
    }
    QGuiApplication::setDesktopFileName(Brand::appId());
    QApplication app(argc, argv);
    QGuiApplication::setApplicationDisplayName(Brand::name());
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/branding/icon.svg")));
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    // What to open: Qt's own options (-platform, …) are already gone from app.arguments().
    SingleInstance::Request initialRequest;
    QString error;
    if (!CommandLine::request(app.arguments().mid(1), &initialRequest, &error)) {
        QTextStream(stderr) << error << Qt::endl;
        return CommandLine::Usage;
    }
    const QString data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!QDir().mkpath(data)) {
        QMessageBox::critical(nullptr, Brand::name(), QStringLiteral("Cannot create the application data folder: ") + data);
        return 1;
    }
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
                if (!result.ok) QTextStream(stderr) << Brand::name() << ": " << CommandLine::printable(result.error) << Qt::endl;
                app.exit(result.ok ? 0 : 1);
            });
        });
        return app.exec();
    }
    Desktop desktop;
    WindowManager windows(&desktop);
    // A Wayland clipboard lives in the process that copied: after its last
    // window closes, OmaWord stays (windowless) while what you copied is
    // still on the clipboard, so it can be pasted, and quits once something
    // else is copied (or a window opens and closes again without a copy).
    // Once an install has replaced its executable it lingers no more: the
    // next launch would reach it and open the old build's window.
    app.setQuitOnLastWindowClosed(false);
    auto *clipboard = QGuiApplication::clipboard();
    ExecutableWatch executable(QCoreApplication::applicationFilePath());
    const auto quitWhenDone = [&app, &windows, &executable, clipboard] {
        if (!windows.hasWindows() && (executable.replaced() || !clipboard->ownsClipboard())) app.quit();
    };
    QObject::connect(&windows, &WindowManager::allClosed, &app, quitWhenDone);
    QObject::connect(clipboard, &QClipboard::changed, &app, quitWhenDone);
    QObject::connect(&executable, &ExecutableWatch::changed, &app, quitWhenDone);
    QTimer::singleShot(0, &instance, [&] {
        windows.openRequest(initialRequest, [&](SingleInstance::Result result) {
            if (!result.ok) QTextStream(stderr) << Brand::name() << ": " << CommandLine::printable(result.error) << Qt::endl;
            if (!windows.hasWindows()) app.exit(1);
        }, false);
        instance.setHandler([&](const SingleInstance::Request &request, SingleInstance::Reply reply) {
            windows.openRequest(request, std::move(reply));
        });
    });
    return app.exec();
}
