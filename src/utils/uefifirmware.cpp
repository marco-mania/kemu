/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "uefifirmware.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QtEndian>

#include <algorithm>
#include <cstring>

using namespace Qt::StringLiterals;

namespace Ovmf
{

namespace
{

// The flash image of a descriptor, if KEMU can boot it: a raw file that exists.
QString flashFile(const QJsonObject &image, const QString &root)
{
    if (image.value(u"format"_s).toString(u"raw"_s) != u"raw"_s)
        return {};
    const QString fileName = image.value(u"filename"_s).toString();
    if (fileName.isEmpty())
        return {};
    const QString path = root + fileName;
    return QFileInfo::exists(path) ? path : QString();
}

// Whether a descriptor is for the x86_64 PCs KEMU starts. Its machines are
// globs of versioned QEMU machine names; other machines may need other
// firmware: openSUSE's 50-xen-hvm-x86_64.json, for Xen's "xenfv", comes with
// a template holding the Microsoft keys, and sorting before
// 60-ovmf-x86_64.json, it could turn Secure Boot on for VMs without it. A
// target without machines is taken to fit, as before.
bool targetsKemuMachines(const QJsonArray &targets)
{
    static const QStringList kemuMachines = {u"pc-q35-99.0"_s, u"pc-i440fx-99.0"_s};
    return std::any_of(targets.cbegin(), targets.cend(), [](const QJsonValue &value) {
        const QJsonObject target = value.toObject();
        if (target.value(u"architecture"_s).toString() != u"x86_64"_s)
            return false;
        const QJsonArray machines = target.value(u"machines"_s).toArray();
        if (machines.isEmpty())
            return true;
        return std::any_of(machines.cbegin(), machines.cend(), [](const QJsonValue &machine) {
            const QRegularExpression glob(QRegularExpression::wildcardToRegularExpression(machine.toString(), QRegularExpression::NonPathWildcardConversion));
            return std::any_of(kemuMachines.cbegin(), kemuMachines.cend(), [&glob](const QString &name) {
                return glob.match(name).hasMatch();
            });
        });
    });
}

QString firstExisting(const QDir &dir, const QStringList &names)
{
    for (const QString &name : names) {
        if (dir.exists(name))
            return dir.filePath(name);
    }
    return {};
}

} // namespace

Firmware fromDescriptors(const QStringList &dirs, bool secureBoot, const QString &root)
{
    QMap<QString, QString> descriptors; // file name, path; sorted by name
    for (const QString &dir : dirs) {
        const QFileInfoList entries = QDir(root + dir).entryInfoList({u"*.json"_s}, QDir::Files | QDir::System, QDir::Name);
        for (const QFileInfo &entry : entries) {
            if (!descriptors.contains(entry.fileName()))
                descriptors.insert(entry.fileName(), entry.absoluteFilePath());
        }
    }

    Firmware withoutKeys;
    for (const QString &path : std::as_const(descriptors)) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        // An empty file masks a descriptor without replacing it.
        const QJsonObject descriptor = QJsonDocument::fromJson(file.readAll()).object();
        if (descriptor.isEmpty() || !descriptor.value(u"interface-types"_s).toArray().contains(u"uefi"_s)
            || !targetsKemuMachines(descriptor.value(u"targets"_s).toArray()))
            continue;

        const QJsonObject mapping = descriptor.value(u"mapping"_s).toObject();
        if (mapping.value(u"device"_s).toString() != u"flash"_s || mapping.value(u"mode"_s).toString(u"split"_s) != u"split"_s)
            continue;

        const QJsonArray features = descriptor.value(u"features"_s).toArray();
        // Confidential guests need memory encryption KEMU does not set up.
        if (features.contains(u"amd-sev"_s) || features.contains(u"amd-sev-es"_s) || features.contains(u"amd-sev-snp"_s) || features.contains(u"intel-tdx"_s))
            continue;
        // KEMU turns SMM on only for Secure Boot.
        const bool isSecureBoot = features.contains(u"secure-boot"_s);
        if (isSecureBoot != secureBoot || (!secureBoot && features.contains(u"requires-smm"_s)))
            continue;

        Firmware firmware;
        firmware.codePath = flashFile(mapping.value(u"executable"_s).toObject(), root);
        firmware.varsPath = flashFile(mapping.value(u"nvram-template"_s).toObject(), root);
        firmware.source = path;
        firmware.secureBoot = isSecureBoot;
        firmware.keysEnrolled = features.contains(u"enrolled-keys"_s) || hasEnrolledKeys(firmware.varsPath);
        if (!firmware.isValid())
            continue;
        // Keys would turn Secure Boot on after all.
        if (!secureBoot && firmware.keysEnrolled)
            continue;
        // Windows checks Secure Boot against the Microsoft keys.
        if (!secureBoot || firmware.keysEnrolled)
            return firmware;
        if (!withoutKeys.isValid())
            withoutKeys = firmware;
    }
    return withoutKeys;
}

