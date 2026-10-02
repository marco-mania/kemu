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
#include "dbusdisplay/gvariantptr.h"
#include "kemu_dbusdisplay_debug.h"

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
      <arg type="u" name="stride" direction="in"/>
      <arg type="u" name="pixman_format" direction="in"/>
      <arg type="ay" name="data" direction="in"/>
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
    <property name="Interfaces" type="as" access="read"/>
  </interface>
</node>
)XML";

static const gchar listenerUnixMapIntrospectionXml[] =
    R"XML(
<node>
  <interface name="org.qemu.Display1.Listener.Unix.Map">
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

static const gchar listenerDmabuf2IntrospectionXml[] =
    R"XML(
<node>
  <interface name="org.qemu.Display1.Listener.Unix.ScanoutDMABUF2">
    <method name="ScanoutDMABUF2">
      <arg type="ah" name="dmabuf" direction="in"/>
      <arg type="u" name="x" direction="in"/>
      <arg type="u" name="y" direction="in"/>
      <arg type="u" name="width" direction="in"/>
      <arg type="u" name="height" direction="in"/>
      <arg type="au" name="offset" direction="in"/>
      <arg type="au" name="stride" direction="in"/>
      <arg type="u" name="num_planes" direction="in"/>
      <arg type="u" name="fourcc" direction="in"/>
      <arg type="u" name="backing_width" direction="in"/>
      <arg type="u" name="backing_height" direction="in"/>
      <arg type="t" name="modifier" direction="in"/>
      <arg type="b" name="y0_top" direction="in"/>
    </method>
  </interface>
</node>
)XML";

static const GDBusInterfaceVTable listenerVTable = {QemuDisplayListener::onMethodCall, QemuDisplayListener::onGetProperty, nullptr, {}};

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

    cleanupConnection();

    unmapMemory();
    closeDmaBuf();
    if (m_ourFd >= 0)
        close(m_ourFd);
}

QImage QemuDisplayListener::currentImage() const
{
    return m_image;
}

QRegion QemuDisplayListener::takeDirtyRegion()
{
    QRegion r = std::move(m_dirtyRegion);
    m_dirtyRegion = QRegion();
    return r;
}

bool QemuDisplayListener::isDmaBuf() const
{
    return m_dmaBuf.numFds > 0 && m_dmaBuf.fds[0].isValid();
}

CursorInfo QemuDisplayListener::cursorInfo() const
{
    return m_cursor;
}

void QemuDisplayListener::ackPendingFrame()
{
    if (!m_pendingFrameAck)
        return;
    g_dbus_method_invocation_return_value(m_pendingFrameAck, nullptr);
    m_pendingFrameAck = nullptr;
}

bool QemuDisplayListener::takeDmaBufScanout(DmaBufScanout &out)
{
    if (!isDmaBuf())
        return false;

    out = std::move(m_dmaBuf);
    m_dmaBuf = DmaBufScanout();
    return true;
}

// =============================================================================
// Socketpair + connection (identical to before)
// =============================================================================

int QemuDisplayListener::createSocketPair()
{
    int sv[2];
    // CLOEXEC: neither end may leak into the processes KEMU spawns later
    // (QEMU for other VMs, swtpm, virtiofsd, passt). The end handed to QEMU
    // travels over SCM_RIGHTS, which is unaffected by the flag.
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
        qCCritical(KEMU_DBUSDISPLAY) << "socketpair():" << strerror(errno);
        return -1;
    }
    // Ownership of sv[0] transfers to the caller (which passes it to QEMU
    // via D-Bus and closes its local handle). We must NOT keep our own
    // reference to it - otherwise the destructor would double-close after
    // the kernel has potentially reassigned the FD number.
    m_ourFd = sv[1];
    return sv[0];
}

