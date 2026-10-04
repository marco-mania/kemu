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

VirtualSwitch::VirtualSwitch(QObject *parent)
    : QObject(parent)
{
}

VirtualSwitch::~VirtualSwitch()
{
    close();
}

bool VirtualSwitch::listen(const QString &socketPath)
{
    if (m_server)
        close();

    QFile::remove(socketPath);

    m_server = new QLocalServer(this);
    if (!m_server->listen(socketPath)) {
        qCWarning(KEMU) << "VirtualSwitch: failed to listen on" << socketPath << ":" << m_server->errorString();
        delete m_server;
        m_server = nullptr;
        return false;
    }

    m_socketPath = socketPath;
    connect(m_server, &QLocalServer::newConnection, this, &VirtualSwitch::onNewConnection);
    qCDebug(KEMU) << "VirtualSwitch: listening on" << socketPath;
    return true;
}

void VirtualSwitch::close()
{
    if (!m_server)
        return;

    const auto clients = m_clients;
    for (const auto &client : clients) {
        if (client)
            client->disconnectFromServer();
    }
    m_clients.clear();
    m_readBuffers.clear();
    m_macToClient.clear();
    m_macToInternal.clear();

    m_server->close();
    m_server->deleteLater();
    m_server = nullptr;

    if (!m_socketPath.isEmpty())
        QFile::remove(m_socketPath);
    m_socketPath.clear();
}

bool VirtualSwitch::isListening() const
{
    return m_server && m_server->isListening();
}

QString VirtualSwitch::socketPath() const
{
    return m_socketPath;
}

int VirtualSwitch::clientCount() const
{
    int count = 0;
    for (const auto &client : m_clients) {
        if (client)
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
        forwardFrame(port, frame);
    });
}

void VirtualSwitch::removeInternalPort(VirtualSwitchPort *port)
{
    if (!port)
        return;
    disconnect(port, nullptr, this, nullptr);
    m_internalPorts.removeAll(port);
    for (auto it = m_macToInternal.begin(); it != m_macToInternal.end();) {
        if (!it.value() || it.value() == port)
            it = m_macToInternal.erase(it);
        else
            ++it;
    }
}

void VirtualSwitch::onNewConnection()
{
    while (QLocalSocket *socket = m_server->nextPendingConnection()) {
        m_clients.append(socket);
        m_readBuffers.insert(socket, {});
        connect(socket, &QLocalSocket::readyRead, this, [this, socket]() {
            onReadyRead(socket);
        });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket]() {
            onSocketDisconnected(socket);
        });
        qCDebug(KEMU) << "VirtualSwitch: client connected," << clientCount() << "total";
        Q_EMIT clientConnected();
    }
}

void VirtualSwitch::onReadyRead(QLocalSocket *socket)
{
    QByteArray &buffer = m_readBuffers[socket];
    buffer.append(socket->readAll());

    while (buffer.size() >= 4) {
        const quint32 length = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(buffer.constData()));
        if (length == 0 || length > kMaxFrameSize) {
            qCWarning(KEMU) << "VirtualSwitch: invalid frame length" << length << "- disconnecting client";
            socket->disconnectFromServer();
            return;
        }
        if (buffer.size() < 4 + static_cast<qsizetype>(length))
            return; // wait for the rest of the frame

        const QByteArray frame = buffer.mid(4, length);
        buffer.remove(0, 4 + length);
        forwardFrame(socket, frame);
    }
}

void VirtualSwitch::onSocketDisconnected(QLocalSocket *socket)
{
    m_clients.removeAll(socket);
    m_readBuffers.remove(socket);
    for (auto it = m_macToClient.begin(); it != m_macToClient.end();) {
        if (it.value() == socket)
            it = m_macToClient.erase(it);
        else
            ++it;
    }
    socket->deleteLater();
    qCDebug(KEMU) << "VirtualSwitch: client disconnected," << clientCount() << "remaining";
    Q_EMIT clientDisconnected();
}

void VirtualSwitch::forwardFrame(QObject *source, const QByteArray &frame)
{
    if (frame.size() < 14)
        return;

    const QByteArray srcMac = frame.mid(6, 6);
    const QByteArray dstMac = frame.left(6);

    qCDebug(KEMU) << "VirtualSwitch: frame" << frame.size() << "bytes," << srcMac.toHex(':') << "->" << dstMac.toHex(':') << "ethertype"
                  << u"0x%1"_s.arg(qFromBigEndian<quint16>(reinterpret_cast<const uchar *>(frame.constData()) + 12), 4, 16, u'0')
                  << (qobject_cast<QLocalSocket *>(source) ? "from socket" : "from internal port");

    // Learn the sender.
    if (auto *socket = qobject_cast<QLocalSocket *>(source))
        m_macToClient.insert(srcMac, socket);
    else if (auto *port = qobject_cast<VirtualSwitchPort *>(source))
        m_macToInternal.insert(srcMac, port);

    const auto deliverToClients = [this, &frame](QLocalSocket *exclude) {
        for (const auto &client : m_clients) {
            if (client && client != exclude)
                writeFrame(client, frame);
        }
    };
    const auto deliverToInternal = [this, &frame](VirtualSwitchPort *exclude) {
        for (const auto &port : m_internalPorts) {
            if (port && port != exclude)
                port->receiveFrame(frame);
        }
    };

    auto *sourceSocket = qobject_cast<QLocalSocket *>(source);
    auto *sourcePort = qobject_cast<VirtualSwitchPort *>(source);

    // Broadcast / multicast: flood to everyone except the origin.
    if (dstMac.at(0) & 0x01) {
        deliverToClients(sourceSocket);
        deliverToInternal(sourcePort);
        return;
    }

    // Known unicast.
    if (m_macToClient.contains(dstMac)) {
        QLocalSocket *target = m_macToClient.value(dstMac);
        if (target && target != sourceSocket) {
            writeFrame(target, frame);
            return;
        }
        if (!target)
            m_macToClient.remove(dstMac); // stale entry, fall through to flood
        else
            return; // hairpin: frame already at its destination
    } else if (m_macToInternal.contains(dstMac)) {
        VirtualSwitchPort *target = m_macToInternal.value(dstMac);
        if (target && target != sourcePort)
            target->receiveFrame(frame);
        else if (!target)
            m_macToInternal.remove(dstMac);
        return;
    }

    // Unknown unicast: flood.
    deliverToClients(sourceSocket);
    deliverToInternal(sourcePort);
}

void VirtualSwitch::writeFrame(QLocalSocket *socket, const QByteArray &frame)
{
    QByteArray out(4, Qt::Uninitialized);
    qToBigEndian<quint32>(static_cast<quint32>(frame.size()), reinterpret_cast<uchar *>(out.data()));
    out.append(frame);
    socket->write(out);
}
