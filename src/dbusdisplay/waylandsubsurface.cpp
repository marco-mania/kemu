/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "waylandsubsurface.h"

#ifdef KEMU_HAVE_QT_GUI_PRIVATE

#include <QGuiApplication>
#include <QWindow>

#include <qpa/qplatformwindow_p.h>

#include <wayland-client.h>

#include "wayland-linux-dmabuf-v1-client-protocol.h"
#include "wayland-viewporter-client-protocol.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>

#include "kemu_dbusdisplay_debug.h"

namespace
{

// Keep a few frames' worth of imported buffers. Guests cycle through two or
// three render targets; beyond that the extra entries only pin memory.
constexpr int kMaxCachedBuffers = 8;

// wl_surface.damage_buffer and set_buffer_transform need wl_compositor v4.
constexpr uint32_t kCompositorVersion = 4;

// v3 still sends the format/modifier events. v4 deprecates them in favour of
// dmabuf feedback, which we do not need: QEMU dictates the format and we only
// want to know whether the compositor can take it.
constexpr uint32_t kDmaBufVersion = 3;

// Clamps one axis of the source rectangle in wl_fixed units. A rectangle that
// pokes a single 1/256th past the buffer edge is a fatal out_of_buffer error,
// and rounding a double into fixed point is exactly how that happens.
void clampAxis(double position, double extent, int32_t limit, int32_t &outPos, int32_t &outSize)
{
    int32_t pos = wl_fixed_from_double(position);
    int32_t size = wl_fixed_from_double(extent);

    if (size < 256)
        size = 256; // one pixel minimum
    if (size > limit)
        size = limit;
    if (pos < 0)
        pos = 0;
    if (pos > limit - size)
        pos = limit - size;

    outPos = pos;
    outSize = size;
}

void registryGlobal(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    static_cast<WaylandSubSurface *>(data)->handleGlobal(registry, name, interface, version);
}

void registryGlobalRemove(void *, wl_registry *, uint32_t)
{
    // The registry is dropped after the initial roundtrip; the globals we
    // hold stay valid until the connection goes away.
}

const struct wl_registry_listener s_registryListener = {
    &registryGlobal,
    &registryGlobalRemove,
};

const struct wl_buffer_listener s_bufferListener = {
    &WaylandSubSurface::onBufferRelease,
};

const struct zwp_linux_dmabuf_v1_listener s_dmabufListener = {
    &WaylandSubSurface::onDmaBufFormat,
    &WaylandSubSurface::onDmaBufModifier,
};

} // namespace

// =============================================================================
// Construction
// =============================================================================

WaylandSubSurface::WaylandSubSurface(QObject *parent)
    : QObject(parent)
{
}

WaylandSubSurface::~WaylandSubSurface()
{
    detach();

    if (m_dmabuf)
        zwp_linux_dmabuf_v1_destroy(m_dmabuf);
    if (m_viewporter)
        wp_viewporter_destroy(m_viewporter);
    if (m_subcompositor)
        wl_subcompositor_destroy(m_subcompositor);
    if (m_compositor)
        wl_compositor_destroy(m_compositor);
    if (m_shm)
        wl_shm_destroy(m_shm);
}

bool WaylandSubSurface::isAttached() const
{
    return m_frame.surface != nullptr;
}

// =============================================================================
// Globals
// =============================================================================