bool QemuDisplayListener::connectListener()
{
    if (m_ourFd < 0)
        return false;

    const int ourFd = m_ourFd;
    m_ourFd = -1;
    m_socketFd = ourFd;

    m_context = g_main_context_new();

    GError *error = nullptr;
    GSocket *gsock = g_socket_new_from_fd(ourFd, &error);
    if (!gsock) {
        qCCritical(KEMU_DBUSDISPLAY) << "Display: g_socket_new_from_fd:" << error->message;
        g_error_free(error);
        close(ourFd);
        cleanupConnection();
        return false;
    }

    GSocketConnection *sockConn = g_socket_connection_factory_create_connection(gsock);
    g_object_unref(gsock);
    if (!sockConn) {
        qCCritical(KEMU_DBUSDISPLAY) << "Display: Socket connection failed";
        cleanupConnection();
        return false;
    }

    // QEMU reads the Interfaces property right after the handshake, so hold
    // back all incoming messages until every interface is exported.
    const auto flags = static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_DELAY_MESSAGE_PROCESSING);

    g_main_context_push_thread_default(m_context);
    m_listenerConn = g_dbus_connection_new_sync(G_IO_STREAM(sockConn), nullptr, flags, nullptr, nullptr, &error);
    g_main_context_pop_thread_default(m_context);
    g_object_unref(sockConn);

    if (!m_listenerConn) {
        qCCritical(KEMU_DBUSDISPLAY) << "Display: D-Bus auth failed:" << error->message;
        g_error_free(error);
        cleanupConnection();
        return false;
    }

    g_dbus_connection_set_exit_on_close(m_listenerConn, FALSE);
    g_signal_connect(m_listenerConn, "closed", G_CALLBACK(QemuDisplayListener::onConnectionClosed), this);

    m_extraInterfaces.clear();
    m_registrationId = registerInterface(listenerIntrospectionXml, false);
    if (!m_registrationId) {
        cleanupConnection();
        return false;
    }

    // Optional: without them QEMU falls back to the base interface.
    m_registrationIdUnixMap = registerInterface(listenerUnixMapIntrospectionXml, true);
    m_registrationIdDmabuf2 = registerInterface(listenerDmabuf2IntrospectionXml, true);

    g_dbus_connection_start_message_processing(m_listenerConn);
    startDispatching();

    qCDebug(KEMU_DBUSDISPLAY) << "Display listener ready, extra interfaces:" << m_extraInterfaces;
    return true;
}

guint QemuDisplayListener::registerInterface(const gchar *introspectionXml, bool advertise)
{
    GError *error = nullptr;
    GDBusNodeInfo *nodeInfo = g_dbus_node_info_new_for_xml(introspectionXml, &error);
    if (!nodeInfo) {
        qCCritical(KEMU_DBUSDISPLAY) << "Display: introspection XML error:" << error->message;
        g_error_free(error);
        return 0;
    }

    // Method calls are dispatched in the thread-default context at
    // registration time; bind them to m_context, not the global default.
    GDBusInterfaceInfo *info = nodeInfo->interfaces[0];
    g_main_context_push_thread_default(m_context);
    const guint id = g_dbus_connection_register_object(m_listenerConn,
                                                       KemuDbusDisplayConst::ListenerPath.constData(),
                                                       info,
                                                       &listenerVTable,
                                                       this,
                                                       nullptr,
                                                       &error);
    g_main_context_pop_thread_default(m_context);
    if (!id) {
        qCWarning(KEMU_DBUSDISPLAY) << "Display: registering" << info->name << "failed:" << error->message;
        g_error_free(error);
    } else if (advertise) {
        m_extraInterfaces.append(QByteArray(info->name));
    }

    g_dbus_node_info_unref(nodeInfo);
    return id;
}

void QemuDisplayListener::startDispatching()
{
    // Deferred to the event loop: dispatching inside connectListener() would
    // emit the initial scanout before the caller has connected its widget.
    QMetaObject::invokeMethod(
        this,
        [this] {
            if (!m_bridge && m_context && !m_closed)
                m_bridge = new GLibEventBridge(m_context, this);
        },
        Qt::QueuedConnection);
}

// =============================================================================
// Connection-Closed Handler
// =============================================================================
void QemuDisplayListener::onConnectionClosed(GDBusConnection *, gboolean remote_peer_vanished, GError *error, gpointer user_data)
{
    auto *self = static_cast<QemuDisplayListener *>(user_data);
    self->m_closed = true;
    if (self->m_bridge)
        self->m_bridge->stop();
    qCDebug(KEMU_DBUSDISPLAY) << "Display: Connection closed" << (remote_peer_vanished ? "(QEMU terminated)" : "(local)") << (error ? error->message : "");
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
    } else if (iface == KemuDbusDisplayConst::ListenerUnixMapIface) {
        if (method == u"ScanoutMap"_s)
            self->handleScanoutMap(parameters, invocation);
        else if (method == u"UpdateMap"_s)
            self->handleUpdateMap(parameters, invocation);
        else
            g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown: %s", method_name);
    } else if (iface == KemuDbusDisplayConst::ListenerScanoutDmabuf2Iface) {
        if (method == u"ScanoutDMABUF2"_s)
            self->handleScanoutDMABUF2(parameters, invocation);
        else
            g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown: %s", method_name);
    } else {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_INTERFACE, "Unknown: %s", interface_name);
    }
}

