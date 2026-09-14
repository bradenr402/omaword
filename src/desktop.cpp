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
#include <cmath>
#include <limits>
#include <fontconfig/fontconfig.h>

Desktop::Desktop(QObject *parent, const QString &hyprctlCommand) : QObject(parent) {
    const QString state = qEnvironmentVariable("XDG_STATE_HOME", QDir::homePath() + QStringLiteral("/.local/state"));
    m_themePath = state + QStringLiteral("/omarchy/current/theme/colors.toml");
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, &Desktop::refresh);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &Desktop::refresh);
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, &Desktop::refresh);
    // Polling also catches replaced theme symlinks and fontconfig edits without
    // requiring modifications to the user's Omarchy hooks.
    connect(&m_poll, &QTimer::timeout, this, &Desktop::refresh);
    m_poll.start(2000);
    // Query the compositor's effective value, independently of theme polling.
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
    });
    connect(&m_roundingPoll, &QTimer::timeout, this, &Desktop::refreshCornerRadius);
    m_roundingPoll.start(5000);
    QDBusConnection::sessionBus().connect(QString(), QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.portal.Settings"), QStringLiteral("SettingChanged"),
        this, SLOT(settingChanged(QString,QString,QDBusVariant)));
    refresh();
    refreshCornerRadius();
    requestScale();
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
    if (m_roundingProcess.state() != QProcess::NotRunning) return;
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

void Desktop::refresh() {
    const auto palette = QGuiApplication::palette();
    QColor background = palette.color(QPalette::Window);
    QColor foreground = palette.color(QPalette::WindowText);
    QColor surface = palette.color(QPalette::AlternateBase);
    QColor accent = palette.color(QPalette::Highlight);
    QColor muted = palette.color(QPalette::PlaceholderText);
    QColor border = palette.color(QPalette::Mid);
    bool dark = background.lightnessF() < 0.5;
    QFile theme(m_themePath);
    if (theme.open(QIODevice::ReadOnly) && theme.size() < 65536) {
        const QString contents = QString::fromUtf8(theme.readAll());
        QMap<QString, QColor> colors;
        const QRegularExpression expression(QStringLiteral("^\\s*([a-z_]+)\\s*=\\s*[\"'](#[0-9a-fA-F]{6})[\"']"), QRegularExpression::MultilineOption);
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
        muted = colors.value(QStringLiteral("muted"), foreground);
        border = QColor::fromRgbF(background.redF() * .78 + foreground.redF() * .22,
            background.greenF() * .78 + foreground.greenF() * .22,
            background.blueF() * .78 + foreground.blueF() * .22);
    }
    QString family = QStringLiteral("monospace");
    FcInitBringUptoDate();
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
    const QString directory = QFileInfo(m_themePath).absolutePath();
    if (QFile::exists(m_themePath) && !m_watcher.files().contains(m_themePath)) m_watcher.addPath(m_themePath);
    if (QDir(directory).exists() && !m_watcher.directories().contains(directory)) m_watcher.addPath(directory);
    if (background == m_background && foreground == m_foreground && surface == m_surface &&
        accent == m_accent && muted == m_muted && border == m_border && family == m_fontFamily && dark == m_dark) return;
    m_background = background; m_foreground = foreground; m_surface = surface;
    m_accent = accent; m_muted = muted; m_border = border; m_fontFamily = family; m_dark = dark;
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