void WaylandSubSurface::handleGlobal(void *registryPtr, uint32_t name, const char *interface, uint32_t version)
{
    auto *registry = static_cast<wl_registry *>(registryPtr);

    if (qstrcmp(interface, wl_compositor_interface.name) == 0) {
        m_compositorVersion = qMin(version, kCompositorVersion);
        m_compositor = static_cast<wl_compositor *>(wl_registry_bind(registry, name, &wl_compositor_interface, m_compositorVersion));
    } else if (qstrcmp(interface, wl_subcompositor_interface.name) == 0) {
        m_subcompositor = static_cast<wl_subcompositor *>(wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
    } else if (qstrcmp(interface, wp_viewporter_interface.name) == 0) {
        m_viewporter = static_cast<wp_viewporter *>(wl_registry_bind(registry, name, &wp_viewporter_interface, 1));
    } else if (qstrcmp(interface, wl_shm_interface.name) == 0) {
        m_shm = static_cast<wl_shm *>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (qstrcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0 && version >= kDmaBufVersion) {
        m_dmabuf = static_cast<zwp_linux_dmabuf_v1 *>(wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, kDmaBufVersion));
        zwp_linux_dmabuf_v1_add_listener(m_dmabuf, &s_dmabufListener, this);
    }
}

void WaylandSubSurface::onDmaBufFormat(void *data, zwp_linux_dmabuf_v1 *, uint32_t format)
{
    // A bare format event means the format works with an implicit layout.
    static_cast<WaylandSubSurface *>(data)->m_formats.append({format, DRM_FORMAT_MOD_INVALID});
}

void WaylandSubSurface::onDmaBufModifier(void *data, zwp_linux_dmabuf_v1 *, uint32_t format, uint32_t modifierHi, uint32_t modifierLo)
{
    const uint64_t modifier = (uint64_t(modifierHi) << 32) | modifierLo;
    static_cast<WaylandSubSurface *>(data)->m_formats.append({format, modifier});
}

bool WaylandSubSurface::bindGlobals(wl_display *display)
{
    // Bind on a private queue: a roundtrip on the default queue would dispatch
    // Qt's own events from inside this call. The bound objects move to the
    // default queue afterwards, so qtwayland pumps them on the GUI thread.
    wl_event_queue *queue = wl_display_create_queue(display);
    auto *wrapped = static_cast<wl_display *>(wl_proxy_create_wrapper(display));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrapped), queue);

    wl_registry *registry = wl_display_get_registry(wrapped);
    wl_proxy_wrapper_destroy(wrapped);

    wl_registry_add_listener(registry, &s_registryListener, this);

    // Two roundtrips: the first delivers the globals, the second the
    // format/modifier events the dmabuf global emits right after binding.
    wl_display_roundtrip_queue(display, queue);
    wl_display_roundtrip_queue(display, queue);

    for (wl_proxy *proxy : {reinterpret_cast<wl_proxy *>(m_compositor),
                            reinterpret_cast<wl_proxy *>(m_subcompositor),
                            reinterpret_cast<wl_proxy *>(m_viewporter),
                            reinterpret_cast<wl_proxy *>(m_shm),
                            reinterpret_cast<wl_proxy *>(m_dmabuf)}) {
        if (proxy)
            wl_proxy_set_queue(proxy, nullptr);
    }

    wl_registry_destroy(registry);
    wl_event_queue_destroy(queue);

    if (!m_compositor || !m_subcompositor || !m_viewporter || !m_shm) {
        qCDebug(KEMU_DBUSDISPLAY) << "SubSurface: missing global - compositor:" << (m_compositor != nullptr) << "subcompositor:" << (m_subcompositor != nullptr)
                                  << "viewporter:" << (m_viewporter != nullptr) << "shm:" << (m_shm != nullptr);
        return false;
    }

    // A compositor without linux-dmabuf is not an error here: present() then
    // refuses the first frame and the caller falls back to the OpenGL path
    // with a logged reason instead of showing an empty sub-surface.
    if (!m_dmabuf)
        qCDebug(KEMU_DBUSDISPLAY) << "SubSurface: compositor has no zwp_linux_dmabuf_v1";

    return true;
}

bool WaylandSubSurface::supportsFormat(uint32_t fourcc, uint64_t modifier) const
{
    bool formatKnown = false;
    for (const auto &pair : m_formats) {
        if (pair.first != fourcc)
            continue;
        formatKnown = true;
        if (pair.second == modifier)
            return true;
    }

    // Without an explicit modifier the format alone decides.
    return formatKnown && modifier == DRM_FORMAT_MOD_INVALID;
}

// =============================================================================
// Surface tree
// =============================================================================