GVariant *QemuDisplayListener::onGetProperty(GDBusConnection *,
                                             const gchar *,
                                             const gchar *,
                                             const gchar *interface_name,
                                             const gchar *property_name,
                                             GError **error,
                                             gpointer user_data)
{
    auto *self = static_cast<QemuDisplayListener *>(user_data);

    if (QLatin1StringView(interface_name) == KemuDbusDisplayConst::ListenerIface && g_strcmp0(property_name, "Interfaces") == 0) {
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE_STRING_ARRAY);
        for (const QByteArray &name : std::as_const(self->m_extraInterfaces))
            g_variant_builder_add(&builder, "s", name.constData());
        return g_variant_builder_end(&builder);
    }

    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "Unknown property %s.%s", interface_name, property_name);
    return nullptr;
}

// Takes ownership of a duplicate of the fd at index in the message's fd list.
static UniqueFd takeInvocationFd(GDBusMethodInvocation *invocation, gint32 index)
{
    GUnixFDList *fdList = g_dbus_message_get_unix_fd_list(g_dbus_method_invocation_get_message(invocation));
    if (!fdList || index < 0 || index >= g_unix_fd_list_get_length(fdList))
        return UniqueFd();

    GError *error = nullptr;
    UniqueFd fd(g_unix_fd_list_get(fdList, index, &error));
    if (!fd.isValid()) {
        qCWarning(KEMU_DBUSDISPLAY) << "DMA-BUF: fd extraction failed:" << (error ? error->message : "unknown");
        g_clear_error(&error);
    }
    return fd;
}

// =============================================================================
// ScanoutDMABUF
// =============================================================================
void QemuDisplayListener::handleScanoutDMABUF(GVariant *parameters, GDBusMethodInvocation *invocation)
{
    ackPendingFrame();
    gint32 fdIndex;
    guint32 width, height, stride, fourcc;
    guint64 modifier;
    gboolean y0_top;
    g_variant_get(parameters, "(huuuutb)", &fdIndex, &width, &height, &stride, &fourcc, &modifier, &y0_top);

    DmaBufScanout buf;
    buf.fds[0] = takeInvocationFd(invocation, fdIndex);
    if (!buf.fds[0].isValid()) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "FD error");
        return;
    }

    // v1 carries no sub-rect: the visible area is the whole buffer.
    buf.numFds = 1;
    buf.numPlanes = 1;
    buf.strides[0] = stride;
    buf.width = buf.backingWidth = width;
    buf.height = buf.backingHeight = height;
    buf.fourcc = fourcc;
    buf.modifier = modifier;
    buf.y0_top = bool(y0_top);

    acceptDmaBuf(std::move(buf), invocation);
}

