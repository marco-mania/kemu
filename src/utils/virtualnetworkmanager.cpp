/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "virtualnetworkmanager.h"

extern "C" {
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
}

#include "utils/externaltools.h"
#include "utils/namespacehelper.h"

using namespace Qt::StringLiterals;

// ============================================================================
// Construction / Destruction
// ============================================================================

VirtualNetworkManager::VirtualNetworkManager(QObject *parent)
    : QObject(parent)
{
}

VirtualNetworkManager::~VirtualNetworkManager()
{
    // Shut down every running network. Killing the namespace anchor process
    // causes the kernel to tear down all interfaces automatically, but we
    // terminate dnsmasq and pasta first for a clean shutdown.
    for (auto it = m_runtime.begin(); it != m_runtime.end(); ++it)
        terminateAll(it.value());
    m_runtime.clear();
}

// ============================================================================
// KConfig Persistence
// ============================================================================

QString VirtualNetworkManager::configGroupName(const QString &networkName)
{
    return u"VirtualNetwork-%1"_s.arg(networkName);
}

void VirtualNetworkManager::load()
{
    m_networks.clear();
    KConfig config(u"kemurc"_s);
    const KConfigGroup root = config.group(u"VirtualNetworks"_s);
    const QStringList names = root.readEntry(u"Names"_s, QStringList());

    for (const QString &name : names) {
        const KConfigGroup grp = config.group(configGroupName(name));
        VirtualNetwork net;
        net.name = name;
        net.gateway = grp.readEntry(u"Gateway"_s, {});
        net.prefixLength = grp.readEntry(u"PrefixLength"_s, 24);
        net.enableDhcp = grp.readEntry(u"EnableDhcp"_s, true);
        net.dhcpRangeStart = grp.readEntry(u"DhcpRangeStart"_s, {});
        net.dhcpRangeEnd = grp.readEntry(u"DhcpRangeEnd"_s, {});
        net.enableNat = grp.readEntry(u"EnableNat"_s, true);
        net.portForwards = grp.readEntry(u"PortForwards"_s, QStringList());
        if (net.isValid())
            m_networks.append(net);
    }
}

void VirtualNetworkManager::save() const
{
    KConfig config(u"kemurc"_s);

    QStringList names;
    for (const auto &net : m_networks)
        names << net.name;

    // Remove config groups for networks that no longer exist.
    KConfigGroup root = config.group(u"VirtualNetworks"_s);
    const QStringList oldNames = root.readEntry(u"Names"_s, QStringList());
    for (const QString &old : oldNames) {
        if (!names.contains(old))
            config.deleteGroup(configGroupName(old));
    }

    root.writeEntry(u"Names"_s, names);

    for (const auto &net : m_networks) {
        KConfigGroup grp = config.group(configGroupName(net.name));
        grp.writeEntry(u"Gateway"_s, net.gateway);
        grp.writeEntry(u"PrefixLength"_s, net.prefixLength);
        grp.writeEntry(u"EnableDhcp"_s, net.enableDhcp);
        grp.writeEntry(u"DhcpRangeStart"_s, net.dhcpRangeStart);
        grp.writeEntry(u"DhcpRangeEnd"_s, net.dhcpRangeEnd);
        grp.writeEntry(u"EnableNat"_s, net.enableNat);
        grp.writeEntry(u"PortForwards"_s, net.portForwards);
    }

    config.sync();
}

// ============================================================================
// Queries
// ============================================================================

QList<VirtualNetwork> VirtualNetworkManager::networks() const
{
    return m_networks;
}

VirtualNetwork VirtualNetworkManager::network(const QString &name) const
{
    for (const auto &n : m_networks)
        if (n.name == name)
            return n;
    return {};
}

QStringList VirtualNetworkManager::networkNames() const
{
    QStringList names;
    names.reserve(m_networks.size());
    for (const auto &n : m_networks)
        names << n.name;
    return names;
}

// ============================================================================
// CRUD
// ============================================================================

