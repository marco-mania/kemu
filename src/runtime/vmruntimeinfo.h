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
#include "utils/guestagentclient.h"

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

    // --- host power-off with the guest ---

    // Armed from the VM's configuration at launch; the menus toggle it per run.
    bool shutdownHostWithGuest = false;
    bool guestPoweredOff = false;

    // --- QEMU guest agent ---

    // Null without a channel (switched off, or a machine without PCI). Its
    // status tells whether an agent runs in the guest and answers.
    QPointer<GuestAgentClient> guestAgent;
    // The guest clock is to be set once the agent answers.
    bool guestTimeSyncPending = false;

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
        // Password chosen for encrypted disks, kept for the deployment only:
        // not every user stores it in the wallet.
        QByteArray diskKey;
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
        guestPoweredOff = false;
        guestTimeSyncPending = false;
    }
};
