#include "testenvironment.h"
#include <QTest>
#include "desktop.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlProperty>
#include <QQmlPropertyMap>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <memory>
#include <cstdio>
#include <fontconfig/fontconfig.h>

// The test executable doubles as hyprctl: no compositor or shell required.
static int fakeHyprctl(QCoreApplication &app) {
    if (app.arguments().mid(1) != QStringList{"getoption", "decoration:rounding", "-j"}) return 99;
    QFile log(qEnvironmentVariable("DESKTOP_TEST_LOG"));
    if (!log.open(QIODevice::WriteOnly | QIODevice::Append)) return 98;
    log.write("query\n");
    log.close();
    const int delay = qEnvironmentVariableIntValue("DESKTOP_TEST_DELAY");
    QTimer::singleShot(delay, &app, [&app] {
        QFile response(qEnvironmentVariable("DESKTOP_TEST_RESPONSE"));
        if (!response.open(QIODevice::ReadOnly)) { app.exit(97); return; }
        const QByteArray bytes = response.readAll();
        std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
        std::fflush(stdout);
        app.exit(qEnvironmentVariableIntValue("DESKTOP_TEST_EXIT"));
    });
    return app.exec();
}

class DesktopTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    QMap<QByteArray, QByteArray> m_environment;

    QString command() const { return m_dir.filePath("hyprctl"); }
    void response(const QByteArray &bytes) {
        QFile file(m_dir.filePath("response"));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(bytes), bytes.size());
    }
    int queries() const {
        QFile file(m_dir.filePath("queries"));
        if (!file.open(QIODevice::ReadOnly)) return 0;
        return file.readAll().count('\n');
    }
    void query(Desktop &desktop) {
        QVERIFY(QMetaObject::invokeMethod(&desktop, "refreshCornerRadius", Qt::DirectConnection));
    }
    // The family fontconfig picks for a family name.
    static QString resolveFamily(const QString &family) {
        QString result;
        FcPattern *pattern = FcNameParse(reinterpret_cast<const FcChar8 *>(family.toUtf8().constData()));
        if (!pattern) return result;
        FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
        FcDefaultSubstitute(pattern);
        FcResult outcome;
        FcPattern *match = FcFontMatch(nullptr, pattern, &outcome);
        FcChar8 *name = nullptr;
        if (match && FcPatternGetString(match, FC_FAMILY, 0, &name) == FcResultMatch)
            result = QString::fromUtf8(reinterpret_cast<const char *>(name));
        if (match) FcPatternDestroy(match);
        FcPatternDestroy(pattern);
        return result;
    }

