#include <QtTest/QTest>
#include "singleinstance.h"
#include <QGuiApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QWindow>

namespace {
struct Environment {
    QByteArray name, old;
    bool existed;
    Environment(const char *key, const QByteArray &value)
        : name(key), old(qgetenv(key)), existed(qEnvironmentVariableIsSet(key)) { qputenv(key, value); }
    ~Environment() { if (existed) qputenv(name, old); else qunsetenv(name); }
};
struct Child : QProcess {
    ~Child() { if (state() != NotRunning) { kill(); waitForFinished(3000); } }
};
const QByteArray activation = "{\"version\":1,\"activate\":true}\n";
QProcessEnvironment isolatedEnvironment(const QString &path)
{
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("QT_QPA_PLATFORM", "offscreen");
    env.insert("QT_QUICK_BACKEND", "software");
    env.insert("XDG_DATA_HOME", path);
    env.insert("XDG_CONFIG_HOME", path);
    env.insert("XDG_RUNTIME_DIR", path);
    env.remove("HYPRLAND_INSTANCE_SIGNATURE");
    env.remove("XDG_ACTIVATION_TOKEN");
    env.remove("DESKTOP_STARTUP_ID");
    return env;
}
}

class SingleInstanceTest : public QObject
{
    Q_OBJECT
private slots:
    void canonicalScopeAndLegacyLock()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString alias = dir.path() + "/alias";
        QVERIFY(QFile::link(dir.path(), alias));
        SingleInstance primary(dir.path(), "test", dir.path());
        SingleInstance secondary(alias, "test", dir.path());
        QCOMPARE(primary.endpoint(), secondary.endpoint());
        QVERIFY(QFileInfo(primary.endpoint()).fileName().startsWith("omaword-"));
        QCOMPARE(primary.start(), SingleInstance::Role::Primary);
        QCOMPARE(secondary.start(), SingleInstance::Role::Secondary);
        QLockFile legacy(dir.path() + "/test.lock");
        legacy.setStaleLockTime(0);
        QVERIFY(!legacy.tryLock());
        qint64 pid = 0;
        QVERIFY(legacy.getLockInfo(&pid, nullptr, nullptr));
        QCOMPARE(pid, QCoreApplication::applicationPid());
        QTemporaryDir other;
        SingleInstance independent(other.path(), "test", dir.path());
        QVERIFY(independent.endpoint() != primary.endpoint());
        QCOMPARE(independent.start(), SingleInstance::Role::Primary);
    }

    void utf8AndNoArgumentRelaunch()
    {
        QTemporaryDir dir;
        Environment token("XDG_ACTIVATION_TOKEN", "test-token");
        Environment startup("DESKTOP_STARTUP_ID", "test-startup");
        SingleInstance primary(dir.path(), "test", dir.path()), secondary(dir.path(), "test", dir.path());
        QCOMPARE(primary.start(), SingleInstance::Role::Primary);
        QCOMPARE(secondary.start(), SingleInstance::Role::Secondary);
        QList<SingleInstance::Request> requests;
        primary.setHandler([&](const auto &request, auto reply) { requests.append(request); reply({true, {}}); });
        const auto first = SingleInstance::Request::fromArguments({QString::fromUtf8("Jean 3:16 — λόγος 🌿")},
            dir.path() + QString::fromUtf8("/日本語 Bible.epub"), true);
        bool done = false;
        SingleInstance::Result result{false, {}};
        secondary.forward(first, [&](auto value) { result = value; done = true; });
        QTRY_VERIFY(done);
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(requests.size(), 1);
        QCOMPARE(requests.first().reference, first.reference);
        QCOMPARE(requests.first().importPath, first.importPath);
        QCOMPARE(requests.first().activationToken, QString("test-token"));
        QCOMPARE(requests.first().startupId, QString("test-startup"));
        QVERIFY(requests.first().newWindow);
        done = false;
        secondary.forward(SingleInstance::Request::fromArguments({}, first.importPath, false),
                          [&](auto value) { result = value; done = true; });
        QTRY_VERIFY(done);
        QVERIFY(result.ok);
        QCOMPARE(requests.size(), 2);
        QVERIFY(requests.last().importPath.isEmpty());
        QVERIFY(requests.last().reference.isEmpty());
        QVERIFY(requests.last().newWindow);
        auto reuse = first;
        reuse.newWindow = false;
        done = false;
        secondary.forward(reuse, [&](auto value) { result = value; done = true; });
        QTRY_VERIFY(done);
        QVERIFY(result.ok);
        QVERIFY(!requests.last().newWindow);
    }

    void startupQueueAndSerializedDispatch()
    {
        QTemporaryDir dir;
        SingleInstance primary(dir.path(), "test", dir.path()), secondary(dir.path(), "test", dir.path());
        QCOMPARE(primary.start(), SingleInstance::Role::Primary);
        int completed = 0, calls = 0;
        secondary.forward({}, [&](auto result) { QVERIFY(result.ok); ++completed; });
        secondary.forward({}, [&](auto result) { QVERIFY(result.ok); ++completed; });
        QTest::qWait(100);
        QCOMPARE(completed, 0);
        SingleInstance::Reply outstanding;
        primary.setHandler([&](const auto &, auto reply) { ++calls; outstanding = reply; });
        QTRY_COMPARE(calls, 1);
        QTest::qWait(50);
        QCOMPARE(calls, 1);
        auto firstReply = outstanding;
        firstReply({true, {}});
        firstReply({true, {}}); // Completion cannot dispatch twice.
        QTRY_COMPARE(calls, 2);
        outstanding({true, {}});
        QTRY_COMPARE(completed, 2);
    }

    void retriesUntilServerStarts()
    {
        QTemporaryDir dir;
        QLockFile legacy(dir.path() + "/test.lock");
        QVERIFY(legacy.tryLock());
        SingleInstance primary(dir.path(), "test", dir.path()), secondary(dir.path(), "test", dir.path());
        QCOMPARE(secondary.start(), SingleInstance::Role::Secondary);
        bool done = false;
        secondary.forward({}, [&](auto result) { QVERIFY2(result.ok, qPrintable(result.error)); done = true; });
        QTimer::singleShot(200, &primary, [&] {
            legacy.unlock();
            QCOMPARE(primary.start(), SingleInstance::Role::Primary);
            primary.setHandler([](const auto &, auto reply) { reply({true, {}}); });
        });
        QTRY_VERIFY(done);
    }

    void malformedFrames_data()
    {
        QTest::addColumn<QByteArray>("frame");
        QTest::newRow("json") << QByteArray("not json\n");
        QTest::newRow("shape") << QByteArray("[]\n");
        QTest::newRow("version") << QByteArray("{\"version\":2,\"activate\":true}\n");
        QTest::newRow("activate") << QByteArray("{\"version\":1,\"activate\":false}\n");
        QTest::newRow("reference-type") << QByteArray("{\"version\":1,\"activate\":true,\"reference\":8}\n");
        QTest::newRow("newWindow-type") << QByteArray("{\"version\":1,\"activate\":true,\"newWindow\":8}\n");
        QTest::newRow("unknown-field") << QByteArray("{\"version\":1,\"activate\":true,\"path\":\"/tmp/x\"}\n");
        QTest::newRow("relative-import") << QByteArray("{\"version\":1,\"activate\":true,\"import\":\"x.epub\"}\n");
        QTest::newRow("long-reference") << QByteArray("{\"version\":1,\"activate\":true,\"reference\":\"") + QByteArray(1025, 'x') + "\"}\n";
        QTest::newRow("null-in-path") << QByteArray("{\"version\":1,\"activate\":true,\"import\":\"/tmp/x\\u0000\"}\n");
        QTest::newRow("two-frames") << activation + activation;
        QTest::newRow("oversize") << QByteArray(SingleInstance::MaxFrame + 1, 'x');
    }
    void malformedFrames()
    {
        QFETCH(QByteArray, frame);
        QTemporaryDir dir;
        SingleInstance primary(dir.path(), "test", dir.path());
        QCOMPARE(primary.start(), SingleInstance::Role::Primary);
        int calls = 0;
        primary.setHandler([&](const auto &, auto reply) { ++calls; reply({true, {}}); });
        QLocalSocket socket;
        socket.connectToServer(primary.endpoint());
        QTRY_COMPARE(socket.state(), QLocalSocket::ConnectedState);
        socket.write(frame);
        QTRY_VERIFY(socket.bytesAvailable() > 0);
        const auto ack = QJsonDocument::fromJson(socket.readAll()).object();
        QCOMPARE(ack.value("ok"), QJsonValue(false));
        QVERIFY(!ack.value("error").toString().isEmpty());
        QCOMPARE(calls, 0);
    }

    void partialFrameTimeoutAndAbandonedStartup()
    {
        QTemporaryDir dir;
        SingleInstance primary(dir.path(), "test", dir.path()), secondary(dir.path(), "test", dir.path());
        QCOMPARE(primary.start(), SingleInstance::Role::Primary);
        QLocalSocket partial;
        partial.connectToServer(primary.endpoint());
        QTRY_COMPARE(partial.state(), QLocalSocket::ConnectedState);
        partial.write("{");
        bool done = false;
        secondary.forward({}, [&](auto result) { QVERIFY(!result.ok); QVERIFY(result.error.contains("unknown")); done = true; }, 100);
        QTRY_VERIFY(done);
        QTRY_VERIFY_WITH_TIMEOUT(partial.bytesAvailable() > 0, 4500);
        QVERIFY(!QJsonDocument::fromJson(partial.readAll()).object().value("ok").toBool());
        int calls = 0;
        primary.setHandler([&](const auto &, auto reply) { ++calls; reply({true, {}}); });
        QTest::qWait(50);
        QCOMPARE(calls, 0);
    }

    void negativeAndInvalidAcknowledgements()
    {
        QTemporaryDir dir;
        SingleInstance client(dir.path(), "test", dir.path());
        QLocalServer server;
        QVERIFY(server.listen(client.endpoint()));
        QByteArray response;
        connect(&server, &QLocalServer::newConnection, &server, [&] {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QLocalSocket::readyRead, socket, [&, socket] {
                socket->readAll(); socket->write(response); socket->disconnectFromServer();
            });
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        });
        const QList<QByteArray> responses = {
            "{\"version\":1,\"ok\":false,\"error\":\"Import already running\"}\n",
            "{\"ok\":true}\n", "{\"version\":1,\"ok\":true,\"error\":\"failure\"}\n",
            QByteArray(SingleInstance::MaxFrame + 1, 'x'), ""};
        for (const auto &value : responses) {
            response = value;
            bool done = false;
            client.forward({}, [&](auto result) { QVERIFY(!result.ok); QVERIFY(!result.error.isEmpty()); done = true; }, 500);
            QTRY_VERIFY(done);
        }
    }

    void olderInstanceTimeoutPreservesLock()
    {
        QTemporaryDir dir;
        QLockFile legacy(dir.path() + "/test.lock");
        QVERIFY(legacy.tryLock());
        SingleInstance secondary(dir.path(), "test", dir.path());
        QCOMPARE(secondary.start(), SingleInstance::Role::Secondary);
        bool done = false;
        QElapsedTimer elapsed;
        elapsed.start();
        secondary.forward({}, [&](auto result) {
            QVERIFY(!result.ok); QVERIFY(result.error.contains("older version")); done = true;
        }, 150);
        QTRY_VERIFY(done);
        QVERIFY(elapsed.elapsed() < 1000);
        QVERIFY(legacy.isLocked());
        QVERIFY(QFile::exists(dir.path() + "/test.lock"));
    }

    void hyprlandActivation_data()
    {
        QTest::addColumn<QByteArray>("mode");
        QTest::addColumn<bool>("success");
        QTest::newRow("own-pid-only") << QByteArray("ok") << true;
        QTest::newRow("wayland-display-name-suffix") << QByteArray("suffix") << true;
        QTest::newRow("window-still-mapping") << QByteArray("mapping") << true;
        QTest::newRow("other-pid-only") << QByteArray("other") << false;
        QTest::newRow("malformed-clients") << QByteArray("malformed") << false;
        QTest::newRow("address-injection") << QByteArray("injection") << false;
        QTest::newRow("dispatch-error") << QByteArray("error") << false;
        QTest::newRow("process-timeout") << QByteArray("timeout") << false;
    }
    void hyprlandActivation()
    {
        QFETCH(QByteArray, mode);
        QFETCH(bool, success);
        QTemporaryDir dir;
        QVERIFY(QFile::link(QCoreApplication::applicationFilePath(), dir.path() + "/hyprctl"));
        Environment path("PATH", dir.path().toUtf8());
        Environment signature("HYPRLAND_INSTANCE_SIGNATURE", "isolated-test");
        Environment fake("SINGLEINSTANCE_FAKE_HYPRCTL", mode);
        Environment pid("SINGLEINSTANCE_TEST_PID", QByteArray::number(QCoreApplication::applicationPid()));
        Environment marker("SINGLEINSTANCE_TEST_MARKER", (dir.path() + "/mapped").toUtf8());
        Environment display("SINGLEINSTANCE_TEST_DISPLAY", QGuiApplication::applicationDisplayName().toUtf8());
        QWindow window;
        window.setTitle("OmaWord · Window 2");
        window.showMinimized();
        bool done = false;
        int ticks = 0;
        QTimer heartbeat;
        heartbeat.setInterval(10);
        connect(&heartbeat, &QTimer::timeout, this, [&] { ++ticks; });
        heartbeat.start();
        QElapsedTimer elapsed;
        elapsed.start();
        SingleInstance::activateWindow(&window, {}, this, [&](auto result) {
            QCOMPARE(result.ok, success); done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 2500);
        QVERIFY(window.windowState() != Qt::WindowMinimized);
        QVERIFY(elapsed.elapsed() < 2500);
        if (mode == "timeout") QVERIFY(ticks > 20);
    }

    void twoProcessesAndCrashRecovery()
    {
        QTemporaryDir dir;
        Child primary;
        primary.setProcessEnvironment(isolatedEnvironment(dir.path()));
        primary.start(QCoreApplication::applicationFilePath(), {"--primary", dir.path()});
        QVERIFY(primary.waitForStarted());
        QTRY_VERIFY(primary.bytesAvailable() > 0);
        QCOMPARE(primary.readLine().trimmed(), QByteArray("listening"));
        SingleInstance secondary(dir.path(), "test", dir.path());
        QCOMPARE(secondary.start(), SingleInstance::Role::Secondary);
        bool done = false;
        secondary.forward({QString::fromUtf8("Éphésiens 2:8"), dir.path() + "/explicit.epub", {}, {}},
                          [&](auto result) { QVERIFY2(result.ok, qPrintable(result.error)); done = true; });
        QTRY_VERIFY(done);
        QTRY_VERIFY(primary.bytesAvailable() > 0);
        const auto received = QJsonDocument::fromJson(primary.readLine()).object();
        QCOMPARE(received.value("reference").toString(), QString::fromUtf8("Éphésiens 2:8"));
        QCOMPARE(received.value("import").toString(), dir.path() + "/explicit.epub");
        QVERIFY(QFile::exists(secondary.endpoint()));
        primary.kill(); // Only the test-owned child; leave a real stale socket + lock.
        QVERIFY(primary.waitForFinished());
        QVERIFY(QFile::exists(secondary.endpoint()));
        {
            QLockFile legacy(dir.path() + "/test.lock");
            legacy.setStaleLockTime(0);
            QVERIFY(legacy.tryLock());
            SingleInstance blocked(dir.path(), "test", dir.path());
            QCOMPARE(blocked.start(), SingleInstance::Role::Secondary);
            QVERIFY(QFile::exists(secondary.endpoint()));
            QVERIFY(legacy.isLocked());
        }
        SingleInstance replacement(dir.path(), "test", dir.path());
        QCOMPARE(replacement.start(), SingleInstance::Role::Primary);
        replacement.setHandler([](const auto &, auto reply) { reply({true, {}}); });
        done = false;
        secondary.forward({}, [&](auto result) { QVERIFY(result.ok); done = true; });
        QTRY_VERIFY(done);
    }

    void actualApplicationHandoff()
    {
        const auto executable = qEnvironmentVariable("OMAWORD_TEST_APP");
        if (executable.isEmpty()) QSKIP("Set OMAWORD_TEST_APP to test the freshly built application.");
        QTemporaryDir dir;
        Child primary;
        primary.setProcessEnvironment(isolatedEnvironment(dir.path()));
        primary.start(executable, {"--import", dir.path() + "/missing-initial.epub"});
        QVERIFY(primary.waitForStarted());
        QByteArray errors;
        QTRY_VERIFY_WITH_TIMEOUT((errors += primary.readAllStandardError()).contains("Cannot read the selected EPUB"), 5000);
        QCOMPARE(primary.state(), QProcess::Running);
        // An empty relaunch must not repeat the original failing import.
        Child secondary;
        secondary.setProcessEnvironment(isolatedEnvironment(dir.path()));
        secondary.start(executable, QStringList{});
        QVERIFY(secondary.waitForFinished(5000));
        QCOMPARE(secondary.exitCode(), 0);
        secondary.start(executable, {"--import", dir.path() + QString::fromUtf8("/不存在.epub")});
        QVERIFY(secondary.waitForFinished(5000));
        QCOMPARE(secondary.exitCode(), 1);
        QVERIFY(secondary.readAllStandardError().contains("Cannot read the selected EPUB"));
        secondary.start(executable, {"John 3:16"});
        QVERIFY(secondary.waitForFinished(5000));
        QCOMPARE(secondary.exitCode(), 1); // No Bible installed: cannot claim navigation succeeded.
        QVERIFY(!secondary.readAllStandardError().isEmpty());
        QCOMPARE(primary.state(), QProcess::Running);
    }

};

