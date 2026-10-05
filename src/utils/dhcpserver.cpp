/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dhcpserver.h"

#include <QHostAddress>
#include <QtEndian>

#include <cstring>

#include "kemu_debug.h"

using namespace Qt::StringLiterals;

namespace
{

constexpr quint16 kEtherTypeIpv4 = 0x0800;
constexpr quint16 kEtherTypeArp = 0x0806;
constexpr quint8 kIpProtoUdp = 17;
constexpr quint16 kDhcpServerPort = 67;
constexpr quint16 kDhcpClientPort = 68;
constexpr quint32 kBootpMagicCookie = 0x63825363;

constexpr quint8 kDhcpDiscover = 1;
constexpr quint8 kDhcpOffer = 2;
constexpr quint8 kDhcpRequest = 3;
constexpr quint8 kDhcpDecline = 4;
constexpr quint8 kDhcpAck = 5;
constexpr quint8 kDhcpNak = 6;
constexpr quint8 kDhcpRelease = 7;

constexpr quint16 kFlagBroadcast = 0x8000;

quint16 ipChecksum(const uchar *data, int length)
{
    quint32 sum = 0;
    for (int i = 0; i + 1 < length; i += 2)
        sum += (static_cast<quint32>(data[i]) << 8) | data[i + 1];
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return static_cast<quint16>(~sum);
}

void put32(QByteArray &buf, quint32 value)
{
    char b[4];
    qToBigEndian(value, b);
    buf.append(b, 4);
}

void put16(QByteArray &buf, quint16 value)
{
    char b[2];
    qToBigEndian(value, b);
    buf.append(b, 2);
}

quint32 get32(const char *p)
{
    quint32 value;
    memcpy(&value, p, 4);
    return qFromBigEndian(value);
}

quint16 get16(const char *p)
{
    quint16 value;
    memcpy(&value, p, 2);
    return qFromBigEndian(value);
}

QString ipString(quint32 ip)
{
    return QHostAddress(ip).toString();
}

} // namespace

DhcpServer::DhcpServer(const QByteArray &serverMac, QObject *parent)
    : VirtualSwitchPort(parent)
    , m_serverMac(serverMac)
{
    m_clock.start();
}

void DhcpServer::configure(quint32 gateway,
                           quint32 serverIp,
                           int prefixLength,
                           quint32 rangeStart,
                           quint32 rangeEnd,
                           quint32 dnsServer,
                           const QList<quint32> &reserved)
{
    m_gateway = gateway;
    m_serverIp = serverIp != 0 ? serverIp : gateway;
    m_ownAddress = serverIp != 0;
    m_netmask = prefixLength > 0 && prefixLength <= 32 ? 0xffffffffu << (32 - prefixLength) : 0xffffff00u;
    m_rangeStart = rangeStart;
    m_rangeEnd = rangeEnd;
    m_dnsServer = dnsServer;

    m_reserved = QSet<quint32>(reserved.cbegin(), reserved.cend());
    m_reserved << m_gateway << m_serverIp << m_dnsServer;
    m_reserved.remove(0);

    qCDebug(KEMU) << "DhcpServer: configured, gateway" << ipString(m_gateway) << "server" << ipString(m_serverIp) << "netmask" << ipString(m_netmask) << "pool"
                  << ipString(m_rangeStart) << "-" << ipString(m_rangeEnd) << "dns" << (m_dnsServer != 0 ? ipString(m_dnsServer) : u"none"_s);
}

