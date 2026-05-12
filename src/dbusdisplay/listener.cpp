// SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "listener.h"

#pragma push_macro("signals")
#undef signals
#include <gio/gunixfdlist.h>
#pragma pop_macro("signals")
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dbusdisplay/dbusdisplayconst.h"

// =============================================================================
// Pixman format constants
// =============================================================================
#define PIXMAN_FORMAT(bpp, type, a, r, g, b) (((bpp) << 24) | ((type) << 16) | ((a) << 12) | ((r) << 8) | ((g) << 4) | ((b)))

#define PIXMAN_TYPE_A 1
#define PIXMAN_TYPE_ARGB 2
#define PIXMAN_TYPE_ABGR 3
#define PIXMAN_TYPE_BGRA 8
#define PIXMAN_TYPE_RGBA 9

#define PIXMAN_x8r8g8b8 PIXMAN_FORMAT(32, PIXMAN_TYPE_ARGB, 0, 8, 8, 8)
#define PIXMAN_a8r8g8b8 PIXMAN_FORMAT(32, PIXMAN_TYPE_ARGB, 8, 8, 8, 8)
#define PIXMAN_x8b8g8r8 PIXMAN_FORMAT(32, PIXMAN_TYPE_ABGR, 0, 8, 8, 8)
#define PIXMAN_a8b8g8r8 PIXMAN_FORMAT(32, PIXMAN_TYPE_ABGR, 8, 8, 8, 8)
#define PIXMAN_b8g8r8x8 PIXMAN_FORMAT(32, PIXMAN_TYPE_BGRA, 0, 8, 8, 8)
#define PIXMAN_b8g8r8a8 PIXMAN_FORMAT(32, PIXMAN_TYPE_BGRA, 8, 8, 8, 8)
#define PIXMAN_r8g8b8x8 PIXMAN_FORMAT(32, PIXMAN_TYPE_RGBA, 0, 8, 8, 8)
#define PIXMAN_r8g8b8a8 PIXMAN_FORMAT(32, PIXMAN_TYPE_RGBA, 8, 8, 8, 8)
#define PIXMAN_r8g8b8 PIXMAN_FORMAT(24, PIXMAN_TYPE_ARGB, 0, 8, 8, 8)
#define PIXMAN_b8g8r8 PIXMAN_FORMAT(24, PIXMAN_TYPE_ABGR, 0, 8, 8, 8)
#define PIXMAN_r5g6b5 PIXMAN_FORMAT(16, PIXMAN_TYPE_ARGB, 0, 5, 6, 5)

// =============================================================================
// D-Bus Interface XML
// =============================================================================
static const gchar listenerIntrospectionXml[] =
    R"XML(
<node>
  <interface name="org.qemu.Display1.Listener">
    <method name="Scanout">
      <arg type="u" name="width" direction="in"/>
      <arg type="u" name="height" direction="in"/>
      <arg type="u" name="stride" direction="in"/>
      <arg type="u" name="pixman_format" direction="in"/>
      <arg type="ay" name="data" direction="in"/>
    </method>
    <method name="Update">
      <arg type="i" name="x" direction="in"/>
      <arg type="i" name="y" direction="in"/>
      <arg type="i" name="w" direction="in"/>
      <arg type="i" name="h" direction="in"/>
    </method>
    <method name="ScanoutDMABUF">
      <arg type="h" name="dmabuf" direction="in"/>
      <arg type="u" name="width" direction="in"/>
      <arg type="u" name="height" direction="in"/>
      <arg type="u" name="stride" direction="in"/>
      <arg type="u" name="fourcc" direction="in"/>
      <arg type="t" name="modifier" direction="in"/>
      <arg type="b" name="y0_top" direction="in"/>
    </method>
    <method name="UpdateDMABUF">
      <arg type="i" name="x" direction="in"/>
      <arg type="i" name="y" direction="in"/>
      <arg type="i" name="w" direction="in"/>
      <arg type="i" name="h" direction="in"/>
      </method>
      <method name="Disable"/>
      <method name="MouseSet">
      <arg type="i" name="x" direction="in"/>
      <arg type="i" name="y" direction="in"/>
      <arg type="i" name="on" direction="in"/>
      </method>
      <method name="CursorDefine">
      <arg type="i" name="width" direction="in"/>
      <arg type="i" name="height" direction="in"/>
      <arg type="i" name="hot_x" direction="in"/>
      <arg type="i" name="hot_y" direction="in"/>
      <arg type="ay" name="data" direction="in"/>
      </method>
      </interface>
      </node>
      )XML";

