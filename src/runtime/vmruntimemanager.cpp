/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "vmruntimemanager.h"

#include "dialogs/settingsdialog.h"
#include "models/virtualenvironmentmodel.h"
#include "utils/asynchelperprocess.h"
#include "utils/externaltools.h"
#include "utils/sandboxcapabilities.h"
#include "utils/savestatemanager.h"
#include "utils/utils.h"
#include "utils/virtualnetworkmanager.h"

#include "kemu_debug.h"

VmRuntimeManager::VmRuntimeManager(VirtualEnvironmentModel *model, VirtualNetworkManager *network_manager, QObject *parent)
    : QObject(parent)
    , m_model(model)
    , m_network_manager(network_manager)
{
}

void VmRuntimeManager::launch(const LaunchRequest &req)
{
    const QString &vmName = req.vmName;

    const VmEntry *entry = m_model->entryByName(vmName);
    if (!entry)
        return;
    if (req.deployment && entry->state != VmEntry::State::Deploying)
        return;

    // Copy: the model entry is mutated below and torn down on the failure path.
    const VMConfig config = entry->config;

    const QString path = vmPath(vmName);
    const QString sockPath = monitorSocketPath(vmName);
    QFile::remove(sockPath);

    int tapFd = -1;
    QString reason;
    if (!openTapFdFor(config, vmName, req.tapDevice, &tapFd, &reason)) {
        tearDown(vmName);
        Q_EMIT startFailed(vmName, reason, 0, KProcess::NormalExit, {});
        return;
    }

    // QEMU takes its own copy of the descriptor in start(); make sure ours is
    // closed on every path from here on.
    const auto tapGuard = qScopeGuard([&tapFd] {
        if (tapFd >= 0)
            ::close(tapFd);
    });

    const QemuNetworkContext netCtx = createQemuNetworkContext(config, req.tapDevice, tapFd);

    const QString savestatePath =
        (!req.deployment && SaveStateManager::hasSavestate(path, vmName)) ? SaveStateManager::savestateFilePath(path, vmName) : QString{};
    const bool isResume = !savestatePath.isEmpty();

    QStringList args = QemuCommandBuilder::build(config, vmDir(vmName), sockPath, netCtx, /*relativePaths=*/false, req.headless, savestatePath);

    // Unattended runs only: an interactive installer's UI could move to the
    // serial console once one is present, which would defeat an attended
    // installation. cloud-init has no such UI, so this is safe unconditionally
    // - and independent of whether a window is shown, which since the GTK/SDL
    // change no longer follows from the same flag.
    if (req.unattendedDeployment)
        args << u"-serial"_s << u"file:%1"_s.arg(path + u'/' + KEMUConst::DeploySerialLogFileName);

    KProcess *process = createQemuProcess(vmName, args, tapFd);

    connect(process, &KProcess::finished, this, [this, vmName](int code, KProcess::ExitStatus status) {
        onQemuExit(vmName, code, status);
    });

    // Error-during-start path (e.g. exec failure). We never reach the probe
    // in that case.
    connect(process, &KProcess::errorOccurred, this, [this, process, vmName, deployment = req.deployment](KProcess::ProcessError) {
        const VmEntry *e = m_model->entryByName(vmName);
        if (e && e->qemuProcess == process)
            return; // onQemuExit() will own this.

        const QString err = process->errorString();
        process->deleteLater();

        if (deployment) {
            // MainWindow rolls the deployment back; the reason string carries
            // the message the old code showed inline.
            Q_EMIT startFailed(vmName, i18n("Failed to start QEMU for deployment: %1", err), 0, KProcess::CrashExit, {});
        } else {
            runtimeFor(vmName).startupFailureShown = true;
            tearDown(vmName);
            Q_EMIT startFailed(vmName, QString{}, -1, KProcess::CrashExit, err.toUtf8());
        }
    });

    process->start();

    // start() can fail synchronously; errorOccurred has already cleaned up.
    if (process->state() == KProcess::NotRunning)
        return;

    // Optimistic registration: if QEMU dies inside the probe window, the probe
    // surfaces a startup failure and dedups against onQemuExit() via
    // VmRuntimeInfo::startupFailureShown.
    m_model->setHeadless(vmName, req.headless);
    m_model->setQemuProcess(vmName, process);

    // Deployment stays in state Deploying: onQemuExit() routes it to finalize
    // or abort when QEMU exits.
    if (!req.deployment)
        m_model->setVmState(vmName, VmEntry::State::Running);

    Q_EMIT vmStarted(vmName, req.headless, req.deployment);

    if (!req.deployment)
        scheduleQemuStartupProbe(process, vmName);

    setUpQmpClient(req, sockPath, isResume);
}

