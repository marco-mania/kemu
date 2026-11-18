/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

/**
 * @brief The kemu-agent installation medium for guests.
 *
 * KEMU offers kemu-agent to a guest as a CD image, like other hypervisors'
 * guest tools. The image is made from kemu-agent bundles (agent/README.md,
 * "Portable bundle"): unpacked in share/kemu/agent/<arch>/ (where KEMU's
 * build installs them unless KEMU_AGENT_BUNDLE is off), or a bundle tarball
 * the user chose. It holds the newest bundle for each architecture, an install.sh and
 * uninstall.sh that run the bundle's own for the guest's architecture, and a
 * README.txt.
 */
namespace GuestAgentMedium
{

/// The label the medium carries; desktops mount it as /run/media/$USER/<label>.
inline constexpr char16_t VolumeLabel[] = u"KEMU_AGENT";

/// One kemu-agent bundle.
struct Bundle {
    QString path; // the directory with BUNDLE-INFO, or the tarball
    QString version; // "0.4.0"
    QString architecture; // as "uname -m" names it: "x86_64", "aarch64"
    bool isArchive = false;
    QString archiveRoot; // the tarball's top directory

    bool operator==(const Bundle &) const = default;
};

/// Reads a bundle: a directory holding BUNDLE-INFO and install.sh (or the
/// BUNDLE-INFO file itself), or a bundle tarball (.tar.gz, .tar.xz). Fills
/// errorOut when it is none.
[[nodiscard]] std::optional<Bundle> readBundle(const QString &path, QString *errorOut = nullptr);

/// The bundles in each path: a bundle itself, or a directory with one bundle
/// per subdirectory (share/kemu/agent/<arch>/). Paths that hold none are
/// skipped.
[[nodiscard]] QList<Bundle> findBundles(const QStringList &paths);

/// Where installed bundles are looked for: share/kemu/agent next to KEMU's
/// own binary and in the XDG data directories, the user's first.
[[nodiscard]] QStringList installedBundlePaths();

/// The newest bundle for each architecture, sorted by architecture.
[[nodiscard]] QList<Bundle> newestPerArchitecture(const QList<Bundle> &bundles);

/// -1, 0 or 1 as version a is older than, the same as or newer than b, by the
/// rules of the bundle's install.sh: dot-separated numbers, and an optional
/// "-suffix" makes a version older than the same one without
/// (0.4.0-rc1 < 0.4.0).
[[nodiscard]] int compareVersions(const QString &a, const QString &b);

/// Where KEMU keeps the medium: a stable path, so that a VM configuration
/// that still names it stays valid when it is rebuilt.
[[nodiscard]] QString defaultMediumPath();

/// Makes the medium at isoPath from bundles (one per architecture; see
/// newestPerArchitecture()), replacing the file in one rename. A medium made
/// from the same, unchanged bundles is kept as it is. Returns false and fills
/// errorOut on failure.
bool build(const QList<Bundle> &bundles, const QString &isoPath, QString &errorOut);

/// The command that installs the agent from the medium in the guest.
[[nodiscard]] QString installCommand();

}
