#include "testenvironment.h"
#include <QtTest>

#include "versioncatalog.h"

#include <QDir>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtEndian>
#include <cstring>

namespace {
void append16(QByteArray &bytes, quint16 value)
{
    char data[2];
    qToLittleEndian(value, data);
    bytes.append(data, 2);
}

void append32(QByteArray &bytes, quint32 value)
{
    char data[4];
    qToLittleEndian(value, data);
    bytes.append(data, 4);
}

// A complete stored ZIP containing only original test text.
QByteArray fixture()
{
    const QByteArray name("01GENtest.usfm");
    const QByteArray text("\\id GEN\n\\h Test\n\\c 1\n\\p\n\\v 1 Original fixture verse.\n");
    quint32 crc = 0xffffffff;
    for (unsigned char ch : text) {
        crc ^= ch;
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    }
    crc ^= 0xffffffff;
    QByteArray zip("PK\x03\x04", 4);
    append16(zip, 20);
    for (int i = 0; i < 4; ++i) append16(zip, 0);
    append32(zip, crc);
    append32(zip, text.size());
    append32(zip, text.size());
    append16(zip, name.size());
    append16(zip, 0);
    zip += name + text;
    const quint32 offset = zip.size();
    zip.append("PK\x01\x02", 4);
    append16(zip, 20);
    append16(zip, 20);
    for (int i = 0; i < 4; ++i) append16(zip, 0);
    append32(zip, crc);
    append32(zip, text.size());
    append32(zip, text.size());
    append16(zip, name.size());
    for (int i = 0; i < 4; ++i) append16(zip, 0);
    append32(zip, 0);
    append32(zip, 0);
    zip += name;
    const quint32 directorySize = zip.size() - offset;
    zip.append("PK\x05\x06", 4);
    append16(zip, 0);
    append16(zip, 0);
    append16(zip, 1);
    append16(zip, 1);
    append32(zip, directorySize);
    append32(zip, offset);
    append16(zip, 0);
    return zip;
}

struct Script {
    QByteArray body = fixture();
    int status = 200;
    qint64 length = -1;
    QNetworkReply::NetworkError error = QNetworkReply::NoError;
    int chunkSize = 64;
    int intervalMs = 0;
    bool hold = false;
};

class FakeReply : public QNetworkReply
{
public:
    FakeReply(const QNetworkRequest &request, Script script, QObject *parent)
        : QNetworkReply(parent), m_script(std::move(script))
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        open(QIODevice::ReadOnly);
        QTimer::singleShot(0, this, [this] {
            if (isFinished()) return;
            setAttribute(QNetworkRequest::HttpStatusCodeAttribute, m_script.status);
            if (m_script.length >= 0) setHeader(QNetworkRequest::ContentLengthHeader, m_script.length);
            emit metaDataChanged();
            if (!isFinished() && !m_script.hold) step();
        });
    }

    void abort() override
    {
        if (isFinished()) return;
        aborted = true;
        setError(OperationCanceledError, "Canceled");
        setFinished(true);
        emit errorOccurred(OperationCanceledError);
        emit finished();
    }
    qint64 bytesAvailable() const override { return m_buffer.size() + QNetworkReply::bytesAvailable(); }
    bool aborted = false;

protected:
    qint64 readData(char *data, qint64 maximum) override
    {
        const qint64 count = qMin(maximum, qint64(m_buffer.size()));
        if (!count) return isFinished() ? -1 : 0;
        std::memcpy(data, m_buffer.constData(), size_t(count));
        m_buffer.remove(0, count);
        return count;
    }

private:
    void step()
    {
        if (isFinished()) return;
        if (m_offset < m_script.body.size()) {
            const QByteArray chunk = m_script.body.mid(m_offset, m_script.chunkSize);
            m_offset += chunk.size();
            m_buffer += chunk;
            emit readyRead();
            if (isFinished()) return;
            emit downloadProgress(m_offset, m_script.length);
            QTimer::singleShot(m_script.intervalMs, this, [this] { step(); });
            return;
        }
        if (m_script.error != NoError) {
            setError(m_script.error, "Scripted connection failure");
            emit errorOccurred(m_script.error);
        }
        setFinished(true);
        emit finished();
    }
    Script m_script;
    QByteArray m_buffer;
    qsizetype m_offset = 0;
};

