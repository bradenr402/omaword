#include "singleinstance.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>
#include <QWindow>
#include <memory>

namespace {
QJsonObject encode(const SingleInstance::Request &request)
{
    QJsonObject object{{"version", 1}, {"activate", true}};
    if (!request.reference.isEmpty()) object.insert("reference", request.reference);
    if (!request.importPath.isEmpty()) object.insert("import", request.importPath);
    if (!request.activationToken.isEmpty()) object.insert("token", request.activationToken);
    if (!request.startupId.isEmpty()) object.insert("startupId", request.startupId);
    return object;
}

bool decode(const QByteArray &frame, SingleInstance::Request *request)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(frame, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    const auto object = document.object();
    if (object.value("version") != QJsonValue(1) || object.value("activate") != QJsonValue(true)) return false;
    const QStringList keys{"version", "activate", "reference", "import", "token", "startupId"};
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (!keys.contains(it.key())) return false;
        if (it.key() == "version" || it.key() == "activate") continue;
        if (!it.value().isString()) return false;
        const auto text = it.value().toString();
        const int limit = it.key() == "reference" ? 1024 : 4096;
        if (text.isEmpty() || text.size() > limit || text.contains(QChar::Null)) return false;
    }
    request->reference = object.value("reference").toString();
    request->importPath = object.value("import").toString();
    request->activationToken = object.value("token").toString();
    request->startupId = object.value("startupId").toString();
    return request->importPath.isEmpty() || QFileInfo(request->importPath).isAbsolute();
}

// No shell, blocking waits, or unbounded compositor output on the GUI thread.
void hyprctl(QObject *context, const QStringList &arguments,
             std::function<void(bool, QByteArray)> reply)
{
    auto *process = new QProcess(context);
    auto *timer = new QTimer(process);
    timer->setSingleShot(true);
    auto output = std::make_shared<QByteArray>();
    auto failed = std::make_shared<bool>(false);
    auto done = std::make_shared<bool>(false);
    auto finish = [process, timer, output, failed, done, reply](bool success) {
        if (*done) return;
        *done = true;
        timer->stop();
        reply(success && !*failed, *output);
        process->deleteLater();
    };
    QObject::connect(process, &QProcess::readyReadStandardOutput, process, [process, output, failed] {
        const auto bytes = process->readAllStandardOutput();
        if (output->size() + bytes.size() > 1024 * 1024) { *failed = true; process->kill(); }
        else output->append(bytes);
    });
    process->setStandardErrorFile(QProcess::nullDevice());
    QObject::connect(timer, &QTimer::timeout, process, [process, failed] { *failed = true; process->kill(); });
    QObject::connect(process, &QProcess::finished, process, [finish](int code, QProcess::ExitStatus status) {
        finish(code == 0 && status == QProcess::NormalExit);
    });
    QObject::connect(process, &QProcess::errorOccurred, process, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) finish(false);
    });
    process->start(QStringLiteral("hyprctl"), arguments);
    // Even two mapping retries plus dispatch fit inside the handoff deadline.
    timer->start(600);
}

void focusHyprland(QObject *context, qint64 pid, SingleInstance::Reply reply, int retries = 2)
{
    hyprctl(context, {"clients", "-j"}, [context, pid, reply, retries](bool ok, const QByteArray &output) {
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(output, &error);
        const bool validClients = ok && error.error == QJsonParseError::NoError && document.isArray();
        QString address;
        if (validClients) {
            for (const auto &value : document.array()) {
                const auto client = value.toObject();
                if (client.value("pid").toInteger(-1) != pid) continue;
                const auto candidate = client.value("address").toString();
                if (QRegularExpression(QStringLiteral("^0x[0-9a-fA-F]+$")).match(candidate).hasMatch()) {
                    address = candidate; break;
                }
            }
        }
        // QML can be ready just before the compositor has mapped its surface.
        if (address.isEmpty() && validClients && retries > 0) {
            QTimer::singleShot(75, context, [context, pid, reply, retries] { focusHyprland(context, pid, reply, retries - 1); });
            return;
        }
        if (address.isEmpty()) { reply({false, "Native activation requested, but Hyprland could not locate this process's window."}); return; }
        hyprctl(context, {"dispatch", QStringLiteral("hl.dsp.focus({ window = \"address:%1\" })").arg(address)},
                [reply](bool success, const QByteArray &result) {
            const auto response = result.trimmed();
            // hyprctl may return exit code zero for a compositor-side error.
            if (!success || response != "ok") reply({false, "Hyprland did not confirm window activation."});
            else reply({true, {}});
        });
    });
}
}

