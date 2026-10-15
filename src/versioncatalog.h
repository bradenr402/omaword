#pragma once

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <functional>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QSaveFile;

class VersionCatalog : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(qreal progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)

public:
    explicit VersionCatalog(QObject *parent = nullptr);
    // Explicit dependency injection for tests. A supplied manager must outlive this object.
    // Only this constructor can transform catalog URLs (e.g. to a local HTTP server).
    struct Dependencies {
        QNetworkAccessManager *manager = nullptr;
        std::function<QUrl(const QUrl &)> urlTransform;
        QString cacheDirectory;
        QString dataDirectory;
        qint64 maximumBytes = 64 * 1024 * 1024;
        int stallTimeoutMs = 30000;
        int totalTimeoutMs = 300000;
    };
    explicit VersionCatalog(const Dependencies &dependencies, QObject *parent = nullptr);
    ~VersionCatalog() override;

    QVariantList entries() const { return m_entries; }
    bool busy() const { return m_busy; }
    qreal progress() const { return m_progress; }
    QString error() const { return m_error; }

    Q_INVOKABLE void download(const QString &id);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void clearError();

signals:
    void busyChanged();
    void progressChanged();
    void errorChanged();
    void downloadFinished(QUrl file);
    void entriesChanged();

private:
    void loadEntries();
    bool parseEntries(const QByteArray &bytes, QVariantList *entries, QString *error) const;
    void start(const QString &id);
    bool checkHeaders();
    void consume();
    void complete();
    void publish(const QVariantList &refreshed = {});
    void fail(const QString &message);
    void releaseTransfer();
    void setProgress(qreal value);
    void setError(const QString &message);

    Dependencies m_dependencies;
    QNetworkAccessManager *m_manager;
    QPointer<QNetworkReply> m_reply;
    std::unique_ptr<QSaveFile> m_file;
    QTimer m_stallTimer, m_totalTimer;
    QVariantList m_entries;
    QString m_error, m_catalogError, m_destination;
    QByteArray m_body;
    QString m_requestedId;
    bool m_refreshing = false;
    quint64 m_transfer = 0;
    qint64 m_received = 0;
    qreal m_progress = 0;
    bool m_busy = false;
    bool m_transitioning = false;
};
