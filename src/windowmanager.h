#pragma once
#include "singleinstance.h"
#include <QList>
#include <QPointer>
#include <QUrl>
#include <QVariant>
class BibleBackend;
class Desktop;
class SoundEffects;
class VersionCatalog;
class QQmlComponent;
class QQmlContext;
class QQmlEngine;
class QWindow;

class WindowManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool available READ available CONSTANT)
public:
    explicit WindowManager(Desktop *desktop, QObject *parent = nullptr,
                           QUrl source = QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    ~WindowManager() override;
    bool available() const { return true; }
    int count() const { return m_readers.size(); }
    void openRequest(const SingleInstance::Request &request, SingleInstance::Reply reply, bool activate = true);
    QWindow *windowAt(int index) const;
    BibleBackend *backendAt(int index) const;
    SoundEffects *sounds() const { return m_sounds; }
    // Follow ("Open beside"): a new window reading `versionId` beside
    // `window`, the two following each other (their passage and the verse at
    // the top) until either closes or unfollows. False, with nothing opened,
    // for a version not installed or a draft that cannot be saved.
    bool openBeside(QWindow *window, const QString &versionId);
    void unfollow(QWindow *window);
    // The window `window` follows, or null.
    QWindow *partnerOf(QWindow *window) const;
public slots:
    void newWindow();
private slots:
    // A window's besideRequested(versionId), unfollowRequested(), and
    // followMoved(location) signals.
    void besideRequested(const QString &versionId);
    void unfollowRequested();
    void relayFollow(const QVariant &location);
signals:
    void countChanged();
private:
    // Every window shares one QML engine (types compiled once, one JavaScript
    // heap): each has its own context, where `bible` and `catalog` are its own.
    struct Reader { QQmlContext *context = nullptr; BibleBackend *bible = nullptr; QWindow *window = nullptr; VersionCatalog *catalog = nullptr; };
    Reader *createWindow();
    void retire(QWindow *window);
    BibleBackend *backendOf(QWindow *window) const;
    struct Link { QPointer<QWindow> a, b; QList<QMetaObject::Connection> connections; };
    QList<Link> m_links;
    static bool flush(QObject *root);
    Desktop *m_desktop;
    SoundEffects *m_sounds; // Shared by every window; preference changes notify all.
    QUrl m_source;
    QQmlEngine *m_engine = nullptr;
    QQmlComponent *m_component = nullptr;
    QList<Reader> m_readers;
    int m_serial = 0;
};