WaylandSubSurface::Slot WaylandSubSurface::createSlot(wl_surface *parentSurface, bool opaque)
{
    Slot slot;
    slot.surface = wl_compositor_create_surface(m_compositor);
    if (!slot.surface)
        return slot;

    slot.subsurface = wl_subcompositor_get_subsurface(m_subcompositor, slot.surface, parentSurface);
    slot.viewport = wp_viewporter_get_viewport(m_viewporter, slot.surface);

    // Input must keep going to the Qt window: an empty input region makes the
    // sub-surface invisible to the pointer and to focus handling.
    if (wl_region *empty = wl_compositor_create_region(m_compositor)) {
        wl_surface_set_input_region(slot.surface, empty);
        wl_region_destroy(empty);
    }

    if (opaque) {
        // Declaring the frame opaque lets the compositor skip everything
        // behind it - and is what makes direct scanout possible at all.
        if (wl_region *full = wl_compositor_create_region(m_compositor)) {
            wl_region_add(full, 0, 0, INT32_MAX, INT32_MAX);
            wl_surface_set_opaque_region(slot.surface, full);
            wl_region_destroy(full);
        }
    }

    return slot;
}

bool WaylandSubSurface::createSurfaces(wl_surface *parentSurface)
{
    m_frame = createSlot(parentSurface, true);
    if (!m_frame.surface || !m_frame.subsurface || !m_frame.viewport)
        return false;

    // Desync: our frames must reach the screen on our own commits instead of
    // waiting for Qt to repaint the toplevel.
    wl_subsurface_set_desync(m_frame.subsurface);
    wl_subsurface_place_above(m_frame.subsurface, parentSurface);

    // Children of the frame, so their position updates ride on our commits.
    // wl_subsurface.set_position only ever takes effect on the parent's
    // commit; hanging these off Qt's toplevel would make every cursor step
    // wait for a Qt repaint. They stay in synchronised mode, which also makes
    // cursor movement and frame content apply atomically.
    m_cursor = createSlot(m_frame.surface, false);
    m_overlay = createSlot(m_frame.surface, false);
    if (!m_cursor.surface || !m_overlay.surface)
        return false;

    wl_subsurface_place_above(m_cursor.subsurface, m_frame.surface);
    wl_subsurface_place_above(m_overlay.subsurface, m_cursor.surface);

    return true;
}

bool WaylandSubSurface::attach(wl_display *display, wl_surface *parentSurface)
{
    if (!display || !parentSurface)
        return false;
    if (m_frame.surface)
        detach();

    m_display = display;
    m_parentSurface = parentSurface;

    if (!m_compositor && !bindGlobals(display)) {
        m_display = nullptr;
        m_parentSurface = nullptr;
        return false;
    }

    if (!createSurfaces(parentSurface)) {
        qCWarning(KEMU_DBUSDISPLAY) << "SubSurface: could not build the surface tree";
        destroySurfaces();
        m_display = nullptr;
        m_parentSurface = nullptr;
        return false;
    }

    qCDebug(KEMU_DBUSDISPLAY) << "SubSurface: active," << m_formats.size() << "dmabuf format/modifier pairs advertised";
    return true;
}

bool WaylandSubSurface::attachToWindow(QWindow *window)
{
    if (!window)
        return false;

    auto *app = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    auto *native = window->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
    if (!app || !native) {
        qCDebug(KEMU_DBUSDISPLAY) << "SubSurface: not running on Wayland";
        return false;
    }

    wl_surface *parentSurface = native->surface();
    if (!parentSurface) {
        qCDebug(KEMU_DBUSDISPLAY) << "SubSurface: window has no surface yet";
        return false;
    }

    if (!attach(app->display(), parentSurface))
        return false;

    // Hiding or reparenting the window destroys the surface we hang off.
    connect(native, &QNativeInterface::Private::QWaylandWindow::surfaceDestroyed, this, &WaylandSubSurface::detach, Qt::UniqueConnection);
    return true;
}

void WaylandSubSurface::destroySlot(Slot &slot)
{
    if (slot.viewport)
        wp_viewport_destroy(slot.viewport);
    if (slot.subsurface)
        wl_subsurface_destroy(slot.subsurface);
    if (slot.surface)
        wl_surface_destroy(slot.surface);
    slot = Slot();
}

void WaylandSubSurface::destroySurfaces()
{
    // Children first: destroying a parent surface before its sub-surfaces
    // leaves orphaned wl_subsurface objects behind.
    destroySlot(m_overlay);
    destroySlot(m_cursor);
    destroySlot(m_frame);
}

