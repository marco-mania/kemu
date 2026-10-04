/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QByteArray>

#include "utils/virtualswitch.h"

/**
 * @brief Gives the gateway address a minimal presence on an isolated
 * (NAT-less) virtual network.
 *
 * Answers ARP requests for the gateway IP and ICMP echo requests sent to it,
 * so guests can resolve and ping their configured gateway even without an
 * uplink. When an uplink (NAT) is active the uplink answers for the gateway
 * itself and this port is not attached.
 */
class GatewayResponder : public VirtualSwitchPort
{
    Q_OBJECT

public:
    explicit GatewayResponder(const QByteArray &mac, quint32 gatewayIp, QObject *parent = nullptr);

    void receiveFrame(const QByteArray &frame) override;

private:
    void answerArp(const QByteArray &frame);
    void answerIcmpEcho(const QByteArray &frame);

    QByteArray m_mac;
    quint32 m_gatewayIp; // host byte order
};
