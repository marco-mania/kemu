/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "virtualnetworkmanager.h"

#include <QCryptographicHash>
#include <QHostAddress>

#include "utils/dhcpserver.h"
#include "utils/dnsserver.h"
#include "utils/gatewayresponder.h"
#include "utils/networkthread.h"
#include "utils/slirpuplink.h"
#include "utils/utils.h"
#include "utils/virtualswitch.h"

#include "kemu_debug.h"

using namespace Qt::StringLiterals;

namespace
{

/// Deterministic locally-administered MAC for the in-process service ports.
QByteArray serviceMac(const QString &seed)
{
    const QByteArray hash = QCryptographicHash::hash(seed.toUtf8(), QCryptographicHash::Md5);
    QByteArray mac;
    mac.append(char(0x02)).append('K').append('E').append('M').append(hash.at(0)).append(hash.at(1));
    return mac;
}

quint32 prefixToMask(int prefixLength)
{
    if (prefixLength <= 0)
        return 0;
    if (prefixLength >= 32)
        return 0xffffffffu;
    return 0xffffffffu << (32 - prefixLength);
}

/// Address for an in-process network service: next to the gateway, inside the
/// subnet, distinct from gateway, 'avoid' and 'avoid2'. Returns 0 if none fits.
quint32 deriveServiceAddress(quint32 gateway, quint32 network, quint32 broadcast, quint32 offset, quint32 avoid, quint32 avoid2 = 0)
{
    const quint32 up = gateway + offset;
    if (up > network && up < broadcast && up != gateway && up != avoid && up != avoid2)
        return up;
    if (offset < gateway) {
        const quint32 down = gateway - offset;
        if (down > network && down < broadcast && down != gateway && down != avoid && down != avoid2)
            return down;
    }
    return 0;
}

} // namespace

// ============================================================================
// Construction / Destruction
// ============================================================================

VirtualNetworkManager::VirtualNetworkManager(QObject *parent)
    : QObject(parent)
{
}

VirtualNetworkManager::~VirtualNetworkManager()
{
    const QStringList names = m_runtime.keys();
    for (const QString &name : names)
        stopNetwork(name);
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
    const KSharedConfig::Ptr config = KSharedConfig::openConfig(u"kemurc"_s);
    const KConfigGroup root = config->group(u"VirtualNetworks"_s);

    const QStringList names = root.readEntry(u"Names"_s, QStringList());

    for (const QString &name : names) {
        const KConfigGroup grp = config->group(configGroupName(name));
        VirtualNetwork net;
        net.name = name;
        net.gateway = grp.readEntry(u"Gateway"_s, {});
        net.prefixLength = grp.readEntry(u"PrefixLength"_s, 24);
        net.enableDhcp = grp.readEntry(u"EnableDhcp"_s, true);
        net.enableDns = grp.readEntry(u"EnableDns"_s, true);
        // Networks saved before the domain became configurable used ".lan".
        net.dnsDomain = grp.hasKey(u"DnsDomain"_s) ? grp.readEntry(u"DnsDomain"_s, QString()) : Utils::vmNameToHostname(name) + u".lan"_s;
        net.dhcpRangeStart = grp.readEntry(u"DhcpRangeStart"_s, {});
        net.dhcpRangeEnd = grp.readEntry(u"DhcpRangeEnd"_s, {});
        net.enableNat = grp.readEntry(u"EnableNat"_s, true);
        net.allowHostAccess = grp.readEntry(u"AllowHostAccess"_s, false);
        net.portForwards = grp.readEntry(u"PortForwards"_s, QStringList());
        if (net.isValid())
            m_networks.append(net);
    }
}

