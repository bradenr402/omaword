#include "soundeffects.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QBuffer>
#include <QGuiApplication>
#include <QMediaDevices>
#include <QSettings>
#include <QtMath>
#include <cmath>
#include <memory>

namespace {
constexpr int sampleRate = 44100;
const char *settingsKey = "sound/effectsEnabled";
const char *motionKey = "motion/reduced";

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

// Deterministic white noise in [-1, 1).
struct Noise {
    quint32 seed;
    double next() { seed = seed * 1664525u + 1013904223u; return double(seed >> 8) / double(1u << 24) * 2.0 - 1.0; }
};

// RBJ band-pass biquad (constant 0 dB peak); coefficients refreshed per sample
// so the centre can sweep without zipper noise at this rate.
struct BandPass {
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    double run(double x, double hz, double q)
    {
        const double w = 2 * M_PI * hz / sampleRate, alpha = qSin(w) / (2 * q), a0 = 1 + alpha;
        const double y = (alpha * x - alpha * x2 + 2 * qCos(w) * y1 - (1 - alpha) * y2) / a0;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
};

double smoothstep(double t) { t = qBound(0.0, t, 1.0); return t * t * (3 - 2 * t); }

// Raised-cosine fade over the first and last few milliseconds, then DC removal.
void finish(QVector<double> &buffer, double fadeMs = 5)
{
    double mean = 0;
    for (double v : buffer) mean += v;
    mean /= qMax<qsizetype>(1, buffer.size());
    for (double &v : buffer) v -= mean;
    const int fade = qMin<int>(buffer.size() / 2, int(sampleRate * fadeMs / 1000));
    for (int i = 0; i < fade; ++i) {
        const double g = 0.5 - 0.5 * qCos(M_PI * i / fade);
        buffer[i] *= g; buffer[buffer.size() - 1 - i] *= g;
    }
}

QByteArray toPcm(const QVector<double> &buffer, double peak)
{
    double maximum = 1e-9;
    for (double v : buffer) maximum = qMax(maximum, qAbs(v));
    QByteArray pcm(buffer.size() * qsizetype(sizeof(qint16)), '\0');
    auto *out = reinterpret_cast<qint16 *>(pcm.data());
    for (qsizetype i = 0; i < buffer.size(); ++i)
        out[i] = qint16(qRound(qBound(-1.0, buffer[i] / maximum * peak, 1.0) * 32767));
    return pcm;
}

// One page turning: band-passed noise whose centre rises ~1.5k→4k Hz while the
// page lifts and falls back to ~2k as it lands, with a soft flutter and a few
// small crinkles. Pure noise, so there is no pitch to hear as a beep.
QVector<double> pageSwish(double ms, quint32 seed, double brightness = 1.0)
{
    const int samples = int(sampleRate * ms / 1000.0);
    QVector<double> out(samples, 0.0);
    Noise noise{seed};
    BandPass body, air;
    double soft = 0; // one-pole low-pass that rounds off the hiss above ~6 kHz
    // Crinkle onsets as fractions of the swish, jittered by the seed.
    double crinkles[4];
    for (int c = 0; c < 4; ++c) crinkles[c] = 0.18 + 0.17 * c + 0.05 * noise.next();
    for (int i = 0; i < samples; ++i) {
        const double t = double(i) / samples;
        const double rise = smoothstep(t / 0.4), fall = smoothstep((t - 0.4) / 0.6);
        const double centre = brightness * (1500 + 2500 * rise - 2000 * fall);
        const double n = noise.next();
        soft += 0.58 * (n - soft);
        const double v = body.run(soft, centre, 1.1) + 0.18 * air.run(soft, centre * 1.8, 1.4);
        // Fast (~15 ms) attack, then a soft fall.
        double envelope = smoothstep(t / 0.07) * qPow(1.0 - t, 1.6);
        envelope *= 1.0 + 0.18 * qSin(2 * M_PI * 23 * t * ms / 1000.0) * qSin(M_PI * t);
        for (double onset : crinkles) {
            const double d = (t - onset) * ms; // ms since this crinkle
            if (d >= 0 && d < 9) envelope += 0.35 * qExp(-d / 2.2) * smoothstep(d / 0.6) * (1.0 - t);
        }
        out[i] = v * envelope;
    }
    return out;
}

void addInto(QVector<double> &into, const QVector<double> &from, int offset, double gain)
{
    if (into.size() < offset + from.size()) into.resize(offset + from.size(), 0.0);
    for (qsizetype i = 0; i < from.size(); ++i) into[offset + i] += from[i] * gain;
}

QByteArray pageTurnPcm()
{
    QVector<double> swish = pageSwish(220, 0x9e3779b9u);
    finish(swish);
    return toPcm(swish, 0.42);
}

// Many pages riffling past and settling: overlapping swishes (more and longer
// with weight), muffled toward the end, onto a short cushioned low thud.
QByteArray thunkPcm(double weight)
{
    weight = qBound(0.05, weight, 1.0);
    const int pages = 2 + qRound(weight * 5);  // 2–7
    const double swishMs = 70 + 40 * weight;   // 70–110 ms each
    const double spacingMs = 26 + 6 * weight;
    QVector<double> out;
    for (int p = 0; p < pages; ++p) {
        const double muffle = 1.0 - 0.35 * double(p) / qMax(1, pages - 1);
        QVector<double> swish = pageSwish(swishMs, 0x2545f491u + 7919u * p, 0.85 * muffle);
        addInto(out, swish, int(sampleRate * p * spacingMs / 1000), 0.55 + 0.1 * (p % 2));
    }
    // The stack lands as the last page settles.
    const int thudStart = int(sampleRate * ((pages - 1) * spacingMs + swishMs * 0.55) / 1000);
    const int thudSamples = int(sampleRate * 0.11);
    QVector<double> thud(thudSamples, 0.0);
    Noise noise{0x51ed270bu};
    BandPass cushion;
    double phase = 0;
    for (int i = 0; i < thudSamples; ++i) {
        const double t = double(i) / thudSamples;
        phase += 2 * M_PI * (115 - 35 * t) / sampleRate;
        const double envelope = smoothstep(t / 0.04) * qExp(-5.0 * t) * (1.0 - t);
        thud[i] = (qSin(phase) * 0.8 + cushion.run(noise.next(), 220, 0.7) * 1.6) * envelope;
    }
    addInto(out, thud, thudStart, 0.3 + 0.15 * weight);
    finish(out);
    return toPcm(out, 0.22 + 0.2 * weight);
}

QHash<QString, QByteArray> baseTones()
{
    QHash<QString, QByteArray> tones;
    tones.insert(QStringLiteral("tick"), synthesize({{1900, 1700, 18, 0.10}}));
    tones.insert(QStringLiteral("boundary"), synthesize({{420, 380, 60, 0.12}}));
    tones.insert(QStringLiteral("chime"), synthesize({{880, 880, 70, 0.09}, {1320, 1320, 110, 0.08}}));
    tones.insert(QStringLiteral("remove"), synthesize({{740, 520, 110, 0.10}}));
    tones.insert(QStringLiteral("error"), synthesize({{170, 110, 90, 0.20}}));
    tones.insert(QStringLiteral("pageTurn"), pageTurnPcm());
    return tones;
}

QSettings *openSettings(const QString &path)
{
    return path.isEmpty() ? new QSettings : new QSettings(path, QSettings::IniFormat);
}
}

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
    m_tones = baseTones();
    // Thunks are pre-rendered per weight step: heavier jumps last longer.
    for (int step = 1; step <= thunkSteps; ++step)
        m_thunks.append(thunkPcm(double(step) / thunkSteps));
    m_tones.insert(QStringLiteral("thunk"), m_thunks.last());
}

