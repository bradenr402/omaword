#include "versioncatalog.h"
#include "aoimporter.h"
#include <QFutureWatcher>

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
#include <QTemporaryDir>
#include <QtConcurrent/QtConcurrentRun>
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
    if (m_dependencies.dataDirectory.isEmpty())
        m_dependencies.dataDirectory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
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
    Q_INIT_RESOURCE(core);
    QFile saved(QDir(m_dependencies.dataDirectory).filePath(QStringLiteral("ao-catalog.json")));
    if (saved.exists()) {
        QString error;
        if (saved.open(QIODevice::ReadOnly) && saved.size() <= 8 * 1024 * 1024
            && parseEntries(saved.readAll(), &m_entries, &error)) return;
        m_error = tr("Saved AO catalog is invalid; using the bundled catalog. %1").arg(error);
    }
    QFile source(QStringLiteral(":/resources/catalog.json"));
    if (!source.open(QIODevice::ReadOnly) || !parseEntries(source.readAll(), &m_entries, &m_catalogError)) {
        m_catalogError = tr("The bundled AO catalog could not be loaded. %1").arg(m_catalogError);
        m_error = m_catalogError;
    }
}

bool VersionCatalog::parseEntries(const QByteArray &bytes, QVariantList *entries, QString *error) const
{
    const auto document = QJsonDocument::fromJson(bytes);
    const auto translations = document.object().value(QStringLiteral("translations"));
    if (!document.isObject() || !translations.isArray() || translations.toArray().isEmpty()
        || translations.toArray().size() > 10000) {
        *error = tr("Expected a nonempty AO translations catalog."); return false;
    }
    QVariantList result;
    QSet<QString> seen;
    for (const auto &value : translations.toArray()) {
        const QJsonObject object = value.toObject();
        QVariantMap entry = object.toVariantMap();
        const QString id = entry.value(QStringLiteral("id")).toString();
        const QString path = QStringLiteral("/api/%1/complete.json").arg(id);
        const QString link = entry.value(QStringLiteral("completeTranslationApiLink")).toString();
        bool valid = AoImporter::validId(id) && !seen.contains(id);
        for (const char *key : {"name", "language", "textDirection"})
            valid = valid && object.value(QLatin1String(key)).isString() && !object.value(QLatin1String(key)).toString().isEmpty();
        valid = valid && (link.isEmpty() || link == path || link == QStringLiteral("https://bible.helloao.org") + path);
        for (const char *key : {"numberOfBooks", "totalNumberOfChapters", "totalNumberOfVerses"}) {
            const auto count = object.value(QLatin1String(key));
            valid = valid && count.isDouble() && count.toDouble() >= 0 && count.toDouble() <= 1000000
                && count.toDouble() == count.toInt();
        }
        if (!valid) { *error = tr("Invalid AO catalog entry: %1").arg(id); return false; }
        seen.insert(id);
        QString abbreviation = entry.value(QStringLiteral("shortName")).toString();
        if (abbreviation.isEmpty()) abbreviation = id;
        if (id == QStringLiteral("eng_net")) abbreviation = QStringLiteral("NET");
        if (id == QStringLiteral("eng_kjv")) abbreviation = QStringLiteral("KJV");
        entry.insert(QStringLiteral("abbreviation"), abbreviation);
        entry.insert(QStringLiteral("url"), link.isEmpty() ? QString() : QStringLiteral("https://bible.helloao.org") + path);
        QString details = entry.value(QStringLiteral("licenseUrl")).toString();
        if (details.isEmpty()) details = entry.value(QStringLiteral("website")).toString();
        const QUrl detailsUrl(details);
        if (detailsUrl.scheme() != QStringLiteral("https") && detailsUrl.scheme() != QStringLiteral("http")) details.clear();
        entry.insert(QStringLiteral("detailsUrl"), details);
        QStringList notices;
        for (const char *key : {"licenseNotice", "licenseNotes", "license"}) {
            const auto notice = object.value(QLatin1String(key));
            if (notice.isString()) notices.append(notice.toString());
            else if (notice.isObject()) notices.append(QString::fromUtf8(QJsonDocument(notice.toObject()).toJson(QJsonDocument::Compact)));
        }
        entry.insert(QStringLiteral("copyright"), notices.join(QStringLiteral("\n\n")));
        const int count = entry.value(QStringLiteral("numberOfBooks")).toInt();
        QString coverage = count < 27 ? tr("Portion") : count == 27 ? tr("27-book edition; book set unverified")
            : count == 39 ? tr("39-book edition; book set unverified") : tr("Book set unverified");
        coverage += tr(" · %1 books · %2 chapters · %3 source verses").arg(count)
            .arg(entry.value(QStringLiteral("totalNumberOfChapters")).toInt()).arg(entry.value(QStringLiteral("totalNumberOfVerses")).toInt());
        const int apocrypha = entry.value(QStringLiteral("numberOfApocryphalBooks")).toInt();
        if (apocrypha) coverage += tr(" · %1 apocryphal books").arg(apocrypha);
        entry.insert(QStringLiteral("description"), coverage);
        entry.insert(QStringLiteral("coverage"), count < 27 ? QStringLiteral("portion") : QStringLiteral("unverified"));
        QString reason;
        if (entry.value(QStringLiteral("language")).toString() != QStringLiteral("eng")) reason = tr("Language support not yet verified; English editions only.");
        if (entry.value(QStringLiteral("textDirection")).toString() != QStringLiteral("ltr")) reason = tr("Right-to-left rendering is not yet supported.");
        if (link.isEmpty()) reason = tr("AO does not advertise a standard complete download.");
        entry.insert(QStringLiteral("compatibilityReason"), reason);
        result.append(entry);
    }
    *entries = result;
    return true;
}

