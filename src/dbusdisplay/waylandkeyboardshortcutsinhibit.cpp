/*
 * SPDX-FileCopyrightText: 2026 Jin Liu <m.liu.jin@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "waylandkeyboardshortcutsinhibit.h"

#include <QGuiApplication>
#include <QWaylandClientExtensionTemplate>
#include <qpa/qplatformwindow_p.h>
#include "qwayland-keyboard-shortcuts-inhibit-unstable-v1.h"

class WaylandKeyboardShortcutsInhibitor : public QObject, public QtWayland::zwp_keyboard_shortcuts_inhibitor_v1
{
    Q_OBJECT

public:
    WaylandKeyboardShortcutsInhibitor(::zwp_keyboard_shortcuts_inhibitor_v1 *id)
        : QtWayland::zwp_keyboard_shortcuts_inhibitor_v1(id)
    {
    }

    ~WaylandKeyboardShortcutsInhibitor() override
    {
        destroy();
    }

    void zwp_keyboard_shortcuts_inhibitor_v1_active() override
    {
        m_active = true;
        Q_EMIT activeChanged(m_active);
    }

    void zwp_keyboard_shortcuts_inhibitor_v1_inactive() override
    {
        m_active = false;
        Q_EMIT activeChanged(m_active);
    }

    bool isActive() const
    {
        return m_active;
    }

Q_SIGNALS:
    void activeChanged(bool active);

private:
    bool m_active = false;
};


class WaylandKeyboardShortcutsInhibitManager : public QWaylandClientExtensionTemplate<WaylandKeyboardShortcutsInhibitManager>, public QtWayland::zwp_keyboard_shortcuts_inhibit_manager_v1
{
    Q_OBJECT

public:
    WaylandKeyboardShortcutsInhibitManager()
        : QWaylandClientExtensionTemplate<WaylandKeyboardShortcutsInhibitManager>(1)
    {
        initialize();
    }

    ~WaylandKeyboardShortcutsInhibitManager() override
    {
        if (isInitialized()) {
            destroy();
        }
    }

    bool inhibit(QWindow *window)
    {
        if (!window) {
            return false;
        }
        if (m_inhibitor) {
            return m_inhibitedWindow == window;
        }

        auto waylandApp = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
        auto waylandWindow = window->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
        if (!waylandApp || !waylandWindow) {
            return false;
        }

        auto seat = waylandApp->lastInputSeat();
        auto surface = waylandWindow->surface();
        if (!seat || !surface) {
            return false;
        }

        m_inhibitor.reset(new WaylandKeyboardShortcutsInhibitor(inhibit_shortcuts(surface, seat)));
        connect(m_inhibitor.get(), &WaylandKeyboardShortcutsInhibitor::activeChanged, this, &WaylandKeyboardShortcutsInhibitManager::inhibitorActiveChanged);
        connect(waylandWindow, &QNativeInterface::Private::QWaylandWindow::surfaceDestroyed, this, [this, waylandWindow] {
            m_inhibitor.reset();
            Q_EMIT inhibitorActiveChanged(false);
            connect(waylandWindow, &QNativeInterface::Private::QWaylandWindow::surfaceCreated, this, [this] {
                if (m_inhibited) {
                    inhibit(m_inhibitedWindow);
                }
            }, Qt::SingleShotConnection);
        }, Qt::SingleShotConnection);

        m_inhibitedWindow = window;
        m_inhibited = true;
        return true;
    }

    void uninhibit()
    {
        if (!m_inhibited) {
            return;
        }

        m_inhibitor.reset();
        m_inhibitedWindow = nullptr;
        m_inhibited = false;

        Q_EMIT inhibitorActiveChanged(false);
    }

    bool hasInhibitor() const
    {
        return m_inhibited;
    }

    bool isInhibitorActive() const
    {
        return m_inhibitor && m_inhibitor->isActive();
    }

Q_SIGNALS:
    void inhibitorActiveChanged(bool active);

private:
    std::unique_ptr<WaylandKeyboardShortcutsInhibitor> m_inhibitor = nullptr;
    bool m_inhibited = false;
    QWindow *m_inhibitedWindow = nullptr;
};


WaylandKeyboardShortcutsInhibit::WaylandKeyboardShortcutsInhibit(QObject *parent)
    : QObject(parent), m_manager(new WaylandKeyboardShortcutsInhibitManager())
{
    connect(m_manager, &WaylandKeyboardShortcutsInhibitManager::activeChanged, this, [this]() {
        Q_EMIT supportedChanged(m_manager->isActive());
    });
    connect(m_manager, &WaylandKeyboardShortcutsInhibitManager::inhibitorActiveChanged, this, &WaylandKeyboardShortcutsInhibit::inhibitorActiveChanged);
}

WaylandKeyboardShortcutsInhibit::~WaylandKeyboardShortcutsInhibit()
{
    delete m_manager;
}

bool WaylandKeyboardShortcutsInhibit::isSupported() const
{
    return m_manager && m_manager->isActive();
}

bool WaylandKeyboardShortcutsInhibit::hasInhibitor() const
{
    return m_manager && m_manager->hasInhibitor();
}

bool WaylandKeyboardShortcutsInhibit::isInhibitorActive() const
{
    return m_manager && m_manager->isInhibitorActive();
}

bool WaylandKeyboardShortcutsInhibit::inhibit(QWindow *window)
{
    if (!m_manager) {
        return false;
    }
    return m_manager->inhibit(window);
}

void WaylandKeyboardShortcutsInhibit::uninhibit()
{
    if (!m_manager) {
        return;
    }
    m_manager->uninhibit();
}


#include "waylandkeyboardshortcutsinhibit.moc"
