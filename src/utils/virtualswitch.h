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

class QLocalServer;
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
 * Every guest gets its own port: a QLocalServer that one QEMU instance
 * connects to via "-netdev stream,server=off,addr.type=unix,addr.path=<path>"
 * (4-byte big-endian length prefix + Ethernet frame). A port opened with a
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

    /// Opens a guest port listening on socketPath; a stale socket file is
    /// removed first. Fails if socketPath or a non-empty mac is in use.
    bool addPort(const QString &socketPath, const QByteArray &mac = {});
    void removePort(const QString &socketPath);
    [[nodiscard]] bool hasPort(const QString &socketPath) const;

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
        QString socketPath;
        QByteArray mac; // empty: source MACs are learned
        QLocalServer *server = nullptr;
        QPointer<QLocalSocket> socket;
        QByteArray readBuffer;
        bool spoofLogged = false;
        bool congested = false;
    };

    void onNewConnection(GuestPort *port);
    void onReadyRead(GuestPort *port);
    void onSocketDisconnected(GuestPort *port);

    void forwardFrame(const QByteArray &frame, GuestPort *fromGuest, VirtualSwitchPort *fromInternal);
    void writeFrame(GuestPort *port, const QByteArray &frame);
    [[nodiscard]] GuestPort *findPort(const QString &socketPath) const;

    QList<GuestPort *> m_ports; // owned
    QList<QPointer<VirtualSwitchPort>> m_internalPorts;
    QHash<QByteArray, GuestPort *> m_macTable;
    QString m_lastError;
};
