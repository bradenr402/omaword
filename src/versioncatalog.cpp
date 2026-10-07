#include "versioncatalog.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QtEndian>
#include <algorithm>

VersionCatalog::VersionCatalog(QObject *parent)
    : VersionCatalog(Dependencies{}, parent)
{
}

VersionCatalog::VersionCatalog(const Dependencies &dependencies, QObject *parent)
    : QObject(parent), m_dependencies(dependencies),
      m_manager(dependencies.manager ? dependencies.manager : new QNetworkAccessManager(this))
{
    if (m_dependencies.cacheDirectory.isEmpty())
        m_dependencies.cacheDirectory = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    m_stallTimer.setSingleShot(true);
    m_totalTimer.setSingleShot(true);
    connect(&m_stallTimer, &QTimer::timeout, this, [this] {
        fail(tr("Download stalled. Please retry."));
    });
    connect(&m_totalTimer, &QTimer::timeout, this, [this] {
        fail(tr("Download timed out. Please retry."));
    });
    loadEntries();
}

VersionCatalog::~VersionCatalog()
{
    releaseTransfer();
}

void VersionCatalog::loadEntries()
{
    QFile source(QStringLiteral(":/resources/catalog.json"));
    QJsonParseError parseError;
    const QJsonDocument document = source.open(QIODevice::ReadOnly)
        ? QJsonDocument::fromJson(source.readAll(), &parseError) : QJsonDocument();
    const QSet<QString> allowed = {"engnet", "eng-kjv2006", "eng-kjv", "engojb",
                                   "engylt", "eng-asv", "engbsb"};
    QSet<QString> seen;
    bool valid = document.isArray() && document.array().size() == allowed.size();
    for (const auto &value : document.array()) {
        const QVariantMap entry = value.toObject().toVariantMap();
        const QString id = entry.value("id").toString();
        valid = valid && allowed.contains(id) && !seen.contains(id)
            && QRegularExpression(QStringLiteral("^[a-z0-9-]+$")).match(id).hasMatch()
            && entry.value("url").toString() == QStringLiteral("https://ebible.org/Scriptures/%1_usfm.zip").arg(id)
            && entry.value("detailsUrl").toString() == QStringLiteral("https://ebible.org/find/details.php?id=%1&all=1").arg(id);
        for (const char *key : {"name", "abbreviation", "copyright", "description", "year", "license"})
            valid = valid && !entry.value(QLatin1String(key)).toString().trimmed().isEmpty();
        // The download's size in bytes, and what the app shows of the edition.
        const QJsonObject object = value.toObject();
        valid = valid && object.value(QStringLiteral("size")).isDouble() && object.value(QStringLiteral("size")).toDouble() > 0
            && object.value(QStringLiteral("features")).isArray();
        static const QSet<QString> features = {"footnotes", "headings", "wordsOfJesus", "apocrypha", "parallels"};
        for (const auto &feature : object.value(QStringLiteral("features")).toArray())
            valid = valid && features.contains(feature.toString());
        seen.insert(id);
        m_entries.append(entry);
    }
    if (!valid) {
        m_entries.clear();
        m_catalogError = tr("The bundled version catalog could not be loaded.");
        m_error = m_catalogError;
    }
}

void VersionCatalog::download(const QString &id)
{
    if (m_busy || m_transitioning)
        return;
    QUrl url;
    for (const auto &value : m_entries) {
        const auto entry = value.toMap();
        if (entry.value("id").toString() == id) {
            url = QUrl(entry.value("url").toString());
            break;
        }
    }
    if (url.isEmpty()) {
        setError(m_catalogError.isEmpty() ? tr("Unknown catalog version.") : m_catalogError);
        return;
    }
    if (m_dependencies.urlTransform)
        url = m_dependencies.urlTransform(url);
    if (!url.isValid() || url.host().isEmpty()
        || (url.scheme() != "https" && !(m_dependencies.urlTransform && url.scheme() == "http"))) {
        setError(tr("The download URL must use HTTPS."));
        return;
    }
    if (m_dependencies.maximumBytes < 52 || m_dependencies.stallTimeoutMs <= 0
        || m_dependencies.totalTimeoutMs <= 0) {
        setError(tr("Invalid download limits."));
        return;
    }
    m_transitioning = true;
    setError({});
    setProgress(0);
    m_received = 0;
    m_prefix.clear();
    m_tail.clear();
    const QString directory = QDir(m_dependencies.cacheDirectory).filePath(QStringLiteral("catalog"));
    m_destination = QDir(directory).filePath(id + QStringLiteral("_usfm.zip"));
    m_file = std::make_unique<QSaveFile>(m_destination);
    m_file->setDirectWriteFallback(false);
    if (m_dependencies.cacheDirectory.isEmpty() || !QDir().mkpath(directory)
        || !m_file->open(QIODevice::WriteOnly)) {
        const QString message = tr("Cannot create the download cache: %1").arg(m_file->errorString());
        m_file.reset();
        setError(message);
        m_transitioning = false;
        return;
    }
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setMaximumRedirectsAllowed(10);
    request.setRawHeader("Accept-Encoding", "identity");
    request.setRawHeader("Accept", "application/zip, application/octet-stream");
    m_reply = m_manager->get(request);
    m_reply->setReadBufferSize(256 * 1024);
    QNetworkReply *reply = m_reply;
    connect(reply, &QNetworkReply::metaDataChanged, this, [this, reply] {
        if (m_reply == reply) checkHeaders();
    });
    connect(reply, &QIODevice::readyRead, this, [this, reply] {
        if (m_reply == reply) consume();
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this, reply](qint64 received, qint64 total) {
        if (m_reply != reply) return;
        if (received > m_dependencies.maximumBytes || total > m_dependencies.maximumBytes) {
            fail(tr("Download exceeds the size limit."));
            return;
        }
        if (total > 0) setProgress(std::clamp(qreal(received) / total, qreal(0), qreal(0.99)));
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        if (m_reply == reply) complete();
    });
    m_stallTimer.start(m_dependencies.stallTimeoutMs);
    m_totalTimer.start(m_dependencies.totalTimeoutMs);
    m_busy = true;
    emit busyChanged();
    m_transitioning = false;
}

