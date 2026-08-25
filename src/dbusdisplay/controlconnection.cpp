/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controlconnection.h"

#pragma push_macro("signals")
#undef signals
#include <gio/gunixfdlist.h>
#pragma pop_macro("signals")
#include <unistd.h>

#include "kemu_dbusdisplay_debug.h"

QemuControlConnection::QemuControlConnection(QObject *parent)
    : QObject(parent)
{
}

void QemuControlConnection::createAsync(int fd, QObject *parent, int timeoutMs, CreateFn done)
{
    GError *error = nullptr;
    GSocket *gsock = g_socket_new_from_fd(fd, &error);
    if (!gsock) {
        const QString msg = QString::fromUtf8(error->message);
        g_error_free(error);
        close(fd);
        done(nullptr, msg);
        return;
    }
    GSocketConnection *sockConn = g_socket_connection_factory_create_connection(gsock);
    g_object_unref(gsock);

    auto *self = new QemuControlConnection(parent);
    self->m_context = g_main_context_new();
    // Bridge first, so the async completion below gets dispatched.
    self->m_bridge = new GLibEventBridge(self->m_context, self);

    struct Ctx {
        QemuControlConnection *self;
        CreateFn done;
        GCancellable *cancellable;
    };
    auto *ctx = new Ctx{self, std::move(done), g_cancellable_new()};

    // Bound the handshake: an old QEMU without p2p support would otherwise
    // leave us waiting forever for the authentication server.
    QTimer::singleShot(timeoutMs, self, [cancellable = ctx->cancellable]() {
        g_cancellable_cancel(cancellable);
    });

    g_main_context_push_thread_default(self->m_context);
    g_dbus_connection_new(
        G_IO_STREAM(sockConn),
        nullptr,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
        nullptr,
        ctx->cancellable,
        [](GObject *, GAsyncResult *res, gpointer userData) {
            auto *ctx = static_cast<Ctx *>(userData);
            GError *err = nullptr;
            GDBusConnection *conn = g_dbus_connection_new_finish(res, &err);
            if (!conn) {
                const QString msg = QString::fromUtf8(err->message);
                g_error_free(err);
                ctx->done(nullptr, msg);
                ctx->self->deleteLater();
            } else {
                ctx->self->m_conn = conn;
                g_dbus_connection_set_exit_on_close(conn, FALSE);
                g_signal_connect(conn, "closed", G_CALLBACK(QemuControlConnection::onClosed), ctx->self);
                ctx->done(ctx->self, QString());
            }
            g_object_unref(ctx->cancellable);
            delete ctx;
        },
        ctx);
    g_main_context_pop_thread_default(self->m_context);
    g_object_unref(sockConn);
}

QemuControlConnection::~QemuControlConnection()
{
    if (m_bridge)
        m_bridge->stop();
    if (m_conn) {
        g_signal_handlers_disconnect_by_data(m_conn, this);
        g_dbus_connection_close_sync(m_conn, nullptr, nullptr);
        g_object_unref(m_conn);
    }
    if (m_context)
        g_main_context_unref(m_context);
}

void QemuControlConnection::onClosed(GDBusConnection *, gboolean, GError *, gpointer userData)
{
    auto *self = static_cast<QemuControlConnection *>(userData);
    Q_EMIT self->connectionLost();
}

void QemuControlConnection::callAsync(const QString &path, const QString &iface, const QString &method, GVariant *params)
{
    // bus_name is NULL on peer-to-peer connections.
    g_dbus_connection_call(m_conn,
                           nullptr,
                           path.toUtf8().constData(),
                           iface.toUtf8().constData(),
                           method.toUtf8().constData(),
                           params,
                           nullptr,
                           G_DBUS_CALL_FLAGS_NONE,
                           -1,
                           nullptr,
                           nullptr,
                           nullptr);
}

