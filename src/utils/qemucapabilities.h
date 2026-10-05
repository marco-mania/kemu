/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QElapsedTimer>
#include <QFileInfo>
#include <QSet>
#include <QString>

#include <KProcess>

class KProcess;

using namespace Qt::StringLiterals;

class QemuCapabilities
{
public:
    static QemuCapabilities &instance();

    // Re-probes the currently resolved qemu-system binary. Call right after
    // ExternalTools::instance().refresh().
    void refresh();

    bool hasDisplay(const QString &name) const;
    bool hasAudiodev(const QString &name) const;

    // Whether the binary was built with seccomp support. Unlike the lists
    // above this defaults to false: -sandbox on a QEMU without it is fatal.
    [[nodiscard]] bool hasSeccompSandbox() const
    {
        return m_seccompSandbox;
    }

    // Whether monitors can be created as objects (-object monitor-qmp, QEMU
    // 11.1+), which replaces the deprecated -mon. Defaults to false: -mon
    // still works on every version.
    [[nodiscard]] bool hasMonitorObject() const
    {
        return m_monitorObject;
    }

    // False when neither probe produced a listing. The Settings page can use
    // this to say "capabilities unknown" instead of silently offering
    // everything.
    [[nodiscard]] bool probeSucceeded() const
    {
        return m_probeSucceeded;
    }

private:
    QemuCapabilities() = default;

    static constexpr int kProbeTimeoutMs = 4000;
    static constexpr int kProbeRetryTimeoutMs = 8000;

    bool runProbes(const QString &qemuPath, int timeoutMs);
    void seedSelfContainedDefaults();
    static void startProbe(KProcess &process, const QString &qemuPath, const QStringList &arguments);
    static QSet<QString> collectProbe(KProcess &process, int remainingMs);
    static bool collectExitStatus(KProcess &process, int remainingMs);

    QSet<QString> m_displays;
    QSet<QString> m_audiodevs;
    bool m_seccompSandbox = false;
    bool m_monitorObject = false;
    bool m_probeSucceeded = false;
};
