/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "virtualswitch.h"

#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QtEndian>

#include "kemu_debug.h"

using namespace Qt::StringLiterals;

// QEMU stream netdev framing: 4-byte big-endian length + Ethernet frame.
static constexpr quint32 kMaxFrameSize = 65535;

// Frames for a client that stops reading (e.g. a paused VM) are dropped
// once this much is queued, like a switch with a full egress queue.
static constexpr qint64 kMaxPendingWriteBytes = 4 * 1024 * 1024;

VirtualSwitch::VirtualSwitch(QObject *parent)
    : QObject(parent)
{
}

VirtualSwitch::~VirtualSwitch()
{
    close();
}

bool VirtualSwitch::addPort(const QString &socketPath, const QByteArray &mac)
{
    m_lastError.clear();

    if (findPort(socketPath)) {
        m_lastError = u"port %1 already exists"_s.arg(socketPath);
        return false;
    }
    if (!mac.isEmpty()) {
        const GuestPort *owner = m_macTable.value(mac);
        if (owner && owner->mac == mac) {
            m_lastError = u"MAC address %1 is already bound to another port"_s.arg(QString::fromLatin1(mac.toHex(':')));
            return false;
        }
    }

    QFile::remove(socketPath);

    auto *server = new QLocalServer(this);
    server->setMaxPendingConnections(1);
    if (!server->listen(socketPath)) {
        m_lastError = server->errorString();
        qCWarning(KEMU) << "VirtualSwitch: failed to listen on" << socketPath << ":" << m_lastError;
        delete server;
        return false;
    }

    auto *port = new GuestPort;
    port->socketPath = socketPath;
    port->mac = mac;
    port->server = server;
    m_ports.append(port);
    if (!mac.isEmpty())
        m_macTable.insert(mac, port);

    connect(server, &QLocalServer::newConnection, this, [this, port]() {
        onNewConnection(port);
    });

    qCDebug(KEMU) << "VirtualSwitch: port listening on" << socketPath << "for" << (mac.isEmpty() ? QByteArray("any MAC") : mac.toHex(':'));
    return true;
}

void VirtualSwitch::removePort(const QString &socketPath)
{
    GuestPort *port = findPort(socketPath);
    if (!port)
        return;

    m_ports.removeOne(port);
    for (auto it = m_macTable.begin(); it != m_macTable.end();) {
        if (it.value() == port)
            it = m_macTable.erase(it);
        else
            ++it;
    }

    const bool hadClient = port->socket;
    disconnect(port->server, nullptr, this, nullptr);
    if (port->socket) {
        disconnect(port->socket, nullptr, this, nullptr);
        port->socket->abort();
    }
    port->server->close();
    port->server->deleteLater(); // also deletes the client socket
    QFile::remove(socketPath);
    delete port;

    qCDebug(KEMU) << "VirtualSwitch: port" << socketPath << "removed";
    if (hadClient)
        Q_EMIT clientDisconnected();
}

bool VirtualSwitch::hasPort(const QString &socketPath) const
{
    const GuestPort *port = findPort(socketPath);
    return port && port->server->isListening();
}

void VirtualSwitch::close()
{
    while (!m_ports.isEmpty())
        removePort(m_ports.constFirst()->socketPath);
    m_macTable.clear();
}

int VirtualSwitch::clientCount() const
{
    int count = 0;
    for (const GuestPort *port : m_ports) {
        if (port->socket)
            ++count;
    }
    return count;
}

void VirtualSwitch::addInternalPort(VirtualSwitchPort *port)
{
    if (!port || m_internalPorts.contains(port))
        return;
    m_internalPorts.append(port);
    connect(port, &VirtualSwitchPort::frameReady, this, [this, port](const QByteArray &frame) {
        forwardFrame(frame, nullptr, port);
    });
}

void VirtualSwitch::removeInternalPort(VirtualSwitchPort *port)
{
    if (!port)
        return;
    disconnect(port, nullptr, this, nullptr);
    m_internalPorts.removeAll(port);
}

VirtualSwitch::GuestPort *VirtualSwitch::findPort(const QString &socketPath) const
{
    for (GuestPort *port : m_ports) {
        if (port->socketPath == socketPath)
            return port;
    }
    return nullptr;
}

void VirtualSwitch::onNewConnection(GuestPort *port)
{
    while (QLocalSocket *socket = port->server->nextPendingConnection()) {
        if (port->socket) {
            qCWarning(KEMU) << "VirtualSwitch: rejecting second connection on" << port->socketPath;
            socket->abort();
            socket->deleteLater();
            continue;
        }

        port->socket = socket;
        port->readBuffer.clear();
        port->spoofLogged = false;
        port->congested = false;
        connect(socket, &QLocalSocket::readyRead, this, [this, port]() {
            onReadyRead(port);
        });
        connect(socket, &QLocalSocket::disconnected, this, [this, port]() {
            onSocketDisconnected(port);
        });
        qCDebug(KEMU) << "VirtualSwitch: client connected on" << port->socketPath << "-" << clientCount() << "total";
        Q_EMIT clientConnected();
    }
}

