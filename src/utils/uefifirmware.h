/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QString>
#include <QStringList>

// UEFI firmware (OVMF) for x86_64 guests in the form KEMU boots it: two flash
// images, the read-only code and a template for the variable store that each
// VM gets a copy of.
namespace Ovmf
{

struct Firmware {
    QString codePath;
    QString varsPath;
    // The QEMU firmware descriptor, or the directory searched by file name.
    QString source;
    bool secureBoot = false;
    // The template carries the Microsoft keys Secure Boot checks against.
    bool keysEnrolled = false;

    [[nodiscard]] bool isValid() const
    {
        return !codePath.isEmpty() && !varsPath.isEmpty();
    }
};

// Reads QEMU firmware descriptors (docs/interop/firmware.json in QEMU), as
// libvirt does: a file name in an earlier directory masks the same name in a
// later one, and the remaining files count in the order of their names. File
// names inside are taken relative to @p root, e.g. a host system mounted into
// a sandbox.
[[nodiscard]] Firmware fromDescriptors(const QStringList &dirs, bool secureBoot, const QString &root = {});

// Looks for the file names distributions use, code and template from one
// directory and of the same size.
[[nodiscard]] Firmware fromFileNames(const QStringList &dirs, bool secureBoot);

} // namespace Ovmf
