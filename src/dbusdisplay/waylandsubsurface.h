/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QImage>
#include <QList>
#include <QObject>
#include <QPair>
#include <QPoint>
#include <QRect>
#include <QRegion>
#include <QSize>

#include <cstdint>

#include "dbusdisplay/listener.h"

class QWindow;

struct wl_buffer;
struct wl_compositor;
struct wl_display;
struct wl_shm;
struct wl_subcompositor;
struct wl_subsurface;
struct wl_surface;
struct wp_viewport;
struct wp_viewporter;
struct zwp_linux_dmabuf_v1;

// =============================================================================
// WaylandSubSurface
// =============================================================================
// Presents guest frames through a Wayland sub-surface instead of copying them
// into the QOpenGLWidget's framebuffer.
//
// The guest's DMA-BUF is handed to the compositor as a wl_buffer via
// zwp_linux_dmabuf_v1, so no pixel ever passes through our process: the
// EGLImage import, the blit into the present texture and Qt's own widget
// composition all disappear. In fullscreen the frame sub-surface covers the
// output on its own, which lets KWin put it straight on a hardware plane
// (direct scanout).
//
// Surface tree:
//   toplevel (Qt)
//     +-- frame    (dmabuf, opaque, desync)   <- guest framebuffer
//           +-- cursor  (shm, sync)           <- guest cursor while captured
//           +-- overlay (shm, sync)           <- OSD message
//
// Cursor and overlay are children of the frame rather than of the toplevel
// because wl_subsurface.set_position only takes effect on the *parent's*
// commit. Hanging them off the frame lets our own commits move them; hanging
// them off Qt's toplevel would make every cursor step wait for a Qt repaint.
//
// Threading: all methods run on the GUI thread. The bound globals are moved
// to the default event queue after binding, so qtwayland dispatches their
// events on the GUI thread, exactly like WaylandPointerLock.
// =============================================================================

class WaylandSubSurface : public QObject
{
    Q_OBJECT

public:
    explicit WaylandSubSurface(QObject *parent = nullptr);
    ~WaylandSubSurface() override;

    // Binds the compositor globals and builds the surface tree under the
    // window's wl_surface. False means this path is unavailable (not Wayland,
    // no private QPA headers, or a missing global) - the caller keeps using
    // the OpenGL path.
    bool attachToWindow(QWindow *window);

    // Same, with the display and parent surface supplied directly. Used by
    // the tests, which drive the class without a QPA platform window.
    bool attach(wl_display *display, wl_surface *parentSurface);

    void detach();
    bool isAttached() const;

    // Whether the compositor can take the given DRM format/modifier pair as a
    // dmabuf. Unsupported pairs must not be submitted: the compositor answers
    // create_immed with a fatal protocol error.
    bool supportsFormat(uint32_t fourcc, uint64_t modifier) const;

    // Placement of the guest frame, in the parent surface's local (logical)
    // coordinates. 'content' is where the full frame would go, 'clip' the
    // visible area (the scroll viewport); the frame is cropped to their
    // intersection. Takes effect on the next commit of the Qt toplevel, so
    // the caller should trigger a widget update afterwards.
    void setGeometry(const QRect &content, const QRect &clip);

    // Shows one guest frame. False means the frame could not be presented;
    // the caller must then fall back to the OpenGL path for good, because a
    // mapped sub-surface would otherwise keep covering the widget.
    bool present(const DmaBufScanout &buf, const QRegion &damage);

    // Guest cursor, drawn above the frame while the pointer is captured.
    // 'position' is in guest framebuffer pixels, hotspot included.
    void setCursor(const QImage &image, const QPoint &hotspot, const QPoint &position, bool visible);

    // OSD message, mirrored from the Qt overlay widget. 'position' is in the
    // parent surface's local coordinates.
    void setOverlay(const QImage &image, const QPoint &position, bool visible);

    // Unmaps the frame (and with it the whole subtree) without tearing down.
    void hide();

    // Geometry of the last presented frame.
    struct Layout {
        uint32_t backingWidth = 0;
        uint32_t backingHeight = 0;
        uint32_t x = 0;
        uint32_t y = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        bool y0_top = true;
        bool valid = false;
    };

