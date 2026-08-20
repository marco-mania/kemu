/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

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
inline constexpr auto VMConfigFileName = "config.json"_L1;
inline constexpr auto VMDefaultIcon = "computer"_L1;

inline constexpr auto OsIconDirPath = "kemu/icons"_L1;

inline constexpr auto DefaultMinDiskSizeGB = 8;
inline constexpr auto DefaultZSTDCompressionLevelForExport = 10;
inline constexpr auto DefaultDisplaySize = QSize(1280, 780);
inline constexpr auto DefaultDisplayType = "GTK"_L1; //

// Hardcoded custom paths for external tools not included in $PATH
inline const QStringList FallbackPaths = {u"/usr/libexec"_s, // Fedora, RHEL, openSUSE
                                          u"/usr/lib/qemu"_s, // Debian, Ubuntu
                                          u"/usr/lib"_s, // General lib directory
                                          u"/usr/local/libexec"_s, // Custom builds
                                          u"/usr/sbin"_s};

inline constexpr auto DBUSSocketFileName = "dbus-bus.sock"_L1;
inline constexpr auto DBUSConfigFileName = "dbus-bus.conf"_L1;
inline constexpr auto PasstSocketFileName = "passt.sock"_L1;
inline constexpr auto SwtpmSocketFileName = "swtpm.sock"_L1;
inline constexpr auto VirtiofsdSocketFileName = "virtiofsd.sock"_L1;
inline constexpr auto VirtiofsdPidFileName = "virtiofsd.sock.pid"_L1;
inline constexpr auto QemuMonitorSocketFileName = "qemu-monitor.sock"_L1;
inline constexpr auto DeploySerialLogFileName = "deploy-serial.log"_L1;

} // namespace KEMUConst
