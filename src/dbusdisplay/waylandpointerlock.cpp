/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "waylandpointerlock.h"

#include <QGuiApplication>
#include <QWindow>

#include <qpa/qplatformwindow_p.h>

#include <wayland-client.h>

#include "wayland-pointer-constraints-unstable-v1-client-protocol.h"
#include "wayland-relative-pointer-unstable-v1-client-protocol.h"

#include "kemu_dbusdisplay_debug.h"

struct WaylandPointerLockCallbacks {
    static void global(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
    {
        Q_UNUSED(version)
        static_cast<WaylandPointerLock *>(data)->handleGlobal(registry, name, interface);
    }
    static void globalRemove(void *, wl_registry *, uint32_t)
    {
        // The registry is dropped after the initial roundtrip; the globals we
        // hold stay valid until the connection goes away.
    }
    static void locked(void *data, zwp_locked_pointer_v1 *)
    {
        static_cast<WaylandPointerLock *>(data)->setLocked(true);
    }
    static void unlocked(void *data, zwp_locked_pointer_v1 *)
    {
        // Compositor dropped the constraint (focus loss, user action). With a
        // persistent lifetime it may come back on its own.
        static_cast<WaylandPointerLock *>(data)->setLocked(false);
    }
    static void relativeMotion(void *data, zwp_relative_pointer_v1 *, uint32_t, uint32_t, wl_fixed_t, wl_fixed_t, wl_fixed_t dxUnaccel, wl_fixed_t dyUnaccel)
    {
        // Unaccelerated deltas: the guest applies its own acceleration curve,
        // so the accelerated pair would apply the host curve twice.
        static_cast<WaylandPointerLock *>(data)->handleRelativeMotion(wl_fixed_to_double(dxUnaccel), wl_fixed_to_double(dyUnaccel));
    }
};

static const wl_registry_listener s_registryListener = {
    &WaylandPointerLockCallbacks::global,
    &WaylandPointerLockCallbacks::globalRemove,
};

static const zwp_locked_pointer_v1_listener s_lockedPointerListener = {
    &WaylandPointerLockCallbacks::locked,
    &WaylandPointerLockCallbacks::unlocked,
};

static const zwp_relative_pointer_v1_listener s_relativePointerListener = {
    &WaylandPointerLockCallbacks::relativeMotion,
};

WaylandPointerLock::WaylandPointerLock(QObject *parent)
    : QObject(parent)
{
    auto *app = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    if (!app) {
        qCDebug(KEMU_DBUSDISPLAY) << "PointerLock: not running on Wayland";
        return;
    }
    m_display = app->display();
    if (m_display)
        bindGlobals();

    if (!isSupported())
        qCWarning(KEMU_DBUSDISPLAY) << "PointerLock: compositor lacks pointer-constraints or relative-pointer";
}

WaylandPointerLock::~WaylandPointerLock()
{
    unlock();
    if (m_relativeManager)
        zwp_relative_pointer_manager_v1_destroy(m_relativeManager);
    if (m_constraints)
        zwp_pointer_constraints_v1_destroy(m_constraints);
}

void WaylandPointerLock::bindGlobals()
{
    // Bind on a private queue: a roundtrip on the default queue would dispatch
    // Qt's own events from inside this constructor. The bound objects are
    // handed back to the default queue afterwards, so qtwayland pumps them on
    // the GUI thread and our listeners fire there.
    wl_event_queue *queue = wl_display_create_queue(m_display);
    auto *wrapped = static_cast<wl_display *>(wl_proxy_create_wrapper(m_display));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrapped), queue);

    wl_registry *registry = wl_display_get_registry(wrapped);
    wl_proxy_wrapper_destroy(wrapped);

    wl_registry_add_listener(registry, &s_registryListener, this);
    wl_display_roundtrip_queue(m_display, queue);

    if (m_constraints)
        wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(m_constraints), nullptr);
    if (m_relativeManager)
        wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(m_relativeManager), nullptr);

    wl_registry_destroy(registry);
    wl_event_queue_destroy(queue);
}

void WaylandPointerLock::handleGlobal(wl_registry *registry, uint32_t name, const char *interface)
{
    if (qstrcmp(interface, zwp_pointer_constraints_v1_interface.name) == 0) {
        m_constraints = static_cast<zwp_pointer_constraints_v1 *>(wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, 1));
    } else if (qstrcmp(interface, zwp_relative_pointer_manager_v1_interface.name) == 0) {
        m_relativeManager = static_cast<zwp_relative_pointer_manager_v1 *>(wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1));
    }
}

void WaylandPointerLock::handleRelativeMotion(double dx, double dy)
{
    Q_EMIT relativeMotion(dx, dy);
}

bool WaylandPointerLock::lock(QWindow *window)
{
    if (!isSupported() || !window)
        return false;
    if (m_lockedPointer)
        return true; // already requested

    auto *app = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    auto *native = window->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
    if (!app || !native)
        return false;

    wl_pointer *pointer = app->pointer();
    wl_surface *surface = native->surface();
    if (!pointer || !surface) {
        qCWarning(KEMU_DBUSDISPLAY) << "PointerLock: no pointer or surface available";
        return false;
    }

    m_relativePointer = zwp_relative_pointer_manager_v1_get_relative_pointer(m_relativeManager, pointer);
    zwp_relative_pointer_v1_add_listener(m_relativePointer, &s_relativePointerListener, this);

    // Null region: the whole surface. PERSISTENT so the lock re-engages by
    // itself after a focus change instead of silently staying dead.
    m_lockedPointer = zwp_pointer_constraints_v1_lock_pointer(m_constraints, surface, pointer, nullptr, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    zwp_locked_pointer_v1_add_listener(m_lockedPointer, &s_lockedPointerListener, this);

    // Hiding or reparenting the window destroys the surface under the lock.
    connect(native, &QNativeInterface::Private::QWaylandWindow::surfaceDestroyed, this, &WaylandPointerLock::unlock, Qt::UniqueConnection);

    wl_display_flush(m_display);
    return true;
}

void WaylandPointerLock::unlock()
{
    if (m_lockedPointer) {
        zwp_locked_pointer_v1_destroy(m_lockedPointer);
        m_lockedPointer = nullptr;
    }
    if (m_relativePointer) {
        zwp_relative_pointer_v1_destroy(m_relativePointer);
        m_relativePointer = nullptr;
    }
    setLocked(false);

    if (m_display)
        wl_display_flush(m_display);
}

void WaylandPointerLock::setLocked(bool locked)
{
    if (m_locked == locked)
        return;
    m_locked = locked;
    Q_EMIT lockedChanged(locked);
}
