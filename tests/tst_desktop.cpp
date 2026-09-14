#include <QTest>
#include "desktop.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFont>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlProperty>
#include <QQmlPropertyMap>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <memory>
#include <cstdio>

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

private slots:
    void initTestCase() {
        QVERIFY(m_dir.isValid());
        for (const auto &name : {"HYPRLAND_INSTANCE_SIGNATURE", "XDG_STATE_HOME",
                                "DESKTOP_TEST_LOG", "DESKTOP_TEST_RESPONSE",
                                "DESKTOP_TEST_DELAY", "DESKTOP_TEST_EXIT"})
            m_environment.insert(name, qgetenv(name));
        QVERIFY(QFile::link(QCoreApplication::applicationFilePath(), command()));
        qputenv("XDG_STATE_HOME", m_dir.path().toUtf8());
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
        QCOMPARE(queries(), 2);
        QTest::qWait(100);
        qputenv("DESKTOP_TEST_DELAY", "0");
        response("{\"int\":7}");
        query(desktop);
        QTRY_COMPARE(desktop.cornerRadius(), 7.0);
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

    void separateFiveSecondPolling() {
        Desktop desktop(nullptr, command());
        QTRY_COMPARE(desktop.cornerRadius(), 11.0);
        response("{\"int\":23}");
        for (int i = 0; i < 3; ++i)
            QVERIFY(QMetaObject::invokeMethod(&desktop, "refresh", Qt::DirectConnection));
        QTest::qWait(2500); // Includes a normal theme poll.
        QCOMPARE(queries(), 1);
        QCOMPARE(desktop.cornerRadius(), 11.0);
        QTRY_COMPARE_WITH_TIMEOUT(desktop.cornerRadius(), 23.0, 3500);
        QCOMPARE(queries(), 2);
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