void DhcpServer::answerArp(const QByteArray &frame)
{
    // ARP requests for our service address get a reply so clients can reach
    // this server directly for unicast lease renewals.
    if (!m_ownAddress)
        return;
    if (frame.size() < 14 + 28 || get16(frame.constData() + 12) != kEtherTypeArp)
        return;

    const char *arp = frame.constData() + 14;
    if (get16(arp + 0) != 1 || get16(arp + 2) != kEtherTypeIpv4 || static_cast<quint8>(arp[4]) != 6 || static_cast<quint8>(arp[5]) != 4)
        return;
    if (get16(arp + 6) != 1 || get32(arp + 24) != m_serverIp) // request for us?
        return;

    qCDebug(KEMU) << "DhcpServer: answering ARP for" << ipString(m_serverIp) << "to" << QByteArray(frame.constData() + 6, 6).toHex(':');

    QByteArray reply;
    reply.append(frame.constData() + 6, 6); // dst = requester MAC
    reply.append(m_serverMac);
    put16(reply, kEtherTypeArp);
    put16(reply, 1); // Ethernet
    put16(reply, kEtherTypeIpv4);
    reply.append(char(6)).append(char(4));
    put16(reply, 2); // reply
    reply.append(m_serverMac);
    put32(reply, m_serverIp);
    reply.append(frame.constData() + 6, 6);
    reply.append(arp + 14, 4); // requester IP

    Q_EMIT frameReady(reply);
}

void DhcpServer::receiveFrame(const QByteArray &frame)
{
    answerArp(frame);

    // Ethernet + IPv4 + UDP/67 filter.
    if (frame.size() < 14 + 20 + 8 + 240)
        return;
    if (get16(frame.constData() + 12) != kEtherTypeIpv4)
        return;

    const char *ip = frame.constData() + 14;
    const int ihl = (ip[0] & 0x0f) * 4;
    if (ihl < 20 || frame.size() < 14 + ihl + 8 + 240)
        return;
    if (static_cast<quint8>(ip[9]) != kIpProtoUdp)
        return;

    const char *udp = ip + ihl;
    if (get16(udp + 0) != kDhcpClientPort || get16(udp + 2) != kDhcpServerPort)
        return;

    const char *bootp = udp + 8;
    if (static_cast<quint8>(bootp[0]) != 1) // BOOTREQUEST
        return;
    if (static_cast<quint8>(bootp[1]) != 1 || static_cast<quint8>(bootp[2]) != 6) // Ethernet, 6-byte MAC
        return;
    if (get32(bootp + 236) != kBootpMagicCookie)
        return;

    const quint32 xid = get32(bootp + 4);
    const quint16 flags = get16(bootp + 10);
    const quint32 ciaddr = get32(bootp + 12);
    const QByteArray clientMac = QByteArray(bootp + 28, 6);

    // No relay support: chaddr must be the sender, or one guest could
    // drain the pool with invented client addresses.
    if (clientMac != frame.mid(6, 6))
        return;

    // Options.
    quint8 msgType = 0;
    quint32 requestedIp = 0;
    const char *opt = bootp + 240;
    const char *end = frame.constData() + frame.size();
    while (opt < end) {
        const quint8 code = static_cast<quint8>(*opt++);
        if (code == 0)
            continue; // pad
        if (code == 255)
            break;
        if (opt >= end)
            break;
        const quint8 len = static_cast<quint8>(*opt++);
        if (opt + len > end)
            break;
        if (code == 53 && len == 1)
            msgType = static_cast<quint8>(*opt);
        else if (code == 50 && len == 4)
            requestedIp = get32(opt);
        opt += len;
    }

    expireLeases();

    qCDebug(KEMU) << "DhcpServer: message type" << msgType << "from" << clientMac.toHex(':') << "xid" << xid << "ciaddr" << ipString(ciaddr) << "requested"
                  << ipString(requestedIp) << "flags" << Qt::hex << flags << Qt::dec;

    switch (msgType) {
    case kDhcpDiscover: {
        const quint32 ip = allocateLease(clientMac, requestedIp);
        if (ip != 0)
            sendReply(kDhcpOffer, clientMac, xid, ip, flags);
        break;
    }
    case kDhcpRequest: {
        // Renewal via ciaddr, selection/rebinding via requested IP.
        const quint32 wanted = requestedIp != 0 ? requestedIp : ciaddr;
        const auto it = m_leases.constFind(clientMac);
        const bool ours = it != m_leases.constEnd() && it->ip == wanted;
        if (wanted != 0 && isInPool(wanted) && (ours || isLeasable(wanted))) {
            // A client asking for another free address moves its lease there
            // instead of being ACKed with an address it did not ask for.
            if (!ours)
                m_leases.remove(clientMac);
            sendReply(kDhcpAck, clientMac, xid, allocateLease(clientMac, wanted), flags);
            return;
        }
        sendReply(kDhcpNak, clientMac, xid, 0, kFlagBroadcast);
        break;
    }
    case kDhcpDecline: {
        // The address is in use by someone else (e.g. a static setup):
        // keep it out of the pool instead of handing it out again.
        const auto it = m_leases.find(clientMac);
        if (requestedIp != 0 && it != m_leases.end() && it->ip == requestedIp) {
            m_declined.insert(requestedIp, m_clock.elapsed() + kDeclineHoldSeconds * 1000);
            m_leases.erase(it);
            qCWarning(KEMU) << "DhcpServer:" << ipString(requestedIp) << "declined by" << clientMac.toHex(':') << "- address held back";
        }
        break;
    }
    case kDhcpRelease:
        releaseLease(clientMac, requestedIp != 0 ? requestedIp : ciaddr);
        break;
    default:
        break;
    }
}

