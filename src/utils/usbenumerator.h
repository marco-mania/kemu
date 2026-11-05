/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QList>
#include <QString>

struct UsbDeviceInfo {
    QString name;
    QString manufacturer;
    uint16_t vendorId = 0;
    uint16_t productId = 0;
    int busNum = 0;
    int devAddr = 0;

    QString displayName() const;
    QString idString() const; // "0951:1666"
    QString busKey() const; // "1:4" (bus:addr)
    QString qemuId() const; // "usb-pt-1-4"
};

using namespace Qt::StringLiterals;

namespace UsbEnumerator
{
QList<UsbDeviceInfo> enumerate();

// The node usb-host opens, such as /dev/bus/usb/003/025.
QString devicePath(int busNum, int devAddr);
// 0 if this process may open the node for reading and writing, the errno
// otherwise. QEMU runs with the same rights, in the same sandbox.
int accessError(int busNum, int devAddr);
}
