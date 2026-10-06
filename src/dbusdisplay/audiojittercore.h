/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// =============================================================================
// AudioJitterCore - adaptive jitter buffer between two independent clocks
// =============================================================================
// QEMU pushes PCM in fixed blocks (480 frames by default) paced by its own
// timer; the host sink pulls at the sound card's rate. The two never agree:
// blocks arrive with jitter (guest/QEMU load, D-Bus, scheduling) and the
// clocks drift apart by tens to hundreds of ppm.
//
// This core absorbs both:
//
//   * Headroom. The controlled quantity is the fill level left over right
//     after the sink has taken its chunk - the reserve that has to bridge a
//     late block. Playback starts only once that reserve (targetMs) plus the
//     first chunk is queued, and after an underrun it prebuffers again.
//     The chunk size of the sink does not matter: it is added on top.
//
//   * Adaptive resampling. A PI controller nudges the playback ratio by at
//     most +-maxCorrection (default 0.5 %, a few cents - inaudible on program
//     material). Too full: consume slightly faster; too empty: slower. The
//     integral term ends up holding the clock drift, so the reserve settles
//     on the target instead of creeping towards an under- or overrun.
//
//   * Band-limited interpolation for the fractional read position: a
//     24-tap Kaiser-windowed sinc (256 phases, linearly blended). Better
//     than 80 dB SNR up to 15 kHz at 48 kHz, ~2.3 M MAC/s for stereo.
//
//     After an underrun or a resync the integral is frozen until the fill
//     has come back across the target, so recovering from a stall does not
//     get mistaken for drift.
//
//   * Safety nets: if a burst (e.g. after a host stall) piles up far more
//     than the target, it jumps back to the target in one step instead of
//     carrying the extra latency for many seconds. The ring is bounded and
//     drops the oldest audio on overflow.
//
// Samples are interleaved float. Not thread-safe; the caller serialises
// push() and pull().
// =============================================================================

class AudioJitterCore
{
public:
    struct Config {
        int channels = 2;
        int sampleRate = 48000;
        double targetMs = 40.0; // reserve kept after each sink pull
        double capacityMs = 1000.0; // hard ring size
        double maxCorrection = 0.005; // max relative rate deviation (0.5 %)
        double fadeInMs = 3.0; // ramp after (re)starting, avoids a click
    };

    struct Stats {
        double fillMs = 0.0; // smoothed reserve after a pull
        double minFillMs = 0.0; // lowest reserve while playing (this window)
        double correctionPpm = 0.0; // current rate correction
        uint64_t underruns = 0; // ran dry while playing (cumulative)
        uint64_t resyncs = 0; // backlog far too large, jumped back (cumulative)
        uint64_t overflowFrames = 0; // dropped because the ring was full
        bool playing = false;
    };

    // Interpolation kernel: kHalfTaps frames on either side of the read
    // position. Also the number of frames the core keeps as look-ahead.
    static constexpr int kHalfTaps = 12;
    static constexpr int kTaps = 2 * kHalfTaps;
    static constexpr int kPhases = 256;

    void configure(const Config &cfg)
    {
        m_cfg = cfg;
        m_cfg.channels = std::max(1, m_cfg.channels);
        m_cfg.sampleRate = std::max(1, m_cfg.sampleRate);
        m_targetFrames = std::max(1.0, m_cfg.targetMs * m_cfg.sampleRate / 1000.0);
        m_capacity = std::max(int(m_targetFrames * 4) + 4 * kTaps, int(m_cfg.capacityMs * m_cfg.sampleRate / 1000.0));
        m_fadeFrames = std::max(1, int(m_cfg.fadeInMs * m_cfg.sampleRate / 1000.0));

        // Loop tuning. The reserve integrates the rate error, so with P
        // control alone the loop settles with time constant targetSec / kp
        // (8 s at 40 ms and 0.5 %). The integral (time kp / ki = 30 s) takes
        // over the steady clock drift. With the 1.5 s reserve filter below
        // the phase margin is about 60 degrees, so nothing overshoots.
        m_kp = m_cfg.maxCorrection;
        m_ki = m_kp / 30.0;

        m_ring.assign(size_t(m_capacity) * m_cfg.channels, 0.0f);
        m_integral = 0.0;
        m_step = 1.0;
        m_stats = Stats();
        reset();
    }

    // Drops all queued audio and prebuffers again. Not counted as underrun;
    // meant for mute, disable and stream restarts. Keeps the drift estimate.
    void reset()
    {
        m_read = 0;
        m_count = 0;
        m_frac = 0.0;
        m_playing = false;
        m_recovering = false;
        m_holdSign = 0;
        m_fillAvg = m_targetFrames;
        m_minFill = -1.0;
        std::fill(m_ring.begin(), m_ring.end(), 0.0f);
    }

