/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#pragma push_macro("signals")
#undef signals
#include <glib.h>
#pragma pop_macro("signals")

// =============================================================================
// GVariantPtr - RAII wrapper for GVariant*
// =============================================================================
// Owns a single GVariant reference. On destruction, g_variant_unref() is
// called if the pointer is non-null. Move-only - copying a GVariant requires
// an explicit ref bump, which would defeat the purpose of the wrapper.
//
// Typical use in a D-Bus method handler:
//
//     GVariant *raw = nullptr;
//     g_variant_get(parameters, "(t@ay)", &id, &raw);
//     GVariantPtr data(raw);          // takes ownership immediately
//     // ... use data.get() ...
//     // ... no manual unref needed, even on early return ...
// =============================================================================

class GVariantPtr
{
public:
    GVariantPtr() = default;
    explicit GVariantPtr(GVariant *v) noexcept
        : m_v(v)
    {
    }

    ~GVariantPtr()
    {
        reset();
    }

    GVariantPtr(const GVariantPtr &) = delete;
    GVariantPtr &operator=(const GVariantPtr &) = delete;

    GVariantPtr(GVariantPtr &&other) noexcept
        : m_v(other.m_v)
    {
        other.m_v = nullptr;
    }
    GVariantPtr &operator=(GVariantPtr &&other) noexcept
    {
        if (this != &other) {
            reset();
            m_v = other.m_v;
            other.m_v = nullptr;
        }
        return *this;
    }

    GVariant *get() const noexcept
    {
        return m_v;
    }
    explicit operator bool() const noexcept
    {
        return m_v != nullptr;
    }

    // Release ownership without unref (caller takes responsibility).
    GVariant *release() noexcept
    {
        GVariant *tmp = m_v;
        m_v = nullptr;
        return tmp;
    }

    void reset(GVariant *v = nullptr) noexcept
    {
        if (m_v)
            g_variant_unref(m_v);
        m_v = v;
    }

private:
    GVariant *m_v = nullptr;
};