void VirtualNetworkManager::save() const
{
    KSharedConfig::Ptr config = KSharedConfig::openConfig(u"kemurc"_s);

    QStringList names;
    for (const auto &net : m_networks)
        names << net.name;

    // Remove config groups for networks that no longer exist.
    KConfigGroup root = config->group(u"VirtualNetworks"_s);
    const QStringList oldNames = root.readEntry(u"Names"_s, QStringList());
    for (const QString &old : oldNames) {
        if (!names.contains(old))
            config->deleteGroup(configGroupName(old));
    }

    root.writeEntry(u"Names"_s, names);

    for (const auto &net : m_networks) {
        KConfigGroup grp = config->group(configGroupName(net.name));
        grp.writeEntry(u"Gateway"_s, net.gateway);
        grp.writeEntry(u"PrefixLength"_s, net.prefixLength);
        grp.writeEntry(u"EnableDhcp"_s, net.enableDhcp);
        grp.writeEntry(u"EnableDns"_s, net.enableDns);
        grp.writeEntry(u"DnsDomain"_s, net.dnsDomain);
        grp.writeEntry(u"DhcpRangeStart"_s, net.dhcpRangeStart);
        grp.writeEntry(u"DhcpRangeEnd"_s, net.dhcpRangeEnd);
        grp.writeEntry(u"EnableNat"_s, net.enableNat);
        grp.writeEntry(u"AllowHostAccess"_s, net.allowHostAccess);
        grp.writeEntry(u"PortForwards"_s, net.portForwards);
    }

    config->sync();
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
    if (!validateName(net.name))
        return false;

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
    if (!validateName(net.name))
        return false;

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
    resetError();

    if (isRunning(name))
        return true;

    if (!validateName(name))
        return false;

    const VirtualNetwork net = network(name);
    if (!net.isValid()) {
        setError(name, i18n("Invalid network configuration."));
        return false;
    }

    const QHostAddress gatewayAddress(net.gateway);
    if (gatewayAddress.isNull() || gatewayAddress.protocol() != QAbstractSocket::IPv4Protocol) {
        setError(name, i18n("Invalid gateway address '%1'.", net.gateway));
        return false;
    }

    const quint32 gatewayIp = gatewayAddress.toIPv4Address();
    const quint32 mask = prefixToMask(net.prefixLength);
    const quint32 networkAddr = gatewayIp & mask;
    const quint32 broadcastAddr = networkAddr | ~mask;

    const QString dnsDomain = net.effectiveDnsDomain().toLower();
    if (net.enableDhcp && net.enableDns && !Utils::isValidDnsDomain(dnsDomain)) {
        setError(name, i18n("The DNS domain '%1' is not valid.", dnsDomain));
        return false;
    }

    // DHCP pool, validated before anything is created.
    quint32 rangeStart = 0;
    quint32 rangeEnd = 0;
    if (net.enableDhcp) {
        rangeStart = QHostAddress(net.dhcpRangeStart).toIPv4Address();
        rangeEnd = QHostAddress(net.dhcpRangeEnd).toIPv4Address();
        if (rangeStart == 0 || rangeEnd == 0 || rangeStart > rangeEnd) {
            // Default pool: network + 100 .. network + 200, clamped to the subnet.
            rangeStart = networkAddr + 100;
            rangeEnd = networkAddr + 200;
            if (rangeEnd >= broadcastAddr)
                rangeEnd = broadcastAddr - 1;
            if (rangeStart > rangeEnd || rangeStart <= networkAddr) {
                setError(name, i18n("Cannot derive a DHCP range from gateway '%1'.", net.gateway));
                return false;
            }
        }
        if (rangeStart <= networkAddr || rangeEnd >= broadcastAddr) {
            setError(name,
                     i18n("The DHCP range %1 - %2 lies outside the subnet %3/%4.",
                          QHostAddress(rangeStart).toString(),
                          QHostAddress(rangeEnd).toString(),
                          QHostAddress(networkAddr).toString(),
                          net.prefixLength));
            return false;
        }
    }

    // Service addresses: the DHCP server advertises the NAT DNS proxy.
    const quint32 dnsIp = net.enableNat ? deriveServiceAddress(gatewayIp, networkAddr, broadcastAddr, 1, 0) : 0;
    const quint32 dhcpIp = deriveServiceAddress(gatewayIp, networkAddr, broadcastAddr, 2, dnsIp);
    // DNS requires DHCP: the lease table is its only name->address source.
    const bool runDns = net.enableDhcp && net.enableDns;
    const quint32 nameDnsIp = runDns ? deriveServiceAddress(gatewayIp, networkAddr, broadcastAddr, 3, dnsIp, dhcpIp) : 0;

    const QString dirPath = runtimeDir(name);
    if (!QDir().mkpath(dirPath)) {
        setError(name, i18n("Failed to create the runtime directory '%1'.", dirPath));
        return false;
    }

    RuntimeState &state = m_runtime[name];
    state.thread = std::make_shared<NetworkThread>(u"kemu-net"_s);

    qCDebug(KEMU) << "VirtualNetworkManager: network" << name << "started in" << dirPath << "- gateway" << gatewayAddress.toString() << "prefix"
                  << net.prefixLength << "dhcp" << net.enableDhcp << "nat" << net.enableNat;

    // The fabric lives on its own thread: guest frames are switched there,
    // independently of whatever blocks the GUI thread.
    QString uplinkError;
    state.thread->run([&] {
        auto *vswitch = new VirtualSwitch;
        state.vswitch = vswitch;

        if (net.enableNat) {
            auto *uplink = new SlirpUplink;
            SlirpUplink::Config cfg;
            cfg.network = networkAddr;
            cfg.netmask = mask;
            cfg.gateway = gatewayIp;
            cfg.nameserver = dnsIp != 0 ? dnsIp : networkAddr + 1;
            cfg.allowHostLoopback = net.allowHostAccess;

            if (!uplink->start(cfg)) {
                uplinkError = uplink->lastError();
                delete uplink;
                return;
            }
            // Context is the manager, so the signal is re-emitted on its thread.
            connect(uplink, &SlirpUplink::errorOccurred, this, [this, name](const QString &message) {
                Q_EMIT errorOccurred(name, message);
            });

            vswitch->addInternalPort(uplink);
            state.uplink = uplink;
            qCDebug(KEMU) << "VirtualNetworkManager: NAT uplink attached, DNS proxy at"
                          << (dnsIp != 0 ? QHostAddress(dnsIp).toString() : u"none"_s);
        } else {
            auto *responder = new GatewayResponder(serviceMac(name + u"-gw"_s), gatewayIp);
            vswitch->addInternalPort(responder);
            state.gatewayResponder = responder;
            qCDebug(KEMU) << "VirtualNetworkManager: gateway responder attached (no NAT)";
        }

        if (net.enableDhcp) {
            auto *dhcp = new DhcpServer(serviceMac(name + u"-dhcp"_s));
            dhcp->configure(gatewayIp,
                            dhcpIp,
                            net.prefixLength,
                            rangeStart,
                            rangeEnd,
                            nameDnsIp != 0 ? nameDnsIp : (net.enableNat ? dnsIp : 0),
                            {dnsIp, nameDnsIp});
            if (nameDnsIp != 0)
                dhcp->setSearchDomain(dnsDomain);
            vswitch->addInternalPort(dhcp);
            state.dhcp = dhcp;
            qCDebug(KEMU) << "VirtualNetworkManager: DHCP server attached, pool" << QHostAddress(rangeStart).toString() << "-"
                          << QHostAddress(rangeEnd).toString();
        }

        // In-process DNS server: resolves VM hostnames in the network's DNS domain from
        // the DHCP lease table. Other names are forwarded only with NAT.
        if (nameDnsIp != 0) {
            auto *dns = new DnsServer(serviceMac(name + u"-dns"_s));
            dns->configure(nameDnsIp, dnsDomain, net.enableNat);
            if (state.dhcp)
                dns->setDhcpServer(state.dhcp);
            vswitch->addInternalPort(dns);
            state.dns = dns;
            qCDebug(KEMU) << "VirtualNetworkManager: DNS server attached at" << QHostAddress(nameDnsIp).toString() << "zone" << dnsDomain;
        } else if (runDns) {
            qCDebug(KEMU) << "VirtualNetworkManager: no address left for the DNS server (subnet too small)";
        } else {
            qCDebug(KEMU) << "VirtualNetworkManager: DNS server disabled for this network";
        }
    });

    if (!uplinkError.isEmpty()) {
        stopNetwork(name);
        setError(name, i18n("Failed to start NAT uplink: %1", uplinkError));
        return false;
    }

    if (state.uplink)
        applyPortForwards(name, net);

    Q_EMIT networkStarted(name);
    return true;
}

