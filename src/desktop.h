#pragma once

#include <QObject>
#include <QColor>
#include <QFileSystemWatcher>
#include <QTimer>
#include <QUrl>
#include <QDBusVariant>
#include "brand.h"

class Desktop : public QObject {
    Q_OBJECT
    Q_PROPERTY(QColor background MEMBER m_background NOTIFY changed)
    Q_PROPERTY(QColor foreground MEMBER m_foreground NOTIFY changed)
    Q_PROPERTY(QColor surface MEMBER m_surface NOTIFY changed)
    Q_PROPERTY(QColor muted MEMBER m_muted NOTIFY changed)
    Q_PROPERTY(QColor accent MEMBER m_accent NOTIFY changed)
    Q_PROPERTY(QColor border MEMBER m_border NOTIFY changed)
    Q_PROPERTY(QString fontFamily MEMBER m_fontFamily NOTIFY changed)
    Q_PROPERTY(double textScale MEMBER m_textScale NOTIFY changed)
    Q_PROPERTY(bool dark MEMBER m_dark NOTIFY changed)
    Q_PROPERTY(QString appName READ appName CONSTANT)
    Q_PROPERTY(QString appId READ appId CONSTANT)
    Q_PROPERTY(QString version READ version CONSTANT)
public:
    explicit Desktop(QObject *parent = nullptr);
    QString appName() const { return Brand::appName(); }
    QString appId() const { return Brand::appId(); }
    QString version() const { return Brand::version(); }
    Q_INVOKABLE void openExternal(const QUrl &url);
signals:
    void changed();
private slots:
    void refresh();
    void settingChanged(const QString &space, const QString &key, const QDBusVariant &value);
private:
    void requestScale();
    void setScale(const QVariant &value);
    QColor m_background, m_foreground, m_surface, m_muted, m_accent, m_border;
    QString m_fontFamily, m_themePath;
    double m_textScale = 1.0;
    bool m_dark = true;
    QFileSystemWatcher m_watcher;
    QTimer m_poll;
};
