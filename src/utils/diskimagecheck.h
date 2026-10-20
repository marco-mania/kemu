/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QJsonValue>
#include <QString>

// Disk images from archives, appliances or downloads are untrusted. Formats
// like qcow2 and VMDK can name other files (backing files, external data
// files, VMDK extents); QEMU or qemu-img would then read host files into
// the guest. These checks reject such images before they are used.
namespace DiskImageCheck
{

// Reads the header directly, no qemu-img needed. Empty if @p path is a
// qcow2 image that stands on its own, otherwise the reason it is not.
[[nodiscard]] QString qcow2Problem(const QString &path);

struct Inspection {
    QString format; // as detected by qemu-img
    QString problem; // empty: safe to convert with -f format
};

// Runs qemu-img info on @p path. Files the image refers to are only
// accepted inside @p allowedDir (none if empty), backing files only if
// @p allowBacking.
[[nodiscard]] Inspection inspect(const QString &qemuImg, const QString &path, const QString &allowedDir, bool allowBacking, int timeoutMs = 30000);

// The evaluation behind inspect(): @p info is the output of qemu-img info
// --output=json, with --backing-chain if @p allowBacking.
[[nodiscard]] Inspection evaluate(const QJsonValue &info, const QString &path, const QString &allowedDir, bool allowBacking);

}
