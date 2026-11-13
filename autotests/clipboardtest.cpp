/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// QemuClipboardManager against a stand-in for QEMU's side of
// org.qemu.Display1.Clipboard, over a real peer-to-peer GDBus connection.

#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QTest>

#include <future>
#include <mutex>
#include <optional>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "dbusdisplay/clipboard.h"
#include "dbusdisplay/controlconnection.h"
#include "dbusdisplay/dbusdisplayconst.h"

namespace
{
const gchar clipboardXml[] = R"XML(
<node>
  <interface name="org.qemu.Display1.Clipboard">
    <method name="Register"/>
    <method name="Unregister"/>
    <method name="Grab">
      <arg type="u" name="selection" direction="in"/>
      <arg type="u" name="serial" direction="in"/>
      <arg type="as" name="mimes" direction="in"/>
    </method>
    <method name="Release">
      <arg type="u" name="selection" direction="in"/>
    </method>
    <method name="Request">
      <arg type="u" name="selection" direction="in"/>
      <arg type="as" name="mimes" direction="in"/>
      <arg type="s" name="mime" direction="out"/>
      <arg type="ay" name="data" direction="out"/>
    </method>
  </interface>
</node>
)XML";

struct Grab {
    uint serial;
    QStringList mimes;
};

// QEMU's side: answers the client's calls on its own thread and context, and
// can call the client back the way QEMU does.
class FakeQemu
{
public:
    // The handshake needs the client side, so this does not wait for it:
    // see waitUntilReady().
    explicit FakeQemu(int fd)
    {
        m_started = m_ready.get_future();
        m_thread = std::thread([this, fd] {
            run(fd, m_ready);
        });
    }

    bool waitUntilReady()
    {
        for (int i = 0; i < 400; ++i) {
            if (m_started.wait_for(std::chrono::milliseconds(5)) == std::future_status::ready)
                return m_conn != nullptr;
            QCoreApplication::processEvents();
        }
        return false;
    }

    ~FakeQemu()
    {
        g_main_loop_quit(m_loop);
        m_thread.join();
    }

    std::vector<Grab> grabs() const
    {
        std::lock_guard lock(m_mutex);
        return m_grabs;
    }

    int registrations() const
    {
        std::lock_guard lock(m_mutex);
        return m_registrations;
    }

    // QEMU restarting the serial counter, e.g. when the guest agent reconnects.
    void resetSerial()
    {
        callClient("Register", nullptr);
    }

    // QEMU fetching the host clipboard for a guest paste.
    std::optional<QString> request()
    {
        GVariantBuilder mimes;
        g_variant_builder_init(&mimes, G_VARIANT_TYPE("as"));
        g_variant_builder_add(&mimes, "s", "text/plain;charset=utf-8");
        GVariant *reply = callClient("Request", g_variant_new("(uas)", 0u, &mimes));
        if (!reply)
            return std::nullopt;
        const gchar *mime = nullptr;
        GVariant *data = nullptr;
        g_variant_get(reply, "(&s@ay)", &mime, &data);
        gsize size = 0;
        const auto *bytes = static_cast<const char *>(g_variant_get_fixed_array(data, &size, 1));
        const QString text = QString::fromUtf8(bytes, qsizetype(size));
        g_variant_unref(data);
        g_variant_unref(reply);
        return text;
    }

private:
    // Blocking calls run on a helper thread, so the test thread keeps
    // spinning the Qt loop that drives the client.
    GVariant *callClient(const char *method, GVariant *params)
    {
        auto result = std::async(std::launch::async, [this, method, params] {
            return g_dbus_connection_call_sync(m_conn,
                                               nullptr,
                                               "/org/qemu/Display1/Clipboard",
                                               "org.qemu.Display1.Clipboard",
                                               method,
                                               params,
                                               nullptr,
                                               G_DBUS_CALL_FLAGS_NONE,
                                               2000,
                                               nullptr,
                                               nullptr);
        });
        while (result.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready)
            QCoreApplication::processEvents();
        return result.get();
    }

    void run(int fd, std::promise<void> &ready)
    {
        GMainContext *context = g_main_context_new();
        g_main_context_push_thread_default(context);
        m_loop = g_main_loop_new(context, FALSE);

        GSocket *socket = g_socket_new_from_fd(fd, nullptr);
        GSocketConnection *stream = g_socket_connection_factory_create_connection(socket);
        g_object_unref(socket);
        gchar *guid = g_dbus_generate_guid();
        m_conn = g_dbus_connection_new_sync(G_IO_STREAM(stream), guid, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_SERVER, nullptr, nullptr, nullptr);
        g_free(guid);
        g_object_unref(stream);

        static const GDBusInterfaceVTable vtable = {&FakeQemu::onMethodCall, nullptr, nullptr, {}};
        GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(clipboardXml, nullptr);
        g_dbus_connection_register_object(m_conn, "/org/qemu/Display1/Clipboard", node->interfaces[0], &vtable, this, nullptr, nullptr);
        g_dbus_node_info_unref(node);

        ready.set_value();
        g_main_loop_run(m_loop);

        g_dbus_connection_close_sync(m_conn, nullptr, nullptr);
        g_object_unref(m_conn);
        g_main_loop_unref(m_loop);
        g_main_context_pop_thread_default(context);
        g_main_context_unref(context);
    }