int main(int argc, char **argv)
{
    // The executable doubles as a fake hyprctl. It never contacts a compositor.
    if (qEnvironmentVariableIsSet("SINGLEINSTANCE_FAKE_HYPRCTL")) {
        QCoreApplication app(argc, argv);
        const auto mode = qgetenv("SINGLEINSTANCE_FAKE_HYPRCTL");
        const auto args = app.arguments().mid(1);
        if (mode == "timeout") { QTimer::singleShot(10000, &app, &QCoreApplication::quit); return app.exec(); }
        if (args == QStringList{"clients", "-j"}) {
            if (mode == "malformed") { QTextStream(stdout) << "invalid" << Qt::endl; return 0; }
            if (mode == "mapping") {
                QFile marker(qEnvironmentVariable("SINGLEINSTANCE_TEST_MARKER"));
                if (!marker.exists()) {
                    if (!marker.open(QIODevice::WriteOnly)) return 2;
                    QTextStream(stdout) << "[]" << Qt::endl;
                    return 0;
                }
            }
            const qint64 pid = qEnvironmentVariable("SINGLEINSTANCE_TEST_PID").toLongLong();
            QJsonArray clients{QJsonObject{{"pid", pid + 1}, {"address", "0xbeef"}}};
            clients.append(QJsonObject{{"pid", pid}, {"title", "OmaWord · Window 1"}, {"address", "0xdef"}});
            const QString title = QString("OmaWord · Window 2") + (mode == "suffix" ? QString(" — ") + qEnvironmentVariable("SINGLEINSTANCE_TEST_DISPLAY") : QString());
            if (mode != "other") clients.append(QJsonObject{{"pid", pid}, {"title", title}, {"address", mode == "injection" ? "0xabc\" }); malicious()" : "0xabc"}});
            QTextStream(stdout) << QJsonDocument(clients).toJson(QJsonDocument::Compact) << Qt::endl;
            return 0;
        }
        if (args != QStringList{"dispatch", "hl.dsp.focus({ window = \"address:0xabc\" })"}) return 2;
        QTextStream(stdout) << (mode == "error" ? "error: dispatch failed" : "ok") << Qt::endl;
        return 0;
    }
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    if (app.arguments().value(1) == "--primary") {
        const auto path = app.arguments().value(2);
        SingleInstance primary(path, "test", path);
        if (primary.start() != SingleInstance::Role::Primary) return 2;
        QTextStream(stdout) << "listening" << Qt::endl;
        QTimer::singleShot(250, &primary, [&] {
            primary.setHandler([](const auto &request, auto reply) {
                QTextStream(stdout) << QJsonDocument(QJsonObject{{"reference", request.reference}, {"import", request.importPath}})
                    .toJson(QJsonDocument::Compact) << Qt::endl;
                reply({true, {}});
            });
        });
        return app.exec();
    }
    SingleInstanceTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_singleinstance.moc"