void VmRuntimeManager::setPerfMonitoringEnabled(bool enabled)
{
    const QStringList names = m_model->vmNames();
    for (const QString &name : names) {
        VmEntry *e = m_model->entryByName(name);
        if (!e || !e->qmpClient || !e->qmpClient->isConnected())
            continue;

        if (enabled && !e->qmpClient->isPerformanceTimerActive()) {
            e->qmpClient->startPerformanceTimer(kPerfSampleIntervalMs);
            Q_EMIT perfMonitoringStarted(name);
        } else if (!enabled && e->qmpClient->isPerformanceTimerActive()) {
            e->qmpClient->stopPerformanceTimer();
        }
    }
}

VmRuntimeInfo &VmRuntimeManager::runtimeFor(const QString &vmName)
{
    auto it = m_runtimes.find(vmName);
    if (it == m_runtimes.end()) {
        VmRuntimeInfo info;
        info.vmName = vmName;
        it = m_runtimes.insert(vmName, info);
    }
    return *it;
}

VmRuntimeInfo *VmRuntimeManager::runtime(const QString &vmName)
{
    auto it = m_runtimes.find(vmName);
    return it == m_runtimes.end() ? nullptr : &*it;
}

const VmRuntimeInfo *VmRuntimeManager::runtime(const QString &vmName) const
{
    auto it = m_runtimes.constFind(vmName);
    return it == m_runtimes.constEnd() ? nullptr : &*it;
}

void VmRuntimeManager::dropRuntime(const QString &vmName)
{
    m_runtimes.remove(vmName);
}

DbusViewerWindow *VmRuntimeManager::viewerWindow(const QString &vmName) const
{
    const auto *rt = runtime(vmName);
    if (!rt || !rt->viewer)
        return nullptr;
    return rt->viewer->window.data();
}

KProcess *VmRuntimeManager::createQemuProcess(const QString &vmName, const QStringList &args, int tapFd)
{
    auto *process = new KProcess(this);
    process->setOutputChannelMode(KProcess::SeparateChannels);

    constexpr int kStderrTailLimit = 65536;
    connect(process, &KProcess::readyReadStandardError, this, [this, process, vmName]() {
        Utils::appendTail(runtimeFor(vmName).stderrTail, process->readAllStandardError(), kStderrTailLimit);
    });

    // Never combine this with KProcess::UnixProcessParameters::CloseFileDescriptors:
    // it would close the tap descriptor despite the cleared CLOEXEC flag.
    Utils::setDiesWithParent(process, tapFd);

    process->setProgram(ExternalTools::instance().path(ExternalTools::QemuSystem));
    process->setArguments(args);

    return process;
}

void VmRuntimeManager::tearDown(const QString &vmName)
{
    VmEntry *entry = m_model->entryByName(vmName);
    if (!entry)
        return;

    // Detach signal sources first so no slot fires during teardown.
    if (entry->qmpClient) {
        entry->qmpClient->disconnect(this);
        entry->qmpClient->disconnectFromVm();
        entry->qmpClient->deleteLater();
    }

    // The integrated viewer is a window, so MainWindow owns closing it.
    // Direct connection: it has to be gone before the helpers below.
    Q_EMIT runtimeTearingDown(vmName);

    auto stopHelper = [](QPointer<KProcess> &p, int waitMs) {
        if (!p)
            return;
        p->disconnect();
        p->terminate();
        if (!p->waitForFinished(waitMs))
            p->kill();
        delete p; // QPointer auto-nullifies
    };

    stopHelper(entry->swtpmProcess, 3000);
    stopHelper(entry->virtiofsdProcess, 3000);
    stopHelper(entry->passtProcess, 3000);

    QFile::remove(swtpmSocketPath(vmName));
    QFile::remove(virtiofsdPidPath(vmName));
    QFile::remove(passtSocketPath(vmName));
    QFile::remove(monitorSocketPath(vmName));

    releaseNetwork(entry->config);
    m_model->clearRuntimeState(vmName);

    if (auto *rt = runtime(vmName)) {
        // startupFailureShown deliberately survives: onQemuExit() calls this
        // before checking the flag, and would otherwise show a second dialog
        // for a failure the startup probe already reported.
        rt->stderrTail.clear();
        rt->userInitiatedShutdown = false;
        rt->resumingFromSavestate = false;
    }
}

