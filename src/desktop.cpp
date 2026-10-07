#include "desktop.h"

#include <QGuiApplication>
#include <QPalette>
#include <QStyleHints>
#include <QStandardPaths>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QRegularExpression>
#include <QDesktopServices>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cmath>
#include <limits>
#include <fontconfig/fontconfig.h>

Desktop::Desktop(QObject *parent, const QString &hyprctlCommand) : QObject(parent) {
    const QString state = qEnvironmentVariable("XDG_STATE_HOME", QDir::homePath() + QStringLiteral("/.local/state"));
    m_themePath = state + QStringLiteral("/omarchy/current/theme/colors.toml");
    m_fontConfigPath = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/fontconfig");
    // Events, not polling. omarchy-theme-set replaces the theme directory
    // (rm -rf, then mv), which ends any watch inside it: the watch on its parent
    // sees the swap, and each refresh re-arms the rest. omarchy-font-set
    // rewrites fontconfig/fonts.conf. A burst of events makes one refresh.
    m_themeDelay.setSingleShot(true);
    m_themeDelay.setInterval(50);
    connect(&m_themeDelay, &QTimer::timeout, this, &Desktop::refresh);
    m_fontDelay.setSingleShot(true);
    m_fontDelay.setInterval(50);
    connect(&m_fontDelay, &QTimer::timeout, this, [this] { FcInitReinitialize(); refresh(); });
    auto changed = [this](const QString &path) {
        (path.startsWith(m_fontConfigPath) || path == QFileInfo(m_fontConfigPath).absolutePath() ? m_fontDelay : m_themeDelay).start();
    };
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, changed);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, changed);
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, &Desktop::refresh);
    // Query the compositor's effective value, independently of theme changes.
    m_roundingProcess.setProgram(hyprctlCommand);
    m_roundingProcess.setArguments({QStringLiteral("getoption"), QStringLiteral("decoration:rounding"), QStringLiteral("-j")});
    m_roundingProcess.setStandardInputFile(QProcess::nullDevice());
    m_roundingProcess.setStandardErrorFile(QProcess::nullDevice());
    m_roundingTimeout.setSingleShot(true);
    m_roundingTimeout.setInterval(1000);
    connect(&m_roundingTimeout, &QTimer::timeout, this, &Desktop::failRoundingQuery);
    connect(&m_roundingProcess, &QProcess::readyReadStandardOutput, this, &Desktop::readRoundingOutput);
    connect(&m_roundingProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        failRoundingQuery();
        if (m_roundingProcess.state() == QProcess::NotRunning) m_roundingTimeout.stop();
    });
    connect(&m_roundingProcess, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        m_roundingTimeout.stop();
        readRoundingOutput();
        qreal radius = 0;
        if (!m_roundingFailed && code == 0 && status == QProcess::NormalExit &&
            !qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")) {
            const auto document = QJsonDocument::fromJson(m_roundingOutput);
            const auto value = document.object().value(QStringLiteral("int"));
            const double number = value.toDouble(-1);
            if (document.isObject() && value.isDouble() && std::isfinite(number) &&
                number >= 0 && number <= std::numeric_limits<int>::max() && std::floor(number) == number)
                radius = number;
        }
        setCornerRadius(radius);
        // A request while this query ran (a reload, a timeout's late reap)
        // asks once more, now: without polling, nothing else would.
        if (m_roundingPending) QTimer::singleShot(0, this, &Desktop::refreshCornerRadius);
    });
    // Hyprland announces every config reload (a theme switch reloads it) on its
    // event socket; the rounding is asked again only then.
    connect(&m_events, &QLocalSocket::readyRead, this, &Desktop::readEvents);
    // animations:enabled comes over Hyprland's request socket: the reply is
    // the whole stream, so it is read when the compositor closes it. Hyprland
    // 0.56 reports it as "bool"; older releases as "int".
    connect(&m_request, &QLocalSocket::readyRead, this, [this] {
        m_requestBuffer += m_request.read(8193 - m_requestBuffer.size());
        if (m_requestBuffer.size() > 8192) m_request.abort();
    });
    connect(&m_request, &QLocalSocket::disconnected, this, [this] {
        const auto reply = QJsonDocument::fromJson(m_requestBuffer).object();
        const auto value = reply.contains(QStringLiteral("bool")) ? reply.value(QStringLiteral("bool")) : reply.value(QStringLiteral("int"));
        const bool enabled = !(value.isBool() || value.isDouble()) || value.toVariant().toBool();
        if (enabled != m_animations) { m_animations = enabled; emit this->changed(); }
    });
    const QString instance = qEnvironmentVariable("HYPRLAND_INSTANCE_SIGNATURE");
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (!instance.isEmpty() && !runtime.isEmpty())
        m_events.connectToServer(runtime + QStringLiteral("/hypr/") + instance + QStringLiteral("/.socket2.sock"), QIODevice::ReadOnly);
    QDBusConnection::sessionBus().connect(QString(), QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.portal.Settings"), QStringLiteral("SettingChanged"),
        this, SLOT(settingChanged(QString,QString,QDBusVariant)));
    refresh();
    refreshCornerRadius();
    refreshAnimations();
    requestScale();
}