void VirtualNetworkManager::stopNetwork(const QString &name)
{
    auto it = m_runtime.find(name);
    if (it == m_runtime.end())
        return;

    qCDebug(KEMU) << "VirtualNetworkManager: stopping network" << name;

    // Notify all attached VMs that they are being detached.
    const QList<QString> vmNames = it->attachedVms.keys();
    for (const QString &vm : vmNames)
        Q_EMIT vmDetached(name, vm);

    RuntimeState state = it.value();
    m_runtime.erase(it);

    // Ports first (their destruction detaches them), then the switch. All of
    // them belong to the network thread, so they are destroyed there.
    if (state.thread) {
        state.thread->run([&state] {
            delete state.dhcp;
            delete state.dns;
            delete state.gatewayResponder;
            delete state.uplink;
            delete state.vswitch;
        });
        state.thread.reset(); // joins the thread
    }

    cleanupRuntimeDir(name);

    Q_EMIT networkStopped(name);
}

bool VirtualNetworkManager::isRunning(const QString &name) const
{
    auto it = m_runtime.find(name);
    return it != m_runtime.end() && it->vswitch;
}

// ============================================================================
// Per-VM Integration
// ============================================================================

QString VirtualNetworkManager::attachVm(const QString &networkName,
                                        const QString &vmName,
                                        const QString &dnsHostname,
                                        const QByteArray &macAddress)
{
    resetError();

    auto it = m_runtime.find(networkName);
    if (it == m_runtime.end() || !it->vswitch) {
        setError(networkName, i18n("Cannot attach VM '%1': network is not running.", vmName));
        return {};
    }

    const QString path = portSocketPath(networkName, vmName);
    if (it->attachedVms.contains(vmName))
        return path;

    if (!macAddress.isEmpty()) {
        for (auto vm = it->attachedVms.cbegin(); vm != it->attachedVms.cend(); ++vm) {
            if (vm.value() == macAddress) {
                setError(networkName,
                         i18n("Cannot attach VM '%1': its MAC address is already used by VM '%2' on this network.", vmName, vm.key()));
                return {};
            }
        }
    }

    const bool registerHost = it->dns && !dnsHostname.isEmpty() && !macAddress.isEmpty();
    bool added = false;
    QString switchError;
    it->thread->run([&] {
        added = it->vswitch->addPort(path, macAddress);
        if (!added)
            switchError = it->vswitch->lastError();
        else if (registerHost)
            it->dns->addHost(dnsHostname, macAddress);
    });

    if (!added) {
        setError(networkName, i18n("Cannot attach VM '%1': %2", vmName, switchError));
        if (it->attachedVms.isEmpty())
            stopNetwork(networkName);
        return {};
    }

    it->attachedVms.insert(vmName, macAddress);

    if (registerHost)
        qCDebug(KEMU) << "VirtualNetworkManager: DNS record" << dnsHostname << "registered for" << vmName;

    qCDebug(KEMU) << "VirtualNetworkManager: VM" << vmName << "attached to" << networkName << "-" << path;
    Q_EMIT vmAttached(networkName, vmName, path);
    return path;
}

