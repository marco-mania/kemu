/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "externaltools.h"

// --- Singleton ---

ExternalTools& ExternalTools::instance()
{
    static ExternalTools s_instance;
    return s_instance;
}

ExternalTools::ExternalTools(QObject* parent)
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
    registerTool(RemoteViewer, u"RemoteViewerPath"_s, u"remote-viewer"_s);
}

// --- Registration ---

void ExternalTools::registerTool(Tool id, const QString& configKey, const QString& defaultBinary)
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

void ExternalTools::detectPath(ToolInfo& info)
{
    // 1) User override from KConfig
    KConfig config(u"kemurc"_s);
    KConfigGroup general = config.group(u"General"_s);
    QString configured = general.readEntry(info.configKey, { });

    if (!configured.isEmpty() && QFileInfo::exists(configured)) {
        info.resolvedPath = configured;
        return;
    }

    // 2) Auto-detect via $PATH
    QString found = QStandardPaths::findExecutable(info.defaultBinary);
    if (!found.isEmpty()) {
        info.resolvedPath = found;
        return;
    }

    // 2b) Special case: try alternative binary names for ISO tools
    if (found.isEmpty() && info.configKey == u"GenIsoImagePath"_s) {
        found = QStandardPaths::findExecutable(u"mkisofs"_s);
        if (found.isEmpty())
            found = QStandardPaths::findExecutable(u"xorriso"_s);
    }

    // 3) Fallback: Hardcoded custom paths for tools not included in $PATH
    static const QStringList fallbackPaths = {
        u"/usr/libexec"_s, // Fedora, RHEL, openSUSE
        u"/usr/lib/qemu"_s, // Debian, Ubuntu
        u"/usr/lib"_s, // General lib directory
        u"/usr/local/libexec"_s, // Custom builds
        u"/usr/sbin"_s
    };

    // findExecutable now specifically searches within our custom path list
    found = QStandardPaths::findExecutable(info.defaultBinary, fallbackPaths);

    info.resolvedPath = found;
}

void ExternalTools::probeVersion(Tool tool, ToolInfo& info)
{
    info.detectedVersion.clear();
    info.available = false;

    if (info.resolvedPath.isEmpty() || !QFileInfo::exists(info.resolvedPath))
        return;

    info.detectedVersion = probeVersionString(tool, info.resolvedPath);

    QFileInfo fi(info.resolvedPath);
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

QString ExternalTools::configKey(Tool tool) const
{
    return m_tools.value(tool).configKey;
}

QString ExternalTools::defaultBinary(Tool tool) const
{
    return m_tools.value(tool).defaultBinary;
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
    case RemoteViewer:
        return i18n("SPICE Remote Viewer");
    }
    return { };
}

QList<ExternalTools::Tool> ExternalTools::allTools()
{
    return { QemuSystem, QemuImg, Swtpm, Virtiofsd, GenIsoImage,
        Passt, Pasta, Dnsmasq, RemoteViewer };
}

QList<ExternalTools::Tool> ExternalTools::coreTools()
{
    return { QemuSystem, QemuImg, Passt };
}

QList<ExternalTools::Tool> ExternalTools::virtualNetworkTools()
{
    return { Pasta, Dnsmasq };
}

QString ExternalTools::probeVersionString(Tool tool, const QString& path)
{
    Q_UNUSED(tool);

    if (path.isEmpty() || !QFileInfo::exists(path))
        return { };

    const QStringList versionArgs = { u"--version"_s };

    KProcess process;
    process.setOutputChannelMode(KProcess::MergedChannels);
    process.setProgram(path);
    process.setArguments(versionArgs);
    process.start();

    if (!process.waitForFinished(2000) || process.exitCode() != 0)
        return { };

    QString output = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
    if (output.isEmpty())
        output = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();

    return output.split(u'\n').value(0).trimmed();
}

QStringList ExternalTools::missingToolNames() const
{
    QStringList missing;
    for (Tool t : allTools()) {
        if (!isAvailable(t))
            missing << displayName(t);
    }
    return missing;
}

bool ExternalTools::isVirtualNetworkCapable() const
{
    for (Tool t : virtualNetworkTools()) {
        if (!isAvailable(t))
            return false;
    }
    return true;
}
