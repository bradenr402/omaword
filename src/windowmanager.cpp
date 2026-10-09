#include "windowmanager.h"
#include "biblebackend.h"
#include "brand.h"
#include "desktop.h"
#include "soundeffects.h"
#include "versioncatalog.h"
#include <QCoreApplication>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QTextStream>
#include <QTimer>
#include <QWindow>
#include <memory>

// A requested passage as the passage palette reads it: an exact reference,
// else its first suggestion (compact input such as jn316).
static QString palettePassage(const BibleBackend *bible, const QString &text)
{
    if (bible->resolveReference(text).value("valid").toBool()) return text;
    const auto candidates = bible->referenceCandidates(text, 1);
    return candidates.isEmpty() ? text : candidates.first().toMap().value("reference").toString();
}

WindowManager::WindowManager(Desktop *desktop, QObject *parent, QUrl source)
    : QObject(parent), m_desktop(desktop), m_sounds(new SoundEffects(this)), m_source(std::move(source)) {}

WindowManager::~WindowManager()
{
    for (const auto &reader : m_readers) {
        reader.window->disconnect(this);
        reader.bible->setFlushCallback({});
        // QML dies while its context objects are still valid.
        delete reader.window;
        delete reader.context;
        delete reader.catalog;
        delete reader.bible;
    }
    if (m_lookup) m_lookup->disconnect(this);
    delete m_lookup.data();
    delete m_lookupContext;
    delete m_lookupBible;
    delete m_component;
    delete m_engine;
}

bool WindowManager::flush(QObject *root)
{
    QVariant saved;
    return root && QMetaObject::invokeMethod(root, "flushNote", Q_RETURN_ARG(QVariant, saved)) && saved.toBool();
}

void WindowManager::ensureEngine()
{
    if (!m_engine) {
        m_engine = new QQmlEngine(this);
        // Qt.quit() and Qt.exit() reach the application, as with QQmlApplicationEngine.
        connect(m_engine, &QQmlEngine::quit, QCoreApplication::instance(), &QCoreApplication::quit, Qt::QueuedConnection);
        connect(m_engine, &QQmlEngine::exit, QCoreApplication::instance(), &QCoreApplication::exit, Qt::QueuedConnection);
        m_engine->rootContext()->setContextProperty(QStringLiteral("desktop"), m_desktop);
        m_engine->rootContext()->setContextProperty(QStringLiteral("sounds"), m_sounds);
        m_engine->rootContext()->setContextProperty(QStringLiteral("windows"), this);
        m_component = new QQmlComponent(m_engine, m_source, QQmlComponent::PreferSynchronous, this);
    }
}

WindowManager::Reader *WindowManager::createWindow()
{
    ensureEngine();
    auto *bible = new BibleBackend(this);
    auto *catalog = new VersionCatalog(this);
    auto *context = new QQmlContext(m_engine->rootContext(), this);
    context->setContextProperty(QStringLiteral("bible"), bible);
    context->setContextProperty(QStringLiteral("catalog"), catalog);
    auto *window = qobject_cast<QWindow *>(m_component->create(context));
    if (!window) {
        QTextStream(stderr) << m_component->errorString() << Qt::endl;
        delete context; delete catalog; delete bible; return nullptr;
    }
    const QString suffix = QStringLiteral(" · Window %1").arg(++m_serial);
    // Distinguish surfaces belonging to this PID even when passages/titles match.
    connect(window, &QWindow::windowTitleChanged, window, [window, suffix](const QString &title) {
        if (!title.endsWith(suffix)) window->setTitle(title + suffix);
    });
    window->setTitle(window->title() + suffix);
    QPointer<QWindow> root(window);
    bible->setFlushCallback([root] { return flush(root); });
    if (window->metaObject()->indexOfSignal("newWindowRequested()") >= 0)
        connect(window, SIGNAL(newWindowRequested()), this, SLOT(newWindow()));
    if (window->metaObject()->indexOfSignal("besideRequested(QString)") >= 0) {
        connect(window, SIGNAL(besideRequested(QString)), this, SLOT(besideRequested(QString)));
        connect(window, SIGNAL(unfollowRequested()), this, SLOT(unfollowRequested()));
    }
    m_readers.append({context, bible, window, catalog});
    // The reader you used last: where a lookup's passage opens.
    connect(window, &QWindow::activeChanged, this, [this, root] { if (root && root->isActive()) m_lastActive = root; });
    connect(window, &QWindow::visibleChanged, this, [this, root](bool visible) {
        if (!visible && root) QTimer::singleShot(0, this, [this, root] {
            if (root && !root->isVisible()) retire(root);
        });
    });
    window->show();
    emit countChanged();
    return &m_readers.last();
}

