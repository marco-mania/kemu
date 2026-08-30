/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "vmruntimemanager.h"

#include "models/virtualenvironmentmodel.h"
#include "utils/externaltools.h"
#include "utils/sandboxcapabilities.h"
#include "utils/virtualnetworkmanager.h"

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