    void push(const float *interleaved, int frames)
    {
        if (!interleaved || frames <= 0 || m_capacity <= 0)
            return;
        const int ch = m_cfg.channels;
        // The frames behind the read position are still the kernel's
        // history, so they must not be overwritten.
        const int live = m_capacity - kHalfTaps;

        if (frames > live) { // keep only the newest
            m_stats.overflowFrames += uint64_t(frames - live);
            interleaved += size_t(frames - live) * ch;
            frames = live;
        }
        const int overflow = m_count + frames - live;
        if (overflow > 0) {
            advance(overflow);
            m_stats.overflowFrames += uint64_t(overflow);
        }

        int writePos = (m_read + m_count) % m_capacity;
        int done = 0;
        while (done < frames) {
            const int chunk = std::min(frames - done, m_capacity - writePos);
            std::memcpy(&m_ring[size_t(writePos) * ch], interleaved + size_t(done) * ch, size_t(chunk) * ch * sizeof(float));
            writePos = (writePos + chunk) % m_capacity;
            done += chunk;
        }
        m_count += frames;
    }

    // Always produces exactly 'frames' frames; silence while prebuffering or
    // after running dry.
    void pull(float *out, int frames)
    {
        if (!out || frames <= 0)
            return;
        const int ch = m_cfg.channels;
        const double dt = double(frames) / m_cfg.sampleRate;
        const double need = m_targetFrames + frames * m_step + kHalfTaps;

        if (!m_playing) {
            if (m_count < need) {
                std::memset(out, 0, size_t(frames) * ch * sizeof(float));
                return;
            }
            m_playing = true;
            m_fadeLeft = m_fadeFrames;
            m_frac = 0.0;
            m_fillAvg = m_targetFrames;
            if (m_recovering)
                holdIntegral();
            m_recovering = false;
        }

        // A backlog far beyond what this pull plus the reserve needs: drop
        // the excess at once (2x target or 100 ms of slack before acting).
        const double slack = std::max(2.0 * m_targetFrames, 0.1 * m_cfg.sampleRate);
        if (m_count > need + slack) {
            advance(m_count - int(need));
            m_frac = 0.0;
            m_fillAvg = m_targetFrames;
            ++m_stats.resyncs;
            holdIntegral();
        }

        const float *const table = kernelTable();
        for (int i = 0; i < frames; ++i) {
            if (m_count < kHalfTaps + 1) {
                // Ran dry: silence for the rest, prebuffer again. The
                // integral (drift estimate) survives; it is still valid.
                std::memset(out + size_t(i) * ch, 0, size_t(frames - i) * ch * sizeof(float));
                m_playing = false;
                m_recovering = true;
                ++m_stats.underruns;
                return;
            }

            // Kernel row for the fractional position, blended between the
            // two neighbouring phases.
            const double pos = m_frac * kPhases;
            int phase = int(pos);
            if (phase >= kPhases)
                phase = kPhases - 1;
            const float blend = float(pos - phase);
            const float *r0 = table + size_t(phase) * kTaps;
            const float *r1 = r0 + kTaps;
            float coeff[kTaps];
            for (int j = 0; j < kTaps; ++j)
                coeff[j] = r0[j] + blend * (r1[j] - r0[j]);

            float gain = 1.0f;
            if (m_fadeLeft > 0) {
                gain = 1.0f - float(m_fadeLeft) / float(m_fadeFrames);
                --m_fadeLeft;
            }

            float *dst = out + size_t(i) * ch;
            for (int c = 0; c < ch; ++c)
                dst[c] = 0.0f;
            int idx = (m_read - (kHalfTaps - 1) + m_capacity) % m_capacity;
            for (int j = 0; j < kTaps; ++j) {
                const float *src = &m_ring[size_t(idx) * ch];
                for (int c = 0; c < ch; ++c)
                    dst[c] += coeff[j] * src[c];
                if (++idx == m_capacity)
                    idx = 0;
            }
            if (gain != 1.0f)
                for (int c = 0; c < ch; ++c)
                    dst[c] *= gain;

            m_frac += m_step;
            const int whole = int(m_frac);
            if (whole > 0) {
                advance(whole);
                m_frac -= whole;
            }
        }

        // Measure where it matters: the reserve left after this pull.
        const double reserve = m_count - kHalfTaps - m_frac;
        if (m_minFill < 0.0 || reserve < m_minFill)
            m_minFill = reserve;
        updateController(reserve, dt);
    }

    int bufferedFrames() const
    {
        return m_count;
    }
    bool isPlaying() const
    {
        return m_playing;
    }

