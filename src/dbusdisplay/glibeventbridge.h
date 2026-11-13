/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#pragma push_macro("signals")
#undef signals
#include <glib.h>
#pragma pop_macro("signals")

#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QSocketNotifier>
#include <QTimer>
#include <QVector>

// =============================================================================
// GLibEventBridge - drives a GMainContext from the Qt event loop
// =============================================================================
// Watches the fds the context reports via g_main_context_query() - most
// importantly its internal wakeup fd - so dispatches queued from other
// threads (the shared GDBus worker) are picked up without polling.
//
// Must be created, used and destroyed on the thread running the Qt event
// loop. The bridge acquires the context for its whole lifetime.
// =============================================================================

class GLibEventBridge : public QObject
{
    Q_OBJECT

public:
    explicit GLibEventBridge(GMainContext *context, QObject *parent = nullptr);
    ~GLibEventBridge() override;

    // Creates a context this bridge can drive. Every context handed to the
    // bridge must come from here: GLib only signals the wakeup fd when a
    // source is attached from a thread other than the owner's, and the
    // bridge owns the context on the very thread that issues our own GDBus
    // calls. Without OWNERLESS_POLLING the call timeouts those attach would
    // never arm m_timeoutTimer, so an unanswered call would wait forever.
    static GMainContext *newContext()
    {
        return g_main_context_new_with_flags(G_MAIN_CONTEXT_FLAGS_OWNERLESS_POLLING);
    }

    // Detach: no further dispatching. Safe to call from within a handler
    // currently being dispatched by this bridge.
    void stop();

private:
    void runPass();
    void updateNotifiers();

    GMainContext *m_ctx = nullptr;
    bool m_acquired = false;
    bool m_active = false;

    QVector<GPollFD> m_pollFds;
    QVector<QPair<int, bool>> m_watched; // fd, wantsWrite
    QList<QSocketNotifier *> m_notifiers;
    QTimer m_timeoutTimer;
};
