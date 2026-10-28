/*
 * SPDX-FileCopyrightText: 2026 Jin Liu <m.liu.jin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>

class QWindow;
class WaylandKeyboardShortcutsInhibitManager;

// =============================================================================
// WaylandKeyboardShortcutsInhibit - keyboard shortcuts inhibition via Wayland protocols
// =============================================================================
// Wraps zwp_keyboard_shortcuts_inhibit_manager_v1 and zwp_keyboard_shortcuts_inhibitor_v1.
// When inhibition is set for a surface, the compositor disables global keyboard shortcuts
// when that surface is focused.
// =============================================================================

class WaylandKeyboardShortcutsInhibit : public QObject
{
    Q_OBJECT

public:
    explicit WaylandKeyboardShortcutsInhibit(QObject *parent = nullptr);
    ~WaylandKeyboardShortcutsInhibit() override;

    // False when not on Wayland or the compositor lacks either protocol.
    bool isSupported() const;

    // True if an inhibitor is currently in place for the window's surface.
    bool hasInhibitor() const;

    // True if the compositor has actually activated the inhibitor.
    // False if !hasInhibitor() or the user has disabled keyboard shortcuts inhibition in the compositor.
    bool isInhibitorActive() const;

    // Requests inhibition for window's surface.
    // Returns false if the request could not be made at all, or an inhibitor
    // is already in place and the window is different from the argument.
    bool inhibit(QWindow *window);

    void uninhibit();

Q_SIGNALS:
    void supportedChanged(bool supported);
    void inhibitorActiveChanged(bool active);

private:
    WaylandKeyboardShortcutsInhibitManager *m_manager = nullptr;
};
