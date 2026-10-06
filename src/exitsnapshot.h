#pragma once

#include <QImage>
#include <QQuickPaintedItem>

// A still picture of another item, taken synchronously from the window
// (QQuickWindow::grabWindow) while that item is still shown. ExitGhost uses
// it for the exit motion of popups that close at once: unlike a
// ShaderEffectSource, the picture keeps no reference to the closed popup
// (an effect source on a hidden popup breaks focusing into it later).
class ExitSnapshot : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(bool empty READ empty NOTIFY imageChanged)
public:
    explicit ExitSnapshot(QQuickItem *parent = nullptr);
    // Pictures `source` as currently rendered; returns false when it cannot.
    Q_INVOKABLE bool capture(QQuickItem *source);
    Q_INVOKABLE void clear();
    bool empty() const { return m_image.isNull(); }
    void paint(QPainter *painter) override;
signals:
    void imageChanged();
private:
    QImage m_image;
};