void Desktop::refreshAnimations() {
    const QString instance = qEnvironmentVariable("HYPRLAND_INSTANCE_SIGNATURE");
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (instance.isEmpty() || runtime.isEmpty() || m_request.state() != QLocalSocket::UnconnectedState) return;
    m_requestBuffer.clear();
    m_request.connectToServer(runtime + QStringLiteral("/hypr/") + instance + QStringLiteral("/.socket.sock"));
    // Hyprland reads the request on its main thread as soon as a client
    // connects: send it now, in this call. Left for the event loop, it would
    // stall the compositor until a startup that waits on the compositor (the
    // window's creation) times Hyprland's read out, seconds later.
    if (!m_request.waitForConnected(100)) { m_request.abort(); return; }
    m_request.write("j/getoption animations:enabled");
    m_request.flush();
}

double Desktop::contrast(const QColor &a, const QColor &b) {
    const auto luminance = [](const QColor &color) {
        const auto channel = [](double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
        return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF()) + 0.0722 * channel(color.blueF());
    };
    const double x = luminance(a), y = luminance(b);
    return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
}

QColor Desktop::secondaryText(const QColor &background, const QColor &foreground, const QList<QColor> &candidates) {
    QColor chosen = foreground;
    double least = contrast(foreground, background);
    for (const auto &color : candidates) {
        if (!color.isValid()) continue;
        const double ratio = contrast(color, background);
        if (ratio >= 4.5 && ratio < least) { chosen = color; least = ratio; }
    }
    return chosen;
}

Desktop::~Desktop() {
    // QProcess may emit signals while its destructor reaps a running child.
    // Disconnect before the output buffer and other members are destroyed.
    m_roundingProcess.disconnect(this);
    if (m_roundingProcess.state() != QProcess::NotRunning) {
        m_roundingProcess.kill();
        m_roundingProcess.waitForFinished(1000);
    }
}

void Desktop::refreshCornerRadius() {
    if (qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")) {
        failRoundingQuery();
        return;
    }
    if (m_roundingProcess.state() != QProcess::NotRunning) { m_roundingPending = true; return; }
    m_roundingPending = false;
    m_roundingOutput.clear();
    m_roundingFailed = false;
    m_roundingTimeout.start();
    m_roundingProcess.start(QIODevice::ReadOnly);
}

void Desktop::readRoundingOutput() {
    if (m_roundingFailed) return;
    // Read at most one byte beyond the limit, even if a child floods stdout.
    m_roundingOutput += m_roundingProcess.read(8193 - m_roundingOutput.size());
    if (m_roundingOutput.size() > 8192) failRoundingQuery();
}