bool VirtualNetworkManager::addNetwork(const VirtualNetwork &net)
{
    for (const auto &n : m_networks)
        if (n.name == net.name)
            return false;
    m_networks.append(net);
    save();
    Q_EMIT networksChanged();
    return true;
}

bool VirtualNetworkManager::removeNetwork(const QString &name)
{
    if (isRunning(name))
        stopNetwork(name);

    auto it = std::find_if(m_networks.begin(), m_networks.end(), [&](const VirtualNetwork &n) {
        return n.name == name;
    });
    if (it == m_networks.end())
        return false;

    m_networks.erase(it);
    save();
    Q_EMIT networksChanged();
    return true;
}

bool VirtualNetworkManager::updateNetwork(const VirtualNetwork &net)
{
    for (auto &n : m_networks) {
        if (n.name == net.name) {
            n = net;
            save();
            Q_EMIT networksChanged();
            return true;
        }
    }
    return false;
}

// ============================================================================
// Runtime Control
// ============================================================================

bool VirtualNetworkManager::startNetwork(const QString &name)
{
    m_lastError.clear();

    if (isRunning(name))
        return true;

    const VirtualNetwork net = network(name);
    if (!net.isValid()) {
        setError(name, i18n("Invalid network configuration."));
        return false;
    }

    const QDir dir(runtimeDir(name));
    dir.mkpath(u"."_s);

    if (!createNamespace(name)) {
        cleanupRuntimeDir(name);
        return false;
    }

    // pasta must be started BEFORE the bridge, while the namespace
    // is still empty. Otherwise pasta detects existing interfaces
    // and skips its own network setup.
    if (net.enableNat) {
        if (!startPasta(name, net)) {
            stopNetwork(name);
            return false;
        }
    }

    if (!configureBridge(name, net)) {
        stopNetwork(name);
        return false;
    }

    // NAT rules
    if (net.enableNat) {
        if (!setupNat(name)) {
            stopNetwork(name);
            return false;
        }
    }

    if (net.enableDhcp) {
        if (!startDnsmasq(name, net)) {
            stopNetwork(name);
            return false;
        }
    }

    Q_EMIT networkStarted(name);
    return true;
}

void VirtualNetworkManager::stopNetwork(const QString &name)
{
    auto it = m_runtime.find(name);
    if (it == m_runtime.end())
        return;

    // Notify all attached VMs that they are being detached.
    const QStringList vmNames = it->attachedVms.keys();
    for (const QString &vm : vmNames)
        Q_EMIT vmDetached(name, vm);

    terminateAll(it.value());
    m_runtime.erase(it);
    cleanupRuntimeDir(name);

    Q_EMIT networkStopped(name);
}

bool VirtualNetworkManager::isRunning(const QString &name) const
{
    auto it = m_runtime.find(name);
    if (it == m_runtime.end() || it->nsPid <= 0)
        return false;
    return kill(static_cast<pid_t>(it->nsPid), 0) == 0;
}

// ============================================================================
// Per-VM Integration
// ============================================================================

QString VirtualNetworkManager::attachVm(const QString &networkName, const QString &vmName)
{
    resetError();

    if (!isRunning(networkName)) {
        setError(networkName, i18n("Cannot attach VM '%1': network is not running.", vmName));
        return {};
    }

    auto it = m_runtime.find(networkName);

    // Already attached? Return existing tap device.
    if (it->attachedVms.contains(vmName))
        return it->attachedVms.value(vmName);

    const QString tapName = createTapDevice(networkName, vmName);
    if (tapName.isEmpty()) {
        setError(networkName, i18n("Failed to create tap device for VM '%1'.", vmName));
        return {};
    }

    it->attachedVms.insert(vmName, tapName);
    it->vmRefCount++;

    Q_EMIT vmAttached(networkName, vmName, tapName);
    return tapName;
}