void VmRuntimeManager::scheduleQemuStartupProbe(KProcess *process, const QString &vmName)
{
    QPointer<KProcess> safeProcess(process);
    QTimer::singleShot(kQemuStartupProbeMs, this, [this, safeProcess, vmName]() {
        if (!safeProcess)
            return;
        if (safeProcess->state() == KProcess::Running)
            return; // Past the danger zone; onQemuExit() owns any future exit.

        // If finished() has already been delivered, onQemuExit() has run and
        // the entry no longer points at this process - nothing to do.
        const VmEntry *e = m_model->entryByName(vmName);
        if (!e || e->qemuProcess != safeProcess)
            return;

        auto &rt = runtimeFor(vmName);
        rt.startupFailureShown = true;

        // Capture diagnostics BEFORE teardown clears the stderr tail.
        const int exitCode = safeProcess->exitCode();
        const auto exitStatus = safeProcess->exitStatus();
        const QByteArray tail = rt.stderrTail;

        // FailedToStart never emits finished(), so onQemuExit() will never run
        // for this process. Clean up here instead.
        tearDown(vmName);
        m_model->setVmState(vmName, VmEntry::State::Stopped);
        safeProcess->deleteLater();

        Q_EMIT startFailed(vmName, QString{}, exitCode, exitStatus, tail);
    });
}

bool VmRuntimeManager::hasViewer(const QString &vmName) const
{
    const auto *rt = runtime(vmName);
    return rt && rt->viewer.has_value();
}

QStringList VmRuntimeManager::vmNamesWithViewer() const
{
    QStringList names;
    for (auto it = m_runtimes.cbegin(); it != m_runtimes.cend(); ++it) {
        if (it->viewer)
            names << it.key();
    }
    return names;
}

void VmRuntimeManager::clearPausedBySuspend()
{
    for (auto &rt : m_runtimes)
        rt.pausedBySuspend = false;
}

QStringList VmRuntimeManager::takePausedBySuspend()
{
    QStringList names;
    for (auto it = m_runtimes.begin(); it != m_runtimes.end(); ++it) {
        if (std::exchange(it->pausedBySuspend, false))
            names << it.key();
    }
    return names;
}

QList<DbusViewerWindow *> VmRuntimeManager::viewerWindows() const
{
    QList<DbusViewerWindow *> windows;
    for (const auto &rt : std::as_const(m_runtimes)) {
        if (rt.viewer && rt.viewer->window)
            windows << rt.viewer->window;
    }
    return windows;
}

/*********************************************************************************************
 * Start helper processes
 *********************************************************************************************/

void VmRuntimeManager::startSwtpmAsync(const QString &vmName, HelperResult done)
{
    const QString sockPath = swtpmSocketPath(vmName);
    QFile::remove(sockPath);

    AsyncHelperProcess::Options opts;
    opts.program = ExternalTools::instance().path(ExternalTools::Swtpm);
    opts.arguments =
        {u"socket"_s, u"--ctrl"_s, u"type=unixio,path=%1"_s.arg(sockPath), u"--terminate"_s, u"--tpmstate"_s, u"dir=%1"_s.arg(vmPath(vmName)), u"--tpm2"_s};
    opts.socketPath = sockPath;

    AsyncHelperProcess::run(this, opts, [this, vmName, sockPath, done = std::move(done)](KProcess *process, bool ok, const QString &err) {
        if (!ok) {
            QFile::remove(sockPath);
            done(false, i18n("Failed to start swtpm: %1", err));
            return;
        }
        m_model->setSwtpmProcess(vmName, process);
        done(true, {});
    });
}

