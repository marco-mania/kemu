/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDir>
#include <QFile>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QString>
#include <QThread>

#include <KConfig>
#include <KConfigGroup>
#include <KLocalizedString>
#include <KProcess>

#include "utils/virtualnetwork.h"

using namespace Qt::StringLiterals;

/**
 * @brief Manages rootless virtual networks based on Linux user-namespaces.
 *
 * Architecture overview (per network):
 *
 *   ┌─── User Namespace (unshare --user --net) ────────────────────┐
 *   │                                                              │
 *   │   br0  (bridge, gateway IP)                                  │
 *   │    ├── tap-<vm1>  ←──── QEMU VM 1 (outside, holds tap fd)    │
 *   │    ├── tap-<vm2>  ←──── QEMU VM 2                            │
 *   │    └── ...                                                   │
 *   │                                                              │
 *   │   [dnsmasq]  (optional, DHCP on br0)                         │
 *   │                                                              │
 *   └──────────────────────────────────────────────────────────────┘
 *          │
 *       [pasta]  (optional, NAT to host network)
 *
 * Lifecycle:
 *   1. startNetwork()  → creates namespace, bridge, optional dnsmasq + pasta
 *   2. attachVm()      → creates tap device in namespace, attaches to bridge,
 *                         returns tap name for QEMU -netdev
 *   3. detachVm()      → removes tap device, decrements ref count
 *   4. stopNetwork()   → kills all namespace processes; kernel cleans up
 *                         all interfaces automatically
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
     * Start the virtual network:
     *   1. Create a user + network namespace via `unshare`
     *   2. Configure the bridge (br0) with the gateway IP
     *   3. Optionally start dnsmasq for DHCP
     *   4. Optionally start pasta for NAT
     *
     * @return true if the network is running after this call.
     */
    bool startNetwork(const QString &name);

    /**
     * Stop the virtual network.
     * Terminates the namespace anchor process; the kernel destroys
     * all interfaces, tap devices and routes automatically.
     */
    void stopNetwork(const QString &name);

    [[nodiscard]] bool isRunning(const QString &name) const;

    // --- Per-VM Integration ---

    /**
     * Create a tap device inside the namespace and attach it to the bridge.
     * Call this before launching QEMU for a VM that uses this network.
     *
     * @param networkName  The virtual network to attach to.
     * @param vmName       Used to derive a unique tap device name (tap-<vmName>).
     * @return The tap interface name (e.g. "tap-myvm"), or empty on failure.
     */
    QString attachVm(const QString &networkName, const QString &vmName);

    /**
     * Open the VM's tap device inside the namespace and return a descriptor.
     * The caller owns the fd and hands it to QEMU, which therefore stays
     * outside the namespace. Returns -1 on failure.
     */
    [[nodiscard]] int openTapFd(const QString &networkName, const QString &vmName);

    /**
     * Remove the tap device for the given VM and decrement the ref count.
     * If the ref count reaches zero, the network is stopped automatically.
     */
    void detachVm(const QString &networkName, const QString &vmName);

    [[nodiscard]] int connectedVmCount(const QString &name) const;

    /**
     * Returns the PID of the namespace anchor process.
     * Needed to launch QEMU inside the namespace via nsenter.
     * Returns 0 if the network is not running.
     */
    [[nodiscard]] qint64 namespacePid(const QString &name) const;

    /**
     * Returns the tap device name assigned to a specific VM.
     * Used by QemuCommandBuilder to construct the -netdev argument.
     */
    [[nodiscard]] QString tapDeviceName(const QString &networkName, const QString &vmName) const;

    [[nodiscard]] QString lastError() const;

Q_SIGNALS:
    void networkStarted(const QString &name);
    void networkStopped(const QString &name);
    void networksChanged();

    /// Emitted when a VM's tap device has been created and is ready.
    void vmAttached(const QString &networkName, const QString &vmName, const QString &tapDevice);

    /// Emitted when a VM's tap device has been removed.
    void vmDetached(const QString &networkName, const QString &vmName);

    /// Emitted when a runtime error occurs (e.g. namespace creation fails).
    void errorOccurred(const QString &networkName, const QString &message);

private:
    struct RuntimeState {
        /// PID of the fork() anchor child holding the namespace.
        qint64 nsPid = 0;
        /// Optional dnsmasq process running inside the namespace.
        QPointer<KProcess> dnsmasqProcess;
        /// Optional pasta process providing NAT connectivity.
        QPointer<KProcess> pastaProcess;
        /// Currently attached VMs: vmName → tap device name.
        QHash<QString, QString> attachedVms;
        int vmRefCount = 0;
    };

    QList<VirtualNetwork> m_networks;
    QHash<QString, RuntimeState> m_runtime;

    // --- Internal Helpers ---

    /// XDG_RUNTIME_DIR/kemu/net-<name>
    [[nodiscard]] QString runtimeDir(const QString &name) const;

    void cleanupRuntimeDir(const QString &name);

    /**
     * Step 1: Create the user + network namespace.
     * Launches `unshare --user --net --map-root-user -- sleep infinity`.
     */
    bool createNamespace(const QString &name);

    bool setupNat(const QString &name);

    /**
     * Step 2: Configure the bridge inside the namespace.
     * Runs `ip link add br0 type bridge`, assigns the gateway IP, brings it up.
     */
    bool configureBridge(const QString &name, const VirtualNetwork &net);

    /**
     * Step 3 (optional): Start dnsmasq inside the namespace.
     * Binds to br0, serves DHCP in the configured range.
     */
    bool startDnsmasq(const QString &name, const VirtualNetwork &net);

    /**
     * Step 4 (optional): Start pasta to connect the namespace to the host
     * network. Provides NAT for outbound traffic and optional port forwards.
     */
    bool startPasta(const QString &name, const VirtualNetwork &net);

    /**
     * Create a tap device inside the namespace, set it up, and attach it to br0.
     * @return The tap device name, or empty on failure.
     */
    QString createTapDevice(const QString &networkName, const QString &vmName);

    /**
     * Remove a tap device from the namespace.
     */
    void destroyTapDevice(const QString &networkName, const QString &tapName);

    /**
     * Terminate all processes associated with a network.
     * Killing the namespace anchor process causes the kernel to
     * clean up all interfaces, bridges and tap devices automatically.
     */
    void terminateAll(RuntimeState &state);

    /// Derive a tap device name from the VM name (max 15 chars for IFNAMSIZ).
    [[nodiscard]] static QString makeTapName(const QString &vmName);

    static QString configGroupName(const QString &networkName);

    QString m_lastError;
    void setError(const QString &networkName, const QString &message);
    void resetError();
};