bool VersionCatalog::checkHeaders()
{
    if (!m_reply) return false;
    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 400 || (status >= 200 && status < 300 && status != 200)) {
        fail(tr("Download failed (HTTP %1).").arg(status));
        return false;
    }
    const qint64 length = m_reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
    if (length > m_dependencies.maximumBytes) {
        fail(tr("Download exceeds the size limit."));
        return false;
    }
    return true;
}

void VersionCatalog::consume()
{
    QNetworkReply *reply = m_reply;
    if (!checkHeaders()) return;
    // Redirect response bodies are not part of the archive.
    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 300 && status < 400) {
        while (m_reply->bytesAvailable() > 0) m_reply->read(64 * 1024);
        return;
    }
    while (m_reply == reply && m_reply && m_reply->bytesAvailable() > 0) {
        const QByteArray chunk = m_reply->read(64 * 1024);
        if (chunk.isEmpty()) break;
        if (chunk.size() > m_dependencies.maximumBytes - m_received) {
            fail(tr("Download exceeds the size limit."));
            return;
        }
        if (m_file->write(chunk) != chunk.size()) {
            fail(tr("Cannot save download: %1").arg(m_file->errorString()));
            return;
        }
        m_received += chunk.size();
        if (m_prefix.size() < 4) m_prefix.append(chunk.left(4 - m_prefix.size()));
        m_tail.append(chunk);
        if (m_tail.size() > 65557) m_tail = m_tail.right(65557);
        m_stallTimer.start(m_dependencies.stallTimeoutMs);
        const qint64 total = m_reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        if (total > 0) setProgress(std::min(qreal(0.99), qreal(m_received) / total));
    }
}

bool VersionCatalog::hasZipEnvelope() const
{
    // Transport screening only; the importer validates archive contents and USFM.
    if (m_received < 52 || m_prefix != QByteArray("PK\x03\x04", 4)) return false;
    for (qsizetype i = m_tail.size() - 22; i >= 0; --i) {
        if (m_tail.mid(i, 4) != QByteArray("PK\x05\x06", 4)) continue;
        const auto *record = reinterpret_cast<const uchar *>(m_tail.constData() + i);
        if (i + 22 + qFromLittleEndian<quint16>(record + 20) == m_tail.size()
            && qFromLittleEndian<quint16>(record + 10) > 0)
            return true;
    }
    return false;
}

void VersionCatalog::complete()
{
    QNetworkReply *reply = m_reply;
    consume();
    if (!m_reply || m_reply != reply) return;
    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (m_reply->error() != QNetworkReply::NoError) {
        fail(tr("Download failed: %1").arg(m_reply->errorString()));
        return;
    }
    if (status != 200) {
        fail(tr("Download failed (HTTP %1).").arg(status));
        return;
    }
    const auto lengthHeader = m_reply->header(QNetworkRequest::ContentLengthHeader);
    if (lengthHeader.isValid() && lengthHeader.toLongLong() != m_received) {
        fail(tr("The download is incomplete. Please retry."));
        return;
    }
    if (!hasZipEnvelope()) {
        fail(tr("The download is not a nonempty, complete ZIP archive."));
        return;
    }
    if (!m_file->commit()) {
        fail(tr("Cannot save download: %1").arg(m_file->errorString()));
        return;
    }
    const QUrl file = QUrl::fromLocalFile(m_destination);
    m_transitioning = true;
    releaseTransfer();
    setProgress(1);
    m_busy = false;
    emit busyChanged();
    m_transitioning = false;
    emit downloadFinished(file);
}

void VersionCatalog::releaseTransfer()
{
    m_stallTimer.stop();
    m_totalTimer.stop();
    if (m_reply) {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        disconnect(reply, nullptr, this, nullptr);
        if (!reply->isFinished()) reply->abort();
        reply->deleteLater();
    }
    if (m_file) {
        m_file->cancelWriting();
        m_file.reset();
    }
}

void VersionCatalog::fail(const QString &message)
{
    m_transitioning = true;
    const bool wasBusy = m_busy;
    releaseTransfer();
    m_busy = false;
    setError(message);
    setProgress(0);
    if (wasBusy) emit busyChanged();
    m_transitioning = false;
}

void VersionCatalog::cancel()
{
    if (m_busy) fail({});
}

void VersionCatalog::clearError()
{
    setError({});
}

void VersionCatalog::setError(const QString &message)
{
    if (m_error == message) return;
    m_error = message;
    emit errorChanged();
}

void VersionCatalog::setProgress(qreal value)
{
    if (m_progress == value) return;
    m_progress = value;
    emit progressChanged();
}
