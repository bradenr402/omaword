#include "soundeffects.h"

#include <QGuiApplication>
#include <QSettings>
#include <QtMath>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <pipewire/pipewire.h>
#include <spa/param/audio/raw-utils.h>

namespace {
constexpr int sampleRate = 44100;
// The level QAudioSink played the cues at, applied to the samples instead.
constexpr double outputVolume = 0.6;
const char *settingsKey = "sound/effectsEnabled";
const char *motionKey = "motion/reduced";
const char *introducedKey = "sound/introduced";

struct Note { double startHz, endHz, ms, gain; };

// Sine sweep(s) with a soft attack and exponential decay, 16-bit mono PCM.
QByteArray synthesize(std::initializer_list<Note> notes)
{
    QByteArray pcm;
    for (const Note &note : notes) {
        const int samples = int(sampleRate * note.ms / 1000.0);
        const int attack = qMax(1, sampleRate / 500); // 2 ms
        double phase = 0;
        for (int i = 0; i < samples; ++i) {
            const double t = double(i) / samples;
            const double hz = note.startHz + (note.endHz - note.startHz) * t;
            phase += 2 * M_PI * hz / sampleRate;
            const double envelope = qMin(1.0, double(i) / attack) * qExp(-4.5 * t) * (1.0 - t);
            const auto sample = qint16(qBound(-1.0, qSin(phase) * envelope * note.gain, 1.0) * 32767);
            pcm.append(reinterpret_cast<const char *>(&sample), sizeof sample);
        }
    }
    return pcm;
}

QHash<QString, QByteArray> baseTones()
{
    QHash<QString, QByteArray> tones;
    tones.insert(QStringLiteral("tick"), synthesize({{1900, 1700, 18, 0.10}}));
    tones.insert(QStringLiteral("boundary"), synthesize({{420, 380, 60, 0.12}}));
    tones.insert(QStringLiteral("chime"), synthesize({{880, 880, 70, 0.09}, {1320, 1320, 110, 0.08}}));
    tones.insert(QStringLiteral("remove"), synthesize({{740, 520, 110, 0.10}}));
    // A selection taking in a whole passage (m): a short rising glide that settles.
    tones.insert(QStringLiteral("select"), synthesize({{560, 840, 45, 0.09}, {840, 860, 60, 0.07}}));
    // As loud as the others: a key that cannot act is not an alarm.
    tones.insert(QStringLiteral("error"), synthesize({{170, 110, 90, 0.10}}));
    return tones;
}

QByteArray scaled(const QByteArray &pcm, double volume)
{
    QByteArray out(pcm.size(), Qt::Uninitialized);
    const auto *in = reinterpret_cast<const qint16 *>(pcm.constData());
    auto *samples = reinterpret_cast<qint16 *>(out.data());
    for (qsizetype i = 0; i < pcm.size() / qsizetype(sizeof(qint16)); ++i) samples[i] = qint16(in[i] * volume);
    return out;
}

QSettings *openSettings(const QString &path)
{
    return path.isEmpty() ? new QSettings : new QSettings(path, QSettings::IniFormat);
}
}

// One cue at a time, each on its own PipeWire stream: a new stream goes to the
// output that is the default right now (WirePlumber routes it), and once a cue
// has drained its stream goes inactive, so an idle app is never woken.
struct SoundEffects::Output {
    pw_thread_loop *loop = nullptr;
    pw_context *context = nullptr;
    pw_core *core = nullptr;
    spa_hook coreListener{};
    pw_stream *stream = nullptr;
    spa_hook streamListener{};
    // Read by the loop thread while a stream plays; replaced under its lock.
    QByteArray pcm;
    qsizetype offset = 0;
    bool draining = false;
    // Set from the loop thread when the PipeWire daemon goes away.
    std::atomic<bool> failed{false};

    static void coreError(void *data, uint32_t id, int, int result, const char *)
    {
        if (id == PW_ID_CORE && result == -EPIPE) static_cast<Output *>(data)->failed = true;
    }
    static void process(void *data)
    {
        auto *self = static_cast<Output *>(data);
        if (self->draining) return;
        pw_buffer *buffer = pw_stream_dequeue_buffer(self->stream);
        if (!buffer) return;
        spa_data &target = buffer->buffer->datas[0];
        qsizetype bytes = 0;
        if (target.data) {
            constexpr uint32_t stride = sizeof(qint16);
            uint32_t frames = target.maxsize / stride;
            if (buffer->requested) frames = qMin<uint32_t>(frames, uint32_t(buffer->requested));
            bytes = qMin<qsizetype>(self->pcm.size() - self->offset, qsizetype(frames) * stride);
            std::memcpy(target.data, self->pcm.constData() + self->offset, size_t(bytes));
            self->offset += bytes;
            target.chunk->offset = 0;
            target.chunk->stride = stride;
            target.chunk->size = uint32_t(bytes);
        }
        pw_stream_queue_buffer(self->stream, buffer);
        if (self->offset >= self->pcm.size()) {
            self->draining = true;
            pw_stream_flush(self->stream, true);
        }
    }
    static void drained(void *data)
    {
        auto *self = static_cast<Output *>(data);
        pw_stream_set_active(self->stream, false);
    }

