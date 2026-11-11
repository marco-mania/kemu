/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "displayconsoles.h"

#include "dbusdisplay/controlconnection.h"
#include "dbusdisplay/dbusdisplayconst.h"
#include "kemu_dbusdisplay_debug.h"

#include <QPointer>

#include <algorithm>
#include <memory>
#include <utility>

using namespace Qt::StringLiterals;

QString QemuDisplayConsole::path() const
{
    return KemuDbusDisplayConst::ConsolePathPrefix + QString::number(id);
}

QList<guint> QemuDisplayConsoles::parseIds(GVariant *reply)
{
    if (!reply || !g_variant_is_of_type(reply, G_VARIANT_TYPE("(v)")))
        return {};

    GVariant *value = nullptr;
    g_variant_get(reply, "(v)", &value);
    if (!g_variant_is_of_type(value, G_VARIANT_TYPE("au"))) {
        g_variant_unref(value);
        return {};
    }

    QList<guint> ids;
    GVariantIter iter;
    g_variant_iter_init(&iter, value);
    guint id;
    while (g_variant_iter_next(&iter, "u", &id)) {
        if (!ids.contains(id))
            ids.append(id);
    }
    g_variant_unref(value);
    return ids;
}

std::optional<QemuDisplayConsole> QemuDisplayConsoles::parseProperties(guint id, GVariant *reply)
{
    if (!reply || !g_variant_is_of_type(reply, G_VARIANT_TYPE("(a{sv})")))
        return std::nullopt;

    GVariant *properties = nullptr;
    g_variant_get(reply, "(@a{sv})", &properties);

    const gchar *type = nullptr;
    guint head = 0;
    if (!g_variant_lookup(properties, "Type", "&s", &type) || g_strcmp0(type, "Graphic") != 0
        || !g_variant_lookup(properties, "Head", "u", &head)) {
        g_variant_unref(properties);
        return std::nullopt;
    }

    const gchar *address = nullptr;
    g_variant_lookup(properties, "DeviceAddress", "&s", &address);
    QemuDisplayConsole console{id, head, address ? QString::fromUtf8(address) : QString()};
    g_variant_unref(properties);
    return console;
}

void QemuDisplayConsoles::sortByHead(QList<QemuDisplayConsole> &consoles)
{
    std::sort(consoles.begin(), consoles.end(), [](const QemuDisplayConsole &a, const QemuDisplayConsole &b) {
        return a.head == b.head ? a.id < b.id : a.head < b.head;
    });
}

QList<QemuDisplayConsole> QemuDisplayConsoles::additionalConsoles(const QList<QemuDisplayConsole> &consoles)
{
    QList<QemuDisplayConsole> additional;
    for (const auto &console : consoles) {
        if (console.id != 0)
            additional.append(console);
    }
    sortByHead(additional);
    return additional;
}

void QemuDisplayConsoles::discoverAsync(QemuControlConnection *ctrl, DiscoveryFn done)
{
    ctrl->callWithReply(QString(KemuDbusDisplayConst::VmPath),
                        u"org.freedesktop.DBus.Properties"_s,
                        u"Get"_s,
                        g_variant_new("(ss)", "org.qemu.Display1.VM", "ConsoleIDs"),
                        [ctrl = QPointer<QemuControlConnection>(ctrl), done = std::move(done)](GVariant *reply, const GError *error) {
                            if (error) {
                                done({}, QString::fromUtf8(error->message));
                                return;
                            }

                            if (!ctrl)
                                return;

                            const QList<guint> ids = parseIds(reply);
                            if (ids.isEmpty()) {
                                done({}, u"QEMU returned no display consoles"_s);
                                return;
                            }

                            struct Pending {
                                DiscoveryFn done;
                                QList<QemuDisplayConsole> consoles;
                                int remaining = 0;
                            };
                            auto pending = std::make_shared<Pending>();
                            pending->done = done;
                            pending->remaining = ids.size();

                            for (guint id : ids) {
                                const QString path = KemuDbusDisplayConst::ConsolePathPrefix + QString::number(id);
                                ctrl->callWithReply(path,
                                                    u"org.freedesktop.DBus.Properties"_s,
                                                    u"GetAll"_s,
                                                    g_variant_new("(s)", "org.qemu.Display1.Console"),
                                                    [pending, id](GVariant *properties, const GError *error) {
                                                        if (!error) {
                                                            if (auto console = parseProperties(id, properties))
                                                                pending->consoles.append(*console);
                                                        }
                                                        if (--pending->remaining == 0) {
                                                            sortByHead(pending->consoles);
                                                            pending->done(pending->consoles, QString());
                                                        }
                                                    });
                            }
                        });
}

GVariant *QemuDisplayConsoles::uiInfo(const QRect &geometry)
{
    return g_variant_new("(qqiiuu)", guint16(0), guint16(0),
                         gint(geometry.x()), gint(geometry.y()),
                         guint(geometry.width()), guint(geometry.height()));
}

void QemuDisplayConsoles::setUiInfoAsync(QemuControlConnection *ctrl, const QString &consolePath, const QRect &geometry)
{
    ctrl->callWithReply(consolePath,
                        QString(KemuDbusDisplayConst::ConsoleIface),
                        u"SetUIInfo"_s,
                        uiInfo(geometry),
                        [consolePath](GVariant *, const GError *error) {
                            if (error)
                                qCWarning(KEMU_DBUSDISPLAY) << "Could not update display geometry:" << consolePath << error->message;
                        });
}