class FakeManager : public QNetworkAccessManager
{
public:
    Script script;
    QNetworkRequest lastRequest;
    QPointer<FakeReply> lastReply;
    int requests = 0;
protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override
    {
        ++requests;
        lastRequest = request;
        lastReply = new FakeReply(request, script, this);
        return lastReply;
    }
};

VersionCatalog::Dependencies dependencies(FakeManager &manager, const QTemporaryDir &directory)
{
    VersionCatalog::Dependencies result;
    result.manager = &manager;
    result.cacheDirectory = directory.path();
    result.stallTimeoutMs = 500;
    result.totalTimeoutMs = 1500;
    return result;
}

QString destination(const QTemporaryDir &directory)
{
    return directory.filePath("catalog/engnet_usfm.zip");
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool seedCache(const QTemporaryDir &directory)
{
    if (!QDir(directory.path()).mkpath("catalog")) return false;
    QFile file(destination(directory));
    return file.open(QIODevice::WriteOnly) && file.write("previous cache") == 14;
}
}

class CatalogTest : public QObject
{
    Q_OBJECT
private slots:
    void metadataAndAllowlist()
    {
        QObject parent;
        VersionCatalog bundled(&parent);
        QCOMPARE(bundled.parent(), &parent);
        QVERIFY2(bundled.error().isEmpty(), qPrintable(bundled.error()));
        QCOMPARE(bundled.entries().size(), 7);
        const QStringList ids = {"engnet", "eng-kjv2006", "eng-kjv", "engojb", "engylt", "eng-asv", "engbsb"};
        const QStringList abbreviations = {"NET", "KJV", "KJV+", "OJB", "YLT", "ASV", "BSB"};
        for (int i = 0; i < ids.size(); ++i) {
            const auto entry = bundled.entries()[i].toMap();
            QCOMPARE(entry.size(), 11);
            QCOMPARE(entry.value("id").toString(), ids[i]);
            QCOMPARE(entry.value("abbreviation").toString(), abbreviations[i]);
            QCOMPARE(entry.value("url").toString(), "https://ebible.org/Scriptures/" + ids[i] + "_usfm.zip");
            QCOMPARE(entry.value("detailsUrl").toString(), "https://ebible.org/find/details.php?id=" + ids[i] + "&all=1");
            QVERIFY(!entry.value("name").toString().isEmpty());
            QVERIFY(!entry.value("copyright").toString().isEmpty());
            for (const char *key : {"description", "year", "license"}) QVERIFY2(!entry.value(key).toString().isEmpty(), key);
            QVERIFY(entry.value("size").toDouble() > 1000000);
            QVERIFY(entry.value("features").canConvert<QVariantList>());
        }
        // What each edition carries, as measured from its source.
        auto features = [&](int index) { return bundled.entries()[index].toMap()["features"].toStringList(); };
        QCOMPARE(features(0), QStringList({"headings"}));
        QCOMPARE(features(2), QStringList({"footnotes", "wordsOfJesus", "apocrypha"}));
        QVERIFY(features(4).isEmpty());
        QCOMPARE(bundled.entries()[3].toMap()["license"].toString(), QString("CC BY 4.0"));
        QVERIFY(bundled.entries()[0].toMap()["copyright"].toString().contains("© 1996-2016 Biblical Studies Press, L. L. C."));
        QVERIFY(bundled.entries()[3].toMap()["copyright"].toString().contains("CC BY 4.0"));
        QCOMPARE(bundled.entries()[6].toMap()["name"].toString(), "Berean Standard Bible");

        QTemporaryDir directory;
        FakeManager manager;
        VersionCatalog catalog(dependencies(manager, directory));
        for (const QString &id : {QString("../engnet"), QString("https://example.com/a.zip"), QString("unknown"), QString()}) {
            catalog.download(id);
            QVERIFY(!catalog.error().isEmpty());
            QVERIFY(!catalog.busy());
            catalog.clearError();
            QVERIFY(catalog.error().isEmpty());
        }
        QCOMPARE(manager.requests, 0);
    }

