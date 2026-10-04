/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "slirpuplink.h"

extern "C" {
#include <arpa/inet.h>
#include <libslirp.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
}

#include <QHash>
#include <QHostAddress>
#include <QMutex>
#include <QMutexLocker>
#include <QSemaphore>
#include <QThread>

#include <atomic>
#include <cstdint>
#include <fcntl.h>
#include <functional>
#include <utility>
#include <vector>

#include "kemu_debug.h"

using namespace Qt::StringLiterals;

// libslirp 4.9 introduced slirp_os_socket and deprecated slirp_pollfds_fill.
#if SLIRP_CHECK_VERSION(4, 9, 0)
using SlirpPollSocket = slirp_os_socket;
#define KEMU_SLIRP_POLLFDS_FILL slirp_pollfds_fill_socket
#else
using SlirpPollSocket = int;
#define KEMU_SLIRP_POLLFDS_FILL slirp_pollfds_fill
#endif

// ---------------------------------------------------------------------------
// Worker thread: owns the Slirp instance and drives its poll loop.
// ---------------------------------------------------------------------------

class SlirpWorkerThread : public QThread
{
    Q_OBJECT

public:
    SlirpUplink::Config config;

    void requestStop()
    {
        m_stopRequested.store(true);
        wake();
    }

    // Thread-safe entry points (called from the main thread).
    void postFrame(const QByteArray &frame)
    {
        QMutexLocker locker(&m_mutex);
        if (m_inbound.size() >= kMaxQueuedFrames)
            return; // drop under pressure; TCP retransmits
        m_inbound.append(frame);
        locker.unlock();
        wake();
    }

    void postTask(std::function<void(Slirp *)> task)
    {
        QMutexLocker locker(&m_mutex);
        m_tasks.append(std::move(task));
        locker.unlock();
        wake();
    }

    /// Blocks until the slirp instance is up (or failed) and reports success.
    bool waitInitialized(QString *error)
    {
        if (!m_initReady.tryAcquire(1, 2000)) {
            *error = u"timed out waiting for libslirp initialization"_s;
            return false;
        }
        if (!m_initError.isEmpty()) {
            *error = m_initError;
            return false;
        }
        return true;
    }

Q_SIGNALS:
    void frameOut(const QByteArray &frame);

protected:
    void run() override
    {
        if (pipe2(m_wakePipe, O_NONBLOCK) != 0) {
            m_initError = u"pipe2() failed"_s;
            m_initReady.release();
            return;
        }

        Slirp *slirp = slirp_new(&m_slirpConfig, &m_slirpCallbacks, this);
        if (!slirp) {
            m_initError = u"slirp_new() failed"_s;
            m_initReady.release();
            close(m_wakePipe[0]);
            close(m_wakePipe[1]);
            return;
        }
        qCDebug(KEMU) << "SlirpUplink: libslirp" << slirp_version_string() << "initialized, poll loop starting";
        m_initReady.release();

        while (!m_stopRequested.load()) {
            uint32_t timeout = UINT32_MAX;

            m_pollfds.clear();
            KEMU_SLIRP_POLLFDS_FILL(slirp, &timeout, &SlirpWorkerThread::addPollSocket, this);
            m_pollfds.push_back({m_wakePipe[0], POLLIN, 0});

            const qint64 now = clockMs();
            for (const Timer *timer : std::as_const(m_timers)) {
                if (!timer->armed)
                    continue;
                const qint64 delta = qMax<qint64>(0, timer->deadlineMs - now);
                timeout = qMin<qint64>(timeout, delta);
            }

            const int ret = poll(m_pollfds.data(), m_pollfds.size(), timeout == UINT32_MAX ? -1 : static_cast<int>(timeout));

            if (ret > 0 && (m_pollfds.back().revents & POLLIN)) {
                char buf[64];
                while (read(m_wakePipe[0], buf, sizeof(buf)) > 0) { } // non-blocking: drains until EAGAIN
            }

            QList<QByteArray> inbound;
            QList<std::function<void(Slirp *)>> tasks;
            {
                QMutexLocker locker(&m_mutex);
                inbound.swap(m_inbound);
                tasks.swap(m_tasks);
            }
            for (const QByteArray &frame : inbound)
                slirp_input(slirp, reinterpret_cast<const uint8_t *>(frame.constData()), frame.size());
            for (const auto &task : tasks)
                task(slirp);

            slirp_pollfds_poll(slirp, ret < 0 ? 1 : 0, &SlirpWorkerThread::getREvents, this);

            fireTimers(slirp);
        }

        // slirp_cleanup() frees its timers via timerFree(); delete whatever
        // remains afterwards.
        qCDebug(KEMU) << "SlirpUplink: poll loop exiting, cleaning up";
        slirp_cleanup(slirp);
        for (Timer *timer : std::as_const(m_timers))
            delete timer;
        m_timers.clear();

        close(m_wakePipe[0]);
        close(m_wakePipe[1]);
        qCDebug(KEMU) << "SlirpUplink: cleanup done";
    }

private:
    struct Timer {
        SlirpTimerCb cb = nullptr;
        void *cbOpaque = nullptr;
        SlirpTimerId id = SLIRP_TIMER_RA;
        bool opaqueStyle = false;
        bool armed = false;
        qint64 deadlineMs = 0;
    };

