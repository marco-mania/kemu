/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QString>

namespace DrmDevice
{
// Whether a DRM node's sysfs entry, e.g. /sys/class/drm/card0, belongs to a
// virtio-gpu device. Its "device" is the virtio device bound to virtio_gpu
// (virtio-mmio), or the PCI device bound to virtio-pci, whose virtioN child
// is bound to virtio_gpu.
bool isVirtioGpu(const QString &entryPath);
}
