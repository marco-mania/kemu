/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QByteArray>
#include <QObject>

// A byte stream to the host. The port is one; the tests use a fake.
class Transport : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    // The port is open and the host end is connected.
    virtual bool isConnected() const = 0;
    // Queues bytes; they are dropped while not connected.
    virtual void write(const QByteArray &data) = 0;
    // Bytes queued but not yet taken by the port.
    virtual qsizetype pendingBytes() const = 0;

Q_SIGNALS:
    void connected();
    void disconnected();
    void received(const QByteArray &data);
    // All queued bytes went out after a write had to wait. Never emitted
    // from inside write().
    void drained();
};
