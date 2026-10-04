/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dnsserver.h"

#include <QFile>
#include <QHostAddress>
#include <QNetworkDatagram>
#include <QStringList>
#include <QTimer>
#include <QUdpSocket>
#include <QtEndian>

#include <cstring>

#include "utils/dhcpserver.h"

#include "kemu_debug.h"

using namespace Qt::StringLiterals;

namespace
{

constexpr quint16 kEtherTypeIpv4 = 0x0800;
constexpr quint16 kEtherTypeArp = 0x0806;
constexpr quint8 kIpProtoUdp = 17;
constexpr quint16 kDnsPort = 53;

constexpr quint16 kDnsTypeA = 1;
constexpr quint16 kDnsTypePtr = 12;
constexpr quint16 kDnsTypeAaaa = 28;
constexpr quint16 kDnsTypeAny = 255;
constexpr quint16 kDnsClassIn = 1;
constexpr quint16 kDnsClassAny = 255;

constexpr quint32 kDefaultTtl = 30;
constexpr int kUpstreamTimeoutMs = 4000;

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

/// Parses a possibly compressed DNS name at offset into a dotted lowercase
/// string. endOffset receives the position right after the name's first
/// (uncompressed) occurrence, i.e. where QTYPE begins in a question.
bool parseDnsName(const char *data, int size, int offset, QString *name, int *endOffset)
{
    QStringList labels;
    int pos = offset;
    int jumps = 0;
    bool jumped = false;

    while (true) {
        if (pos >= size)
            return false;
        const quint8 len = static_cast<quint8>(data[pos]);
        if ((len & 0xc0) == 0xc0) { // compression pointer
            if (pos + 1 >= size)
                return false;
            const int target = ((len & 0x3f) << 8) | static_cast<quint8>(data[pos + 1]);
            if (!jumped)
                *endOffset = pos + 2;
            jumped = true;
            if (++jumps > 16 || target >= size)
                return false;
            pos = target;
            continue;
        }
        if (len & 0xc0)
            return false; // reserved label types
        ++pos;
        if (len == 0) {
            if (!jumped)
                *endOffset = pos;
            break;
        }
        if (pos + len > size)
            return false;
        labels << QString::fromLatin1(data + pos, len).toLower();
        pos += len;
    }

    *name = labels.join(u'.');
    return true;
}

void appendDnsName(QByteArray &buf, const QString &name)
{
    const QStringList labels = name.split(u'.');
    for (const QString &label : labels) {
        const QByteArray l = label.toLatin1();
        buf.append(char(l.size()));
        buf.append(l);
    }
    buf.append(char(0));
}

/// First IPv4 nameserver from /etc/resolv.conf, 0 if none.
quint32 firstUpstreamResolver()
{
    QFile file(u"/etc/resolv.conf"_s);
    if (!file.open(QIODevice::ReadOnly))
        return 0;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (!line.startsWith("nameserver"))
            continue;
        const QHostAddress addr(QString::fromLatin1(line.mid(10).trimmed()));
        if (addr.protocol() == QAbstractSocket::IPv4Protocol)
            return addr.toIPv4Address();
    }
    return 0;
}

} // namespace

DnsServer::DnsServer(const QByteArray &serverMac, QObject *parent)
    : VirtualSwitchPort(parent)
    , m_serverMac(serverMac)
{
}

void DnsServer::configure(quint32 serverIp, const QString &zone, bool forwardUpstream)
{
    m_serverIp = serverIp;
    m_zone = zone.toLower();
    m_forwardUpstream = forwardUpstream;
    m_upstreamIp = forwardUpstream ? firstUpstreamResolver() : 0;

    if (m_upstreamIp != 0) {
        m_upstream = new QUdpSocket(this);
        connect(m_upstream, &QUdpSocket::readyRead, this, &DnsServer::onUpstreamReadyRead);
    }

    qCDebug(KEMU) << "DnsServer: configured, address" << QHostAddress(m_serverIp).toString() << "zone" << m_zone
                  << "upstream" << (m_upstreamIp != 0 ? QHostAddress(m_upstreamIp).toString() : u"none"_s);
}

void DnsServer::setDhcpServer(DhcpServer *dhcp)
{
    m_dhcp = dhcp;
}

void DnsServer::addHost(const QString &hostname, const QByteArray &mac)
{
    if (hostname.isEmpty() || mac.size() != 6)
        return;
    m_hostByMac.insert(mac, hostname.toLower());
    qCDebug(KEMU) << "DnsServer: registered host" << hostname.toLower() << "for" << mac.toHex(':');
}

void DnsServer::removeHost(const QByteArray &mac)
{
    m_hostByMac.remove(mac);
}

quint32 DnsServer::addressForHost(const QString &hostname) const
{
    if (!m_dhcp)
        return 0;
    for (auto it = m_hostByMac.constBegin(); it != m_hostByMac.constEnd(); ++it) {
        if (it.value() == hostname) {
            const quint32 ip = m_dhcp->leaseIpForMac(it.key());
            if (ip != 0)
                return ip;
        }
    }
    return 0;
}

