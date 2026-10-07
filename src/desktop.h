#pragma once

#include <QObject>
#include <QColor>
#include <QFileSystemWatcher>
#include <QLocalSocket>
#include <QTimer>
#include <QUrl>
#include <QDBusVariant>
#include <QProcess>
#include "brand.h"

class Desktop : public QObject {
    Q_OBJECT
    Q_PROPERTY(QColor background MEMBER m_background NOTIFY changed)
    Q_PROPERTY(QColor foreground MEMBER m_foreground NOTIFY changed)
    Q_PROPERTY(QColor surface MEMBER m_surface NOTIFY changed)
    // Secondary text: the theme's least prominent text color that still reads
    // (WCAG 4.5:1 on the background), never brighter than the foreground.
    Q_PROPERTY(QColor muted MEMBER m_muted NOTIFY changed)
    Q_PROPERTY(QColor accent MEMBER m_accent NOTIFY changed)
    // Omarchy palette color1 (words of Jesus); the accent when the theme has none.
    Q_PROPERTY(QColor color1 MEMBER m_color1 NOTIFY changed)
    // Omarchy palette color3 (yellow, the highlighter ink); the accent when the theme has none.
    Q_PROPERTY(QColor color3 MEMBER m_color3 NOTIFY changed)
    Q_PROPERTY(QColor border MEMBER m_border NOTIFY changed)
    // Errors: the theme's red (else color1, else the accent).
    Q_PROPERTY(QColor error MEMBER m_error NOTIFY changed)
    // Selected text: the theme's selection color (else a tint of the accent).
    Q_PROPERTY(QColor selection MEMBER m_selection NOTIFY changed)
    // Hyprland's animations:enabled (true outside Hyprland); the app animates
    // only when this and its own Animations setting are on.
    Q_PROPERTY(bool animations READ animations NOTIFY changed)
    Q_PROPERTY(QString fontFamily MEMBER m_fontFamily NOTIFY changed)
    Q_PROPERTY(double textScale MEMBER m_textScale NOTIFY changed)
    Q_PROPERTY(qreal cornerRadius READ cornerRadius NOTIFY changed)
    Q_PROPERTY(bool dark MEMBER m_dark NOTIFY changed)
    Q_PROPERTY(QString appName READ appName CONSTANT)
    Q_PROPERTY(QString appId READ appId CONSTANT)
    Q_PROPERTY(QString version READ version CONSTANT)
public:
    explicit Desktop(QObject *parent = nullptr, const QString &hyprctlCommand = QStringLiteral("hyprctl"));
    ~Desktop() override;
    qreal cornerRadius() const { return m_cornerRadius; }
    bool animations() const { return m_animations; }
    // WCAG 2 contrast ratio of two opaque colors (1–21).
    static double contrast(const QColor &a, const QColor &b);
    // The least prominent candidate with at least 4.5:1 against background
    // and no more contrast than foreground; foreground when none qualifies.
    static QColor secondaryText(const QColor &background, const QColor &foreground, const QList<QColor> &candidates);
    QString appName() const { return Brand::appName(); }
    QString appId() const { return Brand::appId(); }
    QString version() const { return Brand::version(); }
    Q_INVOKABLE void openExternal(const QUrl &url);
signals:
    void changed();
private slots:
    void refresh();
    void refreshCornerRadius();
    void refreshAnimations();
    void settingChanged(const QString &space, const QString &key, const QDBusVariant &value);
private:
    void watch();
    void readEvents();
    void requestScale();
    void setScale(const QVariant &value);
    void setCornerRadius(qreal radius);
    void readRoundingOutput();
    void failRoundingQuery();
    QColor m_background, m_foreground, m_surface, m_muted, m_accent, m_border, m_color1, m_color3, m_error, m_selection;
    QString m_fontFamily, m_themePath, m_fontConfigPath;
    double m_textScale = 1.0;
    qreal m_cornerRadius = 0;
    bool m_dark = true;
    bool m_themeLoaded = false;
    QFileSystemWatcher m_watcher;
    QTimer m_themeDelay, m_fontDelay;
    QLocalSocket m_events;
    QByteArray m_eventBuffer;
    QTimer m_roundingTimeout;
    QProcess m_roundingProcess;
    QByteArray m_roundingOutput;
    bool m_roundingFailed = false;
    bool m_roundingPending = false;
    // Hyprland's request socket: one getoption query per config (re)load.
    QLocalSocket m_request;
    QByteArray m_requestBuffer;
    bool m_animations = true;
};
