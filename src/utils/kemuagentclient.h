/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QCborMap>
#include <QLocalSocket>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <optional>

#include "utils/kemuagentprotocol.h"

/**
 * @brief The host end of the KEMU agent protocol (docs/kemu-agent-protocol.md).
 *
 * kemu-agent runs in the guest's graphical session and talks to KEMU over
 * the virtio-serial port org.kde.kemu.agent.0. QEMU connects that port to
 * one end of a socketpair; this client owns the other end. Whether the guest
 * has opened the port is only known to QEMU, which reports it through QMP
 * (VSERPORT_CHANGE, query-chardev) - the owner passes that on with
 * setPortOpen(). A HELLO counts as an open port as well, since it may arrive
 * before the QMP event.
 *
 * The client runs the connection lifecycle of section 5: it answers HELLO
 * with WELCOME and POLICY, asks for a HELLO when none comes, resynchronises
 * after stale or broken input and pings an idle agent. Clipboard messages
 * are passed on through message(). Display layouts outlast a protocol
 * session, because the monitors do not move with a new HELLO, but not the
 * agent: once the guest closes the port, the layout is dropped.
 */
class KemuAgentClient : public QObject
{
    Q_OBJECT

public:
    enum class Status {
        Waiting, // the guest has not opened the port: no agent running
        Connecting, // the port is open, no session yet
        Connected,
        Unresponsive, // a session, but the agent does not answer pings
    };
    Q_ENUM(Status)

    struct Timing {
        int helloRequestDelayMs = 1000;
        int helloRequestRepeatMs = 10000;
        int partialFrameMs = 2000;
        int idlePingMs = 10000;
        int pongTimeoutMs = 10000;
    };

    // What HELLO told about the agent.
    struct AgentInfo {
        QString name;
        QString version;
        QString osPrettyName;
        QString sessionType; // "wayland" or "x11"
        QString desktop;
        QString clipboardBackend; // "ext-data-control", "wlr-data-control", "x11" or "none"

        bool operator==(const AgentInfo &) const = default;
    };

    struct Policy {
        bool toHost = false;
        bool toGuest = false;
        bool primary = false;
        bool files = false; // section 6.8
        quint64 filesLimit = 0;

        bool operator==(const Policy &) const = default;
    };

    explicit KemuAgentClient(QObject *parent = nullptr);
    explicit KemuAgentClient(const Timing &timing, QObject *parent = nullptr);
    ~KemuAgentClient() override;

    // Takes ownership of @p socketFd, one end of a connected socketpair.
    void connectToVm(int socketFd);

    // From QMP: whether the guest side of the port is open.
    void setPortOpen(bool open);

    // The guest was put back to an earlier state: whatever its agent last
    // heard is stale. Ends the session and asks for a new HELLO.
    void resync();

    [[nodiscard]] Status status() const
    {
        return m_status;
    }
    // A session is running (Connected or Unresponsive).
    [[nodiscard]] bool hasSession() const
    {
        return m_ready;
    }
    [[nodiscard]] AgentInfo agentInfo() const
    {
        return m_info;
    }
    // Listed by both sides.
    [[nodiscard]] bool hasCapability(QLatin1StringView capability) const;
    // Largest clipboard item the agent accepts.
    [[nodiscard]] quint64 peerMaxTransfer() const
    {
        return m_peerMaxTransfer;
    }

    // The last layout the agent reported, in this session or an earlier one
    // while the port stayed open.
    [[nodiscard]] std::optional<KemuAgentProtocol::Layout> layout() const
    {
        return m_layout;
    }

    // Sent right away in a session, otherwise with the next WELCOME.
    void setPolicy(const Policy &policy);
    [[nodiscard]] Policy policy() const
    {
        return m_policy;
    }

    // A message of the session. Dropped without one.
    void send(quint16 type, const QCborMap &body = {});
    // Bytes queued for the port.
    [[nodiscard]] qsizetype pendingBytes() const;

    static constexpr quint64 kMaxTransfer = KemuAgentProtocol::DefaultMaxTransfer;

Q_SIGNALS:
    void statusChanged(KemuAgentClient::Status status);
    void agentInfoChanged();
    // WELCOME and POLICY are out; clipboard and layout may start.
    void sessionStarted();
    // Everything that belonged to the session is gone.
    void sessionEnded();
    void layoutChanged();
    // A clipboard message of the session.
    void message(quint16 type, const QCborMap &body);
    // The socket took all queued bytes.
    void drained();

private:
    void onReadyRead();
    void process();
    void handleFrame(const KemuAgentProtocol::Frame &frame);
    void handleHello(const QCborMap &body);
    void framingError(const QString &why);
    void scanForHello();
    void endSession();
    void sendHelloRequest();
    void write(quint16 type, const QCborMap &body);
    void sendPolicy();
    void setStatus(Status status);
    void noteActivity();

    Timing m_timing;
    QLocalSocket m_socket;
    KemuAgentProtocol::FrameReader m_reader;

    Status m_status = Status::Waiting;
    bool m_portOpen = false;
    bool m_ready = false;
    AgentInfo m_info;
    QSet<QString> m_capabilities;
    quint64 m_peerMaxTransfer = 0;
    Policy m_policy;
    std::optional<KemuAgentProtocol::Layout> m_layout;
    quint64 m_nextPingId = 0;

    QTimer m_helloRequest;
    QTimer m_partial;
    QTimer m_idle;
    QTimer m_pongTimeout;
};
