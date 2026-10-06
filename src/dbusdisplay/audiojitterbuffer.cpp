/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "audiojitterbuffer.h"

#include <QMutexLocker>

#include <cmath>
#include <cstring>

namespace
{

// QEMU's sample format -> float. Native endianness; Init() rejects the rest.
void toFloat(QAudioFormat::SampleFormat fmt, const char *src, float *dst, size_t samples)
{
    switch (fmt) {
    case QAudioFormat::UInt8: {
        const auto *s = reinterpret_cast<const quint8 *>(src);
        for (size_t i = 0; i < samples; ++i)
            dst[i] = (float(s[i]) - 128.0f) * (1.0f / 128.0f);
        break;
    }
    case QAudioFormat::Int16:
        for (size_t i = 0; i < samples; ++i) {
            qint16 v;
            std::memcpy(&v, src + i * sizeof(v), sizeof(v));
            dst[i] = float(v) * (1.0f / 32768.0f);
        }
        break;
    case QAudioFormat::Int32:
        for (size_t i = 0; i < samples; ++i) {
            qint32 v;
            std::memcpy(&v, src + i * sizeof(v), sizeof(v));
            dst[i] = float(double(v) * (1.0 / 2147483648.0));
        }
        break;
    case QAudioFormat::Float:
        std::memcpy(dst, src, samples * sizeof(float));
        break;
    default:
        std::fill(dst, dst + samples, 0.0f);
        break;
    }
}

// float -> the sink's sample format, clipped.
void fromFloat(QAudioFormat::SampleFormat fmt, const float *src, char *dst, size_t samples)
{
    switch (fmt) {
    case QAudioFormat::UInt8: {
        auto *d = reinterpret_cast<quint8 *>(dst);
        for (size_t i = 0; i < samples; ++i)
            d[i] = quint8(std::lrint(qBound(-1.0f, src[i], 1.0f) * 127.0f) + 128);
        break;
    }
    case QAudioFormat::Int16:
        for (size_t i = 0; i < samples; ++i) {
            const auto v = qint16(std::lrint(qBound(-1.0f, src[i], 1.0f) * 32767.0f));
            std::memcpy(dst + i * sizeof(v), &v, sizeof(v));
        }
        break;
    case QAudioFormat::Int32:
        for (size_t i = 0; i < samples; ++i) {
            const auto v = qint32(std::llrint(double(qBound(-1.0f, src[i], 1.0f)) * 2147483647.0));
            std::memcpy(dst + i * sizeof(v), &v, sizeof(v));
        }
        break;
    case QAudioFormat::Float:
        std::memcpy(dst, src, samples * sizeof(float));
        break;
    default: // never chosen as sink format
        break;
    }
}

} // namespace

AudioJitterBuffer::AudioJitterBuffer(const QAudioFormat &inFormat, const QAudioFormat &outFormat, double targetMs, QObject *parent)
    : QIODevice(parent)
    , m_in(inFormat)
    , m_out(outFormat)
    , m_channels(qMax(1, inFormat.channelCount()))
{
    Q_ASSERT(inFormat.channelCount() == outFormat.channelCount());
    Q_ASSERT(inFormat.sampleRate() == outFormat.sampleRate());

    AudioJitterCore::Config cfg;
    cfg.channels = m_channels;
    cfg.sampleRate = inFormat.sampleRate();
    cfg.targetMs = targetMs;
    m_core.configure(cfg);
}

double AudioJitterBuffer::targetMsFromEnv()
{
    bool ok = false;
    const int ms = qEnvironmentVariableIntValue("KEMU_AUDIO_BUFFER_MS", &ok);
    return ok ? qBound(10, ms, 500) : 40;
}

void AudioJitterBuffer::push(const char *data, qint64 bytes)
{
    const int bpf = m_in.bytesPerFrame();
    if (!data || bpf <= 0)
        return;
    const qint64 frames = bytes / bpf;
    if (frames <= 0)
        return;

    const size_t samples = size_t(frames) * size_t(m_channels);

    QMutexLocker lock(&m_mutex);
    if (m_scratchIn.size() < samples)
        m_scratchIn.resize(samples);
    toFloat(m_in.sampleFormat(), data, m_scratchIn.data(), samples);
    m_core.push(m_scratchIn.data(), int(frames));
}

void AudioJitterBuffer::restartBuffering()
{
    QMutexLocker lock(&m_mutex);
    m_core.reset();
}

AudioJitterCore::Stats AudioJitterBuffer::takeStats()
{
    QMutexLocker lock(&m_mutex);
    return m_core.stats(true);
}

qint64 AudioJitterBuffer::bytesAvailable() const
{
    // Silence is always on offer, so a backend asking first never stalls.
    return QIODevice::bytesAvailable() + m_out.bytesForDuration(100'000);
}

qint64 AudioJitterBuffer::readData(char *data, qint64 maxlen)
{
    return pull(data, maxlen);
}

qint64 AudioJitterBuffer::pull(char *data, qint64 maxlen)
{
    const int bpf = m_out.bytesPerFrame();
    if (!data || bpf <= 0)
        return 0;
    const qint64 frames = maxlen / bpf;
    if (frames <= 0)
        return 0;

    const size_t samples = size_t(frames) * size_t(m_channels);

    QMutexLocker lock(&m_mutex);
    if (m_scratchOut.size() < samples)
        m_scratchOut.resize(samples);
    m_core.pull(m_scratchOut.data(), int(frames));
    fromFloat(m_out.sampleFormat(), m_scratchOut.data(), data, samples);
    return frames * bpf;
}
