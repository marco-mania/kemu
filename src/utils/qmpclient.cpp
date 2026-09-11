/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "qmpclient.h"

#include "kemu_debug.h"

QMPClient::QMPClient(QObject *parent)
    : QObject(parent)
{
    connect(&m_socket, &QLocalSocket::readyRead, this, &QMPClient::onReadyRead);
    connect(&m_socket, &QLocalSocket::errorOccurred, this, &QMPClient::onSocketError);
    connect(&m_socket, &QLocalSocket::disconnected, this, [this] {
        failPendingRequests(i18n("The connection to the QEMU monitor was lost."));
    });
    connect(&m_socket, &QLocalSocket::disconnected, this, &QMPClient::disconnected);

    m_screenshotTimer.setTimerType(Qt::CoarseTimer);
    connect(&m_screenshotTimer, &QTimer::timeout, this, &QMPClient::performScreenshot);

    m_cpuSampleClock.start();
    m_blockSampleClock.start();

    m_perfTimer.setTimerType(Qt::CoarseTimer);
    connect(&m_perfTimer, &QTimer::timeout, this, &QMPClient::performAllSamples);
}

QMPClient::~QMPClient()
{
    m_perfTimer.stop();
    m_screenshotTimer.stop();
    disconnectFromVm();
}

void QMPClient::connectToVm(const QString &socketPath)
{
    if (m_socket.state() != QLocalSocket::UnconnectedState) {
        m_socket.abort();
    }
    m_handshakeComplete = false;
    failPendingRequests(i18n("The connection to the QEMU monitor was reset."));
    m_pendingRequests.clear();
    m_socket.connectToServer(socketPath);
}

void QMPClient::disconnectFromVm()
{
    m_perfTimer.stop();
    m_screenshotTimer.stop();
    m_lastCpuSamples.clear();
    m_lastBlockSamples.clear();
    m_maxMemoryBytes = 0;
    m_guestStatsEnabled = false;
    m_balloonPath.clear();
    if (m_socket.isOpen()) {
        m_socket.disconnectFromServer();
    }
    failPendingRequests(i18n("The connection to the QEMU monitor was closed."));
}

bool QMPClient::isConnected() const
{
    return m_socket.state() == QLocalSocket::ConnectedState && m_handshakeComplete;
}

void QMPClient::sendCommand(const QString &command, const QJsonObject &args, std::function<void(QMPResult)> callback)
{
    if (!m_socket.isOpen()) {
        if (callback)
            callback({false, u"Socket is not connected."_s, {}, QJsonArray()});
        return;
    }

    const QString cmdId = u"cmd_%1"_s.arg(++m_commandCounter);

    QJsonObject cmdObj;
    cmdObj.insert(u"execute"_s, command);
    cmdObj.insert(u"id"_s, cmdId);
    if (!args.isEmpty()) {
        cmdObj.insert(u"arguments"_s, args);
    }

    if (callback) {
        m_pendingRequests.insert(cmdId, callback);
    }

    m_socket.write(QJsonDocument(cmdObj).toJson(QJsonDocument::Compact) + "\n");
    m_socket.flush();
}

QMPResult QMPClient::sendCommandSync(const QString &command, const QJsonObject &args, int timeoutMs)
{
    QMPResult finalResult;
    QEventLoop loop;
    QTimer timeoutTimer;

    timeoutTimer.setSingleShot(true);
    connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);

    // The callback outlives this frame if the command times out, so it must
    // not touch the stack objects afterwards.
    auto waiting = std::make_shared<bool>(true);

    sendCommand(command, args, [waiting, &finalResult, &loop](QMPResult result) {
        if (!*waiting)
            return;
        finalResult = result;
        loop.quit();
    });

    timeoutTimer.start(timeoutMs);
    loop.exec();

    *waiting = false;

    if (timeoutTimer.isActive()) {
        timeoutTimer.stop();
    } else {
        finalResult.success = false;
        finalResult.errorMessage = i18n("Command '%1' timed out after %2 ms.", command, timeoutMs);
    }

    return finalResult;
}