private slots:
    void initTestCase() {
        QVERIFY(m_dir.isValid());
        for (const auto &name : {"HYPRLAND_INSTANCE_SIGNATURE", "XDG_STATE_HOME", "XDG_RUNTIME_DIR",
                                "DESKTOP_TEST_LOG", "DESKTOP_TEST_RESPONSE",
                                "DESKTOP_TEST_DELAY", "DESKTOP_TEST_EXIT"})
            m_environment.insert(name, qgetenv(name));
        QVERIFY(QFile::link(QCoreApplication::applicationFilePath(), command()));
        qputenv("XDG_STATE_HOME", m_dir.path().toUtf8());
        // A short runtime path: Unix socket paths are limited to 107 bytes.
        qputenv("XDG_RUNTIME_DIR", m_dir.path().toUtf8());
        qputenv("DESKTOP_TEST_LOG", m_dir.filePath("queries").toUtf8());
        qputenv("DESKTOP_TEST_RESPONSE", m_dir.filePath("response").toUtf8());
    }
    void cleanupTestCase() {
        for (auto it = m_environment.cbegin(); it != m_environment.cend(); ++it) {
            if (it.value().isNull()) qunsetenv(it.key().constData());
            else qputenv(it.key().constData(), it.value());
        }
    }
    void init() {
        qputenv("HYPRLAND_INSTANCE_SIGNATURE", "desktop-test");
        qputenv("DESKTOP_TEST_DELAY", "0");
        qputenv("DESKTOP_TEST_EXIT", "0");
        QFile::remove(m_dir.filePath("queries"));
        response("{\"int\":11}");
    }

    void effectiveInteger_data() {
        QTest::addColumn<QByteArray>("output");
        QTest::addColumn<double>("expected");
        QTest::newRow("zero") << QByteArray("{\"int\":0}") << 0.0;
        QTest::newRow("positive") << QByteArray("{\"option\":\"decoration:rounding\",\"int\":17,\"set\":false}") << 17.0;
        QTest::newRow("over-100") << QByteArray("{\"int\":120}") << 120.0;
        QTest::newRow("malformed") << QByteArray("{broken") << 0.0;
        QTest::newRow("empty") << QByteArray() << 0.0;
        QTest::newRow("missing-int") << QByteArray("{\"float\":12}") << 0.0;
        QTest::newRow("string") << QByteArray("{\"int\":\"12\"}") << 0.0;
        QTest::newRow("null") << QByteArray("{\"int\":null}") << 0.0;
        QTest::newRow("boolean") << QByteArray("{\"int\":true}") << 0.0;
        QTest::newRow("array") << QByteArray("[{\"int\":12}]") << 0.0;
        QTest::newRow("negative") << QByteArray("{\"int\":-1}") << 0.0;
        QTest::newRow("fraction") << QByteArray("{\"int\":1.5}") << 0.0;
        QTest::newRow("overflow") << QByteArray("{\"int\":2147483648}") << 0.0;
        QTest::newRow("nonfinite") << QByteArray("{\"int\":1e999}") << 0.0;
        QTest::newRow("trailing-junk") << QByteArray("{\"int\":12} junk") << 0.0;
        QTest::newRow("stdout-limit") << (QByteArray("{\"int\":12}") + QByteArray(8182, ' ')) << 12.0;
        QTest::newRow("stdout-over-limit") << (QByteArray("{\"int\":12}") + QByteArray(8183, ' ')) << 0.0;
        QTest::newRow("stdout-flood") << QByteArray(1024 * 1024, 'x') << 0.0;
    }
    void effectiveInteger() {
        QFETCH(QByteArray, output);
        QFETCH(double, expected);
        Desktop desktop(nullptr, command());
        QCOMPARE(desktop.cornerRadius(), 0.0);
        QTRY_COMPARE_WITH_TIMEOUT(desktop.cornerRadius(), 11.0, 2000);
        response(output);
        query(desktop);
        QTRY_COMPARE_WITH_TIMEOUT(desktop.cornerRadius(), expected, 2000);
        QCOMPARE(queries(), 2);
        // A failed/oversized response must not poison subsequent queries.
        QTest::qWait(50);
        response("{\"int\":9}");
        query(desktop);
        QTRY_COMPARE_WITH_TIMEOUT(desktop.cornerRadius(), 9.0, 2000);
    }

    void paletteColor1() {
        // color1 comes from the Omarchy palette; without it, the accent.
        const QString theme = m_dir.filePath("omarchy/current/theme");
        QVERIFY(QDir().mkpath(theme));
        QFile file(theme + "/colors.toml");
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("accent = \"#112233\"\nbackground = \"#000000\"\nforeground = \"#ffffff\"\ncolor1 = \"#cc4455\"\n");
        file.close();
        {
            Desktop desktop(nullptr, command());
            QCOMPARE(desktop.property("color1").value<QColor>(), QColor("#cc4455"));
        }
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("accent = \"#112233\"\nred = \"#ed5b5a\"\n");
        file.close();
        {
            Desktop desktop(nullptr, command());
            QCOMPARE(desktop.property("color1").value<QColor>(), QColor("#ed5b5a"));
        }
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("accent = \"#112233\"\nbackground = \"#000000\"\n");
        file.close();
        {
            Desktop desktop(nullptr, command());
            QCOMPARE(desktop.property("color1").value<QColor>(), QColor("#112233"));
            QCOMPARE(desktop.property("color3").value<QColor>(), QColor("#112233"));
        }
        // color3 (the highlighter ink): `color3`, else `yellow`, else the accent.
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("accent = \"#112233\"\ncolor3 = \"#e5c07b\"\nyellow = \"#ffff00\"\n");
        file.close();
        {
            Desktop desktop(nullptr, command());
            QCOMPARE(desktop.property("color3").value<QColor>(), QColor("#e5c07b"));
        }
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("accent = \"#112233\"\nyellow = \"#f1fa8c\"\n");
        file.close();
        {
            Desktop desktop(nullptr, command());
            QCOMPARE(desktop.property("color3").value<QColor>(), QColor("#f1fa8c"));
        }
        QVERIFY(QFile::remove(file.fileName()));
    }
    void nonzeroExit() {
        Desktop desktop(nullptr, command());
        QTRY_COMPARE(desktop.cornerRadius(), 11.0);
        qputenv("DESKTOP_TEST_EXIT", "1");
        response("{\"int\":18}");
        query(desktop);
        QTRY_COMPARE(desktop.cornerRadius(), 0.0);
    }

    void timeoutDoesNotFreezeOrOverlap() {
        Desktop desktop(nullptr, command());
        QTRY_COMPARE(desktop.cornerRadius(), 11.0);
        qputenv("DESKTOP_TEST_DELAY", "10000");
        int ticks = 0;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, this, [&] { ++ticks; query(desktop); });
        QElapsedTimer elapsed;
        elapsed.start();
        query(desktop);
        QVERIFY(elapsed.elapsed() < 250);
        heartbeat.start(20);
        QTRY_COMPARE_WITH_TIMEOUT(desktop.cornerRadius(), 0.0, 2000);
        heartbeat.stop();
        QVERIFY(elapsed.elapsed() >= 800);
        QVERIFY(elapsed.elapsed() < 2000);
        QVERIFY(ticks >= 10);
        // The requests made during the slow query start no process of their
        // own; together they ask once more after it, so the latest value
        // arrives without any polling.
        qputenv("DESKTOP_TEST_DELAY", "0");
        response("{\"int\":7}");
        query(desktop);
        QTRY_COMPARE(desktop.cornerRadius(), 7.0);
        QVERIFY(queries() <= 4);
    }

    void missingEnvironment() {
        qunsetenv("HYPRLAND_INSTANCE_SIGNATURE");
        Desktop desktop(nullptr, command());
        query(desktop);
        QTest::qWait(100);
        QCOMPARE(desktop.cornerRadius(), 0.0);
        QCOMPARE(queries(), 0);
        qputenv("HYPRLAND_INSTANCE_SIGNATURE", "desktop-test");
        query(desktop);
        QTRY_COMPARE(desktop.cornerRadius(), 11.0);
        qunsetenv("HYPRLAND_INSTANCE_SIGNATURE");
        query(desktop);
        QCOMPARE(desktop.cornerRadius(), 0.0);
        QCOMPARE(queries(), 1);
    }

    void missingCommand() {
        const QString missing = m_dir.filePath("missing-hyprctl");
        Desktop desktop(nullptr, missing);
        bool responsive = false;
        QTimer::singleShot(20, this, [&] { responsive = true; });
        QTRY_VERIFY_WITH_TIMEOUT(responsive, 500);
        QCOMPARE(desktop.cornerRadius(), 0.0);
        QCOMPARE(queries(), 0);
        QVERIFY(QFile::link(QCoreApplication::applicationFilePath(), missing));
        query(desktop);
        QTRY_COMPARE(desktop.cornerRadius(), 11.0);
        QVERIFY(QFile::remove(missing));
        query(desktop);
        QTRY_COMPARE(desktop.cornerRadius(), 0.0);
    }

    void readableSecondaryTextErrorAndSelection() {
        // WCAG ratios: black on white is 21:1, a color on itself 1:1.
        QCOMPARE(qRound(Desktop::contrast(Qt::black, Qt::white) * 100), 2100);
        QCOMPARE(Desktop::contrast(QColor("#336699"), QColor("#336699")), 1.0);
        const QString theme = m_dir.filePath("omarchy/current/theme");
        QVERIFY(QDir().mkpath(theme));
        QFile file(theme + "/colors.toml");
        auto write = [&](const QByteArray &bytes) {
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); file.write(bytes); file.close();
        };
        // A readable muted is kept: the least prominent that reads.
        write("background = \"#060B1E\"\nforeground = \"#ffcead\"\nmuted = \"#6d7db6\"\n"
              "dark_foreground = \"#6d7db6\"\nlight_foreground = \"#c9b8a6\"\nred = \"#ED5B5A\"\nselection = \"#252e56\"\naccent = \"#7d82d9\"\n");
        {
            Desktop desktop(nullptr, command());
            QCOMPARE(desktop.property("muted").value<QColor>(), QColor("#6d7db6"));
            QVERIFY(Desktop::contrast(QColor("#6d7db6"), QColor("#060B1E")) >= 4.5);
            QCOMPARE(desktop.property("error").value<QColor>(), QColor("#ED5B5A"));
            QCOMPARE(desktop.property("selection").value<QColor>(), QColor("#252e56"));
        }
        // Too faint a muted (under 4.5:1) gives way to the next that reads.
        write("background = \"#ffffff\"\nforeground = \"#000000\"\nmuted = \"#cccccc\"\n"
              "dark_foreground = \"#333333\"\nlight_foreground = \"#767676\"\naccent = \"#2255aa\"\n");
        {
            Desktop desktop(nullptr, command());
            QCOMPARE(desktop.property("muted").value<QColor>(), QColor("#767676"));
            // No red: color1's fallback, the accent; no selection: a tint of the accent.
            QCOMPARE(desktop.property("error").value<QColor>(), QColor("#2255aa"));
            const auto selection = desktop.property("selection").value<QColor>();
            QVERIFY(selection != QColor("#2255aa") && selection != QColor("#ffffff"));
        }
        // Nothing but the foreground reads: the foreground.
        write("background = \"#ffffff\"\nforeground = \"#000000\"\nmuted = \"#eeeeee\"\n");
        {
            Desktop desktop(nullptr, command());
            QCOMPARE(desktop.property("muted").value<QColor>(), QColor("#000000"));
        }
        QVERIFY(QFile::remove(file.fileName()));
    }
    void hyprlandAnimationsFollowReloads() {
        // Hyprland's request socket answers getoption; a config reload asks again.
        const QString requests = m_dir.filePath("hypr/desktop-test/.socket.sock");
        const QString events = m_dir.filePath("hypr/desktop-test/.socket2.sock");
        QVERIFY(QDir().mkpath(QFileInfo(requests).absolutePath()));
        QLocalServer requestServer, eventServer;
        QVERIFY2(requestServer.listen(requests), qPrintable(requestServer.errorString()));
        QVERIFY2(eventServer.listen(events), qPrintable(eventServer.errorString()));
        // Hyprland 0.56 answers with "bool".
        QByteArray reply = "{\"option\": \"animations:enabled\", \"bool\": false, \"set\": true }";
        QStringList asked;
        auto answer = [&](QLocalSocket *client) {
            asked << QString::fromUtf8(client->readAll());
            client->write(reply); client->flush(); client->disconnectFromServer();
        };
        std::unique_ptr<Desktop> desktop = std::make_unique<Desktop>(nullptr, command());
        // The request is sent within the constructor, before this process
        // returns to its event loop: Hyprland reads it on its main thread and
        // must never wait for us (it would stall the compositor and our window).
        QVERIFY(requestServer.waitForNewConnection(1000));
        auto *first = requestServer.nextPendingConnection();
        QVERIFY(first->bytesAvailable() || first->waitForReadyRead(1000));
        answer(first);
        connect(&requestServer, &QLocalServer::newConnection, this, [&] {
            auto *client = requestServer.nextPendingConnection();
            if (client->bytesAvailable()) answer(client);
            else connect(client, &QLocalSocket::readyRead, client, [&, client] { answer(client); });
        });
        Desktop &desktopRef = *desktop;
        QTRY_VERIFY(!desktopRef.animations());
        QCOMPARE(asked, QStringList{"j/getoption animations:enabled"});
        QTRY_VERIFY(eventServer.hasPendingConnections());
        std::unique_ptr<QLocalSocket> hyprland(eventServer.nextPendingConnection());
        // Older releases answer with "int".
        reply = "{\"int\":1}";
        hyprland->write("configreloaded>>\n"); QVERIFY(hyprland->waitForBytesWritten());
        QTRY_VERIFY(desktopRef.animations());
        QCOMPARE(asked.size(), 2);
        reply = "{\"int\":0}";
        hyprland->write("configreloaded>>\n"); QVERIFY(hyprland->waitForBytesWritten());
        QTRY_VERIFY(!desktopRef.animations());
        // A malformed reply means enabled, as outside Hyprland.
        reply = "{\"int\":0";
        hyprland->write("configreloaded>>\n"); QVERIFY(hyprland->waitForBytesWritten());
        QTRY_COMPARE(asked.size(), 4);
        QTRY_VERIFY(desktopRef.animations());
    }
    void configReloadEventsRequery() {
        // Hyprland's event socket: only config reloads ask for the rounding
        // again, never a timer or any other event.
        const QString events = m_dir.filePath("hypr/desktop-test/.socket2.sock");
        QVERIFY(QDir().mkpath(QFileInfo(events).absolutePath()));
        QLocalServer server;
        QVERIFY2(server.listen(events), qPrintable(server.errorString()));
        Desktop desktop(nullptr, command());
        QTRY_VERIFY(server.hasPendingConnections());
        std::unique_ptr<QLocalSocket> hyprland(server.nextPendingConnection());
        QTRY_COMPARE(desktop.cornerRadius(), 11.0);
        QCOMPARE(queries(), 1);
        response("{\"int\":23}");
        for (int i = 0; i < 3; ++i)
            QVERIFY(QMetaObject::invokeMethod(&desktop, "refresh", Qt::DirectConnection));
        hyprland->write("activewindow>>kitty,shell\nworkspace>>2\nconfigreloaded");
        QVERIFY(hyprland->waitForBytesWritten());
        QTest::qWait(300);
        QCOMPARE(queries(), 1);
        QCOMPARE(desktop.cornerRadius(), 11.0);
        // A line split across reads still counts once it ends.
        hyprland->write(">>\n");
        QVERIFY(hyprland->waitForBytesWritten());
        QTRY_COMPARE(desktop.cornerRadius(), 23.0);
        QCOMPARE(queries(), 2);
        QTest::qWait(300);
        QCOMPARE(queries(), 2);
    }

    void themeSwapsFollowWithoutPolling() {
        // omarchy-theme-set stages next-theme, removes theme, and moves
        // next-theme in; the old theme stays until the new one arrives.
        const QString current = m_dir.filePath("omarchy/current");
        QVERIFY(QDir(current + "/theme").removeRecursively());
        QVERIFY(QDir().mkpath(current + "/theme"));
        auto write = [](const QString &path, const QByteArray &accent) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
                && file.write("background = \"#000000\"\nforeground = \"#ffffff\"\naccent = \"" + accent + "\"\n") > 0;
        };
        QVERIFY(write(current + "/theme/colors.toml", "#112233"));
        Desktop desktop(nullptr, command());
        const auto accent = [&] { return desktop.property("accent").value<QColor>(); };
        QCOMPARE(accent(), QColor("#112233"));
        QVERIFY(QDir().mkpath(current + "/next-theme"));
        QVERIFY(write(current + "/next-theme/colors.toml", "#445566"));
        QVERIFY(QDir(current + "/theme").removeRecursively());
        QTest::qWait(300);
        QCOMPARE(accent(), QColor("#112233"));
        QCOMPARE(desktop.property("background").value<QColor>(), QColor("#000000"));
        QVERIFY(QDir(current).rename("next-theme", "theme"));
        QTRY_COMPARE(accent(), QColor("#445566"));
        // The new theme's file is watched in turn.
        QVERIFY(write(current + "/theme/colors.toml", "#778899"));
        QTRY_COMPARE(accent(), QColor("#778899"));
        QVERIFY(QDir(current + "/theme").removeRecursively());
    }

    void fontConfigChangesFollow() {
        // omarchy-font-set rewrites fontconfig/fonts.conf to put the chosen
        // family first for monospace; the app follows at once.
        QStringList families;
        for (const QString &family : QFontDatabase::families()) {
            if (resolveFamily(family) == family && !families.contains(family)) families << family;
            if (families.size() == 2) break;
        }
        if (families.size() < 2) QSKIP("Two installed font families are needed.");
        const QString directory = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/fontconfig";
        QVERIFY(QDir().mkpath(directory));
        auto choose = [&](const QString &family) {
            QFile file(directory + "/fonts.conf");
            return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(
                "<?xml version=\"1.0\"?>\n<!DOCTYPE fontconfig SYSTEM \"fonts.dtd\">\n<fontconfig>\n"
                "  <match target=\"pattern\">\n    <test name=\"family\" qual=\"any\"><string>monospace</string></test>\n"
                "    <edit name=\"family\" mode=\"prepend_first\" binding=\"strong\"><string>" + family.toHtmlEscaped().toUtf8()
                + "</string></edit>\n  </match>\n</fontconfig>\n") > 0;
        };
        QVERIFY(choose(families[0]));
        FcInitReinitialize();
        Desktop desktop(nullptr, command());
        QCOMPARE(desktop.property("fontFamily").toString(), families[0]);
        QVERIFY(choose(families[1]));
        QTRY_COMPARE(desktop.property("fontFamily").toString(), families[1]);
        QVERIFY(QFile::remove(directory + "/fonts.conf"));
        FcInitReinitialize();
    }

    void toolTipThemeAndWrapping() {
        QQmlEngine engine;
        struct Theme : QQmlPropertyMap {
            Theme() : QQmlPropertyMap(this, nullptr) {}
        } theme;
        theme.insert("fontFamily", QStringLiteral("monospace"));
        theme.insert("textScale", 1.5);
        theme.insert("foreground", QColor("#eeeeee"));
        theme.insert("surface", QColor("#222222"));
        theme.insert("border", QColor("#555555"));
        theme.insert("cornerRadius", 13.0);
        const QString source = QFINDTESTDATA("../qml/AppToolTip.qml");
        QVERIFY(!source.isEmpty());
        QQmlComponent component(&engine, QUrl::fromLocalFile(source));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> tooltip(component.createWithInitialProperties({
            {"theme", QVariant::fromValue(&theme)},
            {"text", QString("<b>Plain text</b> ").repeated(50)}, {"delay", 650}
        }));
        QVERIFY2(tooltip, qPrintable(component.errorString()));
        QCOMPARE(tooltip->property("delay").toInt(), 650);
        const QFont font = tooltip->property("font").value<QFont>();
        QCOMPARE(font.family(), QString("monospace"));
        QCOMPARE(font.pixelSize(), 18);
        auto *background = tooltip->property("background").value<QQuickItem *>();
        auto *content = tooltip->property("contentItem").value<QQuickItem *>();
        QVERIFY(background);
        QVERIFY(content);
        QCOMPARE(background->property("color").value<QColor>(), QColor("#222222"));
        QCOMPARE(QQmlProperty::read(background, "border.color").value<QColor>(), QColor("#555555"));
        QCOMPARE(background->property("radius").toReal(), 13.0);
        QCOMPARE(content->property("color").value<QColor>(), QColor("#eeeeee"));
        QCOMPARE(content->property("text").toString(), QString("<b>Plain text</b> ").repeated(50));
        QCOMPARE(content->property("textFormat").toInt(), 0); // Text.PlainText
        QCOMPARE(content->property("wrapMode").toInt(), 4); // Text.Wrap
        QVERIFY(tooltip->property("width").toReal() <= 540);
        QVERIFY(tooltip->property("width").toReal() > 100);
        QVERIFY(content->implicitHeight() > 2 * font.pixelSize());
        theme.insert("cornerRadius", 0.0);
        theme.insert("textScale", 1.0);
        QTRY_COMPARE(background->property("radius").toReal(), 0.0);
        QCOMPARE(tooltip->property("font").value<QFont>().pixelSize(), 12);
    }

    void sharedAcrossWindows() {
        Desktop desktop(nullptr, command());
        QQmlEngine firstEngine, secondEngine;
        QQuickWindow firstWindow, secondWindow;
        firstWindow.resize(240, 200);
        secondWindow.resize(640, 400);
        const QUrl source = QUrl::fromLocalFile(QFINDTESTDATA("../qml/AppToolTip.qml"));
        QQmlComponent firstComponent(&firstEngine, source), secondComponent(&secondEngine, source);
        std::unique_ptr<QObject> first(firstComponent.createWithInitialProperties({
            {"theme", QVariant::fromValue(&desktop)}, {"parent", QVariant::fromValue(firstWindow.contentItem())},
            {"text", QString("Long tooltip ").repeated(50)}
        }));
        std::unique_ptr<QObject> second(secondComponent.createWithInitialProperties({
            {"theme", QVariant::fromValue(&desktop)}, {"parent", QVariant::fromValue(secondWindow.contentItem())}
        }));
        QVERIFY2(first, qPrintable(firstComponent.errorString()));
        QVERIFY2(second, qPrintable(secondComponent.errorString()));
        auto *firstBackground = first->property("background").value<QQuickItem *>();
        auto *secondBackground = second->property("background").value<QQuickItem *>();
        QVERIFY(firstBackground);
        QVERIFY(secondBackground);
        QTRY_COMPARE(firstBackground->property("radius").toReal(), 11.0);
        QCOMPARE(secondBackground->property("radius").toReal(), 11.0);
        QVERIFY(first->property("width").toReal() <= 224);
        response("{\"int\":0}");
        query(desktop);
        QTRY_COMPARE(firstBackground->property("radius").toReal(), 0.0);
        QCOMPARE(secondBackground->property("radius").toReal(), 0.0);
        QCOMPARE(queries(), 2);
    }
};

int main(int argc, char **argv) {
    if (argc > 1 && QByteArray(argv[1]) == "getoption") {
        QCoreApplication app(argc, argv);
        return fakeHyprctl(app);
    }
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_BACKEND", "software");
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle("Basic");
    DesktopTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_desktop.moc"
