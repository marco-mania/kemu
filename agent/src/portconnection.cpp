/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "portconnection.h"

#include <QFile>
#include <QSocketNotifier>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include "logging.h"

using namespace Qt::StringLiterals;

namespace
{
constexpr qsizetype kReadChunk = 64 * 1024;
constexpr qsizetype kReadPerWakeup = 1024 * 1024;
constexpr qsizetype kWriteChunk = 256 * 1024;
} // namespace

PortConnection::PortConnection(const QString &path, QObject *parent)
    : Transport(parent)
    , m_path(path)
{
    m_probe.setInterval(500);
    connect(&m_probe, &QTimer::timeout, this, &PortConnection::probeHost);
}

PortConnection::~PortConnection()
{
    // No signals from a dying object.
    blockSignals(true);
    close();
}

PortConnection::OpenResult PortConnection::open()
{
    if (m_fd >= 0)
        return OpenResult::Ok;

    const QByteArray native = QFile::encodeName(m_path);
    const int fd = ::open(native.constData(), O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) {
        const int error = errno;
        switch (error) {
        case ENOENT:
        case ENODEV:
        case ENXIO:
            return OpenResult::Missing;
        case EBUSY:
            return OpenResult::Busy;
        case EACCES:
        case EPERM:
            return OpenResult::Denied;
        default:
            qCWarning(KEMU_AGENT) << "Port: cannot open" << m_path << std::strerror(error);
            return OpenResult::Failed;
        }
    }

    // A port that is a tty (FreeBSD's virtio console, a pty in the tests)
    // must not touch the bytes.
    if (::isatty(fd)) {
        termios attributes;
        if (::tcgetattr(fd, &attributes) == 0) {
            ::cfmakeraw(&attributes);
            ::tcsetattr(fd, TCSANOW, &attributes);
        }
    }

    m_fd = fd;
    m_readNotifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    m_readNotifier->setEnabled(false);
    connect(m_readNotifier, &QSocketNotifier::activated, this, &PortConnection::onReadable);
    m_writeNotifier = new QSocketNotifier(fd, QSocketNotifier::Write, this);
    m_writeNotifier->setEnabled(false);
    connect(m_writeNotifier, &QSocketNotifier::activated, this, &PortConnection::onWritable);

    qCDebug(KEMU_AGENT) << "Port: opened" << m_path;
    probeHost();
    return OpenResult::Ok;
}

void PortConnection::close()
{
    if (m_fd < 0)
        return;
    m_probe.stop();
    delete m_readNotifier;
    m_readNotifier = nullptr;
    delete m_writeNotifier;
    m_writeNotifier = nullptr;
    ::close(m_fd);
    m_fd = -1;
    m_output.clear();
    m_outputPos = 0;
    qCDebug(KEMU_AGENT) << "Port: closed";

    const bool wasConnected = m_hostConnected;
    m_hostConnected = false;
    if (wasConnected)
        Q_EMIT disconnected();
}

void PortConnection::write(const QByteArray &data)
{
    if (!m_hostConnected || data.isEmpty())
        return;
    m_output += data;
    if (!m_writeNotifier->isEnabled())
        flush();
}

void PortConnection::flush()
{
    while (m_outputPos < m_output.size()) {
        const qsizetype size = std::min(m_output.size() - m_outputPos, kWriteChunk);
        const ssize_t written = ::write(m_fd, m_output.constData() + m_outputPos, size_t(size));
        if (written > 0) {
            m_outputPos += written;
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        if (written < 0 && errno == EAGAIN) {
            if (m_outputPos > 1024 * 1024) {
                m_output.remove(0, m_outputPos);
                m_outputPos = 0;
            }
            m_writeNotifier->setEnabled(true);
            return;
        }
        qCDebug(KEMU_AGENT) << "Port: write failed," << std::strerror(errno);
        hostGone();
        return;
    }
    m_output.clear();
    m_outputPos = 0;
    m_writeNotifier->setEnabled(false);
}

void PortConnection::onWritable()
{
    flush();
    // Only from here: a drained() inside write() would re-enter the writer.
    if (m_hostConnected && pendingBytes() == 0)
        Q_EMIT drained();
}

void PortConnection::onReadable()
{
    QByteArray data;
    bool gone = false;
    char buffer[kReadChunk];
    while (data.size() < kReadPerWakeup) {
        const ssize_t count = ::read(m_fd, buffer, sizeof(buffer));
        if (count > 0) {
            data.append(buffer, count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && errno == EAGAIN)
            break;
        // 0: no data and no host (virtio). EIO: the other end of a pty.
        gone = true;
        break;
    }
    if (!data.isEmpty())
        Q_EMIT received(data);
    if (gone && m_fd >= 0)
        hostGone();
}

bool PortConnection::hostAway() const
{
    pollfd entry{m_fd, POLLIN, 0};
    if (::poll(&entry, 1, 0) < 0)
        return false;
    return entry.revents & (POLLHUP | POLLERR);
}

void PortConnection::probeHost()
{
    if (m_fd < 0)
        return;
    if (hostAway()) {
        if (!m_probe.isActive()) {
            qCDebug(KEMU_AGENT) << "Port: waiting for the host";
            m_probe.start();
        }
        return;
    }
    m_probe.stop();
    if (m_hostConnected)
        return;
    m_hostConnected = true;
    m_readNotifier->setEnabled(true);
    qCDebug(KEMU_AGENT) << "Port: host connected";
    Q_EMIT connected();
}

void PortConnection::hostGone()
{
    if (!m_hostConnected)
        return;
    m_hostConnected = false;
    m_output.clear();
    m_outputPos = 0;
    m_readNotifier->setEnabled(false);
    m_writeNotifier->setEnabled(false);
    qCDebug(KEMU_AGENT) << "Port: host gone";
    m_probe.start();
    Q_EMIT disconnected();
}