void QMPClient::sendFileDescriptor(const QString &fdname, int fd, std::function<void(QMPResult)> callback)
{
    if (!m_socket.isOpen() || fd < 0) {
        ::close(fd);
        if (callback)
            callback({false, u"Socket is not connected."_s, {}, QJsonArray()});
        return;
    }

    // The fd must ride on the same message as the getfd command, so this
    // bypasses QLocalSocket's buffer. Drain it first to keep byte order.
    m_socket.flush();
    while (m_socket.bytesToWrite() > 0)
        m_socket.waitForBytesWritten(1000);

    const QString cmdId = u"cmd_%1"_s.arg(++m_commandCounter);
    QJsonObject cmdObj;
    cmdObj.insert(u"execute"_s, u"getfd"_s);
    cmdObj.insert(u"id"_s, cmdId);
    cmdObj.insert(u"arguments"_s, QJsonObject{{u"fdname"_s, fdname}});
    const QByteArray payload = QJsonDocument(cmdObj).toJson(QJsonDocument::Compact) + "\n";

    struct iovec iov;
    iov.iov_base = const_cast<char *>(payload.constData());
    iov.iov_len = static_cast<size_t>(payload.size());

    char cmsgBuf[CMSG_SPACE(sizeof(int))] = {};
    struct msghdr msg = {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgBuf;
    msg.msg_controllen = sizeof(cmsgBuf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

    const ssize_t sent = ::sendmsg(static_cast<int>(m_socket.socketDescriptor()), &msg, MSG_NOSIGNAL);
    ::close(fd); // kernel holds its own reference while the message is in flight

    if (sent < 0) {
        if (callback)
            callback({false, u"sendmsg failed: %1"_s.arg(QString::fromLocal8Bit(strerror(errno))), {}, QJsonArray()});
        return;
    }
    if (callback)
        m_pendingRequests.insert(cmdId, callback);
}

// ---------------------------------------------------------------------------
// Screenshot
// ---------------------------------------------------------------------------

void QMPClient::startScreenshotTimer(const QString &dumpPath, int intervalMs)
{
    m_screenshotDumpPath = dumpPath;
    m_screenshotInFlight = false;

    m_screenshotTimer.start(intervalMs);
    performScreenshot();
}

void QMPClient::stopScreenshotTimer()
{
    m_screenshotTimer.stop();
    m_screenshotInFlight = false;
}

bool QMPClient::isScreenshotTimerActive() const
{
    return m_screenshotTimer.isActive();
}

void QMPClient::requestScreenshot(const QString &dumpPath)
{
    if (!isConnected() || m_screenshotInFlight)
        return;

    m_screenshotInFlight = true;

    QJsonObject args;
    args.insert(u"filename"_s, dumpPath);

    sendCommand(u"screendump"_s, args, [this, dumpPath](QMPResult result) {
        m_screenshotInFlight = false;

        if (!result.success) {
            qCDebug(KEMU) << "screendump failed:" << result.errorMessage;
            return;
        }

        const QImage image(dumpPath);
        if (image.isNull()) {
            qCDebug(KEMU) << "Failed to load screendump image from" << dumpPath;
            return;
        }

        Q_EMIT screenshotCaptured(image);
    });
}

void QMPClient::performScreenshot()
{
    if (!isConnected())
        return;
    requestScreenshot(m_screenshotDumpPath);
}

// ---------------------------------------------------------------------------
// CPU usage
// ---------------------------------------------------------------------------

QMPClient::ProcCpuSample QMPClient::readProcStat(qint64 tid)
{
    ProcCpuSample sample;

    QFile f(u"/proc/%1/stat"_s.arg(tid));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return sample;

    const QByteArray data = f.readAll();
    f.close();

    const int closeParenPos = data.lastIndexOf(')');
    if (closeParenPos < 0)
        return sample;

    const QList<QByteArray> fields = QByteArray::fromRawData(data.constData() + closeParenPos + 2, data.size() - closeParenPos - 2).split(' ');

    if (fields.size() > 12) {
        sample.utime = fields.at(11).toLongLong();
        sample.stime = fields.at(12).toLongLong();
    }

    return sample;
}

void QMPClient::requestCpuUsage(std::function<void(VmCpuUsage)> callback)
{
    if (!isConnected()) {
        if (callback)
            callback({});
        return;
    }

    sendCommand(u"query-cpus-fast"_s, {}, [this, callback](QMPResult result) {
        VmCpuUsage usage;

        if (!result.success) {
            qCDebug(KEMU) << "query-cpus-fast failed:" << result.errorMessage;
            if (callback)
                callback(usage);
            return;
        }

        const QJsonArray cpuArray = result.dataArray;
        if (cpuArray.isEmpty()) {
            if (callback)
                callback(usage);
            return;
        }

        const qint64 nowNs = m_cpuSampleClock.nsecsElapsed();
        const long ticksPerSec = sysconf(_SC_CLK_TCK);

        usage.isValid = true;
        double totalPercent = 0.0;

        for (const QJsonValue &val : cpuArray) {
            const QJsonObject cpu = val.toObject();
            const int cpuIndex = cpu.value(u"cpu-index"_s).toInt();
            const qint64 tid = cpu.value(u"thread-id"_s).toVariant().toLongLong();

            ProcCpuSample current = readProcStat(tid);
            current.wallNs = nowNs;

            VCpuUsageInfo info;
            info.cpuIndex = cpuIndex;
            info.threadId = tid;

            if (m_lastCpuSamples.contains(tid)) {
                const ProcCpuSample &prev = m_lastCpuSamples.value(tid);
                const qint64 dticks = (current.utime + current.stime) - (prev.utime + prev.stime);
                const double dsecs = static_cast<double>(current.wallNs - prev.wallNs) / 1.0e9;

                if (dsecs > 0.0 && ticksPerSec > 0) {
                    const double cpuSecs = static_cast<double>(dticks) / ticksPerSec;
                    info.usagePercent = qBound(0.0, (cpuSecs / dsecs) * 100.0, 100.0);
                }
            }

            m_lastCpuSamples.insert(tid, current);
            usage.vcpus.append(info);
            totalPercent += info.usagePercent;
        }

        if (!usage.vcpus.isEmpty())
            usage.averagePercent = totalPercent / usage.vcpus.size();

        if (callback)
            callback(usage);
    });
}

VmCpuUsage QMPClient::getCpuUsageSync(int timeoutMs)
{
    VmCpuUsage finalResult;
    QEventLoop loop;
    QTimer timeoutTimer;

    timeoutTimer.setSingleShot(true);
    connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);

    requestCpuUsage([&](VmCpuUsage result) {
        finalResult = result;
        loop.quit();
    });

    timeoutTimer.start(timeoutMs);
    loop.exec();

    if (!timeoutTimer.isActive()) {
        finalResult.isValid = false;
    } else {
        timeoutTimer.stop();
    }

    return finalResult;
}

void QMPClient::startCpuUsageTimer()
{
    m_cpuUsageInFlight = false;
    m_lastCpuSamples.clear();
    m_cpuSampleClock.restart();
    performCpuUsageSample();
}

void QMPClient::stopCpuUsageTimer()
{
    m_cpuUsageInFlight = false;
    m_lastCpuSamples.clear();
}

bool QMPClient::isCpuUsageTimerActive() const
{
    return m_perfTimer.isActive();
}

void QMPClient::performCpuUsageSample()
{
    if (!isConnected() || m_cpuUsageInFlight)
        return;

    m_cpuUsageInFlight = true;

    sendCommand(u"query-cpus-fast"_s, {}, [this](QMPResult result) {
        m_cpuUsageInFlight = false;

        if (!result.success) {
            qCDebug(KEMU) << "query-cpus-fast failed:" << result.errorMessage;
            return;
        }

        const QJsonArray cpuArray = result.dataArray;
        if (cpuArray.isEmpty())
            return;

        const qint64 nowNs = m_cpuSampleClock.nsecsElapsed();
        const long ticksPerSec = sysconf(_SC_CLK_TCK);

        VmCpuUsage usage;
        usage.isValid = true;
        double totalPercent = 0.0;

        for (const QJsonValue &val : cpuArray) {
            const QJsonObject cpu = val.toObject();
            const int cpuIndex = cpu.value(u"cpu-index"_s).toInt();
            const qint64 tid = cpu.value(u"thread-id"_s).toVariant().toLongLong();

            ProcCpuSample current = readProcStat(tid);
            current.wallNs = nowNs;

            VCpuUsageInfo info;
            info.cpuIndex = cpuIndex;
            info.threadId = tid;

            if (m_lastCpuSamples.contains(tid)) {
                const ProcCpuSample &prev = m_lastCpuSamples.value(tid);
                const qint64 dticks = (current.utime + current.stime) - (prev.utime + prev.stime);
                const double dsecs = static_cast<double>(current.wallNs - prev.wallNs) / 1.0e9;

                if (dsecs > 0.0 && ticksPerSec > 0) {
                    const double cpuSecs = static_cast<double>(dticks) / ticksPerSec;
                    info.usagePercent = qBound(0.0, (cpuSecs / dsecs) * 100.0, 100.0);
                }
            }

            m_lastCpuSamples.insert(tid, current);
            usage.vcpus.append(info);
            totalPercent += info.usagePercent;
        }

        if (!usage.vcpus.isEmpty())
            usage.averagePercent = totalPercent / usage.vcpus.size();

        Q_EMIT cpuUsageUpdated(usage);
    });
}

// ---------------------------------------------------------------------------
// RAM usage (query-balloon)
// ---------------------------------------------------------------------------

void QMPClient::requestMemoryUsage(std::function<void(VmMemoryUsage)> callback)
{
    if (!isConnected()) {
        if (callback)
            callback({});
        return;
    }

    sendCommand(u"query-balloon"_s, {}, [this, callback](QMPResult result) {
        VmMemoryUsage usage;

        if (result.success) {
            usage.actualBytes = result.data.value(u"actual"_s).toVariant().toLongLong();

            if (m_maxMemoryBytes == 0 && usage.actualBytes > 0)
                m_maxMemoryBytes = usage.actualBytes;

            usage.maxBytes = m_maxMemoryBytes;

            // Only valid if balloon is actively deflated
            if (usage.maxBytes > 0 && usage.actualBytes < usage.maxBytes) {
                usage.usagePercent = static_cast<double>(usage.actualBytes) / usage.maxBytes * 100.0;
                usage.isValid = true;
            }
        }

        if (callback)
            callback(usage);
    });
}

VmMemoryUsage QMPClient::getMemoryUsageSync(int timeoutMs)
{
    VmMemoryUsage finalResult;
    QEventLoop loop;
    QTimer timeoutTimer;

    timeoutTimer.setSingleShot(true);
    connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);

    requestMemoryUsage([&](VmMemoryUsage result) {
        finalResult = result;
        loop.quit();
    });

    timeoutTimer.start(timeoutMs);
    loop.exec();

    if (!timeoutTimer.isActive()) {
        finalResult.isValid = false;
    } else {
        timeoutTimer.stop();
    }

    return finalResult;
}

