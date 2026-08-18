/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QFileInfo>
#include <QHash>
#include <QObject>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTextBrowser>

#include <KConfig>
#include <KConfigGroup>
#include <KLocalizedString>
#include <KProcess>

using namespace Qt::StringLiterals;

/**
 * @brief Central registry for external tool dependencies (qemu-system,
 * qemu-img, swtpm, ...).
 *
 * Singleton. Call ExternalTools::instance().refresh() once at startup.
 * After that, query paths, versions and availability from anywhere:
 *
 *   const auto& tools = ExternalTools::instance();
 *   if (!tools.isAvailable(ExternalTools::QemuImg)) { ... }
 *   QString path = tools.path(ExternalTools::QemuImg);
 *
 */
class ExternalTools : public QObject
{
    Q_OBJECT

public:
    enum Tool {
        QemuSystem, // qemu-system-x86_64
        QemuImg, // qemu-img
        Swtpm, // swtpm
        Virtiofsd, // virtiofsd (virtio-fs shared file system daemon)
        GenIsoImage, // genisoimage, mkisofs, or xorriso
        Passt, // passt -- single-VM user-mode networking (unchanged)
        Pasta, // pasta -- namespace-to-host NAT connector (from passt project)
        Dnsmasq, // dnsmasq -- lightweight DHCP/DNS for virtual networks
        RemoteViewer, // remote-viewer (virt-viewer, for SPICE display)
        DbusDaemon // dbus-daemon for per-VM private bus
    };
    Q_ENUM(Tool)

    enum FeatureGroupId {
        Core,
        VirtualNetwork,
        SpiceDisplay,
        DbusDisplay,
        TpmEmulation,
        SharedFolders,
        CloudInitIso
    };
    Q_ENUM(FeatureGroupId)

    struct FeatureGroup {
        FeatureGroupId id;
        QString name;
        QString description;
        QList<Tool> tools;
        bool suppressible; // false for Core: cannot be silenced
    };

    static ExternalTools &instance();

    [[nodiscard]] QString path(Tool tool) const;
    [[nodiscard]] QString version(Tool tool) const;
    [[nodiscard]] bool isAvailable(Tool tool) const;
    [[nodiscard]] bool isAvailable(const QList<Tool> &tools) const;
    [[nodiscard]] QString configKey(Tool tool) const;
    [[nodiscard]] QString defaultBinary(Tool tool) const;

    // What auto-detection found, regardless of any configured override.
    [[nodiscard]] QString detectedPath(Tool tool) const;

    // The user's override from kemurc, empty when detection is in charge.
    [[nodiscard]] QString configuredPath(Tool tool) const;

    // Human-readable display name, e.g. "QEMU System Emulator"
    [[nodiscard]] static QString displayName(Tool tool);

    // Returns all registered tool ids.
    [[nodiscard]] static QList<Tool> allTools();

    // Tools required for basic VM operation (always needed).
    [[nodiscard]] static QList<Tool> coreTools();

    // Tools required only for the virtual-network feature.
    [[nodiscard]] static QList<Tool> virtualNetworkTools();

    [[nodiscard]] static QList<Tool> spiceDisplayTools();
    [[nodiscard]] static QList<Tool> dbusDisplayTools();

    // Tools that cannot be used in the Flatpak sandbox (namespace-based
    // networking). Empty in native mode.
    [[nodiscard]] static QList<Tool> sandboxIrrelevantTools();

    /**
     * Probe the version string for a given tool at an arbitrary path.
     * Handles tool-specific quirks (e.g. `ip -V`, stderr output).
     * Returns the first line of version output, or empty on failure.
     *
     * Used by SettingsDialog to validate user-entered paths before saving.
     */
    [[nodiscard]] static QString probeVersionString(Tool tool, const QString &path);

    // Convenience: list of tools that are configured but NOT available.
    [[nodiscard]] QStringList missingToolNames() const;

    // Convenience: check if all tools for virtual networking are available.
    [[nodiscard]] bool isVirtualNetworkCapable() const;

    [[nodiscard]] static QList<FeatureGroup> featureGroups();
    [[nodiscard]] QList<FeatureGroup> missingFeatureGroups() const;

    [[nodiscard]] static bool isWarningSuppressed(FeatureGroupId id);
    static void setWarningSuppressed(FeatureGroupId id, bool suppressed);
    [[nodiscard]] static QList<FeatureGroupId> suppressedFeatureGroups();
    static void resetAllSuppressions();

    [[nodiscard]] static QString suppressionConfigKey(FeatureGroupId id);

    // --- (Re-)Detection ---

    /**
     * Resolve paths (from KConfig or auto-detect) and probe versions.
     * Call once at application startup and again after the user saves
     * changed paths in SettingsDialog.
     */
    void refresh();

Q_SIGNALS:
    // Emitted at the end of refresh(). Connect to show warnings in the UI.
    void toolsRefreshed();

private:
    explicit ExternalTools(QObject *parent = nullptr);
    ~ExternalTools() override = default;
    Q_DISABLE_COPY_MOVE(ExternalTools)

    struct ToolInfo {
        QString configKey; // key in kemurc  (e.g. "QemuSystemPath")
        QString defaultBinary; // binary name for findExecutable()
        QString configuredPath; // user override from kemurc, may be empty
        QString detectedPath; // what auto-detection found, ignoring the override
        QString resolvedPath; // configuredPath if usable, else detectedPath
        QString detectedVersion; // first line of --version output
        bool available = false; // file exists AND is executable
    };

    QHash<Tool, ToolInfo> m_tools;

    void registerTool(Tool id, const QString &configKey, const QString &defaultBinary);
    void detectPath(ToolInfo &info);
    static QString autoDetect(const ToolInfo &info);
    void probeVersion(Tool tool, ToolInfo &info);
};
