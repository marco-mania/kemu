/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QStringList>

#include <KProcess>

#include "utils/virtualnetwork.h"
#include "utils/vmconfig.h"

using namespace Qt::StringLiterals;

/**
 * @brief Network context passed to QemuCommandBuilder::build().
 *
 * For NetworkMode::VirtualNetwork (namespace-based):
 *   - tapDevice is set (e.g. "tap-myvm"), passtSocketPath is empty, useSlirp is false.
 *
 * For NetworkMode != VirtualNetwork, exactly one of:
 *   - useSlirp is true: QEMU's built-in usermode networking is used, no
 *     helper process required.
 *   - useSlirp is false (default): passt is used via passtSocketPath.
 */
struct NetworkContext {
    /// Tap device name inside the namespace (VirtualNetwork mode).
    QString tapDevice;

    /// Open descriptor for that tap device. When set, QEMU receives the
    /// descriptor rather than the name and runs outside the namespace.
    int tapFd = -1;

    /// Unix socket path for passt (Passt mode).
    QString passtSocketPath;

    /// Use QEMU's built-in "-netdev user" backend instead of passt.
    bool useSlirp = false;
};

class QemuCommandBuilder
{
public:
    /**
     * @brief Constructs the complete argument list for the QEMU process.
     *
     * @param config                 The VM configuration.
     * @param vmDir                  The directory containing the VM files (disk, VARS).
     * @param monitorSocketPath      Path to the QMP socket.
     * @param networkContext         Network-mode-specific context (tap device or passt socket).
     * @param relativePaths          Whether to emit relative paths.
     * @param headless               Whether to suppress display output.
     * @param incomingSavestatePath  Path to file with saved state.
     */
    static QStringList build(const VMConfig &config,
                             const QDir &vmDir,
                             const QString &monitorSocketPath,
                             const NetworkContext &networkContext = {},
                             const bool relativePaths = false,
                             const bool headless = false,
                             const QString &incomingSavestatePath = {});

    /**
     * @brief Generates a complete, self-contained shell script to launch the VM.
     *
     * Includes all helper processes (swtpm, virtiofsd, namespace setup,
     * dnsmasq, pasta) and the QEMU invocation.
     *
     * @param config             The VM configuration.
     * @param vmDir              The directory containing the VM files.
     * @param monitorSocketPath  Path to the QMP socket.
     * @param networkContext     Network-mode-specific context.
     * @param network            Virtual network config (if applicable).
     * @param relativePaths      Whether to emit relative paths (for portable scripts).
     */
    static QString buildShellScript(const VMConfig &config,
                                    const QDir &vmDir,
                                    const QString &monitorSocketPath,
                                    const NetworkContext &networkContext,
                                    const VirtualNetwork &network = {},
                                    const bool relativePaths = false);

    /**
     * @brief Formats the QEMU argument list into a readable multi-line string.
     * @param args  The list of QEMU arguments.
     * @return A formatted string with line continuation characters.
     */
    static QString argsToReadableString(const QStringList &args);

private:
    QemuCommandBuilder() = delete;
};
