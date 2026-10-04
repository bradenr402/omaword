#include "keyhold.h"
#include "brand.h"

#include <QCoreApplication>
#include <QKeyEvent>
#include <QQmlEngine>

static void registerKeyHold()
{
    static const auto module = (Brand::name() + QStringLiteral(".Reader")).toUtf8();
    qmlRegisterType<KeyHold>(module.constData(), 1, 0, "KeyHold");
}
Q_COREAPP_STARTUP_FUNCTION(registerKeyHold)

KeyHold::KeyHold(QObject *parent) : QObject(parent)
{
    if (auto app = QCoreApplication::instance()) app->installEventFilter(this);
}

KeyHold::~KeyHold()
{
    if (auto app = QCoreApplication::instance()) app->removeEventFilter(this);
}

bool KeyHold::eventFilter(QObject *watched, QEvent *event)
{
    // One key event is delivered to several objects (window, items); only the
    // window-level copy is needed, and duplicates are idempotent anyway.
    // Keys consumed by a Shortcut never arrive as KeyPress; Qt first offers them
    // as ShortcutOverride, before the shortcut activates.
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::ShortcutOverride) {
        const auto key = static_cast<QKeyEvent *>(event);
        const auto released = m_releasedAt.constFind(key->key());
        const bool stale = key->timestamp() && released != m_releasedAt.cend() && key->timestamp() < *released;
        if (!key->isAutoRepeat() && !stale && !m_down.contains(key->key())) m_down.insert(key->key(), ++m_presses);
    } else if (event->type() == QEvent::KeyRelease) {
        const auto key = static_cast<QKeyEvent *>(event);
        if (key->isAutoRepeat()) return QObject::eventFilter(watched, event);
        if (key->timestamp()) m_releasedAt.insert(key->key(), key->timestamp());
        if (m_down.remove(key->key())) emit released(key->key());
    } else if (event->type() == QEvent::ApplicationDeactivate) {
        const auto keys = m_down.keys();
        m_down.clear();
        for (int key : keys) emit released(key);
    }
    return QObject::eventFilter(watched, event);
}
