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
#include <utility>

#include "kemu_dbusdisplay_debug.h"

class QemuControlDispatch : public QObject
{
public:
    QemuControlDispatch()
        : m_context(g_main_context_new())
        , m_bridge(new GLibEventBridge(m_context, this))
        , m_cancellable(g_cancellable_new())
    {
    }

    ~QemuControlDispatch() override
    {
        delete m_bridge;
        g_object_unref(m_cancellable);
        g_main_context_unref(m_context);
    }

    GMainContext *context() const
    {
        return m_context;
    }

    GCancellable *cancellable() const
    {
        return m_cancellable;
    }

    void begin()
    {
        ++m_pending;
    }

    void finish()
    {
        --m_pending;
        if (m_shuttingDown && !m_pending)
            deleteLater();
    }

    void shutdown()
    {
        m_shuttingDown = true;
        g_cancellable_cancel(m_cancellable);
        if (!m_pending)
            deleteLater();
    }

private:
    GMainContext *m_context;
    GLibEventBridge *m_bridge;
    GCancellable *m_cancellable;
    int m_pending = 0;
    bool m_shuttingDown = false;
};

QemuControlConnection::QemuControlConnection(QObject *parent)
    : QObject(parent)
    , m_dispatch(new QemuControlDispatch)
    , m_context(m_dispatch->context())
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
    auto *dispatch = self->m_dispatch;

    struct Ctx {
        QPointer<QemuControlConnection> self;
        QemuControlDispatch *dispatch;
        CreateFn done;
        GCancellable *cancellable;
        QTimer *timer;
    };
    auto *ctx = new Ctx{self, dispatch, std::move(done), G_CANCELLABLE(g_object_ref(dispatch->cancellable())), new QTimer(dispatch)};
    ctx->timer->setSingleShot(true);
    QObject::connect(ctx->timer, &QTimer::timeout, dispatch, [cancellable = ctx->cancellable]() {
        g_cancellable_cancel(cancellable);
    });
    ctx->timer->start(timeoutMs);

    dispatch->begin();
    g_main_context_push_thread_default(dispatch->context());
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
            QString msg;
            if (!conn) {
                msg = QString::fromUtf8(err->message);
                g_error_free(err);
            } else if (ctx->self) {
                ctx->self->m_conn = conn;
                g_dbus_connection_set_exit_on_close(conn, FALSE);
                g_signal_connect(conn, "closed", G_CALLBACK(QemuControlConnection::onClosed), ctx->self.data());
            } else {
                g_dbus_connection_close(conn, nullptr, nullptr, nullptr);
                g_object_unref(conn);
            }
            ctx->timer->stop();
            delete ctx->timer;
            g_object_unref(ctx->cancellable);

            const bool ok = (conn != nullptr);
            QTimer::singleShot(0, ctx->dispatch, [ctx, ok, msg]() {
                if (ctx->self) {
                    if (ok) {
                        ctx->done(ctx->self.data(), QString());
                    } else {
                        ctx->done(nullptr, msg);
                        if (ctx->self)
                            ctx->self->deleteLater();
                    }
                }
                auto *dispatch = ctx->dispatch;
                delete ctx;
                dispatch->finish();
            });
        },
        ctx);
    g_main_context_pop_thread_default(dispatch->context());
    g_object_unref(sockConn);
}

QemuControlConnection::~QemuControlConnection()
{
    if (m_conn) {
        g_signal_handlers_disconnect_by_data(m_conn, this);
        for (guint id : std::as_const(m_signalIds))
            g_dbus_connection_signal_unsubscribe(m_conn, id);
        for (guint id : std::as_const(m_objectIds))
            g_dbus_connection_unregister_object(m_conn, id);

        m_dispatch->begin();
        g_main_context_push_thread_default(m_context);
        g_dbus_connection_close(m_conn,
                                nullptr,
                                [](GObject *source, GAsyncResult *res, gpointer userData) {
                                    auto *dispatch = static_cast<QemuControlDispatch *>(userData);
                                    GError *error = nullptr;
                                    g_dbus_connection_close_finish(G_DBUS_CONNECTION(source), res, &error);
                                    g_clear_error(&error);
                                    dispatch->finish();
                                },
                                m_dispatch);
        g_main_context_pop_thread_default(m_context);
        g_object_unref(m_conn);
    }
    m_dispatch->shutdown();
}

void QemuControlConnection::onClosed(GDBusConnection *, gboolean, GError *, gpointer userData)
{
    auto *self = static_cast<QemuControlConnection *>(userData);
    Q_EMIT self->connectionLost();
}

