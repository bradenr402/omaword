#pragma once

#include <QObject>
#include <QSet>

// Tracks physically held keys application-wide so QML can drive its own key
// repeat (without the platform's initial repeat delay) and stop on release.
class KeyHold : public QObject
{
    Q_OBJECT
public:
    explicit KeyHold(QObject *parent = nullptr);
    ~KeyHold() override;
    Q_INVOKABLE bool isDown(int key) const { return m_down.contains(key); }
signals:
    void released(int key);
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    QSet<int> m_down;
};