static const gchar listenerUnixIntrospectionXml[] =
    R"XML(
<node>
  <interface name="org.qemu.Display1.Listener.Unix">
    <method name="ScanoutMap">
      <arg type="h" name="handle" direction="in"/>
      <arg type="u" name="offset" direction="in"/>
      <arg type="u" name="width" direction="in"/>
      <arg type="u" name="height" direction="in"/>
      <arg type="u" name="stride" direction="in"/>
      <arg type="u" name="pixman_format" direction="in"/>
    </method>
    <method name="UpdateMap">
      <arg type="i" name="x" direction="in"/>
      <arg type="i" name="y" direction="in"/>
      <arg type="i" name="w" direction="in"/>
      <arg type="i" name="h" direction="in"/>
    </method>
  </interface>
</node>
)XML";

static const GDBusInterfaceVTable listenerVTable = {QemuDisplayListener::onMethodCall, nullptr, nullptr, {}};

// =============================================================================
// Constructor / Destructor
// =============================================================================

QemuDisplayListener::QemuDisplayListener(QObject *parent)
    : QObject(parent)
{
}

QemuDisplayListener::~QemuDisplayListener()
{
    m_closed = true;

    if (m_registrationId && m_listenerConn) {
        g_dbus_connection_unregister_object(m_listenerConn, m_registrationId);
        m_registrationId = 0;
    }
    if (m_registrationIdUnix && m_listenerConn) {
        g_dbus_connection_unregister_object(m_listenerConn, m_registrationIdUnix);
        m_registrationIdUnix = 0;
    }
    if (m_listenerConn) {
        g_dbus_connection_close_sync(m_listenerConn, nullptr, nullptr);
        g_object_unref(m_listenerConn);
        m_listenerConn = nullptr;
    }
    while (g_main_context_iteration(nullptr, FALSE)) { }

    unmapMemory();
    closeDmaBuf();
    if (m_qemuFd >= 0)
        close(m_qemuFd);
    if (m_ourFd >= 0)
        close(m_ourFd);
}

QImage QemuDisplayListener::currentImage() const
{
    const QMutexLocker l(&m_mutex);
    return m_image;
}
bool QemuDisplayListener::isDmaBuf() const
{
    const QMutexLocker l(&m_mutex);
    return m_isDmaBuf;
}
CursorInfo QemuDisplayListener::cursorInfo() const
{
    const QMutexLocker l(&m_mutex);
    return m_cursor;
}

bool QemuDisplayListener::takeDmaBufScanout(DmaBufScanout &out)
{
    const QMutexLocker l(&m_mutex);
    if (!m_isDmaBuf || m_dmaBuf.fd < 0)
        return false;
    out = m_dmaBuf;
    m_dmaBuf.fd = -1;
    return true;
}

// =============================================================================
// Socketpair + connection (identical to before)
// =============================================================================

int QemuDisplayListener::createSocketPair()
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        qCritical() << "socketpair():" << strerror(errno);
        return -1;
    }
    m_qemuFd = sv[0];
    m_ourFd = sv[1];
    return m_qemuFd;
}