    static qint64 clockMs()
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    }

    void wake()
    {
        const char byte = 0;
        if (m_wakePipe[1] >= 0)
            Q_UNUSED(write(m_wakePipe[1], &byte, 1)); // EAGAIN when full is fine
    }

    void fireTimers(Slirp *slirp)
    {
        const qint64 now = clockMs();
        const QList<Timer *> timers = m_timers.values();
        for (Timer *timer : timers) {
            // A timer callback may free timers (including this one), so
            // re-validate against the registry before dereferencing.
            if (!m_timers.contains(timer) || !timer->armed || timer->deadlineMs > now)
                continue;
            timer->armed = false;
            const SlirpTimerCb cb = timer->cb;
            void *cbOpaque = timer->cbOpaque;
            const SlirpTimerId id = timer->id;
            const bool opaqueStyle = timer->opaqueStyle;
            if (opaqueStyle)
                slirp_handle_timer(slirp, id, cbOpaque);
            else if (cb)
                cb(cbOpaque);
        }
    }

    // --- libslirp callbacks (all invoked on the worker thread) ---

    static ssize_t sendPacket(const void *buf, size_t len, void *opaque)
    {
        auto *self = static_cast<SlirpWorkerThread *>(opaque);
        qCDebug(KEMU) << "SlirpUplink: slirp -> switch," << len << "bytes";
        Q_EMIT self->frameOut(QByteArray(static_cast<const char *>(buf), static_cast<qsizetype>(len)));
        return static_cast<ssize_t>(len);
    }

    static void guestError(const char *msg, void * /*opaque*/)
    {
        qCWarning(KEMU) << "SlirpUplink: guest error:" << msg;
    }

    static int64_t clockGetNs(void * /*opaque*/)
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
    }

    static void *timerNew(SlirpTimerCb cb, void *cbOpaque, void *opaque)
    {
        auto *self = static_cast<SlirpWorkerThread *>(opaque);
        auto *timer = new Timer;
        timer->cb = cb;
        timer->cbOpaque = cbOpaque;
        self->m_timers.insert(timer, timer);
        return timer;
    }

    static void *timerNewOpaque(SlirpTimerId id, void *cbOpaque, void *opaque)
    {
        auto *self = static_cast<SlirpWorkerThread *>(opaque);
        auto *timer = new Timer;
        timer->id = id;
        timer->cbOpaque = cbOpaque;
        timer->opaqueStyle = true;
        self->m_timers.insert(timer, timer);
        return timer;
    }

    static void timerFree(void *timer, void *opaque)
    {
        auto *self = static_cast<SlirpWorkerThread *>(opaque);
        auto *entry = static_cast<Timer *>(timer);
        self->m_timers.remove(entry);
        delete entry;
    }

    static void timerMod(void *timer, int64_t expireTime, void *opaque)
    {
        auto *self = static_cast<SlirpWorkerThread *>(opaque);
        auto *entry = static_cast<Timer *>(timer);
        entry->deadlineMs = expireTime;
        entry->armed = true;
        self->wake();
    }

    static void notify(void *opaque)
    {
        static_cast<SlirpWorkerThread *>(opaque)->wake();
    }

    // Deprecated poll callbacks, no-ops for us: we re-collect all fds via
    // slirp_pollfds_fill_socket() on every iteration. libslirp calls these
    // unconditionally whenever cfg_version < 6, so they must not be null.
    static void registerPollFd(int /*fd*/, void * /*opaque*/)
    {
    }

    static void unregisterPollFd(int /*fd*/, void * /*opaque*/)
    {
    }

    static int addPollSocket(SlirpPollSocket fd, int events, void *opaque)
    {
        auto *self = static_cast<SlirpWorkerThread *>(opaque);
        short pollEvents = 0;
        if (events & SLIRP_POLL_IN)
            pollEvents |= POLLIN;
        if (events & SLIRP_POLL_OUT)
            pollEvents |= POLLOUT;
        if (events & SLIRP_POLL_PRI)
            pollEvents |= POLLPRI;
        self->m_pollfds.push_back({fd, pollEvents, 0});
        return static_cast<int>(self->m_pollfds.size()) - 1;
    }

    static int getREvents(int idx, void *opaque)
    {
        auto *self = static_cast<SlirpWorkerThread *>(opaque);
        if (idx < 0 || idx >= static_cast<int>(self->m_pollfds.size()))
            return 0;
        const short revents = self->m_pollfds[idx].revents;
        int events = 0;
        if (revents & POLLIN)
            events |= SLIRP_POLL_IN;
        if (revents & POLLOUT)
            events |= SLIRP_POLL_OUT;
        if (revents & POLLPRI)
            events |= SLIRP_POLL_PRI;
        if (revents & POLLERR)
            events |= SLIRP_POLL_ERR;
        if (revents & POLLHUP)
            events |= SLIRP_POLL_HUP;
        return events;
    }

    int m_wakePipe[2] = {-1, -1};
    std::atomic<bool> m_stopRequested{false};

    QSemaphore m_initReady;
    QString m_initError;

    QMutex m_mutex;
    QList<QByteArray> m_inbound;
    QList<std::function<void(Slirp *)>> m_tasks;

    std::vector<pollfd> m_pollfds;
    QHash<void *, Timer *> m_timers;

    static constexpr int kMaxQueuedFrames = 256;

    // Set up before run(): libslirp configuration and callback table.
    SlirpConfig m_slirpConfig = {};
    SlirpCb m_slirpCallbacks = {};

