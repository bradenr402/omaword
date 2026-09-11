#include "biblebackend.h"
#include "brand.h"
#include "desktop.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QIcon>
#include <QLockFile>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QMessageBox>
#include <QTimer>
#include <QTextStream>

int main(int argc, char *argv[]) {
    // Keep QStandardPaths (and the instance lock) compatible with existing data.
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
    // Qt's automatic version output uses the intentionally legacy storage name.
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
    QLockFile lock(data + '/' + Brand::storageId() + QStringLiteral(".lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock()) {
        QMessageBox::information(nullptr, Brand::name(), Brand::name() + QStringLiteral(" is already open, or its data folder is locked. Switch to the existing window to continue reading."));
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
    const QString reference = parser.positionalArguments().join(QChar(' '));
    if (parser.isSet(importOption)) {
        if (!reference.isEmpty()) QObject::connect(&bible, &BibleBackend::readyChanged, &bible, [&bible, reference] { if (bible.ready()) bible.navigate(reference); });
        QTimer::singleShot(0, &bible, [&bible, &parser, importOption] { bible.importEpub(QUrl::fromLocalFile(QFileInfo(parser.value(importOption)).absoluteFilePath())); });
    } else if (!reference.isEmpty()) {
        QTimer::singleShot(0, &bible, [&bible, reference] { bible.navigate(reference); });
    }
    return app.exec();
}
