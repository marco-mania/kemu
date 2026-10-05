/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

#include <memory>

#include <KConfigGroup>
#include <KLocalizedString>
#include <KSharedConfig>

#include "utils/virtualnetwork.h"

using namespace Qt::StringLiterals;

class DhcpServer;
class DnsServer;
class GatewayResponder;
class NetworkThread;
class SlirpUplink;
class VirtualSwitch;

/**
 * @brief Manages virtual networks backed by an in-memory L2 switch.
 *
 * Architecture overview (per network, entirely inside the KEMU process):
 *
 *   ┌─── KEMU process ───────────────────────────────────────────────┐
 *   │                                                                │
 *   │   VirtualSwitch (one port per VM, bound to the VM's MAC)       │
 *   │    ├── socketpair ←─────── QEMU VM 1 (-netdev stream, fd)      │
 *   │    ├── socketpair ←─────── QEMU VM 2                           │
 *   │    ├── DhcpServer         (internal port, optional)            │
 *   │    ├── DnsServer          (internal port, optional)            │
 *   │    ├── SlirpUplink        (internal port, optional NAT)        │
 *   │    └── GatewayResponder   (internal port, only without NAT)    │
 *   │                                                                │
 *   └────────────────────────────────────────────────────────────────┘
 *
 * Each running network owns a NetworkThread; the switch and its internal
 * ports live there, so guest traffic does not share the GUI thread.
 *
 * No root rights, no kernel interfaces and no namespaces are involved, so
 * virtual networks work unchanged inside sandboxes (Flatpak) and on
 * immutable systems.
 *
 * Lifecycle:
 *   1. startNetwork()  → switch created; DHCP / uplink ports attach
 *   2. attachVm()      → opens the VM's port, returns its socket path
 *   3. detachVm()      → closes the VM's port, decrements the ref count
 *   4. stopNetwork()   → closes the switch; QEMU clients drop off
 */
class VirtualNetworkManager : public QObject
{
    Q_OBJECT

public:
    explicit VirtualNetworkManager(QObject *parent = nullptr);
    ~VirtualNetworkManager() override;

    // --- KConfig Persistence ---

    void load();
    void save() const;

    // --- Network Definitions (CRUD) ---

    [[nodiscard]] QList<VirtualNetwork> networks() const;
    [[nodiscard]] VirtualNetwork network(const QString &name) const;
    [[nodiscard]] QStringList networkNames() const;

    bool addNetwork(const VirtualNetwork &net);
    bool removeNetwork(const QString &name);
    bool updateNetwork(const VirtualNetwork &net);

    // --- Runtime Control ---

    /**
     * Start the virtual network: the in-memory switch begins listening and
     * the configured internal ports (DHCP, NAT uplink or gateway responder)
     * attach to it.
     *
     * @return true if the network is running after this call.
     */
    bool startNetwork(const QString &name);

    /// Stop the virtual network and disconnect all attached VMs.
    void stopNetwork(const QString &name);

    [[nodiscard]] bool isRunning(const QString &name) const;

    // --- Per-VM Integration ---

    /**
     * Register a VM with the network. Call this before launching QEMU.
     *
     * @param dnsHostname Hostname for the in-process DNS server
     *        ("<hostname>.<dns domain>"), may be empty.
     * @param macAddress  MAC of the VM's NIC. The VM's switch port only
     *        accepts frames from this MAC; it also maps the hostname to the
     *        DHCP-assigned address.
     * @return The descriptor for the VM's switch port
     *         ("-netdev stream,addr.type=fd,addr.str="), or -1 on failure.
     *         The switch owns it: hand it to the guest process, then call
     *         releaseGuestFd(), and never close it directly.
     */
    int attachVm(const QString &networkName, const QString &vmName, const QString &dnsHostname, const QByteArray &macAddress);

    /// Closes the switch's copy of the VM's port descriptor once the guest
    /// process has inherited it.
    void releaseGuestFd(const QString &networkName, const QString &vmName);

    /**
     * Remove the VM from the network and decrement the ref count.
     * If the ref count reaches zero, the network is stopped automatically.
     */
    void detachVm(const QString &networkName, const QString &vmName);

    /// Detaches the VM from whichever running network holds it. Used on paths
    /// that have to release an attachment without knowing (or trusting) the
    /// VM's current configuration.
    void detachVmFromAll(const QString &vmName);

    [[nodiscard]] int connectedVmCount(const QString &name) const;

    [[nodiscard]] QString lastError() const;

Q_SIGNALS:
    void networkStarted(const QString &name);
    void networkStopped(const QString &name);
    void networksChanged();

    /// Emitted when a VM has been registered with a network.
    void vmAttached(const QString &networkName, const QString &vmName);

    /// Emitted when a VM has been removed from a network.
    void vmDetached(const QString &networkName, const QString &vmName);

    /// Emitted when a runtime error occurs (e.g. the switch cannot listen).
    void errorOccurred(const QString &networkName, const QString &message);

private:
    struct RuntimeState {
        // Owns the thread the switch and all ports below live on.
        std::shared_ptr<NetworkThread> thread;
        QPointer<VirtualSwitch> vswitch;
        QPointer<DhcpServer> dhcp;
        QPointer<DnsServer> dns;
        QPointer<GatewayResponder> gatewayResponder;
        QPointer<SlirpUplink> uplink;
        QHash<QString, QByteArray> attachedVms; // vm name -> NIC MAC
    };

    QList<VirtualNetwork> m_networks;
    QHash<QString, RuntimeState> m_runtime;

    // --- Internal Helpers ---

    /// Reports an error and returns false for names that cannot be used as a
    /// configuration group or in log output.
    bool validateName(const QString &name);

    /// Applies the network-level port forwards ("proto/host:guestIp:port")
    /// to the NAT uplink.
    void applyPortForwards(const QString &name, const VirtualNetwork &net);

    static QString configGroupName(const QString &networkName);

    QString m_lastError;
    void setError(const QString &networkName, const QString &message);
    void resetError();
};
