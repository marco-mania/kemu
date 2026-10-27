/*
 * SPDX-FileCopyrightText: 2026 KEMU contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QCoreApplication>
#include <QEventLoop>
#include <QSignalSpy>
#include <QTemporaryFile>
#include <QTest>
#include <QThread>
#include <QTimer>

#pragma push_macro("signals")
#undef signals
#include <gio/gunixfdlist.h>
#pragma pop_macro("signals")

#include <algorithm>
#include <chrono>
#include <cstring>
#include <future>
#include <iterator>
#include <unistd.h>

#include "dbusdisplay/audiojittercore.h"
#include "dbusdisplay/listener.h"
#include "dbusdisplay/pcmringbuffer.h"

namespace
{
constexpr guint32 kPixmanX8R8G8B8 = (32u << 24) | (2u << 16) | (8u << 8) | (8u << 4) | 8u;
constexpr const char *kListenerPath = "/org/qemu/Display1/Listener";
constexpr const char *kListenerIface = "org.qemu.Display1.Listener";
constexpr const char *kMapIface = "org.qemu.Display1.Listener.Unix.Map";
constexpr const char *kDmaBuf2Iface = "org.qemu.Display1.Listener.Unix.ScanoutDMABUF2";

struct CallResult {
    bool ok = false;
    QString error;
};

class DisplayPeer
{
public:
    QemuDisplayListener listener;

    ~DisplayPeer()
    {
        if (m_connection) {
            g_dbus_connection_close_sync(m_connection, nullptr, nullptr);
            g_object_unref(m_connection);
        }
    }

    bool start()
    {
        const int fd = listener.createSocketPair();
        if (fd < 0)
            return false;

        auto connection = std::async(std::launch::async, [fd]() -> GDBusConnection * {
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
            GDBusConnection *peer = g_dbus_connection_new_sync(G_IO_STREAM(stream),
                                                                guid,
                                                                G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_SERVER,
                                                                nullptr,
                                                                nullptr,
                                                                &error);
            g_free(guid);
            g_object_unref(stream);
            g_clear_error(&error);
            return peer;
        });

        const bool ready = listener.connectListener();
        m_connection = connection.get();
        QCoreApplication::processEvents();
        return ready && m_connection;
    }

    CallResult call(const char *iface, const char *method, GVariant *parameters = nullptr, GUnixFDList *fds = nullptr)
    {
        if (parameters)
            g_variant_ref_sink(parameters);
        if (fds)
            g_object_ref(fds);

        auto result = std::async(std::launch::async, [this, iface, method, parameters, fds]() {
            GError *error = nullptr;
            GVariant *reply = nullptr;
            if (fds) {
                reply = g_dbus_connection_call_with_unix_fd_list_sync(m_connection,
                                                                       nullptr,
                                                                       kListenerPath,
                                                                       iface,
                                                                       method,
                                                                       parameters,
                                                                       nullptr,
                                                                       G_DBUS_CALL_FLAGS_NONE,
                                                                       2000,
                                                                       fds,
                                                                       nullptr,
                                                                       nullptr,
                                                                       &error);
            } else {
                reply = g_dbus_connection_call_sync(m_connection,
                                                    nullptr,
                                                    kListenerPath,
                                                    iface,
                                                    method,
                                                    parameters,
                                                    nullptr,
                                                    G_DBUS_CALL_FLAGS_NONE,
                                                    2000,
                                                    nullptr,
                                                    &error);
            }

            CallResult r;
            r.ok = reply != nullptr;
            if (error)
                r.error = QString::fromUtf8(error->message);
            g_clear_error(&error);
            if (reply)
                g_variant_unref(reply);
            if (parameters)
                g_variant_unref(parameters);
            if (fds)
                g_object_unref(fds);
            return r;
        });

        while (result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1);
        }
        return result.get();
    }

    CallResult scanout(int width, int height, int stride, const QByteArray &data)
    {
        GVariant *bytes = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, data.constData(), data.size(), sizeof(guint8));
        return call(kListenerIface, "Scanout", g_variant_new("(uuuu@ay)", guint32(width), guint32(height), guint32(stride), kPixmanX8R8G8B8, bytes));
    }

private:
    GDBusConnection *m_connection = nullptr;
};

} // namespace

class DbusDisplayTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void scanoutAndDirtyRegion()
    {
        DisplayPeer peer;
        QVERIFY(peer.start());
        QSignalSpy scanouts(&peer.listener, &QemuDisplayListener::scanoutChanged);

        QByteArray pixels(32, '\0');
        for (int i = 0; i < pixels.size(); ++i)
            pixels[i] = char(i + 1);
        QVERIFY2(peer.scanout(3, 2, 16, pixels).ok, "Valid Scanout failed");

        const QImage image = peer.listener.currentImage();
        QCOMPARE(image.size(), QSize(3, 2));
        QCOMPARE(std::memcmp(image.constScanLine(0), pixels.constData(), 12), 0);
        QCOMPARE(std::memcmp(image.constScanLine(1), pixels.constData() + 16, 12), 0);
        QCOMPARE(peer.listener.takeDirtyRegion(), QRegion(QRect(0, 0, 3, 2)));
        QVERIFY(peer.listener.takeDirtyRegion().isEmpty());
        QCOMPARE(scanouts.size(), 1);
    }

    void rejectsInvalidScanouts()
    {
        DisplayPeer peer;
        QVERIFY(peer.start());
        QByteArray valid(16, '\x12');
        QVERIFY(peer.scanout(2, 2, 8, valid).ok);
        peer.listener.takeDirtyRegion();

        QVERIFY(!peer.scanout(2, 2, 4, valid).ok);
        QVERIFY(!peer.scanout(2, 2, 8, QByteArray(8, '\0')).ok);
        QCOMPARE(peer.listener.currentImage().size(), QSize(2, 2));
        QCOMPARE(static_cast<int>(peer.listener.currentImage().constScanLine(0)[0]), 0x12);
        QVERIFY(peer.listener.takeDirtyRegion().isEmpty());
    }

    void updateClipsSourceAndDirtyRegion()
    {
        DisplayPeer peer;
        QVERIFY(peer.start());
        QVERIFY(peer.scanout(4, 4, 16, QByteArray(64, '\0')).ok);
        peer.listener.takeDirtyRegion();
        QSignalSpy updates(&peer.listener, &QemuDisplayListener::regionUpdated);

        QByteArray patch(36, '\0');
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 3; ++x)
                patch[y * 12 + x * 4] = char(10 * y + x + 1);
        GVariant *bytes = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, patch.constData(), patch.size(), 1);
        QVERIFY(peer.call(kListenerIface, "Update", g_variant_new("(iiiiuu@ay)", -1, -1, 3, 3, 12u, kPixmanX8R8G8B8, bytes)).ok);

        const QImage image = peer.listener.currentImage();
        QCOMPARE(int(image.constScanLine(0)[0]), 12);
        QCOMPARE(int(image.constScanLine(0)[4]), 13);
        QCOMPARE(int(image.constScanLine(1)[0]), 22);
        QCOMPARE(int(image.constScanLine(1)[4]), 23);
        QCOMPARE(int(image.constScanLine(2)[0]), 0);
        QCOMPARE(peer.listener.takeDirtyRegion(), QRegion(QRect(0, 0, 2, 2)));
        QCOMPARE(updates.size(), 1);
        QCOMPARE(updates.at(0).at(0).toRect(), QRect(0, 0, 2, 2));
    }

    void rejectsShortUpdate()
    {
        DisplayPeer peer;
        QVERIFY(peer.start());
        QVERIFY(peer.scanout(4, 4, 16, QByteArray(64, '\0')).ok);
        peer.listener.takeDirtyRegion();
        QByteArray patch(8, '\0');
        GVariant *bytes = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, patch.constData(), patch.size(), 1);
        QVERIFY(!peer.call(kListenerIface, "Update", g_variant_new("(iiiiuu@ay)", 0, 0, 2, 2, 8u, kPixmanX8R8G8B8, bytes)).ok);
        QVERIFY(peer.listener.takeDirtyRegion().isEmpty());
    }

    void mappedScanoutAndUpdateMap()
    {
        DisplayPeer peer;
        QVERIFY(peer.start());
        QTemporaryFile file;
        QVERIFY(file.open());
        QCOMPARE(file.write(QByteArray(64, '\x51')), qint64(64));
        QVERIFY(file.flush());

        GUnixFDList *fds = g_unix_fd_list_new();
        GError *error = nullptr;
        QCOMPARE(g_unix_fd_list_append(fds, file.handle(), &error), 0);
        QVERIFY(error == nullptr);
        const CallResult result = peer.call(kMapIface, "ScanoutMap", g_variant_new("(huuuuu)", 0, 0u, 4u, 4u, 16u, kPixmanX8R8G8B8), fds);
        g_object_unref(fds);
        QVERIFY2(result.ok, qPrintable(result.error));

        QCOMPARE(peer.listener.currentImage().size(), QSize(4, 4));
        QCOMPARE(int(peer.listener.currentImage().constScanLine(0)[0]), 0x51);
        QCOMPARE(peer.listener.takeDirtyRegion(), QRegion(QRect(0, 0, 4, 4)));
        QSignalSpy updates(&peer.listener, &QemuDisplayListener::regionUpdated);
        QVERIFY(peer.call(kMapIface, "UpdateMap", g_variant_new("(iiii)", 1, 1, 2, 2)).ok);
        QCOMPARE(peer.listener.takeDirtyRegion(), QRegion(QRect(1, 1, 2, 2)));
        QCOMPARE(updates.size(), 1);
    }

    void cursorAndMouseEvents()
    {
        DisplayPeer peer;
        QVERIFY(peer.start());
        QSignalSpy cursors(&peer.listener, &QemuDisplayListener::cursorImageChanged);
        QSignalSpy moves(&peer.listener, &QemuDisplayListener::cursorMoved);

        const QByteArray pixels(16, '\x5a');
        GVariant *bytes = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, pixels.constData(), pixels.size(), 1);
        QVERIFY(peer.call(kListenerIface, "CursorDefine", g_variant_new("(iiii@ay)", 2, 2, 1, 1, bytes)).ok);
        QVERIFY(peer.call(kListenerIface, "MouseSet", g_variant_new("(iii)", 7, 9, 1)).ok);

        const CursorInfo cursor = peer.listener.cursorInfo();
        QCOMPARE(cursor.image.size(), QSize(2, 2));
        QCOMPARE(cursor.hot_x, 1);
        QCOMPARE(cursor.hot_y, 1);
        QCOMPARE(cursor.pos_x, 7);
        QCOMPARE(cursor.pos_y, 9);
        QVERIFY(cursor.visible);
        QCOMPARE(cursors.size(), 1);
        QCOMPARE(moves.size(), 1);
    }

    void dmaBufFdOwnership()
    {
        DisplayPeer peer;
        QVERIFY(peer.start());
        QTemporaryFile file;
        QVERIFY(file.open());
        QCOMPARE(file.write(QByteArray(64, '\0')), qint64(64));

        GUnixFDList *fds = g_unix_fd_list_new();
        GError *error = nullptr;
        QCOMPARE(g_unix_fd_list_append(fds, file.handle(), &error), 0);
        QVERIFY(error == nullptr);
        const CallResult result = peer.call(kListenerIface,
                                            "ScanoutDMABUF",
                                            g_variant_new("(huuuutb)", 0, 4u, 4u, 16u, 0x34325258u, guint64(0), TRUE),
                                            fds);
        g_object_unref(fds);
        QVERIFY2(result.ok, qPrintable(result.error));
        QVERIFY(peer.listener.isDmaBuf());

        DmaBufScanout scanout;
        QVERIFY(peer.listener.takeDmaBufScanout(scanout));
        QCOMPARE(scanout.numPlanes, 1);
        QCOMPARE(scanout.numFds, 1);
        QCOMPARE(scanout.width, 4u);
        QVERIFY(scanout.fds[0].isValid());
        QVERIFY(!peer.listener.isDmaBuf());
        QVERIFY(peer.call(kListenerIface, "Disable").ok);
        QVERIFY(scanout.fds[0].isValid());
    }

    void rejectsShortMappedFile()
    {
        DisplayPeer peer;
        QVERIFY(peer.start());
        QVERIFY(peer.scanout(2, 2, 8, QByteArray(16, '\x34')).ok);
        peer.listener.takeDirtyRegion();

        QTemporaryFile file;
        QVERIFY(file.open());
        QCOMPARE(file.write(QByteArray(16, '\x56')), qint64(16));
        QVERIFY(file.flush());

        GUnixFDList *fds = g_unix_fd_list_new();
        GError *error = nullptr;
        QCOMPARE(g_unix_fd_list_append(fds, file.handle(), &error), 0);
        QVERIFY(error == nullptr);
        const CallResult result = peer.call(kMapIface, "ScanoutMap", g_variant_new("(huuuuu)", 0, 0u, 4u, 4u, 16u, kPixmanX8R8G8B8), fds);
        g_object_unref(fds);

        QVERIFY(!result.ok);
        QCOMPARE(peer.listener.currentImage().size(), QSize(2, 2));
        QCOMPARE(int(peer.listener.currentImage().constScanLine(0)[0]), 0x34);
        QVERIFY(peer.listener.takeDirtyRegion().isEmpty());
    }

    void disableClearsScanout()
    {
        DisplayPeer peer;
        QVERIFY(peer.start());
        QVERIFY(peer.scanout(2, 2, 8, QByteArray(16, '\x44')).ok);
        QSignalSpy scanouts(&peer.listener, &QemuDisplayListener::scanoutChanged);

        QVERIFY(peer.call(kListenerIface, "Disable").ok);
        QVERIFY(peer.listener.currentImage().isNull());
        QVERIFY(peer.listener.takeDirtyRegion().isEmpty());
        QCOMPARE(scanouts.size(), 1);
        QCOMPARE(scanouts.at(0).at(0).toInt(), 0);
        QCOMPARE(scanouts.at(0).at(1).toInt(), 0);
    }

    void dmaBufRejectsOverflowingDimensions()
    {
        DisplayPeer peer;
        QVERIFY(peer.start());
        QTemporaryFile file;
        QVERIFY(file.open());
        QCOMPARE(file.write(QByteArray(64, '\0')), qint64(64));

        GUnixFDList *fds = g_unix_fd_list_new();
        GError *error = nullptr;
        QCOMPARE(g_unix_fd_list_append(fds, file.handle(), &error), 0);
        QVERIFY(error == nullptr);
        const CallResult result = peer.call(kListenerIface,
                                            "ScanoutDMABUF",
                                            g_variant_new("(huuuutb)", 0, G_MAXUINT32, 10u, 40u, 0x34325258u, guint64(0), TRUE),
                                            fds);
        QVERIFY(!result.ok);
        QVERIFY(!peer.listener.isDmaBuf());

        const gint32 handles[] = {0};
        const guint32 offsets[] = {0};
        const guint32 strides[] = {16};
        GVariant *handleArray = g_variant_new_fixed_array(G_VARIANT_TYPE_HANDLE, handles, 1, sizeof(gint32));
        GVariant *offsetArray = g_variant_new_fixed_array(G_VARIANT_TYPE_UINT32, offsets, 1, sizeof(guint32));
        GVariant *strideArray = g_variant_new_fixed_array(G_VARIANT_TYPE_UINT32, strides, 1, sizeof(guint32));
        GVariant *args = g_variant_new("(@ahuuuu@au@auuuuutb)",
                                      handleArray, 0u, 0u, 4u, 4u, offsetArray, strideArray, 1u, 0x34325258u, G_MAXUINT32, 4u, guint64(0), TRUE);
        QVERIFY(!peer.call(kDmaBuf2Iface, "ScanoutDMABUF2", args, fds).ok);
        g_object_unref(fds);
        QVERIFY(!peer.listener.isDmaBuf());
    }

    void deferredDmaBufUpdateAcknowledged()
    {
        DisplayPeer peer;
        QVERIFY(peer.start());
        QSignalSpy updates(&peer.listener, &QemuDisplayListener::regionUpdated);
        QTimer::singleShot(50, &peer.listener, &QemuDisplayListener::ackPendingFrame);
        QVERIFY(peer.call(kListenerIface, "UpdateDMABUF", g_variant_new("(iiii)", 0, 0, 2, 2)).ok);
        QCOMPARE(updates.size(), 1);
    }

    void audioJitterPrebufferAndUnderrun()
    {
        AudioJitterCore core;
        AudioJitterCore::Config cfg;
        cfg.channels = 1;
        cfg.sampleRate = 1000;
        cfg.targetMs = 10;
        cfg.capacityMs = 100;
        cfg.fadeInMs = 1;
        core.configure(cfg);

        float output[20];
        core.pull(output, 20);
        QVERIFY(!core.isPlaying());
        for (float sample : output)
            QCOMPARE(sample, 0.0f);

        float input[50];
        std::fill(std::begin(input), std::end(input), 0.25f);
        core.push(input, 50);
        core.pull(output, 20);
        QVERIFY(core.isPlaying());
        QVERIFY(core.stats().underruns == 0);
        core.pull(output, 20);
        QVERIFY(core.stats().underruns > 0);
        QVERIFY(!core.isPlaying());
    }

    void pcmRingWrapAndOverflow()
    {
        PcmRingBuffer ring;
        ring.reset(8, 2);
        QCOMPARE(ring.write("abcdef", 6), 0);
        char out[16] = {};
        QCOMPARE(ring.read(out, 4), 4);
        QCOMPARE(QByteArray(out, 4), QByteArray("abcd"));

        QCOMPARE(ring.write("ghijkl", 6), 0);
        QCOMPARE(ring.size(), 8);
        QCOMPARE(ring.read(out, 8), 8);
        QCOMPARE(QByteArray(out, 8), QByteArray("efghijkl"));
        QVERIFY(ring.isEmpty());

        QCOMPARE(ring.write("mnopqrstuv", 10), 2);
        QCOMPARE(ring.read(out, 8), 8);
        QCOMPARE(QByteArray(out, 8), QByteArray("opqrstuv"));
    }
};

QTEST_GUILESS_MAIN(DbusDisplayTest)

#include "dbusdisplaytest.moc"
