/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// The guest end of the kemu-agent port for the tests: what kemu-agent
// would send, written by hand, and every frame KEMU sends back.

#include <QCborArray>
#include <QLocalSocket>
#include <QObject>
#include <QTest>

#include <algorithm>

#include "utils/kemuagentprotocol.h"

using namespace Qt::StringLiterals;

namespace ScriptedAgentDefaults
{
inline const QByteArray Nonce("0123456789abcdef");
}

class ScriptedAgent : public QObject
{
public:
    explicit ScriptedAgent(int fd)
    {
        m_socket.setSocketDescriptor(fd, QLocalSocket::ConnectedState);
        connect(&m_socket, &QLocalSocket::readyRead, this, [this] {
            m_reader.append(m_socket.readAll());
            for (;;) {
                KemuAgentProtocol::Frame frame;
                QString error;
                const auto result = m_reader.next(&frame, &error);
                if (result == KemuAgentProtocol::FrameReader::Result::Error)
                    qFatal("KEMU sent a broken frame: %s", qPrintable(error));
                if (result != KemuAgentProtocol::FrameReader::Result::Frame)
                    break;
                frames.append(frame);
            }
        });
    }

    void send(quint16 type, const QCborMap &body = {})
    {
        sendRaw(KemuAgentProtocol::encodeFrame(type, body));
    }
    void sendRaw(const QByteArray &bytes)
    {
        m_socket.write(bytes);
        m_socket.flush();
    }

    void hello(const QStringList &capabilities = {u"clipboard"_s, u"display.layout"_s},
               const QList<int> &versions = {1},
               const QByteArray &nonce = ScriptedAgentDefaults::Nonce,
               qint64 maxTransfer = 16 * 1024 * 1024)
    {
        QCborArray caps;
        for (const QString &capability : capabilities)
            caps.append(capability);
        QCborArray vs;
        for (int version : versions)
            vs.append(version);
        send(KemuAgentProtocol::Hello,
             QCborMap{
                 {u"nonce"_s, nonce},
                 {u"versions"_s, vs},
                 {u"agent"_s, QCborMap{{u"name"_s, u"kemu-agent"_s}, {u"version"_s, u"0.1.0"_s}}},
                 {u"os"_s, QCborMap{{u"id"_s, u"fedora"_s}, {u"pretty_name"_s, u"Fedora Linux 43"_s}}},
                 {u"session"_s, QCborMap{{u"type"_s, u"wayland"_s}, {u"desktop"_s, u"KDE"_s}}},
                 {u"backends"_s, QCborMap{{u"clipboard"_s, u"ext-data-control"_s}, {u"layout"_s, u"wayland"_s}}},
                 {u"capabilities"_s, caps},
                 {u"limits"_s, QCborMap{{u"max_transfer"_s, maxTransfer}}},
             });
    }

    // Removes and returns the frames of this type that arrived so far.
    QList<KemuAgentProtocol::Frame> take(quint16 type)
    {
        QList<KemuAgentProtocol::Frame> result;
        for (auto it = frames.begin(); it != frames.end();) {
            if (it->type == type) {
                result.append(*it);
                it = frames.erase(it);
            } else {
                ++it;
            }
        }
        return result;
    }
    // Waits for the next frame of this type and removes it.
    std::optional<KemuAgentProtocol::Frame> next(quint16 type, int timeoutMs = 2000)
    {
        if (!QTest::qWaitFor(
                [&] {
                    return has(type);
                },
                timeoutMs))
            return std::nullopt;
        const auto it = std::find_if(frames.begin(), frames.end(), [type](const KemuAgentProtocol::Frame &frame) {
            return frame.type == type;
        });
        const KemuAgentProtocol::Frame frame = *it;
        frames.erase(it);
        return frame;
    }
    bool has(quint16 type) const
    {
        return std::any_of(frames.cbegin(), frames.cend(), [type](const KemuAgentProtocol::Frame &frame) {
            return frame.type == type;
        });
    }

    QList<KemuAgentProtocol::Frame> frames;

private:
    QLocalSocket m_socket;
    KemuAgentProtocol::FrameReader m_reader;
};
