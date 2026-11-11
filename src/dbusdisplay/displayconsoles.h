/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QList>
#include <QRect>
#include <QString>

#include <functional>
#include <optional>

#pragma push_macro("signals")
#undef signals
#include <glib.h>
#pragma pop_macro("signals")

class QemuControlConnection;

struct QemuDisplayConsole {
    guint id = 0;
    guint head = 0;
    QString deviceAddress;

    QString path() const;
};

class QemuDisplayConsoles
{
public:
    using DiscoveryFn = std::function<void(const QList<QemuDisplayConsole> &consoles, const QString &error)>;

    static void discoverAsync(QemuControlConnection *ctrl, DiscoveryFn done);
    static QList<guint> parseIds(GVariant *reply);
    static std::optional<QemuDisplayConsole> parseProperties(guint id, GVariant *reply);
    static void sortByHead(QList<QemuDisplayConsole> &consoles);
    static QList<QemuDisplayConsole> additionalConsoles(const QList<QemuDisplayConsole> &consoles);
    static GVariant *uiInfo(const QRect &geometry);
    // An empty geometry unplugs the head in the guest.
    static void setUiInfoAsync(QemuControlConnection *ctrl, const QString &consolePath, const QRect &geometry);
};