SingleInstance::Request SingleInstance::Request::fromArguments(const QStringList &references,
                                                              const QString &importArgument, bool importProvided)
{
    Request request;
    request.reference = references.join(QChar(' '));
    if (importProvided) request.importPath = QFileInfo(importArgument).absoluteFilePath();
    request.activationToken = qEnvironmentVariable("XDG_ACTIVATION_TOKEN");
    request.startupId = qEnvironmentVariable("DESKTOP_STARTUP_ID");
    return request;
}

SingleInstance::SingleInstance(const QString &dataDirectory, const QString &storageId,
                               const QString &runtimeDirectory, QObject *parent)
    : QObject(parent), m_lock(dataDirectory + '/' + storageId + ".lock")
{
    const auto canonical = QFileInfo(dataDirectory).canonicalFilePath();
    const auto hash = QCryptographicHash::hash((canonical + '/' + storageId).toUtf8(), QCryptographicHash::Sha256).toHex().left(32);
    m_endpoint = QDir(runtimeDirectory).filePath("omaword-" + QString::fromLatin1(hash) + ".socket");
    if (canonical.isEmpty() || runtimeDirectory.isEmpty()) m_error = QStringLiteral("Application data or user runtime directory is unavailable.");
    m_lock.setStaleLockTime(0);
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    m_server.setMaxPendingConnections(16);
    connect(&m_server, &QLocalServer::newConnection, this, &SingleInstance::acceptConnections);
}

SingleInstance::Role SingleInstance::start()
{
    if (!m_error.isEmpty()) return Role::Error;
    if (!m_lock.tryLock()) {
        if (m_lock.error() == QLockFile::LockFailedError) return Role::Secondary;
        m_error = QStringLiteral("Cannot acquire the application data lock (check folder permissions).");
        return Role::Error;
    }
    // Only the lock owner may clean up a socket left by a crashed primary.
    QLocalServer::removeServer(m_endpoint);
    if (!m_server.listen(m_endpoint)) {
        m_error = m_server.errorString();
        return Role::Error;
    }
    return Role::Primary;
}

void SingleInstance::setHandler(Handler handler)
{
    m_handler = std::move(handler);
    dispatch();
}

void SingleInstance::respond(QLocalSocket *socket, Result result)
{
    if (!socket || socket->state() != QLocalSocket::ConnectedState) return;
    const QJsonObject ack{{"version", 1}, {"ok", result.ok}, {"error", result.error.left(2048)}};
    socket->write(QJsonDocument(ack).toJson(QJsonDocument::Compact) + '\n');
    socket->disconnectFromServer();
}

void SingleInstance::acceptConnections()
{
    while (auto *socket = m_server.nextPendingConnection()) {
        if (m_connections >= 16) { respond(socket, {false, "Too many activation requests."}); socket->deleteLater(); continue; }
        ++m_connections;
        socket->setReadBufferSize(MaxFrame + 1);
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QObject::destroyed, this, [this] { --m_connections; });
        auto *timer = new QTimer(socket);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, socket, [this, socket] {
            respond(socket, {false, "Activation request timed out before completion."});
        });
        timer->start(HandoffTimeout + 500);
        auto buffer = std::make_shared<QByteArray>();
        auto received = std::make_shared<bool>(false);
        auto read = [this, socket, buffer, received] {
            if (*received) return;
            buffer->append(socket->read(MaxFrame + 1 - buffer->size()));
            const auto newline = buffer->indexOf('\n');
            if (buffer->size() > MaxFrame || (newline >= 0 && newline != buffer->size() - 1)) {
                *received = true;
                respond(socket, {false, "Activation frame is too large or contains extra data."});
            } else if (newline >= 0) {
                *received = true;
                Request request;
                if (!decode(buffer->left(newline), &request)) {
                    respond(socket, {false, "Malformed activation request."});
                    return;
                }
                // Disconnected startup clients must not accumulate indefinitely.
                m_pending.removeIf([](const Pending &pending) {
                    return !pending.socket || pending.socket->state() != QLocalSocket::ConnectedState;
                });
                m_pending.enqueue({socket, request});
                dispatch();
            }
        };
        connect(socket, &QLocalSocket::readyRead, socket, read);
        read();
    }
}