void QemuDisplayListener::handleScanoutDMABUF2(GVariant *parameters, GDBusMethodInvocation *invocation)
{
    ackPendingFrame();
    GVariant *rawHandles = nullptr;
    GVariant *rawOffsets = nullptr;
    GVariant *rawStrides = nullptr;
    guint32 x, y, width, height, numPlanes, fourcc, backingWidth, backingHeight;
    guint64 modifier;
    gboolean y0_top;
    g_variant_get(parameters,
                  "(@ahuuuu@au@auuuuutb)",
                  &rawHandles,
                  &x,
                  &y,
                  &width,
                  &height,
                  &rawOffsets,
                  &rawStrides,
                  &numPlanes,
                  &fourcc,
                  &backingWidth,
                  &backingHeight,
                  &modifier,
                  &y0_top);
    const GVariantPtr handlesVariant(rawHandles);
    const GVariantPtr offsetsVariant(rawOffsets);
    const GVariantPtr stridesVariant(rawStrides);

    gsize nHandles = 0, nOffsets = 0, nStrides = 0;
    const auto *handles = static_cast<const gint32 *>(g_variant_get_fixed_array(handlesVariant.get(), &nHandles, sizeof(gint32)));
    const auto *offsets = static_cast<const guint32 *>(g_variant_get_fixed_array(offsetsVariant.get(), &nOffsets, sizeof(guint32)));
    const auto *strides = static_cast<const guint32 *>(g_variant_get_fixed_array(stridesVariant.get(), &nStrides, sizeof(guint32)));

    if (numPlanes < 1 || numPlanes > guint32(DmaBufScanout::kMaxPlanes) || nHandles < 1 || nHandles > numPlanes || nOffsets < numPlanes
        || nStrides < numPlanes || width == 0 || height == 0 || x >= backingWidth || y >= backingHeight) {
        qCWarning(KEMU_DBUSDISPLAY) << "ScanoutDMABUF2: invalid arguments: planes" << numPlanes << "fds" << nHandles << "rect" << x << y << width << height
                                    << "backing" << backingWidth << backingHeight;
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "Invalid ScanoutDMABUF2 arguments");
        return;
    }

    DmaBufScanout buf;
    for (gsize i = 0; i < nHandles; ++i) {
        buf.fds[i] = takeInvocationFd(invocation, handles[i]);
        if (!buf.fds[i].isValid()) {
            g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "FD error");
            return;
        }
    }
    buf.numFds = int(nHandles);
    buf.numPlanes = int(numPlanes);
    for (guint32 i = 0; i < numPlanes; ++i) {
        buf.offsets[i] = offsets[i];
        buf.strides[i] = strides[i];
    }

    buf.x = x;
    buf.y = y;
    buf.width = std::min(width, backingWidth - x);
    buf.height = std::min(height, backingHeight - y);
    buf.backingWidth = backingWidth;
    buf.backingHeight = backingHeight;
    buf.fourcc = fourcc;
    buf.modifier = modifier;
    buf.y0_top = bool(y0_top);

    acceptDmaBuf(std::move(buf), invocation);
}

void QemuDisplayListener::acceptDmaBuf(DmaBufScanout &&buf, GDBusMethodInvocation *invocation)
{
    // Drop previous state: any old DMA-BUF fds and any software mapping.
    unmapMemory();
    m_image = {};
    m_dmaBuf = std::move(buf);

    const int width = int(m_dmaBuf.width);
    const int height = int(m_dmaBuf.height);

    g_dbus_method_invocation_return_value(invocation, nullptr);
    Q_EMIT dmaBufScanout(width, height);
}

void QemuDisplayListener::handleUpdateDMABUF(GVariant *parameters, GDBusMethodInvocation *invocation)
{
    gint32 x, y, w, h;
    g_variant_get(parameters, "(iiii)", &x, &y, &w, &h);
    deferFrameAck(invocation);
    Q_EMIT regionUpdated(QRect(x, y, w, h));
}

// =============================================================================
// Scanout / ScanoutMap / Update
// =============================================================================
void QemuDisplayListener::handleScanout(GVariant *parameters, GDBusMethodInvocation *invocation)
{
    ackPendingFrame();
    guint32 width, height, stride, pixman_format;
    GVariant *rawData = nullptr;
    g_variant_get(parameters, "(uuuu@ay)", &width, &height, &stride, &pixman_format, &rawData);
    GVariantPtr dataVariant(rawData); // RAII (from Punkt 6)

    const QImage::Format qtFormat = pixmanFormatToQt(pixman_format);
    if (qtFormat == QImage::Format_Invalid) {
        qCWarning(KEMU_DBUSDISPLAY) << "Scanout: unsupported pixman format" << pixman_format;
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED, "unsupported pixel format");
        return;
    }

    gsize dataLen = 0;
    const guint8 *data = static_cast<const guint8 *>(g_variant_get_fixed_array(dataVariant.get(), &dataLen, sizeof(guint8)));
    const gsize expected = static_cast<gsize>(stride) * height;
    if (!data || dataLen < expected) {
        qCWarning(KEMU_DBUSDISPLAY) << "Scanout: short pixel array" << dataLen << "<" << expected;
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "short pixel array");
        return;
    }

    // Drop any previous state (DMA-BUF or ScanoutMap mapping). After
    // unmapMemory() m_image is null - we re-create it next.
    closeDmaBuf();
    unmapMemory();

    // Reuse the existing backbuffer if geometry AND format match.
    // In steady state (constant resolution, constant format) this
    // branch is never taken, so no per-frame allocation.
    if (m_image.size() != QSize(int(width), int(height)) || m_image.format() != qtFormat) {
        m_image = QImage(int(width), int(height), qtFormat);
    }

    // memcpy row-by-row. Source and destination strides may differ
    // (QImage rounds up to 32-bit alignment), so we cannot blast the
    // whole buffer in one memcpy. dstStride is needed because
    // m_image.scanLine(y) already accounts for it - but only for the
    // destination side. We still need srcStride to walk the input.
    const size_t srcStride = stride;
    const int bpp = m_image.depth() / 8;
    const size_t rowBytes = static_cast<size_t>(width) * bpp;

    if (srcStride == static_cast<size_t>(m_image.bytesPerLine())) {
        memcpy(m_image.bits(), data, srcStride * height);
    } else {
        for (uint32_t y = 0; y < height; ++y) {
            memcpy(m_image.scanLine(static_cast<int>(y)), data + y * srcStride, rowBytes);
        }
    }

    // Full-frame change.
    m_dirtyRegion = QRegion(0, 0, int(width), int(height));

    g_dbus_method_invocation_return_value(invocation, nullptr);
    Q_EMIT scanoutChanged(int(width), int(height));
}

