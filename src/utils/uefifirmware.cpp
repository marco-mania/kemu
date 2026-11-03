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

#include <algorithm>

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

bool targetsX86_64(const QJsonArray &targets)
{
    return std::any_of(targets.cbegin(), targets.cend(), [](const QJsonValue &target) {
        return target.toObject().value(u"architecture"_s).toString() == u"x86_64"_s;
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
            || !targetsX86_64(descriptor.value(u"targets"_s).toArray()))
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
        firmware.keysEnrolled = features.contains(u"enrolled-keys"_s);
        if (!firmware.isValid())
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
        // Code without a template is of no use; another directory may have both.
        if (varsPath.isEmpty())
            continue;

        const QString varsName = QFileInfo(varsPath).fileName();
        Firmware firmware;
        firmware.codePath = codePath;
        firmware.varsPath = varsPath;
        firmware.source = dir.absolutePath();
        firmware.secureBoot = secureBoot;
        firmware.keysEnrolled = secureBoot && (varsName.contains(".ms."_L1) || varsName.contains("-ms-"_L1) || varsName.contains("secboot"_L1));
        return firmware;
    }
    return {};
}

} // namespace Ovmf
