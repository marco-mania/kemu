/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#pragma push_macro("signals")
#undef signals
#include <gio/gio.h>
#pragma pop_macro("signals")

#include <QObject>
#include <QString>
#include <functional>

#include "dbusdisplay/glibeventbridge.h"

using namespace Qt::StringLiterals;

// =============================================================================
// QemuControlConnection - p2p GDBus control channel to QEMU
// =============================================================================
// Wraps the KEMU end of the socketpair handed to QEMU via QMP add_client.
// Runs on its own GMainContext, dispatched by GLibEventBridge on the GUI
// thread. All callbacks fire on the GUI thread.
// =============================================================================

class QemuControlConnection : public QObject
{
    Q_OBJECT

public:
    // Takes ownership of fd. Completes on the GUI thread: on success with a
    // ready connection, on failure/timeout with nullptr and a message.
    using CreateFn = std::function<void(QemuControlConnection *conn, const QString &error)>;
    static void createAsync(int fd, QObject *parent, int timeoutMs, CreateFn done);
    ~QemuControlConnection() override;

    GDBusConnection *connection() const
    {
        return m_conn;
    }
    GMainContext *context() const
    {
        return m_context;
    }

    // Fire-and-forget method call (hot path: input events).
    void callAsync(const QString &path, const QString &iface, const QString &method, GVariant *params);

    // Method call with reply. reply is owned by the callback (unref'd after);
    // error is set instead on failure.
    using ReplyFn = std::function<void(GVariant *reply, const GError *error)>;
    void callWithReply(const QString &path, const QString &iface, const QString &method, GVariant *params, ReplyFn fn);

    // Synchronous call passing one fd (index 0 in the fd list). Does not
    // close fd. Returns false and fills errorMsg on failure.
    bool callWithFdSync(const QString &path, const QString &iface, const QString &method, int fd, QString *errorMsg = nullptr);

    // Signal subscription; handler fires on the GUI thread.
    using SignalFn = std::function<void(GVariant *parameters)>;
    guint subscribeSignal(const QString &path, const QString &iface, const QString &member, SignalFn fn);
    void unsubscribeSignal(guint id);

    // Object registration (for the clipboard callbacks).
    guint
    registerObject(const QString &path, const gchar *introspectionXml, const GDBusInterfaceVTable *vtable, gpointer userData, QString *errorMsg = nullptr);
    void unregisterObject(guint id);

Q_SIGNALS:
    void connectionLost();

private:
    explicit QemuControlConnection(QObject *parent = nullptr);
    static void onClosed(GDBusConnection *conn, gboolean remotePeerVanished, GError *error, gpointer userData);

    GDBusConnection *m_conn = nullptr;
    GMainContext *m_context = nullptr;
    GLibEventBridge *m_bridge = nullptr;
};
