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

#include <QImage>
#include <QMutex>
#include <QObject>
#include <QPainter>
#include <QPoint>
#include <QRect>

using namespace Qt::StringLiterals;

struct DmaBufScanout {
    int fd = -1;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t fourcc = 0;
    uint64_t modifier = 0;
    bool y0_top = true;
};

struct CursorInfo {
    QImage image;
    int hot_x = 0;
    int hot_y = 0;
    int pos_x = 0;
    int pos_y = 0;
    bool visible = false;
};

class QemuDisplayListener : public QObject
{
    Q_OBJECT

public:
    explicit QemuDisplayListener(QObject *parent = nullptr);
    ~QemuDisplayListener() override;

    int createSocketPair();
    bool connectListener();

    QImage currentImage() const;
    bool takeDmaBufScanout(DmaBufScanout &out);
    bool isDmaBuf() const;
    CursorInfo cursorInfo() const;

    // FD of the GDBus socket (for QSocketNotifier-based event integration)
    int socketFd() const
    {
        return m_socketFd;
    }

    static void onMethodCall(GDBusConnection *connection,
                             const gchar *sender,
                             const gchar *object_path,
                             const gchar *interface_name,
                             const gchar *method_name,
                             GVariant *parameters,
                             GDBusMethodInvocation *invocation,
                             gpointer user_data);
    static void onConnectionClosed(GDBusConnection *connection, gboolean remote_peer_vanished, GError *error, gpointer user_data);

Q_SIGNALS:
    void scanoutChanged(int width, int height);
    void dmaBufScanout(int width, int height);
    void regionUpdated(const QRect &rect);
    void cursorChanged();
    void connectionLost();

private:
    void handleScanout(GVariant *parameters, GDBusMethodInvocation *invocation);
    void handleScanoutMap(GVariant *parameters, GDBusMethodInvocation *invocation);
    void handleScanoutDMABUF(GVariant *parameters, GDBusMethodInvocation *invocation);
    void handleUpdate(GVariant *parameters, GDBusMethodInvocation *invocation);
    void handleUpdateMap(GVariant *parameters, GDBusMethodInvocation *invocation);
    void handleUpdateDMABUF(GVariant *parameters, GDBusMethodInvocation *invocation);
    void handleDisable(GDBusMethodInvocation *invocation);
    void handleMouseSet(GVariant *parameters, GDBusMethodInvocation *invocation);
    void handleCursorDefine(GVariant *parameters, GDBusMethodInvocation *invocation);

    void unmapMemory();
    void closeDmaBuf();
    static QImage::Format pixmanFormatToQt(uint32_t pixman_format);

    GDBusConnection *m_listenerConn = nullptr;
    guint m_registrationId = 0;
    guint m_registrationIdUnix = 0;

    int m_qemuFd = -1;
    int m_ourFd = -1;
    int m_socketFd = -1; // Stored FD for external monitoring

    mutable QMutex m_mutex;

    QImage m_image;
    void *m_mappedMemory = nullptr;
    size_t m_mappedSize = 0;

    DmaBufScanout m_dmaBuf;
    bool m_isDmaBuf = false;

    CursorInfo m_cursor;
    bool m_closed = false;
};