void QemuDisplayListener::handleScanoutMap(GVariant *parameters, GDBusMethodInvocation *invocation)
{
    ackPendingFrame();
    guint32 fdIndex, offset, width, height, stride, pixman_format;
    g_variant_get(parameters, "(huuuuu)", &fdIndex, &offset, &width, &height, &stride, &pixman_format);
    GDBusMessage *msg = g_dbus_method_invocation_get_message(invocation);
    GUnixFDList *fdList = g_dbus_message_get_unix_fd_list(msg);
    if (!fdList) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "No FD list");
        return;
    }
    GError *error = nullptr;
    // Take ownership immediately, as handleScanoutDMABUF() does: every return
    // path below then frees the descriptor.
    UniqueFd memFd(g_unix_fd_list_get(fdList, fdIndex, &error));
    if (!memFd.isValid()) {
        if (error)
            g_error_free(error);
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "FD error");
        return;
    }

    const QImage::Format qtFormat = pixmanFormatToQt(pixman_format);
    if (qtFormat == QImage::Format_Invalid) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED, "Unsupported pixman format: 0x%x", pixman_format);
        return;
    }

    const size_t mapSize = static_cast<size_t>(height) * stride + offset;
    void *mapped = mmap(nullptr, mapSize, PROT_READ, MAP_SHARED, memFd.get(), 0);
    memFd.reset(); // the mapping stays valid without the descriptor
    if (mapped == MAP_FAILED) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "mmap failed");
        return;
    }

    // Drop previous frame/mapping. unmapMemory() handles the munmap of any
    // existing m_mappedMemory and resets m_image; closeDmaBuf() releases any
    // previous DMA-BUF FD.
    closeDmaBuf();
    unmapMemory();

    // Keep the mapping alive: m_image is now a NON-OWNING view over QEMU's
    // shared framebuffer. Subsequent UpdateMap calls write into this memory
    // from the QEMU side; paintGL reads from it directly. The mapping stays
    // alive until the next Scanout/ScanoutMap/Disable or destruction.
    m_mappedMemory = mapped;
    m_mappedSize = mapSize;

    const uchar *pixels = static_cast<const uchar *>(mapped) + offset;
    m_image = QImage(pixels, width, height, stride, qtFormat);

    // Full-frame change - force a complete texture upload.
    m_dirtyRegion = QRegion(0, 0, int(width), int(height));

    g_dbus_method_invocation_return_value(invocation, nullptr);
    Q_EMIT scanoutChanged(width, height);
}