void Desktop::failRoundingQuery() {
    m_roundingFailed = true;
    m_roundingTimeout.stop();
    m_roundingOutput.clear();
    m_roundingProcess.closeReadChannel(QProcess::StandardOutput);
    setCornerRadius(0);
    if (m_roundingProcess.state() != QProcess::NotRunning) m_roundingProcess.kill();
}

void Desktop::setCornerRadius(qreal radius) {
    if (m_cornerRadius == radius) return;
    m_cornerRadius = radius;
    emit changed();
}

void Desktop::readEvents() {
    m_eventBuffer += m_events.readAll();
    bool reloaded = false;
    for (qsizetype end; (end = m_eventBuffer.indexOf('\n')) >= 0; m_eventBuffer.remove(0, end + 1))
        reloaded |= m_eventBuffer.startsWith("configreloaded>>");
    // Events are short lines; a runaway one is dropped rather than buffered.
    if (m_eventBuffer.size() > 65536) m_eventBuffer.clear();
    if (reloaded) { refreshCornerRadius(); refreshAnimations(); }
}

void Desktop::watch() {
    const QString theme = QFileInfo(m_themePath).absolutePath();
    const QString current = QFileInfo(theme).absolutePath();
    QStringList paths{current, theme, m_themePath};
    // fonts.conf and its directory; until that exists, the config home it
    // will appear in.
    if (QFileInfo::exists(m_fontConfigPath)) paths << m_fontConfigPath << m_fontConfigPath + QStringLiteral("/fonts.conf");
    else paths << QFileInfo(m_fontConfigPath).absolutePath();
    for (const QString &path : paths) {
        if (!QFileInfo::exists(path) || m_watcher.files().contains(path) || m_watcher.directories().contains(path)) continue;
        m_watcher.addPath(path);
    }
}

