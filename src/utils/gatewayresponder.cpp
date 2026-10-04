/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gatewayresponder.h"

#include <QHostAddress>
#include <QtEndian>

#include <cstring>

#include "kemu_debug.h"

namespace
{

constexpr quint16 kEtherTypeIpv4 = 0x0800;
constexpr quint16 kEtherTypeArp = 0x0806;
constexpr quint8 kIpProtoIcmp = 1;

quint16 internetChecksum(const uchar *data, int length)
{
    quint32 sum = 0;
    for (int i = 0; i + 1 < length; i += 2)
        sum += (static_cast<quint32>(data[i]) << 8) | data[i + 1];
    if (length & 1)
        sum += static_cast<quint32>(data[length - 1]) << 8;
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return static_cast<quint16>(~sum);
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

} // namespace

GatewayResponder::GatewayResponder(const QByteArray &mac, quint32 gatewayIp, QObject *parent)
    : VirtualSwitchPort(parent)
    , m_mac(mac)
    , m_gatewayIp(gatewayIp)
{
}

void GatewayResponder::receiveFrame(const QByteArray &frame)
{
    if (frame.size() < 14)
        return;

    const quint16 etherType = get16(frame.constData() + 12);
    if (etherType == kEtherTypeArp)
        answerArp(frame);
    else if (etherType == kEtherTypeIpv4)
        answerIcmpEcho(frame);
}

void GatewayResponder::answerArp(const QByteArray &frame)
{
    // Ethernet/IPv4 ARP request for the gateway address?
    if (frame.size() < 14 + 28)
        return;
    const char *arp = frame.constData() + 14;
    if (get16(arp + 0) != 1 || get16(arp + 2) != kEtherTypeIpv4 || static_cast<quint8>(arp[4]) != 6 || static_cast<quint8>(arp[5]) != 4)
        return;
    if (get16(arp + 6) != 1) // request
        return;
    if (get32(arp + 24) != m_gatewayIp)
        return;

    qCDebug(KEMU) << "GatewayResponder: answering ARP for" << QHostAddress(m_gatewayIp).toString() << "to" << QByteArray(frame.constData() + 6, 6).toHex(':');

    QByteArray reply;
    reply.append(frame.mid(6, 6)); // dst = sender MAC
    reply.append(m_mac);
    reply.append(frame.mid(12, 2)); // ethertype ARP

    QByteArray body;
    body.append(arp, 8); // htype, ptype, hlen, plen
    char op[2];
    qToBigEndian<quint16>(2, op); // reply
    body.append(op, 2);
    body.append(m_mac); // sender MAC
    char gw[4];
    qToBigEndian(m_gatewayIp, gw);
    body.append(gw, 4); // sender IP
    body.append(frame.mid(14 + 8, 6)); // target MAC = original sender MAC
    body.append(frame.mid(14 + 18, 4)); // target IP = original sender IP
    reply.append(body);

    Q_EMIT frameReady(reply);
}

void GatewayResponder::answerIcmpEcho(const QByteArray &frame)
{
    if (frame.size() < 14 + 20 + 8)
        return;

    const char *ip = frame.constData() + 14;
    const int ihl = (ip[0] & 0x0f) * 4;
    if (ihl < 20 || frame.size() < 14 + ihl + 8)
        return;
    if (static_cast<quint8>(ip[9]) != kIpProtoIcmp)
        return;
    if (get32(ip + 16) != m_gatewayIp)
        return;

    const char *icmp = ip + ihl;
    if (static_cast<quint8>(icmp[0]) != 8) // echo request
        return;

    qCDebug(KEMU) << "GatewayResponder: answering ICMP echo to" << QHostAddress(m_gatewayIp).toString() << "from" << QHostAddress(get32(ip + 12)).toString();

    const int ipTotalLength = get16(ip + 2);
    if (ipTotalLength < ihl + 8 || frame.size() < 14 + ipTotalLength)
        return;

    QByteArray reply;
    reply.append(frame.mid(6, 6)); // dst MAC = sender MAC
    reply.append(m_mac);
    reply.append(frame.mid(12, 2)); // ethertype IPv4

    QByteArray ipOut = frame.mid(14, ipTotalLength);
    ipOut[16] = ip[12]; // swap addresses
    ipOut[17] = ip[13];
    ipOut[18] = ip[14];
    ipOut[19] = ip[15];
    qToBigEndian(m_gatewayIp, ipOut.data() + 12);
    ipOut[10] = 0;
    ipOut[11] = 0;
    const quint16 ipSum = internetChecksum(reinterpret_cast<const uchar *>(ipOut.constData()), ihl);
    qToBigEndian(ipSum, ipOut.data() + 10);

    ipOut[ihl] = 0; // echo reply
    ipOut[ihl + 2] = 0;
    ipOut[ihl + 3] = 0;
    const quint16 icmpSum = internetChecksum(reinterpret_cast<const uchar *>(ipOut.constData()) + ihl, ipTotalLength - ihl);
    qToBigEndian(icmpSum, ipOut.data() + ihl + 2);

    reply.append(ipOut);
    Q_EMIT frameReady(reply);
}