bool QemuDisplayListener::connectListener()
{
    if (m_ourFd < 0)
        return false;
    const int ourFd = m_ourFd;
    m_ourFd = -1;
    m_socketFd = ourFd; // Remember FD number for QSocketNotifier

    GError *error = nullptr;
    GSocket *gsock = g_socket_new_from_fd(ourFd, &error);
    if (!gsock) {
        qCritical() << "g_socket_new_from_fd:" << error->message;
        g_error_free(error);
        close(ourFd);
        return false;
    }
    GSocketConnection *sockConn = g_socket_connection_factory_create_connection(gsock);
    g_object_unref(gsock);
    if (!sockConn)
        return false;

    m_listenerConn = g_dbus_connection_new_sync(G_IO_STREAM(sockConn), nullptr, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT, nullptr, nullptr, &error);
    g_object_unref(sockConn);
    if (!m_listenerConn) {
        qCritical() << "D-Bus Auth:" << error->message;
        g_error_free(error);
        return false;
    }

    g_dbus_connection_set_exit_on_close(m_listenerConn, FALSE);
    g_signal_connect(m_listenerConn, "closed", G_CALLBACK(QemuDisplayListener::onConnectionClosed), this);

    GDBusNodeInfo *nodeInfo = g_dbus_node_info_new_for_xml(listenerIntrospectionXml, &error);
    if (!nodeInfo) {
        qCritical() << error->message;
        g_error_free(error);
        return false;
    }
    m_registrationId = g_dbus_connection_register_object(m_listenerConn,
                                                         KemuDbusDisplayConst::ListenerPath.constData(),
                                                         nodeInfo->interfaces[0],
                                                         &listenerVTable,
                                                         this,
                                                         nullptr,
                                                         &error);
    g_dbus_node_info_unref(nodeInfo);
    if (!m_registrationId) {
        qCritical() << error->message;
        g_error_free(error);
        return false;
    }

    GDBusNodeInfo *unixNode = g_dbus_node_info_new_for_xml(listenerUnixIntrospectionXml, &error);
    if (unixNode) {
        m_registrationIdUnix = g_dbus_connection_register_object(m_listenerConn,
                                                                 KemuDbusDisplayConst::ListenerPath.constData(),
                                                                 unixNode->interfaces[0],
                                                                 &listenerVTable,
                                                                 this,
                                                                 nullptr,
                                                                 &error);
        g_dbus_node_info_unref(unixNode);
        if (!m_registrationIdUnix) {
            qWarning() << error->message;
            g_error_free(error);
        }
    }

    qDebug() << "Display listener ready";
    return true;
}

// =============================================================================
// Connection-Closed Handler
// =============================================================================
void QemuDisplayListener::onConnectionClosed(GDBusConnection *, gboolean remote_peer_vanished, GError *error, gpointer user_data)
{
    auto *self = static_cast<QemuDisplayListener *>(user_data);
    self->m_closed = true;
    qDebug() << "Display: Connection closed" << (remote_peer_vanished ? "(QEMU terminated)" : "(local)") << (error ? error->message : "");
    Q_EMIT self->connectionLost();
}

// =============================================================================
// Dispatcher
// =============================================================================

void QemuDisplayListener::onMethodCall(GDBusConnection *,
                                       const gchar *,
                                       const gchar *,
                                       const gchar *interface_name,
                                       const gchar *method_name,
                                       GVariant *parameters,
                                       GDBusMethodInvocation *invocation,
                                       gpointer user_data)
{
    auto *self = static_cast<QemuDisplayListener *>(user_data);
    if (self->m_closed) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_DISCONNECTED, "Connection closed");
        return;
    }

    const QString iface = QString::fromUtf8(interface_name);
    const QString method = QString::fromUtf8(method_name);

    if (iface == KemuDbusDisplayConst::ListenerIface) {
        if (method == u"Scanout"_s)
            self->handleScanout(parameters, invocation);
        else if (method == u"Update"_s)
            self->handleUpdate(parameters, invocation);
        else if (method == u"ScanoutDMABUF"_s)
            self->handleScanoutDMABUF(parameters, invocation);
        else if (method == u"UpdateDMABUF"_s)
            self->handleUpdateDMABUF(parameters, invocation);
        else if (method == u"Disable"_s)
            self->handleDisable(invocation);
        else if (method == u"MouseSet"_s)
            self->handleMouseSet(parameters, invocation);
        else if (method == u"CursorDefine"_s)
            self->handleCursorDefine(parameters, invocation);
        else
            g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown: %s", method_name);
    } else if (iface == KemuDbusDisplayConst::ListenerUnixIface) {
        if (method == u"ScanoutMap"_s)
            self->handleScanoutMap(parameters, invocation);
        else if (method == u"UpdateMap"_s)
            self->handleUpdateMap(parameters, invocation);
        else
            g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown: %s", method_name);
    }
}

// =============================================================================
// ScanoutDMABUF
// =============================================================================
void QemuDisplayListener::handleScanoutDMABUF(GVariant *parameters, GDBusMethodInvocation *invocation)
{
    guint32 fdIndex, width, height, stride, fourcc;
    guint64 modifier;
    gboolean y0_top;
    g_variant_get(parameters, "(huuuutb)", &fdIndex, &width, &height, &stride, &fourcc, &modifier, &y0_top);

    GDBusMessage *msg = g_dbus_method_invocation_get_message(invocation);
    GUnixFDList *fdList = g_dbus_message_get_unix_fd_list(msg);
    if (!fdList) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "No FD list");
        return;
    }
    GError *error = nullptr;
    const int dmabufFd = g_unix_fd_list_get(fdList, fdIndex, &error);
    if (dmabufFd < 0) {
        g_error_free(error);
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "FD error");
        return;
    }

    // qDebug() << "ScanoutDMABUF:" << width << "x" << height
    //          << "fourcc: 0x" << Qt::hex << fourcc << Qt::dec
    //          << "y0_top:" << y0_top << "FD:" << dmabufFd;

    {
        const QMutexLocker l(&m_mutex);
        unmapMemory();
        m_imageData.clear();
        m_image = QImage();
        closeDmaBuf();
        m_dmaBuf = {dmabufFd, width, height, stride, fourcc, modifier, bool(y0_top)};
        m_isDmaBuf = true;
    }
    g_dbus_method_invocation_return_value(invocation, nullptr);
    Q_EMIT dmaBufScanout(width, height);
}

