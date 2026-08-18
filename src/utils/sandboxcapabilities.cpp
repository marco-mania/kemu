/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sandboxcapabilities.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>

#include <KLocalizedString>

extern "C" {
#include <sched.h>
#include <sys/wait.h>
#include <unistd.h>
}

namespace
{

struct ProbeResult {
    bool ok = false;
    QString reason;
};

ProbeResult probeUserNamespaces()
{
    QFile sysctl(u"/proc/sys/kernel/unprivileged_userns_clone"_s);
    if (sysctl.open(QIODevice::ReadOnly)) {
        const QByteArray value = sysctl.readAll().trimmed();
        sysctl.close();
        if (value == "0")
            return {false, i18n("Unprivileged user namespaces are disabled by the kernel.")};
    }

    const pid_t child = fork();
    if (child < 0)
        return {false, i18n("Could not fork a probe process.")};

    if (child == 0)
        _exit(unshare(CLONE_NEWUSER | CLONE_NEWNET) == 0 ? 0 : 1);

    int status = 0;
    waitpid(child, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return {true, {}};

    return {false, i18n("Creating user and network namespaces was denied.")};
}

ProbeResult probeUsbPassthrough()
{
    // QEMU opens the device nodes directly; without them usb-host cannot work,
    // no matter what the enumeration reports.
    if (!QFileInfo::exists(u"/dev/bus/usb"_s))
        return {false, i18n("USB device nodes are not accessible.")};
    if (!QFileInfo(u"/sys/bus/usb/devices"_s).isReadable())
        return {false, i18n("USB devices are not exposed through sysfs.")};
    return {true, {}};
}

ProbeResult probeHostTerminal(QString *program)
{
    static const QStringList candidates = {u"konsole"_s, u"x-terminal-emulator"_s, u"xterm"_s};
    for (const QString &candidate : candidates) {
        const QString found = QStandardPaths::findExecutable(candidate);
        if (!found.isEmpty()) {
            *program = found;
            return {true, {}};
        }
    }
    return {false, i18n("No terminal emulator was found.")};
}

QString runtimeDir()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    return dir.isEmpty() ? QStringLiteral("/run/user/%1").arg(getuid()) : dir;
}

ProbeResult probePipeWireAudio()
{
    if (QFileInfo::exists(runtimeDir() + u"/pipewire-0"_s))
        return {true, {}};
    return {false, i18n("No PipeWire socket is reachable.")};
}

ProbeResult probePulseAudio()
{
    if (QFileInfo::exists(runtimeDir() + u"/pulse/native"_s))
        return {true, {}};
    if (QProcessEnvironment::systemEnvironment().contains(u"PULSE_SERVER"_s))
        return {true, {}};
    return {false, i18n("No PulseAudio socket is reachable.")};
}

ProbeResult probeExternalToolPaths()
{
#ifdef KEMU_SELF_CONTAINED
    return {false, i18n("This build ships its own copies of the external tools.")};
#else
    return {true, {}};
#endif
}

} // namespace

SandboxCapabilities &SandboxCapabilities::instance()
{
    static SandboxCapabilities s_instance;
    return s_instance;
}

void SandboxCapabilities::refresh()
{
    m_terminalProgram.clear();

    const auto store = [this](Capability capability, const ProbeResult &result) {
        m_states.insert(capability, {result.ok, result.reason});
    };

    store(UserNamespaces, probeUserNamespaces());
    store(UsbPassthrough, probeUsbPassthrough());
    store(HostTerminal, probeHostTerminal(&m_terminalProgram));
    store(PipeWireAudio, probePipeWireAudio());
    store(PulseAudio, probePulseAudio());
    store(ExternalToolPaths, probeExternalToolPaths());
}

bool SandboxCapabilities::has(Capability capability) const
{
    return m_states.value(capability).available;
}

QString SandboxCapabilities::unavailableReason(Capability capability) const
{
    const State state = m_states.value(capability);
    return state.available ? QString() : state.reason;
}

void SandboxCapabilities::disable(Capability capability, const QString &reason)
{
    m_states.insert(capability, {false, reason});
    if (capability == HostTerminal)
        m_terminalProgram.clear();
}

QString SandboxCapabilities::terminalProgram() const
{
    return has(HostTerminal) ? m_terminalProgram : QString();
}

QString SandboxCapabilities::configName(Capability capability)
{
    switch (capability) {
    case UserNamespaces:
        return u"user-namespaces"_s;
    case UsbPassthrough:
        return u"usb-passthrough"_s;
    case HostTerminal:
        return u"host-terminal"_s;
    case PipeWireAudio:
        return u"pipewire-audio"_s;
    case PulseAudio:
        return u"pulseaudio"_s;
    case ExternalToolPaths:
        return u"external-tool-paths"_s;
    }
    return {};
}

bool SandboxCapabilities::parseCapability(const QString &name, Capability *capability)
{
    for (const Capability candidate : allCapabilities()) {
        if (configName(candidate) == name.toLower()) {
            *capability = candidate;
            return true;
        }
    }
    return false;
}

QList<SandboxCapabilities::Capability> SandboxCapabilities::allCapabilities()
{
    return {UserNamespaces, UsbPassthrough, HostTerminal, PipeWireAudio, PulseAudio, ExternalToolPaths};
}
