/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sandboxenvironment.h"

#include <QFileInfo>

#include <optional>

namespace
{
std::optional<bool> s_override;
}

void SandboxEnvironment::setOverride(Mode mode)
{
    switch (mode) {
    case Mode::Flatpak:
        s_override = true;
        break;
    case Mode::Native:
        s_override = false;
        break;
    case Mode::Auto:
        s_override.reset();
        break;
    }
}

SandboxEnvironment::Mode SandboxEnvironment::parseMode(const QString &value, bool *ok)
{
    if (ok)
        *ok = true;
    const QString v = value.toLower();
    if (v == u"auto"_s)
        return Mode::Auto;
    if (v == u"native"_s)
        return Mode::Native;
    if (v == u"flatpak"_s)
        return Mode::Flatpak;
    if (ok)
        *ok = false;
    return Mode::Auto;
}

bool SandboxEnvironment::isFlatpak()
{
    // Detection result is stable for the process lifetime, so cache it.
    // The override, if set before the first call, wins over detection.
    static const bool detected = QFileInfo::exists(u"/.flatpak-info"_s);
    return s_override.value_or(detected);
}
