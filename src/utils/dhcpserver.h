/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QSet>

#include "utils/virtualswitch.h"

/**
 * @brief Minimal in-memory DHCPv4 server attached to a VirtualSwitch as an
 * internal port.
 *
 * Implements the RFC 2131 core exchange (DISCOVER/OFFER, REQUEST/ACK/NAK,
 * RELEASE, DECLINE) for a single IPv4 subnet. Declined addresses are held
 * back for kDeclineHoldSeconds. Leases are kept in memory and
 * expire after kLeaseTimeSeconds; the pool is derived from the configured
 * address range. The gateway address and, optionally, a DNS server address
 * are advertised.
 *
 * Not a general-purpose DHCP server: no lease persistence, no option sets
 * beyond what desktop guests need, no relay support.
 */
class DhcpServer : public VirtualSwitchPort
{
    Q_OBJECT

public:
    explicit DhcpServer(const QByteArray &serverMac, QObject *parent = nullptr);

    /// All addresses in host byte order. dnsServer == 0 omits the DNS option.
    /// serverIp is this server's own address (advertised as DHCP server id);
    /// the server answers ARP requests for it so unicast lease renewals work.
    /// serverIp == 0 falls back to the gateway address without ARP answering.
    /// reserved lists further addresses owned by network services; they are
    /// never leased, like the gateway, serverIp and dnsServer.
    void configure(quint32 gateway,
                   quint32 serverIp,
                   int prefixLength,
                   quint32 rangeStart,
                   quint32 rangeEnd,
                   quint32 dnsServer,
                   const QList<quint32> &reserved = {});

    void receiveFrame(const QByteArray &frame) override;
    [[nodiscard]] bool ownsMac(const QByteArray &mac) const override
    {
        return mac == m_serverMac;
    }

    /// Current lease address for a client MAC, 0 if none or expired.
    /// Used by the DNS server to resolve VM hostnames.
    [[nodiscard]] quint32 leaseIpForMac(const QByteArray &clientMac) const;

private:
    struct Lease {
        quint32 ip = 0;
        qint64 expiresAtMs = 0;
    };

    void answerArp(const QByteArray &frame);

    quint32 allocateLease(const QByteArray &clientMac, quint32 requestedIp);
    void releaseLease(const QByteArray &clientMac, quint32 ip);
    void sendReply(quint8 msgType, const QByteArray &clientMac, quint32 xid, quint32 yiaddr, quint16 flags);
    bool isInPool(quint32 ip) const;
    bool isLeased(quint32 ip) const;
    bool isLeasable(quint32 ip) const;
    void expireLeases();

    QByteArray m_serverMac;

    quint32 m_gateway = 0;
    quint32 m_serverIp = 0;
    quint32 m_netmask = 0;
    quint32 m_rangeStart = 0;
    quint32 m_rangeEnd = 0;
    quint32 m_dnsServer = 0;
    bool m_ownAddress = false;
    QSet<quint32> m_reserved;

    QHash<QByteArray, Lease> m_leases; // key: client MAC (6 bytes)
    QHash<quint32, qint64> m_declined; // address -> held back until (ms)
    QElapsedTimer m_clock;

    static constexpr qint64 kLeaseTimeSeconds = 12 * 60 * 60;
    static constexpr qint64 kDeclineHoldSeconds = 10 * 60;
};
