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
 * @brief Runtime wiring passed to QemuCommandBuilder::build().
 *
 * The descriptors belong to their owners (VirtualSwitch, UserModeNetwork,
 * VmRuntimeManager); the builder only writes their numbers into the command
 * line, and the launcher clears their CLOEXEC flag for the child.
 *
 * For NetworkMode::VirtualNetwork:
 *   - switchFd is set: QEMU gets one end of the socketpair that is a port of
 *     the running network's in-memory L2 switch.
 *   - mcastAddress is set (export scripts only): QEMU's built-in multicast
 *     networking is used instead, because a standalone script can neither
 *     inherit a descriptor nor reach the switch inside the KEMU process.
 *
 * For NetworkMode::UserMode, exactly one of:
 *   - useSlirp is true (export scripts only): QEMU's built-in usermode
 *     networking is used, because a standalone script cannot reach the
 *     slirp instance living inside the KEMU process.
 *   - useSlirp is false (default): userNetFd is one end of the socketpair
 *     KEMU's in-process user-mode network switches.
 */
struct QemuNetworkContext {
    /// Virtual network: descriptor of this VM's switch port.
    int switchFd = -1;

    /// User mode: descriptor of KEMU's in-process slirp network.
    int userNetFd = -1;

    /// QMP monitor descriptor. Below zero, QEMU opens a listening unix
    /// socket at monitorSocketPath instead (export scripts).
    int monitorFd = -1;

    /// "group:port" for QEMU's socket,mcast= backend (export scripts only).
    QString mcastAddress;

    /// Use QEMU's built-in "-netdev user" backend (export scripts only).
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
     * @param networkContext         Network-mode-specific context (switch or user-mode socket).
     * @param relativePaths          Whether to emit relative paths.
     * @param headless               Whether to suppress display output.
     * @param incomingSavestatePath  Path to file with saved state.
     */
    static QStringList build(const VMConfig &config,
                             const QDir &vmDir,
                             const QString &monitorSocketPath,
                             const QemuNetworkContext &networkContext = {},
                             const bool relativePaths = false,
                             const bool headless = false,
                             const QString &incomingSavestatePath = {});

    /**
     * @brief Generates a complete, self-contained shell script to launch the VM.
     *
     * Includes all helper processes (swtpm, virtiofsd) and the QEMU
     * invocation. Virtual networks are exported on QEMU's built-in multicast
     * networking, since a standalone script cannot reach the in-memory switch
     * inside the KEMU process.
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
                                    const QemuNetworkContext &networkContext,
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
