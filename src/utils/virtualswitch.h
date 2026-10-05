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

class QLocalSocket;

/**
 * @brief One internal (in-process) port of a VirtualSwitch.
 *
 * Internal ports are KEMU-side services attached to the switch fabric
 * (DHCP server, gateway responder, NAT uplink). The switch delivers frames
 * via receiveFrame(); a port emits frameReady() to put a frame on the wire.
 * A port receives broadcast/multicast frames and unicast frames addressed
 * to a MAC it owns, never flooded unicast.
 */
class VirtualSwitchPort : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    /// Called with a complete Ethernet frame taken from the wire.
    virtual void receiveFrame(const QByteArray &frame) = 0;

    [[nodiscard]] virtual bool ownsMac(const QByteArray &mac) const = 0;

Q_SIGNALS:
    /// The port wants to transmit a complete Ethernet frame onto the wire.
    void frameReady(const QByteArray &frame);
};

/**
 * @brief In-memory L2 switch for virtual networks.
 *
 * Every guest gets its own port: a socketpair whose guest end is handed to
 * one QEMU instance as "-netdev stream,addr.type=fd,addr.str=<fd>" (4-byte
 * big-endian length prefix + Ethernet frame). Nothing is created in the file
 * system, so no other process can connect to a port. A port opened with a
 * MAC only accepts frames from that source MAC and owns it in the MAC table
 * (port security); a port without one learns source MACs. Broadcast and
 * multicast frames are flooded, unknown unicast only to guest ports.
 *
 * Runs entirely in the KEMU process: no root rights, no kernel interfaces,
 * no namespaces, fully functional inside sandboxed (Flatpak) environments.
 */
class VirtualSwitch : public QObject
{
    Q_OBJECT

public:
    explicit VirtualSwitch(QObject *parent = nullptr);
    ~VirtualSwitch() override;

    /// Opens a guest port and returns the descriptor for QEMU's
    /// "addr.type=fd", or -1 on failure (portId or a non-empty mac in use).
    /// The switch keeps that descriptor open; the caller passes it to the
    /// guest process and then calls releaseGuestFd(), it must never close it
    /// itself.
    int addPort(const QString &portId, const QByteArray &mac = {});

    /// Descriptor of the port's guest end, -1 once released or unknown.
    [[nodiscard]] int guestFd(const QString &portId) const;

    /// Closes the switch's copy of the guest descriptor, after the guest
    /// process has inherited it. Without this the port never sees the guest
    /// go away.
    void releaseGuestFd(const QString &portId);

    void removePort(const QString &portId);
    [[nodiscard]] bool hasPort(const QString &portId) const;

    /// Closes all guest ports and disconnects their clients.
    void close();

    [[nodiscard]] int clientCount() const;
    [[nodiscard]] QString lastError() const
    {
        return m_lastError;
    }

    /// Attach an internal port (not owned). The switch subscribes to the
    /// port's frameReady() signal for the lifetime of the attachment.
    void addInternalPort(VirtualSwitchPort *port);
    void removeInternalPort(VirtualSwitchPort *port);

Q_SIGNALS:
    void clientConnected();
    void clientDisconnected();

private:
    struct GuestPort {
        QString portId;
        QByteArray mac; // empty: source MACs are learned
        QPointer<QLocalSocket> socket; // switch end of the socketpair
        int guestFd = -1; // guest end, -1 once handed over
        QByteArray readBuffer;
        bool spoofLogged = false;
        bool congested = false;
    };

    void onReadyRead(GuestPort *port);
    void onSocketDisconnected(GuestPort *port);

    void forwardFrame(const QByteArray &frame, GuestPort *fromGuest, VirtualSwitchPort *fromInternal);
    void writeFrame(GuestPort *port, const QByteArray &frame);
    [[nodiscard]] GuestPort *findPort(const QString &portId) const;

    QList<GuestPort *> m_ports; // owned
    QList<QPointer<VirtualSwitchPort>> m_internalPorts;
    QHash<QByteArray, GuestPort *> m_macTable;
    QString m_lastError;
};
