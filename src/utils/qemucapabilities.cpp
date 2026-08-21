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
    m_displays.clear();
    m_audiodevs.clear();

    const QString qemu = ExternalTools::instance().path(ExternalTools::QemuSystem);
    if (qemu.isEmpty() || !QFileInfo::exists(qemu))
        return;

    // Start both probes before awaiting either, so a refresh costs one
    // timeout in total rather than one per help option.
    KProcess displayProbe;
    KProcess audiodevProbe;
    startProbe(displayProbe, qemu, u"-display"_s);
    startProbe(audiodevProbe, qemu, u"-audiodev"_s);

    QElapsedTimer elapsed;
    elapsed.start();

    m_displays = collectProbe(displayProbe, kProbeTimeoutMs - static_cast<int>(elapsed.elapsed()));
    m_audiodevs = collectProbe(audiodevProbe, kProbeTimeoutMs - static_cast<int>(elapsed.elapsed()));
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

void QemuCapabilities::startProbe(KProcess &process, const QString &qemuPath, const QString &helpOption)
{
    // Separate channels: a warning on stderr must not be parsed as a backend.
    process.setOutputChannelMode(KProcess::SeparateChannels);
    process.setProgram(qemuPath);
    process.setArguments({helpOption, u"help"_s});
    process.start();
}

QSet<QString> QemuCapabilities::collectProbe(KProcess &process, int remainingMs)
{
    QSet<QString> result;

    if (process.state() != QProcess::NotRunning && !process.waitForFinished(qMax(0, remainingMs))) {
        // Never leave a running process behind: ~QProcess would block and warn.
        process.kill();
        process.waitForFinished(200);
        return result;
    }

    // The exit code is deliberately ignored; what matters is the listing.
    QString output = QString::fromLocal8Bit(process.readAllStandardOutput());
    if (output.trimmed().isEmpty())
        output = QString::fromLocal8Bit(process.readAllStandardError());

    const QStringList lines = output.split(u'\n', Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.endsWith(u':')) // "Available ...:" header
            continue;
        result.insert(trimmed.section(u' ', 0, 0).toLower());
    }

    return result;
}
