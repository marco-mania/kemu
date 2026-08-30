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
#include "utils/qemucommandbuilder.h"
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
    struct ExitInfo {
        QString vmName;
        int exitCode = 0;
        KProcess::ExitStatus exitStatus = KProcess::NormalExit;
        bool abnormal = false;
        bool userInitiated = false;
        bool startupFailureAlreadyShown = false;
        bool wasDeploying = false;
        bool savestatePresent = false;
        QByteArray stderrTail;
    };

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

    KProcess *createQemuProcess(const QString &vmName, const QStringList &args, int tapFd);
    void tearDown(const QString &vmName);
    void scheduleQemuStartupProbe(KProcess *process, const QString &vmName);

    // Start helper processes
    // Helper daemons report asynchronously; the error text travels with the
    // result because there is no caller frame left to fill an out-parameter.
    using HelperResult = std::function<void(bool ok, const QString &error)>;
    void startSwtpmAsync(const QString &vmName, HelperResult done);
    void startVirtiofsdAsync(const QString &vmName, HelperResult done);
    void startPasstAsync(const QString &vmName, const QList<PortForwardRule> &portForwards, HelperResult done);
    // Single source of truth for the user-mode networking backend.
    [[nodiscard]] bool usePasstFor(const VMConfig &config) const;
    [[nodiscard]] QemuNetworkContext createQemuNetworkContext(const VMConfig &config, const QString &tapDevice = {}, int tapFd = -1) const;

    // Misc helper functions
    [[nodiscard]] bool hasLiveQemu(const QString &vmName) const;
    // Returns the tap device name, or an empty string on failure. `reason` is
    // filled on failure and is meant for the caller to display.
    QString ensureNetworkRunning(const QString &networkName, const QString &vmName, QString *reason);
    void releaseNetwork(const VMConfig &config);

    // Single source of truth for VM store locations.
    [[nodiscard]] QDir baseDir() const;
    [[nodiscard]] QDir vmDir(const QString &vmName) const;

public Q_SLOTS:
    void onQemuExit(const QString &vmName, int exitCode, KProcess::ExitStatus exitStatus);

Q_SIGNALS:
    // QEMU is up and registered in the model.
    void vmStarted(const QString &vmName, bool headless, bool deployment);

    // Emitted at the very start of tearDown(), before any signal source is
    // detached. Connect directly: the viewer must be gone before the QMP
    // client and the helper processes go.
    void runtimeTearingDown(const QString &vmName);

    // QEMU never came up. exitCode/exitStatus are meaningless when the failure
    // happened before the process was launched; `reason`, if non-empty,
    // describes that case.
    void startFailed(const QString &vmName, const QString &reason, int exitCode, KProcess::ExitStatus exitStatus, const QByteArray &stderrTail);

    // QEMU exited after a successful start.
    void vmExited(const VmRuntimeManager::ExitInfo &info);

    // channel matches VirtualEnvironmentModel::appendPerfSample().
    void perfSample(const QString &vmName, int channel, double value);

private:
    static constexpr int kQemuStartupProbeMs = 750;

    VirtualEnvironmentModel *m_model = nullptr;
    VirtualNetworkManager *m_network_manager = nullptr;

    QHash<QString, VmRuntimeInfo> m_runtimes;
};

Q_DECLARE_METATYPE(VmRuntimeManager::ExitInfo)