void QemuDisplayListener::handleUpdate(GVariant *parameters, GDBusMethodInvocation *invocation)
{
    gint32 x, y, w, h;
    guint32 stride, pixman_format;
    GVariant *rawData = nullptr;
    g_variant_get(parameters, "(iiiiuu@ay)", &x, &y, &w, &h, &stride, &pixman_format, &rawData);
    GVariantPtr dataVariant(rawData);

    // Update is only valid in the non-mapped Scanout path. ScanoutMap
    // sends UpdateMap, which is handled separately and writes nothing.
    if (m_mappedMemory) {
        qCDebug(KEMU_DBUSDISPLAY) << "Update while ScanoutMap active - ignoring";
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
    }

    const QImage::Format qtFormat = pixmanFormatToQt(pixman_format);
    if (m_image.isNull() || m_image.format() != qtFormat) {
        // No matching backbuffer to update into. QEMU will resync via a
        // full Scanout if needed.
        qCDebug(KEMU_DBUSDISPLAY) << "Update without matching backbuffer - ignoring";
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
    }

    // Clip rect to backbuffer (defensive against bogus coordinates).
    const int dstX = std::max(0, x);
    const int dstY = std::max(0, y);
    const int copyW = std::min(w, m_image.width() - dstX);
    const int copyH = std::min(h, m_image.height() - dstY);

    if (copyW > 0 && copyH > 0) {
        gsize dataLen = 0;
        const guint8 *data = static_cast<const guint8 *>(g_variant_get_fixed_array(dataVariant.get(), &dataLen, sizeof(guint8)));
        const gsize expected = static_cast<gsize>(stride) * copyH;
        if (data && dataLen >= expected) {
            const int bpp = m_image.depth() / 8;
            const size_t rowBytes = static_cast<size_t>(copyW) * bpp;
            const size_t srcStride = stride;

            for (int row = 0; row < copyH; ++row) {
                memcpy(m_image.scanLine(dstY + row) + dstX * bpp, data + row * srcStride, rowBytes);
            }

            addDirtyRect(QRect(dstX, dstY, copyW, copyH));
        } else {
            qCWarning(KEMU_DBUSDISPLAY) << "Update: short pixel array";
        }
    }

    g_dbus_method_invocation_return_value(invocation, nullptr);
    Q_EMIT regionUpdated(QRect(x, y, w, h));
}

void QemuDisplayListener::handleUpdateMap(GVariant *parameters, GDBusMethodInvocation *invocation)
{
    // UpdateMap signals that QEMU has written into the existing scanout
    // mapping at rect (x, y, w, h). No pixel data on the wire - we already
    // see the new bytes via m_image's non-owning view.
    gint32 x, y, w, h;
    g_variant_get(parameters, "(iiii)", &x, &y, &w, &h);

    // The pixels are already visible through the shared mapping, but the
    // GPU upload still needs to know which rect changed.
    addDirtyRect(QRect(x, y, w, h));

    // QEMU fires UpdateMap without waiting for the reply, so deferring it
    // would throttle nothing.
    g_dbus_method_invocation_return_value(invocation, nullptr);
    Q_EMIT regionUpdated(QRect(x, y, w, h));
}

// =============================================================================
// Disable
// =============================================================================
void QemuDisplayListener::handleDisable(GDBusMethodInvocation *invocation)
{
    ackPendingFrame();
    closeDmaBuf();
    unmapMemory();
    m_image = {}; // no framebuffer at all until the next Scanout
    m_dirtyRegion = QRegion();

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
    gint32 width = 0, height = 0, hot_x = 0, hot_y = 0;
    GVariant *rawData = nullptr;
    g_variant_get(parameters, "(iiii@ay)", &width, &height, &hot_x, &hot_y, &rawData);
    const GVariantPtr dataVariant(rawData); // takes ownership

    // Reject implausible geometry before it reaches the size arithmetic below,
    // which would otherwise overflow gint32 and pass the length check.
    constexpr gint32 kMaxCursorDim = 512;
    if (width <= 0 || height <= 0 || width > kMaxCursorDim || height > kMaxCursorDim) {
        qCWarning(KEMU_DBUSDISPLAY) << "CursorDefine: implausible size" << width << "x" << height << "- ignoring";
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
    }

    gsize dataLen = 0;
    const guint8 *data = static_cast<const guint8 *>(g_variant_get_fixed_array(dataVariant.get(), &dataLen, sizeof(guint8)));

    const gsize needed = static_cast<gsize>(width) * static_cast<gsize>(height) * 4u;
    if (!data || dataLen < needed) {
        qCWarning(KEMU_DBUSDISPLAY) << "CursorDefine: short pixel array" << dataLen << "<" << needed;
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
    }

    // QEMU sends ARGB32 data; copy() detaches from the GVariant's storage.
    m_cursor.image = QImage(data, width, height, width * 4, QImage::Format_ARGB32).copy();
    m_cursor.hot_x = hot_x;
    m_cursor.hot_y = hot_y;

    g_dbus_method_invocation_return_value(invocation, nullptr);
    Q_EMIT cursorImageChanged();
}