void WaylandSubSurface::detach()
{
    if (!m_frame.surface && m_bufferCache.isEmpty())
        return;

    destroySurfaces();
    releaseBufferCache();

    if (m_cursorBuffer) {
        wl_buffer_destroy(m_cursorBuffer);
        m_cursorBuffer = nullptr;
    }
    if (m_overlayBuffer) {
        wl_buffer_destroy(m_overlayBuffer);
        m_overlayBuffer = nullptr;
    }
    m_cursorImageKey = -1;
    m_overlayImageKey = -1;
    m_layout = Layout();
    m_transformSet = false;

    if (m_display)
        wl_display_flush(m_display);

    m_parentSurface = nullptr;
}

// =============================================================================
// Geometry
// =============================================================================

void WaylandSubSurface::setGeometry(const QRect &content, const QRect &clip)
{
    if (m_content == content && m_clip == clip)
        return;
    m_content = content;
    m_clip = clip;
    applyGeometry();
    commitFrame();
}

QRect WaylandSubSurface::visibleRect() const
{
    if (m_content.isEmpty())
        return {};
    return m_clip.isEmpty() ? m_content : (m_content & m_clip);
}

void WaylandSubSurface::unmapFrame()
{
    if (!m_frame.surface || !m_frame.mapped)
        return;
    wl_surface_attach(m_frame.surface, nullptr, 0, 0);
    wl_surface_commit(m_frame.surface);
    m_frame.mapped = false;
    if (m_display)
        wl_display_flush(m_display);
}

void WaylandSubSurface::commitFrame()
{
    if (!m_frame.surface)
        return;
    // Also applies everything the synchronised children have queued.
    wl_surface_commit(m_frame.surface);
    if (m_display)
        wl_display_flush(m_display);
}

WaylandSubSurface::SourceRect WaylandSubSurface::computeSourceRect(const Layout &layout, const QRect &content, const QRect &visible)
{
    SourceRect src;
    if (content.isEmpty() || visible.isEmpty() || layout.width == 0 || layout.height == 0)
        return src;

    // Logical pixels -> guest framebuffer pixels.
    const double sx = double(layout.width) / double(content.width());
    const double sy = double(layout.height) / double(content.height());

    const double srcW = visible.width() * sx;
    const double srcH = visible.height() * sy;
    double srcX = layout.x + (visible.x() - content.x()) * sx;
    double srcY = layout.y + (visible.y() - content.y()) * sy;

    // A bottom-up buffer is flipped with a buffer transform, and the source
    // rectangle is read in post-transform coordinates, so mirror y as well.
    if (!layout.y0_top)
        srcY = double(layout.backingHeight) - (srcY + srcH);

    clampAxis(srcX, srcW, wl_fixed_from_int(int(layout.backingWidth)), src.x, src.width);
    clampAxis(srcY, srcH, wl_fixed_from_int(int(layout.backingHeight)), src.y, src.height);
    return src;
}

void WaylandSubSurface::applyGeometry()
{
    if (!m_frame.surface || !m_layout.valid || m_content.isEmpty())
        return;

    const QRect visible = visibleRect();
    if (visible.isEmpty()) {
        unmapFrame();
        return;
    }

    const SourceRect src = computeSourceRect(m_layout, m_content, visible);
    wp_viewport_set_source(m_frame.viewport, src.x, src.y, src.width, src.height);
    wp_viewport_set_destination(m_frame.viewport, visible.width(), visible.height());

    // set_position is applied on the *parent's* commit, so the move itself
    // lands with the next Qt repaint; the viewport state is ours.
    wl_subsurface_set_position(m_frame.subsurface, visible.x(), visible.y());
    m_frameOrigin = visible.topLeft();
}

// =============================================================================
// DMA-BUF buffers
// =============================================================================

void WaylandSubSurface::onBufferRelease(void *data, wl_buffer *buffer)
{
    auto *self = static_cast<WaylandSubSurface *>(data);
    for (auto &entry : self->m_bufferCache) {
        if (entry.buffer == buffer) {
            entry.busy = false;
            return;
        }
    }
}

void WaylandSubSurface::dropBuffer(CachedBuffer &entry)
{
    if (entry.buffer)
        wl_buffer_destroy(entry.buffer);
    entry.buffer = nullptr;
}

void WaylandSubSurface::releaseBufferCache()
{
    for (auto &entry : m_bufferCache)
        dropBuffer(entry);
    m_bufferCache.clear();
}

