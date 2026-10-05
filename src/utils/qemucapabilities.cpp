/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "qemucapabilities.h"

#include "const.h"
#include "utils/externaltools.h"
#include "utils/sandboxcapabilities.h"

#include "kemu_debug.h"

QemuCapabilities &QemuCapabilities::instance()
{
    static QemuCapabilities s_instance;
    return s_instance;
}

void QemuCapabilities::refresh()
{
    m_displays.clear();
    m_audiodevs.clear();
    m_seccompSandbox = false;
    m_monitorObject = false;
    m_probeSucceeded = false;

    const QString qemu = ExternalTools::instance().path(ExternalTools::QemuSystem);
    if (qemu.isEmpty() || !QFileInfo::exists(qemu)) {
        seedSelfContainedDefaults();
        return;
    }

    // One retry with a longer budget. The first run after an install pays for
    // paging in the QEMU binary, which can outlast the normal budget on slow
    // storage; the second attempt hits a warm page cache.
    m_probeSucceeded = runProbes(qemu, kProbeTimeoutMs);
    if (!m_probeSucceeded) {
        qCWarning(KEMU) << "QEMU capability probe came back empty, retrying with a longer budget";
        m_probeSucceeded = runProbes(qemu, kProbeRetryTimeoutMs);
    }

    if (!m_probeSucceeded) {
        qCWarning(KEMU) << "Could not determine the capabilities of" << qemu;
        seedSelfContainedDefaults();
    }
}

bool QemuCapabilities::hasDisplay(const QString &name) const
{
    // An empty set means the probe failed against a QEMU we do not control;
    // never lock the user out over that. Self-contained builds never reach
    // this branch because refresh() seeds a known-good minimum instead.
    return m_displays.isEmpty() || m_displays.contains(name.toLower());
}

bool QemuCapabilities::hasAudiodev(const QString &name) const
{
    return m_audiodevs.isEmpty() || m_audiodevs.contains(name.toLower());
}

bool QemuCapabilities::runProbes(const QString &qemuPath, int timeoutMs)
{
    // Start all probes before awaiting any, so a refresh costs one timeout
    // in total rather than one per help option.
    KProcess displayProbe;
    KProcess audiodevProbe;
    KProcess sandboxProbe;
    KProcess monitorProbe;
    startProbe(displayProbe, qemuPath, {u"-display"_s, u"help"_s});
    startProbe(audiodevProbe, qemuPath, {u"-audiodev"_s, u"help"_s});
    // Options are parsed in order, so -sandbox is checked before -version
    // exits. A QEMU without seccomp support fails right there.
    startProbe(sandboxProbe, qemuPath, {u"-sandbox"_s, KEMUConst::QemuSandboxOptions, u"-version"_s});
    // Lists the properties and exits 0 if the type exists; older versions
    // reject the unknown qom-type and exit 1.
    startProbe(monitorProbe, qemuPath, {u"-object"_s, u"monitor-qmp,help"_s});

    QElapsedTimer elapsed;
    elapsed.start();

    m_displays = collectProbe(displayProbe, timeoutMs - static_cast<int>(elapsed.elapsed()));
    m_audiodevs = collectProbe(audiodevProbe, timeoutMs - static_cast<int>(elapsed.elapsed()));
    m_seccompSandbox = collectExitStatus(sandboxProbe, timeoutMs - static_cast<int>(elapsed.elapsed()));
    m_monitorObject = collectExitStatus(monitorProbe, timeoutMs - static_cast<int>(elapsed.elapsed()));

    return !m_displays.isEmpty() && !m_audiodevs.isEmpty();
}

void QemuCapabilities::seedSelfContainedDefaults()
{
    // Against a QEMU we do not control, a failed probe must not lock the user
    // out: has*() keeps its permissive fallback for that case.
    if (SandboxCapabilities::instance().has(SandboxCapabilities::ExternalToolPaths))
        return;

    // A build that ships its own QEMU knows what it shipped. Falling back to
    // "everything is available" would offer backends the image does not
    // contain and turn a probe timeout into a confusing QEMU start failure.
    // The integrated viewer only ever needs these two.
    m_displays = {u"none"_s, u"dbus"_s};
    m_audiodevs = {u"none"_s, u"dbus"_s};
}

void QemuCapabilities::startProbe(KProcess &process, const QString &qemuPath, const QStringList &arguments)
{
    // Separate channels: a warning on stderr must not be parsed as a backend.
    process.setOutputChannelMode(KProcess::SeparateChannels);
    process.setProgram(qemuPath);
    process.setArguments(arguments);
    process.start();
}

bool QemuCapabilities::collectExitStatus(KProcess &process, int remainingMs)
{
    if (process.state() != KProcess::NotRunning && !process.waitForFinished(qMax(0, remainingMs))) {
        process.kill();
        process.waitForFinished(200);
        return false;
    }
    return process.error() != QProcess::FailedToStart && process.exitStatus() == KProcess::NormalExit && process.exitCode() == 0;
}

QSet<QString> QemuCapabilities::collectProbe(KProcess &process, int remainingMs)
{
    QSet<QString> result;

    if (process.state() != KProcess::NotRunning && !process.waitForFinished(qMax(0, remainingMs))) {
        // Never leave a running process behind: ~KProcess would block and warn.
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
