#include "mousenavigation.h"
#include "brand.h"

#include <QCoreApplication>
#include <QMouseEvent>
#include <QQmlEngine>

static void registerMouseNavigation()
{
    static const auto module = (Brand::name() + QStringLiteral(".Reader")).toUtf8();
    qmlRegisterType<MouseNavigation>(module.constData(), 1, 0, "MouseNavigation");
}
Q_COREAPP_STARTUP_FUNCTION(registerMouseNavigation)

MouseNavigation::MouseNavigation(QObject *parent) : QObject(parent)
{
    if (auto app = QCoreApplication::instance()) app->installEventFilter(this);
}

MouseNavigation::~MouseNavigation()
{
    if (auto app = QCoreApplication::instance()) app->removeEventFilter(this);
}

void MouseNavigation::setWindow(QWindow *window)
{
    if (m_window == window) return;
    m_window = window;
    emit windowChanged();
}

bool MouseNavigation::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress && (!m_window || watched == m_window)) {
        const auto button = static_cast<QMouseEvent *>(event)->button();
        // Watched, never taken: whatever is under the pointer still gets it.
        if (button == Qt::BackButton || button == Qt::ForwardButton) emit navigated(button == Qt::ForwardButton);
    }
    return QObject::eventFilter(watched, event);
}
