/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <functional>
#include <unistd.h>

#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLocalSocket>
#include <QMap>
#include <QObject>
#include <QTimer>

#include <KLocalizedString>

using namespace Qt::StringLiterals;

struct QMPResult {
    bool success = false;
    QString errorMessage;
    QJsonObject data;
    QJsonArray dataArray;
};

// ---- CPU ----

struct VCpuUsageInfo {
    int cpuIndex = -1;
    qint64 threadId = -1;
    double usagePercent = 0.0;
};

struct VmCpuUsage {
    QList<VCpuUsageInfo> vcpus;
    double averagePercent = 0.0;
    bool isValid = false;
};

// ---- RAM (balloon) ----

struct VmMemoryUsage {
    qint64 actualBytes = 0; // current balloon size (guest-visible RAM)
    qint64 maxBytes = 0; // maximum configured RAM (from QEMU, not VMConfig)
    double usagePercent = 0.0; // actualBytes / maxBytes * 100
    bool isValid = false;
};

// ---- Block I/O ----

struct BlockDeviceStats {
    QString device;
    QString node;

    // Absolute counters (since VM start)
    qint64 rdBytes = 0;
    qint64 wrBytes = 0;
    qint64 rdOps = 0;
    qint64 wrOps = 0;

    // Rates (per second, computed from delta between two samples)
    double rdBytesPerSec = 0.0;
    double wrBytesPerSec = 0.0;
    double rdOpsPerSec = 0.0;
    double wrOpsPerSec = 0.0;
};

struct VmBlockStats {
    QList<BlockDeviceStats> devices;
    // Aggregated rates across all devices
    double totalRdBytesPerSec = 0.0;
    double totalWrBytesPerSec = 0.0;
    double totalRdOpsPerSec = 0.0;
    double totalWrOpsPerSec = 0.0;
    bool isValid = false;
};

class QMPClient : public QObject
{
    Q_OBJECT

public:
    explicit QMPClient(QObject *parent = nullptr);
    ~QMPClient();

    void connectToVm(const QString &socketPath);
    void disconnectFromVm();
    bool isConnected() const;

    void sendCommand(const QString &command, const QJsonObject &args = QJsonObject(), std::function<void(QMPResult)> callback = nullptr);
    QMPResult sendCommandSync(const QString &command, const QJsonObject &args = QJsonObject(), int timeoutMs = 2000);

    // Screenshot
    void startScreenshotTimer(const QString &dumpPath, int intervalMs = 20000);
    void stopScreenshotTimer();
    bool isScreenshotTimerActive() const;
    void requestScreenshot(const QString &dumpPath);

    // CPU usage (via /proc/<tid>/stat)
    void requestCpuUsage(std::function<void(VmCpuUsage)> callback);
    VmCpuUsage getCpuUsageSync(int timeoutMs = 2000);
    void startCpuUsageTimer();
    void stopCpuUsageTimer();
    bool isCpuUsageTimerActive() const;

    // RAM usage (via query-balloon, requires virtio-balloon)
    void requestMemoryUsage(std::function<void(VmMemoryUsage)> callback);
    VmMemoryUsage getMemoryUsageSync(int timeoutMs = 2000);

    // Block I/O stats (via query-blockstats, delta-based rates)
    void requestBlockStats(std::function<void(VmBlockStats)> callback);
    VmBlockStats getBlockStatsSync(int timeoutMs = 2000);

    // Combined performance monitoring timer.
    // Emits cpuUsageUpdated, memoryUsageUpdated, and blockStatsUpdated
    // at each interval tick.
    void startPerformanceTimer(int intervalMs = 1000);
    void stopPerformanceTimer();
    bool isPerformanceTimerActive() const;

    // USB passthrough
    QMPResult attachUsbDevice(int busNum, int devAddr, const QString &id);
    QMPResult detachUsbDevice(const QString &id);

Q_SIGNALS:
    void connected();
    void disconnected();
    void errorOccurred(const QString &errorMsg);

    void eventReceived(const QString &eventName, const QJsonObject &eventData);

    void screenshotCaptured(const QImage &image);

    void cpuUsageUpdated(const VmCpuUsage &usage);
    void memoryUsageUpdated(const VmMemoryUsage &usage);
    void blockStatsUpdated(const VmBlockStats &stats);

private Q_SLOTS:
    void onReadyRead();
    void onSocketError(QLocalSocket::LocalSocketError socketError);

private:
    QLocalSocket m_socket;
    QByteArray m_buffer;
    bool m_handshakeComplete = false;

    int m_commandCounter = 0;

    QMap<QString, std::function<void(QMPResult)>> m_pendingRequests;

    void processLine(const QByteArray &line);

    // Screenshot
    QTimer m_screenshotTimer;
    QString m_screenshotDumpPath;
    bool m_screenshotInFlight = false;
    void performScreenshot();

    // CPU usage
    struct ProcCpuSample {
        qint64 utime = 0;
        qint64 stime = 0;
        qint64 wallNs = 0;
    };

    QMap<qint64, ProcCpuSample> m_lastCpuSamples;
    QElapsedTimer m_cpuSampleClock;
    bool m_cpuUsageInFlight = false;
    void performCpuUsageSample();
    static ProcCpuSample readProcStat(qint64 tid);

    // RAM usage
    enum class MemorySampleState {
        Idle,
        Discovering,
        Enabling,
        Reading
    };
    MemorySampleState m_memorySampleState = MemorySampleState::Idle;
    qint64 m_maxMemoryBytes = 0; // cached from first balloon query
    // bool m_memoryInFlight = false;
    bool m_guestStatsEnabled = false;
    QString m_balloonPath;
    void performMemorySample();
    void startBalloonDiscovery();
    void enableGuestStats();
    void readGuestStats();

    // Block I/O
    struct BlockIoSnapshot {
        qint64 rdBytes = 0;
        qint64 wrBytes = 0;
        qint64 rdOps = 0;
        qint64 wrOps = 0;
        qint64 wallNs = 0;
    };

    QMap<QString, BlockIoSnapshot> m_lastBlockSamples; // key = node-name
    QElapsedTimer m_blockSampleClock;
    bool m_blockStatsInFlight = false;
    void performBlockStatsSample();

    // Combined performance timer (drives CPU, RAM, and I/O sampling)
    QTimer m_perfTimer;
    void performAllSamples();
};