void VirtualNetworkManager::detachVm(const QString &networkName, const QString &vmName)
{
    auto it = m_runtime.find(networkName);
    if (it == m_runtime.end())
        return;

    const QString tapName = it->attachedVms.value(vmName);
    if (tapName.isEmpty())
        return;

    // Remove the tap device from the namespace.
    destroyTapDevice(networkName, tapName);

    it->attachedVms.remove(vmName);
    it->vmRefCount = qMax(0, it->vmRefCount - 1);

    Q_EMIT vmDetached(networkName, vmName);

    // Auto-stop the network when no VMs remain.
    if (it->vmRefCount <= 0)
        stopNetwork(networkName);
}

int VirtualNetworkManager::connectedVmCount(const QString &name) const
{
    auto it = m_runtime.find(name);
    return (it != m_runtime.end()) ? it->vmRefCount : 0;
}

qint64 VirtualNetworkManager::namespacePid(const QString &name) const
{
    auto it = m_runtime.find(name);
    return (it != m_runtime.end()) ? it->nsPid : 0;
}

QString VirtualNetworkManager::tapDeviceName(const QString &networkName, const QString &vmName) const
{
    auto it = m_runtime.find(networkName);
    if (it == m_runtime.end())
        return {};
    return it->attachedVms.value(vmName);
}

// ============================================================================
// Capability Check
// ============================================================================

bool VirtualNetworkManager::isUserNamespaceSupported()
{
    return NamespaceHelper::isUserNamespaceSupported();
}

QString VirtualNetworkManager::lastError() const
{
    return m_lastError;
}

// ============================================================================
// Internal: Runtime Directory
// ============================================================================

QString VirtualNetworkManager::runtimeDir(const QString &name) const
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    return base + u"/kemu/net-%1"_s.arg(name);
}

void VirtualNetworkManager::cleanupRuntimeDir(const QString &name)
{
    QDir dir(runtimeDir(name));
    if (dir.exists())
        dir.removeRecursively();
}

// ============================================================================
// Internal: Namespace Creation (Step 1)
// ============================================================================

bool VirtualNetworkManager::createNamespace(const QString &name)
{
    QString error;
    const qint64 pid = NamespaceHelper::createNamespace(&error);
    if (pid < 0) {
        setError(name, i18n("Failed to create namespace: %1", error));
        return false;
    }

    RuntimeState &state = m_runtime[name];
    state.nsPid = pid;

    if (!NamespaceHelper::setInterfaceUp(pid, u"lo"_s, &error)) {
        setError(name, i18n("Loopback configuration failed: %1", error));
        kill(static_cast<pid_t>(pid), SIGKILL);
        waitpid(static_cast<pid_t>(pid), nullptr, 0);
        m_runtime.remove(name);
        return false;
    }

    return true;
}

bool VirtualNetworkManager::setupNat(const QString &name)
{
    const qint64 pid = m_runtime.value(name).nsPid;
    if (pid <= 0)
        return false;

    NamespaceHelper::enableIpForward(pid);

    const QString pastaIface = NamespaceHelper::getDefaultRouteDevice(pid);
    if (pastaIface.isEmpty()) {
        setError(name, i18n("Cannot determine pasta network interface."));
        return false;
    }

    QString error;
    if (!NamespaceHelper::setupMasquerade(pid, pastaIface, &error)) {
        setError(name, i18n("Failed to setup NAT masquerade: %1", error));
        return false;
    }

    return true;
}

// ============================================================================
// Internal: Bridge Configuration
// ============================================================================

bool VirtualNetworkManager::configureBridge(const QString &name, const VirtualNetwork &net)
{
    const qint64 pid = m_runtime.value(name).nsPid;
    if (pid <= 0)
        return false;

    QString error;

    if (!NamespaceHelper::createBridge(pid, u"br0"_s, &error)) {
        setError(name, i18n("Failed to create bridge: %1", error));
        return false;
    }

    NamespaceHelper::setBridgeStp(pid, u"br0"_s, false);

    if (!NamespaceHelper::addAddress(pid, u"br0"_s, net.gateway, net.prefixLength, &error)) {
        setError(name, i18n("Failed to assign IP %1 to bridge: %2", net.gateway, error));
        return false;
    }

    if (!NamespaceHelper::setInterfaceUp(pid, u"br0"_s, &error)) {
        setError(name, i18n("Failed to bring bridge up: %1", error));
        return false;
    }

    return true;
}