    void streamingAtomicSuccess()
    {
        QTemporaryDir directory;
        QVERIFY(seedCache(directory));
        FakeManager manager;
        manager.script.length = manager.script.body.size();
        VersionCatalog catalog(dependencies(manager, directory));
        QSignalSpy finished(&catalog, &VersionCatalog::downloadFinished);
        QSignalSpy progress(&catalog, &VersionCatalog::progressChanged);
        QSignalSpy busy(&catalog, &VersionCatalog::busyChanged);
        bool partialProgress = false;
        connect(&catalog, &VersionCatalog::progressChanged, this, [&] {
            if (catalog.progress() > 0 && catalog.progress() < 1) {
                partialProgress = true;
                QCOMPARE(readFile(destination(directory)), QByteArray("previous cache"));
            }
        });
        catalog.download("engnet");
        QVERIFY(catalog.busy());
        catalog.download("engbsb");
        QCOMPARE(manager.requests, 1);
        QCOMPARE(manager.lastRequest.url(), QUrl("https://ebible.org/Scriptures/engnet_usfm.zip"));
        QCOMPARE(manager.lastRequest.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(),
                 int(QNetworkRequest::NoLessSafeRedirectPolicy));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 1000);
        QVERIFY(partialProgress);
        QVERIFY(progress.size() > 1);
        QCOMPARE(busy.size(), 2);
        QCOMPARE(catalog.progress(), 1.0);
        QVERIFY(!catalog.busy());
        QVERIFY(catalog.error().isEmpty());
        QCOMPARE(finished[0][0].toUrl(), QUrl::fromLocalFile(destination(directory)));
        QCOMPARE(readFile(destination(directory)), fixture());
        QCOMPARE(QDir(directory.filePath("catalog")).entryList(QDir::Files | QDir::Hidden).size(), 1);
    }

    void failedTransfers_data()
    {
        QTest::addColumn<int>("scenario");
        QTest::addColumn<QString>("message");
        QTest::newRow("404") << 0 << "HTTP 404";
        QTest::newRow("network") << 1 << "connection failure";
        QTest::newRow("html") << 2 << "ZIP";
        QTest::newRow("empty") << 3 << "ZIP";
        QTest::newRow("truncated-zip") << 4 << "ZIP";
        QTest::newRow("empty-zip") << 5 << "ZIP";
        QTest::newRow("oversize-header") << 6 << "size limit";
        QTest::newRow("oversize-stream") << 7 << "size limit";
        QTest::newRow("length-mismatch") << 8 << "incomplete";
        QTest::newRow("partial-response") << 9 << "HTTP 206";
        QTest::newRow("https-downgrade") << 10 << "connection failure";
    }

    void failedTransfers()
    {
        QFETCH(int, scenario);
        QFETCH(QString, message);
        QTemporaryDir directory;
        QVERIFY(seedCache(directory));
        FakeManager manager;
        auto config = dependencies(manager, directory);
        config.maximumBytes = 1024;
        switch (scenario) {
        case 0: manager.script.status = 404; break;
        case 1: manager.script.error = QNetworkReply::RemoteHostClosedError; break;
        case 2: manager.script.body = "<html>Download unavailable</html>"; break;
        case 3: manager.script.body.clear(); break;
        case 4: manager.script.body.chop(22); break;
        case 5: manager.script.body = QByteArray("PK\x05\x06", 4) + QByteArray(18, '\0'); break;
        case 6: manager.script.length = 1025; break;
        case 7: manager.script.body += QByteArray(1025, 'x'); manager.script.chunkSize = 200; break;
        case 8: manager.script.length = manager.script.body.size() + 1; break;
        case 9: manager.script.status = 206; break;
        case 10: manager.script.error = QNetworkReply::InsecureRedirectError; break;
        }
        VersionCatalog catalog(config);
        QSignalSpy finished(&catalog, &VersionCatalog::downloadFinished);
        catalog.download("engnet");
        QTRY_VERIFY_WITH_TIMEOUT(!catalog.busy(), 1000);
        QVERIFY2(catalog.error().contains(message), qPrintable(catalog.error()));
        QCOMPARE(finished.size(), 0);
        QCOMPARE(readFile(destination(directory)), QByteArray("previous cache"));
        QCOMPARE(catalog.progress(), 0.0);
        QCOMPARE(QDir(directory.filePath("catalog")).entryList(QDir::Files | QDir::Hidden).size(), 1);
        manager.script = Script{};
        catalog.download("engnet");
        QVERIFY(catalog.error().isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 1000);
        QCOMPARE(readFile(destination(directory)), fixture());
    }

    void cancelAndReuse()
    {
        QTemporaryDir directory;
        QVERIFY(seedCache(directory));
        FakeManager manager;
        manager.script.intervalMs = 20;
        manager.script.length = manager.script.body.size();
        VersionCatalog catalog(dependencies(manager, directory));
        QSignalSpy finished(&catalog, &VersionCatalog::downloadFinished);
        catalog.download("engnet");
        QTRY_VERIFY_WITH_TIMEOUT(catalog.progress() > 0, 300);
        QPointer<FakeReply> canceled = manager.lastReply;
        catalog.cancel();
        QVERIFY(canceled && canceled->aborted);
        QVERIFY(!catalog.busy());
        QVERIFY(catalog.error().isEmpty());
        QCOMPARE(catalog.progress(), 0.0);
        QCOMPARE(readFile(destination(directory)), QByteArray("previous cache"));
        QCOMPARE(finished.size(), 0);
        catalog.cancel();
        manager.script = Script{};
        catalog.download("engnet");
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 1000);
        QCOMPARE(readFile(destination(directory)), fixture());
    }

    void timeouts_data()
    {
        QTest::addColumn<bool>("stall");
        QTest::newRow("stall") << true;
        QTest::newRow("total-despite-progress") << false;
    }

    void timeouts()
    {
        QFETCH(bool, stall);
        QTemporaryDir directory;
        QVERIFY(seedCache(directory));
        FakeManager manager;
        manager.script.hold = stall;
        manager.script.chunkSize = 1;
        manager.script.intervalMs = 5;
        auto config = dependencies(manager, directory);
        config.stallTimeoutMs = stall ? 40 : 250;
        config.totalTimeoutMs = stall ? 500 : 80;
        VersionCatalog catalog(config);
        QSignalSpy finished(&catalog, &VersionCatalog::downloadFinished);
        catalog.download("engnet");
        QTRY_VERIFY_WITH_TIMEOUT(!catalog.busy(), 500);
        QVERIFY2(catalog.error().contains(stall ? "stalled" : "timed out"), qPrintable(catalog.error()));
        QCOMPARE(finished.size(), 0);
        QCOMPARE(readFile(destination(directory)), QByteArray("previous cache"));
        manager.script = Script{};
        catalog.download("engnet");
        QVERIFY(catalog.error().isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 500);
    }

    void cacheFailuresAndDestruction()
    {
        QTemporaryDir directory;
        FakeManager manager;
        auto config = dependencies(manager, directory);
        QFile obstruction(directory.filePath("catalog"));
        QVERIFY(obstruction.open(QIODevice::WriteOnly));
        obstruction.close();
        VersionCatalog catalog(config);
        catalog.download("engnet");
        QVERIFY(!catalog.busy());
        QVERIFY(catalog.error().contains("cache"));
        QCOMPARE(manager.requests, 0);
        QVERIFY(obstruction.remove());
        QVERIFY(seedCache(directory));
        manager.script.hold = true;
        {
            VersionCatalog active(config);
            active.download("engnet");
            QVERIFY(active.busy());
        }
        QVERIFY(manager.lastReply && manager.lastReply->aborted);
        QCOMPARE(readFile(destination(directory)), QByteArray("previous cache"));
        QCOMPARE(QDir(directory.filePath("catalog")).entryList(QDir::Files | QDir::Hidden).size(), 1);
    }

    void commitFailureAndRetry()
    {
        QTemporaryDir directory;
        FakeManager manager;
        VersionCatalog catalog(dependencies(manager, directory));
        QSignalSpy finished(&catalog, &VersionCatalog::downloadFinished);
        catalog.download("engnet");
        // Force atomic rename to fail after the staging file was opened.
        QVERIFY(QDir().mkdir(destination(directory)));
        QTRY_VERIFY_WITH_TIMEOUT(!catalog.busy(), 500);
        QVERIFY(catalog.error().contains("Cannot save"));
        QCOMPARE(finished.size(), 0);
        QVERIFY(QDir().rmdir(destination(directory)));
        catalog.download("engnet");
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 500);
        QCOMPARE(readFile(destination(directory)), fixture());
    }

    void cancelFromProgressAndRetry()
    {
        QTemporaryDir directory;
        FakeManager manager;
        manager.script.length = manager.script.body.size();
        VersionCatalog catalog(dependencies(manager, directory));
        QSignalSpy finished(&catalog, &VersionCatalog::downloadFinished);
        bool canceled = false;
        connect(&catalog, &VersionCatalog::progressChanged, this, [&] {
            if (!canceled && catalog.progress() > 0) {
                canceled = true;
                catalog.cancel();
                catalog.download("engnet");
            }
        });
        catalog.download("engnet");
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 500);
        QCOMPARE(manager.requests, 2);
        QCOMPARE(readFile(destination(directory)), fixture());
        QVERIFY(catalog.error().isEmpty());
    }

    void localServerTransfer()
    {
        QTemporaryDir directory;
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QByteArray archive = fixture();
        int requests = 0;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            while (auto *socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                    QByteArray request = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", request);
                    if (!request.contains("\r\n\r\n") || socket->property("sent").toBool()) return;
                    socket->setProperty("sent", true);
                    ++requests;
                    if (request.startsWith("GET /start ")) {
                        socket->write("HTTP/1.1 302 Found\r\nLocation: /archive\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                        socket->disconnectFromHost();
                        return;
                    }
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
                    const auto first = archive.left(31);
                    socket->write(QByteArray::number(first.size(), 16) + "\r\n" + first + "\r\n");
                    QTimer::singleShot(10, socket, [socket, archive] {
                        const auto rest = archive.mid(31);
                        socket->write(QByteArray::number(rest.size(), 16) + "\r\n" + rest + "\r\n0\r\n\r\n");
                        socket->disconnectFromHost();
                    });
                });
            }
        });
        VersionCatalog::Dependencies config;
        config.cacheDirectory = directory.path();
        config.urlTransform = [&](const QUrl &source) {
            if (source.scheme() != "https" || source.host() != "ebible.org") return QUrl();
            return QUrl(QString("http://127.0.0.1:%1/start").arg(server.serverPort()));
        };
        VersionCatalog catalog(config);
        QSignalSpy finished(&catalog, &VersionCatalog::downloadFinished);
        catalog.download("engnet");
        QTRY_VERIFY_WITH_TIMEOUT(!catalog.busy(), 1000);
        QVERIFY2(catalog.error().isEmpty(), qPrintable(catalog.error()));
        QCOMPARE(requests, 2);
        QCOMPARE(finished.size(), 1);
        QCOMPARE(readFile(finished[0][0].toUrl().toLocalFile()), archive);
    }
};

QTEST_GUILESS_MAIN(CatalogTest)
#include "tst_catalog.moc"
