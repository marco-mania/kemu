/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>
#include <QString>

#include <memory>

#include "utils/vmconfig.h"

class NetworkThread;
class SlirpUplink;
class VirtualSwitch;

/**
 * @brief Per-VM user-mode networking, entirely inside the KEMU process.
 *
 * Replaces the external passt helper: an in-memory L2 switch listens on a
 * unix socket that QEMU connects to via "-netdev stream", and a SlirpUplink
 * port terminates the traffic. The addressing follows the slirp (and QEMU
 * "-netdev user") conventions: network 10.0.2.0/24, gateway/DHCP 10.0.2.2,
 * DNS proxy 10.0.2.3, first DHCP lease 10.0.2.15.
 *
 * Switch and uplink live on their own NetworkThread, so the VM's traffic
 * does not share the GUI thread.
 *
 * No external process, no root rights, no namespaces - so user-mode
 * networking also works inside sandboxes (Flatpak), where passt's
 * unshare() is blocked.
 *
 * Port forwards bind 127.0.0.1:hostPort on the host and target the first
 * DHCP address; guests with static addresses outside the DHCP range keep
 * connectivity but are not reachable through forwards (same assumption
 * QEMU's hostfwd makes).
 */
class UserModeNetwork : public QObject
{
    Q_OBJECT

public:
    explicit UserModeNetwork(QObject *parent = nullptr);
    ~UserModeNetwork() override;

    /// Start switch and slirp uplink; apply port forwards.
    bool start(const QString &socketPath, const QList<PortForwardRule> &portForwards);
    void stop();
    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] QString lastError() const
    {
        return m_lastError;
    }

Q_SIGNALS:
    void errorOccurred(const QString &message);

private:
    std::unique_ptr<NetworkThread> m_thread;
    VirtualSwitch *m_switch = nullptr;
    SlirpUplink *m_uplink = nullptr;
    QString m_socketPath;
    QString m_lastError;
};
