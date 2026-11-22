/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QString>

#include <optional>

#include "utils/kemuagentprotocol.h"

// What KEMU asked QEMU for, from the VM's configuration (see the uses*()
// functions of QemuCommandBuilder).
struct GraphicsRequest {
    bool acceleration = false; // virtio-vga-gl
    bool hostBlobs = false; // blob=on
    int hostmemMb = 0;
    bool vulkan = false; // venus=on
    bool nativeContext = false; // drm_native_context=on

    bool operator==(const GraphicsRequest &) const = default;
};

// What the guest makes of the request, as far as kemu-agent tells
// (GRAPHICS_INFO, docs/kemu-agent-protocol.md section 7.5).
struct GraphicsStatus {
    enum class OpenGl {
        Unknown, // no report, or no OpenGL context in the guest
        Virgl,
        NativeContext, // the host GPU's own driver
        Zink, // OpenGL on Vulkan
        Software, // llvmpipe and friends: the host GPU is not used
        Other, // some other GPU, e.g. one passed through
    };

    OpenGl openGl = OpenGl::Unknown;
    QString renderer; // plainGuestText() of the guest's GL_RENDERER
    // From the capability sets of the device; unset when unknown.
    std::optional<bool> venusOffered;
    std::optional<bool> nativeContextOffered;
    // Something asked for does not take effect.
    bool warning = false;
};

// Sorts a GL_RENDERER string. @p nativeContextOffered tells a native
// context apart from a GPU of another kind.
GraphicsStatus::OpenGl classifyRenderer(const QString &renderer, bool nativeContextOffered);

GraphicsStatus evaluateGraphics(const GraphicsRequest &request, const std::optional<KemuAgentProtocol::GuestGraphics> &guest);

// The host GPU that virgl renders on, from its GL_RENDERER: "Mesa Intel(R)
// Graphics (MTL)" from "virgl (Mesa Intel(R) Graphics (MTL))". Empty when
// the renderer does not name it.
QString virglHostRenderer(const QString &renderer);

// A string from the guest, fit for a plain-text tool tip: no control
// characters, nothing Qt could take for markup, and not too long.
QString plainGuestText(const QString &text);