quint32 DhcpServer::allocateLease(const QByteArray &clientMac, quint32 requestedIp)
{
    const auto it = m_leases.find(clientMac);
    if (it != m_leases.end()) {
        it->expiresAtMs = m_clock.elapsed() + kLeaseTimeSeconds * 1000;
        return it->ip;
    }

    quint32 ip = 0;
    if (requestedIp != 0 && isInPool(requestedIp) && isLeasable(requestedIp)) {
        ip = requestedIp;
    } else if (m_rangeStart != 0 && m_rangeStart <= m_rangeEnd) {
        // 64-bit counter: a range ending at 255.255.255.255 must not wrap.
        for (quint64 candidate = m_rangeStart; candidate <= m_rangeEnd; ++candidate) {
            if (isLeasable(static_cast<quint32>(candidate))) {
                ip = static_cast<quint32>(candidate);
                break;
            }
        }
    }

    if (ip != 0)
        m_leases.insert(clientMac, {ip, m_clock.elapsed() + kLeaseTimeSeconds * 1000});
    else
        qCWarning(KEMU) << "DhcpServer: address pool exhausted, no lease for" << clientMac.toHex(':');

    qCDebug(KEMU) << "DhcpServer: lease for" << clientMac.toHex(':') << "is" << ipString(ip);
    return ip;
}

void DhcpServer::releaseLease(const QByteArray &clientMac, quint32 ip)
{
    const auto it = m_leases.find(clientMac);
    if (it != m_leases.end() && (ip == 0 || it->ip == ip))
        m_leases.erase(it);
}

quint32 DhcpServer::leaseIpForMac(const QByteArray &clientMac) const
{
    const auto it = m_leases.constFind(clientMac);
    if (it == m_leases.constEnd() || it->expiresAtMs <= m_clock.elapsed())
        return 0;
    return it->ip;
}

bool DhcpServer::isInPool(quint32 ip) const
{
    return ip >= m_rangeStart && ip <= m_rangeEnd;
}

bool DhcpServer::isLeasable(quint32 ip) const
{
    return !m_reserved.contains(ip) && !m_declined.contains(ip) && !isLeased(ip);
}

bool DhcpServer::isLeased(quint32 ip) const
{
    const qint64 now = m_clock.elapsed();
    for (const Lease &lease : m_leases) {
        if (lease.ip == ip && lease.expiresAtMs > now)
            return true;
    }
    return false;
}

void DhcpServer::expireLeases()
{
    const qint64 now = m_clock.elapsed();
    for (auto it = m_leases.begin(); it != m_leases.end();) {
        if (it->expiresAtMs <= now)
            it = m_leases.erase(it);
        else
            ++it;
    }
    for (auto it = m_declined.begin(); it != m_declined.end();) {
        if (it.value() <= now)
            it = m_declined.erase(it);
        else
            ++it;
    }
}

