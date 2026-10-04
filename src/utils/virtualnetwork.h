/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QString>
#include <QStringList>

using namespace Qt::StringLiterals;

/**
 * @brief Configuration of a single virtual network.
 *
 * Each virtual network is realized entirely inside the KEMU process:
 *   - an in-memory L2 switch (VirtualSwitch) that QEMU instances connect to
 *     via "-netdev stream" over a unix socket
 *   - optionally a built-in DHCP server (DhcpServer)
 *   - optionally a built-in DNS server (DnsServer) resolving the VMs'
 *     hostnames in "<network>.lan" (requires the DHCP server: its lease
 *     table is the only name -> address source)
 *   - optionally an embedded libslirp instance (SlirpUplink) for
 *     NAT / internet access
 *
 * No root rights, kernel interfaces or namespaces are involved, so virtual
 * networks work unchanged inside sandboxes (Flatpak) and on immutable
 * systems.
 */
struct VirtualNetwork {
    /// Unique, user-visible name (also used as KConfig group key and runtime-dir
    /// suffix).
    QString name;

    // --- Subnet ---

    /// Gateway address of the network, e.g. "192.168.100.1"
    QString gateway;

    /// CIDR prefix length for the subnet, e.g. 24 → 192.168.100.0/24
    int prefixLength = 24;

    // --- DHCP ---

    /// When true, the built-in DHCP server hands out leases on the network.
    bool enableDhcp = true;

    /// First address of the DHCP range, e.g. "192.168.100.100".
    /// If empty and enableDhcp is true, a sensible default is derived from the
    /// gateway.
    QString dhcpRangeStart;

    /// Last address of the DHCP range, e.g. "192.168.100.200".
    QString dhcpRangeEnd;

    /// When true (and enableDhcp is true), a built-in DNS server resolves
    /// the VMs' hostnames in "<network>.lan" from the DHCP lease table.
    /// Other queries go to the host resolver only if enableNat is set.
    bool enableDns = true;

    // --- NAT / Internet ---

    /// When true, an embedded libslirp instance provides outbound
    /// connectivity (NAT). When false the network is fully isolated
    /// (air-gapped).
    bool enableNat = true;

    /// Optional: network-level port forwards handled by the NAT uplink
    /// (host 127.0.0.1 → guest address).
    /// Format per entry: "tcp/8080:192.168.100.50:80" or "udp/5353:192.168.100.50:53"
    QStringList portForwards;

    // --- Validation ---

    [[nodiscard]] bool isValid() const
    {
        return !name.isEmpty() && !gateway.isEmpty();
    }

    bool operator==(const VirtualNetwork &other) const
    {
        return name == other.name;
    }
};
