/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>
#include <QPointF>

struct wl_display;
struct wl_pointer;
struct wl_registry;
struct wl_surface;
struct zwp_locked_pointer_v1;
struct zwp_pointer_constraints_v1;
struct zwp_relative_pointer_manager_v1;
struct zwp_relative_pointer_v1;

class QWindow;

// =============================================================================
// WaylandPointerLock - real pointer capture via Wayland protocols
// =============================================================================
// Wraps zwp_pointer_constraints_v1 (lock_pointer) and
// zwp_relative_pointer_manager_v1. While locked the compositor freezes the
// visible cursor and stops sending wl_pointer.motion; movement arrives as
// unaccelerated deltas through relativeMotion() instead. Buttons, wheel and
// keyboard are unaffected, so an ungrab shortcut keeps working.
//
// The lock is requested with LIFETIME_PERSISTENT: the compositor may drop and
// re-establish it on focus changes without the client re-requesting. Watch
// lockedChanged() rather than assuming lock() means locked.
//
// Lives on the GUI thread. Wayland callbacks are dispatched from the default
// queue, which qtwayland pumps on the GUI thread, so the signals are direct.
// =============================================================================

class WaylandPointerLock : public QObject
{
    Q_OBJECT

public:
    explicit WaylandPointerLock(QObject *parent = nullptr);
    ~WaylandPointerLock() override;

    // False when not on Wayland or the compositor lacks either protocol.
    bool isSupported() const
    {
        return m_constraints && m_relativeManager;
    }

    // Whether the compositor has actually activated the lock.
    bool isLocked() const
    {
        return m_locked;
    }

    // Requests the lock for window's surface. Returns false if the request
    // could not be made at all; a true return does not mean locked yet.
    bool lock(QWindow *window);
    void unlock();

Q_SIGNALS:
    // Unaccelerated deltas in surface-local coordinates.
    void relativeMotion(double dx, double dy);
    // The compositor activated or dropped the lock.
    void lockedChanged(bool locked);

private:
    void bindGlobals();

    // Called from the wayland listener trampolines in the .cpp.
    void handleGlobal(wl_registry *registry, uint32_t name, const char *interface);
    void handleRelativeMotion(double dx, double dy);
    void setLocked(bool locked);

    friend struct WaylandPointerLockCallbacks;

    wl_display *m_display = nullptr;
    zwp_pointer_constraints_v1 *m_constraints = nullptr;
    zwp_relative_pointer_manager_v1 *m_relativeManager = nullptr;

    zwp_locked_pointer_v1 *m_lockedPointer = nullptr;
    zwp_relative_pointer_v1 *m_relativePointer = nullptr;

    bool m_locked = false;
};