void VirtualNetworkManager::detachVm(const QString &networkName, const QString &vmName)
{
    auto it = m_runtime.find(networkName);
    if (it == m_runtime.end())
        return;

    const QByteArray mac = it->attachedVms.value(vmName);
    if (!it->attachedVms.remove(vmName))
        return;

    const QString path = portSocketPath(networkName, vmName);
    if (it->thread) {
        it->thread->run([&] {
            if (it->vswitch)
                it->vswitch->removePort(path);
            if (it->dns && !mac.isEmpty())
                it->dns->removeHost(mac);
        });
    }

    qCDebug(KEMU) << "VirtualNetworkManager: VM" << vmName << "detached from" << networkName;
    Q_EMIT vmDetached(networkName, vmName);

    // Auto-stop the network when no VMs remain.
    if (it->attachedVms.isEmpty())
        stopNetwork(networkName);
}

int VirtualNetworkManager::connectedVmCount(const QString &name) const
{
    auto it = m_runtime.find(name);
    return (it != m_runtime.end()) ? static_cast<int>(it->attachedVms.size()) : 0;
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

QString VirtualNetworkManager::portSocketPath(const QString &networkName, const QString &vmName) const
{
    // Short, filesystem-safe name: unix socket paths are limited to 108 bytes.
    const QByteArray id = QCryptographicHash::hash(vmName.toUtf8(), QCryptographicHash::Sha256).toHex().left(12);
    return runtimeDir(networkName) + u"/vm-"_s + QString::fromLatin1(id) + u".sock"_s;
}

void VirtualNetworkManager::cleanupRuntimeDir(const QString &name)
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + u"/kemu/net-"_s;
    const QString path = runtimeDir(name);

    // removeRecursively() on a path built from configuration data, so make
    // sure the name cannot walk out of the runtime directory.
    if (!path.startsWith(base) || path.contains(u"/.."_s) || path.endsWith(u"/.."_s)) {
        qCWarning(KEMU) << "VirtualNetworkManager: refusing to remove" << path;
        return;
    }

    QDir dir(path);
    if (dir.exists())
        dir.removeRecursively();
}