void QMPClient::performMemorySample()
{
    if (!isConnected())
        return;

    if (m_balloonPath.isEmpty()) {
        m_memorySampleState = MemorySampleState::Discovering;
        startBalloonDiscovery();
    } else if (!m_guestStatsEnabled) {
        m_memorySampleState = MemorySampleState::Enabling;
        enableGuestStats();
    } else {
        m_memorySampleState = MemorySampleState::Reading;
        readGuestStats();
    }
}

void QMPClient::startBalloonDiscovery()
{
    m_memorySampleState = MemorySampleState::Discovering;

    sendCommand(u"qom-list"_s, QJsonObject{{u"path"_s, u"/machine/peripheral-anon"_s}}, [this](QMPResult result) {
        if (result.success) {
            for (const auto &val : result.dataArray) {
                const QJsonObject obj = val.toObject();
                const QString type = obj.value(u"type"_s).toString();
                if (type.contains(u"balloon"_s, Qt::CaseInsensitive)) {
                    m_balloonPath = u"/machine/peripheral-anon/%1"_s.arg(obj.value(u"name"_s).toString());
                    break;
                }
            }
        }

        if (m_balloonPath.isEmpty()) {
            sendCommand(u"qom-list"_s, QJsonObject{{u"path"_s, u"/machine/peripheral"_s}}, [this](QMPResult r2) {
                if (r2.success) {
                    for (const auto &val : r2.dataArray) {
                        const QJsonObject obj = val.toObject();
                        const QString type = obj.value(u"type"_s).toString();
                        if (type.contains(u"balloon"_s, Qt::CaseInsensitive)) {
                            m_balloonPath = u"/machine/peripheral/%1"_s.arg(obj.value(u"name"_s).toString());
                            break;
                        }
                    }
                }
                m_memorySampleState = MemorySampleState::Idle;
                startBalloonDiscovery();
            });
            return;
        }

        m_memorySampleState = MemorySampleState::Idle;
        performMemorySample();
    });
}