void Desktop::refresh() {
    const auto palette = QGuiApplication::palette();
    QColor background = palette.color(QPalette::Window);
    QColor foreground = palette.color(QPalette::WindowText);
    QColor surface = palette.color(QPalette::AlternateBase);
    QColor accent = palette.color(QPalette::Highlight);
    QColor muted = palette.color(QPalette::PlaceholderText);
    QColor border = palette.color(QPalette::Mid);
    QColor color1, color3, error, selection;
    bool dark = background.lightnessF() < 0.5;
    QFile theme(m_themePath);
    const bool readable = theme.open(QIODevice::ReadOnly) && theme.size() < 65536;
    if (!readable && m_themeLoaded) {
        // Mid-swap: Omarchy has removed the old theme directory and is about
        // to move the new one in. Keep the last theme rather than flash Qt's.
        background = m_background; foreground = m_foreground; surface = m_surface; accent = m_accent;
        muted = m_muted; border = m_border; color1 = m_color1; color3 = m_color3; dark = m_dark;
        error = m_error; selection = m_selection;
    } else if (readable) {
        m_themeLoaded = true;
        const QString contents = QString::fromUtf8(theme.readAll());
        QMap<QString, QColor> colors;
        const QRegularExpression expression(QStringLiteral("^\\s*([a-z_0-9]+)\\s*=\\s*[\"'](#[0-9a-fA-F]{6})[\"']"), QRegularExpression::MultilineOption);
        auto it = expression.globalMatch(contents);
        while (it.hasNext()) {
            const auto match = it.next();
            colors.insert(match.captured(1), QColor(match.captured(2)));
        }
        background = colors.value(QStringLiteral("background"), background);
        foreground = colors.value(QStringLiteral("foreground"), foreground);
        dark = background.lightnessF() < 0.5;
        surface = colors.value(QStringLiteral("lighter_background"), dark ? background.lighter(145) : background.darker(105));
        accent = colors.value(QStringLiteral("accent"), accent);
        muted = secondaryText(background, foreground, {colors.value(QStringLiteral("muted")),
            colors.value(QStringLiteral("dark_foreground")), colors.value(QStringLiteral("light_foreground"))});
        error = colors.value(QStringLiteral("red"));
        selection = colors.value(QStringLiteral("selection"));
        // Palette slot 1 (ANSI red): `color1`, or `red` in named palettes.
        color1 = colors.value(QStringLiteral("color1"), colors.value(QStringLiteral("red")));
        // Palette slot 3 (ANSI yellow): `color3`, or `yellow`.
        color3 = colors.value(QStringLiteral("color3"), colors.value(QStringLiteral("yellow")));
        border = QColor::fromRgbF(background.redF() * .78 + foreground.redF() * .22,
            background.greenF() * .78 + foreground.greenF() * .22,
            background.blueF() * .78 + foreground.blueF() * .22);
    }
    QString family = QStringLiteral("monospace");
    FcPattern *pattern = FcNameParse(reinterpret_cast<const FcChar8 *>("monospace"));
    if (pattern) {
        FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
        FcDefaultSubstitute(pattern);
        FcResult result;
        FcPattern *match = FcFontMatch(nullptr, pattern, &result);
        FcChar8 *name = nullptr;
        if (match && FcPatternGetString(match, FC_FAMILY, 0, &name) == FcResultMatch)
            family = QString::fromUtf8(reinterpret_cast<const char *>(name));
        if (match) FcPatternDestroy(match);
        FcPatternDestroy(pattern);
    }
    if (!color1.isValid()) color1 = accent;
    if (!color3.isValid()) color3 = accent;
    if (!error.isValid()) error = color1;
    if (!selection.isValid())
        selection = QColor::fromRgbF(background.redF() * .65 + accent.redF() * .35,
            background.greenF() * .65 + accent.greenF() * .35, background.blueF() * .65 + accent.blueF() * .35);
    watch();
    if (background == m_background && foreground == m_foreground && surface == m_surface &&
        accent == m_accent && muted == m_muted && border == m_border && color1 == m_color1 && color3 == m_color3 && family == m_fontFamily && dark == m_dark
        && error == m_error && selection == m_selection) return;
    m_background = background; m_foreground = foreground; m_surface = surface;
    m_accent = accent; m_color1 = color1; m_color3 = color3; m_muted = muted; m_border = border; m_fontFamily = family; m_dark = dark;
    m_error = error; m_selection = selection;
    emit changed();
}

void Desktop::requestScale() {
    auto message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.portal.Desktop"),
        QStringLiteral("/org/freedesktop/portal/desktop"), QStringLiteral("org.freedesktop.portal.Settings"), QStringLiteral("Read"));
    message << QStringLiteral("org.gnome.desktop.interface") << QStringLiteral("text-scaling-factor");
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *finished) {
        QDBusPendingReply<QDBusVariant> reply = *finished;
        if (reply.isValid()) setScale(reply.value().variant());
        finished->deleteLater();
    });
}

void Desktop::setScale(const QVariant &value) {
    bool ok = false;
    const double scale = value.toDouble(&ok);
    if (!ok || !qIsFinite(scale) || scale <= 0) return;
    const double bounded = qBound(0.5, scale, 3.0);
    if (qFuzzyCompare(bounded, m_textScale)) return;
    m_textScale = bounded;
    emit changed();
}

void Desktop::settingChanged(const QString &space, const QString &key, const QDBusVariant &value) {
    if (space == QStringLiteral("org.gnome.desktop.interface") && key == QStringLiteral("text-scaling-factor")) setScale(value.variant());
    if (key == QStringLiteral("color-scheme")) refresh();
}

void Desktop::openExternal(const QUrl &url) {
    if (url.scheme() == QStringLiteral("https") || url.scheme() == QStringLiteral("http") ||
        (url.isLocalFile() && QFileInfo(url.toLocalFile()).isDir()))
        QDesktopServices::openUrl(url);
}
