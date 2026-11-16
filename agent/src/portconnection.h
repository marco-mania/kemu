/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QString>
#include <QTimer>

#include "transport.h"

class QSocketNotifier;

// The guest end of the virtio-serial port (section 3.1).
//
// Linux reports a missing host end as POLLHUP, and keeps reporting it while
// the host stays away. The read notifier would fire without end then, so
// the connection checks for the host on a timer until it is back.
class PortConnection : public Transport
{
    Q_OBJECT

public:
    enum class OpenResult {
        Ok,
        Missing, // no such device: not a KEMU VM, or the port is not there yet
        Busy, // another process holds the port
        Denied, // no ACL for us (yet)
        Failed,
    };

    explicit PortConnection(const QString &path, QObject *parent = nullptr);
    ~PortConnection() override;

    QString path() const
    {
        return m_path;
    }

    OpenResult open();
    void close();
    bool isOpen() const
    {
        return m_fd >= 0;
    }

    bool isConnected() const override
    {
        return m_hostConnected;
    }
    void write(const QByteArray &data) override;
    qsizetype pendingBytes() const override
    {
        return m_output.size() - m_outputPos;
    }

    // How often to look for a host while it is away.
    void setProbeInterval(int msecs)
    {
        m_probe.setInterval(msecs);
    }

private:
    void onReadable();
    void onWritable();
    void flush();
    void probeHost();
    void hostGone();
    bool hostAway() const;

    QString m_path;
    int m_fd = -1;
    bool m_hostConnected = false;
    QSocketNotifier *m_readNotifier = nullptr;
    QSocketNotifier *m_writeNotifier = nullptr;
    QTimer m_probe;
    QByteArray m_output;
    qsizetype m_outputPos = 0;
};
