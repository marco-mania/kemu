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
 */
class VirtualSwitchPort : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    /// Called with a complete Ethernet frame taken from the wire.
    virtual void receiveFrame(const QByteArray &frame) = 0;

Q_SIGNALS:
    /// The port wants to transmit a complete Ethernet frame onto the wire.
    void frameReady(const QByteArray &frame);
};

/**
 * @brief In-memory L2 switch for virtual networks.
 *
 * QEMU instances connect to the QLocalServer via
 * "-netdev stream,server=off,addr.type=unix,addr.path=<socketPath>"
 * (4-byte big-endian length prefix + Ethernet frame). Frames are forwarded
 * between sockets and internal ports using MAC learning; unknown unicast,
 * broadcast and multicast frames are flooded.
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

    /// Starts listening on socketPath; a stale socket file is removed first.
    bool listen(const QString &socketPath);

    /// Stops listening and disconnects all clients.
    void close();

    [[nodiscard]] bool isListening() const;
    [[nodiscard]] QString socketPath() const;
    [[nodiscard]] int clientCount() const;

    /// Attach an internal port (not owned). The switch subscribes to the
    /// port's frameReady() signal for the lifetime of the attachment.
    void addInternalPort(VirtualSwitchPort *port);
    void removeInternalPort(VirtualSwitchPort *port);

Q_SIGNALS:
    void clientConnected();
    void clientDisconnected();

private:
    void onNewConnection();
    void onReadyRead(QLocalSocket *socket);
    void onSocketDisconnected(QLocalSocket *socket);

    /// Learn the source MAC and forward the frame. `source` identifies the
    /// origin: either a QLocalSocket* or a VirtualSwitchPort*.
    void forwardFrame(QObject *source, const QByteArray &frame);
    void writeFrame(QLocalSocket *socket, const QByteArray &frame);

    QLocalServer *m_server = nullptr;
    QString m_socketPath;

    QList<QPointer<QLocalSocket>> m_clients;
    QList<QPointer<VirtualSwitchPort>> m_internalPorts;

    QHash<QByteArray, QPointer<QLocalSocket>> m_macToClient;
    QHash<QByteArray, QPointer<VirtualSwitchPort>> m_macToInternal;
    QHash<QLocalSocket *, QByteArray> m_readBuffers;
};
