/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QAudioFormat>
#include <QIODevice>
#include <QMutex>

#include <vector>

#include "dbusdisplay/audiojittercore.h"

// =============================================================================
// AudioJitterBuffer - adaptive buffer between two audio clocks
// =============================================================================
// Audio goes in through push() (in the producer's sample format) and comes
// out through pull() (in the consumer's format). In between sits the
// adaptive AudioJitterCore, which keeps a fixed reserve and follows the
// clock drift by resampling. Used in both directions:
//
//   playback  QEMU's Write() blocks -> push(), QAudioSink pulls through
//             readData() (the device is opened ReadOnly and handed to it)
//   capture   QAudioSource's readyRead -> push(), QEMU's Read() -> pull()
//
// The buffer never runs dry from the consumer's point of view: while the core
// is prebuffering it hands out silence. That keeps a sink in ActiveState and
// its clock running, instead of falling into IdleState and restarting on
// every hiccup; QEMU's Read() always gets the size it asked for.
//
// Thread safety: every public method may be called from any thread;
// readData() runs on whatever thread the Qt audio backend reads from.
// One mutex guards the core; it is held for microseconds.
// =============================================================================

class AudioJitterBuffer : public QIODevice
{
public:
    AudioJitterBuffer(const QAudioFormat &inFormat, const QAudioFormat &outFormat, double targetMs, QObject *parent = nullptr);

    // Reserve against producer jitter, from KEMU_AUDIO_BUFFER_MS (10..500,
    // default 40). Every millisecond here is a millisecond of latency, and
    // every millisecond missing is a crackle under load.
    static double targetMsFromEnv();

    // Whole frames only; a trailing partial frame is dropped.
    void push(const char *data, qint64 bytes);
    // Fills whole frames of maxlen and returns the bytes written; never short
    // by more than a partial frame (silence while prebuffering).
    qint64 pull(char *data, qint64 maxlen);
    void restartBuffering(); // drop queued audio, prebuffer again
    AudioJitterCore::Stats takeStats();

    bool isSequential() const override
    {
        return true;
    }
    qint64 bytesAvailable() const override;

protected:
    qint64 readData(char *data, qint64 maxlen) override;
    qint64 writeData(const char *, qint64) override
    {
        return -1;
    }

private:
    QAudioFormat m_in;
    QAudioFormat m_out;
    int m_channels = 0;

    mutable QMutex m_mutex;
    AudioJitterCore m_core;
    std::vector<float> m_scratchIn;
    std::vector<float> m_scratchOut;
};
