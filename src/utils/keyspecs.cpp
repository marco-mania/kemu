/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "keyspecs.h"

#include <algorithm>
#include <cmath>

#include <QLocale>
#include <QStringList>
#include <QThread>

#include <KLocalizedString>

#include <unistd.h>

using namespace Qt::StringLiterals;

namespace KeySpecs
{
QString coresText(int cores)
{
    return i18ncp("@info number of virtual CPU cores", "%1 core", "%1 cores", cores);
}

QString cpuModelText(VMSpecs::CpuModel model)
{
    switch (model) {
    case VMSpecs::CpuModel::Host:
        return i18nc("@label short CPU model", "Host CPU");
    case VMSpecs::CpuModel::Qemu64:
        return i18nc("@label short CPU model", "QEMU 64-bit");
    case VMSpecs::CpuModel::Qemu32:
        return i18nc("@label short CPU model", "QEMU 32-bit");
    case VMSpecs::CpuModel::I486:
        return i18nc("@label short CPU model", "Intel 80486");
    case VMSpecs::CpuModel::Pentium:
        return i18nc("@label short CPU model", "Intel Pentium");
    case VMSpecs::CpuModel::Pentium2:
        return i18nc("@label short CPU model", "Intel Pentium II");
    case VMSpecs::CpuModel::Pentium3:
        return i18nc("@label short CPU model", "Intel Pentium III");
    case VMSpecs::CpuModel::Pentium4:
        return i18nc("@label short CPU model", "Intel Pentium 4");
    case VMSpecs::CpuModel::CoreDuo:
        return i18nc("@label short CPU model", "Intel Core Duo");
    case VMSpecs::CpuModel::Core2Duo:
        return i18nc("@label short CPU model", "Intel Core 2 Duo");
    case VMSpecs::CpuModel::Nehalem:
        return i18nc("@label short CPU model", "Intel Nehalem");
    case VMSpecs::CpuModel::SandyBridge:
        return i18nc("@label short CPU model", "Intel Sandy Bridge");
    case VMSpecs::CpuModel::Haswell:
        return i18nc("@label short CPU model", "Intel Haswell");
    case VMSpecs::CpuModel::Skylake:
        return i18nc("@label short CPU model", "Intel Skylake");
    case VMSpecs::CpuModel::Phenom:
        return i18nc("@label short CPU model", "AMD Phenom");
    case VMSpecs::CpuModel::Epyc:
        return i18nc("@label short CPU model", "AMD Zen 1");
    }
    return VMConfig::toUIString(model);
}

QString memoryText(double mib, int digits)
{
    const QLocale locale;
    if (mib < 1024)
        return locale.toString(qRound(mib)) + i18nc("Memory size in mebibytes, keep the leading space", " MiB");
    // Enough digits for the whole number, so 128 does not turn into 1.3e+02.
    const double gib = mib / 1024.0;
    return locale.toString(gib, 'g', std::max(digits, int(std::log10(gib)) + 1)) + i18nc("Memory size in gibibytes, keep the leading space", " GiB");
}

QString diskSizeText(int gb)
{
    return i18n("%1 GB", gb);
}

QString diskFacts(VMSpecs::DiskInterface diskInterface, bool encrypted, bool isNew)
{
    QStringList facts{VMConfig::toUIString(diskInterface)};
    if (encrypted)
        facts << i18nc("@info state of the system disk", "encrypted");
    if (isNew)
        facts << i18nc("@info state of the system disk", "new");
    return facts.join(u", "_s);
}

int gibRoundedUp(qint64 bytes)
{
    return int((bytes + (1LL << 30) - 1) >> 30);
}

int hostCpuCount()
{
    return std::max(0, QThread::idealThreadCount());
}

qint64 hostMemoryMib()
{
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long pageSize = sysconf(_SC_PAGESIZE);
    return pages > 0 && pageSize > 0 ? (qint64(pages) * pageSize) >> 20 : 0;
}

QString hostCpusText()
{
    const int count = hostCpuCount();
    return count > 0 ? i18nc("@info number of host CPUs", "Host: %1", count) : QString();
}

QString hostMemoryText()
{
    const qint64 mib = hostMemoryMib();
    return mib > 0 ? i18nc("@info host memory", "Host: %1", memoryText(double(mib), 2)) : QString();
}
}