    // Source rectangle in wl_fixed (24.8) units.
    struct SourceRect {
        int32_t x = 0;
        int32_t y = 0;
        int32_t width = 256;
        int32_t height = 256;
    };

    // Maps the visible part of the widget back onto the buffer. Kept free of
    // Wayland state so it can be tested on its own: a source rectangle that
    // pokes a single 1/256th past the buffer edge is a fatal out_of_buffer
    // error that would take the whole process down.
    static SourceRect computeSourceRect(const Layout &layout, const QRect &content, const QRect &visible);

    static void onBufferRelease(void *data, wl_buffer *buffer);
    static void onDmaBufFormat(void *data, zwp_linux_dmabuf_v1 *dmabuf, uint32_t format);
    static void onDmaBufModifier(void *data, zwp_linux_dmabuf_v1 *dmabuf, uint32_t format, uint32_t modifierHi, uint32_t modifierLo);
    void handleGlobal(void *registry, uint32_t name, const char *interface, uint32_t version);

private:
    // One wl_surface plus the objects that always travel with it.
    struct Slot {
        wl_surface *surface = nullptr;
        wl_subsurface *subsurface = nullptr;
        wp_viewport *viewport = nullptr;
        bool mapped = false;
    };

    // A wl_buffer wrapping one guest DMA-BUF, cached across frames.
    //
    // Keyed on the dmabuf's inode: the kernel hands out a fresh fd number per
    // export but keeps one dma_buf (and thus one inode) per underlying GEM
    // object, so a guest that cycles through three render targets ends up
    // with three cached entries instead of one import per frame.
    struct CachedBuffer {
        wl_buffer *buffer = nullptr;
        uint64_t inode = 0;
        uint32_t fourcc = 0;
        uint64_t modifier = 0;
        int numPlanes = 0;
        uint32_t offsets[DmaBufScanout::kMaxPlanes] = {};
        uint32_t strides[DmaBufScanout::kMaxPlanes] = {};
        uint32_t width = 0;
        uint32_t height = 0;
        bool busy = false;
        uint64_t lastUsed = 0;
    };

    bool bindGlobals(wl_display *display);
    bool createSurfaces(wl_surface *parentSurface);
    void destroySurfaces();

    Slot createSlot(wl_surface *parentSurface, bool opaque);
    void destroySlot(Slot &slot);

    void applyGeometry();
    void unmapFrame();
    void commitFrame();
    QRect visibleRect() const;

    wl_buffer *bufferForScanout(const DmaBufScanout &buf);
    void releaseBufferCache();
    void dropBuffer(CachedBuffer &entry);

    // Uploads a QImage into a fresh shm wl_buffer. Returns nullptr on failure.
    wl_buffer *createShmBuffer(const QImage &image);

    void showImageSlot(Slot &slot, const QImage &image, const QPoint &position, qreal scale);
    void hideSlot(Slot &slot);

    wl_display *m_display = nullptr;
    wl_surface *m_parentSurface = nullptr;

    wl_compositor *m_compositor = nullptr;
    uint32_t m_compositorVersion = 0;
    wl_subcompositor *m_subcompositor = nullptr;
    wp_viewporter *m_viewporter = nullptr;
    wl_shm *m_shm = nullptr;
    zwp_linux_dmabuf_v1 *m_dmabuf = nullptr;

    Slot m_frame;
    Slot m_cursor;
    Slot m_overlay;

    Layout m_layout;
    QRect m_content;
    QRect m_clip;
    // Origin of the frame surface in the parent's coordinates. Cursor and
    // overlay are its children, so their positions are relative to it.
    QPoint m_frameOrigin;

    QList<CachedBuffer> m_bufferCache;
    uint64_t m_useCounter = 0;

    // (format, modifier) pairs the compositor advertised. Submitting anything
    // else is a fatal protocol error, so unknown pairs fall back instead.
    QList<QPair<uint32_t, uint64_t>> m_formats;

    wl_buffer *m_cursorBuffer = nullptr;
    wl_buffer *m_overlayBuffer = nullptr;
    qint64 m_cursorImageKey = -1;
    qint64 m_overlayImageKey = -1;

    bool m_transformSet = false;
};
