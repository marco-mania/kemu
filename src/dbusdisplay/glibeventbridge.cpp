/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "glibeventbridge.h"

#include <utility>

#include "kemu_dbusdisplay_debug.h"

GLibEventBridge::GLibEventBridge(GMainContext *context, QObject *parent)
    : QObject(parent)
    , m_ctx(context)
{
    g_main_context_ref(m_ctx);

    // Dispatching requires ownership. GDBus only queues into the context
    // from its worker thread and never acquires it itself.
    m_acquired = g_main_context_acquire(m_ctx);
    if (!m_acquired) {
        qCWarning(KEMU_DBUSDISPLAY) << "GLibEventBridge: context owned by another thread";
        return;
    }
    m_active = true;

    m_timeoutTimer.setSingleShot(true);
    connect(&m_timeoutTimer, &QTimer::timeout, this, &GLibEventBridge::runPass);

    runPass();
}

GLibEventBridge::~GLibEventBridge()
{
    stop();
    if (m_acquired)
        g_main_context_release(m_ctx);
    g_main_context_unref(m_ctx);
}

void GLibEventBridge::stop()
{
    m_active = false;
    m_timeoutTimer.stop();
    for (auto *n : std::as_const(m_notifiers))
        n->setEnabled(false);
}

void GLibEventBridge::runPass()
{
    if (!m_active)
        return;

    // Dispatch everything that is ready; iteration() polls with timeout 0
    // and cannot block.
    while (g_main_context_iteration(m_ctx, FALSE)) {
        if (!m_active)
            return; // a dispatched handler stopped us (connection lost)
    }
    // QPointer<GLibEventBridge> guard(this);
    // for (;;) {
    //     if (!g_main_context_iteration(m_ctx, FALSE))
    //         break;
    //     if (!guard || !m_active)
    //         return; // a dispatched handler stopped or destroyed us
    // }

    // Ask the context which fds to wait on next. The set includes its
    // internal wakeup fd, signalled when another thread queues a dispatch.
    gint priority = 0;
    g_main_context_prepare(m_ctx, &priority);

    gint timeoutMs = -1;
    if (m_pollFds.size() < 4)
        m_pollFds.resize(4);
    gint nfds;
    while ((nfds = g_main_context_query(m_ctx, priority, &timeoutMs, m_pollFds.data(), m_pollFds.size())) > m_pollFds.size())
        m_pollFds.resize(nfds);
    m_pollFds.resize(nfds);

    updateNotifiers();

    if (timeoutMs >= 0)
        m_timeoutTimer.start(timeoutMs);
    else
        m_timeoutTimer.stop();
}

void GLibEventBridge::updateNotifiers()
{
    // The fd set is stable in practice; rebuild only on change.
    QVector<QPair<int, bool>> wanted;
    for (const GPollFD &p : std::as_const(m_pollFds)) {
        if (p.events & G_IO_IN)
            wanted.append({p.fd, false});
        if (p.events & G_IO_OUT)
            wanted.append({p.fd, true});
    }
    if (wanted == m_watched)
        return;

    qDeleteAll(m_notifiers);
    m_notifiers.clear();
    for (const auto &[fd, wantsWrite] : std::as_const(wanted)) {
        auto *n = new QSocketNotifier(fd, wantsWrite ? QSocketNotifier::Write : QSocketNotifier::Read, this);
        connect(n, &QSocketNotifier::activated, this, &GLibEventBridge::runPass);
        m_notifiers.append(n);
    }
    m_watched = wanted;
}
