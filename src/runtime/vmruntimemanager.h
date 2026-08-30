/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

#include "runtime/vmruntimeinfo.h"
#include "utils/vmconfig.h"

class VirtualEnvironmentModel;
class VirtualNetworkManager;

/**
 * Owns the runtime state of every virtual machine and, from step 4 on, the
 * QEMU start and teardown path.
 *
 * Never touches a widget. Everything the user should see is a signal.
 */
class VmRuntimeManager : public QObject
{
    Q_OBJECT

public:
    explicit VmRuntimeManager(VirtualEnvironmentModel *model, VirtualNetworkManager *network_manager, QObject *parent = nullptr);

    VmRuntimeInfo &runtimeFor(const QString &vmName);
    [[nodiscard]] VmRuntimeInfo *runtime(const QString &vmName);
    [[nodiscard]] const VmRuntimeInfo *runtime(const QString &vmName) const;
    void dropRuntime(const QString &vmName);

    [[nodiscard]] DbusViewerWindow *viewerWindow(const QString &vmName) const;
    [[nodiscard]] bool hasViewer(const QString &vmName) const;
    [[nodiscard]] QStringList vmNamesWithViewer() const;

    // Suspend bookkeeping: which VMs did KEMU pause for a system suspend?
    void clearPausedBySuspend();
    [[nodiscard]] QStringList takePausedBySuspend();

    // Live viewer windows across all VMs. Entries whose window has already
    // been destroyed are skipped.
    [[nodiscard]] QList<DbusViewerWindow *> viewerWindows() const;

    // Misc helper functions
    [[nodiscard]] bool hasLiveQemu(const QString &vmName) const;
    // Returns the tap device name, or an empty string on failure. `reason` is
    // filled on failure and is meant for the caller to display.
    QString ensureNetworkRunning(const QString &networkName, const QString &vmName, QString *reason);
    void releaseNetwork(const VMConfig &config);

private:
    VirtualEnvironmentModel *m_model = nullptr;
    VirtualNetworkManager *m_network_manager = nullptr;

    QHash<QString, VmRuntimeInfo> m_runtimes;
};
