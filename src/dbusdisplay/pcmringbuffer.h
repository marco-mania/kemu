/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QByteArray>

#include <cstring>

// =============================================================================
// PcmRingBuffer - fixed-size byte ring for PCM streaming
// =============================================================================
// Single producer, single consumer, both on the GUI thread - no locking.
//
// The buffer never grows. Once full it drops the oldest frames, which is the
// only sane policy for realtime audio: a backlog we cannot drain in time is
// latency, not sound. Dropping happens on frame boundaries, otherwise the
// channel order would shift for the rest of the stream.
//
// Two ways to consume:
//   read()           copies out into a caller-owned block - for the D-Bus
//                    Read reply on the capture side
//   peek()/discard() hands out the readable span in place - for pushing into
//                    a QIODevice that may accept less than offered
// =============================================================================

class PcmRingBuffer
{
public:
    // Allocates the ring. capacity is rounded up to a whole frame.
    void reset(int capacity, int frameSize = 1)
    {
        m_frameSize = qMax(1, frameSize);
        m_capacity = qMax(m_frameSize, capacity);
        m_capacity += (m_frameSize - m_capacity % m_frameSize) % m_frameSize;
        m_buffer.resize(m_capacity);
        clear();
    }

    void clear() noexcept
    {
        m_readPos = 0;
        m_writePos = 0;
        m_bufferedBytes = 0;
    }

    int capacity() const noexcept
    {
        return m_capacity;
    }
    int size() const noexcept
    {
        return m_bufferedBytes;
    }
    bool isEmpty() const noexcept
    {
        return m_bufferedBytes == 0;
    }

    // Returns how many bytes had to be dropped to make room - 0 in the
    // normal case, > 0 means the consumer is not keeping up.
    int write(const char *src, int len)
    {
        if (!src || len <= 0 || m_capacity <= 0)
            return 0;

        int dropped = 0;

        if (len > m_capacity) { // keep only the newest samples
            const int skip = alignUp(len - m_capacity);
            src += skip;
            len -= skip;
            dropped += skip;
        }

        const int overflow = m_bufferedBytes + len - m_capacity;
        if (overflow > 0) { // drop oldest to make room
            const int drop = qMin(alignUp(overflow), m_bufferedBytes);
            m_readPos = (m_readPos + drop) % m_capacity;
            m_bufferedBytes -= drop;
            dropped += drop;
        }

        char *dst = m_buffer.data();
        int done = 0;
        while (done < len) {
            const int chunk = qMin(len - done, m_capacity - m_writePos);
            memcpy(dst + m_writePos, src + done, chunk);
            m_writePos = (m_writePos + chunk) % m_capacity;
            done += chunk;
        }
        m_bufferedBytes += len;
        return dropped;
    }

    // Copies out up to maxLen bytes; returns what was actually available.
    int read(char *dst, int maxLen)
    {
        if (!dst || maxLen <= 0 || m_capacity <= 0)
            return 0;

        const int toRead = qMin(maxLen, m_bufferedBytes);
        const char *src = m_buffer.constData();
        int done = 0;
        while (done < toRead) {
            const int chunk = qMin(toRead - done, m_capacity - m_readPos);
            memcpy(dst + done, src + m_readPos, chunk);
            m_readPos = (m_readPos + chunk) % m_capacity;
            done += chunk;
        }
        m_bufferedBytes -= toRead;
        return toRead;
    }

    // Contiguous readable span. len is shorter than size() at the wrap point,
    // so an exhausting consumer needs at most two passes. The pointer stays
    // valid until the next write(); it is invalidated by discard() only in
    // the sense that the data behind it becomes free for reuse.
    const char *peek(int &len) const noexcept
    {
        len = qMin(m_bufferedBytes, m_capacity - m_readPos);
        return m_buffer.constData() + m_readPos;
    }

    void discard(int len) noexcept
    {
        len = qMin(len, m_bufferedBytes);
        if (len <= 0)
            return;
        m_readPos = (m_readPos + len) % m_capacity;
        m_bufferedBytes -= len;
    }

private:
    int alignUp(int bytes) const noexcept
    {
        return ((bytes + m_frameSize - 1) / m_frameSize) * m_frameSize;
    }

    QByteArray m_buffer;
    int m_capacity = 0;
    int m_frameSize = 1;
    int m_readPos = 0;
    int m_writePos = 0;
    int m_bufferedBytes = 0;
};
