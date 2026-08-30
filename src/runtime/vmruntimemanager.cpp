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
    connect(process, &QProcess::readyReadStandardError, this, [this, process, vmName]() {
        Utils::appendTail(runtimeFor(vmName).stderrTail, process->readAllStandardError(), kStderrTailLimit);
    });

    // Never combine this with QProcess::UnixProcessParameters::CloseFileDescriptors:
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

    const QDir dir = vmDir(vmName);

    // Detach signal sources first so no slot fires during teardown.
    if (entry->qmpClient) {
        entry->qmpClient->disconnect();
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

    QFile::remove(dir.filePath(KEMUConst::SwtpmSocketFileName));
    QFile::remove(dir.filePath(KEMUConst::VirtiofsdPidFileName));
    QFile::remove(dir.filePath(KEMUConst::PasstSocketFileName));
    QFile::remove(vmDir(vmName).absoluteFilePath(KEMUConst::QemuMonitorSocketFileName));

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
    const QString vmPath = vmDir(vmName).absolutePath();
    const QString sockPath = vmPath + u'/' + KEMUConst::SwtpmSocketFileName;
    QFile::remove(sockPath);

    AsyncHelperProcess::Options opts;
    opts.program = ExternalTools::instance().path(ExternalTools::Swtpm);
    opts.arguments =
        {u"socket"_s, u"--ctrl"_s, u"type=unixio,path=%1"_s.arg(sockPath), u"--terminate"_s, u"--tpmstate"_s, u"dir=%1"_s.arg(vmPath), u"--tpm2"_s};
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

    const QString sockPath = vmDir(vmName).absolutePath() + u'/' + KEMUConst::VirtiofsdSocketFileName;
    const QString pidPath = vmDir(vmName).absolutePath() + u'/' + KEMUConst::VirtiofsdPidFileName;
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

    const QString sockPath = vmDir(vmName).absolutePath() + u'/' + KEMUConst::PasstSocketFileName;
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

void VmRuntimeManager::onQemuExit(const QString &vmName, int exitCode, KProcess::ExitStatus exitStatus)
{
    const VmEntry *entry = m_model->entryByName(vmName);
    if (!entry)
        return;

    QPointer<KProcess> p = entry->qemuProcess;
    auto &rt = runtimeFor(vmName);
    const QString vmPath = vmDir(vmName).absolutePath();

    ExitInfo info;
    info.vmName = vmName;
    info.exitCode = exitCode;
    info.exitStatus = exitStatus;
    info.abnormal = (exitStatus != KProcess::NormalExit) || (exitCode != 0);
    info.userInitiated = rt.userInitiatedShutdown;
    info.startupFailureAlreadyShown = rt.startupFailureShown;
    info.wasDeploying = entry->state == VmEntry::State::Deploying;
    info.savestatePresent = SaveStateManager::hasSavestate(vmPath, vmName);

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