void WindowManager::retire(QWindow *window)
{
    unfollow(window);
    for (qsizetype i = 0; i < m_readers.size(); ++i) {
        if (m_readers[i].window != window) continue;
        const auto reader = m_readers.takeAt(i);
        reader.window->disconnect(this);
        reader.bible->setFlushCallback({});
        // This runs on the next event turn, after the QML close handler returned.
        delete reader.window;
        delete reader.context;
        delete reader.catalog;
        reader.bible->deleteLater();
        emit countChanged();
        if (!hasWindows()) emit allClosed();
        return;
    }
}

QWindow *WindowManager::openLookup(const QString &text, const QString &translation)
{
    ensureEngine();
    // Afresh each time: a new window opens on the workspace you are on.
    if (m_lookup) { m_lookup->disconnect(this); retireLookup(); }
    auto *bible = new BibleBackend(this);
    const QString id = translation.isEmpty() ? QString() : bible->versionIdFor(translation);
    if ((!translation.isEmpty() && (id.isEmpty() || !bible->readVersion(id))) || !bible->ready()) { delete bible; return nullptr; }
    auto *context = new QQmlContext(m_engine->rootContext(), this);
    QQmlComponent component(m_engine, QUrl(QStringLiteral("qrc:/qml/LookupWindow.qml")), QQmlComponent::PreferSynchronous);
    auto *window = qobject_cast<QWindow *>(component.createWithInitialProperties({{QStringLiteral("initialText"), text},
        {QStringLiteral("theme"), QVariant::fromValue<QObject *>(m_desktop)}, {QStringLiteral("backend"), QVariant::fromValue<QObject *>(bible)},
        {QStringLiteral("effects"), QVariant::fromValue<QObject *>(m_sounds)}}, context));
    if (!window) {
        QTextStream(stderr) << component.errorString() << Qt::endl;
        delete context; delete bible; return nullptr;
    }
    m_lookup = window; m_lookupContext = context; m_lookupBible = bible; m_lookupTranslation = translation;
    connect(window, SIGNAL(openRequested(QString)), this, SLOT(lookupOpened(QString)));
    QPointer<QWindow> root(window);
    connect(window, &QWindow::visibleChanged, this, [this, root](bool visible) {
        if (!visible && root) QTimer::singleShot(0, this, [this, root] {
            if (root && !root->isVisible() && root == m_lookup) { retireLookup(); if (!hasWindows()) emit allClosed(); }
        });
    });
    window->show();
    return window;
}

void WindowManager::retireLookup()
{
    delete m_lookup.data();
    delete m_lookupContext; m_lookupContext = nullptr;
    if (m_lookupBible) m_lookupBible->deleteLater();
    m_lookupBible = nullptr;
}

void WindowManager::lookupOpened(const QString &reference)
{
    SingleInstance::Request request;
    request.reference = reference;
    request.translation = m_lookupTranslation;
    request.newWindow = false;
    openRequest(request, [](SingleInstance::Result result) {
        if (!result.ok) QTextStream(stderr) << Brand::name() << ": " << result.error << Qt::endl;
    });
}

QWindow *WindowManager::windowAt(int index) const { return m_readers.value(index).window; }

BibleBackend *WindowManager::backendOf(QWindow *window) const
{
    for (const auto &reader : m_readers) if (reader.window == window) return reader.bible;
    return nullptr;
}

bool WindowManager::openBeside(QWindow *window, const QString &versionId)
{
    auto *bible = backendOf(window);
    if (!bible || bible->versionIdFor(versionId).isEmpty() || !flush(window)) return false;
    QVariant location;
    QMetaObject::invokeMethod(window, "followLocation", Q_RETURN_ARG(QVariant, location));
    unfollow(window);
    QPointer<QWindow> leader(window);
    auto *reader = createWindow();
    if (!reader) return false;
    QPointer<QWindow> follower(reader->window);
    // Read there without making it the version you read (as a peer would).
    if (!reader->bible->readVersion(bible->versionIdFor(versionId))) { follower->close(); return false; }
    QMetaObject::invokeMethod(follower, "followTo", Q_ARG(QVariant, location));
    if (!leader) return true;
    Link link{leader, follower, {}};
    for (QWindow *root : {leader.data(), follower.data()})
        link.connections << connect(root, SIGNAL(followMoved(QVariant)), this, SLOT(relayFollow(QVariant)));
    // Each names the other's version, which can change.
    for (const auto &[self, other] : {std::pair{leader, follower}, std::pair{follower, leader}}) {
        auto *otherBible = backendOf(other);
        const auto name = [self = self, otherBible] { if (self) self->setProperty("followPartner", otherBible->versionAbbreviation()); };
        link.connections << connect(otherBible, &BibleBackend::versionChanged, self, name);
        name();
    }
    m_links << link;
    return true;
}