void QMPClient::enableGuestStats()
{
    m_memorySampleState = MemorySampleState::Enabling;

    QJsonObject args;
    args[u"path"_s] = m_balloonPath;
    args[u"property"_s] = u"guest-stats-polling-interval"_s;
    args[u"value"_s] = 2;

    sendCommand(u"qom-set"_s, args, [this](QMPResult result) {
        if (!result.success) {
            qCDebug(KEMU) << "Failed to enable guest stats polling:" << result.errorMessage;
            m_guestStatsEnabled = false;
            m_memorySampleState = MemorySampleState::Idle;
        } else {
            m_guestStatsEnabled = true;
            m_memorySampleState = MemorySampleState::Idle;
            performMemorySample();
        }
    });
}

void QMPClient::readGuestStats()
{
    m_memorySampleState = MemorySampleState::Reading;

    QJsonObject args;
    args[u"path"_s] = m_balloonPath;
    args[u"property"_s] = u"guest-stats"_s;

    sendCommand(u"qom-get"_s, args, [this](QMPResult result) {
        m_memorySampleState = MemorySampleState::Idle;

        if (result.success) {
            const QJsonObject stats = result.data.value(u"stats"_s).toObject();
            const qint64 totalMem = stats.value(u"stat-total-memory"_s).toInteger();
            const qint64 availableMem = stats.value(u"stat-available-memory"_s).toInteger();
            const qint64 freeMem = stats.value(u"stat-free-memory"_s).toInteger();

            if (totalMem > 0) {
                const qint64 effectiveFree = (availableMem > 0) ? availableMem : freeMem;
                const qint64 usedMem = totalMem - effectiveFree;

                VmMemoryUsage usage;
                usage.maxBytes = totalMem;
                usage.actualBytes = usedMem;
                usage.usagePercent = static_cast<double>(usedMem) / totalMem * 100.0;
                usage.isValid = true;
                Q_EMIT memoryUsageUpdated(usage);
                return;
            }
        }

        sendCommand(u"query-balloon"_s, {}, [this](QMPResult fbResult) {
            VmMemoryUsage usage;
            if (fbResult.success) {
                const qint64 actual = fbResult.data.value(u"actual"_s).toVariant().toLongLong();
                if (m_maxMemoryBytes == 0 && actual > 0)
                    m_maxMemoryBytes = actual;

                if (m_maxMemoryBytes > 0 && actual < m_maxMemoryBytes) {
                    usage.actualBytes = actual;
                    usage.maxBytes = m_maxMemoryBytes;
                    usage.usagePercent = static_cast<double>(actual) / m_maxMemoryBytes * 100.0;
                    usage.isValid = true;
                }
            }
            Q_EMIT memoryUsageUpdated(usage);
        });
    });
}

