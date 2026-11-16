/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "clipboardchoice.h"

using namespace Qt::StringLiterals;

namespace ClipboardChoice
{
Choice choose(bool hasWayland, bool hasX11, const std::optional<DataControl> &dataControl, const QByteArray &presetPlatform)
{
    Choice choice;
    // A Wayland platform the session asks Qt applications for is not ours
    // to follow: the agent needs no window, and with data-control it talks
    // to the compositor itself.
    const bool preset = !presetPlatform.isEmpty() && !presetPlatform.startsWith("wayland");

    // Data-control on a Wayland connection of the agent's own: works
    // whatever platform Qt runs on, so Qt needs none with a display.
    if (hasWayland && dataControl && (dataControl->ext || dataControl->wlrVersion > 0)) {
        choice.protocol = dataControl->ext ? u"ext-data-control"_s : u"wlr-data-control"_s;
        if (!preset)
            choice.platform = "offscreen";
        return choice;
    }

    // Otherwise the X server's selections, which it serves to any client:
    // an X11 session, or Xwayland where the compositor lacks data-control
    // (GNOME).
    QString why;
    if (hasWayland)
        why = dataControl ? u"the compositor offers no data-control protocol"_s : u"the Wayland compositor cannot be reached"_s;
    if (preset && !presetPlatform.startsWith("xcb")) {
        choice.note = u"QT_QPA_PLATFORM=%1 gives no access to the session's clipboard"_s.arg(QString::fromLatin1(presetPlatform));
        if (!why.isEmpty())
            choice.note.prepend(why + u"; "_s);
        return choice;
    }
    if (hasX11) {
        if (!preset)
            choice.platform = "xcb";
        choice.protocol = u"x11"_s;
        if (!why.isEmpty())
            choice.note = why + u"; using X11 through Xwayland for the clipboard"_s;
        return choice;
    }

    if (!preset)
        choice.platform = "offscreen";
    choice.note = why + u" and there is no X11; clipboard sharing is off"_s;
    return choice;
}
}