bool VirtualNetworkManager::validateName(const QString &name)
{
    QString reason;
    if (Utils::isValidNetworkName(name, &reason))
        return true;
    setError(name, i18n("Invalid network name '%1': %2", name, reason));
    return false;
}

// ============================================================================
// Internal: Port Forwards
// ============================================================================

void VirtualNetworkManager::applyPortForwards(const QString &name, const VirtualNetwork &net)
{
    auto it = m_runtime.find(name);
    if (it == m_runtime.end() || !it->uplink)
        return;

    for (const QString &fwd : net.portForwards) {
        // Format: "proto/hostPort:guestIp:guestPort", e.g. "tcp/8080:192.168.100.50:80"
        const int slash = fwd.indexOf(u'/');
        const QString proto = fwd.left(slash).toLower();
        const bool udp = proto == u"udp"_s;
        const QStringList parts = fwd.mid(slash + 1).split(u':');

        bool okHost = false, okGuest = false;
        const int hostPort = parts.value(0).toInt(&okHost);
        const QHostAddress guestAddress(parts.value(1));
        const int guestPort = parts.value(2).toInt(&okGuest);

        if (slash <= 0 || (!udp && proto != u"tcp"_s) || parts.size() != 3 || !okHost || !okGuest || guestAddress.isNull()
            || guestAddress.protocol() != QAbstractSocket::IPv4Protocol || hostPort <= 0 || hostPort > 65535 || guestPort <= 0 || guestPort > 65535) {
            qCWarning(KEMU) << "Ignoring port forward" << fwd << "for network" << name
                            << "- expected format proto/hostPort:guestIp:guestPort (e.g. tcp/8080:192.168.100.50:80)";
            continue;
        }

        qCDebug(KEMU) << "VirtualNetworkManager: applying port forward" << fwd << "for network" << name;
        it->uplink->addPortForward(udp, static_cast<quint16>(hostPort), guestAddress.toIPv4Address(), static_cast<quint16>(guestPort));
    }
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