// ---------------------------------------------------------------------------
// Block I/O stats (query-blockstats)
// ---------------------------------------------------------------------------

void QMPClient::requestBlockStats(std::function<void(VmBlockStats)> callback)
{
    if (!isConnected()) {
        if (callback)
            callback({});
        return;
    }

    sendCommand(u"query-blockstats"_s, {}, [this, callback](QMPResult result) {
        VmBlockStats stats;

        if (!result.success) {
            if (callback)
                callback(stats);
            return;
        }

        const QJsonArray devices = result.dataArray;
        if (devices.isEmpty()) {
            if (callback)
                callback(stats);
            return;
        }

        const qint64 nowNs = m_blockSampleClock.nsecsElapsed();
        stats.isValid = true;

        for (const QJsonValue &val : devices) {
            const QJsonObject entry = val.toObject();
            const QString device = entry.value(u"device"_s).toString();
            const QString node = entry.value(u"node-name"_s).toString();
            const QJsonObject s = entry.value(u"stats"_s).toObject();

            if (s.isEmpty())
                continue;

            // Use node-name as key; fall back to device name
            const QString key = node.isEmpty() ? device : node;
            if (key.isEmpty())
                continue;

            BlockDeviceStats ds;
            ds.device = device;
            ds.node = node;
            ds.rdBytes = s.value(u"rd_bytes"_s).toVariant().toLongLong();
            ds.wrBytes = s.value(u"wr_bytes"_s).toVariant().toLongLong();
            ds.rdOps = s.value(u"rd_operations"_s).toVariant().toLongLong();
            ds.wrOps = s.value(u"wr_operations"_s).toVariant().toLongLong();

            if (m_lastBlockSamples.contains(key)) {
                const BlockIoSnapshot &prev = m_lastBlockSamples.value(key);
                const double dsecs = static_cast<double>(nowNs - prev.wallNs) / 1.0e9;

                if (dsecs > 0.0) {
                    ds.rdBytesPerSec = qMax(0.0, static_cast<double>(ds.rdBytes - prev.rdBytes) / dsecs);
                    ds.wrBytesPerSec = qMax(0.0, static_cast<double>(ds.wrBytes - prev.wrBytes) / dsecs);
                    ds.rdOpsPerSec = qMax(0.0, static_cast<double>(ds.rdOps - prev.rdOps) / dsecs);
                    ds.wrOpsPerSec = qMax(0.0, static_cast<double>(ds.wrOps - prev.wrOps) / dsecs);
                }
            }

            m_lastBlockSamples.insert(key, {ds.rdBytes, ds.wrBytes, ds.rdOps, ds.wrOps, nowNs});

            stats.devices.append(ds);
            stats.totalRdBytesPerSec += ds.rdBytesPerSec;
            stats.totalWrBytesPerSec += ds.wrBytesPerSec;
            stats.totalRdOpsPerSec += ds.rdOpsPerSec;
            stats.totalWrOpsPerSec += ds.wrOpsPerSec;
        }

        if (callback)
            callback(stats);
    });
}

