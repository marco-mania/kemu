/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QCborArray>
#include <QCborMap>
#include <QMimeData>
#include <QtEndian>

#include <array>
#include <cstring>
#include <memory>

#include "clipboardbackend.h"
#include "frame.h"
#include "protocol.h"
#include "session.h"
#include "transport.h"

// A port whose host end the test plays.
class FakeTransport : public Transport
{
public:
    bool isConnected() const override
    {
        return m_connected;
    }
    void write(const QByteArray &data) override
    {
        if (m_connected)
            m_written += data;
    }
    qsizetype pendingBytes() const override
    {
        return m_pending;
    }

    void connectHost()
    {
        m_connected = true;
        Q_EMIT connected();
    }
    void disconnectHost()
    {
        m_connected = false;
        Q_EMIT disconnected();
    }
    void feed(const QByteArray &data)
    {
        Q_EMIT received(data);
    }
    void feedFrame(quint16 type, const QCborMap &body = {})
    {
        feed(encodeFrame(type, body));
    }
    // Pretends the port holds this much unsent output.
    void setPending(qsizetype pending)
    {
        m_pending = pending;
        if (pending == 0)
            Q_EMIT drained();
    }

    // What the agent wrote since the last call, as frames.
    QList<Frame> takeFrames()
    {
        FrameReader reader;
        reader.append(m_written);
        m_written.clear();
        QList<Frame> frames;
        Frame frame;
        while (reader.next(&frame) == FrameReader::Result::Frame)
            frames.append(frame);
        return frames;
    }
    QList<Frame> takeFrames(quint16 type)
    {
        QList<Frame> result;
        const QList<Frame> frames = takeFrames();
        for (const Frame &frame : frames) {
            if (frame.type == type)
                result.append(frame);
        }
        return result;
    }

private:
    bool m_connected = false;
    QByteArray m_written;
    qsizetype m_pending = 0;
};

// The guest clipboard, held in memory.
class FakeBackend : public ClipboardBackend
{
public:
    QString name() const override
    {
        return QStringLiteral("fake");
    }
    bool supportsPrimary() const override
    {
        return true;
    }
    const QMimeData *mimeData(Protocol::Selection selection) const override
    {
        return m_data[int(selection)].get();
    }
    void setMimeData(Protocol::Selection selection, QMimeData *data) override
    {
        m_data[int(selection)].reset(data);
        ++setCount;
        Q_EMIT changed(selection);
    }
    void clear(Protocol::Selection selection) override
    {
        m_data[int(selection)].reset();
        ++clearCount;
        Q_EMIT changed(selection);
    }

    // A guest application copies.
    void appCopy(Protocol::Selection selection, QMimeData *data)
    {
        m_data[int(selection)].reset(data);
        Q_EMIT changed(selection);
    }

    int setCount = 0;
    int clearCount = 0;

private:
    std::array<std::unique_ptr<QMimeData>, Protocol::SelectionCount> m_data;
};

inline QByteArray rawFrame(quint16 type, const QByteArray &body)
{
    QByteArray frame(Protocol::HeaderSize, Qt::Uninitialized);
    std::memcpy(frame.data(), Protocol::Magic, 4);
    qToLittleEndian<quint16>(type, frame.data() + 4);
    qToLittleEndian<quint16>(0, frame.data() + 6);
    qToLittleEndian<quint32>(quint32(body.size()), frame.data() + 8);
    return frame + body;
}

inline QCborMap welcomeBody(const QByteArray &nonce, const QStringList &capabilities, quint64 maxTransfer = 0)
{
    QCborMap body;
    body.insert(QLatin1StringView("nonce"), nonce);
    body.insert(QLatin1StringView("version"), 1);
    body.insert(QLatin1StringView("host"), QCborMap{{QStringLiteral("name"), QStringLiteral("KEMU")}, {QStringLiteral("version"), QStringLiteral("9.9")}});
    QCborArray caps;
    for (const QString &capability : capabilities)
        caps.append(capability);
    body.insert(QLatin1StringView("capabilities"), caps);
    if (maxTransfer)
        body.insert(QLatin1StringView("limits"), QCborMap{{QStringLiteral("max_transfer"), qint64(maxTransfer)}});
    return body;
}

inline QCborMap policyBody(bool toHost, bool toGuest, bool primary)
{
    return QCborMap{{QStringLiteral("clipboard"),
                     QCborMap{{QStringLiteral("to_host"), toHost}, {QStringLiteral("to_guest"), toGuest}, {QStringLiteral("primary"), primary}}}};
}

inline Session::Timing fastTiming()
{
    Session::Timing timing;
    timing.helloRetryMs = 100;
    timing.helloRetryMaxMs = 400;
    timing.helloMinIntervalMs = 0;
    timing.partialFrameMs = 100;
    timing.versionMismatchWaitMs = 60000;
    return timing;
}

// Plays the host through the handshake; returns the nonce it answered.
inline QByteArray completeHandshake(FakeTransport &transport, Session &session, const QStringList &capabilities, const QCborMap &policy)
{
    if (!transport.isConnected())
        transport.connectHost();
    session.start();
    const QList<Frame> hellos = transport.takeFrames(Protocol::Hello);
    if (hellos.isEmpty())
        return {};
    const QByteArray nonce = Fields::bytes(hellos.last().body, QLatin1StringView("nonce")).value_or(QByteArray());
    transport.feedFrame(Protocol::Welcome, welcomeBody(nonce, capabilities));
    transport.feedFrame(Protocol::Policy, policy);
    return nonce;
}
