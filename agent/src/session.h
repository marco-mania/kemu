/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QByteArray>
#include <QCborMap>
#include <QElapsedTimer>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include "frame.h"

using namespace Qt::StringLiterals;

class Transport;

// The agent's side of the connection lifecycle (section 5): handshake,
// resynchronisation, policy, keep-alive and goodbye. Feature messages
// arrive through message() once the session is READY.
class Session : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Waiting, // no host on the port
        Handshake,
        Ready,
        Idle, // the host said goodbye; wait for HELLO_REQUEST
    };
    Q_ENUM(State)

    struct Timing {
        int helloRetryMs = 2000;
        int helloRetryMaxMs = 30000;
        int helloMinIntervalMs = 1000;
        int partialFrameMs = 2000;
        int versionMismatchWaitMs = 5 * 60 * 1000;
    };

    // What HELLO tells the host about this agent.
    struct Identity {
        QString agentName = u"kemu-agent"_s;
        QString agentVersion;
        QStringList capabilities;
        QString osId;
        QString osVersionId;
        QString osPrettyName;
        QString sessionType; // "wayland" or "x11"
        QString desktop;
        QString clipboardBackend;
        quint64 maxTransfer = 0;
    };

    struct Policy {
        bool toHost = false;
        bool toGuest = false;
        bool primary = false;
        bool files = false; // section 6.8
        quint64 filesLimit = 0;
        bool operator==(const Policy &) const = default;
    };

    Session(Transport *transport, const Identity &identity, const Timing &timing, QObject *parent = nullptr);
    Session(Transport *transport, const Identity &identity, QObject *parent = nullptr);

    State state() const
    {
        return m_state;
    }
    Transport *transport() const
    {
        return m_transport;
    }
    void setIdentity(const Identity &identity)
    {
        m_identity = identity;
    }

    // Negotiated: listed by both sides.
    bool hasCapability(QLatin1StringView capability) const;
    QSet<QString> capabilities() const
    {
        return m_capabilities;
    }
    quint64 peerMaxTransfer() const
    {
        return m_peerMaxTransfer;
    }
    const Policy &policy() const
    {
        return m_policy;
    }
    QString hostName() const
    {
        return m_hostName;
    }
    QString hostVersion() const
    {
        return m_hostVersion;
    }

    // A message of this session. Dropped unless READY.
    void send(quint16 type, const QCborMap &body = {});
    // Ends the session from our side; the caller closes the port next.
    void sendGoodbye(const QString &reason);

    // Starts the handshake if the transport is connected already.
    void start();

Q_SIGNALS:
    void stateChanged(Session::State state);
    void ready();
    void policyChanged(const Session::Policy &previous);
    // Everything that belonged to the session is gone.
    void reset();
    void message(quint16 type, const QCborMap &body);

private:
    void onConnected();
    void onDisconnected();
    void onReceived(const QByteArray &data);
    void process();
    void handleFrame(const Frame &frame);
    void handleWelcome(const QCborMap &body);
    void handlePolicy(const QCborMap &body);
    void framingError(const QString &why);

    void beginHandshake();
    void sendHello();
    void scheduleHello(int delayMs);
    void scanForHandshake();
    void setState(State state);
    void resetSession();
    void write(quint16 type, const QCborMap &body);

    Transport *m_transport;
    Identity m_identity;
    Timing m_timing;
    State m_state = State::Waiting;
    FrameReader m_reader;

    QByteArray m_nonce;
    QTimer m_helloTimer;
    int m_helloBackoffMs = 0;
    QElapsedTimer m_lastHello;
    QTimer m_partialTimer;

    QSet<QString> m_capabilities;
    quint64 m_peerMaxTransfer = 0;
    Policy m_policy;
    QString m_hostName;
    QString m_hostVersion;
};
