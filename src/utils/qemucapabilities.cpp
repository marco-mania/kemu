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

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>
#include <QVersionNumber>

#include <algorithm>

QemuCapabilities &QemuCapabilities::instance()
{
    static QemuCapabilities s_instance;
    return s_instance;
}

void QemuCapabilities::refresh()
{
    m_displays.clear();
    m_audiodevs.clear();
    m_devices.clear();
    m_chardevs.clear();
    m_seccompSandbox = false;
    m_monitorObject = false;
    m_virtioGpuBlob = false;
    m_virglRenderServer.clear();
    m_kvmHonorGuestPat = false;
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

    if (m_virtioGpuBlob && !hostKernelSupportsGpuBlobs())
        qCInfo(KEMU) << "QEMU supports virtio-gpu blob resources, but kernel" << QSysInfo::kernelVersion()
                     << "is older than 6.13; 3D acceleration runs without them";

    m_virglRenderServer = findVirglRenderServer(qemu, {u"/usr"_s, u"/usr/local"_s});
    if (m_virtioGpuBlob) {
        if (m_virglRenderServer.isEmpty())
            qCInfo(KEMU) << "No virgl_render_server found; VMs get no Vulkan (Venus)";
        else
            qCDebug(KEMU) << "Venus render server:" << m_virglRenderServer;
    }
}

QString QemuCapabilities::findVirglRenderServer(const QString &qemuPath, const QStringList &systemPrefixes)
{
    const auto isProgram = [](const QString &path) {
        const QFileInfo info(path);
        return info.isFile() && info.isExecutable();
    };

    // virglrenderer does not fall back to its built-in path when the variable
    // is set, so neither does this.
    const QString override = qEnvironmentVariable(RenderServerEnvironment);
    if (!override.isEmpty())
        return isProgram(override) ? override : QString();

    // The path as configured and its target: a QEMU reached through a symlink
    // (NixOS: /run/current-system/sw/bin) has a prefix of its own on each end.
    QStringList prefixes;
    const QFileInfo qemu(qemuPath);
    for (const QString &binary : {qemu.absoluteFilePath(), qemu.canonicalFilePath()}) {
        if (binary.isEmpty())
            continue;
        const QFileInfo binDir(QFileInfo(binary).absolutePath());
        if (binDir.fileName() == u"bin"_s)
            prefixes << binDir.absolutePath();
    }
    prefixes << systemPrefixes;
    prefixes.removeDuplicates();

    // meson installs to libexec/ by default; Arch sets libexecdir to lib/.
    for (const QString &prefix : std::as_const(prefixes)) {
        for (const QString &relative : {u"libexec/virgl_render_server"_s, u"lib/virgl_render_server"_s}) {
            const QString candidate = QDir(prefix).filePath(relative);
            if (isProgram(candidate))
                return candidate;
        }
    }
    return {};
}

bool QemuCapabilities::hostKernelSupportsGpuBlobs()
{
    return hostKernelAtLeast(kGpuBlobKernelMajor, kGpuBlobKernelMinor);
}

bool QemuCapabilities::hostKernelHonorsGuestPat()
{
    return hostKernelAtLeast(kGuestPatKernelMajor, kGuestPatKernelMinor);
}

bool QemuCapabilities::hostKernelAtLeast(int major, int minor)
{
    if (QSysInfo::kernelType() != u"linux"_s)
        return false;
    // "6.18.44-fc-v595": parsing stops at the first non-numeric suffix.
    return QVersionNumber::fromString(QSysInfo::kernelVersion()) >= QVersionNumber(major, minor);
}

QSet<QString> QemuCapabilities::parsePropertyHelp(const QString &output)
{
    // Lines read: "  blob=<bool>            - on/off (default: false)", some
    // without a description. The header ("virtio-vga-gl options:") and error
    // messages have no "=<".
    QSet<QString> result;
    const QStringList lines = output.split(u'\n', Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QStringView trimmed = QStringView(line).trimmed();
        const qsizetype separator = trimmed.indexOf(u"=<"_s);
        if (separator <= 0)
            continue;
        const QStringView name = trimmed.first(separator);
        const bool plainName = std::all_of(name.begin(), name.end(), [](QChar c) {
            return c.isLetterOrNumber() || c == u'-' || c == u'_' || c == u'.';
        });
        if (plainName)
            result.insert(name.toString());
    }
    return result;
}

QSet<QString> QemuCapabilities::parseQomPropertyList(const QString &output)
{
    // One reply per line: the greeting, {"return": {}} for the commands
    // without a result, and {"return": [{"name": "...", "type": "..."}, ...]}.
    QSet<QString> result;
    const QStringList lines = output.split(u'\n', Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QJsonValue reply = QJsonDocument::fromJson(line.toUtf8()).object().value(u"return"_s);
        const QJsonArray properties = reply.toArray();
        for (const QJsonValue &property : properties) {
            const QString name = property.toObject().value(u"name"_s).toString();
            if (!name.isEmpty())
                result.insert(name);
        }
    }
    return result;
}

