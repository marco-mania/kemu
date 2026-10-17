/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include "const.h"

#include "runtime/vmruntimeinfo.h"
#include "utils/qemucommandbuilder.h"
#include "utils/qmpclient.h"
#include "utils/vmconfig.h"

class VirtualEnvironmentModel;
class VirtualNetworkManager;
class UserModeNetwork;

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
        bool guestPoweredOff = false; // by the guest itself, not through KEMU
        bool shutdownHostWithGuest = false;
        QByteArray stderrTail;
    };

    explicit VmRuntimeManager(VirtualEnvironmentModel *model, VirtualNetworkManager *network_manager, QObject *parent = nullptr);

    struct LaunchRequest {
        QString vmName;
        bool headless = false;

        /// Virtual network mode: the descriptor ensureNetworkRunning()
        /// returned for this VM. Owned by the switch.
        int networkFd = -1;

        // Deployment keeps the VM in state Deploying, skips savestate
        // handling, and samples CPU and I/O but not memory.
        bool deployment = false;

        // Unattended (cloud-image) deployment: writes a serial log regardless
        // of whether a window happens to be shown. Never set for an attended
        // ISO install, where an interactive installer could move its UI to the
        // serial console once one is present.
        bool unattendedDeployment = false;
    };
    void launch(const LaunchRequest &request);

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

    KProcess *createQemuProcess(const QString &vmName, const QStringList &args, const QList<int> &inheritFds = {});
    void tearDown(const QString &vmName);
    void scheduleQemuStartupProbe(KProcess *process, const QString &vmName);

    // Start helper processes
    // Helper daemons report asynchronously; the error text travels with the
    // result because there is no caller frame left to fill an out-parameter.
    using HelperResult = std::function<void(bool ok, const QString &error)>;
    void startSwtpmAsync(const QString &vmName, HelperResult done);
    void startVirtiofsdAsync(const QString &vmName, HelperResult done);
    // Starts the in-process user-mode network for the VM if it is not
    // running yet. Returns true on success; `reason` is filled on failure and
    // is meant for the caller to display.
    bool ensureUserModeNetwork(const VMConfig &config, QString *reason);
    void stopUserModeNetwork(const QString &vmName);
    [[nodiscard]] QemuNetworkContext createQemuNetworkContext(const VMConfig &config, int switchFd = -1, int monitorFd = -1) const;

    // Applies a changed monitoring preference to the VMs already running.
    void setPerfMonitoringEnabled(bool enabled);

    // Misc helper functions
    [[nodiscard]] bool hasLiveQemu(const QString &vmName) const;
    // Returns the descriptor of the VM's switch port, or -1 on failure.
    // `reason` is filled on failure and is meant for the caller to display.
    // `dnsHostname`/`macAddress` are used to register the VM's DNS record
    // with the in-process DNS server (both may be empty).
    int ensureNetworkRunning(const QString &networkName,
                             const QString &vmName,
                             const QString &dnsHostname,
                             const QByteArray &macAddress,
                             QString *reason);
    void releaseNetwork(const QString &vmName);

    // Single source of truth for VM store locations.
    [[nodiscard]] QDir baseDir() const;
    [[nodiscard]] QDir vmDir(const QString &vmName) const;
    [[nodiscard]] QString vmPath(const QString &vmName) const;

    [[nodiscard]] QString monitorSocketPath(const QString &vmName) const
    {
        return vmDir(vmName).absoluteFilePath(KEMUConst::QemuMonitorSocketFileName);
    }
    [[nodiscard]] QString swtpmSocketPath(const QString &vmName) const
    {
        return vmDir(vmName).absoluteFilePath(KEMUConst::SwtpmSocketFileName);
    }
    [[nodiscard]] QString virtiofsdSocketPath(const QString &vmName) const
    {
        return vmDir(vmName).absoluteFilePath(KEMUConst::VirtiofsdSocketFileName);
    }
    [[nodiscard]] QString virtiofsdPidPath(const QString &vmName) const
    {
        return vmDir(vmName).absoluteFilePath(KEMUConst::VirtiofsdPidFileName);
    }
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

    // The QMP handshake is complete and the guest is configured.
    void qmpReady(const QString &vmName);

    // Forwarded QMP events; MainWindow::onQMPEvent() stays where it is.
    void qmpEvent(const QString &vmName, const QString &eventName, const QJsonObject &data);

    // The QMP performance timer has started and samples are about to flow.
    // Fires for deployments too, unlike qmpReady().
    void perfMonitoringStarted(const QString &vmName);

    // channel matches VirtualEnvironmentModel::appendPerfSample().
    void perfSample(const QString &vmName, int channel, double value);

private:
    static constexpr int kQemuStartupProbeMs = 750;
    static constexpr int kHelperShutdownMs = 3000;
    // Grace period for the kernel to reap a helper after SIGKILL.
    static constexpr int kHelperKillWaitMs = 1000;

    VirtualEnvironmentModel *m_model = nullptr;
    VirtualNetworkManager *m_network_manager = nullptr;

    QHash<QString, VmRuntimeInfo> m_runtimes;

    // In-process user-mode networks, keyed by VM name.
    QHash<QString, QPointer<UserModeNetwork>> m_userModeNets;

    void setUpQmpClient(const LaunchRequest &req, int monitorFd, bool isResume);
    void connectPerfSignals(QMPClient *client, const QString &vmName, bool includeMemory);

    static constexpr int kPerfSampleIntervalMs = 1000;
};

Q_DECLARE_METATYPE(VmRuntimeManager::ExitInfo)