void SingleInstance::dispatch()
{
    if (!m_handler || m_dispatching) return;
    while (!m_pending.isEmpty()) {
        const auto pending = m_pending.dequeue();
        if (!pending.socket || pending.socket->state() != QLocalSocket::ConnectedState) continue;
        m_dispatching = true;
        auto replied = std::make_shared<bool>(false);
        QPointer<SingleInstance> self(this);
        m_handler(pending.request, [self, pending, replied](Result result) {
            if (!self || *replied) return;
            *replied = true;
            self->respond(pending.socket, result);
            self->m_dispatching = false;
            QTimer::singleShot(0, self, &SingleInstance::dispatch);
        });
        return;
    }
}

void SingleInstance::forward(const Request &request, Reply reply, int timeoutMs)
{
    const auto frame = QJsonDocument(encode(request)).toJson(QJsonDocument::Compact) + '\n';
    Request validated;
    if (frame.size() > MaxFrame || !decode(frame.trimmed(), &validated)) {
        reply({false, "Command-line activation arguments are invalid or too long."});
        return;
    }
    auto *job = new QObject(this);
    auto *socket = new QLocalSocket(job);
    socket->setReadBufferSize(MaxFrame + 1);
    auto *retry = new QTimer(job);
    auto *deadline = new QTimer(job);
    deadline->setSingleShot(true);
    auto sent = std::make_shared<bool>(false);
    auto done = std::make_shared<bool>(false);
    auto buffer = std::make_shared<QByteArray>();
    auto finish = [job, socket, retry, deadline, done, reply](Result result) {
        if (*done) return;
        *done = true;
        retry->stop(); deadline->stop(); socket->abort();
        reply(result);
        job->deleteLater();
    };
    connect(deadline, &QTimer::timeout, job, [finish, sent] {
        finish({false, *sent
            ? "The existing instance did not acknowledge the request. Its outcome is unknown; check the window before retrying an import."
            : "The data folder is locked, but the existing instance has no responding IPC server. It may still be starting or be an older version. Restart it to forward a reference or import; no arguments were forwarded."});
    });
    connect(socket, &QLocalSocket::connected, job, [socket, retry, sent, frame] {
        *sent = true;
        retry->stop();
        socket->write(frame);
    });
    auto read = [socket, buffer, finish, done] {
        if (*done) return;
        buffer->append(socket->read(MaxFrame + 1 - buffer->size()));
        const auto newline = buffer->indexOf('\n');
        if (buffer->size() > MaxFrame) { finish({false, "Oversized activation acknowledgement."}); return; }
        if (newline < 0) return;
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(buffer->left(newline), &error);
        const auto ack = document.object();
        if (newline != buffer->size() - 1 || error.error != QJsonParseError::NoError || !document.isObject()
            || ack.size() != 3 || ack.value("version") != QJsonValue(1) || !ack.value("ok").isBool()
            || !ack.value("error").isString() || (ack.value("ok").toBool() && !ack.value("error").toString().isEmpty())
            || (!ack.value("ok").toBool() && ack.value("error").toString().isEmpty())) {
            finish({false, "Malformed activation acknowledgement."}); return;
        }
        finish({ack.value("ok").toBool(), ack.value("error").toString()});
    };
    connect(socket, &QLocalSocket::readyRead, job, read);
    connect(socket, &QLocalSocket::disconnected, job, [read, finish, sent, done] {
        read();
        if (*sent && !*done) finish({false, "The existing instance disconnected without acknowledging the request; its outcome is unknown."});
    });
    const auto endpoint = m_endpoint;
    auto attempt = [socket, endpoint, sent, done] {
        if (!*done && !*sent && socket->state() == QLocalSocket::UnconnectedState)
            socket->connectToServer(endpoint);
    };
    connect(retry, &QTimer::timeout, job, attempt);
    retry->start(50);
    deadline->start(qBound(1, timeoutMs, HandoffTimeout));
    attempt();
}

void SingleInstance::activateWindow(QWindow *window, const Request &request, QObject *context, Reply reply)
{
    if (!window) { reply({false, "The existing window is unavailable."}); return; }
    if (window->windowState() == Qt::WindowMinimized) window->showNormal();
    else window->show();
    window->raise();
    // Public Qt has no setter for an incoming Wayland activation token. Changing
    // the environment after platform initialization does not apply that token.
    // Preserve tokens in IPC, request native activation, and use PID-scoped
    // compositor activation on Hyprland (including another workspace).
    Q_UNUSED(request);
    window->requestActivate();
    if (qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")) { reply({true, {}}); return; }
    focusHyprland(context, QCoreApplication::applicationPid(), reply);
}
