/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>
#include <QString>
#include <QThread>

#include <functional>

/**
 * @brief Dedicated event loop thread for one in-process network.
 *
 * The switch, its internal ports and all their sockets live on this thread,
 * so guest traffic keeps flowing while the GUI thread is busy or blocked
 * (process teardown, libslirp startup, modal dialogs).
 *
 * Objects belonging to the network must be created, used and destroyed
 * inside run(), which marshals the call and waits for it. Callers may
 * capture by reference: the calling thread is blocked meanwhile.
 */
class NetworkThread
{
public:
    explicit NetworkThread(const QString &name);
    ~NetworkThread();

    NetworkThread(const NetworkThread &) = delete;
    NetworkThread &operator=(const NetworkThread &) = delete;

    /// Runs fn on the network thread and waits for it to finish.
    void run(const std::function<void()> &fn) const;

private:
    QThread m_thread;
    QObject *m_context = nullptr; // lives on m_thread
};
