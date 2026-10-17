/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QList>
#include <QString>

#include <optional>

class QIODevice;

struct OvfDisk {
    QString href; // as written in the descriptor, untrusted; empty: blank disk
    qint64 capacityBytes = 0; // 0: not specified
    QString compression; // ovf:compression, e.g. "gzip"
    qint64 chunkSize = 0; // ovf:chunkSize; > 0: split into numbered chunks

    [[nodiscard]] bool isBlank() const
    {
        return href.isEmpty();
    }
};

/// What an OVF descriptor says about its first virtual system. Values the
/// descriptor leaves out are empty or 0; choosing defaults is up to the caller.
struct OvfDescriptor {
    QString name;
    int virtualSystemCount = 0; // > 1 for a VirtualSystemCollection
    int cpuCount = 0;
    qint64 memoryBytes = 0;
    QString macAddress; // first network adapter, "aa:bb:cc:dd:ee:ff"
    QList<OvfDisk> disks; // attachment order, boot disk first
};

/// Reader for OVF 1.x/2.x descriptors as written by VirtualBox and VMware.
class OvfParser
{
public:
    [[nodiscard]] static std::optional<OvfDescriptor> parse(QIODevice *device);
    [[nodiscard]] static std::optional<OvfDescriptor> parseFile(const QString &path);

    /// Converts an OVF quantity plus allocation units to bytes. Handles the
    /// programmatic form ("byte * 2^20", "byte * 10^9") and textual units
    /// ("MegaBytes", "GB"). Returns 0 for non-positive or overflowing values.
    [[nodiscard]] static qint64 quantityToBytes(qint64 quantity, const QString &units, qint64 defaultUnitBytes);
};
