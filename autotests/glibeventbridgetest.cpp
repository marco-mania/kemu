/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QCoreApplication>
#include <QTest>

#include <atomic>
#include <thread>

#include "dbusdisplay/glibeventbridge.h"

namespace
{
gboolean markFired(gpointer data)
{
    static_cast<std::atomic_bool *>(data)->store(true);
    return G_SOURCE_REMOVE;
}
} // namespace

class GLibEventBridgeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // GDBus attaches the timeout of every call to the thread-default context
    // from the calling thread - for KEMU the GUI thread, which also owns the
    // bridged context. GLib does not wake an owner about its own attaches
    // unless the context polls ownerless, so the bridge must pick this up.
    void timeoutAttachedFromOwningThread()
    {
        GMainContext *context = GLibEventBridge::newContext();
        auto *bridge = new GLibEventBridge(context);

        std::atomic_bool fired = false;
        GSource *source = g_timeout_source_new(50);
        g_source_set_callback(source, markFired, &fired, nullptr);
        g_source_attach(source, context);
        g_source_unref(source);

        QTRY_VERIFY_WITH_TIMEOUT(fired.load(), 2000);

        delete bridge;
        g_main_context_unref(context);
    }

    // The GDBus worker thread queues replies and incoming calls this way.
    void idleAttachedFromOtherThread()
    {
        GMainContext *context = GLibEventBridge::newContext();
        auto *bridge = new GLibEventBridge(context);

        std::atomic_bool fired = false;
        std::thread worker([context, &fired] {
            GSource *source = g_idle_source_new();
            g_source_set_callback(source, markFired, &fired, nullptr);
            g_source_attach(source, context);
            g_source_unref(source);
        });
        worker.join();

        QTRY_VERIFY_WITH_TIMEOUT(fired.load(), 2000);

        delete bridge;
        g_main_context_unref(context);
    }
};

QTEST_GUILESS_MAIN(GLibEventBridgeTest)
#include "glibeventbridgetest.moc"
