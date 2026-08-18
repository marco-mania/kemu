/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "qemucapabilities.h"

#include "utils/externaltools.h"

QemuCapabilities &QemuCapabilities::instance()
{
    static QemuCapabilities s_instance;
    return s_instance;
}

void QemuCapabilities::refresh()
{
    const QString qemu = ExternalTools::instance().path(ExternalTools::QemuSystem);
    m_displays = probe(qemu, u"-display"_s);
    m_audiodevs = probe(qemu, u"-audiodev"_s);
    m_probed = true;
}

QSet<QString> QemuCapabilities::probe(const QString &qemuPath, const QString &helpOption)
{
    QSet<QString> result;
    if (qemuPath.isEmpty() || !QFileInfo::exists(qemuPath))
        return result;

    KProcess process;
    process.setOutputChannelMode(KProcess::MergedChannels);
    process.setProgram(qemuPath);
    process.setArguments({helpOption, u"help"_s});
    process.start();

    if (!process.waitForFinished(2000))
        return result;

    const QString output = QString::fromLocal8Bit(process.readAllStandardOutput());
    const QStringList lines = output.split(u'\n', Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        // Skip the "Available ...:" header and any indented option details.
        if (trimmed.isEmpty() || trimmed.endsWith(u':'))
            continue;
        result.insert(trimmed.section(u' ', 0, 0).toLower());
    }

    return result;
}

bool QemuCapabilities::hasDisplay(const QString &name) const
{
    // An empty set means the probe failed; never lock the user out over that.
    return m_displays.isEmpty() || m_displays.contains(name.toLower());
}

bool QemuCapabilities::hasAudiodev(const QString &name) const
{
    return m_audiodevs.isEmpty() || m_audiodevs.contains(name.toLower());
}
