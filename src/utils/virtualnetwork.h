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
 * Each virtual network maps to one Linux user-namespace containing:
 *   - a bridge interface (br0)
 *   - optionally dnsmasq for DHCP
 *   - optionally pasta for NAT / internet access
 *
 * Per-VM tap devices are attached to the bridge at VM start time.
 */
struct VirtualNetwork {
    /// Unique, user-visible name (also used as KConfig group key and runtime-dir
    /// suffix).
    QString name;

    // --- Subnet ---

    /// Bridge / gateway address inside the namespace, e.g. "192.168.100.1"
    QString gateway;

    /// CIDR prefix length for the subnet, e.g. 24 → 192.168.100.0/24
    int prefixLength = 24;

    // --- DHCP (dnsmasq) ---

    /// When true, dnsmasq is started inside the namespace to hand out leases.
    bool enableDhcp = true;

    /// First address of the DHCP range, e.g. "192.168.100.100".
    /// If empty and enableDhcp is true, a sensible default is derived from the
    /// gateway.
    QString dhcpRangeStart;

    /// Last address of the DHCP range, e.g. "192.168.100.200".
    QString dhcpRangeEnd;

    // --- NAT / Internet (pasta) ---

    /// When true, pasta is started to provide outbound connectivity (NAT).
    /// When false the network is fully isolated (air-gapped).
    bool enableNat = true;

    /// Optional: network-level port forwards handled by pasta (host → namespace).
    /// Individual per-VM forwards are still stored in VMConfig::portForwards().
    /// Format per entry: "tcp/8080:80" or "udp/5353:53"
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
