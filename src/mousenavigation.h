#pragma once

#include <QObject>
#include <QPointer>
#include <QWindow>

// The mouse's Back and Forward buttons anywhere in a window, over dialogs and
// panels included. An item covering the window would have to stand above every
// popup, which Qt counts as a popup's one chance to close on a press outside
// it (6.12), leaving dialogs open on a click beside them; an application
// filter sees the press first and leaves it untouched.
class MouseNavigation : public QObject
{
    Q_OBJECT
    // The window whose presses count: each reader window navigates its own.
    Q_PROPERTY(QWindow *window READ window WRITE setWindow NOTIFY windowChanged)
public:
    explicit MouseNavigation(QObject *parent = nullptr);
    ~MouseNavigation() override;
    QWindow *window() const { return m_window; }
    void setWindow(QWindow *window);
signals:
    void windowChanged();
    void navigated(bool forward);
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    QPointer<QWindow> m_window;
};
