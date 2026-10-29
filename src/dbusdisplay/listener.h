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
#include <QObject>
#include <QPainter>
#include <QPoint>
#include <QRect>
#include <QRegion>

#include <QByteArrayList>

#include <algorithm>
#include <array>
#include <cstdint>

#include "dbusdisplay/glibeventbridge.h"
#include "dbusdisplay/uniquefd.h"

using namespace Qt::StringLiterals;

#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffULL
#endif
#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR 0ULL
#endif

struct DmaBufScanout {
    static constexpr int kMaxPlanes = 4;

    std::array<UniqueFd, kMaxPlanes> fds;
    std::array<uint32_t, kMaxPlanes> offsets = {};
    std::array<uint32_t, kMaxPlanes> strides = {};
    int numFds = 0; // planes beyond numFds share the last fd
    int numPlanes = 0;

    // Visible scanout rect inside the backing buffer
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t backingWidth = 0;
    uint32_t backingHeight = 0;

    uint32_t fourcc = 0;
    uint64_t modifier = DRM_FORMAT_MOD_INVALID;
    bool y0_top = true;

    int planeFd(int plane) const
    {
        return numFds > 0 ? fds[std::min(plane, numFds - 1)].get() : -1;
    }
};

struct CursorInfo {
    QImage image;
    int hot_x = 0;
    int hot_y = 0;
    int pos_x = 0;
    int pos_y = 0;
    bool visible = false;
};

// =============================================================================
// QemuDisplayListener
// =============================================================================
// Receives display updates from QEMU via D-Bus.
//
// Threading model:
//   All public methods (currentImage(), cursorInfo(), takeDmaBufScanout(),
//   isDmaBuf()) and all GDBus callbacks run on the GUI thread. The
//   connection, its exported objects and its "closed" signal are bound to the
//   private m_context, which the listener's own GLibEventBridge drives from
//   the Qt event loop. Nothing goes through the global default GMainContext.
//
//   Therefore no internal synchronisation is required. Do NOT call methods
//   of this class from any other thread without revisiting this assumption.
// =============================================================================

class QemuDisplayListener : public QObject
{
    Q_OBJECT

public:
    explicit QemuDisplayListener(QObject *parent = nullptr);
    ~QemuDisplayListener() override;

    int createSocketPair();
    bool connectListener();

    QImage currentImage() const;

    // Returns rects that have changed since the last call and resets the
    // internal dirty region. After this call, the listener considers all
    // pixel data to be uploaded to the GPU.
    //
    // On full-frame events (Scanout, ScanoutMap, Disable), the dirty
    // region implicitly covers the whole frame so the caller re-uploads
    // everything.
    QRegion takeDirtyRegion();

    bool takeDmaBufScanout(DmaBufScanout &out);
    void restoreDmaBufScanout(DmaBufScanout &&buf);
    bool isDmaBuf() const;
    CursorInfo cursorInfo() const;

    // Acknowledge a pending Update reply. Called by the display widget once
    // the frame content has actually been consumed (uploaded/rendered).
    void ackPendingFrame();

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
    static GVariant *onGetProperty(GDBusConnection *connection,
                                   const gchar *sender,
                                   const gchar *object_path,
                                   const gchar *interface_name,
                                   const gchar *property_name,
                                   GError **error,
                                   gpointer user_data);
    static void onConnectionClosed(GDBusConnection *connection, gboolean remote_peer_vanished, GError *error, gpointer user_data);

    GMainContext *mainContext() const
    {
        return m_context;
    }

Q_SIGNALS:
    void scanoutChanged(int width, int height);
    void dmaBufScanout(int width, int height);
    void regionUpdated(const QRect &rect);
    void cursorImageChanged(); // CursorDefine: new image / hotspot
    void cursorMoved(); // MouseSet: position and/or visibility
    void connectionLost();

private:
    void handleScanout(GVariant *parameters, GDBusMethodInvocation *invocation);
    void handleScanoutMap(GVariant *parameters, GDBusMethodInvocation *invocation);
    void handleScanoutDMABUF(GVariant *parameters, GDBusMethodInvocation *invocation);
    void handleScanoutDMABUF2(GVariant *parameters, GDBusMethodInvocation *invocation);
    void acceptDmaBuf(DmaBufScanout &&buf, GDBusMethodInvocation *invocation);
    void handleUpdate(GVariant *parameters, GDBusMethodInvocation *invocation);
    void handleUpdateMap(GVariant *parameters, GDBusMethodInvocation *invocation);
    void handleUpdateDMABUF(GVariant *parameters, GDBusMethodInvocation *invocation);
    void handleDisable(GDBusMethodInvocation *invocation);
    void handleMouseSet(GVariant *parameters, GDBusMethodInvocation *invocation);
    void handleCursorDefine(GVariant *parameters, GDBusMethodInvocation *invocation);

    void addDirtyRect(const QRect &r);
    void unmapMemory();
    void closeDmaBuf();
    static QImage::Format pixmanFormatToQt(uint32_t pixman_format);

    void deferFrameAck(GDBusMethodInvocation *invocation);
    GDBusMethodInvocation *m_pendingFrameAck = nullptr;

    void cleanupConnection();
    guint registerInterface(const gchar *introspectionXml, bool advertise);
    void startDispatching();

    GDBusConnection *m_listenerConn = nullptr;
    GMainContext *m_context = nullptr;
    GLibEventBridge *m_bridge = nullptr;
    guint m_registrationId = 0;
    guint m_registrationIdUnixMap = 0;
    guint m_registrationIdDmabuf2 = 0;

    // Optional interfaces reported to QEMU via the "Interfaces" property
    QByteArrayList m_extraInterfaces;

    QRegion m_dirtyRegion;

    int m_ourFd = -1;
    int m_socketFd = -1; // Stored FD for external monitoring

    QImage m_image;
    void *m_mappedMemory = nullptr;
    size_t m_mappedSize = 0;

    DmaBufScanout m_dmaBuf;

    CursorInfo m_cursor;
    bool m_closed = false;
};
