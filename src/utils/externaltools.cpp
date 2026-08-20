/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "externaltools.h"

#include "const.h"

#include "utils/sandboxcapabilities.h"

// --- Singleton ---

ExternalTools &ExternalTools::instance()
{
    static ExternalTools s_instance;
    return s_instance;
}

ExternalTools::ExternalTools(QObject *parent)
    : QObject(parent)
{
    // Register every external tool the project depends on.
    // The configKey must match the key used in SettingsDialog::saveSettings().
    registerTool(QemuSystem, u"QemuSystemPath"_s, u"qemu-system-x86_64"_s);
    registerTool(QemuImg, u"QemuImgPath"_s, u"qemu-img"_s);
    registerTool(Swtpm, u"SwtpmPath"_s, u"swtpm"_s);
    registerTool(Virtiofsd, u"VirtiofsdPath"_s, u"virtiofsd"_s);
    registerTool(GenIsoImage, u"GenIsoImagePath"_s, u"genisoimage"_s);
    registerTool(Passt, u"PasstPath"_s, u"passt"_s);
    registerTool(Pasta, u"PastaPath"_s, u"pasta"_s);
    registerTool(Dnsmasq, u"DnsmasqPath"_s, u"dnsmasq"_s);
    registerTool(DbusDaemon, u"DbusDaemonPath"_s, u"dbus-daemon"_s);
}

// --- Registration ---

void ExternalTools::registerTool(Tool id, const QString &configKey, const QString &defaultBinary)
{
    ToolInfo info;
    info.configKey = configKey;
    info.defaultBinary = defaultBinary;
    m_tools.insert(id, info);
}

// --- Refresh (path resolution + version probing) ---

void ExternalTools::refresh()
{
    for (auto it = m_tools.begin(); it != m_tools.end(); ++it) {
        detectPath(it.value());
        probeVersion(it.key(), it.value());
    }
    Q_EMIT toolsRefreshed();
}

void ExternalTools::detectPath(ToolInfo &info)
{
    info.detectedPath = autoDetect(info);

    info.configuredPath.clear();
    if (SandboxCapabilities::instance().has(SandboxCapabilities::ExternalToolPaths)) {
        KConfig config(u"kemurc"_s);
        const KConfigGroup general = config.group(u"General"_s);
        info.configuredPath = general.readEntry(info.configKey, QString());
    }

    info.resolvedPath = (!info.configuredPath.isEmpty() && QFileInfo::exists(info.configuredPath)) ? info.configuredPath : info.detectedPath;
}

QString ExternalTools::autoDetect(const ToolInfo &info)
{
    QString found = QStandardPaths::findExecutable(info.defaultBinary);
    if (!found.isEmpty())
        return found;

    // Alternative binary names for the ISO tool.
    if (info.configKey == u"GenIsoImagePath"_s) {
        found = QStandardPaths::findExecutable(u"mkisofs"_s);
        if (found.isEmpty())
            found = QStandardPaths::findExecutable(u"xorriso"_s);
        if (!found.isEmpty())
            return found;
    }

    return QStandardPaths::findExecutable(info.defaultBinary, KEMUConst::FallbackPaths);
}

void ExternalTools::probeVersion(Tool tool, ToolInfo &info)
{
    info.detectedVersion.clear();
    info.available = false;

    if (info.resolvedPath.isEmpty() || !QFileInfo::exists(info.resolvedPath))
        return;

    info.detectedVersion = probeVersionString(tool, info.resolvedPath);

    const QFileInfo fi(info.resolvedPath);
    info.available = fi.exists() && fi.isExecutable();
}

// --- Queries ---

QString ExternalTools::path(Tool tool) const
{
    return m_tools.value(tool).resolvedPath;
}

QString ExternalTools::version(Tool tool) const
{
    return m_tools.value(tool).detectedVersion;
}

bool ExternalTools::isAvailable(Tool tool) const
{
    return m_tools.value(tool).available;
}

bool ExternalTools::isAvailable(const QList<ExternalTools::Tool> &tools) const
{
    for (const auto tool : tools)
        if (!m_tools.value(tool).available)
            return false;
    return true;
}

QString ExternalTools::configKey(Tool tool) const
{
    return m_tools.value(tool).configKey;
}

QString ExternalTools::defaultBinary(Tool tool) const
{
    return m_tools.value(tool).defaultBinary;
}

QString ExternalTools::detectedPath(Tool tool) const
{
    return m_tools.value(tool).detectedPath;
}

QString ExternalTools::configuredPath(Tool tool) const
{
    return m_tools.value(tool).configuredPath;
}

QString ExternalTools::displayName(Tool tool)
{
    switch (tool) {
    case QemuSystem:
        return i18n("QEMU System Emulator");
    case QemuImg:
        return i18n("QEMU Image Tool");
    case Swtpm:
        return i18n("Software TPM Emulator");
    case Virtiofsd:
        return i18n("Virtio-fs Shared File System Daemon");
    case GenIsoImage:
        return i18n("ISO Image Creator (genisoimage/mkisofs/xorriso)");
    case Passt:
        return i18n("passt Network Backend");
    case Pasta:
        return i18n("pasta Namespace Network Connector");
    case Dnsmasq:
        return i18n("dnsmasq DHCP/DNS Server");
    case DbusDaemon:
        return i18n("D-Bus Daemon");
    }
    return {};
}

