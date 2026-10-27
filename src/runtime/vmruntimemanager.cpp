/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "vmruntimemanager.h"

#include "dialogs/settingsdialog.h"
#include "models/virtualenvironmentmodel.h"
#include "utils/asynchelperprocess.h"
#include "utils/diskencryption.h"
#include "utils/externaltools.h"
#include "utils/guestagentclient.h"
#include "utils/qemucapabilities.h"
#include "utils/savestatemanager.h"
#include "utils/tpmstate.h"
#include "utils/usermodenetwork.h"
#include "utils/utils.h"
#include "utils/virtualnetworkmanager.h"

#include "kemu_debug.h"

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

#include <QDateTime>

VmRuntimeManager::VmRuntimeManager(VirtualEnvironmentModel *model, VirtualNetworkManager *network_manager, QObject *parent)
    : QObject(parent)
    , m_model(model)
    , m_network_manager(network_manager)
{
}

void VmRuntimeManager::launch(const LaunchRequest &req)
{
    const QString &vmName = req.vmName;

    // The caller attached the network and started the user-mode stack before
    // launching, so both have to be released on every early return.
    const VmEntry *entry = m_model->entryByName(vmName);
    if (!entry) {
        releaseNetwork(vmName);
        stopUserModeNetwork(vmName);
        return;
    }
    if (req.deployment && entry->state != VmEntry::State::Deploying) {
        tearDown(vmName);
        return;
    }

    // Copy: the model entry is mutated below and torn down on the failure path.
    VMConfig config = entry->config;

    const QString path = vmPath(vmName);

    // QMP rides on a socketpair: nothing in the file system that another
    // process of this user could connect to, and the monitor is reachable the
    // moment QEMU starts.
    int qmpFds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, qmpFds) != 0) {
        qCWarning(KEMU) << "VmRuntimeManager: socketpair() for the QMP monitor failed:" << strerror(errno);
        tearDown(vmName);
        return;
    }

    QemuNetworkContext netCtx = createQemuNetworkContext(config, req.networkFd, qmpFds[1]);

    // The guest agent gets a socketpair as well, for the same reasons.
    int qgaFds[2] = {-1, -1};
    if (config.useGuestAgent() && VMConfig::supportsPciBus(config.machineType())) {
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, qgaFds) == 0) {
            netCtx.guestAgentFd = qgaFds[1];
        } else {
            // Not worth failing the start for; without a descriptor the
            // command line would fall back to a listening socket.
            qCWarning(KEMU) << "VmRuntimeManager: socketpair() for the guest agent failed:" << strerror(errno);
            config.setUseGuestAgent(false);
        }
    }
    const auto closeQgaFds = [&qgaFds] {
        for (int &fd : qgaFds) {
            if (fd >= 0)
                ::close(fd);
            fd = -1;
        }
    };

    if (config.isEncrypted()) {
        netCtx.diskKeyFd = DiskEncryption::secretFd(req.diskKey);
        if (netCtx.diskKeyFd < 0) {
            qCWarning(KEMU) << "VmRuntimeManager: could not pass the disk password to QEMU";
            ::close(qmpFds[0]);
            ::close(qmpFds[1]);
            closeQgaFds();
            tearDown(vmName);
            return;
        }
    }

    const QString savestatePath =
        (!req.deployment && SaveStateManager::hasSavestate(path, vmName)) ? SaveStateManager::savestateFilePath(path, vmName) : QString{};
    const bool isResume = !savestatePath.isEmpty();

    // The monitor path is only a fallback for exported scripts; this QEMU
    // gets the descriptor from netCtx.
    QStringList args =
        QemuCommandBuilder::build(config, vmDir(vmName), monitorSocketPath(vmName), netCtx, /*relativePaths=*/false, req.headless, savestatePath);

    // Unattended runs only: an interactive installer's UI could move to the
    // serial console once one is present, which would defeat an attended
    // installation. cloud-init has no such UI, so this is safe unconditionally
    // - and independent of whether a window is shown, which does not follow
    // from the same flag now that only the integrated viewer runs headless.
    if (req.unattendedDeployment)
        args << u"-serial"_s << u"file:%1"_s.arg(Utils::escapeQemuOptionValue(path + u'/' + KEMUConst::DeploySerialLogFileName));

    // A guest that breaks into QEMU could otherwise start programs, change
    // credentials and reach obsolete or privileged syscalls. Off unless the
    // user asks for it: the filter also stops QEMU features that need those
    // calls, and it does so with SIGSYS rather than an error message.
    const QStringList sandbox = QemuCapabilities::instance().hasSeccompSandbox()
        ? QemuCommandBuilder::sandboxArgs(config, SettingsDialog::qemuSeccompSandbox())
        : QStringList();
    args << sandbox;

    QList<int> inheritFds = {qmpFds[1]};
    if (netCtx.switchFd >= 0)
        inheritFds << netCtx.switchFd;
    if (netCtx.userNetFd >= 0)
        inheritFds << netCtx.userNetFd;
    if (netCtx.diskKeyFd >= 0)
        inheritFds << netCtx.diskKeyFd;
    if (netCtx.guestAgentFd >= 0)
        inheritFds << netCtx.guestAgentFd;

    KProcess *process = createQemuProcess(vmName, args, inheritFds);
    const bool sdlDisplay = config.displayType() == VMSpecs::DisplayType::Sdl && !req.headless;
    if (!sandbox.isEmpty() || sdlDisplay) {
        QProcessEnvironment env = sandbox.isEmpty() ? QProcessEnvironment::systemEnvironment() : Utils::environmentWithoutDbusAutolaunch();
        if (sdlDisplay)
            env.insert(u"SDL_VIDEODRIVER"_s, u"wayland"_s);
        process->setProcessEnvironment(env);
    }

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

    // QEMU read the password at startup, or never will.
    if (netCtx.diskKeyFd >= 0)
        ::close(netCtx.diskKeyFd);

    // start() can fail synchronously; errorOccurred has already cleaned up.
    if (process->state() == KProcess::NotRunning) {
        ::close(qmpFds[0]);
        ::close(qmpFds[1]);
        closeQgaFds();
        return;
    }

    // The child has the descriptors now, so drop our copies. The switch and
    // the user-mode network own theirs and close them on request.
    ::close(qmpFds[1]);
    if (qgaFds[0] >= 0) {
        ::close(qgaFds[1]);
        auto *agent = new GuestAgentClient(this);
        agent->connectToVm(qgaFds[0]);
        connect(agent, &GuestAgentClient::statusChanged, this, [this, vmName](GuestAgentClient::Status status) {
            if (status == GuestAgentClient::Status::Connected)
                syncGuestTime(vmName);
            Q_EMIT guestAgentChanged(vmName);
        });
        connect(agent, &GuestAgentClient::guestInfoChanged, this, [this, agent, vmName] {
            m_model->setGuestAddress(vmName, agent->guestInfo().addresses.value(0));
            Q_EMIT guestAgentChanged(vmName);
        });
        runtimeFor(vmName).guestAgent = agent;
    }
    if (netCtx.switchFd >= 0 && m_network_manager)
        m_network_manager->releaseGuestFd(config.virtualNetworkName(), vmName);
    if (netCtx.userNetFd >= 0) {
        if (auto net = m_userModeNets.value(vmName))
            net->releaseGuestFd();
    }

    // Optimistic registration: if QEMU dies inside the probe window, the probe
    // surfaces a startup failure and dedups against onQemuExit() via
    // VmRuntimeInfo::startupFailureShown.
    m_model->setHeadless(vmName, req.headless);
    m_model->setQemuProcess(vmName, process);

    runtimeFor(vmName).shutdownHostWithGuest = !req.deployment && config.guestShutdownAction() == VMSpecs::GuestShutdownAction::ShutdownHost;

    // Deployment stays in state Deploying: onQemuExit() routes it to finalize
    // or abort when QEMU exits.
    if (!req.deployment)
        m_model->setVmState(vmName, VmEntry::State::Running);

    Q_EMIT vmStarted(vmName, req.headless, req.deployment);

    if (!req.deployment)
        scheduleQemuStartupProbe(process, vmName);

    setUpQmpClient(req, qmpFds[0], isResume);
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

