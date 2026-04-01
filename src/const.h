/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QSize>

#pragma once

using namespace Qt::StringLiterals;

namespace KEMUConst {

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

}
