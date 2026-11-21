/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "drmdevice.h"

#include <QDir>
#include <QFileInfo>

using namespace Qt::StringLiterals;

namespace
{
// The name of the driver a sysfs device directory is bound to.
QString driverOf(const QString &deviceDir)
{
    return QFileInfo(QFileInfo(deviceDir + u"/driver"_s).symLinkTarget()).fileName();
}
} // namespace

bool DrmDevice::isVirtioGpu(const QString &entryPath)
{
    const QString device = entryPath + u"/device"_s;
    const QString driver = driverOf(device);
    if (driver == u"virtio_gpu"_s)
        return true;
    if (driver != u"virtio-pci"_s)
        return false;
    const QStringList children = QDir(device).entryList({u"virtio*"_s}, QDir::Dirs | QDir::NoDotAndDotDot | QDir::System);
    for (const QString &child : children) {
        if (driverOf(device + u'/' + child) == u"virtio_gpu"_s)
            return true;
    }
    return false;
}
