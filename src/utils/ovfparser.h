/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

class QIODevice;

enum class OvfStorageBus {
    Unknown,
    Ide,
    Scsi,
    Sata,
    Nvme
};

enum class OvfFirmware {
    Unknown,
    Bios,
    Efi
};

struct OvfDisk {
    QString href; // as written in the descriptor, untrusted; empty: blank disk
    qint64 capacityBytes = 0; // 0: not specified
    QString compression; // ovf:compression, e.g. "gzip"
    qint64 chunkSize = 0; // ovf:chunkSize; > 0: split into numbered chunks
    OvfStorageBus bus = OvfStorageBus::Unknown; // controller the disk is attached to

    [[nodiscard]] bool isBlank() const
    {
        return href.isEmpty();
    }
};

struct OvfNetworkAdapter {
    QString model; // rasd:ResourceSubType as written, e.g. "E1000", "PCNet32"
    QString macAddress; // "aa:bb:cc:dd:ee:ff", empty if unknown
};

/// What an OVF descriptor says about its first virtual system. Values the
/// descriptor leaves out are empty or 0; choosing defaults is up to the caller.
struct OvfDescriptor {
    QString name;
    QString annotation; // AnnotationSection, the VM's description
    QStringList licenses; // EulaSection texts; OVF requires accepting them before deployment
    int virtualSystemCount = 0; // > 1 for a VirtualSystemCollection
    int cpuCount = 0;
    qint64 memoryBytes = 0;
    QList<OvfNetworkAdapter> networkAdapters;
    QList<OvfDisk> disks; // attachment order, boot disk first

    // Vendor extensions; OVF itself has no firmware setting.
    OvfFirmware firmware = OvfFirmware::Unknown;
    bool secureBoot = false;

    int osId = 0; // CIM operating system id, 0: not specified
    QString osType; // vendor id, e.g. "debian10_64Guest" or "Debian_64"
    QString osDescription;

    [[nodiscard]] QString firstMacAddress() const
    {
        for (const OvfNetworkAdapter &adapter : networkAdapters) {
            if (!adapter.macAddress.isEmpty())
                return adapter.macAddress;
        }
        return {};
    }
};

struct OvfManifestEntry {
    QString fileName;
    QCryptographicHash::Algorithm algorithm = QCryptographicHash::Sha256;
    QByteArray digest; // lower-case hex
};

/// Reader for OVF 0.9, 1.x and 2.x descriptors as common hypervisors write them.
class OvfParser
{
public:
    [[nodiscard]] static std::optional<OvfDescriptor> parse(QIODevice *device);
    [[nodiscard]] static std::optional<OvfDescriptor> parseFile(const QString &path);

    /// Reads an OVA manifest ("SHA256(file)= digest" per line). Fails on any
    /// line it does not understand.
    [[nodiscard]] static std::optional<QList<OvfManifestEntry>> parseManifest(QIODevice *device);

    /// Converts an OVF quantity plus allocation units to bytes. Handles the
    /// programmatic form ("byte * 2^20", "byte * 10^9") and textual units
    /// ("MegaBytes", "GB"). Returns 0 for non-positive or overflowing values.
    [[nodiscard]] static qint64 quantityToBytes(qint64 quantity, const QString &units, qint64 defaultUnitBytes);
};
