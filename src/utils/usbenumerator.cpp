// SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "usbenumerator.h"
#include <libudev.h>

QString UsbDeviceInfo::displayName() const
{
    if (!manufacturer.isEmpty() && !name.contains(manufacturer))
        return manufacturer + u" "_s + name;
    return name;
}

QString UsbDeviceInfo::idString() const
{
    return QStringLiteral("%1:%2").arg(vendorId, 4, 16, QChar(u'0')).arg(productId, 4, 16, QChar(u'0'));
}

QString UsbDeviceInfo::busKey() const
{
    return QStringLiteral("%1:%2").arg(busNum).arg(devAddr);
}

QString UsbDeviceInfo::qemuId() const
{
    return QStringLiteral("usb-pt-%1-%2").arg(busNum).arg(devAddr);
}

QList<UsbDeviceInfo> UsbEnumerator::enumerate()
{
    QList<UsbDeviceInfo> devices;

    struct udev *ctx = udev_new();
    if (!ctx)
        return devices;

    struct udev_enumerate *e = udev_enumerate_new(ctx);
    udev_enumerate_add_match_subsystem(e, "usb");
    udev_enumerate_add_match_property(e, "DEVTYPE", "usb_device");
    udev_enumerate_scan_devices(e);

    struct udev_list_entry *entry;
    udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(e))
    {
        struct udev_device *dev = udev_device_new_from_syspath(ctx, udev_list_entry_get_name(entry));
        if (!dev)
            continue;

        auto attr = [&](const char *a) -> QString {
            const char *v = udev_device_get_sysattr_value(dev, a);
            return v ? QString::fromUtf8(v).trimmed() : QString();
        };

        const int devClass = attr("bDeviceClass").toInt(nullptr, 16);
        const uint16_t vid = attr("idVendor").toUInt(nullptr, 16);
        const uint16_t pid = attr("idProduct").toUInt(nullptr, 16);

        // Skip hubs and root hubs
        if (devClass == 9 || (vid == 0 && pid == 0)) {
            udev_device_unref(dev);
            continue;
        }

        UsbDeviceInfo info;
        info.vendorId = vid;
        info.productId = pid;
        info.busNum = attr("busnum").toInt();
        info.devAddr = attr("devnum").toInt();
        info.manufacturer = attr("manufacturer");
        info.name = attr("product");
        if (info.name.isEmpty())
            info.name = QStringLiteral("Unknown Device");

        devices.append(info);
        udev_device_unref(dev);
    }

    udev_enumerate_unref(e);
    udev_unref(ctx);

    std::sort(devices.begin(), devices.end(), [](const UsbDeviceInfo &a, const UsbDeviceInfo &b) {
        return a.busNum != b.busNum ? a.busNum < b.busNum : a.devAddr < b.devAddr;
    });

    return devices;
}