wl_buffer *WaylandSubSurface::bufferForScanout(const DmaBufScanout &buf)
{
    // The dmabuf's inode identifies the underlying GEM object across the fresh
    // fd numbers QEMU hands out on every frame: the kernel keeps one dma_buf
    // per exported object, so a triple-buffering guest settles on three
    // cached wl_buffers instead of one import per frame.
    struct stat st = {};
    uint64_t inode = 0;
    if (fstat(buf.planeFd(0), &st) == 0)
        inode = uint64_t(st.st_ino);

    if (inode != 0) {
        for (auto &entry : m_bufferCache) {
            if (entry.inode != inode || entry.fourcc != buf.fourcc || entry.modifier != buf.modifier || entry.numPlanes != buf.numPlanes
                || entry.width != buf.backingWidth || entry.height != buf.backingHeight) {
                continue;
            }
            bool sameLayout = true;
            for (int p = 0; p < buf.numPlanes; ++p)
                sameLayout = sameLayout && entry.offsets[p] == buf.offsets[p] && entry.strides[p] == buf.strides[p];
            if (!sameLayout)
                continue;

            entry.lastUsed = ++m_useCounter;
            return entry.buffer;
        }
    }

    zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(m_dmabuf);
    if (!params)
        return nullptr;

    for (int p = 0; p < buf.numPlanes; ++p) {
        zwp_linux_buffer_params_v1_add(params,
                                       buf.planeFd(p),
                                       uint32_t(p),
                                       buf.offsets[p],
                                       buf.strides[p],
                                       uint32_t(buf.modifier >> 32),
                                       uint32_t(buf.modifier & 0xffffffff));
    }

    // y0_top is handled with a buffer transform rather than the Y_INVERT flag:
    // the flag interacts badly with viewporter source rectangles and several
    // compositors ignore it outright.
    wl_buffer *buffer = zwp_linux_buffer_params_v1_create_immed(params, int(buf.backingWidth), int(buf.backingHeight), buf.fourcc, 0);
    zwp_linux_buffer_params_v1_destroy(params);

    if (!buffer)
        return nullptr;

    wl_buffer_add_listener(buffer, &s_bufferListener, this);

    // Evict the least recently used entry that is not on screen.
    while (m_bufferCache.size() >= kMaxCachedBuffers) {
        int victim = -1;
        for (int i = 0; i < m_bufferCache.size(); ++i) {
            if (m_bufferCache[i].busy)
                continue;
            if (victim < 0 || m_bufferCache[i].lastUsed < m_bufferCache[victim].lastUsed)
                victim = i;
        }
        if (victim < 0)
            break; // everything still in flight; let the cache grow this once
        dropBuffer(m_bufferCache[victim]);
        m_bufferCache.removeAt(victim);
    }

    CachedBuffer entry;
    entry.buffer = buffer;
    entry.inode = inode;
    entry.fourcc = buf.fourcc;
    entry.modifier = buf.modifier;
    entry.numPlanes = buf.numPlanes;
    entry.width = buf.backingWidth;
    entry.height = buf.backingHeight;
    for (int p = 0; p < buf.numPlanes; ++p) {
        entry.offsets[p] = buf.offsets[p];
        entry.strides[p] = buf.strides[p];
    }
    entry.lastUsed = ++m_useCounter;
    m_bufferCache.append(entry);

    return buffer;
}