// ============================================================================
// Internal: dnsmasq for DHCP
// ============================================================================

bool VirtualNetworkManager::startDnsmasq(const QString &name, const VirtualNetwork &net)
{
    const auto &tools = ExternalTools::instance();
    if (!tools.isAvailable(ExternalTools::Dnsmasq)) {
        setError(name, i18n("'dnsmasq' not found. Install dnsmasq to enable DHCP."));
        return false;
    }

    const qint64 pid = m_runtime.value(name).nsPid;
    if (pid <= 0)
        return false;

    // Derive a sensible DHCP range if not explicitly configured.
    // Given gateway "192.168.100.1", we default to .100 – .200.
    QString rangeStart = net.dhcpRangeStart;
    QString rangeEnd = net.dhcpRangeEnd;

    if (rangeStart.isEmpty() || rangeEnd.isEmpty()) {
        // Parse the gateway to derive the range.
        const QStringList octets = net.gateway.split(u'.');
        if (octets.size() == 4) {
            const QString prefix = octets[0] + u'.' + octets[1] + u'.' + octets[2];
            if (rangeStart.isEmpty())
                rangeStart = prefix + u".100"_s;
            if (rangeEnd.isEmpty())
                rangeEnd = prefix + u".200"_s;
        } else {
            setError(name, i18n("Cannot derive DHCP range from gateway '%1'.", net.gateway));
            return false;
        }
    }

    const QString runDir = runtimeDir(name);
    const QString pidFile = runDir + u"/dnsmasq.pid"_s;
    const QString leaseFile = runDir + u"/dnsmasq.leases"_s;

    auto *process = new KProcess(this);
    NamespaceHelper::prepareProcessForNamespace(process, pid);
    process->setProgram(tools.path(ExternalTools::Dnsmasq));
    process->setArguments({u"--interface=br0"_s,
                           u"--bind-interfaces"_s,
                           u"--except-interface=lo"_s,
                           u"--dhcp-range=%1,%2,12h"_s.arg(rangeStart, rangeEnd),
                           u"--no-daemon"_s,
                           u"--pid-file=%1"_s.arg(pidFile),
                           u"--dhcp-leasefile=%1"_s.arg(leaseFile),
                           u"--no-resolv"_s,
                           u"--server=1.1.1.1"_s,
                           u"--server=1.0.0.1"_s,
                           u"--listen-address=%1"_s.arg(net.gateway),
                           u"--log-dhcp"_s});
    process->setOutputChannelMode(KProcess::MergedChannels);
    process->start();

    if (!process->waitForStarted(3000)) {
        setError(name, i18n("dnsmasq failed to start."));
        delete process;
        return false;
    }

    // Give dnsmasq a moment to bind to the interface.
    QThread::msleep(200);

    // Verify it is still running (it may exit immediately on config errors).
    if (process->state() != KProcess::Running) {
        const QString stderr = QString::fromUtf8(process->readAllStandardOutput()).trimmed();
        setError(name, i18n("dnsmasq exited immediately: %1", stderr));
        delete process;
        return false;
    }

    m_runtime[name].dnsmasqProcess = process;
    return true;
}

// ============================================================================
// Internal: pasta for NAT (Step 4)
// ============================================================================

