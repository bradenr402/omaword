#include "exitsnapshot.h"
#include "brand.h"

#include <QCoreApplication>
#include <QPainter>
#include <QQmlEngine>
#include <QQuickWindow>

static void registerExitSnapshot()
{
    static const auto module = (Brand::name() + QStringLiteral(".Reader")).toUtf8();
    qmlRegisterType<ExitSnapshot>(module.constData(), 1, 0, "ExitSnapshot");
}
Q_COREAPP_STARTUP_FUNCTION(registerExitSnapshot)

ExitSnapshot::ExitSnapshot(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
}

bool ExitSnapshot::capture(QQuickItem *source)
{
    auto target = window();
    if (!source || !target || !source->isVisible() || source->width() <= 0 || source->height() <= 0) return false;
    const QImage frame = target->grabWindow();
    if (frame.isNull()) return false;
    const qreal ratio = frame.devicePixelRatio() > 0 ? frame.devicePixelRatio() : 1;
    const QRectF scene = source->mapRectToScene(QRectF(0, 0, source->width(), source->height()));
    const QRect pixels = QRectF(scene.topLeft() * ratio, scene.size() * ratio).toAlignedRect().intersected(frame.rect());
    if (pixels.isEmpty()) return false;
    m_image = frame.copy(pixels);
    m_image.setDevicePixelRatio(ratio);
    emit imageChanged();
    update();
    return true;
}

void ExitSnapshot::clear()
{
    if (m_image.isNull()) return;
    m_image = QImage();
    emit imageChanged();
    update();
}

void ExitSnapshot::paint(QPainter *painter)
{
    if (!m_image.isNull()) painter->drawImage(boundingRect(), m_image);
}