bool WaylandSubSurface::present(const DmaBufScanout &buf, const QRegion &damage)
{
    if (!m_frame.surface || !m_dmabuf)
        return false;
    if (buf.numPlanes <= 0 || buf.numPlanes > DmaBufScanout::kMaxPlanes || buf.planeFd(0) < 0)
        return false;
    if (buf.backingWidth == 0 || buf.backingHeight == 0 || buf.width == 0 || buf.height == 0)
        return false;
    if (buf.x + buf.width > buf.backingWidth || buf.y + buf.height > buf.backingHeight)
        return false;

    // Submitting a pair the compositor never advertised can be answered with a
    // fatal protocol error, so refuse here and let the caller fall back.
    if (!supportsFormat(buf.fourcc, buf.modifier)) {
        qCWarning(KEMU_DBUSDISPLAY).nospace() << "SubSurface: compositor does not accept fourcc=0x" << Qt::hex << buf.fourcc << " modifier=0x" << buf.modifier;
        return false;
    }

    wl_buffer *buffer = bufferForScanout(buf);
    if (!buffer) {
        qCWarning(KEMU_DBUSDISPLAY) << "SubSurface: wl_buffer creation failed";
        return false;
    }

    m_layout.backingWidth = buf.backingWidth;
    m_layout.backingHeight = buf.backingHeight;
    m_layout.x = buf.x;
    m_layout.y = buf.y;
    m_layout.width = buf.width;
    m_layout.height = buf.height;
    m_layout.y0_top = buf.y0_top;
    m_layout.valid = true;

    if (!buf.y0_top && !m_transformSet) {
        wl_surface_set_buffer_transform(m_frame.surface, WL_OUTPUT_TRANSFORM_FLIPPED_180);
        m_transformSet = true;
        qCDebug(KEMU_DBUSDISPLAY) << "SubSurface: bottom-up buffer, using a flipped buffer transform";
    }

    if (visibleRect().isEmpty()) {
        unmapFrame();
        return true; // nothing on screen right now, but the frame was accepted
    }

    for (auto &entry : m_bufferCache) {
        if (entry.buffer == buffer)
            entry.busy = true;
    }

    wl_surface_attach(m_frame.surface, buffer, 0, 0);

    if (m_compositorVersion < 4) {
        wl_surface_damage(m_frame.surface, 0, 0, INT32_MAX, INT32_MAX);
    } else if (buf.y0_top && !damage.isEmpty()) {
        // Damage is in guest framebuffer pixels; the buffer may be larger.
        for (const QRect &r : damage)
            wl_surface_damage_buffer(m_frame.surface, int(buf.x) + r.x(), int(buf.y) + r.y(), r.width(), r.height());
    } else {
        wl_surface_damage_buffer(m_frame.surface, 0, 0, INT32_MAX, INT32_MAX);
    }

    applyGeometry();
    commitFrame();
    m_frame.mapped = true;

    return true;
}

void WaylandSubSurface::hide()
{
    hideSlot(m_cursor);
    hideSlot(m_overlay);
    unmapFrame();
}

// =============================================================================
// Shared-memory surfaces (cursor, OSD)
// =============================================================================

wl_buffer *WaylandSubSurface::createShmBuffer(const QImage &source)
{
    if (source.isNull() || !m_shm)
        return nullptr;

    // wl_shm ARGB8888 is premultiplied and little-endian, which is byte for
    // byte QImage::Format_ARGB32_Premultiplied on the platforms we run on.
    const QImage image = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (image.isNull())
        return nullptr;

    const int stride = image.width() * 4;
    const size_t size = size_t(stride) * size_t(image.height());
    if (size == 0)
        return nullptr;

    const int fd = memfd_create("kemu-wl-shm", MFD_CLOEXEC);
    if (fd < 0)
        return nullptr;
    if (ftruncate(fd, off_t(size)) != 0) {
        close(fd);
        return nullptr;
    }

    void *data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return nullptr;
    }

    auto *dst = static_cast<uchar *>(data);
    for (int y = 0; y < image.height(); ++y)
        memcpy(dst + size_t(y) * size_t(stride), image.constScanLine(y), size_t(stride));
    munmap(data, size);

    wl_shm_pool *pool = wl_shm_create_pool(m_shm, fd, int32_t(size));
    close(fd);
    if (!pool)
        return nullptr;

    wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, image.width(), image.height(), stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);

    return buffer;
}

void WaylandSubSurface::hideSlot(Slot &slot)
{
    if (!slot.surface || !slot.mapped)
        return;
    wl_surface_attach(slot.surface, nullptr, 0, 0);
    wl_surface_commit(slot.surface);
    slot.mapped = false;
}