void WindowManager::unfollow(QWindow *window)
{
    for (qsizetype i = 0; i < m_links.size(); ++i) {
        if (m_links[i].a != window && m_links[i].b != window) continue;
        const auto link = m_links.takeAt(i);
        for (const auto &connection : link.connections) disconnect(connection);
        for (QWindow *root : {link.a.data(), link.b.data()}) if (root) root->setProperty("followPartner", QString());
        return;
    }
}

QWindow *WindowManager::partnerOf(QWindow *window) const
{
    for (const auto &link : m_links) {
        if (link.a == window) return link.b;
        if (link.b == window) return link.a;
    }
    return nullptr;
}

void WindowManager::besideRequested(const QString &versionId)
{
    auto *window = qobject_cast<QWindow *>(sender());
    if (!openBeside(window, versionId) && window)
        QMetaObject::invokeMethod(window, "rejected", Q_ARG(QVariant, QStringLiteral("Could not open a window beside")));
}

void WindowManager::unfollowRequested() { unfollow(qobject_cast<QWindow *>(sender())); }

void WindowManager::relayFollow(const QVariant &location)
{
    if (auto *partner = partnerOf(qobject_cast<QWindow *>(sender())))
        QMetaObject::invokeMethod(partner, "followTo", Q_ARG(QVariant, location));
}
BibleBackend *WindowManager::backendAt(int index) const { return m_readers.value(index).bible; }

void WindowManager::newWindow()
{
    openRequest({}, [](SingleInstance::Result result) {
        if (!result.ok) QTextStream(stderr) << Brand::name() << ": " << result.error << Qt::endl;
    });
}

void WindowManager::openRequest(const SingleInstance::Request &request, SingleInstance::Reply reply, bool activate)
{
    if (request.lookup) {
        QPointer<QWindow> window(openLookup(request.reference, request.translation));
        if (!window) { reply({false, request.translation.isEmpty() ? QStringLiteral("Add a Bible version first.")
                                     : QStringLiteral("No installed translation \u201c%1\u201d.").arg(request.translation)}); return; }
        if (!activate) { reply({true, {}}); return; }
        SingleInstance::activateWindow(window, request, this, reply);
        return;
    }
    Reader *reader = nullptr;
    if (!request.newWindow && !m_readers.isEmpty()) {
        // The active reader, else the one you used last, else the newest.
        reader = &m_readers.last();
        for (auto &candidate : m_readers) if (candidate.window == m_lastActive) reader = &candidate;
        for (auto &candidate : m_readers) if (candidate.window->isActive()) { reader = &candidate; break; }
    } else reader = createWindow();
    if (!reader) { reply({false, QStringLiteral("Cannot create a reader window.")}); return; }
    auto *bible = reader->bible;
    QPointer<QWindow> root(reader->window);
    SingleInstance::Result action{true, {}};
    const QString versionId = request.translation.isEmpty() ? QString() : bible->versionIdFor(request.translation);
    if (!request.translation.isEmpty() && versionId.isEmpty())
        action = {false, QStringLiteral("No installed translation \u201c%1\u201d.").arg(request.translation)};
    else if (!request.importPath.isEmpty() || !request.reference.isEmpty() || !versionId.isEmpty()) {
        if (!flush(root)) action = {false, QStringLiteral("The unsaved note could not be saved.")};
        else {
            QMetaObject::invokeMethod(root, "savePosition");
            if (!request.importPath.isEmpty()) {
                if (bible->busy()) action = {false, QStringLiteral("A USFM import is already running.")};
                else {
                    bible->importUsfm(QUrl::fromLocalFile(request.importPath));
                    if (!bible->busy()) action = {false, bible->error()};
                    else if (!request.reference.isEmpty()) {
                        auto connection = std::make_shared<QMetaObject::Connection>();
                        *connection = connect(bible, &BibleBackend::busyChanged, bible,
                            [bible, root, reference = request.reference, connection] {
                                if (bible->busy()) return;
                                QObject::disconnect(*connection);
                                if (root && bible->ready() && bible->error().isEmpty() && flush(root)) bible->navigate(palettePassage(bible, reference));
                            });
                    }
                }
            } else {
                // --translation: the window switches to it, as Versions would.
                if (!versionId.isEmpty() && versionId != bible->versionId()) {
                    QMetaObject::invokeMethod(root, "switchVersion", Q_ARG(QVariant, versionId));
                    if (bible->versionId() != versionId)
                        action = {false, bible->error().isEmpty() ? QStringLiteral("Cannot switch to %1.").arg(request.translation) : bible->error()};
                }
                if (action.ok && !request.reference.isEmpty() && !bible->navigate(palettePassage(bible, request.reference)))
                    action = {false, bible->error()};
            }
        }
    }
    if (!activate) { reply(action); return; }
    SingleInstance::activateWindow(root, request, this, [action, reply](SingleInstance::Result activation) {
        if (!action.ok) reply(action);
        else if (!activation.ok) reply({false, QStringLiteral("Request accepted, but window activation failed: ") + activation.error});
        else reply(action);
    });
}
