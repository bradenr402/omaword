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
    delete m_component;
    delete m_engine;
}

bool WindowManager::flush(QObject *root)
{
    QVariant saved;
    return root && QMetaObject::invokeMethod(root, "flushNote", Q_RETURN_ARG(QVariant, saved)) && saved.toBool();
}

WindowManager::Reader *WindowManager::createWindow()
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
    m_readers.append({context, bible, window, catalog});
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
        return;
    }
}

QWindow *WindowManager::windowAt(int index) const { return m_readers.value(index).window; }
BibleBackend *WindowManager::backendAt(int index) const { return m_readers.value(index).bible; }

void WindowManager::newWindow()
{
    openRequest({}, [](SingleInstance::Result result) {
        if (!result.ok) QTextStream(stderr) << Brand::name() << ": " << result.error << Qt::endl;
    });
}

void WindowManager::openRequest(const SingleInstance::Request &request, SingleInstance::Reply reply, bool activate)
{
    Reader *reader = nullptr;
    if (!request.newWindow && !m_readers.isEmpty()) {
        reader = &m_readers.last();
        for (auto &candidate : m_readers) if (candidate.window->isActive()) { reader = &candidate; break; }
    } else reader = createWindow();
    if (!reader) { reply({false, QStringLiteral("Cannot create a reader window.")}); return; }
    auto *bible = reader->bible;
    QPointer<QWindow> root(reader->window);
    SingleInstance::Result action{true, {}};
    const QString versionId = request.translation.isEmpty() ? QString() : bible->versionIdFor(request.translation);
    if (!request.translation.isEmpty() && versionId.isEmpty())
        action = {false, QStringLiteral("No installed translation \u201c%1\u201d.").arg(request.translation)};
    else if (!request.lookup && (!request.importPath.isEmpty() || !request.reference.isEmpty() || !versionId.isEmpty())) {
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
    // Lookup: the passage palette, ready to type, with any text typed into it.
    if (request.lookup && action.ok) QMetaObject::invokeMethod(root, "lookUp", Q_ARG(QVariant, request.reference));
    if (!activate) { reply(action); return; }
    SingleInstance::activateWindow(root, request, this, [action, reply](SingleInstance::Result activation) {
        if (!action.ok) reply(action);
        else if (!activation.ok) reply({false, QStringLiteral("Request accepted, but window activation failed: ") + activation.error});
        else reply(action);
    });
}
