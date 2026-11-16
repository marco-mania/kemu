/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <optional>

#include "clipboardchoice.h"

// Questions to answer before Qt picks its platform.
namespace PlatformProbe
{
// Which data-control protocols the Wayland compositor offers us; nullopt
// without a compositor to connect to.
std::optional<ClipboardChoice::DataControl> waylandDataControl();
}