// =============================================================================
// MouseSet – Cursor position and visibility from guest
// =============================================================================
void QemuDisplayListener::handleMouseSet(GVariant *parameters, GDBusMethodInvocation *invocation)
{
    gint32 x, y, on;
    g_variant_get(parameters, "(iii)", &x, &y, &on);

    {
        m_cursor.pos_x = x;
        m_cursor.pos_y = y;
        m_cursor.visible = (on != 0);
    }

    g_dbus_method_invocation_return_value(invocation, nullptr);
    Q_EMIT cursorMoved();
}

// =============================================================================
// Helper functions
// =============================================================================

void QemuDisplayListener::addDirtyRect(const QRect &r)
{
    // QRegion unions are O(n) in the rect count; collapse before it degenerates.
    constexpr int kMaxDirtyRects = 32;
    if (m_dirtyRegion.rectCount() >= kMaxDirtyRects)
        m_dirtyRegion = QRegion(m_dirtyRegion.boundingRect());
    m_dirtyRegion += r;
}

void QemuDisplayListener::unmapMemory()
{
    if (m_mappedMemory) {
        munmap(m_mappedMemory, m_mappedSize);
        m_mappedMemory = nullptr;
        m_mappedSize = 0;
        m_image = {}; // was a non-owning view into the mapping
    }
}

void QemuDisplayListener::closeDmaBuf()
{
    m_dmaBuf = DmaBufScanout();
}

void QemuDisplayListener::deferFrameAck(GDBusMethodInvocation *invocation)
{
    // QEMU throttles on the reply, so at most one frame is in flight.
    // Release a stale one first, just in case.
    ackPendingFrame();
    m_pendingFrameAck = invocation;
}

QImage::Format QemuDisplayListener::pixmanFormatToQt(uint32_t pf)
{
    // Pixman names refer to bit positions (high-to-low). Qt formats refer to
    // byte order in memory. The conversions below are correct on
    // little-endian hosts only, which covers every supported QEMU/KDE host.
    //
    // Mappings we trust:
    //   PIXMAN_x8r8g8b8 (0x__RRGGBB) -> LE bytes B,G,R,X -> Format_RGB32
    //   PIXMAN_a8r8g8b8 (0xAARRGGBB) -> LE bytes B,G,R,A -> Format_ARGB32
    //   PIXMAN_x8b8g8r8 (0x__BBGGRR) -> LE bytes R,G,B,X -> Format_RGBX8888
    //   PIXMAN_a8b8g8r8 (0xAABBGGRR) -> LE bytes R,G,B,A -> Format_RGBA8888
    //
    // QEMU sends PIXMAN_x8r8g8b8 in well over 99% of cases. Anything else
    // we reject explicitly rather than guessing - the old fall-through to
    // Format_RGB32 produced silently scrambled colors for some inputs.
    switch (pf) {
    case PIXMAN_x8r8g8b8:
        return QImage::Format_RGB32;
    case PIXMAN_a8r8g8b8:
        return QImage::Format_ARGB32;
    case PIXMAN_x8b8g8r8:
        return QImage::Format_RGBX8888;
    case PIXMAN_a8b8g8r8:
        return QImage::Format_RGBA8888;
    default:
        qCWarning(KEMU_DBUSDISPLAY) << "Unsupported pixman format: 0x" << Qt::hex << pf << Qt::dec << "- ignoring frame.";
        return QImage::Format_Invalid;
    }
}

void QemuDisplayListener::cleanupConnection()
{
    if (m_bridge)
        m_bridge->stop();
    ackPendingFrame();
    if (m_registrationIdDmabuf2 && m_listenerConn) {
        g_dbus_connection_unregister_object(m_listenerConn, m_registrationIdDmabuf2);
        m_registrationIdDmabuf2 = 0;
    }
    if (m_registrationIdUnixMap && m_listenerConn) {
        g_dbus_connection_unregister_object(m_listenerConn, m_registrationIdUnixMap);
        m_registrationIdUnixMap = 0;
    }
    if (m_registrationId && m_listenerConn) {
        g_dbus_connection_unregister_object(m_listenerConn, m_registrationId);
        m_registrationId = 0;
    }
    if (m_listenerConn) {
        g_dbus_connection_close_sync(m_listenerConn, nullptr, nullptr);
        g_object_unref(m_listenerConn);
        m_listenerConn = nullptr;
    }
    if (m_context) {
        g_main_context_unref(m_context);
        m_context = nullptr;
    }
}