KProcess *VmRuntimeManager::createQemuProcess(const QString &vmName, const QStringList &args, const QList<int> &inheritFds)
{
    auto *process = new KProcess(this);
    process->setOutputChannelMode(KProcess::SeparateChannels);

    constexpr int kStderrTailLimit = 65536;
    connect(process, &KProcess::readyReadStandardError, this, [this, process, vmName]() {
        Utils::appendTail(runtimeFor(vmName).stderrTail, process->readAllStandardError(), kStderrTailLimit);
    });

    // Never combine this with KProcess::UnixProcessParameters::CloseFileDescriptors.
    Utils::setDiesWithParent(process, inheritFds);

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
    if (VmRuntimeInfo &rt = runtimeFor(vmName); rt.guestAgent) {
        rt.guestAgent->disconnect(this);
        rt.guestAgent->deleteLater();
        rt.guestAgent = nullptr;
        Q_EMIT guestAgentChanged(vmName);
    }

    // The integrated viewer is a window, so MainWindow owns closing it.
    // Direct connection: it has to be gone before the helpers below.
    Q_EMIT runtimeTearingDown(vmName);

    auto terminateHelper = [](QPointer<KProcess> &p) {
        if (!p)
            return;
        p->disconnect();
        p->terminate();
    };

    auto reapHelper = [](QPointer<KProcess> &p, int waitMs) {
        if (!p)
            return;
        // waitForFinished() also returns false for a process that has already
        // been reaped, so check the state rather than the return value.
        if (!p->waitForFinished(waitMs) && p->state() != KProcess::NotRunning) {
            qCWarning(KEMU) << "Helper" << p->program() << "did not exit after SIGTERM, sending SIGKILL";
            p->kill();
            // kill() only sends the signal: QProcess keeps reporting Running
            // until the child has been reaped. Deleting it before then makes
            // ~QProcess warn "Destroyed while process is still running" and
            // block the GUI thread in another waitForFinished() of up to 30 s.
            if (!p->waitForFinished(kHelperKillWaitMs) && p->state() != KProcess::NotRunning) {
                // Not even SIGKILL got through in time (e.g. the child is in
                // uninterruptible sleep on the shared directory). Let it be
                // reaped asynchronously instead of destroying it while running.
                qCWarning(KEMU) << "Helper" << p->program() << "still running after SIGKILL, reaping it later";
                KProcess *straggler = p.data();
                QObject::connect(straggler, &KProcess::finished, straggler, &QObject::deleteLater);
                p = nullptr;
                return;
            }
        }
        delete p; // QPointer auto-nullifies
    };

    // Signal both first, then share one deadline: they shut down in
    // parallel instead of one after the other.
    terminateHelper(entry->swtpmProcess);
    terminateHelper(entry->virtiofsdProcess);

    QElapsedTimer deadline;
    deadline.start();
    const auto remaining = [&deadline] {
        return static_cast<int>(qMax(qint64(0), qint64(kHelperShutdownMs) - deadline.elapsed()));
    };

    reapHelper(entry->swtpmProcess, remaining());
    reapHelper(entry->virtiofsdProcess, remaining());

    stopUserModeNetwork(vmName);

    QFile::remove(swtpmSocketPath(vmName));
    QFile::remove(virtiofsdPidPath(vmName));

    releaseNetwork(vmName);
    m_model->clearRuntimeState(vmName);

    if (auto *rt = runtime(vmName)) {
        // startupFailureShown deliberately survives: onQemuExit() calls this
        // before checking the flag, and would otherwise show a second dialog
        // for a failure the startup probe already reported.
        rt->stderrTail.clear();
        rt->userInitiatedShutdown = false;
        rt->resumingFromSavestate = false;
        rt->guestPoweredOff = false;
        rt->shutdownHostWithGuest = false;
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

void VmRuntimeManager::refreshGuestAgentPort(const QString &vmName)
{
    const VmEntry *entry = m_model->entryByName(vmName);
    if (!entry || !entry->qmpClient || !runtimeFor(vmName).guestAgent)
        return;

    entry->qmpClient->sendCommand(u"query-chardev"_s, {}, [this, vmName](const QMPResult &result) {
        GuestAgentClient *agent = runtimeFor(vmName).guestAgent;
        if (!agent || !result.success)
            return;
        for (const QJsonValue &chardev : result.dataArray) {
            if (chardev.toObject().value(u"label"_s).toString() == KEMUConst::GuestAgentChardevId)
                agent->setPortOpen(chardev.toObject().value(u"frontend-open"_s).toBool());
        }
    });
}

void VmRuntimeManager::requestGuestTimeSync(const QString &vmName, bool resync)
{
    VmRuntimeInfo &rt = runtimeFor(vmName);
    rt.guestTimeSyncPending = true;
    // A guest put back to an earlier state may have left the agent mid-way.
    if (resync && rt.guestAgent)
        rt.guestAgent->resync();
    syncGuestTime(vmName);
}

void VmRuntimeManager::syncGuestTime(const QString &vmName)
{
    VmRuntimeInfo &rt = runtimeFor(vmName);
    GuestAgentClient *agent = rt.guestAgent;
    if (!rt.guestTimeSyncPending || !agent || !agent->isConnected())
        return; // tried again once the agent connects

    rt.guestTimeSyncPending = false;
    // Nanoseconds since the epoch, UTC; the agent sets the guest's system
    // clock (and, on Linux, its RTC) from it.
    const qint64 now = QDateTime::currentMSecsSinceEpoch() * 1000000;
    agent->sendCommand(u"guest-set-time"_s, {{u"time"_s, now}}, [vmName](bool ok, const QJsonValue &, const QString &error) {
        if (!ok)
            qCWarning(KEMU) << "Could not set the clock of" << vmName << ":" << error;
    });
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

void VmRuntimeManager::startSwtpmAsync(const QString &vmName, const QByteArray &diskKey, HelperResult done)
{
    const QString sockPath = swtpmSocketPath(vmName);
    QFile::remove(sockPath);

    AsyncHelperProcess::Options opts;
    opts.program = ExternalTools::instance().path(ExternalTools::Swtpm);
    opts.arguments =
        {u"socket"_s, u"--ctrl"_s, u"type=unixio,path=%1"_s.arg(sockPath), u"--terminate"_s, u"--tpmstate"_s, u"dir=%1"_s.arg(vmPath(vmName)), u"--tpm2"_s};
    opts.socketPath = sockPath;

    int keyFd = -1;
    if (!diskKey.isEmpty()) {
        // A state from before the disk was encrypted is moved over once.
        const QDir stateDir = vmDir(vmName);
        if (TpmState::encryptionOf(stateDir) == TpmState::Encryption::Plain) {
            const QDir target(stateDir.filePath(u".tpm-rekey"_s));
            const QString ioctl = ExternalTools::instance().path(ExternalTools::SwtpmIoctl);
            const QString error = TpmState::rekey(opts.program, ioctl, stateDir, {}, diskKey, target);
            if (!error.isEmpty() || !TpmState::commit(stateDir, target)) {
                done(false, i18n("The TPM state could not be encrypted: %1", error));
                return;
            }
        }

        keyFd = DiskEncryption::secretFd(diskKey);
        if (keyFd < 0) {
            done(false, i18n("The password could not be passed to swtpm."));
            return;
        }
        opts.arguments << u"--key"_s << TpmState::keyOption(keyFd);
        opts.inheritFds = {keyFd};
    }

    AsyncHelperProcess::run(this, opts, [this, vmName, sockPath, done = std::move(done)](KProcess *process, bool ok, const QString &err) {
        if (!ok) {
            QFile::remove(sockPath);
            done(false, i18n("Failed to start swtpm: %1", err));
            return;
        }
        m_model->setSwtpmProcess(vmName, process);
        done(true, {});
    });

    // swtpm has read the password at startup, or never will.
    if (keyFd >= 0)
        ::close(keyFd);
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

bool VmRuntimeManager::ensureUserModeNetwork(const VMConfig &config, QString *reason)
{
    const QString vmName = config.name();

    if (auto net = m_userModeNets.value(vmName)) {
        if (net->isRunning())
            return true;
        // Stale instance from a previous failed start.
        net->stop();
        net->deleteLater();
        m_userModeNets.remove(vmName);
    }

    auto *net = new UserModeNetwork(this);
    if (!net->start(config.portForwards())) {
        if (reason)
            *reason = net->lastError();
        net->deleteLater();
        return false;
    }
    m_userModeNets.insert(vmName, net);
    return true;
}

void VmRuntimeManager::stopUserModeNetwork(const QString &vmName)
{
    if (auto net = m_userModeNets.take(vmName)) {
        net->stop();
        net->deleteLater();
    }
}

QemuNetworkContext VmRuntimeManager::createQemuNetworkContext(const VMConfig &config, int switchFd, int monitorFd) const
{
    QemuNetworkContext ctx;
    ctx.monitorFd = monitorFd;

    if (config.networkMode() == VMSpecs::NetworkMode::VirtualNetwork) {
        ctx.switchFd = switchFd;
        return ctx;
    }

    // User mode: the in-process network that ensureUserModeNetwork() started
    // before launch owns the descriptor. buildShellScript() replaces this
    // with useSlirp for standalone exports.
    if (const auto net = m_userModeNets.value(config.name()))
        ctx.userNetFd = net->guestFd();
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

int VmRuntimeManager::ensureNetworkRunning(const QString &networkName,
                                           const QString &vmName,
                                           const QString &dnsHostname,
                                           const QByteArray &macAddress,
                                           QString *reason)
{
    const auto fail = [reason](const QString &text) {
        if (reason)
            *reason = text;
        return -1;
    };

    // Virtual networks run entirely in-process (in-memory switch plus
    // embedded libslirp uplink) - no namespaces, no external tools.
    const VirtualNetwork net = m_network_manager->network(networkName);
    if (!net.isValid())
        return fail(i18n("Virtual network '%1' not found or invalid: %2", networkName, m_network_manager->lastError()));

    if (!m_network_manager->startNetwork(networkName))
        return fail(i18n("Failed to start virtual network '%1': %2", networkName, m_network_manager->lastError()));

    const int guestFd = m_network_manager->attachVm(networkName, vmName, dnsHostname, macAddress);
    if (guestFd < 0)
        return fail(i18n("Failed to attach VM '%1' to the network: %2", vmName, m_network_manager->lastError()));

    qCDebug(KEMU) << "VmRuntimeManager: VM" << vmName << "uses switch port fd" << guestFd;
    return guestFd;
}

void VmRuntimeManager::releaseNetwork(const QString &vmName)
{
    // By name, not by the VM's current configuration: the attachment has to
    // go even when the configuration changed or the entry is already gone.
    if (m_network_manager)
        m_network_manager->detachVmFromAll(vmName);
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
    info.guestPoweredOff = rt.guestPoweredOff && !rt.userInitiatedShutdown;
    info.shutdownHostWithGuest = rt.shutdownHostWithGuest;

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

void VmRuntimeManager::setUpQmpClient(const LaunchRequest &req, int monitorFd, bool isResume)
{
    const QString vmName = req.vmName;

    auto *client = new QMPClient(this);
    m_model->setQmpClient(vmName, client);

    connect(client, &QMPClient::eventReceived, this, [this, vmName](const QString &eventName, const QJsonObject &data) {
        // Also sent after KEMU's own system_powerdown; onQemuExit() filters that out.
        if (eventName == u"SHUTDOWN"_s && data.value(u"guest"_s).toBool() && data.value(u"reason"_s).toString() == u"guest-shutdown"_s)
            runtimeFor(vmName).guestPoweredOff = true;

        // qemu-ga opened or closed its port in the guest.
        if (eventName == u"VSERPORT_CHANGE"_s && data.value(u"id"_s).toString() == KEMUConst::GuestAgentPortId) {
            if (GuestAgentClient *agent = runtimeFor(vmName).guestAgent)
                agent->setPortOpen(data.value(u"open"_s).toBool());
        }

        Q_EMIT qmpEvent(vmName, eventName, data);
    });

    connect(client, &QMPClient::connected, this, [this, client, vmName, deployment = req.deployment, isResume]() {
        // The agent may have been running before anyone listened for
        // VSERPORT_CHANGE.
        refreshGuestAgentPort(vmName);

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

    // The monitor descriptor is connected from the start, so there is nothing
    // to wait for: QEMU's greeting arrives on its own.
    client->connectToVm(monitorFd);
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
