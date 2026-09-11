#include "biblebackend.h"
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

int main(int argc, char *argv[]) {
    QCoreApplication::setApplicationName(QStringLiteral("omabible"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QGuiApplication::setDesktopFileName(QStringLiteral("omabible"));
    QApplication app(argc, argv);
    QGuiApplication::setApplicationDisplayName(QStringLiteral("OmaBible"));
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/assets/omabible.svg")));
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("A quiet, offline Bible reader for Omarchy."));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption importOption(QStringLiteral("import"), QStringLiteral("Import an official NET Bible 2.1 EPUB."), QStringLiteral("epub"));
    parser.addOption(importOption);
    parser.addPositionalArgument(QStringLiteral("reference"), QStringLiteral("Passage to open, e.g. \"John 3:16\"."), QStringLiteral("[reference]"));
    parser.process(app);

    const QString data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!QDir().mkpath(data)) {
        QMessageBox::critical(nullptr, QStringLiteral("OmaBible"), QStringLiteral("Cannot create the application data folder: ") + data);
        return 1;
    }
    QLockFile lock(data + QStringLiteral("/omabible.lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock()) {
        QMessageBox::information(nullptr, QStringLiteral("OmaBible"), QStringLiteral("OmaBible is already open, or its data folder is locked. Switch to the existing window to continue reading."));
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