QString DnsServer::hostForAddress(quint32 ip) const
{
    if (!m_dhcp)
        return {};
    for (auto it = m_hostByMac.constBegin(); it != m_hostByMac.constEnd(); ++it) {
        if (m_dhcp->leaseIpForMac(it.key()) == ip)
            return it.value() + u'.' + m_zone;
    }
    return {};
}

void DnsServer::answerArp(const QByteArray &frame)
{
    if (frame.size() < 14 + 28 || get16(frame.constData() + 12) != kEtherTypeArp)
        return;

    const char *arp = frame.constData() + 14;
    if (get16(arp + 0) != 1 || get16(arp + 2) != kEtherTypeIpv4 || static_cast<quint8>(arp[4]) != 6 || static_cast<quint8>(arp[5]) != 4)
        return;
    if (get16(arp + 6) != 1 || get32(arp + 24) != m_serverIp) // request for us?
        return;

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

void DnsServer::receiveFrame(const QByteArray &frame)
{
    answerArp(frame);

    if (m_serverIp == 0)
        return;

    // Ethernet + IPv4 + UDP/53 addressed to us.
    if (frame.size() < 14 + 20 + 8 + 12)
        return;
    if (get16(frame.constData() + 12) != kEtherTypeIpv4)
        return;

    const char *ip = frame.constData() + 14;
    const int ihl = (ip[0] & 0x0f) * 4;
    if (ihl < 20 || frame.size() < 14 + ihl + 8 + 12)
        return;
    if (static_cast<quint8>(ip[9]) != kIpProtoUdp)
        return;
    if (get32(ip + 16) != m_serverIp)
        return;

    const char *udp = ip + ihl;
    if (get16(udp + 2) != kDnsPort)
        return;

    handleQuery(frame);
}

void DnsServer::handleQuery(const QByteArray &frame)
{
    const char *ip = frame.constData() + 14;
    const int ihl = (ip[0] & 0x0f) * 4;
    const char *udp = ip + ihl;
    const char *dns = udp + 8;
    const int dnsSize = frame.size() - 14 - ihl - 8;

    const QByteArray clientMac = frame.mid(6, 6);
    const quint32 clientIp = get32(ip + 12);
    const quint16 clientPort = get16(udp + 0);

    const quint16 id = get16(dns + 0);
    const quint16 flags = get16(dns + 2);
    if (flags & 0x8000) // not a query
        return;
    if (get16(dns + 4) != 1) // exactly one question
        return;

    QString qname;
    int questionEnd = 0;
    if (!parseDnsName(dns, dnsSize, 12, &qname, &questionEnd) || questionEnd + 4 > dnsSize)
        return;
    const quint16 qtype = get16(dns + questionEnd);
    const quint16 qclass = get16(dns + questionEnd + 2);

    qCDebug(KEMU) << "DnsServer: query" << qname << "type" << qtype << "from" << QHostAddress(clientIp).toString();

    // Response scaffold: header + echoed question section.
    const auto buildResponse = [&](quint16 rcode, bool authoritative, const QByteArray &answers, int answerCount) {
        QByteArray response = QByteArray(dns, questionEnd + 4);
        quint16 rflags = 0x8000 | (flags & 0x0100); // QR, preserve RD
        if (authoritative)
            rflags |= 0x0400;
        if (m_upstreamIp != 0)
            rflags |= 0x0080; // RA
        rflags |= rcode & 0x0f;
        qToBigEndian(rflags, response.data() + 2);
        qToBigEndian<quint16>(static_cast<quint16>(answerCount), response.data() + 6);
        qToBigEndian<quint16>(0, response.data() + 8); // NSCOUNT
        qToBigEndian<quint16>(0, response.data() + 10); // ARCOUNT (strip EDNS)
        response.append(answers);
        sendResponse(clientMac, clientIp, clientPort, response);
    };

    // PTR: reverse lookup into the lease table.
    if (qtype == kDnsTypePtr && qname.endsWith(u".in-addr.arpa"_s)) {
        const QStringList parts = qname.left(qname.size() - 13).split(u'.');
        if (parts.size() == 4) {
            quint32 addr = 0;
            bool ok = true;
            for (int i = 3; i >= 0; --i) {
                bool okPart = false;
                const uint part = parts.at(i).toUInt(&okPart);
                if (!okPart || part > 255) {
                    ok = false;
                    break;
                }
                addr = (addr << 8) | part;
            }
            if (ok) {
                const QString fqdn = hostForAddress(addr);
                if (!fqdn.isEmpty()) {
                    QByteArray answer;
                    put16(answer, 0xc00c); // name: pointer to the question
                    put16(answer, kDnsTypePtr);
                    put16(answer, kDnsClassIn);
                    put32(answer, kDefaultTtl);
                    QByteArray rdata;
                    appendDnsName(rdata, fqdn);
                    put16(answer, static_cast<quint16>(rdata.size()));
                    answer.append(rdata);
                    qCDebug(KEMU) << "DnsServer: PTR" << QHostAddress(addr).toString() << "->" << fqdn;
                    buildResponse(0, true, answer, 1);
                    return;
                }
            }
        }
        forwardQuery(id, clientMac, clientIp, clientPort, QByteArray(dns, dnsSize));
        return;
    }

    if (qclass != kDnsClassIn && qclass != kDnsClassAny) {
        forwardQuery(id, clientMac, clientIp, clientPort, QByteArray(dns, dnsSize));
        return;
    }

    // Local zone: "<label>.<zone>".
    const bool inZone = qname == m_zone || qname.endsWith(u'.' + m_zone);
    if (!inZone) {
        forwardQuery(id, clientMac, clientIp, clientPort, QByteArray(dns, dnsSize));
        return;
    }

    const QString label = qname == m_zone ? QString() : qname.left(qname.size() - m_zone.size() - 1);
    quint32 addr = 0;
    if (!label.isEmpty() && !label.contains(u'.'))
        addr = addressForHost(label);

    if (addr == 0) {
        // Known zone, unknown name (or zone apex): NXDOMAIN / NODATA.
        const bool exists = !label.isEmpty() && !label.contains(u'.') && [&]() {
            for (const QString &host : m_hostByMac)
                if (host == label)
                    return true;
            return false;
        }();
        buildResponse(exists ? 0 : 3, true, {}, 0);
        if (exists)
            qCDebug(KEMU) << "DnsServer: NODATA for" << qname;
        else
            qCDebug(KEMU) << "DnsServer: NXDOMAIN for" << qname;
        return;
    }

    if (qtype != kDnsTypeA && qtype != kDnsTypeAny && qtype != kDnsTypeAaaa) {
        buildResponse(0, true, {}, 0); // NODATA for other types
        return;
    }

    QByteArray answers;
    int count = 0;
    if (qtype == kDnsTypeA || qtype == kDnsTypeAny) {
        put16(answers, 0xc00c); // name: pointer to the question
        put16(answers, kDnsTypeA);
        put16(answers, kDnsClassIn);
        put32(answers, kDefaultTtl);
        put16(answers, 4);
        put32(answers, addr);
        ++count;
    }
    // AAAA: NODATA - the virtual network is IPv4-only.

    qCDebug(KEMU) << "DnsServer: A" << qname << "->" << QHostAddress(addr).toString();
    buildResponse(0, true, answers, count);
}

void DnsServer::forwardQuery(quint16 id, const QByteArray &clientMac, quint32 clientIp, quint16 clientPort, const QByteArray &dnsPayload)
{
    if (!m_upstream || m_upstreamIp == 0) {
        // REFUSED on isolated networks, SERVFAIL if no resolver was found.
        const quint16 rcode = m_forwardUpstream ? 2 : 5;
        qCDebug(KEMU) << "DnsServer: not forwarding query id" << id << "- rcode" << rcode;
        QByteArray response = dnsPayload;
        quint16 rflags = get16(response.constData() + 2);
        rflags = (rflags & 0x0100) | 0x8000 | rcode;
        qToBigEndian(rflags, response.data() + 2);
        sendResponse(clientMac, clientIp, clientPort, response);
        return;
    }

    PendingQuery pending{clientMac, clientIp, clientPort};
    m_pending[id].append(pending);
    QTimer::singleShot(kUpstreamTimeoutMs, this, [this, id]() {
        m_pending.remove(id);
    });

    qCDebug(KEMU) << "DnsServer: forwarding query id" << id << "to" << QHostAddress(m_upstreamIp).toString();
    m_upstream->writeDatagram(dnsPayload, QHostAddress(m_upstreamIp), kDnsPort);
}

void DnsServer::onUpstreamReadyRead()
{
    while (m_upstream->hasPendingDatagrams()) {
        const QByteArray answer = m_upstream->receiveDatagram().data();
        if (answer.size() < 2)
            continue;
        const quint16 id = get16(answer.constData());
        const auto pendings = m_pending.take(id);
        for (const PendingQuery &pending : pendings)
            sendResponse(pending.clientMac, pending.clientIp, pending.clientPort, answer);
        if (!pendings.isEmpty())
            qCDebug(KEMU) << "DnsServer: relayed upstream answer for id" << id;
    }
}

void DnsServer::sendResponse(const QByteArray &clientMac, quint32 clientIp, quint16 clientPort, const QByteArray &dnsPayload)
{
    // UDP header. Checksum stays 0 (optional for IPv4).
    QByteArray udp;
    put16(udp, kDnsPort);
    put16(udp, clientPort);
    put16(udp, static_cast<quint16>(8 + dnsPayload.size()));
    put16(udp, 0);
    udp.append(dnsPayload);

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
    put32(ip, clientIp);
    const quint16 checksum = ipChecksum(reinterpret_cast<const uchar *>(ip.constData()), ip.size());
    qToBigEndian(checksum, ip.data() + 10);

    QByteArray frame;
    frame.append(clientMac);
    frame.append(m_serverMac);
    put16(frame, kEtherTypeIpv4);
    frame.append(ip);
    frame.append(udp);

    Q_EMIT frameReady(frame);
}
