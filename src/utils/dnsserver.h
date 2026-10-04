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

#include "utils/virtualswitch.h"

class DhcpServer;
class QUdpSocket;

/**
 * @brief Minimal DNS server attached to a VirtualSwitch as an internal port.
 *
 * Answers A/AAAA/PTR queries for the network-local zone
 * "<hostname>.<network>.lan" from the DHCP lease table. With upstream
 * forwarding enabled, everything else goes to the host's resolver (first
 * IPv4 nameserver from /etc/resolv.conf); otherwise it is refused, so an
 * isolated network has no path to the outside. UDP only.
 */
class DnsServer : public VirtualSwitchPort
{
    Q_OBJECT

public:
    explicit DnsServer(const QByteArray &serverMac, QObject *parent = nullptr);

    /// All addresses in host byte order. serverIp is this server's own
    /// address; the server answers ARP requests for it. zone is the local
    /// DNS zone (e.g. "test-net.lan"). forwardUpstream must only be set for
    /// networks with internet access (NAT).
    void configure(quint32 serverIp, const QString &zone, bool forwardUpstream);

    /// Lease source used to resolve hostnames to addresses (may be null
    /// when DHCP is disabled - the local zone then stays empty).
    void setDhcpServer(DhcpServer *dhcp);

    /// Registers a VM hostname for its NIC MAC. The address is resolved
    /// lazily from the DHCP lease table at query time.
    void addHost(const QString &hostname, const QByteArray &mac);
    void removeHost(const QByteArray &mac);

    void receiveFrame(const QByteArray &frame) override;

private:
    struct PendingQuery {
        QByteArray clientMac;
        quint32 clientIp = 0;
        quint16 clientPort = 0;
    };

    void answerArp(const QByteArray &frame);
    void handleQuery(const QByteArray &frame);
    void sendResponse(const QByteArray &clientMac, quint32 clientIp, quint16 clientPort, const QByteArray &dnsPayload);
    void forwardQuery(quint16 id, const QByteArray &clientMac, quint32 clientIp, quint16 clientPort, const QByteArray &dnsPayload);
    void onUpstreamReadyRead();

    /// A record for hostname if it has a valid lease, 0 otherwise.
    quint32 addressForHost(const QString &hostname) const;
    /// FQDN for the VM holding the lease for ip, empty if none.
    QString hostForAddress(quint32 ip) const;

    QByteArray m_serverMac;
    quint32 m_serverIp = 0;
    QString m_zone; // e.g. "test-net.lan"

    QHash<QByteArray, QString> m_hostByMac; // NIC MAC -> hostname label
    QPointer<DhcpServer> m_dhcp;

    bool m_forwardUpstream = false;
    QUdpSocket *m_upstream = nullptr;
    quint32 m_upstreamIp = 0;
    QHash<quint16, QList<PendingQuery>> m_pending; // key: DNS transaction id
};