void QemuControlConnection::callAsync(const QString &path, const QLatin1StringView &iface, const QString &method, GVariant *params)
{
    // bus_name is NULL on peer-to-peer connections.
    g_dbus_connection_call(m_conn,
                           nullptr,
                           path.toUtf8().constData(),
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
                           iface.toUtf8().constData(),
#else
                           QString(iface).toUtf8().constData(),
#endif
                           method.toUtf8().constData(),
                           params,
                           nullptr,
                           G_DBUS_CALL_FLAGS_NONE,
                           -1,
                           m_dispatch->cancellable(),
                           nullptr,
                           nullptr);
}

void QemuControlConnection::callWithReply(const QString &path, const QString &iface, const QString &method, GVariant *params, ReplyFn fn)
{
    struct Ctx {
        ReplyFn fn;
        QPointer<QemuControlConnection> self;
        QemuControlDispatch *dispatch;
    };
    auto *ctx = new Ctx{std::move(fn), this, m_dispatch};

    m_dispatch->begin();
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
        m_dispatch->cancellable(),
        [](GObject *source, GAsyncResult *res, gpointer userData) {
            auto *ctx = static_cast<Ctx *>(userData);
            GError *error = nullptr;
            GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
            if (ctx->self)
                ctx->fn(reply, error);
            if (reply)
                g_variant_unref(reply);
            if (error)
                g_error_free(error);
            auto *dispatch = ctx->dispatch;
            delete ctx;
            dispatch->finish();
        },
        ctx);
    g_main_context_pop_thread_default(m_context);
}

void QemuControlConnection::callWithFd(const QString &path, const QString &iface, const QString &method, int fd, FdCallFn fn)
{
    GError *error = nullptr;
    GUnixFDList *fdList = g_unix_fd_list_new();
    const int index = g_unix_fd_list_append(fdList, fd, &error);
    close(fd);
    if (index < 0) {
        const QString msg = QString::fromUtf8(error->message);
        g_error_free(error);
        g_object_unref(fdList);
        fn(false, msg);
        return;
    }

    struct Ctx {
        FdCallFn fn;
        QPointer<QemuControlConnection> self;
        QemuControlDispatch *dispatch;
    };
    auto *ctx = new Ctx{std::move(fn), this, m_dispatch};

    m_dispatch->begin();
    g_main_context_push_thread_default(m_context);
    g_dbus_connection_call_with_unix_fd_list(
        m_conn,
        nullptr,
        path.toUtf8().constData(),
        iface.toUtf8().constData(),
        method.toUtf8().constData(),
        g_variant_new("(h)", 0),
        nullptr,
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        fdList,
        m_dispatch->cancellable(),
        [](GObject *source, GAsyncResult *res, gpointer userData) {
            auto *ctx = static_cast<Ctx *>(userData);
            GError *err = nullptr;
            GVariant *reply = g_dbus_connection_call_with_unix_fd_list_finish(G_DBUS_CONNECTION(source), nullptr, res, &err);
            const bool ok = (reply != nullptr);
            QString msg;
            if (reply)
                g_variant_unref(reply);
            if (err) {
                msg = QString::fromUtf8(err->message);
                g_error_free(err);
            }
            QTimer::singleShot(0, ctx->dispatch, [ctx, ok, msg]() {
                if (ctx->self)
                    ctx->fn(ok, msg);
                auto *dispatch = ctx->dispatch;
                delete ctx;
                dispatch->finish();
            });
        },
        ctx);
    g_main_context_pop_thread_default(m_context);
    g_object_unref(fdList);
}

guint QemuControlConnection::subscribeSignal(const QString &path, const QString &iface, const QString &member, SignalFn fn)
{
    struct Ctx {
        SignalFn fn;
        QPointer<QemuControlConnection> self;
    };
    auto *ctx = new Ctx{std::move(fn), this};

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
            auto *ctx = static_cast<Ctx *>(userData);
            if (ctx->self)
                ctx->fn(parameters);
        },
        ctx,
        [](gpointer userData) {
            delete static_cast<Ctx *>(userData);
        });
    g_main_context_pop_thread_default(m_context);
    if (id)
        m_signalIds.append(id);
    return id;
}

void QemuControlConnection::unsubscribeSignal(guint id)
{
    if (id && m_conn) {
        m_signalIds.removeAll(id);
        g_dbus_connection_signal_unsubscribe(m_conn, id);
    }
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
    // Bind method dispatch to the bridged m_context, not the global default.
    g_main_context_push_thread_default(m_context);
    const guint id = g_dbus_connection_register_object(m_conn, path.toUtf8().constData(), nodeInfo->interfaces[0], vtable, userData, nullptr, &error);
    g_main_context_pop_thread_default(m_context);
    g_dbus_node_info_unref(nodeInfo);
    if (!id) {
        if (errorMsg)
            *errorMsg = QString::fromUtf8(error->message);
        g_error_free(error);
    } else {
        m_objectIds.append(id);
    }
    return id;
}

void QemuControlConnection::unregisterObject(guint id)
{
    if (id && m_conn) {
        m_objectIds.removeAll(id);
        g_dbus_connection_unregister_object(m_conn, id);
    }
}
