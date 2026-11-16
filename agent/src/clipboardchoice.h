/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QByteArray>
#include <QString>

#include <optional>

// Which clipboard a process without a window can reach in this session, and
// so which platform Qt is to use (section 11.3). Decided before Qt starts;
// no platform code here, so the tests can go through every case.
namespace ClipboardChoice
{
// What the Wayland compositor offers.
struct DataControl {
    bool ext = false; // ext_data_control_manager_v1
    uint wlrVersion = 0; // zwlr_data_control_manager_v1, 0 = not offered
};

struct Choice {
    // Value for QT_QPA_PLATFORM; empty = leave it as it is.
    QByteArray platform;
    // For HELLO: "ext-data-control", "wlr-data-control" or "x11"; empty =
    // no clipboard in this session.
    QString protocol;
    // For the log when the choice is not the obvious one.
    QString note;
};

// @p dataControl: nullopt when there is no compositor to ask.
// @p presetPlatform: QT_QPA_PLATFORM as the session set it.
Choice choose(bool hasWayland, bool hasX11, const std::optional<DataControl> &dataControl, const QByteArray &presetPlatform);
}
