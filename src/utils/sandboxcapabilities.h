/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QHash>
#include <QString>

using namespace Qt::StringLiterals;

class SandboxCapabilities
{
public:
    enum Capability {
        UsbPassthrough,
        HostTerminal,
        PipeWireAudio,
        PulseAudio,
        ExternalToolPaths,
    };

    static SandboxCapabilities &instance();

    // Probes every capability. Call once before the main window is built.
    void refresh();

    bool has(Capability capability) const;

    // i18n'd explanation, empty while the capability is available.
    QString unavailableReason(Capability capability) const;

    // Turns a capability off for testing (--disable-capability).
    void disable(Capability capability, const QString &reason);

    // Terminal emulator found by the HostTerminal probe, empty if none.
    QString terminalProgram() const;

    static QString configName(Capability capability);
    static bool parseCapability(const QString &name, Capability *capability);
    static QList<Capability> allCapabilities();

    // Direct environment fact, not a probed capability: whether KEMU runs
    // inside a Flatpak sandbox.
    [[nodiscard]] bool isFlatpak() const
    {
        return m_isFlatpak;
    }

private:
    SandboxCapabilities() = default;

    struct State {
        bool available = false;
        QString reason;
    };

    QHash<int, State> m_states;
    QString m_terminalProgram;

    bool m_isFlatpak = false;
};