void VmRuntimeManager::startVirtiofsdAsync(const QString &vmName, HelperResult done)
{
    const VmEntry *entry = m_model->entryByName(vmName);
    if (!entry) {
        done(false, {});
        return;
    }

    const QString sockPath = virtiofsdSocketPath(vmName);
    const QString pidPath = virtiofsdPidPath(vmName);
    const QString sharedPath = entry->config.sharedPath();
    QFile::remove(sockPath);

    AsyncHelperProcess::Options opts;
    opts.program = ExternalTools::instance().path(ExternalTools::Virtiofsd);
    opts.arguments = {u"--socket-path=%1"_s.arg(sockPath), u"--shared-dir=%1"_s.arg(sharedPath), u"--cache=auto"_s, u"--sandbox=none"_s};
    opts.socketPath = sockPath;

    AsyncHelperProcess::run(this, opts, [this, vmName, sockPath, pidPath, done = std::move(done)](KProcess *process, bool ok, const QString &err) {
        if (!ok) {
            QFile::remove(sockPath);
            QFile::remove(pidPath);
            done(false, i18n("Failed to start virtiofsd: %1", err));
            return;
        }
        m_model->setVirtiofsdProcess(vmName, process);
        done(true, {});
    });
}

void VmRuntimeManager::startPasstAsync(const QString &vmName, const QList<PortForwardRule> &portForwards, HelperResult done)
{
    const VmEntry *entry = m_model->entryByName(vmName);
    if (entry && !usePasstFor(entry->config)) {
        // Not reachable via the normal start paths; the VM is on SLIRP and
        // its command line has no passt netdev, so continuing is safe.
        qCWarning(KEMU) << "startPasstAsync() called for" << vmName << "which does not use passt - skipping";
        done(true, {});
        return;
    }

    const QString sockPath = passtSocketPath(vmName);
    QFile::remove(sockPath);

    QStringList passtArgs = {u"--socket"_s, sockPath, u"-f"_s, u"-q"_s};

    QStringList tcpPorts, udpPorts;
    for (const auto &rule : portForwards) {
        const QString mapping = (rule.hostPort != rule.guestPort) ? u"%1:%2"_s.arg(rule.hostPort).arg(rule.guestPort) : QString::number(rule.hostPort);
        if (rule.protocol == PortForwardRule::Protocol::Tcp)
            tcpPorts << mapping;
        else
            udpPorts << mapping;
    }
    if (!tcpPorts.isEmpty())
        passtArgs << u"-t"_s << tcpPorts.join(u","_s);
    if (!udpPorts.isEmpty())
        passtArgs << u"-u"_s << udpPorts.join(u","_s);

    AsyncHelperProcess::Options opts;
    opts.program = ExternalTools::instance().path(ExternalTools::Passt);
    opts.arguments = passtArgs;
    opts.socketPath = sockPath;

    AsyncHelperProcess::run(this, opts, [this, vmName, sockPath, done = std::move(done)](KProcess *process, bool ok, const QString &err) {
        if (!ok) {
            QFile::remove(sockPath);
            done(false, i18n("Failed to start passt: %1", err));
            return;
        }
        m_model->setPasstProcess(vmName, process);
        done(true, {});
    });
}

bool VmRuntimeManager::usePasstFor(const VMConfig &config) const
{
    if (config.networkCard() == VMSpecs::NetworkCard::None)
        return false;
    if (config.networkMode() == VMSpecs::NetworkMode::VirtualNetwork)
        return false;
    if (!ExternalTools::instance().isPasstUsable())
        return false;
    return !SettingsDialog::useSlirpNetworking();
}

QemuNetworkContext VmRuntimeManager::createQemuNetworkContext(const VMConfig &config, const QString &tapDevice, int tapFd) const
{
    QemuNetworkContext ctx;
    if (!tapDevice.isEmpty()) {
        ctx.tapDevice = tapDevice;
        ctx.tapFd = tapFd;
        return ctx;
    }
    ctx.useSlirp = !usePasstFor(config);
    return ctx;
}

/*********************************************************************************************
 * Misc helper functions
 *********************************************************************************************/

bool VmRuntimeManager::hasLiveQemu(const QString &vmName) const
{
    const VmEntry *entry = m_model->entryByName(vmName);
    return entry && (entry->state == VmEntry::State::Running || entry->state == VmEntry::State::Deploying);
}

