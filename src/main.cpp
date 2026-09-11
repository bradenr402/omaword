#include "biblebackend.h"
#include "brand.h"
#include "desktop.h"
#include "singleinstance.h"
#include "storagemigration.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QIcon>
#include <QDir>
#include <QStandardPaths>
#include <QMessageBox>
#include <QTimer>
#include <QTextStream>
#include <QWindow>
#include <memory>

int main(int argc, char *argv[]) {
    // Use the new identity from the outset; migrate only as the primary instance.
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
    QCommandLineOption importOption(QStringLiteral("import"), QStringLiteral("Import a supported structured Bible EPUB (NET 2.1 or Haiola/eBible KJV 2006)."), QStringLiteral("epub"));
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
    // Keep the historical lock alive until after the backend is destroyed, so
    // an old executable cannot recreate a second writable store during this run.
    StorageMigration migration(data, QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation));
    if (!migration.run()) {
        QTextStream(stderr) << Brand::name() << ": " << migration.errorString() << Qt::endl;
        QMessageBox::critical(nullptr, Brand::name(), migration.errorString());
        return 1;
    }
    Desktop desktop;
    BibleBackend bible;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("desktop"), &desktop);
    engine.rootContext()->setContextProperty(QStringLiteral("bible"), &bible);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) return 1;
    QObject *root = engine.rootObjects().first();
    auto flushNote = [root] {
        QVariant saved;
        return QMetaObject::invokeMethod(root, "flushNote", Q_RETURN_ARG(QVariant, saved)) && saved.toBool();
    };
    auto applyRequest = [root, &bible, flushNote](const SingleInstance::Request &request) -> SingleInstance::Result {
        if (request.importPath.isEmpty() && request.reference.isEmpty()) return {true, {}};
        if (bible.busy()) return {false, QStringLiteral("An EPUB import is already running. Wait for it to finish before opening another reference or import.")};
        if (!flushNote()) return {false, QStringLiteral("The unsaved note could not be saved. The requested reference or import was not opened.")};
        QMetaObject::invokeMethod(root, "savePosition");
        if (!request.importPath.isEmpty()) {
            bible.importEpub(QUrl::fromLocalFile(request.importPath));
            if (!bible.busy()) return {false, bible.error().isEmpty() ? QStringLiteral("The EPUB import could not be started.") : bible.error()};
            if (!request.reference.isEmpty()) {
                // One import, one follow-up. readyChanged also fires on later
                // version switches, so it must not retain a CLI reference.
                auto connection = std::make_shared<QMetaObject::Connection>();
                *connection = QObject::connect(&bible, &BibleBackend::busyChanged, &bible,
                    [&bible, root, flushNote, reference = request.reference, connection] {
                        if (bible.busy()) return;
                        QObject::disconnect(*connection);
                        if (bible.ready() && bible.error().isEmpty() && flushNote()) {
                            QMetaObject::invokeMethod(root, "savePosition");
                            bible.navigate(reference);
                        }
                    });
            }
            return {true, {}}; // Accepted, not a claim that the import has finished.
        }
        if (!bible.navigate(request.reference)) return {false, bible.error()};
        return {true, {}};
    };
    QTimer::singleShot(0, &instance, [&, root, applyRequest] {
        const auto result = applyRequest(initialRequest);
        if (!result.ok) QTextStream(stderr) << Brand::name() << ": " << result.error << Qt::endl;
        // Startup requests wait behind the initial CLI action, including its
        // synchronous import inspection and transition to busy.
        instance.setHandler([&, root, applyRequest](const SingleInstance::Request &request, SingleInstance::Reply reply) {
            const auto action = applyRequest(request);
            SingleInstance::activateWindow(qobject_cast<QWindow *>(root), request, &instance,
                [action, reply](SingleInstance::Result activation) {
                    if (!action.ok) reply(action);
                    else if (!activation.ok) reply({false, QStringLiteral("Request accepted, but window activation failed: ") + activation.error});
                    else reply({true, {}});
                });
        });
    });
    return app.exec();
}