bool QemuCapabilities::allowsVirglBlob(const QSet<QString> &properties)
{
    return properties.contains(u"blob"_s) && properties.contains(u"hostmem"_s) && properties.contains(u"venus"_s);
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

bool QemuCapabilities::hasDevice(const QString &name) const
{
    // Device names are case-sensitive ("VGA").
    return m_devices.isEmpty() || m_devices.contains(name);
}

bool QemuCapabilities::hasChardev(const QString &name) const
{
    return m_chardevs.isEmpty() || m_chardevs.contains(name.toLower());
}

bool QemuCapabilities::runProbes(const QString &qemuPath, int timeoutMs)
{
    // Start all probes before awaiting any, so a refresh costs one timeout
    // in total rather than one per help option.
    KProcess displayProbe;
    KProcess audiodevProbe;
    KProcess sandboxProbe;
    KProcess monitorProbe;
    KProcess deviceProbe;
    KProcess chardevProbe;
    KProcess gpuGlProbe;
    KProcess kvmAccelProbe;
    startProbe(displayProbe, qemuPath, {u"-display"_s, u"help"_s});
    startProbe(audiodevProbe, qemuPath, {u"-audiodev"_s, u"help"_s});
    startProbe(deviceProbe, qemuPath, {u"-device"_s, u"help"_s});
    startProbe(chardevProbe, qemuPath, {u"-chardev"_s, u"help"_s});
    // Modular builds before QEMU 9.1 (Debian, Ubuntu) cannot load the GL
    // device module without a GL display ("undefined symbol:
    // qemu_egl_display"). The listing is empty then, which is right: those
    // versions cannot use blob resources with virgl anyway.
    startProbe(gpuGlProbe, qemuPath, {u"-device"_s, u"virtio-vga-gl,help"_s});
    // Accelerators have no "help" listing ("Property 'kvm-accel.help' not
    // found"), so ask over QMP. The machine "none" has no devices or CPUs;
    // the class needs no instance to list its properties.
    startProbe(kvmAccelProbe, qemuPath, {u"-machine"_s, u"none"_s, u"-nodefaults"_s, u"-display"_s, u"none"_s, u"-qmp"_s, u"stdio"_s});
    kvmAccelProbe.write(R"({"execute": "qmp_capabilities"}
{"execute": "qom-list-properties", "arguments": {"typename": "kvm-accel"}}
{"execute": "quit"}
)");
    kvmAccelProbe.closeWriteChannel();
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
    m_devices = collectDeviceProbe(deviceProbe, timeoutMs - static_cast<int>(elapsed.elapsed()));
    m_chardevs = collectProbe(chardevProbe, timeoutMs - static_cast<int>(elapsed.elapsed()));
    m_virtioGpuBlob = allowsVirglBlob(parsePropertyHelp(collectOutput(gpuGlProbe, timeoutMs - static_cast<int>(elapsed.elapsed()))));
    m_kvmHonorGuestPat = parseQomPropertyList(collectOutput(kvmAccelProbe, timeoutMs - static_cast<int>(elapsed.elapsed()))).contains(u"honor-guest-pat"_s);

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

QString QemuCapabilities::collectOutput(KProcess &process, int remainingMs)
{
    if (process.state() != KProcess::NotRunning && !process.waitForFinished(qMax(0, remainingMs))) {
        // Never leave a running process behind: ~KProcess would block and warn.
        process.kill();
        process.waitForFinished(200);
        return {};
    }

    // The exit code is deliberately ignored; what matters is the listing.
    QString output = QString::fromLocal8Bit(process.readAllStandardOutput());
    if (output.trimmed().isEmpty())
        output = QString::fromLocal8Bit(process.readAllStandardError());
    return output;
}

QSet<QString> QemuCapabilities::collectDeviceProbe(KProcess &process, int remainingMs)
{
    // Lines read: name "qxl-vga", bus PCI, desc "…"
    static constexpr QStringView prefix = u"name \"";
    QSet<QString> result;
    const QStringList lines = collectOutput(process, remainingMs).split(u'\n', Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QStringView trimmed = QStringView(line).trimmed();
        if (!trimmed.startsWith(prefix))
            continue;
        const qsizetype end = trimmed.indexOf(u'"', prefix.size());
        if (end > prefix.size())
            result.insert(trimmed.sliced(prefix.size(), end - prefix.size()).toString());
    }
    return result;
}

QSet<QString> QemuCapabilities::collectProbe(KProcess &process, int remainingMs)
{
    QSet<QString> result;
    const QStringList lines = collectOutput(process, remainingMs).split(u'\n', Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.endsWith(u':')) // "Available …:" header
            continue;
        result.insert(trimmed.section(u' ', 0, 0).toLower());
    }

    return result;
}
