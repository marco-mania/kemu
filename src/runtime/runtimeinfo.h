/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <optional>

#include <QByteArray>
#include <QPointer>
#include <QString>

#include "dbusdisplay/dbusviewerwindow.h"
#include "utils/cloudimagedeployer.h"

/**
 * Runtime state of a single virtual machine.
 *
 * One instance exists per VM that has been started at least once in this
 * session. Created on demand, dropped when the VM itself is removed.
 *
 * This is deliberately a plain struct: it holds state, not behaviour. The
 * logic that acts on it moves into VmRuntimeManager in a later step.
 */
struct VmRuntimeInfo {
    QString vmName;

    // --- integrated D-Bus viewer ---

    struct ViewerState {
        QPointer<DbusViewerWindow> window;
        QemuDisplayListener *displayListener = nullptr;
        QemuAudioListener *audioOut = nullptr;
        QemuAudioInListener *audioIn = nullptr;
        QPointer<QemuControlConnection> ctrl;
    };

    // Empty until the viewer has been set up; mirrors the old
    // m_dbusViewers.contains() semantics.
    std::optional<ViewerState> viewer;
    int viewerConnectAttempts = 0;

    // --- QEMU failure handling ---

    QByteArray stderrTail;
    bool userInitiatedShutdown = false;
    bool resumingFromSavestate = false;
    bool startupFailureShown = false;

    // --- suspend / resume ---

    bool pausedBySuspend = false;

    // --- deployment ---

    struct PendingDeployment {
        enum class Type {
            CloudImage,
            ManualIso
        };
        Type type = Type::ManualIso;
        QString imageUrl;
        QString imageLocalPath;
        CloudDeployConfig cloudConfig;
    };

    // Outlives a single QEMU run: a deployment spans start, install and
    // finalize. Not touched by resetRunState().
    std::optional<PendingDeployment> deployment;

    // Clears the failure-handling state before a new run. Mirrors what the
    // start paths do today.
    void resetRunState()
    {
        stderrTail.clear();
        userInitiatedShutdown = false;
        resumingFromSavestate = false;
        startupFailureShown = false;
    }
};