void VersionCatalog::download(const QString &id)
{
    if (!AoImporter::validId(id)) { setError(tr("Invalid AO translation ID.")); return; }
    start(id);
}

void VersionCatalog::refresh()
{
    start({});
}

void VersionCatalog::start(const QString &id)
{
    if (m_busy || m_transitioning)
        return;
    QUrl url;
    m_refreshing = id.isEmpty();
    m_requestedId = id;
    if (m_refreshing) url = QUrl(QStringLiteral("https://bible.helloao.org/api/available_translations.json"));
    for (const auto &value : m_entries) {
        const auto entry = value.toMap();
        if (entry.value("id").toString() == id) {
            if (!entry.value(QStringLiteral("compatibilityReason")).toString().isEmpty()) {
                setError(entry.value(QStringLiteral("compatibilityReason")).toString()); return;
            }
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
    m_body.clear();
    ++m_transfer;
    const QString directory = m_refreshing ? m_dependencies.dataDirectory
        : QDir(m_dependencies.cacheDirectory).filePath(QStringLiteral("catalog"));
    m_destination = QDir(directory).filePath(m_refreshing ? QStringLiteral("ao-catalog.json") : id + QStringLiteral("_complete.json"));
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
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    request.setMaximumRedirectsAllowed(10);
    request.setRawHeader("Accept-Encoding", "identity");
    request.setRawHeader("Accept", "application/json");
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
        m_body.append(chunk);
        if (m_refreshing && m_body.size() > 8 * 1024 * 1024) { fail(tr("Catalog exceeds the size limit.")); return; }
        m_stallTimer.start(m_dependencies.stallTimeoutMs);
        const qint64 total = m_reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        if (total > 0) setProgress(std::min(qreal(0.99), qreal(m_received) / total));
    }
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
    QVariantList refreshed;
    QString validationError;
    if (m_refreshing) {
        if (!parseEntries(m_body, &refreshed, &validationError)) { fail(validationError); return; }
    } else {
        const auto root = QJsonDocument::fromJson(m_body).object();
        if (root.value(QStringLiteral("translation")).toObject().value(QStringLiteral("id")).toString() != m_requestedId
            || !root.value(QStringLiteral("books")).isArray() || root.value(QStringLiteral("books")).toArray().isEmpty()) {
            fail(tr("The response is not the requested AO complete translation.")); return;
        }
        for (const auto &book : root.value(QStringLiteral("books")).toArray()) {
            const auto object = book.toObject();
            if (!object.value(QStringLiteral("id")).isString() || !object.value(QStringLiteral("chapters")).isArray()
                || object.value(QStringLiteral("chapters")).toArray().isEmpty()) {
                fail(tr("The AO download contains invalid book data.")); return;
            }
        }
        m_stallTimer.stop();
        const auto transfer = m_transfer;
        auto *watcher = new QFutureWatcher<AoImporter::Result>(this);
        connect(watcher, &QFutureWatcher<AoImporter::Result>::finished, this, [this, watcher, transfer] {
            const auto result = watcher->result(); watcher->deleteLater();
            if (!m_busy || transfer != m_transfer) return;
            if (!result.ok) { fail(result.error); return; }
            if (result.edition.id != m_requestedId) { fail(tr("The AO response changed translation identity.")); return; }
            publish();
        });
        watcher->setFuture(QtConcurrent::run([bytes = m_body] {
            QTemporaryDir staging;
            QFile source(staging.filePath(QStringLiteral("complete.json")));
            if (!staging.isValid() || !source.open(QIODevice::WriteOnly) || source.write(bytes) != bytes.size()) {
                AoImporter::Result result; result.error = QStringLiteral("Cannot stage AO download validation."); return result;
            }
            source.close();
            return AoImporter::importFile(source.fileName(), staging.filePath(QStringLiteral("validated.sqlite")));
        }));
        return;
    }
    publish(refreshed);
}

void VersionCatalog::publish(const QVariantList &refreshed)
{
    if (!m_file->commit()) {
        fail(tr("Cannot save download: %1").arg(m_file->errorString()));
        return;
    }
    const QUrl file = QUrl::fromLocalFile(m_destination);
    const bool refreshedCatalog = m_refreshing;
    if (refreshedCatalog) { m_entries = refreshed; emit entriesChanged(); }
    m_transitioning = true;
    releaseTransfer();
    setProgress(1);
    m_busy = false;
    emit busyChanged();
    m_transitioning = false;
    if (!refreshedCatalog) emit downloadFinished(file);
}

void VersionCatalog::releaseTransfer()
{
    ++m_transfer;
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