void DhcpServer::setSearchDomain(const QString &domain)
{
    m_domainName.clear();
    m_domainSearch.clear();

    const QByteArray name = domain.toLatin1();
    if (name.isEmpty() || name.size() > 200)
        return;
    QByteArray encoded;
    for (const QByteArray &label : name.split('.')) {
        if (label.isEmpty() || label.size() > 63)
            return;
        encoded.append(char(label.size())).append(label);
    }
    encoded.append(char(0));
    m_domainName = name;
    m_domainSearch = encoded;
}

void DhcpServer::sendReply(quint8 msgType, const QByteArray &clientMac, quint32 xid, quint32 yiaddr, quint16 flags)
{
    const bool broadcast = (flags & kFlagBroadcast) || msgType == kDhcpNak;
    const quint32 dstIp = broadcast ? 0xffffffffu : (yiaddr != 0 ? yiaddr : 0xffffffffu);

    qCDebug(KEMU) << "DhcpServer: sending type" << msgType << "to" << clientMac.toHex(':') << "yiaddr" << ipString(yiaddr)
                  << (broadcast ? "broadcast" : "unicast");

    // BOOTP payload.
    QByteArray bootp;
    bootp.append(char(2)); // BOOTREPLY
    bootp.append(char(1)); // Ethernet
    bootp.append(char(6)); // MAC length
    bootp.append(char(0)); // hops
    put32(bootp, xid);
    put16(bootp, 0); // secs
    put16(bootp, flags);
    put32(bootp, 0); // ciaddr
    put32(bootp, yiaddr);
    put32(bootp, m_serverIp); // siaddr
    put32(bootp, 0); // giaddr
    bootp.append(clientMac);
    bootp.append(10, char(0)); // chaddr padding
    bootp.append(64, char(0)); // sname
    bootp.append(128, char(0)); // file
    put32(bootp, kBootpMagicCookie);

    // Options: message type, server id, lease time, netmask, router, DNS.
    bootp.append(char(53)).append(char(1)).append(char(msgType));
    bootp.append(char(54)).append(char(4));
    put32(bootp, m_serverIp);
    bootp.append(char(51)).append(char(4));
    put32(bootp, static_cast<quint32>(kLeaseTimeSeconds));
    bootp.append(char(1)).append(char(4));
    put32(bootp, m_netmask);
    bootp.append(char(3)).append(char(4));
    put32(bootp, m_gateway);
    if (m_dnsServer != 0) {
        bootp.append(char(6)).append(char(4));
        put32(bootp, m_dnsServer);
    }
    if (!m_domainName.isEmpty()) {
        bootp.append(char(15)).append(char(m_domainName.size())).append(m_domainName);
        bootp.append(char(119)).append(char(m_domainSearch.size())).append(m_domainSearch);
    }
    bootp.append(char(255));

    // UDP header. Checksum stays 0 (optional for IPv4).
    QByteArray udp;
    put16(udp, kDhcpServerPort);
    put16(udp, kDhcpClientPort);
    put16(udp, static_cast<quint16>(8 + bootp.size()));
    put16(udp, 0);
    udp.append(bootp);

    // IPv4 header.
    QByteArray ip;
    ip.append(char(0x45)); // version 4, IHL 5
    ip.append(char(0)); // DSCP/ECN
    put16(ip, static_cast<quint16>(20 + udp.size()));
    put16(ip, 0); // identification
    put16(ip, 0); // flags/fragment offset
    ip.append(char(64)); // TTL
    ip.append(char(kIpProtoUdp));
    put16(ip, 0); // checksum placeholder
    put32(ip, m_serverIp);
    put32(ip, dstIp);
    const quint16 checksum = ipChecksum(reinterpret_cast<const uchar *>(ip.constData()), ip.size());
    qToBigEndian(checksum, ip.data() + 10);

    // Ethernet header. Unicast replies reach the client by MAC even before it
    // owns its address; broadcast-flagged clients get a broadcast frame.
    QByteArray frame;
    if (broadcast)
        frame.append(6, char(0xff));
    else
        frame.append(clientMac);
    frame.append(m_serverMac);
    put16(frame, kEtherTypeIpv4);
    frame.append(ip);
    frame.append(udp);

    Q_EMIT frameReady(frame);
}
