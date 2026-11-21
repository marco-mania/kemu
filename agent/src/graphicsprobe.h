/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>
#include <QProcess>
#include <QTimer>

#include "graphicsinfo.h"

// kemu-agent --graphics-probe: makes an OpenGL context on the GPU, the way
// an application of the session would, and prints its strings as JSON
// (GraphicsQuery::probeOutput()). Loads libEGL at run time: the portable
// bundle's Qt has no OpenGL, and the guest's Mesa is what counts. Returns
// the exit status.
int runGraphicsProbe();

// Collects GraphicsInfo once: the capability sets right away, the OpenGL
// strings from runGraphicsProbe() in a process of its own, so a GPU driver
// that crashes or hangs cannot take the agent with it.
class GraphicsProbeRunner : public QObject
{
    Q_OBJECT

public:
    explicit GraphicsProbeRunner(QObject *parent = nullptr);

    void start();

Q_SIGNALS:
    void finished(const GraphicsInfo &info);

private:
    void finish();

    QProcess m_process;
    QTimer m_timeout;
    GraphicsInfo m_info;
    bool m_done = false;
};
