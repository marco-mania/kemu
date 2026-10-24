/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QSize>
#include <QStringList>

using namespace Qt::StringLiterals;

namespace KEMUConst
{

inline constexpr auto DetailViewIconSize = 64; // pixel x pixel
inline constexpr auto ListViewIconSize = 32; // pixel x pixel

inline constexpr auto CustomIconStoreSize = 256; // pixel x pixel
inline constexpr auto CustomIconFileNameBase = "icon"_L1;
inline constexpr auto CustomIconFileFormat = "PNG"_L1;

inline constexpr auto VMDiskFileName = "disk.qcow2"_L1;
inline constexpr int MaxExtraDisks = 8;
inline constexpr int MaxSmbiosStringLength = 64;
inline constexpr auto VMConfigFileName = "config.json"_L1;
inline constexpr auto OvmfVarsFileName = "OVMF_VARS.fd"_L1;
inline constexpr auto VMDefaultIcon = "computer"_L1;

inline constexpr auto OsIconDirPath = "kemu/icons"_L1;
// Bundled QEMU logo, used for the guest agent and the About dialog.
inline constexpr auto QemuLogoPath = "kemu/icons/misc-icons/qemu.svg"_L1;

inline constexpr auto TemplateDbFilePath = "kemu/data/templates.json"_L1;
inline constexpr auto TemplateDropInDirPath = "kemu/data/templates.d"_L1;
inline constexpr auto CloudInitTemplateDirPath = "kemu/data/cloud-init-templates"_L1;
// Hidden template ids, in the user's data dir only.
inline constexpr auto HiddenTemplatesFilePath = "kemu/data/hidden-templates.json"_L1;
// Content installed from a download service, one package per subdirectory.
inline constexpr auto DownloadedTemplateDirPath = "kemu/ghns/templates"_L1;
inline constexpr auto DownloadedCloudInitDirPath = "kemu/ghns/cloud-init"_L1;

inline constexpr auto DefaultTemplateSortOrder = 500;
inline constexpr auto DefaultMinDiskSizeGB = 8;
inline constexpr auto DefaultZSTDCompressionLevelForExport = 10;
inline constexpr auto DefaultDisplaySize = QSize(1280, 720);

// Extra directories for QStandardPaths::findExecutable(), covering helpers that
// distributions install outside $PATH.
//
// This has to be a function rather than an inline const: applicationDirPath()
// is only meaningful once QApplication exists, and a namespace-scope object
// would be initialised before main() runs.
//
// Deliberately no /run/host entries. Unlike firmware, a host binary cannot be
// executed from inside a sandbox - it would be linked against libraries the
// image does not have.
inline QStringList fallbackPaths()
{
    const QDir prefix(QCoreApplication::applicationDirPath() + u"/.."_s);

    return {
        // Helpers shipped alongside our own binaries always win.
        prefix.absoluteFilePath(u"libexec"_s),
        prefix.absoluteFilePath(u"lib/qemu"_s),

        u"/usr/libexec"_s, // Fedora, RHEL, openSUSE
        u"/usr/lib/qemu"_s, // Debian, Ubuntu
        u"/usr/lib"_s, // General lib directory
        u"/usr/local/libexec"_s, // Custom builds
        u"/usr/sbin"_s,
    };
}

inline constexpr auto SwtpmSocketFileName = "swtpm.sock"_L1;
inline constexpr auto VirtiofsdSocketFileName = "virtiofsd.sock"_L1;
inline constexpr auto VirtiofsdPidFileName = "virtiofsd.sock.pid"_L1;
inline constexpr auto QemuMonitorSocketFileName = "qemu-monitor.sock"_L1;
inline constexpr auto DeploySerialLogFileName = "deploy-serial.log"_L1;

// Net client id used for -netdev; also the `name` argument of QMP set_link.
inline constexpr auto NetdevId = "nic0"_L1;

// Chardev id of the QMP monitor when it rides on an inherited descriptor.
inline constexpr auto QmpChardevId = "qmp0"_L1;

// QEMU guest agent: chardev, virtio-serial port and the name qemu-ga looks
// for. Export scripts let QEMU listen on GuestAgentSocketFileName instead.
inline constexpr auto GuestAgentChardevId = "qga0"_L1;
inline constexpr auto GuestAgentPortId = "qga0port"_L1;
inline constexpr auto GuestAgentPortName = "org.qemu.guest_agent.0"_L1;
inline constexpr auto GuestAgentSocketFileName = "qemu-ga.sock"_L1;

// QEMU's seccomp filter, as libvirt applies it. Resource control calls fail
// with EPERM; anything else on the deny lists kills QEMU with SIGSYS. Opt-in
// through the "Confine QEMU with its seccomp sandbox" setting, because that
// SIGSYS is indistinguishable from a QEMU bug and hides diagnostics.
inline constexpr auto QemuSandboxOptions = "on,obsolete=deny,elevateprivileges=deny,spawn=deny,resourcecontrol=deny"_L1;

inline static constexpr int kExitPolicyTimeoutMs = 120'000;

// Time to cancel a host shutdown that follows a guest power-off.
inline constexpr int kHostShutdownCountdownSec = 30;

} // namespace KEMUConst