    bool open()
    {
        pw_init(nullptr, nullptr);
        loop = pw_thread_loop_new("omaword-sound", nullptr);
        if (!loop) return false;
        context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
        if (!context || pw_thread_loop_start(loop) != 0) return false;
        pw_thread_loop_lock(loop);
        core = pw_context_connect(context, nullptr, 0);
        if (core) {
            static const pw_core_events events = [] {
                pw_core_events e{};
                e.version = PW_VERSION_CORE_EVENTS;
                e.error = coreError;
                return e;
            }();
            pw_core_add_listener(core, &coreListener, &events, this);
        }
        pw_thread_loop_unlock(loop);
        return core;
    }
    bool play(const QByteArray &cue)
    {
        static const pw_stream_events events = [] {
            pw_stream_events e{};
            e.version = PW_VERSION_STREAM_EVENTS;
            e.process = process;
            e.drained = drained;
            return e;
        }();
        pw_thread_loop_lock(loop);
        // A new cue replaces any cue still sounding.
        if (stream) pw_stream_destroy(stream);
        pcm = cue; offset = 0; draining = false;
        stream = pw_stream_new(core, "OmaWord sound effect", pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Notification", nullptr));
        int result = -ENOMEM;
        if (stream) {
            spa_zero(streamListener);
            pw_stream_add_listener(stream, &streamListener, &events, this);
            uint8_t space[256];
            spa_pod_builder builder;
            spa_pod_builder_init(&builder, space, sizeof space);
            spa_audio_info_raw info{};
            info.format = SPA_AUDIO_FORMAT_S16;
            info.rate = sampleRate;
            info.channels = 1;
            info.position[0] = SPA_AUDIO_CHANNEL_MONO;
            const spa_pod *params[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info)};
            result = pw_stream_connect(stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1);
        }
        pw_thread_loop_unlock(loop);
        return result == 0;
    }
    void stop()
    {
        pw_thread_loop_lock(loop);
        if (stream) pw_stream_destroy(stream);
        stream = nullptr;
        pw_thread_loop_unlock(loop);
    }
    ~Output()
    {
        if (loop) {
            pw_thread_loop_lock(loop);
            if (stream) pw_stream_destroy(stream);
            if (core) pw_core_disconnect(core);
            pw_thread_loop_unlock(loop);
            pw_thread_loop_stop(loop);
        }
        if (context) pw_context_destroy(context);
        if (loop) pw_thread_loop_destroy(loop);
        pw_deinit();
    }
};

SoundEffects::SoundEffects(QObject *parent, const QString &settingsPath, bool audioOutput)
    : QObject(parent), m_settingsPath(settingsPath), m_audioOutput(audioOutput)
{
    const QByteArray env = qgetenv("OMAWORD_SOUND").trimmed().toLower();
    m_silenced = env == "0" || env == "off" || env == "false";
    // Headless sessions (tests, CI) never open an audio device.
    if (QGuiApplication::platformName() == QStringLiteral("offscreen")) m_audioOutput = false;
    std::unique_ptr<QSettings> settings(openSettings(m_settingsPath));
    m_enabled = settings->value(settingsKey, true).toBool();
    m_reduceMotion = settings->value(motionKey, false).toBool();
    m_introduced = settings->value(introducedKey, false).toBool();
    const auto tones = baseTones();
    for (auto it = tones.cbegin(); it != tones.cend(); ++it) m_tones.insert(it.key(), scaled(it.value(), outputVolume));
}

SoundEffects::~SoundEffects() = default;

QByteArray SoundEffects::render(const QString &name)
{
    return baseTones().value(name);
}

QStringList SoundEffects::names()
{
    return {QStringLiteral("tick"), QStringLiteral("boundary"), QStringLiteral("chime"), QStringLiteral("remove"),
            QStringLiteral("select"), QStringLiteral("error")};
}

int SoundEffects::cooldownFor(const QString &name)
{
    return name == QStringLiteral("error") || name == QStringLiteral("boundary") ? burstCooldownMs : cooldownMs;
}

void SoundEffects::setEnabled(bool enabled)
{
    if (m_enabled == enabled) return;
    m_enabled = enabled;
    std::unique_ptr<QSettings> settings(openSettings(m_settingsPath));
    settings->setValue(settingsKey, enabled);
    settings->sync();
    if (!enabled && m_output) m_output->stop();
    emit enabledChanged();
}

void SoundEffects::setReduceMotion(bool reduce)
{
    if (m_reduceMotion == reduce) return;
    m_reduceMotion = reduce;
    std::unique_ptr<QSettings> settings(openSettings(m_settingsPath));
    settings->setValue(motionKey, reduce);
    settings->sync();
    emit reduceMotionChanged();
}

bool SoundEffects::ensureOutput()
{
    if (!m_audioOutput) return false;
    if (m_output && !m_output->failed) return true;
    // PipeWire missing or restarted: try again later, never give up for the session.
    if (m_outputAttempt.isValid() && m_outputAttempt.elapsed() < retryMs) return false;
    m_output.reset();
    m_outputAttempt.start();
    auto output = std::make_unique<Output>();
    if (!output->open()) return false;
    m_output = std::move(output);
    return true;
}

bool SoundEffects::play(const QString &name)
{
    if (!m_enabled) return false;
    const auto tone = m_tones.constFind(name);
    if (tone == m_tones.cend()) return false;
    m_lastSound = name; ++m_playCount;
    emit played();
    if (m_silenced) return false;
    QElapsedTimer &last = m_lastPlayed[name];
    if (last.isValid() && last.elapsed() < cooldownFor(name)) return false;
    last.start();
    if (!m_introduced) {
        m_introduced = true;
        std::unique_ptr<QSettings> settings(openSettings(m_settingsPath));
        settings->setValue(introducedKey, true);
        settings->sync();
        emit introduced();
    }
    if (ensureOutput() && !m_output->play(*tone)) m_output->failed = true;
    return true;
}