    static void onMethodCall(GDBusConnection *,
                             const gchar *,
                             const gchar *,
                             const gchar *,
                             const gchar *method,
                             GVariant *parameters,
                             GDBusMethodInvocation *invocation,
                             gpointer userData)
    {
        auto *self = static_cast<FakeQemu *>(userData);
        if (g_strcmp0(method, "Grab") == 0) {
            guint32 selection = 0, serial = 0;
            gchar **mimes = nullptr;
            g_variant_get(parameters, "(uu^as)", &selection, &serial, &mimes);
            Grab grab{serial, {}};
            for (gchar **m = mimes; m && *m; ++m)
                grab.mimes << QString::fromUtf8(*m);
            g_strfreev(mimes);
            std::lock_guard lock(self->m_mutex);
            self->m_grabs.push_back(grab);
        } else if (g_strcmp0(method, "Register") == 0) {
            std::lock_guard lock(self->m_mutex);
            ++self->m_registrations;
        } else if (g_strcmp0(method, "Request") == 0) {
            g_dbus_method_invocation_return_dbus_error(invocation, "org.qemu.Display1.Error.Failed", "Empty clipboard");
            return;
        }
        g_dbus_method_invocation_return_value(invocation, nullptr);
    }

    std::promise<void> m_ready;
    std::future<void> m_started;
    std::thread m_thread;
    GMainLoop *m_loop = nullptr;
    GDBusConnection *m_conn = nullptr;
    mutable std::mutex m_mutex;
    std::vector<Grab> m_grabs;
    int m_registrations = 0;
};

void setHostText(const QString &text, bool secret = false)
{
    auto *mime = new QMimeData;
    mime->setText(text);
    if (secret)
        mime->setData(u"x-kde-passwordManagerHint"_s, "secret");
    QGuiApplication::clipboard()->setMimeData(mime);
}
} // namespace

class ClipboardTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void hostToGuest()
    {
        int fds[2];
        QVERIFY(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        FakeQemu qemu(fds[1]);

        QemuControlConnection *ctrl = nullptr;
        QemuControlConnection::createAsync(fds[0], this, 2000, [&](QemuControlConnection *connection, const QString &) {
            ctrl = connection;
        });
        QTRY_VERIFY_WITH_TIMEOUT(ctrl != nullptr, 3000);
        QVERIFY(qemu.waitUntilReady());

        // Already on the clipboard before sharing starts: offered right away
        // instead of waiting for the next change.
        setHostText(u"first"_s);
        auto *manager = new QemuClipboardManager(ctrl, ClipboardDirection::Both, ctrl);
        QVERIFY(manager->setup());
        QTRY_COMPARE_WITH_TIMEOUT(qemu.registrations(), 1, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(qemu.grabs().size(), size_t(1), 2000);
        QCOMPARE(qemu.grabs().at(0).serial, 0u);
        QVERIFY(qemu.grabs().at(0).mimes.contains(u"text/plain;charset=utf-8"_s));
        QCOMPARE(qemu.request(), std::optional<QString>(u"first"_s));

        // Password manager entries are shared like any other text, so they
        // can be pasted into the guest.
        setHostText(u"hunter2"_s, true);
        QTRY_COMPARE_WITH_TIMEOUT(qemu.grabs().size(), size_t(2), 2000);
        QCOMPARE(qemu.grabs().at(1).serial, 1u);
        QCOMPARE(qemu.request(), std::optional<QString>(u"hunter2"_s));

        setHostText(u"second"_s);
        QTRY_COMPARE_WITH_TIMEOUT(qemu.grabs().size(), size_t(3), 2000);
        QCOMPARE(qemu.grabs().at(2).serial, 2u);
        QCOMPARE(qemu.request(), std::optional<QString>(u"second"_s));

        // QEMU restarts the session: the current content is offered again,
        // with the serial back at 0.
        qemu.resetSerial();
        QTRY_COMPARE_WITH_TIMEOUT(qemu.grabs().size(), size_t(4), 2000);
        QCOMPARE(qemu.grabs().at(3).serial, 0u);
        QCOMPARE(qemu.request(), std::optional<QString>(u"second"_s));

        delete manager;
        delete ctrl;
    }
};

QTEST_MAIN(ClipboardTest)
#include "clipboardtest.moc"