void WaylandSubSurface::showImageSlot(Slot &slot, const QImage &image, const QPoint &position, qreal scale)
{
    if (!slot.surface)
        return;

    wl_subsurface_set_position(slot.subsurface, position.x(), position.y());

    // The image is in device pixels; the viewport maps it onto the logical
    // size the rest of the geometry is expressed in.
    const int logicalW = qMax(1, qRound(image.width() * scale));
    const int logicalH = qMax(1, qRound(image.height() * scale));
    wp_viewport_set_destination(slot.viewport, logicalW, logicalH);

    if (m_compositorVersion >= 4)
        wl_surface_damage_buffer(slot.surface, 0, 0, INT32_MAX, INT32_MAX);
    else
        wl_surface_damage(slot.surface, 0, 0, INT32_MAX, INT32_MAX);

    wl_surface_commit(slot.surface);
    slot.mapped = true;
}

void WaylandSubSurface::setCursor(const QImage &image, const QPoint &hotspot, const QPoint &position, bool visible)
{
    if (!m_cursor.surface)
        return;

    if (!visible || image.isNull() || !m_layout.valid || m_content.isEmpty()) {
        hideSlot(m_cursor);
        commitFrame();
        return;
    }

    if (image.cacheKey() != m_cursorImageKey) {
        wl_buffer *buffer = createShmBuffer(image);
        if (!buffer) {
            hideSlot(m_cursor);
            commitFrame();
            return;
        }
        if (m_cursorBuffer)
            wl_buffer_destroy(m_cursorBuffer);
        m_cursorBuffer = buffer;
        m_cursorImageKey = image.cacheKey();
        wl_surface_attach(m_cursor.surface, m_cursorBuffer, 0, 0);
    }

    // Guest framebuffer pixels -> logical pixels, then into the frame
    // surface's own coordinate system (its origin is the visible rect).
    const double sx = double(m_content.width()) / double(m_layout.width);
    const double sy = double(m_content.height()) / double(m_layout.height);

    const QPoint topLeft(m_content.x() + qRound((position.x() - hotspot.x()) * sx) - m_frameOrigin.x(),
                         m_content.y() + qRound((position.y() - hotspot.y()) * sy) - m_frameOrigin.y());

    showImageSlot(m_cursor, image, topLeft, sx);
    commitFrame();
}

void WaylandSubSurface::setOverlay(const QImage &image, const QPoint &position, bool visible)
{
    if (!m_overlay.surface)
        return;

    if (!visible || image.isNull()) {
        hideSlot(m_overlay);
        commitFrame();
        return;
    }

    if (image.cacheKey() != m_overlayImageKey) {
        wl_buffer *buffer = createShmBuffer(image);
        if (!buffer) {
            hideSlot(m_overlay);
            commitFrame();
            return;
        }
        if (m_overlayBuffer)
            wl_buffer_destroy(m_overlayBuffer);
        m_overlayBuffer = buffer;
        m_overlayImageKey = image.cacheKey();
        wl_surface_attach(m_overlay.surface, m_overlayBuffer, 0, 0);
    }

    showImageSlot(m_overlay, image, position - m_frameOrigin, 1.0);
    commitFrame();
}

#else // !KEMU_HAVE_QT_GUI_PRIVATE

// Reaching a QWindow's wl_surface requires Qt's private QPA headers, which
// are not available in this build (see the top-level CMakeLists.txt), so the
// zero-copy path is compiled out. attachToWindow() reports failure and the
// display widget keeps using its OpenGL pipeline.

WaylandSubSurface::WaylandSubSurface(QObject *parent)
    : QObject(parent)
{
}

WaylandSubSurface::~WaylandSubSurface() = default;

bool WaylandSubSurface::attachToWindow(QWindow *)
{
    return false;
}

bool WaylandSubSurface::attach(wl_display *, wl_surface *)
{
    return false;
}

void WaylandSubSurface::detach()
{
}

bool WaylandSubSurface::isAttached() const
{
    return false;
}

bool WaylandSubSurface::supportsFormat(uint32_t, uint64_t) const
{
    return false;
}

void WaylandSubSurface::setGeometry(const QRect &, const QRect &)
{
}

bool WaylandSubSurface::present(const DmaBufScanout &, const QRegion &)
{
    return false;
}

void WaylandSubSurface::setCursor(const QImage &, const QPoint &, const QPoint &, bool)
{
}

void WaylandSubSurface::setOverlay(const QImage &, const QPoint &, bool)
{
}

void WaylandSubSurface::hide()
{
}

#endif // KEMU_HAVE_QT_GUI_PRIVATE