bool VirtualNetworkManager::startPasta(const QString &name, const VirtualNetwork &net)
{
    const auto &tools = ExternalTools::instance();
    if (!tools.isAvailable(ExternalTools::Pasta)) {
        setError(name, i18n("'pasta' not found. Install passt to enable NAT."));
        return false;
    }

    const qint64 pid = m_runtime.value(name).nsPid;
    if (pid <= 0)
        return false;

    QStringList args = {u"--userns"_s, u"/proc/%1/ns/user"_s.arg(pid), u"--netns"_s, u"/proc/%1/ns/net"_s.arg(pid), u"--config-net"_s, u"-f"_s};

    for (const QString &fwd : net.portForwards) {
        if (fwd.startsWith(u"tcp/"_s))
            args << u"-t"_s << fwd.mid(4);
        else if (fwd.startsWith(u"udp/"_s))
            args << u"-u"_s << fwd.mid(4);
    }

    auto *process = new KProcess(this);
    process->setProgram(tools.path(ExternalTools::Pasta));
    process->setArguments(args);
    process->setOutputChannelMode(KProcess::MergedChannels);
    process->start();

    if (!process->waitForStarted(3000)) {
        setError(name, i18n("pasta failed to start."));
        delete process;
        return false;
    }

    m_runtime[name].pastaProcess = process;
    return true;
}

// ============================================================================
// Internal: Tap Device Management
// ============================================================================

QString VirtualNetworkManager::createTapDevice(const QString &networkName, const QString &vmName)
{
    const qint64 pid = m_runtime.value(networkName).nsPid;
    if (pid <= 0)
        return {};

    const QString tapName = makeTapName(vmName);
    QString error;

    if (!NamespaceHelper::createTapDevice(pid, tapName, &error))
        return {};

    if (!NamespaceHelper::addInterfaceToBridge(pid, tapName, u"br0"_s, &error)) {
        NamespaceHelper::removeInterface(pid, tapName);
        return {};
    }

    if (!NamespaceHelper::setInterfaceUp(pid, tapName, &error)) {
        NamespaceHelper::removeInterface(pid, tapName);
        return {};
    }

    return tapName;
}

void VirtualNetworkManager::destroyTapDevice(const QString &networkName, const QString &tapName)
{
    const qint64 pid = m_runtime.value(networkName).nsPid;
    if (pid <= 0)
        return;
    NamespaceHelper::removeInterface(pid, tapName);
}

// ============================================================================
// Internal: Process Termination
// ============================================================================

void VirtualNetworkManager::terminateAll(RuntimeState &state)
{
    auto terminateKProcess = [](QPointer<KProcess> &p) {
        if (!p)
            return;
        p->terminate();
        if (!p->waitForFinished(3000))
            p->kill();
        delete p;
        p = nullptr;
    };

    terminateKProcess(state.dnsmasqProcess);
    terminateKProcess(state.pastaProcess);

    // The namespace anchor is a fork() child, not a KProcess
    if (state.nsPid > 0) {
        kill(static_cast<pid_t>(state.nsPid), SIGKILL);
        waitpid(static_cast<pid_t>(state.nsPid), nullptr, 0);
    }

    state.nsPid = 0;
    state.attachedVms.clear();
    state.vmRefCount = 0;
}

// ============================================================================
// Internal: Tap Device Naming
// ============================================================================

QString VirtualNetworkManager::makeTapName(const QString &vmName)
{
    // Linux IFNAMSIZ is 16 bytes (15 chars + NUL terminator).
    // Prefix "tap-" uses 4 characters, leaving 11 for the VM name.
    // We sanitize to lowercase alphanumerics to avoid any issues
    // with special characters in interface names.
    QString sanitized = vmName.toLower();
    sanitized.remove(QRegularExpression(u"[^a-z0-9]"_s));

    // Ensure we have at least some characters after sanitization.
    if (sanitized.isEmpty())
        sanitized = u"vm"_s;

    return u"tap-%1"_s.arg(sanitized.left(11));
}

void VirtualNetworkManager::setError(const QString &networkName, const QString &message)
{
    Q_EMIT errorOccurred(networkName, message);
    m_lastError = message;
}

void VirtualNetworkManager::resetError()
{
    m_lastError.clear();
}