public:
    void prepareSlirpConfig()
    {
        m_slirpConfig.version = 4; // disable_dhcp requires SlirpConfig version 4 (libslirp >= 4.7)
        m_slirpConfig.restricted = 0;
        m_slirpConfig.in_enabled = true;
        m_slirpConfig.vnetwork.s_addr = htonl(config.network);
        m_slirpConfig.vnetmask.s_addr = htonl(config.netmask);
        m_slirpConfig.vhost.s_addr = htonl(config.gateway);
        m_slirpConfig.in6_enabled = false;
        m_slirpConfig.vdhcp_start.s_addr = htonl(config.dhcpStart != 0 ? config.dhcpStart : (config.network | 0x0f));
        m_slirpConfig.vnameserver.s_addr = htonl(config.nameserver);
        m_slirpConfig.disable_dns = false;
        m_slirpConfig.disable_dhcp = !config.enableDhcp;
        m_slirpConfig.disable_host_loopback = !config.allowHostLoopback;
        m_slirpConfig.enable_emu = false;

        m_slirpCallbacks.send_packet = &SlirpWorkerThread::sendPacket;
        m_slirpCallbacks.guest_error = &SlirpWorkerThread::guestError;
        m_slirpCallbacks.clock_get_ns = &SlirpWorkerThread::clockGetNs;
        m_slirpCallbacks.timer_new = &SlirpWorkerThread::timerNew;
        m_slirpCallbacks.timer_free = &SlirpWorkerThread::timerFree;
        m_slirpCallbacks.timer_mod = &SlirpWorkerThread::timerMod;
        m_slirpCallbacks.notify = &SlirpWorkerThread::notify;
        m_slirpCallbacks.timer_new_opaque = &SlirpWorkerThread::timerNewOpaque;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        m_slirpCallbacks.register_poll_fd = &SlirpWorkerThread::registerPollFd;
        m_slirpCallbacks.unregister_poll_fd = &SlirpWorkerThread::unregisterPollFd;
#pragma GCC diagnostic pop
    }
};

// ---------------------------------------------------------------------------
// Main-thread facade
// ---------------------------------------------------------------------------

SlirpUplink::SlirpUplink(QObject *parent)
    : VirtualSwitchPort(parent)
{
}

SlirpUplink::~SlirpUplink()
{
    stop();
}

bool SlirpUplink::start(const Config &config)
{
    if (m_thread)
        return true;

    m_thread = new SlirpWorkerThread;
    m_thread->config = config;
    m_thread->prepareSlirpConfig();

    // frameOut is emitted from the worker thread - queue it onto ourselves.
    connect(m_thread, &SlirpWorkerThread::frameOut, this, &SlirpUplink::frameReady, Qt::QueuedConnection);

    m_thread->start();

    QString error;
    if (!m_thread->waitInitialized(&error)) {
        stop();
        m_lastError = error;
        Q_EMIT errorOccurred(error);
        return false;
    }
    qCDebug(KEMU) << "SlirpUplink: started";
    return true;
}

void SlirpUplink::stop()
{
    if (!m_thread)
        return;
    m_thread->requestStop();
    if (!m_thread->wait(3000)) {
        m_thread->terminate();
        m_thread->wait();
    }
    delete m_thread;
    m_thread = nullptr;
}

bool SlirpUplink::isRunning() const
{
    return m_thread && m_thread->isRunning();
}

void SlirpUplink::addPortForward(bool udp, quint16 hostPort, quint32 guestIp, quint16 guestPort)
{
    if (!m_thread)
        return;
    qCDebug(KEMU) << "SlirpUplink: adding port forward" << (udp ? "udp" : "tcp") << "127.0.0.1:" << hostPort << "->" << QHostAddress(guestIp).toString() << ":"
                  << guestPort;
    m_thread->postTask([udp, hostPort, guestIp, guestPort](Slirp *slirp) {
        struct in_addr hostAddr;
        hostAddr.s_addr = htonl(INADDR_LOOPBACK);
        struct in_addr guestAddr;
        guestAddr.s_addr = htonl(guestIp);
        if (slirp_add_hostfwd(slirp, udp ? 1 : 0, hostAddr, hostPort, guestAddr, guestPort) < 0)
            qCWarning(KEMU) << "SlirpUplink: failed to add port forward" << hostPort << "->" << guestPort;
    });
}

void SlirpUplink::receiveFrame(const QByteArray &frame)
{
    if (m_thread) {
        qCDebug(KEMU) << "SlirpUplink: switch -> slirp," << frame.size() << "bytes";
        m_thread->postFrame(frame);
    }
}

#include "slirpuplink.moc"
