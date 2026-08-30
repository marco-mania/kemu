/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "vmruntimemanager.h"

VmRuntimeManager::VmRuntimeManager(VirtualEnvironmentModel *model, VirtualNetworkManager *networks, QObject *parent)
    : QObject(parent)
    , m_model(model)
    , m_networks(networks)
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
