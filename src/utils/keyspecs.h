/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QString>

#include "utils/vmconfig.h"

/**
 * @brief Short texts for the tiles of a VM, such as its CPU cores, memory and system disk.
 *
 * In one place, so each figure is said the same way and in the same units
 * wherever it is shown.
 */
namespace KeySpecs
{
[[nodiscard]] QString coresText(int cores);
/// The CPU model in a few words, without the year, such as "Intel Haswell".
[[nodiscard]] QString cpuModelText(VMSpecs::CpuModel model);
/// The chipset's generation, "Modern", "Legacy" or "Vintage", and what it is, such as "Q35 + PCIe".
[[nodiscard]] QString chipsetText(VMSpecs::MachineType type);
[[nodiscard]] QString chipsetDetail(VMSpecs::MachineType type);
/// As the memory spin box writes it: MiB below 1 GiB, GiB above, with at least @p digits.
[[nodiscard]] QString memoryText(double mib, int digits = 3);
[[nodiscard]] QString diskSizeText(int gb);
/// Controller and state, such as "VirtIO, encrypted".
[[nodiscard]] QString diskFacts(VMSpecs::DiskInterface diskInterface, bool encrypted, bool isNew = false);
/// Whole GiB, rounded up as the settings show a disk image.
[[nodiscard]] int gibRoundedUp(qint64 bytes);

/// The host's CPUs and memory; 0 if unknown.
[[nodiscard]] int hostCpuCount();
[[nodiscard]] qint64 hostMemoryMib();
/// "Host: 16" and "Host: 31 GiB"; empty if unknown.
[[nodiscard]] QString hostCpusText();
[[nodiscard]] QString hostMemoryText();
}