void QemuDisplayListener::handleUpdateDMABUF(GVariant *parameters, GDBusMethodInvocation *invocation)
{
    gint32 x, y, w, h;
    g_variant_get(parameters, "(iiii)", &x, &y, &w, &h);
    g_dbus_method_invocation_return_value(invocation, nullptr);
    Q_EMIT regionUpdated(QRect(x, y, w, h));
}

// =============================================================================
// Scanout / ScanoutMap / Update
// =============================================================================
void QemuDisplayListener::handleScanout(GVariant *parameters, GDBusMethodInvocation *invocation)
{
    guint32 width, height, stride, pixman_format;
    GVariant *dataVariant = nullptr;
    g_variant_get(parameters, "(uuuu@ay)", &width, &height, &stride, &pixman_format, &dataVariant);
    gsize dataLen = 0;
    const guint8 *data = static_cast<const guint8 *>(g_variant_get_fixed_array(dataVariant, &dataLen, sizeof(guint8)));
    const QImage::Format qtFormat = pixmanFormatToQt(pixman_format);
    {
        const QMutexLocker l(&m_mutex);
        closeDmaBuf();
        unmapMemory();
        m_isDmaBuf = false;
        m_imageData = QByteArray(reinterpret_cast<const char *>(data), dataLen);
        m_image = QImage(reinterpret_cast<const uchar *>(m_imageData.constData()), width, height, stride, qtFormat);
    }
    g_variant_unref(dataVariant);
    g_dbus_method_invocation_return_value(invocation, nullptr);
    Q_EMIT scanoutChanged(width, height);
}

void QemuDisplayListener::handleScanoutMap(GVariant *parameters, GDBusMethodInvocation *invocation)
{
    guint32 fdIndex, offset, width, height, stride, pixman_format;
    g_variant_get(parameters, "(huuuuu)", &fdIndex, &offset, &width, &height, &stride, &pixman_format);
    GDBusMessage *msg = g_dbus_method_invocation_get_message(invocation);
    GUnixFDList *fdList = g_dbus_message_get_unix_fd_list(msg);
    if (!fdList) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "No FD list");
        return;
    }
    GError *error = nullptr;
    const int memFd = g_unix_fd_list_get(fdList, fdIndex, &error);
    if (memFd < 0) {
        g_error_free(error);
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "FD error");
        return;
    }
    const QImage::Format qtFormat = pixmanFormatToQt(pixman_format);
    const size_t mapSize = static_cast<size_t>(height) * stride + offset;
    void *mapped = mmap(nullptr, mapSize, PROT_READ, MAP_SHARED, memFd, 0);
    close(memFd);
    if (mapped == MAP_FAILED) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "mmap failed");
        return;
    }
    {
        const QMutexLocker l(&m_mutex);
        closeDmaBuf();
        unmapMemory();
        m_isDmaBuf = false;
        m_imageData.clear();
        m_mappedMemory = mapped;
        m_mappedSize = mapSize;
        const uchar *pixels = static_cast<const uchar *>(mapped) + offset;
        m_image = QImage(pixels, width, height, stride, qtFormat);
    }
    g_dbus_method_invocation_return_value(invocation, nullptr);
    Q_EMIT scanoutChanged(width, height);
}

void QemuDisplayListener::handleUpdate(GVariant *parameters, GDBusMethodInvocation *invocation)
{
    gint32 x, y, w, h;
    g_variant_get(parameters, "(iiii)", &x, &y, &w, &h);
    g_dbus_method_invocation_return_value(invocation, nullptr);
    Q_EMIT regionUpdated(QRect(x, y, w, h));
}