Firmware fromFileNames(const QStringList &dirs, bool secureBoot)
{
    // Images holding code and variables in one file do not fit the two flash
    // devices KEMU uses, so OVMF.fd is no candidate.
    static const QStringList sbCode = {u"OVMF_CODE.secboot.4m.fd"_s,
                                       u"OVMF_CODE_4M.secboot.fd"_s,
                                       u"OVMF_CODE.secboot.fd"_s,
                                       u"edk2-x86_64-secure-code.fd"_s,
                                       u"ovmf-x86_64-smm-ms-code.bin"_s,
                                       u"ovmf-x86_64-smm-code.bin"_s};

    static const QStringList sbVars = {u"OVMF_VARS.4m.ms.fd"_s,
                                       u"OVMF_VARS_4M.ms.fd"_s,
                                       u"OVMF_VARS.ms.fd"_s,
                                       u"OVMF_VARS.secboot.fd"_s,
                                       u"ovmf-x86_64-smm-ms-vars.bin"_s,
                                       u"ovmf-x86_64-smm-vars.bin"_s,
                                       u"OVMF_VARS_4M.fd"_s,
                                       u"OVMF_VARS.4m.fd"_s,
                                       u"OVMF_VARS.fd"_s,
                                       u"edk2-i386-vars.fd"_s};

    static const QStringList code = {u"OVMF_CODE.4m.fd"_s, u"OVMF_CODE_4M.fd"_s, u"OVMF_CODE.fd"_s, u"edk2-x86_64-code.fd"_s, u"ovmf-x86_64-code.bin"_s};

    static const QStringList vars = {u"OVMF_VARS.4m.fd"_s, u"OVMF_VARS_4M.fd"_s, u"OVMF_VARS.fd"_s, u"edk2-i386-vars.fd"_s, u"ovmf-x86_64-vars.bin"_s};

    for (const QString &d : dirs) {
        const QDir dir(d);
        const QString codePath = firstExisting(dir, secureBoot ? sbCode : code);
        if (codePath.isEmpty())
            continue;

        // A 4MB code image needs 4MB vars - mixing the two bricks the boot,
        // so prefer candidates of the same size family.
        const bool is4m = codePath.contains("4m"_L1, Qt::CaseInsensitive);
        QStringList varsNames = secureBoot ? sbVars : vars;
        std::stable_partition(varsNames.begin(), varsNames.end(), [is4m](const QString &n) {
            return n.contains("4m"_L1, Qt::CaseInsensitive) == is4m;
        });
        const QString varsPath = firstExisting(dir, varsNames);
        // Code without a template is of no use; another directory may have
        // both. Nor is a template with keys without Secure Boot.
        if (varsPath.isEmpty() || (!secureBoot && hasEnrolledKeys(varsPath)))
            continue;

        const QString varsName = QFileInfo(varsPath).fileName();
        Firmware firmware;
        firmware.codePath = codePath;
        firmware.varsPath = varsPath;
        firmware.source = dir.absolutePath();
        firmware.secureBoot = secureBoot;
        firmware.keysEnrolled =
            secureBoot && (varsName.contains(".ms."_L1) || varsName.contains("-ms-"_L1) || varsName.contains("secboot"_L1) || hasEnrolledKeys(varsPath));
        return firmware;
    }
    return {};
}

bool hasEnrolledKeys(const QString &varsPath)
{
    QFile file(varsPath);
    // Variable stores are 128 KiB to a few MiB; anything far larger is no store.
    if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024 * 1024)
        return false;
    const QByteArray store = file.readAll();

    // An authenticated variable (edk2's AUTHENTICATED_VARIABLE_HEADER): start
    // id 0x55aa, state, reserved, attributes, monotonic count, timestamp,
    // public key index, name size, data size, vendor GUID, then the name.
    // PK belongs to EFI_GLOBAL_VARIABLE, 8be4df61-93ca-11d2-aa0d-00e098032b8c.
    static const QByteArray pk = QByteArray::fromHex("61dfe48bca93d211aa0d00e098032b8c") + QByteArray("P\0K\0\0\0", 6);
    constexpr qsizetype guidOffset = 44;
    constexpr quint8 varAdded = 0x3f;
    constexpr quint8 inDeletedTransition = 0xfe; // cleared bit 0: being replaced, still valid

    for (qsizetype at = store.indexOf(pk); at >= 0; at = store.indexOf(pk, at + 1)) {
        const qsizetype header = at - guidOffset;
        if (header < 0)
            continue;
        const auto *bytes = reinterpret_cast<const uchar *>(store.constData() + header);
        quint32 nameSize = 0;
        quint32 dataSize = 0;
        std::memcpy(&nameSize, bytes + 36, sizeof nameSize);
        std::memcpy(&dataSize, bytes + 40, sizeof dataSize);
        const bool valid = bytes[0] == 0xaa && bytes[1] == 0x55 && (bytes[2] == varAdded || bytes[2] == (varAdded & inDeletedTransition));
        if (valid && qFromLittleEndian(nameSize) == 6 && qFromLittleEndian(dataSize) > 0)
            return true;
    }
    return false;
}

} // namespace Ovmf