QString VmRuntimeManager::ensureNetworkRunning(const QString &networkName, const QString &vmName, QString *reason)
{
    const auto fail = [reason](const QString &text) {
        if (reason)
            *reason = text;
        return QString{};
    };

    if (!SandboxCapabilities::instance().has(SandboxCapabilities::UserNamespaces)) {
        return fail(
            i18n("User namespaces are not supported on this system. Virtual "
                 "networking requires unprivileged user namespaces."));
    }

    if (!ExternalTools::instance().isVirtualNetworkCapable()) {
        return fail(
            i18n("Required tools for virtual networking are "
                 "missing. Please check Settings → Executables."));
    }

    const VirtualNetwork net = m_network_manager->network(networkName);
    if (!net.isValid())
        return fail(i18n("Virtual network '%1' not found or invalid: %2", networkName, m_network_manager->lastError()));

    if (!m_network_manager->startNetwork(networkName))
        return fail(i18n("Failed to start virtual network '%1': %2", networkName, m_network_manager->lastError()));

    const QString tapDevice = m_network_manager->attachVm(networkName, vmName);
    if (tapDevice.isEmpty())
        return fail(i18n("Failed to create network interface for VM '%1': %2", vmName, m_network_manager->lastError()));

    return tapDevice;
}

void VmRuntimeManager::releaseNetwork(const VMConfig &config)
{
    if (!m_network_manager)
        return;
    if (config.networkMode() != VMSpecs::NetworkMode::VirtualNetwork)
        return;
    if (config.virtualNetworkName().isEmpty())
        return;
    m_network_manager->detachVm(config.virtualNetworkName(), config.name());
}

QDir VmRuntimeManager::baseDir() const
{
    return QDir(SettingsDialog::getVmBasePath());
}

QDir VmRuntimeManager::vmDir(const QString &vmName) const
{
    return QDir(baseDir().absoluteFilePath(vmName));
}

QString VmRuntimeManager::vmPath(const QString &vmName) const
{
    return baseDir().absoluteFilePath(vmName);
}

void VmRuntimeManager::onQemuExit(const QString &vmName, int exitCode, KProcess::ExitStatus exitStatus)
{
    const VmEntry *entry = m_model->entryByName(vmName);
    if (!entry)
        return;

    QPointer<KProcess> p = entry->qemuProcess;
    auto &rt = runtimeFor(vmName);

    ExitInfo info;
    info.vmName = vmName;
    info.exitCode = exitCode;
    info.exitStatus = exitStatus;
    info.abnormal = (exitStatus != KProcess::NormalExit) || (exitCode != 0);
    info.userInitiated = rt.userInitiatedShutdown;
    info.startupFailureAlreadyShown = rt.startupFailureShown;
    info.wasDeploying = entry->state == VmEntry::State::Deploying;
    info.savestatePresent = SaveStateManager::hasSavestate(vmPath(vmName), vmName);

    if (p)
        rt.stderrTail.append(p->readAllStandardError());
    info.stderrTail = rt.stderrTail;

    tearDown(vmName);

    // clearRuntimeState() has just set Stopped; a completed save-state is the
    // only outcome that needs a different one.
    if (!info.wasDeploying && info.savestatePresent && !info.abnormal)
        m_model->setVmState(vmName, VmEntry::State::Saved);

    rt.startupFailureShown = false;

    if (p)
        p->deleteLater();

    Q_EMIT vmExited(info);
}

bool VmRuntimeManager::openTapFdFor(const VMConfig &config, const QString &vmName, const QString &tapDevice, int *tapFd, QString *reason)
{
    *tapFd = -1;

    if (config.networkMode() != VMSpecs::NetworkMode::VirtualNetwork || tapDevice.isEmpty() || !m_network_manager)
        return true;

    // Opening the tap inside the namespace and handing QEMU the descriptor
    // keeps QEMU in our namespace, and thus at our UID - the D-Bus display
    // authenticates over the socket's peer credentials.
    const int fd = m_network_manager->openTapFd(config.virtualNetworkName(), vmName);
    if (fd < 0) {
        if (reason)
            *reason = i18n("Could not open the network interface for '%1': %2", vmName, m_network_manager->lastError());
        return false;
    }

    *tapFd = fd;
    return true;
}