VmBlockStats QMPClient::getBlockStatsSync(int timeoutMs)
{
    VmBlockStats finalResult;
    QEventLoop loop;
    QTimer timeoutTimer;

    timeoutTimer.setSingleShot(true);
    connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);

    requestBlockStats([&](VmBlockStats result) {
        finalResult = result;
        loop.quit();
    });

    timeoutTimer.start(timeoutMs);
    loop.exec();

    if (!timeoutTimer.isActive()) {
        finalResult.isValid = false;
    } else {
        timeoutTimer.stop();
    }

    return finalResult;
}

void QMPClient::performBlockStatsSample()
{
    if (!isConnected() || m_blockStatsInFlight)
        return;

    m_blockStatsInFlight = true;

    sendCommand(u"query-blockstats"_s, {}, [this](QMPResult result) {
        m_blockStatsInFlight = false;

        if (!result.success)
            return;

        const QJsonArray devices = result.dataArray;
        if (devices.isEmpty())
            return;

        const qint64 nowNs = m_blockSampleClock.nsecsElapsed();

        VmBlockStats stats;
        stats.isValid = true;

        for (const QJsonValue &val : devices) {
            const QJsonObject entry = val.toObject();
            const QString device = entry.value(u"device"_s).toString();
            const QString node = entry.value(u"node-name"_s).toString();
            const QJsonObject s = entry.value(u"stats"_s).toObject();

            if (s.isEmpty())
                continue;

            const QString key = node.isEmpty() ? device : node;
            if (key.isEmpty())
                continue;

            BlockDeviceStats ds;
            ds.device = device;
            ds.node = node;
            ds.rdBytes = s.value(u"rd_bytes"_s).toVariant().toLongLong();
            ds.wrBytes = s.value(u"wr_bytes"_s).toVariant().toLongLong();
            ds.rdOps = s.value(u"rd_operations"_s).toVariant().toLongLong();
            ds.wrOps = s.value(u"wr_operations"_s).toVariant().toLongLong();

            if (m_lastBlockSamples.contains(key)) {
                const BlockIoSnapshot &prev = m_lastBlockSamples.value(key);
                const double dsecs = static_cast<double>(nowNs - prev.wallNs) / 1.0e9;

                if (dsecs > 0.0) {
                    ds.rdBytesPerSec = qMax(0.0, static_cast<double>(ds.rdBytes - prev.rdBytes) / dsecs);
                    ds.wrBytesPerSec = qMax(0.0, static_cast<double>(ds.wrBytes - prev.wrBytes) / dsecs);
                    ds.rdOpsPerSec = qMax(0.0, static_cast<double>(ds.rdOps - prev.rdOps) / dsecs);
                    ds.wrOpsPerSec = qMax(0.0, static_cast<double>(ds.wrOps - prev.wrOps) / dsecs);
                }
            }

            m_lastBlockSamples.insert(key, {ds.rdBytes, ds.wrBytes, ds.rdOps, ds.wrOps, nowNs});

            stats.devices.append(ds);
            stats.totalRdBytesPerSec += ds.rdBytesPerSec;
            stats.totalWrBytesPerSec += ds.wrBytesPerSec;
            stats.totalRdOpsPerSec += ds.rdOpsPerSec;
            stats.totalWrOpsPerSec += ds.wrOpsPerSec;
        }

        Q_EMIT blockStatsUpdated(stats);
    });
}

