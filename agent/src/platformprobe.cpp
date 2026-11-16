/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "platformprobe.h"

#include <cstring>

#include <wayland-client.h>

namespace
{
void registryGlobal(void *data, wl_registry *, uint32_t, const char *interface, uint32_t version)
{
    auto *dataControl = static_cast<ClipboardChoice::DataControl *>(data);
    if (std::strcmp(interface, "ext_data_control_manager_v1") == 0)
        dataControl->ext = true;
    else if (std::strcmp(interface, "zwlr_data_control_manager_v1") == 0)
        dataControl->wlrVersion = version;
}

void registryGlobalRemove(void *, wl_registry *, uint32_t)
{
}

const wl_registry_listener s_listener = {registryGlobal, registryGlobalRemove};
} // namespace

std::optional<ClipboardChoice::DataControl> PlatformProbe::waylandDataControl()
{
    wl_display *display = wl_display_connect(nullptr);
    if (!display)
        return std::nullopt;
    ClipboardChoice::DataControl dataControl;
    wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &s_listener, &dataControl);
    wl_display_roundtrip(display);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return dataControl;
}