void VmRuntimeManager::setUpQmpClient(const LaunchRequest &req, const QString &sockPath, bool isResume)
{
    const QString vmName = req.vmName;

    auto *client = new QMPClient(this);
    m_model->setQmpClient(vmName, client);

    connect(client, &QMPClient::eventReceived, this, [this, vmName](const QString &eventName, const QJsonObject &data) {
        Q_EMIT qmpEvent(vmName, eventName, data);
    });

    connect(client, &QMPClient::connected, this, [this, client, vmName, deployment = req.deployment, isResume]() {
        // The monitor widget is the only consumer of these samples.
        if (SettingsDialog::isPerformanceMonitorEnabled()) {
            client->startPerformanceTimer(kPerfSampleIntervalMs);
            Q_EMIT perfMonitoringStarted(vmName);
        }

        if (deployment)
            return;

        // Enable balloon guest statistics for actual RAM usage.
        QJsonObject balloonArgs;
        balloonArgs[u"path"_s] = u"/machine/peripheral-anon/balloon0"_s;
        balloonArgs[u"property"_s] = u"guest-stats-polling-interval"_s;
        balloonArgs[u"value"_s] = 2;
        client->sendCommand(u"qom-set"_s, balloonArgs);

        Q_EMIT qmpReady(vmName);

        // Successful resume: the savestate file has done its job, drop it so a
        // subsequent normal start does not try to resume again.
        const QString path = vmPath(vmName);
        if (SaveStateManager::hasSavestate(path, vmName)) {
            SaveStateManager::discardSavestate(path, vmName);
            m_model->setHasSavestate(vmName, false);
        }

        if (isResume) {
            // Do NOT 'cont' here: QMP is up long before the incoming migration
            // has finished loading the (paused) state. Enable MIGRATION events
            // and let MainWindow resume once status == "completed".
            runtimeFor(vmName).resumingFromSavestate = true;

            QJsonArray caps;
            QJsonObject capEvents;
            capEvents.insert(u"capability"_s, u"events"_s);
            capEvents.insert(u"state"_s, true);
            caps.append(capEvents);
            client->sendCommand(u"migrate-set-capabilities"_s, {{u"capabilities"_s, caps}}, nullptr);
        }
    });

    // Deployments get CPU and I/O: both are measured entirely host-side and
    // show whether the installation is making progress. Memory is left out -
    // the balloon driver is not up in the guest yet and the guest-stats
    // qom-set above is skipped, so query-balloon would report a flat 100%.
    connectPerfSignals(client, vmName, /*includeMemory=*/!req.deployment);

    QPointer<QMPClient> safeClient(client);
    AsyncHelperProcess::waitForSocket(this, sockPath, 2000, 50, [safeClient, vmName, sockPath](bool ok) {
        if (!ok) {
            qCWarning(KEMU) << "QMP: socket not ready for" << vmName;
            return;
        }
        if (safeClient)
            safeClient->connectToVm(sockPath);
    });
}

void VmRuntimeManager::connectPerfSignals(QMPClient *client, const QString &vmName, bool includeMemory)
{
    connect(client, &QMPClient::cpuUsageUpdated, this, [this, vmName](const VmCpuUsage &usage) {
        if (!usage.isValid)
            return;
        m_model->appendPerfSample(vmName, 0, usage.averagePercent);
        Q_EMIT perfSample(vmName, 0, usage.averagePercent);
    });

    if (includeMemory) {
        connect(client, &QMPClient::memoryUsageUpdated, this, [this, vmName](const VmMemoryUsage &usage) {
            if (!usage.isValid)
                return;
            m_model->appendPerfSample(vmName, 1, usage.usagePercent);
            Q_EMIT perfSample(vmName, 1, usage.usagePercent);
        });
    }

    connect(client, &QMPClient::blockStatsUpdated, this, [this, vmName](const VmBlockStats &stats) {
        if (!stats.isValid)
            return;
        VmEntry *e = m_model->entryByName(vmName);
        if (!e)
            return;

        const double totalBps = stats.totalRdBytesPerSec + stats.totalWrBytesPerSec;
        if (totalBps > e->ioPeakBytesPerSec)
            e->ioPeakBytesPerSec = totalBps;
        const double pct = (e->ioPeakBytesPerSec > 0.0) ? (totalBps / e->ioPeakBytesPerSec * 100.0) : 0.0;

        m_model->appendPerfSample(vmName, 2, pct);
        Q_EMIT perfSample(vmName, 2, pct);
    });
}