// ---------------------------------------------------------------------------
// Combined performance timer
// ---------------------------------------------------------------------------

void QMPClient::startPerformanceTimer(int intervalMs)
{
    m_cpuUsageInFlight = false;
    // m_memoryInFlight = false;
    m_blockStatsInFlight = false;
    m_lastCpuSamples.clear();
    m_lastBlockSamples.clear();
    m_maxMemoryBytes = 0;
    m_guestStatsEnabled = false;

    m_balloonPath.clear();
    m_memorySampleState = MemorySampleState::Idle;
    m_cpuSampleClock.restart();
    m_blockSampleClock.restart();

    m_perfTimer.start(intervalMs);
    performAllSamples();
}

void QMPClient::stopPerformanceTimer()
{
    m_perfTimer.stop();
    m_cpuUsageInFlight = false;
    // m_memoryInFlight = false;
    m_blockStatsInFlight = false;
    m_lastCpuSamples.clear();
    m_lastBlockSamples.clear();
    m_memorySampleState = MemorySampleState::Idle;
}

bool QMPClient::isPerformanceTimerActive() const
{
    return m_perfTimer.isActive();
}

void QMPClient::performAllSamples()
{
    if (!isConnected())
        return;

    performCpuUsageSample();
    performMemorySample();
    performBlockStatsSample();
}

