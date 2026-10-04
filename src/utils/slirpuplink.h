/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QByteArray>

#include "utils/virtualswitch.h"

class SlirpWorkerThread;

/**
 * @brief NAT uplink for a virtual network, backed by an embedded libslirp
 * instance.
 *
 * One instance per virtual network, or one per VM for user-mode
 * networking (UserModeNetwork). The slirp stack terminates guest TCP/UDP
 * connections in userspace and re-originates them on the host - no root
 * rights, no kernel interfaces, no namespaces. For virtual networks,
 * libslirp's own DHCP is disabled (DhcpServer owns address assignment);
 * for user-mode networking it hands out the guest address itself
 * (Config::enableDhcp). Its DNS proxy at the configured nameserver
 * address stays active in both cases.
 *
 * The slirp event loop runs on a dedicated worker thread; receiveFrame() and
 * addPortForward() are thread-safe and marshal into it.
 */
class SlirpUplink : public VirtualSwitchPort
{
    Q_OBJECT

public:
    struct Config {
        quint32 network = 0; // host byte order
        quint32 netmask = 0;
        quint32 gateway = 0; // exposed to guests as default route
        quint32 nameserver = 0; // slirp DNS proxy address
        bool enableDhcp = false; // slirp's own DHCP server (user-mode networking)
        quint32 dhcpStart = 0; // first DHCP address; 0 -> network + 15 (slirp default)
        bool allowHostLoopback = false; // gateway address reaches the host's 127.0.0.1
    };

    explicit SlirpUplink(QObject *parent = nullptr);
    ~SlirpUplink() override;

    bool start(const Config &config);
    void stop();
    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] QString lastError() const
    {
        return m_lastError;
    }

    /// Forward 127.0.0.1:hostPort on the host to guestIp:guestPort.
    void addPortForward(bool udp, quint16 hostPort, quint32 guestIp, quint16 guestPort);

    void receiveFrame(const QByteArray &frame) override;
    [[nodiscard]] bool ownsMac(const QByteArray &mac) const override;

Q_SIGNALS:
    void errorOccurred(const QString &message);

private:
    SlirpWorkerThread *m_thread = nullptr;
    Config m_config;
    QString m_lastError;
};
