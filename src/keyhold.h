#pragma once

#include <QObject>
#include <QHash>

// Tracks physically held keys application-wide so QML can drive its own key
// repeat (without the platform's initial repeat delay) and stop on release.
class KeyHold : public QObject
{
    Q_OBJECT
public:
    explicit KeyHold(QObject *parent = nullptr);
    ~KeyHold() override;
    Q_INVOKABLE bool isDown(int key) const { return m_down.contains(key); }
    // Identifies one physical press of a held key (0 when up): a hold may only
    // continue while the press that began it is still down.
    Q_INVOKABLE int press(int key) const { return m_down.value(key); }
signals:
    void released(int key);
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    QHash<int, int> m_down;
    // Platform timestamp of each key's last release; a stale press/override
    // for it (queued before that release) must not mark it down again.
    QHash<int, quint64> m_releasedAt;
    int m_presses = 0;
};
