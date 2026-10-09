#pragma once
#include "singleinstance.h"
#include <QList>
#include <QPointer>
#include <QUrl>
#include <QVariant>
#include <QWindow>
class BibleBackend;
class Desktop;
class SoundEffects;
class VersionCatalog;
class QQmlComponent;
class QQmlContext;
class QQmlEngine;


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
    // Reader windows (the lookup window is not one).
    int count() const { return m_readers.size(); }
    bool hasWindows() const { return !m_readers.isEmpty() || m_lookup; }
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
    // Look up (omaword lookup): a small window for a passage, opened afresh
    // where you are, with `text` typed into it, reading `translation` (by ID
    // or abbreviation) or the version you read. Null when it cannot open.
    QWindow *openLookup(const QString &text, const QString &translation = {});
    QWindow *lookupWindow() const { return m_lookup; }
public slots:
    void newWindow();
private slots:
    // A window's besideRequested(versionId), unfollowRequested(), and
    // followMoved(location) signals.
    void besideRequested(const QString &versionId);
    void unfollowRequested();
    void relayFollow(const QVariant &location);
    // The lookup window's enter: the passage in the reader you used last.
    void lookupOpened(const QString &reference);
signals:
    void countChanged();
    // The last window, reader or lookup, closed.
    void allClosed();
private:
    // Every window shares one QML engine (types compiled once, one JavaScript
    // heap): each has its own context, where `bible` and `catalog` are its own.
    struct Reader { QQmlContext *context = nullptr; BibleBackend *bible = nullptr; QWindow *window = nullptr; VersionCatalog *catalog = nullptr; };
    void ensureEngine();
    Reader *createWindow();
    void retire(QWindow *window);
    BibleBackend *backendOf(QWindow *window) const;
    struct Link { QPointer<QWindow> a, b; QList<QMetaObject::Connection> connections; };
    QList<Link> m_links;
    QPointer<QWindow> m_lastActive;
    QPointer<QWindow> m_lookup;
    QQmlContext *m_lookupContext = nullptr;
    BibleBackend *m_lookupBible = nullptr;
    QString m_lookupTranslation;
    void retireLookup();
    static bool flush(QObject *root);
    Desktop *m_desktop;
    SoundEffects *m_sounds; // Shared by every window; preference changes notify all.
    QUrl m_source;
    QQmlEngine *m_engine = nullptr;
    QQmlComponent *m_component = nullptr;
    QList<Reader> m_readers;
    int m_serial = 0;
};
