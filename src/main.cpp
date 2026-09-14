#include "brand.h"
#include "desktop.h"
#include "singleinstance.h"
#include "windowmanager.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QQuickStyle>
#include <QIcon>
#include <QDir>
#include <QStandardPaths>
#include <QMessageBox>
#include <QTimer>
#include <QTextStream>

int main(int argc, char *argv[]) {
    // Establish the storage identity before resolving application paths.
    QCoreApplication::setApplicationName(Brand::storageId());
    QCoreApplication::setApplicationVersion(Brand::version());
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
    parser.addPositionalArgument(QStringLiteral("reference"), QStringLiteral("Passage to open, e.g. \"John 3:16\"."), QStringLiteral("[reference]"));
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
    const auto initialRequest = SingleInstance::Request::fromArguments(
        parser.positionalArguments(), parser.value(importOption), parser.isSet(importOption));
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
