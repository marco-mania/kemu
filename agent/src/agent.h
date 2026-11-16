/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>
#include <QTimer>

#include <memory>

#include "portconnection.h"
#include "session.h"

class ClipboardBackend;
class ClipboardSync;
class DisplayLayoutSource;
class LayoutReporter;
class SessionWatcher;

// Ties the parts together: holds the port while our session is active,
// runs the protocol session on it, and lets clipboard and layout use it.
class Agent : public QObject
{
    Q_OBJECT

public:
    struct Options {
        QString portPath;
        int retryMs = 2000;
    };

    // backend and layout may be nullptr; the agent then does without.
    Agent(const Options &options,
          const Session::Identity &identity,
          ClipboardBackend *backend,
          DisplayLayoutSource *layout,
          SessionWatcher *watcher,
          QObject *parent = nullptr);
    ~Agent() override;

    void start();
    // Says goodbye and lets go of the port.
    void shutdown(const QString &reason);

    Session *session()
    {
        return &m_session;
    }

private:
    void tryOpen();
    void onActiveChanged(bool active);

    Options m_options;
    PortConnection m_port;
    Session m_session;
    std::unique_ptr<ClipboardSync> m_clipboard;
    std::unique_ptr<LayoutReporter> m_layout;
    SessionWatcher *m_watcher;
    QTimer m_retry;
    PortConnection::OpenResult m_lastProblem = PortConnection::OpenResult::Ok;
};