// ---------------------------------------------------------------------------
// USB
// ---------------------------------------------------------------------------

QMPResult QMPClient::attachUsbDevice(int busNum, int devAddr, const QString &id)
{
    QJsonObject args;
    args[u"driver"_s] = u"usb-host"_s;
    args[u"hostbus"_s] = busNum;
    args[u"hostaddr"_s] = devAddr;
    args[u"id"_s] = id;
    return sendCommandSync(u"device_add"_s, args, 5000);
}

QMPResult QMPClient::detachUsbDevice(const QString &id)
{
    QJsonObject args;
    args[u"id"_s] = id;
    return sendCommandSync(u"device_del"_s, args, 5000);
}

// ---------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------

void QMPClient::setNetworkLink(const QString &nicId, bool up, std::function<void(QMPResult)> callback)
{
    QJsonObject args;
    args[u"name"_s] = nicId;
    args[u"up"_s] = up;
    sendCommand(u"set_link"_s, args, std::move(callback));
}

// ---------------------------------------------------------------------------
// Socket I/O
// ---------------------------------------------------------------------------

void QMPClient::onReadyRead()
{
    m_buffer.append(m_socket.readAll());

    while (true) {
        const int newlinePos = m_buffer.indexOf('\n');
        if (newlinePos < 0)
            break;

        const QByteArray line = m_buffer.left(newlinePos);
        m_buffer.remove(0, newlinePos + 1);
        processLine(line);
    }
}

void QMPClient::processLine(const QByteArray &line)
{
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &error);

    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return;
    }

    const QJsonObject root = doc.object();

    if (root.contains(u"QMP"_s) && !m_handshakeComplete) {
        QJsonObject capObj;
        capObj.insert(u"execute"_s, u"qmp_capabilities"_s);
        m_socket.write(QJsonDocument(capObj).toJson(QJsonDocument::Compact) + "\n");
        m_socket.flush();
        return;
    }

    if (root.contains(u"event"_s)) {
        const QString eventName = root.value(u"event"_s).toString();
        const QJsonObject eventData = root.value(u"data"_s).toObject();
        Q_EMIT eventReceived(eventName, eventData);
        return;
    }

    if (root.contains(u"return"_s) || root.contains(u"error"_s)) {
        if (!m_handshakeComplete && root.contains(u"return"_s) && !root.contains(u"id"_s)) {
            m_handshakeComplete = true;
            Q_EMIT connected();
            return;
        }

        const QString cmdId = root.value(u"id"_s).toString();

        if (m_pendingRequests.contains(cmdId)) {
            auto callback = m_pendingRequests.take(cmdId);

            QMPResult result;
            if (root.contains(u"error"_s)) {
                const QJsonObject err = root.value(u"error"_s).toObject();
                result.success = false;
                result.errorMessage = err.value(u"desc"_s).toString();
            } else {
                result.success = true;
                const QJsonValue retVal = root.value(u"return"_s);
                if (retVal.isObject()) {
                    result.data = retVal.toObject();
                } else if (retVal.isArray()) {
                    result.dataArray = retVal.toArray();
                }
            }
            callback(result);
        }
    }
}

void QMPClient::onSocketError(QLocalSocket::LocalSocketError /*socketError*/)
{
    Q_EMIT errorOccurred(m_socket.errorString());
}

void QMPClient::failPendingRequests(const QString &reason)
{
    // Take the map first: a callback may send a new command, which would
    // otherwise re-enter the container we are iterating.
    const auto pending = std::exchange(m_pendingRequests, {});

    for (const auto &callback : pending) {
        if (callback)
            callback({false, reason, {}, QJsonArray()});
    }

    m_screenshotInFlight = false;
    m_cpuUsageInFlight = false;
    m_blockStatsInFlight = false;
    m_memorySampleState = MemorySampleState::Idle;
}