void QemuControlConnection::callWithReply(const QString &path, const QString &iface, const QString &method, GVariant *params, ReplyFn fn)
{
    struct Ctx {
        ReplyFn fn;
    };
    auto *ctx = new Ctx{std::move(fn)};

    // Route the reply into our bridged context so it fires on the GUI thread.
    g_main_context_push_thread_default(m_context);
    g_dbus_connection_call(
        m_conn,
        nullptr,
        path.toUtf8().constData(),
        iface.toUtf8().constData(),
        method.toUtf8().constData(),
        params,
        nullptr,
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        [](GObject *source, GAsyncResult *res, gpointer userData) {
            auto *ctx = static_cast<Ctx *>(userData);
            GError *error = nullptr;
            GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
            ctx->fn(reply, error);
            if (reply)
                g_variant_unref(reply);
            if (error)
                g_error_free(error);
            delete ctx;
        },
        ctx);
    g_main_context_pop_thread_default(m_context);
}

bool QemuControlConnection::callWithFdSync(const QString &path, const QString &iface, const QString &method, int fd, QString *errorMsg)
{
    GError *error = nullptr;
    GUnixFDList *fdList = g_unix_fd_list_new();
    if (g_unix_fd_list_append(fdList, fd, &error) < 0) {
        if (errorMsg)
            *errorMsg = QString::fromUtf8(error->message);
        g_error_free(error);
        g_object_unref(fdList);
        return false;
    }

    GVariant *reply = g_dbus_connection_call_with_unix_fd_list_sync(m_conn,
                                                                    nullptr,
                                                                    path.toUtf8().constData(),
                                                                    iface.toUtf8().constData(),
                                                                    method.toUtf8().constData(),
                                                                    g_variant_new("(h)", 0),
                                                                    nullptr,
                                                                    G_DBUS_CALL_FLAGS_NONE,
                                                                    5000,
                                                                    fdList,
                                                                    nullptr,
                                                                    nullptr,
                                                                    &error);
    g_object_unref(fdList);

    if (!reply) {
        if (errorMsg)
            *errorMsg = QString::fromUtf8(error->message);
        g_error_free(error);
        return false;
    }
    g_variant_unref(reply);
    return true;
}

guint QemuControlConnection::subscribeSignal(const QString &path, const QString &iface, const QString &member, SignalFn fn)
{
    struct Ctx {
        SignalFn fn;
    };
    auto *ctx = new Ctx{std::move(fn)};

    g_main_context_push_thread_default(m_context);
    const guint id = g_dbus_connection_signal_subscribe(
        m_conn,
        nullptr,
        iface.toUtf8().constData(),
        member.toUtf8().constData(),
        path.toUtf8().constData(),
        nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *, GVariant *parameters, gpointer userData) {
            static_cast<Ctx *>(userData)->fn(parameters);
        },
        ctx,
        [](gpointer userData) {
            delete static_cast<Ctx *>(userData);
        });
    g_main_context_pop_thread_default(m_context);
    return id;
}

void QemuControlConnection::unsubscribeSignal(guint id)
{
    if (id && m_conn)
        g_dbus_connection_signal_unsubscribe(m_conn, id);
}

guint QemuControlConnection::registerObject(const QString &path,
                                            const gchar *introspectionXml,
                                            const GDBusInterfaceVTable *vtable,
                                            gpointer userData,
                                            QString *errorMsg)
{
    GError *error = nullptr;
    GDBusNodeInfo *nodeInfo = g_dbus_node_info_new_for_xml(introspectionXml, &error);
    if (!nodeInfo) {
        if (errorMsg)
            *errorMsg = QString::fromUtf8(error->message);
        g_error_free(error);
        return 0;
    }
    const guint id = g_dbus_connection_register_object(m_conn, path.toUtf8().constData(), nodeInfo->interfaces[0], vtable, userData, nullptr, &error);
    g_dbus_node_info_unref(nodeInfo);
    if (!id) {
        if (errorMsg)
            *errorMsg = QString::fromUtf8(error->message);
        g_error_free(error);
    }
    return id;
}

void QemuControlConnection::unregisterObject(guint id)
{
    if (id && m_conn)
        g_dbus_connection_unregister_object(m_conn, id);
}