QList<ExternalTools::Tool> ExternalTools::allTools()
{
    return {QemuSystem, QemuImg, Swtpm, Virtiofsd, GenIsoImage, Passt, Pasta, Dnsmasq, DbusDaemon};
}

QList<ExternalTools::Tool> ExternalTools::coreTools()
{
    return {QemuSystem, QemuImg};
}

QList<ExternalTools::Tool> ExternalTools::virtualNetworkTools()
{
    return {Pasta, Dnsmasq};
}

QList<ExternalTools::Tool> ExternalTools::dbusDisplayTools()
{
    return {DbusDaemon};
}

QList<ExternalTools::Tool> ExternalTools::unusedTools() const
{
    const auto &caps = SandboxCapabilities::instance();
    QList<Tool> result;

    if (!caps.has(SandboxCapabilities::UserNamespaces))
        result << Passt << Pasta << Dnsmasq;

    return result;
}

QString ExternalTools::probeVersionString(Tool /*tool*/, const QString &path)
{
    if (path.isEmpty() || !QFileInfo::exists(path))
        return {};

    const QStringList versionArgs = {u"--version"_s};

    KProcess process;
    process.setOutputChannelMode(KProcess::MergedChannels);
    process.setProgram(path);
    process.setArguments(versionArgs);
    process.start();

    if (!process.waitForFinished(2000) || process.exitCode() != 0)
        return {};

    QString output = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
    if (output.isEmpty())
        output = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();

    return output.split(u'\n').value(0).trimmed();
}

QStringList ExternalTools::missingToolNames() const
{
    const QList<Tool> irrelevant = unusedTools();
    QStringList missing;
    for (const Tool t : allTools()) {
        if (!irrelevant.contains(t) && !isAvailable(t))
            missing << displayName(t);
    }
    return missing;
}

bool ExternalTools::isVirtualNetworkCapable() const
{
    if (!SandboxCapabilities::instance().has(SandboxCapabilities::UserNamespaces))
        return false;
    return isAvailable(virtualNetworkTools());
}

bool ExternalTools::isPasstUsable() const
{
    // passt does its own unshare()+pivot_root(); without user namespaces it
    // cannot start, and there is no switch to turn that off.
    return SandboxCapabilities::instance().has(SandboxCapabilities::UserNamespaces) && isAvailable(Passt);
}

// --- Feature groups ---

QList<ExternalTools::FeatureGroup> ExternalTools::featureGroups()
{
    return {{Core, i18n("Core Functionality"), i18n("Required for basic VM operation. This warning cannot be disabled."), coreTools(), false},
            {VirtualNetwork, i18n("Virtual Networks"), i18n("User-defined isolated networks with built-in DHCP and DNS."), virtualNetworkTools(), true},
            {DbusDisplay, i18n("D-Bus Integrated Display"), i18n("Embedded display via a per-VM D-Bus session."), dbusDisplayTools(), true},
            {TpmEmulation,
             i18n("TPM Emulation"),
             i18n("Software TPM 2.0 device for guests that require it (e.g. Windows "
                  "11)."),
             {Swtpm},
             true},
            {SharedFolders, i18n("Shared Folders"), i18n("Host-to-guest folder sharing via virtio-fs."), {Virtiofsd}, true},
            {CloudInitIso, i18n("Cloud-Init / ISO Creation"), i18n("Generate seed ISOs for automated cloud-image deployment."), {GenIsoImage}, true}};
}

QList<ExternalTools::FeatureGroup> ExternalTools::missingFeatureGroups() const
{
    const QList<Tool> unused = unusedTools();
    QList<FeatureGroup> result;
    for (const auto &group : featureGroups()) {
        for (const Tool t : group.tools) {
            if (unused.contains(t))
                continue;
            if (!isAvailable(t)) {
                result.append(group);
                break;
            }
        }
    }
    return result;
}

QString ExternalTools::suppressionConfigKey(FeatureGroupId id)
{
    switch (id) {
    case Core:
        return u"IgnoreMissing_Core"_s;
    case VirtualNetwork:
        return u"IgnoreMissing_VirtualNetwork"_s;
    case DbusDisplay:
        return u"IgnoreMissing_DbusDisplay"_s;
    case TpmEmulation:
        return u"IgnoreMissing_TpmEmulation"_s;
    case SharedFolders:
        return u"IgnoreMissing_SharedFolders"_s;
    case CloudInitIso:
        return u"IgnoreMissing_CloudInitIso"_s;
    }
    return {};
}

bool ExternalTools::isWarningSuppressed(FeatureGroupId id)
{
    KConfig config(u"kemurc"_s);
    const KConfigGroup g = config.group(u"MissingToolWarnings"_s);
    return g.readEntry(suppressionConfigKey(id), false);
}

void ExternalTools::setWarningSuppressed(FeatureGroupId id, bool suppressed)
{
    KConfig config(u"kemurc"_s);
    KConfigGroup g = config.group(u"MissingToolWarnings"_s);
    g.writeEntry(suppressionConfigKey(id), suppressed);
    config.sync();
}

QList<ExternalTools::FeatureGroupId> ExternalTools::suppressedFeatureGroups()
{
    QList<FeatureGroupId> result;
    for (const auto &g : featureGroups()) {
        if (g.suppressible && isWarningSuppressed(g.id))
            result.append(g.id);
    }
    return result;
}

void ExternalTools::resetAllSuppressions()
{
    KConfig config(u"kemurc"_s);
    config.deleteGroup(u"MissingToolWarnings"_s);
    config.sync();
}