void QemuDisplayListener::handleUpdateMap(GVariant *parameters, GDBusMethodInvocation *invocation)
{
    handleUpdate(parameters, invocation);
}

// =============================================================================
// Disable
// =============================================================================
void QemuDisplayListener::handleDisable(GDBusMethodInvocation *invocation)
{
    {
        const QMutexLocker l(&m_mutex);
        closeDmaBuf();
        unmapMemory();
        m_imageData.clear();
        m_image = QImage();
        m_isDmaBuf = false;
    }
    g_dbus_method_invocation_return_value(invocation, nullptr);
    Q_EMIT scanoutChanged(0, 0);
}

// =============================================================================
// CursorDefine – Receive cursor image from guest
// =============================================================================
// QEMU sends the cursor image as ARGB data (32bpp, premultiplied alpha).
// With DMA-BUF, the cursor is NOT included in the framebuffer – it is a
// separate hardware cursor plane. We must render it ourselves.

void QemuDisplayListener::handleCursorDefine(GVariant *parameters, GDBusMethodInvocation *invocation)
{
    gint32 width, height, hot_x, hot_y;
    GVariant *dataVariant = nullptr;
    g_variant_get(parameters, "(iiii@ay)", &width, &height, &hot_x, &hot_y, &dataVariant);

    gsize dataLen = 0;
    const guint8 *data = static_cast<const guint8 *>(g_variant_get_fixed_array(dataVariant, &dataLen, sizeof(guint8)));

    // qDebug() << "CursorDefine:" << width << "x" << height
    //          << "hot:" << hot_x << "," << hot_y
    //          << "Data:" << dataLen << "bytes";

    {
        const QMutexLocker l(&m_mutex);
        if (data && dataLen >= static_cast<gsize>(width * height * 4)) {
            // QEMU sends ARGB32 data
            m_cursor.image = QImage(data, width, height, width * 4, QImage::Format_ARGB32).copy();
            m_cursor.hot_x = hot_x;
            m_cursor.hot_y = hot_y;
        }
    }

    g_variant_unref(dataVariant);
    g_dbus_method_invocation_return_value(invocation, nullptr);
    Q_EMIT cursorChanged();
}

// =============================================================================
// MouseSet – Cursor position and visibility from guest
// =============================================================================
void QemuDisplayListener::handleMouseSet(GVariant *parameters, GDBusMethodInvocation *invocation)
{
    gint32 x, y, on;
    g_variant_get(parameters, "(iii)", &x, &y, &on);

    {
        const QMutexLocker l(&m_mutex);
        m_cursor.pos_x = x;
        m_cursor.pos_y = y;
        m_cursor.visible = (on != 0);
    }

    g_dbus_method_invocation_return_value(invocation, nullptr);
    Q_EMIT cursorChanged();
}

// =============================================================================
// Helper functions
// =============================================================================

void QemuDisplayListener::unmapMemory()
{
    if (m_mappedMemory && m_mappedMemory != MAP_FAILED)
        munmap(m_mappedMemory, m_mappedSize);
    m_mappedMemory = nullptr;
    m_mappedSize = 0;
}

void QemuDisplayListener::closeDmaBuf()
{
    if (m_dmaBuf.fd >= 0) {
        close(m_dmaBuf.fd);
        m_dmaBuf.fd = -1;
    }
}

QImage::Format QemuDisplayListener::pixmanFormatToQt(uint32_t pf)
{
    switch (pf) {
    case PIXMAN_x8r8g8b8:
        return QImage::Format_RGB32;
    case PIXMAN_a8r8g8b8:
        return QImage::Format_ARGB32;
    case PIXMAN_x8b8g8r8:
        return QImage::Format_RGBX8888;
    case PIXMAN_a8b8g8r8:
        return QImage::Format_RGBA8888;
    case PIXMAN_b8g8r8x8:
        return QImage::Format_RGB32;
    case PIXMAN_b8g8r8a8:
        return QImage::Format_ARGB32;
    case PIXMAN_r8g8b8x8:
        return QImage::Format_RGBX8888;
    case PIXMAN_r8g8b8a8:
        return QImage::Format_RGBA8888;
    case PIXMAN_r8g8b8:
        return QImage::Format_RGB888;
    case PIXMAN_b8g8r8:
        return QImage::Format_BGR888;
    case PIXMAN_r5g6b5:
        return QImage::Format_RGB16;
    default:
        qWarning() << "Unknown Pixman format: 0x" << Qt::hex << pf;
        return QImage::Format_RGB32;
    }
}