    // Snapshot; resetWindow restarts the minimum-reserve window.
    Stats stats(bool resetWindow = false)
    {
        Stats s = m_stats;
        const double toMs = 1000.0 / m_cfg.sampleRate;
        s.fillMs = m_fillAvg * toMs;
        s.minFillMs = m_minFill < 0.0 ? 0.0 : std::max(0.0, m_minFill) * toMs;
        s.correctionPpm = (m_step - 1.0) * 1e6;
        s.playing = m_playing;
        if (resetWindow)
            m_minFill = -1.0;
        return s;
    }

private:
    // (kPhases + 1) rows of kTaps coefficients, row p for fractional
    // position p / kPhases. Tap j weighs frame (read + j - (kHalfTaps - 1)).
    // Each row is normalised to unity DC gain.
    static const float *kernelTable()
    {
        static const std::vector<float> table = [] {
            constexpr double kCutoff = 0.95; // of Nyquist
            constexpr double kBeta = 8.0; // Kaiser window
            auto besselI0 = [](double x) {
                double sum = 1.0, term = 1.0;
                for (int k = 1; k < 50; ++k) {
                    term *= (x / (2 * k)) * (x / (2 * k));
                    sum += term;
                }
                return sum;
            };
            const double i0Beta = besselI0(kBeta);
            std::vector<float> t(size_t(kPhases + 1) * kTaps);
            double row[kTaps];
            for (int p = 0; p <= kPhases; ++p) {
                const double frac = double(p) / kPhases;
                double sum = 0.0;
                for (int j = 0; j < kTaps; ++j) {
                    const double d = (j - (kHalfTaps - 1)) - frac;
                    const double x = d / kHalfTaps;
                    const double w = std::fabs(x) >= 1.0 ? 0.0 : besselI0(kBeta * std::sqrt(1.0 - x * x)) / i0Beta;
                    const double s = d == 0.0 ? kCutoff : std::sin(M_PI * kCutoff * d) / (M_PI * d);
                    row[j] = s * w;
                    sum += row[j];
                }
                for (int j = 0; j < kTaps; ++j)
                    t[size_t(p) * kTaps + j] = float(row[j] / sum);
            }
            return t;
        }();
        return table.data();
    }

    void advance(int frames)
    {
        frames = std::min(frames, m_count);
        m_read = (m_read + frames) % m_capacity;
        m_count -= frames;
    }

    // After an underrun the late audio still arrives, on top of the reserve
    // rebuilt in the meantime; after a resync the burst that caused it is
    // usually not over. Either way the fill now sits off target for reasons
    // that say nothing about the clocks. The P term drains that, but while it
    // does the error keeps one sign for many seconds, and integrating it
    // would bend the drift estimate - with frequent stalls far enough to
    // starve the buffer and cause the next underrun. So the integral waits
    // until the fill has crossed the target once (or kHoldMaxSec passed).
    void holdIntegral()
    {
        m_holdSign = 0;
        m_holdLeft = kHoldMaxSec;
        m_holding = true;
    }

    void updateController(double reserve, double dt)
    {
        // Blocks arrive as a sawtooth; a 1.5 s low-pass leaves the trend.
        constexpr double kFillTau = 1.5;
        m_fillAvg += (reserve - m_fillAvg) * (1.0 - std::exp(-dt / kFillTau));

        const double err = (m_fillAvg - m_targetFrames) / m_targetFrames;
        const double maxCorr = m_cfg.maxCorrection;
        // Conditional integration: drift only ever shows up as a small,
        // persistent error. A large one is a transient (a stall and the
        // burst behind it) that the P term drains; integrating it would wind
        // the drift estimate up and starve the buffer for half a minute.
        constexpr double kIntegrateBelow = 0.25;
        if (m_holding) {
            m_holdLeft -= dt;
            constexpr double kSignificant = 0.05;
            if (m_holdSign == 0 && std::fabs(err) > kSignificant)
                m_holdSign = err > 0 ? 1 : -1; // the side the transient pushed it to
            if ((m_holdSign != 0 && err * m_holdSign <= 0.0) || m_holdLeft <= 0.0)
                m_holding = false;
        }
        if (!m_holding && std::fabs(err) < kIntegrateBelow)
            m_integral = std::clamp(m_integral + m_ki * err * dt, -maxCorr, maxCorr);
        const double corr = std::clamp(m_kp * err + m_integral, -maxCorr, maxCorr);
        m_step = 1.0 + corr;
    }

    Config m_cfg;
    std::vector<float> m_ring;
    int m_capacity = 0;
    int m_read = 0; // ring index of the frame at the read position
    int m_count = 0; // frames from the read position on
    double m_frac = 0.0; // fractional read position in [0, 1)
    double m_step = 1.0; // input frames consumed per output frame

    double m_targetFrames = 0.0;
    double m_fillAvg = 0.0;
    double m_minFill = -1.0;
    double m_integral = 0.0;
    double m_kp = 0.0;
    double m_ki = 0.0;

    static constexpr double kHoldMaxSec = 20.0;
    bool m_recovering = false; // prebuffering after an underrun
    bool m_holding = false; // integral frozen, see holdIntegral()
    int m_holdSign = 0;
    double m_holdLeft = 0.0;

    int m_fadeFrames = 1;
    int m_fadeLeft = 0;
    bool m_playing = false;

    Stats m_stats;
};