SoundEffects::~SoundEffects()
{
    if (m_sink) m_sink->stop();
}

QByteArray SoundEffects::render(const QString &name, double weight)
{
    if (name == QStringLiteral("thunk")) return thunkPcm(weight);
    return baseTones().value(name);
}

QStringList SoundEffects::names()
{
    return {QStringLiteral("tick"), QStringLiteral("boundary"), QStringLiteral("chime"), QStringLiteral("remove"),
            QStringLiteral("error"), QStringLiteral("pageTurn"), QStringLiteral("thunk")};
}

void SoundEffects::setEnabled(bool enabled)
{
    if (m_enabled == enabled) return;
    m_enabled = enabled;
    std::unique_ptr<QSettings> settings(openSettings(m_settingsPath));
    settings->setValue(settingsKey, enabled);
    settings->sync();
    if (!enabled && m_sink) m_sink->stop();
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
    if (m_sink) return true;
    if (m_outputFailed || !m_audioOutput) return false;
    const auto device = QMediaDevices::defaultAudioOutput();
    QAudioFormat format;
    format.setSampleRate(sampleRate);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);
    if (device.isNull() || !device.isFormatSupported(format)) { m_outputFailed = true; return false; }
    m_sink = new QAudioSink(device, format, this);
    m_sink->setVolume(0.6);
    m_buffer = new QBuffer(this);
    return true;
}

bool SoundEffects::play(const QString &name, double weight)
{
    if (!m_enabled) return false;
    const auto tone = m_tones.constFind(name);
    if (tone == m_tones.cend()) return false;
    weight = std::isfinite(weight) ? qBound(0.05, weight, 1.0) : 1.0;
    m_lastSound = name; m_lastWeight = weight; ++m_playCount;
    emit played();
    if (m_silenced) return false;
    QElapsedTimer &last = m_lastPlayed[name];
    if (last.isValid() && last.elapsed() < cooldownMs) return false;
    last.start();
    if (!ensureOutput()) return true;
    // A new cue replaces any cue still sounding; QAudioSink pulls asynchronously.
    m_sink->stop();
    m_buffer->close();
    const bool thunk = name == QStringLiteral("thunk");
    QByteArray pcm = thunk ? m_thunks.at(qBound(0, qCeil(weight * thunkSteps) - 1, thunkSteps - 1)) : *tone;
    if (!thunk && weight < 1.0) {
        auto *samples = reinterpret_cast<qint16 *>(pcm.data());
        for (qsizetype i = 0; i < pcm.size() / qsizetype(sizeof(qint16)); ++i) samples[i] = qint16(samples[i] * weight);
    }
    m_buffer->setData(pcm);
    m_buffer->open(QIODevice::ReadOnly);
    m_sink->start(m_buffer);
    if (m_sink->error() != QAudio::NoError) { m_sink->stop(); m_outputFailed = true; delete m_sink; m_sink = nullptr; }
    return true;
}
