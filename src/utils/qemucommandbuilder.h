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

    /// Encrypted disks: descriptor QEMU reads the password from. Below zero,
    /// DiskEncryption::ScriptSecretFd (export scripts).
    int diskKeyFd = -1;

    /// Guest agent channel. Below zero, QEMU listens on a unix socket in the
    /// VM folder instead (export scripts).
    int guestAgentFd = -1;

    /// kemu-agent channel (D-Bus display only). Below zero, the guest gets
    /// no agent port.
    int agentFd = -1;
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
     * @param incomingSavestatePath  Path to file with saved state.
     *
     * The display is always KEMU's integrated viewer (-display dbus,p2p=on),
     * whether a viewer is attached yet or not.
     */
    static QStringList build(const VMConfig &config,
                             const QDir &vmDir,
                             const QString &monitorSocketPath,
                             const QemuNetworkContext &networkContext = {},
                             const bool relativePaths = false,
                             const QString &incomingSavestatePath = {});

    /**
     * @brief The QEMU arguments of an exported script (buildShellScript()).
     *
     * As build(), but with QEMU's own SDL window instead of the integrated
     * viewer: a script runs without KEMU as the viewer's peer.
     */
    static QStringList buildForScript(const VMConfig &config,
                                      const QDir &vmDir,
                                      const QString &monitorSocketPath,
                                      const QemuNetworkContext &networkContext = {},
                                      const bool relativePaths = false);

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
     * @param seccompSandbox     Whether to emit the -sandbox block, so an
     *                           exported script runs the way KEMU would.
     */
    static QString buildShellScript(const VMConfig &config,
                                    const QDir &vmDir,
                                    const QString &monitorSocketPath,
                                    const QemuNetworkContext &networkContext,
                                    const VirtualNetwork &network = {},
                                    const bool relativePaths = false,
                                    const bool seccompSandbox = false);

    /**
     * @brief The -sandbox option KEMU confines QEMU with.
     *
     * Empty unless @p enabled, which carries the global setting: the filter
     * denies spawn, privilege changes and resource control, which some guest
     * and host features need. Also empty when the VM's extra arguments bring
     * their own -sandbox, as QEMU installs one filter per occurrence and a
     * later "-sandbox off" could not lift ours. And empty for a VM shown in
     * the @p viewer that uses Vulkan (usesVenus()): virglrenderer starts a
     * program of its own for it. The caller checks QemuCapabilities first.
     */
    static QStringList sandboxArgs(const VMConfig &config, bool enabled, bool viewer = true);

    /**
     * @brief The -device value of the VirtIO graphics adapter.
     *
     * @p gl selects virtio-vga-gl (virgl). @p blob adds host blobs with a
     * host memory window of VMConfig::gpuHostmemMb(); it only applies
     * together with @p gl. @p viewer: for the integrated viewer, which shows
     * several heads, rather than an exported script's SDL window. @p venus
     * adds Vulkan and @p nativeContext a DRM native context; both only apply
     * together with @p blob. As with sandboxArgs(), the caller checks
     * QemuCapabilities first.
     */
    static QString virtioGpuDevice(const VMConfig &config, bool gl, bool blob, bool viewer = true, bool venus = false, bool nativeContext = false);

    /**
     * @brief Whether host blobs can be used on this host: QEMU 9.2+ and a
     * host kernel 6.13+ (see QemuCapabilities).
     */
    static bool hostBlobsAvailable();

    /**
     * @brief Whether Vulkan (Venus) can be used on this host: host blobs and
     * virglrenderer's render server (QemuCapabilities::virglRenderServer()).
     */
    static bool venusAvailable();

    /**
     * @brief Whether a DRM native context can be used on this host: host
     * blobs and a QEMU with drm_native_context (11.0+). Whether
     * virglrenderer supports the host GPU is not checked; without support
     * the guest keeps virgl.
     */
    static bool nativeContextAvailable();

    /**
     * @brief Whether @p config gets host blobs (blob=on) when started for the
     * integrated viewer: 3D and direct GPU memory access are on, and the host
     * has what they need. Exported scripts never use them.
     */
    static bool usesHostBlobs(const VMConfig &config);

    /**
     * @brief Whether @p config gets Vulkan (venus=on) when started for the
     * integrated viewer: 3D, host blobs and Vulkan are on, and the host has
     * what they need. Exported scripts never use it.
     */
    static bool usesVenus(const VMConfig &config);

    /**
     * @brief Whether @p config gets a DRM native context
     * (drm_native_context=on) when started for the integrated viewer: 3D,
     * host blobs and the native context are on, and the host has what they
     * need. Exported scripts never use it.
     */
    static bool usesNativeContext(const VMConfig &config);

    /**
     * @brief Formats the QEMU argument list into a readable multi-line string.
     * @param args  The list of QEMU arguments.
     * @return A formatted string with line continuation characters.
     */
    static QString argsToReadableString(const QStringList &args);

private:
    QemuCommandBuilder() = delete;

    /// Where the VM is shown: KEMU's integrated viewer, or QEMU's own SDL
    /// window for exported scripts, which run without KEMU as the viewer's
    /// peer.
    enum class Frontend {
        Viewer,
        ScriptWindow,
    };

    static QStringList buildArgs(const VMConfig &config,
                                 const QDir &vmDir,
                                 const QString &monitorSocketPath,
                                 const QemuNetworkContext &networkContext,
                                 bool relativePaths,
                                 const QString &incomingSavestatePath,
                                 Frontend frontend);
};
