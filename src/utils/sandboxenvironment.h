/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QString>

using namespace Qt::StringLiterals;

/**
 * @brief Detects (or overrides) whether KEMU runs inside a Flatpak sandbox.
 *
 * The mode is resolved once and cached. It defaults to auto-detection via
 * the /.flatpak-info file, but can be forced from the command line
 * (--sandbox-mode) for testing outside a real Flatpak build.
 */
class SandboxEnvironment
{
public:
    enum class Mode {
        Auto,
        Native,
        Flatpak
    };

    /// Set once from main() before any isFlatpak() call. Auto keeps detection.
    static void setOverride(Mode mode);

    /// Parse "auto" | "native" | "flatpak" (case-insensitive). Returns Auto
    /// for anything unrecognized, with ok=false so main() can warn.
    static Mode parseMode(const QString &value, bool *ok = nullptr);

    /// True if running (or forced to behave as) inside a Flatpak sandbox.
    static bool isFlatpak();
};