void VirtualSwitch::onReadyRead(GuestPort *port)
{
    if (!port->socket)
        return;

    QByteArray &buffer = port->readBuffer;
    buffer.append(port->socket->readAll());

    while (buffer.size() >= 4) {
        const quint32 length = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(buffer.constData()));
        if (length == 0 || length > kMaxFrameSize) {
            qCWarning(KEMU) << "VirtualSwitch: invalid frame length" << length << "- disconnecting client";
            port->socket->disconnectFromServer();
            return;
        }
        if (buffer.size() < 4 + static_cast<qsizetype>(length))
            return; // wait for the rest of the frame

        const QByteArray frame = buffer.mid(4, length);
        buffer.remove(0, 4 + length);
        forwardFrame(frame, port, nullptr);
    }
}

void VirtualSwitch::onSocketDisconnected(GuestPort *port)
{
    if (!port->socket)
        return;

    QLocalSocket *socket = port->socket;
    port->socket = nullptr;
    port->readBuffer.clear();

    if (port->mac.isEmpty()) {
        for (auto it = m_macTable.begin(); it != m_macTable.end();) {
            if (it.value() == port)
                it = m_macTable.erase(it);
            else
                ++it;
        }
    }

    socket->deleteLater();
    qCDebug(KEMU) << "VirtualSwitch: client disconnected from" << port->socketPath << "-" << clientCount() << "remaining";
    Q_EMIT clientDisconnected();
}

void VirtualSwitch::forwardFrame(const QByteArray &frame, GuestPort *fromGuest, VirtualSwitchPort *fromInternal)
{
    if (frame.size() < 14)
        return;

    const QByteArray srcMac = frame.mid(6, 6);
    const QByteArray dstMac = frame.left(6);

    if (fromGuest) {
        GuestPort *owner = m_macTable.value(srcMac);
        bool accepted;
        if (!fromGuest->mac.isEmpty())
            accepted = srcMac == fromGuest->mac;
        else
            accepted = !(srcMac.at(0) & 0x01) && (!owner || owner == fromGuest || owner->mac.isEmpty());

        if (!accepted) {
            if (!fromGuest->spoofLogged) {
                qCWarning(KEMU) << "VirtualSwitch: dropping frames with source MAC" << srcMac.toHex(':') << "on" << fromGuest->socketPath;
                fromGuest->spoofLogged = true;
            }
            return;
        }
        if (fromGuest->mac.isEmpty())
            m_macTable.insert(srcMac, fromGuest);
    }

    qCDebug(KEMU) << "VirtualSwitch: frame" << frame.size() << "bytes," << srcMac.toHex(':') << "->" << dstMac.toHex(':') << "ethertype"
                  << u"0x%1"_s.arg(qFromBigEndian<quint16>(reinterpret_cast<const uchar *>(frame.constData()) + 12), 4, 16, QLatin1Char('0'))
                  << (fromGuest ? "from guest" : "from internal port");

    const auto deliverToGuests = [this, &frame](GuestPort *exclude) {
        for (GuestPort *port : std::as_const(m_ports)) {
            if (port != exclude)
                writeFrame(port, frame);
        }
    };
    const auto deliverToInternal = [this, &frame](VirtualSwitchPort *exclude) {
        for (const auto &port : std::as_const(m_internalPorts)) {
            if (port && port != exclude)
                port->receiveFrame(frame);
        }
    };

    // Broadcast / multicast: flood to everyone except the origin.
    if (dstMac.at(0) & 0x01) {
        deliverToGuests(fromGuest);
        deliverToInternal(fromInternal);
        return;
    }

    // Internal ports are matched first, so a guest cannot shadow their MACs.
    for (const auto &port : std::as_const(m_internalPorts)) {
        if (port && port->ownsMac(dstMac)) {
            if (port != fromInternal)
                port->receiveFrame(frame);
            return;
        }
    }

    // Known unicast; frames for a bound but unconnected port are dropped.
    if (GuestPort *target = m_macTable.value(dstMac)) {
        if (target != fromGuest) // else hairpin
            writeFrame(target, frame);
        return;
    }

    // Unknown unicast: guests only. libslirp routes any frame it is handed,
    // so flooding it would leak guest-to-guest traffic to the host network.
    deliverToGuests(fromGuest);
}

void VirtualSwitch::writeFrame(GuestPort *port, const QByteArray &frame)
{
    if (!port->socket)
        return;
    if (port->socket->bytesToWrite() > kMaxPendingWriteBytes) {
        if (!port->congested) {
            qCWarning(KEMU) << "VirtualSwitch: client on" << port->socketPath << "is not reading, dropping frames";
            port->congested = true;
        }
        return;
    }
    port->congested = false;

    QByteArray out(4, Qt::Uninitialized);
    qToBigEndian<quint32>(static_cast<quint32>(frame.size()), reinterpret_cast<uchar *>(out.data()));
    out.append(frame);
    port->socket->write(out);
}
