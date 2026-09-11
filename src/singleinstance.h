#pragma once

#include <QLocalServer>
#include <QLockFile>
#include <QPointer>
#include <QQueue>
#include <functional>

class QWindow;
class QLocalSocket;

// The legacy data lock is authoritative; the socket is only a handoff channel.
class SingleInstance : public QObject
{
    Q_OBJECT
public:
    struct Request {
        QString reference;
        QString importPath;
        QString activationToken;
        QString startupId;
        static Request fromArguments(const QStringList &references, const QString &importArgument,
                                     bool importProvided);
    };
    struct Result { bool ok; QString error; };
    using Reply = std::function<void(Result)>;
    using Handler = std::function<void(const Request &, Reply)>;
    enum class Role { Primary, Secondary, Error };
    static constexpr int MaxFrame = 16384;
    static constexpr int HandoffTimeout = 3000;

    SingleInstance(const QString &dataDirectory, const QString &storageId,
                   const QString &runtimeDirectory, QObject *parent = nullptr);
    Role start();
    QString errorString() const { return m_error; }
    QString endpoint() const { return m_endpoint; }
    // Install only after QML and the initial CLI action have been initialized.
    // Reply acknowledges acceptance (an import may still be running).
    void setHandler(Handler handler);
    void forward(const Request &request, Reply reply, int timeoutMs = HandoffTimeout);
    static void activateWindow(QWindow *window, const Request &request,
                               QObject *context, Reply reply);

private:
    struct Pending { QPointer<QLocalSocket> socket; Request request; };
    void acceptConnections();
    void dispatch();
    void respond(QLocalSocket *socket, Result result);
    QLockFile m_lock;
    QLocalServer m_server;
    QString m_endpoint, m_error;
    QQueue<Pending> m_pending;
    Handler m_handler;
    bool m_dispatching = false;
    int m_connections = 0;
};
