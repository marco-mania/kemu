/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QString>

using namespace Qt::StringLiterals;

namespace KemuDbusDisplayConst
{

inline const QString ServiceName = u"org.qemu"_s;

// Object paths exported by QEMU's -display dbus
inline constexpr auto ConsolePathPrefix = "/org/qemu/Display1/Console_"_L1;
inline constexpr auto AudioPath = "/org/qemu/Display1/Audio"_L1;
inline constexpr auto ClipboardPath = "/org/qemu/Display1/Clipboard"_L1;

inline constexpr auto ListenerIface = "org.qemu.Display1.Listener"_L1;
inline constexpr auto ListenerUnixIface = "org.qemu.Display1.Listener.Unix"_L1;

// D-Bus interface names
inline constexpr auto ConsoleIface = "org.qemu.Display1.Console"_L1;
inline constexpr auto KeyboardIface = "org.qemu.Display1.Keyboard"_L1;
inline constexpr auto MouseIface = "org.qemu.Display1.Mouse"_L1;
inline constexpr auto AudioIface = "org.qemu.Display1.Audio"_L1;
inline constexpr auto ClipboardIface = "org.qemu.Display1.Clipboard"_L1;

// Object paths that we (the viewer) export back to QEMU
inline constexpr auto ListenerPath = "/org/qemu/Display1/Listener"_L1;
inline constexpr auto AudioOutListenerPath = "/org/qemu/Display1/AudioOutListener"_L1;
inline constexpr auto AudioInListenerPath = "/org/qemu/Display1/AudioInListener"_L1;

} // namespace KemuDbusDisplayConst
