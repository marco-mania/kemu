/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <unistd.h>

// =============================================================================
// UniqueFd - RAII wrapper for Linux file descriptors
// =============================================================================
// Owns a single FD. On destruction, ::close() is called if the FD is valid.
// Move-only - duplicating an FD requires an explicit ::dup() call, which
// would defeat the purpose of the wrapper.
//
// A "valid" FD is any non-negative integer. Default state is -1 (invalid).
// =============================================================================

class UniqueFd
{
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept
        : m_fd(fd)
    {
    }

    ~UniqueFd()
    {
        reset();
    }

    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;

    UniqueFd(UniqueFd &&other) noexcept
        : m_fd(other.m_fd)
    {
        other.m_fd = -1;
    }
    UniqueFd &operator=(UniqueFd &&other) noexcept
    {
        if (this != &other) {
            reset();
            m_fd = other.m_fd;
            other.m_fd = -1;
        }
        return *this;
    }

    int get() const noexcept
    {
        return m_fd;
    }
    bool isValid() const noexcept
    {
        return m_fd >= 0;
    }
    explicit operator bool() const noexcept
    {
        return isValid();
    }

    // Release ownership without closing (caller takes responsibility).
    // Returns the previously-held FD; the wrapper is now empty.
    int release() noexcept
    {
        const int tmp = m_fd;
        m_fd = -1;
        return tmp;
    }

    // Close the held FD (if any) and optionally take ownership of a new one.
    void reset(int fd = -1) noexcept
    {
        if (m_fd >= 0)
            ::close(m_fd);
        m_fd = fd;
    }

private:
    int m_fd = -1;
};
