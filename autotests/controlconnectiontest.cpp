/*
 * SPDX-FileCopyrightText: 2026 KEMU contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QCoreApplication>
#include <QTest>

#pragma push_macro("signals")
#undef signals
#include <gio/gunixfdlist.h>
#pragma pop_macro("signals")

#include <atomic>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <future>
#include <memory>
#include <sys/socket.h>
#include <unistd.h>

#include "dbusdisplay/controlconnection.h"

class ControlConnectionTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void parentDestroyedDuringHandshake()
    {
        int fds[2];
        QVERIFY(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

        auto *owner = new QObject;
        auto called = std::make_shared<std::atomic_bool>(false);
        QemuControlConnection::createAsync(fds[0], owner, 1000, [called](QemuControlConnection *, const QString &) {
            *called = true;
        });
        delete owner;
        close(fds[1]);

        QTest::qWait(100);
        QVERIFY(!*called);
    }

    void pendingCallsAfterDestruction()
    {
        int fds[2];
        QVERIFY(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

        auto peer = std::async(std::launch::async, [fd = fds[1]]() -> GDBusConnection * {
            GError *error = nullptr;
            GSocket *socket = g_socket_new_from_fd(fd, &error);
            if (!socket) {
                g_clear_error(&error);
                close(fd);
                return nullptr;
            }
            GSocketConnection *stream = g_socket_connection_factory_create_connection(socket);
            g_object_unref(socket);
            if (!stream)
                return nullptr;

            gchar *guid = g_dbus_generate_guid();
            GDBusConnection *conn = g_dbus_connection_new_sync(G_IO_STREAM(stream),
                                                               guid,
                                                               G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_SERVER,
                                                               nullptr,
                                                               nullptr,
                                                               &error);
            g_free(guid);
            g_object_unref(stream);
            g_clear_error(&error);
            return conn;
        });

        auto *owner = new QObject;
        QemuControlConnection *ctrl = nullptr;
        bool finished = false;
        QemuControlConnection::createAsync(fds[0], owner, 2000, [&](QemuControlConnection *connection, const QString &) {
            ctrl = connection;
            finished = true;
        });

        QTRY_VERIFY_WITH_TIMEOUT(finished, 3000);
        QVERIFY(ctrl != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(peer.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 3000);
        GDBusConnection *server = peer.get();
        QVERIFY(server != nullptr);

        auto callbacks = std::make_shared<std::atomic_int>(0);
        ctrl->callWithReply(u"/org/qemu/Test"_s, u"org.qemu.Test"_s, u"Pending"_s, nullptr, [callbacks](GVariant *, const GError *) {
            ++*callbacks;
        });

        const int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        QVERIFY(fd >= 0);
        ctrl->callWithFd(u"/org/qemu/Test"_s, u"org.qemu.Test"_s, u"PendingFd"_s, fd, [callbacks](bool, const QString &) {
            ++*callbacks;
        });
        errno = 0;
        QCOMPARE(fcntl(fd, F_GETFD), -1);
        QCOMPARE(errno, EBADF);

        delete owner;
        QTest::qWait(150);
        QCOMPARE(callbacks->load(), 0);

        g_dbus_connection_close_sync(server, nullptr, nullptr);
        g_object_unref(server);
    }
};

QTEST_GUILESS_MAIN(ControlConnectionTest)
#include "controlconnectiontest.moc"
