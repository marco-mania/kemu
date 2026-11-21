/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "agent.h"

#include "clipboardsync.h"
#include "graphicsinfo.h"
#include "graphicsprobe.h"
#include "layoutreporter.h"
#include "logging.h"
#include "sessionwatcher.h"

Agent::Agent(const Options &options,
             const Session::Identity &identity,
             ClipboardBackend *backend,
             DisplayLayoutSource *layout,
             SessionWatcher *watcher,
             QObject *parent)
    : QObject(parent)
    , m_options(options)
    , m_port(options.portPath)
    , m_session(&m_port, identity)
    , m_watcher(watcher)
{
    if (backend)
        m_clipboard = std::make_unique<ClipboardSync>(&m_session, backend);
    if (layout)
        m_layout = std::make_unique<LayoutReporter>(&m_session, layout, HeadMapper());
    if (options.graphics) {
        m_graphics = std::make_unique<GraphicsReporter>(&m_session);
        m_graphicsProbe = std::make_unique<GraphicsProbeRunner>();
        connect(m_graphicsProbe.get(), &GraphicsProbeRunner::finished, m_graphics.get(), &GraphicsReporter::setInfo);
    }

    m_retry.setSingleShot(true);
    m_retry.setInterval(m_options.retryMs);
    connect(&m_retry, &QTimer::timeout, this, &Agent::tryOpen);
    if (m_watcher)
        connect(m_watcher, &SessionWatcher::activeChanged, this, &Agent::onActiveChanged);
}

Agent::~Agent()
{
    // The parts below the session go first; they listen to it.
    m_graphicsProbe.reset();
    m_graphics.reset();
    m_layout.reset();
    m_clipboard.reset();
}

void Agent::start()
{
    // Once per run: the GPU does not change while the guest runs.
    if (m_graphicsProbe)
        m_graphicsProbe->start();
    tryOpen();
}

void Agent::shutdown(const QString &reason)
{
    m_retry.stop();
    m_session.sendGoodbye(reason);
    m_port.close();
}

void Agent::tryOpen()
{
    if (m_watcher && !m_watcher->isActive())
        return;

    const PortConnection::OpenResult result = m_port.open();
    if (result == PortConnection::OpenResult::Ok) {
        m_lastProblem = result;
        m_session.start();
        return;
    }

    // Say it once, then retry quietly (section 3.4).
    if (result != m_lastProblem) {
        switch (result) {
        case PortConnection::OpenResult::Busy:
            qCInfo(KEMU_AGENT) << "Port: held by another session's agent; waiting";
            break;
        case PortConnection::OpenResult::Denied:
            qCInfo(KEMU_AGENT) << "Port: no access yet; waiting";
            break;
        case PortConnection::OpenResult::Missing:
            qCInfo(KEMU_AGENT) << "Port:" << m_options.portPath << "is gone; waiting";
            break;
        default:
            break;
        }
        m_lastProblem = result;
    }
    m_retry.start();
}

void Agent::onActiveChanged(bool active)
{
    if (active) {
        tryOpen();
    } else {
        // Let the agent of the session now active have the port.
        shutdown(u"session-inactive"_s);
    }
}
